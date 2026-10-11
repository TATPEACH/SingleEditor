#include "AsyncRecoveryWriter.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <map>
#include <set>
#include <limits>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::recovery
{
using Clock = std::chrono::steady_clock;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
namespace
{
// First member of the observed storage/callable, so its terminal follows the
// destruction of every actual member. It neither owns nor delays those members.
struct ObservedRecoveryLifetime final
{
    explicit ObservedRecoveryLifetime(OptionalLifecycleToken value) noexcept : token(std::move(value)) {}
    ObservedRecoveryLifetime(ObservedRecoveryLifetime&&) noexcept = default;
    ~ObservedRecoveryLifetime()
    {
        if (!token.isOpen()) return; // Includes a moved-from construction wrapper.
        if (constructed) token.release();
        else token.failed();
    }
    void created() noexcept { token.created(); constructed = true; }
    OptionalLifecycleToken token;
    bool constructed = false;
};
}
#endif
struct AsyncRecoveryWriter::State final
{
    struct Item final { Submission submission; Clock::time_point submitted; };
    State(juce::File root, juce::Uuid owner, Limits limits, Budget bounds
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , OptionalLifecycleToken& observed
#endif
        )
        :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
          observedLifetime(std::move(observed)),
#endif
          store(std::move(root), owner, limits), budget(bounds), ownerId(owner)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.created();
        progress.stateResourceId = observedLifetime.token.resourceId();
#endif
    }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    ObservedRecoveryLifetime observedLifetime;
#endif
    RecoveryStore store;
    std::shared_ptr<RecoveryOwnerLease> activity;
    Budget budget;
    juce::Uuid ownerId;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Item>> queue;
    Progress progress;
    bool retryRequired = false;
    bool initialised = false;
    bool collectBeforeRetry = false;
    decltype(Submission::compact) maintenance;
    bool maintenanceTurn = false;
    // A refused new submission leaves the previously accepted prefix valid.
    // A failed publication callback does not make that same promise.
    bool admissionGap = false;
    std::function<Status(std::shared_ptr<void>&)> cancellationAuthority;
    Status retainedAfter(const Submission* incoming, size_t& output) const try
    {
        std::map<const void*, size_t> backings;
        size_t total = 0, refusedRequirement = 0;
        const auto failure = [&]() -> Status
        {
            if (refusedRequirement == 0) return { Error::budgetExceeded, "queue_budget" };
            return byteCapacityFailure("queue_budget", ByteCapacity::Resource::retainedMemory,
                refusedRequirement, budget.retainedBytes, true);
        };
        const auto add = [&](size_t bytes)
        {
            if (bytes > budget.retainedBytes || total > budget.retainedBytes - bytes)
            {
                if (bytes <= std::numeric_limits<size_t>::max() - total) refusedRequirement = total + bytes;
                return false;
            }
            total += bytes; return true;
        };
        const auto include = [&](const Submission& submission) -> Status
        {
            if (submission.sharedBackings.size() > 32) return { Error::invalidPacket, "retained_backing_count" };
            size_t sharedBytes = 0;
            std::set<const void*> seen;
            for (const auto& backing : submission.sharedBackings)
            {
                const auto key = backing.identity.get();
                if (!key || backing.bytes == 0 || !seen.insert(key).second
                    || backing.bytes > submission.retainedBytes - sharedBytes)
                    return { Error::invalidPacket, "retained_backing_contract" };
                sharedBytes += backing.bytes;
                const auto [found, inserted] = backings.emplace(key, backing.bytes);
                if (!inserted && found->second != backing.bytes) return { Error::invalidPacket, "retained_backing_size" };
                if (inserted && !add(backing.bytes)) return failure();
            }
            if (!add(submission.retainedBytes - sharedBytes)) return failure();
            return {};
        };
        for (const auto& item : queue) if (const auto result = include(item->submission); !result) return result;
        if (incoming) if (const auto result = include(*incoming); !result) return result;
        output = total; return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "retained_accounting_exception" }; }
    // Caller holds the writer mutex. Recording owns no producer/capture and
    // cannot affect the storage result, continuity, admission or retry rules.
    void note(const juce::Uuid& id, RecoveryDiagnosticState::Stage stage, Error error) const
    {
        if (budget.diagnostics) budget.diagnostics->record({ id, ownerId, stage, error,
            { static_cast<int64_t>(progress.pendingPackets), static_cast<int64_t>(progress.retainedBytes),
              progress.safeHead.sequence, static_cast<int64_t>(progress.cancelledPackets), 0, 0 } });
    }
};

AsyncRecoveryWriter::AsyncRecoveryWriter(juce::File root, juce::Uuid owner, Limits limits, Budget budget)
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
{
    auto observed = OptionalLifecycleToken::request(budget.optionalLifecycleContext,
        OptionalLifecycleOwner::recovery, OptionalLifecycleResourceKind::recoveryWriterState);
    try
    {
        state = std::make_shared<State>(std::move(root), owner, limits, budget, observed);
    }
    catch (...)
    {
        // Allocation can fail before State takes the token. A partial State
        // construction instead terminates its own first-member observation.
        observed.failed();
        throw;
    }
}
#else
    : state(std::make_shared<State>(std::move(root), owner, limits, budget))
{
}
#endif
AsyncRecoveryWriter::~AsyncRecoveryWriter() { close(); }

void AsyncRecoveryWriter::launch(const std::shared_ptr<State>& shared)
{
    // Called with mutex held and workerRunning set. The worker owns all storage,
    // captures and completion state; it cannot call a destroyed UI/controller.
    shared->progress.operationId = juce::Uuid();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedWorker final
    {
        ObservedRecoveryLifetime lifetime;
        std::shared_ptr<State> shared;
        void operator()()
        {
            lifetime.created();
            lifetime.token.note(OptionalLifecycleAction::taskStarted);
            AsyncRecoveryWriter::run(shared);
            // All run-local captures are gone. This is a body completion,
            // not an OS join or destruction of the thread's shared State.
            lifetime.token.note(OptionalLifecycleAction::taskCompleted);
        }
    };
    try
    {
        std::thread(ObservedWorker {
            ObservedRecoveryLifetime(OptionalLifecycleToken::request(shared->budget.optionalLifecycleContext,
                OptionalLifecycleOwner::recovery, OptionalLifecycleResourceKind::recoveryWriterWorker,
                shared->progress.stateResourceId)), shared }).detach();
    }
#else
    try { std::thread([shared] { run(shared); }).detach(); }
#endif
    catch (...)
    {
        shared->progress.workerRunning = false;
        shared->retryRequired = true;
        shared->progress.failure = { Error::resourceOrEnvironmentFault, "worker_launch" };
        shared->note(shared->progress.operationId, RecoveryDiagnosticState::Stage::launch, shared->progress.failure.error);
        if (shared->progress.cancellation == Cancellation::requested)
        {
            shared->progress.cancellation = Cancellation::failed;
            shared->progress.cancellationFailure = shared->progress.failure;
            shared->cancellationAuthority = {};
        }
        shared->changed.notify_all();
    }
}

Status AsyncRecoveryWriter::submit(Submission submission)
{
    const std::lock_guard lock(state->mutex);
    auto& progress = state->progress;
    if (!progress.accepting) return { Error::resourceOrEnvironmentFault, "writer_closed" };
    if (progress.checkpointRequired) return { Error::staleHead, "checkpoint_required" };
    const auto rejectGap = [&] (const char* reason, ByteCapacity capacity = {})
    {
        state->admissionGap = true;
        progress.checkpointRequired = true;
        progress.failure = { Error::budgetExceeded, reason, capacity };
        progress.operationId = juce::Uuid();
        state->note(progress.operationId, RecoveryDiagnosticState::Stage::admission, progress.failure.error);
        return progress.failure;
    };
    if (!submission.prepare || state->budget.packets == 0 || state->budget.retainedBytes == 0
        || state->budget.maximumAge.count() <= 0)
        return rejectGap("invalid_submission_or_budget");
    size_t projected = 0;
    const auto accounting = state->retainedAfter(&submission, projected);
    if (!accounting) return rejectGap(accounting.code.toRawUTF8(), accounting.capacity);
    if (state->queue.size() >= state->budget.packets)
        return rejectGap("queue_budget");
    if (!state->queue.empty() && Clock::now() - state->queue.front()->submitted > state->budget.maximumAge)
        return rejectGap("queue_age");
    state->queue.push_back(std::make_shared<State::Item>(State::Item { std::move(submission), Clock::now() }));
    progress.pendingPackets = state->queue.size();
    progress.retainedBytes = projected;
    if (!progress.workerRunning && !state->retryRequired)
    {
        progress.workerRunning = true;
        launch(state);
    }
    // Success means accepted into the ordered queue, never "already durable".
    // A launch/write failure is observable via progress and explicit retry.
    return {};
}

Status AsyncRecoveryWriter::projectedRetainedBytes(const Submission& submission, size_t& output) const
{
    const std::lock_guard lock(state->mutex);
    return state->retainedAfter(&submission, output);
}

AsyncRecoveryWriter::Progress AsyncRecoveryWriter::progress() const
{
    const std::lock_guard lock(state->mutex);
    auto result = state->progress;
    if (result.failure && !state->queue.empty() && Clock::now() - state->queue.front()->submitted > state->budget.maximumAge)
        result.failure = { Error::resourceOrEnvironmentFault, "writer_lag" };
    return result;
}

void AsyncRecoveryWriter::run(const std::shared_ptr<State>& shared)
{
    for (;;)
    {
        std::shared_ptr<State::Item> current;
        SafeHead expected;
        bool collect = false;
        decltype(Submission::compact) maintenance;
        juce::Uuid operationId { nullptr };
        std::function<Status(std::shared_ptr<void>&)> cancelAuthority;
        {
            const std::lock_guard lock(shared->mutex);
            if (shared->progress.cancellation == Cancellation::requested)
                cancelAuthority = std::move(shared->cancellationAuthority);
            else if ((shared->queue.empty() && (!shared->maintenance || !shared->progress.accepting)) || shared->retryRequired)
            {
                if (!shared->progress.accepting && shared->queue.empty()) shared->activity.reset();
                shared->progress.workerRunning = false;
                shared->changed.notify_all();
                return;
            }
            operationId = juce::Uuid();
            shared->progress.operationId = operationId;
            if (!cancelAuthority)
            {
                expected = shared->progress.safeHead;
                if (shared->maintenance && shared->progress.accepting && (shared->queue.empty() || shared->maintenanceTurn))
                {
                    maintenance = std::move(shared->maintenance);
                    shared->maintenanceTurn = false;
                }
                else current = shared->queue.front();
                if (current)
                {
                    collect = shared->collectBeforeRetry;
                    shared->collectBeforeRetry = false;
                }
            }
        }
        if (maintenance)
        {
            OrphanCollectionReport report;
            Status maintained;
            const auto cancel = [&]
            {
                const std::lock_guard lock(shared->mutex);
                return !shared->progress.accepting || shared->progress.cancellation == Cancellation::requested;
            };
            try
            {
#if JUCE_WINDOWS
                const auto priority = GetThreadPriority(GetCurrentThread());
                if (priority == THREAD_PRIORITY_ERROR_RETURN || !SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                    maintained = { Error::resourceOrEnvironmentFault, "maintenance_priority" };
                else
                {
                    struct RestorePriority { int value; ~RestorePriority() { SetThreadPriority(GetCurrentThread(), value); } } restore { priority };
                    maintained = maintenance(shared->store, expected, report, cancel);
                }
#else
                maintained = maintenance(shared->store, expected, report, cancel);
#endif
            }
            catch (...) { maintained = { Error::resourceOrEnvironmentFault, "maintenance_exception" }; }
            {
                const std::lock_guard lock(shared->mutex);
                shared->progress.lastCollection = report;
                shared->progress.maintenanceFailure = maintained;
                if (maintained && report.more && shared->progress.accepting) shared->maintenance = std::move(maintenance);
                if (!maintained && maintained.error != Error::cancelled)
                    shared->note(operationId, RecoveryDiagnosticState::Stage::collectOrphans, maintained.error);
            }
            std::this_thread::yield();
            continue;
        }
        if (cancelAuthority)
        {
            Status permitted;
            std::shared_ptr<void> savedTarget;
            try
            {
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (shared->budget.onBoundary) shared->budget.onBoundary(Boundary::beforeCancellationCheck);
#endif
                permitted = cancelAuthority(savedTarget);
                if (permitted && !savedTarget) permitted = { Error::sourceConflict, "cancellation_missing_guard" };
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (permitted && shared->budget.onBoundary) shared->budget.onBoundary(Boundary::cancellationVerified);
#endif
            }
            catch (...) { permitted = { Error::resourceOrEnvironmentFault, "cancellation_authority_exception" }; }
            if (!permitted)
            {
                savedTarget.reset();
                cancelAuthority = {};
                const std::lock_guard lock(shared->mutex);
                shared->progress.cancellation = Cancellation::failed;
                shared->progress.cancellationFailure = permitted;
                shared->note(operationId, RecoveryDiagnosticState::Stage::savedCancellation, permitted.error);
                shared->retryRequired = true;
                shared->progress.workerRunning = false;
                shared->changed.notify_all();
                return; // Keep every queued capture and the durable prefix.
            }
            std::deque<std::shared_ptr<State::Item>> discarded;
            {
                const std::lock_guard lock(shared->mutex);
                shared->progress.cancelledPackets += shared->queue.size();
                discarded.swap(shared->queue);
                shared->collectBeforeRetry = false;
                // pending/retained remain truthful until captures are destroyed.
            }
            discarded.clear();
            savedTarget.reset();
            cancelAuthority = {};
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (shared->budget.onBoundary) shared->budget.onBoundary(Boundary::cancellationReleased);
#endif
            {
                const std::lock_guard lock(shared->mutex);
                shared->progress.pendingPackets = 0;
                shared->progress.retainedBytes = 0;
                shared->progress.cancellation = Cancellation::completed;
                shared->activity.reset();
                shared->progress.cancellationFailure = {};
                shared->retryRequired = true;
                shared->progress.workerRunning = false;
                shared->changed.notify_all();
            }
            return;
        }
        Status status;
        Status publicationStatus;
        AppendWork appendWork;
        SafeHead published = expected;
        media::DocumentIdentity publishedIdentity;
        using Stage = RecoveryDiagnosticState::Stage;
        auto stage = Stage::inspect;
        try
        {
            if (!shared->initialised)
            {
                const auto view = shared->store.inspect();
                status = view.status;
                if (status)
                {
                    expected = view.head;
                    published = view.head;
                    shared->initialised = true;
                }
            }
            CommitPacket packet;
            if (status && collect)
            {
                stage = Stage::collectOrphans;
                OrphanCollectionReport report;
                status = shared->store.collectOrphans(expected, report);
                const std::lock_guard lock(shared->mutex);
                shared->progress.lastCollection = report;
            }
            if (status && Clock::now() - current->submitted > shared->budget.maximumAge)
            {
                stage = Stage::admission;
                status = { Error::resourceOrEnvironmentFault, "writer_lag" };
            }
            if (status)
            {
                stage = Stage::prepare;
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (shared->budget.onBoundary) shared->budget.onBoundary(Boundary::beforePrepare);
#endif
                status = current->submission.prepare(packet);
            }
            if (status && Clock::now() - current->submitted > shared->budget.maximumAge)
            {
                stage = Stage::admission;
                status = { Error::resourceOrEnvironmentFault, "writer_lag" };
            }
            std::shared_ptr<const PublishedRecoveryCommit> receipt;
            if (status)
            {
                stage = Stage::append;
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (shared->budget.onBoundary) shared->budget.onBoundary(Boundary::beforePublish);
#endif
                status = shared->store.append(expected, packet, published, current->submission.published ? &receipt : nullptr,
                                              &appendWork, &shared->activity);
            }
            if (status)
            {
                stage = Stage::publication;
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (shared->budget.onBoundary) shared->budget.onBoundary(Boundary::afterPublish);
#endif
                publishedIdentity = packet.identity.document;
                if (current->submission.published)
                {
                    try { publicationStatus = current->submission.published(std::move(receipt)); }
                    catch (...) { publicationStatus = { Error::resourceOrEnvironmentFault, "publication_consumer_exception" }; }
                }
            }
        }
        catch (...) { status = { Error::resourceOrEnvironmentFault, "worker_exception" }; }
        {
            const std::lock_guard lock(shared->mutex);
            shared->progress.safeHead = published;
            if (!status)
            {
                shared->retryRequired = true;
                shared->progress.failure = status;
                shared->note(operationId, stage, status.error);
            }
            else
            {
                shared->progress.protectedIdentity = publishedIdentity;
                shared->queue.pop_front();
                shared->progress.pendingPackets = shared->queue.size();
                const auto accounted = shared->retainedAfter(nullptr, shared->progress.retainedBytes);
                if (!accounted && publicationStatus) publicationStatus = accounted;
                if (!publicationStatus)
                {
                    shared->admissionGap = false;
                    shared->progress.checkpointRequired = true;
                    shared->retryRequired = true;
                    shared->progress.failure = publicationStatus;
                    shared->note(operationId, Stage::publication, publicationStatus.error);
                }
                else if (!shared->progress.checkpointRequired) shared->progress.failure = {};
                if (publicationStatus && appendWork.compactionSuggested && current->submission.compact && shared->progress.accepting)
                {
                    shared->maintenance = current->submission.compact;
                    shared->maintenanceTurn = true;
                }
            }
            shared->changed.notify_all();
        }
        // Release retained state/audio before publishing the idle worker state.
        current.reset();
    }
}

Status AsyncRecoveryWriter::retry(bool collectOrphans)
{ return retryInternal(collectOrphans, false); }
Status AsyncRecoveryWriter::retryDraining(bool collectOrphans)
{ return retryInternal(collectOrphans, true); }
Status AsyncRecoveryWriter::retryInternal(bool collectOrphans, bool draining)
{
    const std::lock_guard lock(state->mutex);
    if (state->progress.cancellation == Cancellation::requested || state->progress.cancellation == Cancellation::completed)
        return { Error::cancelled, "writer_cancellation_terminal" };
    if (state->progress.accepting == draining)
        return { Error::resourceOrEnvironmentFault, draining ? "writer_not_closed" : "writer_closed" };
    if (state->progress.checkpointRequired && (!draining || !state->admissionGap))
        return { Error::staleHead, "checkpoint_required" };
    if (state->progress.workerRunning) return { Error::resourceOrEnvironmentFault, "worker_busy" };
    state->retryRequired = false;
    state->collectBeforeRetry = collectOrphans && !state->queue.empty();
    state->progress.failure = {};
    state->progress.cancellation = Cancellation::none;
    state->progress.cancellationFailure = {};
    for (auto& item : state->queue) item->submitted = Clock::now();
    if (!state->queue.empty())
    {
        state->progress.workerRunning = true;
        launch(state);
    }
    return state->progress.failure;
}

Status AsyncRecoveryWriter::cancelAfterSaved(std::function<Status(std::shared_ptr<void>&)> authority)
{
    const std::lock_guard lock(state->mutex);
    if (!authority || state->progress.accepting) return { Error::invalidPacket, "cancellation_open_writer" };
    if (state->progress.cancellation == Cancellation::requested || state->progress.cancellation == Cancellation::completed)
        return { Error::cancelled, "writer_cancellation_terminal" };
    state->cancellationAuthority = std::move(authority);
    state->progress.cancellation = Cancellation::requested;
    state->progress.cancellationFailure = {};
    if (!state->progress.workerRunning)
    {
        state->progress.workerRunning = true;
        launch(state);
    }
    return state->progress.cancellationFailure;
}
void AsyncRecoveryWriter::close()
{
    const std::lock_guard lock(state->mutex);
    state->progress.accepting = false;
    if (!state->progress.workerRunning && state->queue.empty()) state->activity.reset();
}
bool AsyncRecoveryWriter::drain(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state->mutex);
    state->changed.wait_for(lock, std::max(timeout, std::chrono::milliseconds(0)), [&]
    { return !state->progress.workerRunning; });
    return !state->progress.workerRunning && state->queue.empty() && !state->progress.checkpointRequired
        && state->progress.cancellation == Cancellation::none;
}
} // namespace singleeditor::recovery
