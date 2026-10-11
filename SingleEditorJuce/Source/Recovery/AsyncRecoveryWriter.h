#pragma once

#include "RecoveryStore.h"
#include "RecoveryDiagnosticState.h"
#include <chrono>
#include <memory>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor::recovery
{
// Owner-scoped worker. No thread or file access until an accepted submission.
// The prepare closure retains immutable captures and executes only on the worker.
// A rejected committed mutation poisons continuity: a fresh checkpoint is needed
// before later edits can be represented. Retry never skips a failed front item.
class AsyncRecoveryWriter final
{
public:
    enum class Cancellation { none, requested, completed, failed };
#if SINGLE_EDITOR_RECOVERY_TESTING
    enum class Boundary { beforePrepare, beforePublish, afterPublish, beforeCancellationCheck, cancellationVerified, cancellationReleased };
#endif
    struct Budget final
    {
        size_t packets = 128;
        size_t retainedBytes = 256 * 1024 * 1024;
        // Internal Pilot admission/prepare deadline, not a durability SLA.
        // The ordinary 183 MiB PCM corpus alone needs about 1 s to prepare.
        // Retained bytes/packet count remain bounded; overdue work still stops.
        std::chrono::milliseconds maximumAge { 5000 };
        std::shared_ptr<RecoveryDiagnosticState> diagnostics;
#if SINGLE_EDITOR_RECOVERY_TESTING
        std::function<void(Boundary)> onBoundary;
#endif
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        // Captured at Session admission. Workers never read construction TLS.
        OptionalLifecycleContext optionalLifecycleContext;
#endif
    };
    struct RetainedBacking final
    {
        // Strong identity keeps an immutable backing's address from being
        // recycled while another queued capture still shares its allocation.
        std::shared_ptr<const void> identity;
        size_t bytes = 0;
    };
    struct Submission final
    {
        size_t retainedBytes = 0;
        std::function<Status(CommitPacket&)> prepare;
        // Worker-only, after durable append. Failure preserves the published
        // acknowledgement but stops the tail and requires a fresh checkpoint.
        std::function<Status(std::shared_ptr<const PublishedRecoveryCommit>)> published;
        // retainedBytes includes these backings plus this capture's metadata.
        // Only equal live identities with equal byte counts share a charge.
        std::vector<RetainedBacking> sharedBackings;
        std::function<Status(RecoveryStore&, const SafeHead&, OrphanCollectionReport&, std::function<bool()>)> compact;
    };
    struct Progress final
    {
        juce::Uuid operationId { nullptr }; // Actual latest attempt, not a Document version.
        SafeHead safeHead;
        // Set only after append publishes the corresponding head. Queue
        // acceptance/preparation cannot advance the protected Document version.
        media::DocumentIdentity protectedIdentity;
        Status failure;
        size_t pendingPackets = 0;
        size_t retainedBytes = 0;
        bool workerRunning = false;
        bool accepting = true;
        bool checkpointRequired = false;
        Cancellation cancellation = Cancellation::none;
        Status cancellationFailure;
        size_t cancelledPackets = 0;
        OrphanCollectionReport lastCollection;
        Status maintenanceFailure;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        // Joins the real owner UUID in producer Health to this State lifetime.
        std::uint64_t stateResourceId = 0;
#endif
    };

    AsyncRecoveryWriter(juce::File root, juce::Uuid owner, Limits limits, Budget budget);
    ~AsyncRecoveryWriter();
    Status submit(Submission);
    // Owner-thread preflight for the enclosing Session's aggregate budget.
    // Includes all queued captures plus the incoming one; performs no admission,
    // I/O or continuity change. Worker draining can only reduce this bound.
    Status projectedRetainedBytes(const Submission&, size_t&) const;
    Progress progress() const;
    // Explicit retry only; a queue gap cannot be repaired by replaying its tail.
    // Optional explicit maintenance stays on the same serialized worker and
    // precedes retrying the retained front packet; it never skips a queue gap.
    Status retry(bool collectOrphans = false);
    // Explicit owner-controlled retry after close. Replays only the already
    // accepted queue and never makes this writer accept new mutations again.
    // A rejected later submission permits draining its accepted prefix while
    // checkpointRequired stays true. A publication callback failure does not.
    Status retryDraining(bool collectOrphans = false);
    // Stop accepting, then let the accepted prefix finish. No blocking destructor.
    void close();
    bool drain(std::chrono::milliseconds timeout);

private:
    friend class RecoveryOwnerLifecycle;
    // Only lifecycle's actual-Save proof can request this. The worker obtains
    // a nonempty saved-target guard and keeps it through capture destruction.
    // This revokes queued work, never durable storage or a published head.
    Status cancelAfterSaved(std::function<Status(std::shared_ptr<void>&)>);
    struct State;
    static void launch(const std::shared_ptr<State>&);
    static void run(const std::shared_ptr<State>&);
    Status retryInternal(bool collectOrphans, bool draining);
    std::shared_ptr<State> state;
};
} // namespace singleeditor::recovery
