#include "RecoveryMaintenanceQueue.h"
#include "RecoveryStorageLayout.h"
#include "RecoveryDiscovery.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::recovery
{
struct RecoveryMaintenanceQueue::State final
{
    struct Job final
    {
        std::unique_ptr<SavedOwnerRetirement> ticket;
        std::unique_ptr<OwnerDiscardReview> discard;
        JobStatus status;
        bool markerPublished = false;
        bool resumeOnly = false;
        bool discardRevocable = true;
        bool automaticExpiry = false, orphansCollected = false;
        juce::Uuid sweepOperation { nullptr };
        std::function<bool()> cancelled;
    };
    struct Sweep final
    {
        juce::Uuid operation;
        juce::File root;
        std::atomic<bool> cancel { false };
        std::unique_ptr<RecoveryDiscovery> cursor;
        std::vector<RecoveryDiscovery::Row> pending;
        size_t next = 0;
        bool more = true;
        bool expiryPass = false;
        size_t passEntries = 0;
        Progress::Sweep status;
    };
    explicit State(Config settings) : config(std::move(settings)) {}
    Config config;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Job>> jobs;
    std::shared_ptr<Sweep> sweep;
    Status failure;
    bool accepting = true, running = false;
    bool restartSweep = false;
    void note(const JobStatus& job, RecoveryDiagnosticState::Stage stage, Error error) const
    {
        if (config.diagnostics) config.diagnostics->record({ job.operationId, job.owner, stage, error,
            { static_cast<int64_t>(job.batches), static_cast<int64_t>(job.filesRemoved),
              static_cast<int64_t>(job.bytesRemoved), job.head.sequence, 0, 0 } });
    }
};

RecoveryMaintenanceQueue::RecoveryMaintenanceQueue(Config config) : state(std::make_shared<State>(std::move(config)))
{
    if (state->config.jobs == 0 || state->config.jobs > 256 || state->config.batch.files == 0
        || state->config.batch.files > 128 || state->config.batch.bytes == 0
        || state->config.batch.bytes > 64 * 1024 * 1024 || state->config.batch.bytes > state->config.store.readBytes)
    {
        state->failure = { Error::invalidPacket, "maintenance_configuration" };
        if (state->config.diagnostics)
            state->config.diagnostics->record({ juce::Uuid(), juce::Uuid(nullptr),
                RecoveryDiagnosticState::Stage::maintenanceConfiguration, state->failure.error, {} });
    }
    else state->jobs.reserve(state->config.jobs);
}
RecoveryMaintenanceQueue::~RecoveryMaintenanceQueue() { close(); }

Status RecoveryMaintenanceQueue::submit(std::unique_ptr<SavedOwnerRetirement>& ticket)
{
    std::unique_ptr<OwnerDiscardReview> unused;
    return admit(ticket, unused);
}

Status RecoveryMaintenanceQueue::submitConfirmedDiscard(std::unique_ptr<OwnerDiscardReview>& review)
{
    std::unique_ptr<SavedOwnerRetirement> unused;
    return admit(unused, review);
}

Status RecoveryMaintenanceQueue::admit(std::unique_ptr<SavedOwnerRetirement>& ticket, std::unique_ptr<OwnerDiscardReview>& discard)
{
    const std::lock_guard lock(state->mutex);
    if (!state->failure) return state->failure;
    if (!state->accepting) return { Error::cancelled, "maintenance_closed" };
    if ((!ticket && !discard) || (ticket && discard)) return { Error::invalidPacket, "maintenance_ticket" };
    const auto& root = ticket ? ticket->rootDirectory() : discard->rootDirectory();
    const auto owner = ticket ? ticket->ownerId() : discard->ownerId();
    if (state->jobs.size() >= state->config.jobs) return { Error::budgetExceeded, "maintenance_job_budget" };
    if (std::any_of(state->jobs.begin(), state->jobs.end(), [&] (const auto& job)
        { return job->status.root == root && job->status.owner == owner; }))
        return { Error::staleHead, "maintenance_duplicate_owner" };
    auto job = std::make_shared<State::Job>();
    job->status.root = root; job->status.owner = owner;
    job->status.head = ticket ? ticket->frozenHead() : discard->frozenHead();
    job->status.disposition = ticket ? Disposition::saved : Disposition::discarded;
    job->ticket = std::move(ticket);
    job->discard = std::move(discard);
    state->jobs.push_back(std::move(job));
    launch(state);
    return {};
}

Status RecoveryMaintenanceQueue::resumeTerminal(const juce::File& root, const juce::Uuid& owner, const SafeHead& head)
{
    const std::lock_guard lock(state->mutex);
    if (!state->failure) return state->failure;
    if (!state->accepting) return { Error::cancelled, "maintenance_closed" };
    const bool needsHead = head.sequence == 0 && head.hash.isEmpty();
    if (root == juce::File() || owner.isNull() || (!needsHead && (head.sequence < 1 || head.hash.length() != 64
        || !head.hash.containsOnly("0123456789abcdef")))) return { Error::invalidPacket, "maintenance_resume_selector" };
    if (state->jobs.size() >= state->config.jobs) return { Error::budgetExceeded, "maintenance_job_budget" };
    if (std::any_of(state->jobs.begin(), state->jobs.end(), [&] (const auto& job)
        { return job->status.root == root && job->status.owner == owner; }))
        return { Error::staleHead, "maintenance_duplicate_owner" };
    auto job = std::make_shared<State::Job>();
    job->status.root = root; job->status.owner = owner; job->status.head = head;
    job->status.disposition = Disposition::previouslyRetired;
    job->resumeOnly = true;
    state->jobs.push_back(std::move(job));
    launch(state);
    return {};
}

Status RecoveryMaintenanceQueue::sweepTerminals(const juce::File& root, bool policyChanged)
{
    const std::lock_guard lock(state->mutex);
    if (!state->failure && state->failure.code != "maintenance_worker_launch") return state->failure;
    if (!state->accepting) return { Error::cancelled, "maintenance_closed" };
    if (root == juce::File()) return { Error::invalidPacket, "maintenance_sweep_root" };
    if (state->sweep)
    {
        if (state->sweep->root != root) return { Error::invalidPacket, "maintenance_sweep_root_changed" };
        if (state->sweep->status.active)
        {
            if (policyChanged) { state->restartSweep = true; state->sweep->cancel.store(true); }
            state->failure = {}; launch(state); return {};
        }
        if (state->sweep->status.failure && !policyChanged) return {};
    }
    // An explicit retry after discovery failure starts a new cursor. Previously
    // admitted/failed jobs remain owned and are never retried by enumeration.
    state->failure = {}; state->sweep = std::make_shared<State::Sweep>();
    state->sweep->root = root; state->sweep->status.active = true;
    launch(state);
    return {};
}

void RecoveryMaintenanceQueue::cancelTerminalSweep()
{
    const std::lock_guard lock(state->mutex);
    if (state->sweep && state->sweep->status.active)
    {
        state->sweep->cancel.store(true);
        launch(state); // Also releases a cursor paused at queue capacity.
    }
}

void RecoveryMaintenanceQueue::discover(const std::shared_ptr<State>& shared)
{
    std::shared_ptr<State::Sweep> sweep;
    { const std::lock_guard lock(shared->mutex); sweep = shared->sweep; }
    // Only this worker touches the cursor/page.
    const auto finish = [&] (Status failure, bool cancelled)
    {
        sweep->cursor.reset(); sweep->pending.clear();
        const std::lock_guard lock(shared->mutex);
        sweep->status.active = false; sweep->status.completed = failure && !cancelled;
        sweep->status.cancelled = cancelled; sweep->status.failure = failure;
        if (shared->restartSweep && shared->accepting)
        {
            shared->restartSweep = false;
            shared->sweep = std::make_shared<State::Sweep>();
            shared->sweep->root = sweep->root; shared->sweep->status.active = true;
        }
        if (!failure && shared->config.diagnostics)
            shared->config.diagnostics->record({ sweep->operation, juce::Uuid(nullptr),
                RecoveryDiagnosticState::Stage::maintenanceDiscovery, failure.error,
                { static_cast<int64_t>(sweep->status.pages), static_cast<int64_t>(sweep->status.entries),
                  static_cast<int64_t>(sweep->status.terminalOwners), static_cast<int64_t>(sweep->status.preservedOwners), 0, 0 } });
    };
    if (sweep->cancel.load()) { finish({}, true); return; }
    try
    {
        if (sweep->next == sweep->pending.size())
        {
            if (!sweep->more)
            {
                // Finish previously authorized terminal work before considering
                // expiry. No retention file or second cursor on standalone roots.
                std::optional<RecoveryStorageLayout::RetentionPolicy> policy;
                if (sweep->expiryPass || sweep->root.getFileName() != "journals") { finish({}, false); return; }
                const auto status = RecoveryStorageLayout(sweep->root.getParentDirectory()).readRetention(policy);
                if (!status || !policy) { finish(status, false); return; }
                sweep->cursor.reset(); sweep->pending.clear(); sweep->next = 0;
                sweep->expiryPass = true; sweep->passEntries = 0; sweep->more = true;
                const std::lock_guard lock(shared->mutex);
                sweep->status.preservedOwners = 0; // Final eligibility is counted by the second pass.
            }
            const auto remaining = shared->config.store.globalEntries - sweep->passEntries;
            if (remaining == 0) { finish({ Error::budgetExceeded, "maintenance_sweep_entry_budget" }, false); return; }
            if (!sweep->cursor) sweep->cursor = std::make_unique<RecoveryDiscovery>(sweep->root, shared->config.store);
            RecoveryDiscovery::Budget budget; budget.entries = std::min(budget.entries, remaining);
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (shared->config.beforeSweepPage) shared->config.beforeSweepPage();
#endif
            auto page = sweep->cursor->nextPage(budget, [&] { return sweep->cancel.load(); });
            if (!page.status)
            {
                const bool cancelled = page.status.error == Error::cancelled;
                finish(cancelled ? Status{} : page.status, cancelled); return;
            }
            sweep->pending = std::move(page.rows); sweep->next = 0; sweep->more = page.more;
            const std::lock_guard lock(shared->mutex);
            ++sweep->status.pages; sweep->status.entries += page.entriesRead;
            sweep->passEntries += page.entriesRead;
        }
        const std::lock_guard lock(shared->mutex);
        while (sweep->next < sweep->pending.size() && shared->jobs.size() < shared->config.jobs && !sweep->cancel.load())
        {
            const auto& row = sweep->pending[sweep->next++];
            const bool terminal = (row.summary.status && row.summary.retired) || row.summary.status.code == "owner_collecting";
            const bool expiry = sweep->expiryPass && row.summary.status && !terminal && row.summary.head.sequence > 0;
            if ((!sweep->expiryPass && !terminal) || (sweep->expiryPass && !expiry))
            { ++sweep->status.preservedOwners; continue; }
            if (std::any_of(shared->jobs.begin(), shared->jobs.end(), [&] (const auto& job)
                { return job->status.root == sweep->root && job->status.owner == row.owner; }))
            { ++sweep->status.preservedOwners; continue; }
            auto job = std::make_shared<State::Job>();
            job->status.root = sweep->root; job->status.owner = row.owner;
            job->status.head = row.summary.status ? row.summary.head : SafeHead{};
            job->automaticExpiry = expiry; job->resumeOnly = !expiry;
            job->sweepOperation = sweep->operation;
            job->status.disposition = expiry ? Disposition::expired : Disposition::previouslyRetired;
            if (expiry) job->cancelled = [sweep] { return sweep->cancel.load(); };
            shared->jobs.push_back(std::move(job));
            if (expiry) ++sweep->status.expiryCandidates;
            else ++sweep->status.terminalOwners;
        }
    }
    catch (...) { finish({ Error::resourceOrEnvironmentFault, "maintenance_sweep_exception" }, false); }
}

Status RecoveryMaintenanceQueue::cancelDiscard(const juce::File& root, const juce::Uuid& owner)
{
    const std::lock_guard lock(state->mutex);
    const auto found = std::find_if(state->jobs.begin(), state->jobs.end(), [&] (const auto& job)
        { return job->status.root == root && job->status.owner == owner; });
    if (found == state->jobs.end()) return { Error::staleHead, "maintenance_unknown_owner" };
    const auto& job = **found;
    // Check phase first: the worker's publication flags are mutable outside this
    // mutex while retiring/collecting, and become visible with its final phase.
    if ((job.status.phase != Phase::queued && job.status.phase != Phase::failed)
        || job.automaticExpiry || !job.discard || job.markerPublished || !job.discardRevocable)
        return { Error::staleHead, "maintenance_discard_not_revocable" };
    state->jobs.erase(found);
    if (!state->running && state->jobs.empty() && state->failure.code == "maintenance_worker_launch") state->failure = {};
    return {};
}

void RecoveryMaintenanceQueue::launch(const std::shared_ptr<State>& shared)
{
    // Caller holds mutex. Worker cannot observe partially updated admission.
    if (shared->running) return;
    shared->running = true;
    try { std::thread([shared] { run(shared); }).detach(); }
    catch (...)
    {
        shared->running = false;
        shared->failure = { Error::resourceOrEnvironmentFault, "maintenance_worker_launch" };
        const auto queued = std::find_if(shared->jobs.begin(), shared->jobs.end(), [] (const auto& job)
            { return job->status.phase == Phase::queued || job->status.phase == Phase::collecting; });
        if (queued != shared->jobs.end()) shared->note((*queued)->status, RecoveryDiagnosticState::Stage::maintenanceLaunch, shared->failure.error);
        else if (shared->sweep && shared->config.diagnostics)
            shared->config.diagnostics->record({ shared->sweep->operation, juce::Uuid(nullptr),
                RecoveryDiagnosticState::Stage::maintenanceLaunch, shared->failure.error, {} });
    }
}

void RecoveryMaintenanceQueue::run(const std::shared_ptr<State>& shared)
{
    // A diagnostic thread name is optional; its allocation must not terminate
    // a detached worker before it can report storage status to its owner.
    try { juce::Thread::setCurrentThreadName("Recovery maintenance"); } catch (...) {}
#if JUCE_WINDOWS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;)
    {
        std::shared_ptr<State::Job> job;
        {
            const std::lock_guard lock(shared->mutex);
            const auto next = std::find_if(shared->jobs.begin(), shared->jobs.end(), [] (const auto& item)
                { return item->status.phase == Phase::queued || item->status.phase == Phase::collecting; });
            if (next == shared->jobs.end())
            {
                if (!shared->sweep || !shared->sweep->status.active
                    || (!shared->sweep->cancel.load() && shared->jobs.size() >= shared->config.jobs))
                { shared->running = false; return; }
            }
            else
            {
                job = *next;
                job->status.phase = job->markerPublished || job->resumeOnly ? Phase::collecting : Phase::retiring;
            }
        }
        if (!job) { discover(shared); juce::Thread::sleep(1); continue; }
        Status status;
        TerminalCollectionReport report;
        bool preserved = false;
        size_t orphanFiles = 0, orphanBytes = 0;
        using Stage = RecoveryDiagnosticState::Stage;
        const bool collectOnly = job->markerPublished || job->resumeOnly;
        auto stage = collectOnly ? Stage::collectTerminal : job->ticket ? Stage::retireSaved : Stage::discard;
        try
        {
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (shared->config.onBoundary) shared->config.onBoundary(collectOnly ? Phase::collecting : Phase::retiring, job->status.owner);
#endif
            RecoveryStore store(job->status.root, job->status.owner, shared->config.store);
            if (job->resumeOnly && job->status.head.sequence == 0)
            {
                SafeHead resolved;
                status = store.inspectTerminalHead(resolved);
                if (status)
                {
                    const std::lock_guard lock(shared->mutex);
                    job->status.head = std::move(resolved);
                    report.completed = job->status.head.sequence == 0;
                }
            }
            if (!collectOnly && job->automaticExpiry)
            {
                // Re-review current policy and head on every quantum/retry. A
                // discovery row is only a selector, never expiry authority.
                job->discard.reset();
                status = store.reviewConfiguredExpiry(job->discard, job->cancelled);
                if (status)
                {
                    const std::lock_guard lock(shared->mutex);
                    if (job->status.head != job->discard->frozenHead()) job->orphansCollected = false;
                    job->status.head = job->discard->frozenHead();
                }
                if (status && !job->orphansCollected)
                {
                    stage = Stage::collectOrphans;
                    OrphanCollectionReport orphans;
                    status = store.collectOrphans(job->status.head, orphans, shared->config.batch, job->cancelled);
                    orphanFiles = orphans.filesRemoved; orphanBytes = orphans.bytesRemoved;
                    if (status) job->orphansCollected = !orphans.more;
                }
                if (status && job->cancelled && job->cancelled())
                    status = { Error::cancelled, "expiry_sweep_cancelled" };
            }
            if (status && !collectOnly && (!job->automaticExpiry || job->orphansCollected))
            {
                stage = job->ticket ? Stage::retireSaved : Stage::discard;
                OwnerRetirement retirement;
                status = job->ticket ? RecoveryOwnerLifecycle::retireSaved(store, job->status.head, *job->ticket, retirement)
                                     : RecoveryOwnerLifecycle::discard(store, *job->discard, retirement);
                if (status)
                {
#if SINGLE_EDITOR_RECOVERY_TESTING
                    if (shared->config.afterRetirementPublished) shared->config.afterRetirementPublished(job->status.owner);
#endif
                    job->markerPublished = true;
                }
            }
            if (status && !report.completed && (collectOnly || job->markerPublished))
            {
                stage = Stage::collectTerminal;
                status = store.collectTerminal(job->status.head, report, shared->config.batch);
            }
        }
        catch (...) { status = { Error::resourceOrEnvironmentFault, "maintenance_worker_exception" }; }
        if (!status && job->automaticExpiry && !job->markerPublished
            && (status.error == Error::cancelled || status.code == "owner_in_use" || status.code == "owner_busy"
                || status.code == "owner_not_expired" || status.code == "expiry_activity_unproven"
                || status.code == "retention_policy_changed" || status.code == "retention_policy_missing"))
        {
            // These are expected eligibility outcomes, not failed deletions.
            // No terminal authority was published on any of these returns.
            preserved = true; report.completed = true; status = {};
        }
        if (!status && job->discard && !job->markerPublished)
        {
            // A failed return can follow durable marker publication. Resolve
            // cancellation only on this worker, never by UI I/O or a success bit.
            // Unknown/terminal-intent state remains owned and non-revocable.
            job->discardRevocable = false;
            try
            {
                std::optional<OwnerRetirement> existing;
                const auto inspected = RecoveryStore(job->status.root, job->status.owner, shared->config.store).inspectRetirement(existing);
                job->discardRevocable = inspected && !existing;
                if (job->automaticExpiry && inspected && existing && existing->head == job->status.head)
                    job->markerPublished = true;
            }
            catch (...) {} // Original failure remains reported; uncertainty cannot revoke authority.
        }
        {
            const std::lock_guard lock(shared->mutex);
            ++job->status.batches;
            job->status.filesRemoved += orphanFiles + report.filesRemoved;
            job->status.bytesRemoved += orphanBytes + report.bytesRemoved;
            job->status.preserved = preserved;
            if (preserved && shared->sweep && job->sweepOperation == shared->sweep->operation)
                ++shared->sweep->status.preservedOwners;
            job->status.failure = status;
            if (!status) shared->note(job->status, stage, status.error);
            job->status.phase = !status ? Phase::failed : report.completed ? Phase::completed : Phase::collecting;
        }
        // A bounded physical sweep yields between batches, and no worker is
        // kept waiting on failed jobs, UI input, or an empty queue.
        juce::Thread::sleep(1);
    }
}

Status RecoveryMaintenanceQueue::retry(const juce::File& root, const juce::Uuid& owner)
{
    const std::lock_guard lock(state->mutex);
    if (!state->failure && state->failure.code != "maintenance_worker_launch") return state->failure;
    const auto found = std::find_if(state->jobs.begin(), state->jobs.end(), [&] (const auto& job)
        { return job->status.root == root && job->status.owner == owner; });
    if (found == state->jobs.end()) return { Error::staleHead, "maintenance_unknown_owner" };
    auto& job = **found;
    if (job.status.phase != Phase::failed && !(job.status.phase == Phase::queued && !state->running))
        return { Error::staleHead, "maintenance_not_failed" };
    state->failure = {}; job.status.failure = {}; job.status.phase = Phase::queued;
    job.status.operationId = juce::Uuid();
    launch(state);
    return {};
}

RecoveryMaintenanceQueue::Progress RecoveryMaintenanceQueue::progress() const
{
    const std::lock_guard lock(state->mutex);
    Progress result;
    if (state->sweep) result.sweep = state->sweep->status;
    result.accepting = state->accepting; result.workerRunning = state->running; result.failure = state->failure;
    result.settled = !state->accepting && !state->running && state->failure;
    if (result.sweep.active) result.settled = false;
    for (const auto& job : state->jobs)
    {
        result.jobs.push_back(job->status);
        if (result.failure && !job->status.failure) result.failure = job->status.failure;
        if (job->status.phase != Phase::completed) result.settled = false;
    }
    return result;
}

std::vector<RecoveryMaintenanceQueue::JobStatus> RecoveryMaintenanceQueue::takeCompleted()
{
    const std::lock_guard lock(state->mutex);
    std::vector<JobStatus> result;
    for (const auto& job : state->jobs) if (job->status.phase == Phase::completed) result.push_back(job->status);
    std::erase_if(state->jobs, [] (const auto& job) { return job->status.phase == Phase::completed; });
    if (state->sweep && state->sweep->status.active && state->jobs.size() < state->config.jobs && state->failure)
        launch(state);
    return result;
}
void RecoveryMaintenanceQueue::close()
{
    const std::lock_guard lock(state->mutex);
    state->accepting = false;
    if (state->sweep && state->sweep->status.active)
    {
        state->sweep->cancel.store(true);
        launch(state);
    }
}
}
