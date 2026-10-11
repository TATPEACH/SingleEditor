#include "RecoveryRestoreQueue.h"
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
namespace
{
bool terminal(RecoveryAttemptStage stage)
{ return stage == RecoveryAttemptStage::ready || stage == RecoveryAttemptStage::failed || stage == RecoveryAttemptStage::cancelled; }
RecoveryDiagnosticState::Stage diagnosticStage(RecoveryAttemptStage stage)
{
    using Phase = RecoveryDiagnosticState::Stage;
    switch (stage)
    {
        case RecoveryAttemptStage::queued: return Phase::restoreQueued;
        case RecoveryAttemptStage::validating: return Phase::restoreValidation;
        case RecoveryAttemptStage::resolvingSource: return Phase::restoreSource;
        case RecoveryAttemptStage::replayingCheckpoint: return Phase::restoreReplay;
        case RecoveryAttemptStage::preparingCore: return Phase::restoreCore;
        case RecoveryAttemptStage::coreReady: return Phase::restoreCorePublication;
        case RecoveryAttemptStage::materializingFull: return Phase::restoreFull;
        case RecoveryAttemptStage::ready: return Phase::restoreFinalize;
        default: return Phase::restoreAttemptClose;
    }
}
}
struct RecoveryRestoreQueue::State final
{
    struct Job final
    {
        Request request;
        JobStatus status;
        std::unique_ptr<RecoveryRestoreCandidate> candidate; // Worker-confined after admission.
        std::atomic<bool> cancelled { false };
        bool storagePrepared = false, coreOffered = false;
        Status admissionFailure;
        std::shared_ptr<const RecoveredCandidateDocument> core;
        SingleTrackDocument::StatePtr originalCore;
        std::shared_ptr<WaveformModel::PeakCache> fullPeaks;
    };
    explicit State(Config value) : config(std::move(value)), allocation(std::make_shared<RecoveryAllocationBudget>(config.residentMappingBytes)) {}
    Config config;
    std::shared_ptr<RecoveryAllocationBudget> allocation;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Job>> jobs;
    Status failure;
    juce::Uuid launchOperation { nullptr };
    size_t next = 0;
    bool closing = false, running = false, changed = false;
    // Called only at actual result publication while the queue mutex is held.
    void note(const JobStatus* job, RecoveryDiagnosticState::Stage stage, Error error) noexcept
    {
        if (!config.diagnostics || error == Error::none || error == Error::cancelled) return;
        try
        {
            size_t bytes = 0;
            const auto resident = allocation->tryUsedBytes(bytes) ? static_cast<int64_t>(bytes) : -1;
            config.diagnostics->record({ job ? job->attempt : launchOperation, job ? job->owner : juce::Uuid(nullptr), stage, error,
                { static_cast<int64_t>(jobs.size()), resident, job ? job->checkpoint.sequence : 0,
                  job && job->coreAdmitted ? 1 : 0, job && job->waitingForAdmission ? 1 : 0,
                  job ? static_cast<int64_t>(job->attemptCloseFailure.error) : 0 } });
        }
        catch (...) {}
    }
};
RecoveryRestoreQueue::RecoveryRestoreQueue(Config config) : state(std::make_shared<State>(std::move(config))) {}
RecoveryRestoreQueue::~RecoveryRestoreQueue() { close(); }

Status RecoveryRestoreQueue::admit(const std::shared_ptr<State>& shared, Request request, juce::Uuid& output)
{
    const auto& config = shared->config;
    if (shared->closing) return { Error::resourceOrEnvironmentFault, "restore_service_closing" };
    if (shared->jobs.size() >= config.jobs) return { Error::budgetExceeded, "restore_job_budget" };
    if (config.root == juce::File() || config.scratch == juce::File() || config.root == config.scratch
        || config.scratch.isAChildOf(config.root) || config.root.isAChildOf(config.scratch)
        || config.jobs < 1 || config.jobs > 32 || config.residentMappingBytes < 1
        || request.owner.isNull() || request.observedHead.sequence <= 0 || request.selectedHead.sequence <= 0)
        return { Error::invalidPacket, "restore_service_request" };
    try
    {
        auto job = std::make_shared<State::Job>();
        job->request = std::move(request);
        RecoveryRestoreCandidate::Request parameters;
        parameters.observedOwnerHead = job->request.observedHead; parameters.checkpointHead = job->request.selectedHead;
        parameters.attemptDirectory = RecoveryStorageLayout(config.root).attemptDirectory();
        parameters.scratchDirectory = config.scratch; parameters.appBuild = config.appBuild;
        parameters.source = job->request.source; parameters.retryQuarantinedOnce = job->request.retryQuarantinedOnce;
        parameters.limits = config.graph; parameters.allocationBudget = shared->allocation;
        parameters.attemptLimits.rootBytes = config.storage.attemptBytes;
        parameters.attemptLimits.rootEntries = config.storage.attemptEntries;
        parameters.attemptLimits.freeSpaceReserve = config.storage.freeSpaceReserve;
        job->candidate = std::make_unique<RecoveryRestoreCandidate>(
            RecoveryStore(RecoveryStorageLayout(config.root).journalDirectory(), job->request.owner, config.store), std::move(parameters));
        job->status.attempt = job->candidate->getAttemptId(); job->status.owner = job->request.owner;
        job->status.checkpoint = job->request.selectedHead;
        shared->jobs.push_back(job); output = job->status.attempt; shared->changed = true;
        launch(shared);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "restore_admission_exception" }; }
}
Status RecoveryRestoreQueue::start(Request request, juce::Uuid& output)
{ const std::lock_guard lock(state->mutex); return admit(state, std::move(request), output); }
void RecoveryRestoreQueue::launch(const std::shared_ptr<State>& shared)
{
    if (shared->running) return;
    shared->launchOperation = juce::Uuid();
    shared->running = true;
    try { std::thread([shared] { run(shared); }).detach(); shared->failure = {}; }
    catch (...)
    {
        shared->running = false; shared->changed = true;
        shared->failure = { Error::resourceOrEnvironmentFault, "restore_worker_launch" };
        shared->note(nullptr, RecoveryDiagnosticState::Stage::restoreLaunch, shared->failure.error);
    }
}
Status RecoveryRestoreQueue::cancel(const juce::Uuid& attempt)
{
    const std::lock_guard lock(state->mutex);
    for (const auto& job : state->jobs) if (job->status.attempt == attempt)
    {
        if (terminal(job->status.stage)) return { Error::staleHead, "restore_already_terminal" };
        job->cancelled = true; state->changed = true; launch(state); return {};
    }
    return { Error::staleHead, "restore_unknown_attempt" };
}
Status RecoveryRestoreQueue::acknowledgeCore(const juce::Uuid& attempt, bool admitted)
{
    const std::lock_guard lock(state->mutex);
    for (const auto& job : state->jobs) if (job->status.attempt == attempt)
    {
        if (!job->status.waitingForAdmission || !job->coreOffered || job->cancelled || terminal(job->status.stage))
            return { Error::staleHead, "restore_admission_obsolete" };
        job->status.waitingForAdmission = false;
        job->status.coreAdmitted = admitted;
        if (!admitted) job->admissionFailure = { Error::resourceOrEnvironmentFault, "restore_playlist_admission" };
        // Candidate still owns the packet until worker completion/retirement.
        job->core.reset(); state->changed = true; launch(state); return {};
    }
    return { Error::staleHead, "restore_unknown_attempt" };
}
Status RecoveryRestoreQueue::forget(const juce::Uuid& attempt)
{
    const std::lock_guard lock(state->mutex);
    const auto found = std::find_if(state->jobs.begin(), state->jobs.end(), [&](const auto& job) { return job->status.attempt == attempt; });
    if (found == state->jobs.end()) return { Error::staleHead, "restore_unknown_attempt" };
    if (!terminal((*found)->status.stage) || (*found)->core || (*found)->fullPeaks)
        return { Error::resourceOrEnvironmentFault, "restore_work_or_publication_pending" };
    state->jobs.erase(found); state->changed = true; return {};
}
Status RecoveryRestoreQueue::retry(const juce::Uuid& attempt, bool once, juce::Uuid& output)
{
    const std::lock_guard lock(state->mutex);
    const auto found = std::find_if(state->jobs.begin(), state->jobs.end(), [&](const auto& job) { return job->status.attempt == attempt; });
    if (found == state->jobs.end()) return { Error::staleHead, "restore_unknown_attempt" };
    if (!terminal((*found)->status.stage) || (*found)->status.stage == RecoveryAttemptStage::ready || (*found)->core || (*found)->fullPeaks)
    {
        // A thread-launch failure retains the same accepted candidate; no new
        // nonce or attempt record is needed merely to retry its scheduling.
        if (!terminal((*found)->status.stage) && !state->running && state->failure.code == "restore_worker_launch")
        { launch(state); output = attempt; return state->failure; }
        return { Error::resourceOrEnvironmentFault, "restore_retry_not_ready" };
    }
    auto previous = *found;
    const auto index = static_cast<size_t>(found - state->jobs.begin());
    auto request = previous->request; request.retryQuarantinedOnce = once;
    state->jobs.erase(found);
    const auto status = admit(state, std::move(request), output);
    if (!status) state->jobs.insert(state->jobs.begin() + static_cast<std::ptrdiff_t>(index), std::move(previous));
    return status;
}
bool RecoveryRestoreQueue::needsPoll() const
{ const std::lock_guard lock(state->mutex); return state->running || state->changed; }
RecoveryRestoreQueue::Health RecoveryRestoreQueue::health() const
{
    const std::lock_guard lock(state->mutex);
    Health result; result.failure = state->failure; result.workerRunning = state->running;
    result.residentMappingBytes = state->allocation->usedBytes();
    for (const auto& job : state->jobs) result.jobs.push_back(job->status);
    return result;
}
bool RecoveryRestoreQueue::tryDiagnosticObservation(const juce::Uuid& selected, DiagnosticObservation& result) const
{
    result = {};
    const std::unique_lock lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    result.launchOperation = state->launchOperation; result.schedulingFailure = state->failure;
    result.jobs = state->jobs.size(); result.workerRunning = state->running;
    size_t bytes = 0;
    if (state->allocation->tryUsedBytes(bytes)) result.residentMappingBytes = bytes;
    if (!selected.isNull())
        for (const auto& job : state->jobs) if (job->status.attempt == selected) { result.selected = job->status; break; }
    return true;
}
std::vector<RecoveryRestoreQueue::Publication> RecoveryRestoreQueue::takePublications()
{
    const std::lock_guard lock(state->mutex);
    std::vector<Publication> result;
    for (const auto& job : state->jobs)
    {
        if (job->core && !job->coreOffered && !job->cancelled)
        { result.push_back({ job->status.attempt, job->core, job->originalCore, {} }); job->coreOffered = true; }
        if (job->fullPeaks)
        {
            result.push_back({ job->status.attempt, {}, std::move(job->originalCore), std::move(job->fullPeaks) });
        }
    }
    state->changed = false;
    return result;
}
void RecoveryRestoreQueue::close()
{
    const std::lock_guard lock(state->mutex);
    if (state->closing) return;
    state->closing = true;
    bool pending = false;
    for (const auto& job : state->jobs) if (!terminal(job->status.stage)) { job->cancelled = true; pending = true; }
    if (pending) launch(state);
}

void RecoveryRestoreQueue::run(const std::shared_ptr<State>& shared)
{
    try { juce::Thread::setCurrentThreadName("Recovery restore"); } catch (...) {}
#if JUCE_WINDOWS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;)
    {
        std::shared_ptr<State::Job> job;
        Status admissionFailure;
        {
            const std::lock_guard lock(shared->mutex);
            for (size_t i = 0; i < shared->jobs.size(); ++i)
            {
                const auto index = (shared->next + i) % shared->jobs.size(); const auto& candidate = shared->jobs[index];
                if (!terminal(candidate->status.stage)
                    && (!candidate->status.waitingForAdmission || candidate->cancelled || !candidate->admissionFailure))
                { job = candidate; shared->next = (index + 1) % shared->jobs.size(); break; }
            }
            if (!job) { shared->running = false; shared->changed = true; return; }
            admissionFailure = job->admissionFailure;
        }
        Status result;
        bool storageFailure = false;
        std::shared_ptr<const RecoveredCandidateDocument> core;
        std::shared_ptr<WaveformModel::PeakCache> peaks;
        try
        {
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (shared->config.beforeAdvance) shared->config.beforeAdvance(job->candidate->getStage(), job->status.attempt);
#endif
            if (job->cancelled) job->candidate->requestCancel();
            if (!admissionFailure) result = job->candidate->abort(admissionFailure);
            else
            {
                if (!job->storagePrepared && !job->cancelled)
                {
                    result = RecoveryStore(RecoveryStorageLayout(shared->config.root).journalDirectory(), job->request.owner,
                        shared->config.store).checkHead(job->request.observedHead);
                    if (result) result = RecoveryStorageLayout(shared->config.root).prepare(shared->config.storage);
                    if (result) result = ProgressiveRecoveryAudio::prepareScratchDirectory(shared->config.scratch);
                    if (result) job->storagePrepared = true;
                    else { storageFailure = true; job->candidate->abort(result); }
                }
                if (result) result = job->candidate->advance();
            }
            if (result && job->candidate->getStage() == RecoveryAttemptStage::coreReady)
                result = job->candidate->acquireCore(job->status.attempt, core);
            if (result && job->candidate->getStage() == RecoveryAttemptStage::ready) peaks = job->candidate->takeFullPeaks();
        }
        catch (...)
        {
            result = { Error::resourceOrEnvironmentFault, "restore_service_worker_exception" };
            job->candidate->abort(result);
        }
        const auto stage = job->candidate->getStage();
        const auto failureStage = job->candidate->getFailureStage();
        const auto closeFailure = job->candidate->getAttemptCloseStatus();
        // Any reader/mapping cleanup belongs to this worker. Published readers
        // retain their allocation lease and owner lease independently.
        if (terminal(stage)) job->candidate.reset();
        {
            const std::lock_guard lock(shared->mutex);
            job->status.stage = stage; job->status.failureStage = failureStage;
            job->status.failure = result; job->status.attemptCloseFailure = closeFailure;
            if (core)
            {
                job->core = std::move(core); job->originalCore = job->core->coreState;
                job->status.waitingForAdmission = true;
            }
            if (peaks) job->fullPeaks = std::move(peaks);
            if (terminal(stage))
            {
                job->status.waitingForAdmission = false; job->core.reset();
                if (!job->fullPeaks) job->originalCore.reset();
            }
            if (!result && result.error != Error::cancelled)
                shared->note(&job->status, !admissionFailure ? RecoveryDiagnosticState::Stage::restoreAdmission
                    : storageFailure ? RecoveryDiagnosticState::Stage::restoreStorage : diagnosticStage(failureStage), result.error);
            else if (!closeFailure)
                shared->note(&job->status, RecoveryDiagnosticState::Stage::restoreAttemptClose, closeFailure.error);
            shared->changed = true;
        }
    }
}
}
