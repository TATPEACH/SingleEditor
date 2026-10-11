#pragma once

#include "RecoveryMutationProducer.h"

namespace singleeditor::recovery
{
class SavedOwnerRetirement;
// Document-owner-thread controller, attached explicitly after CoreReady. One
// observer spans all saves; each source epoch owns its immutable writer queue.
// No I/O, thread, timer or owner directory before an actual dirty checkpoint.
// The host still owns UI, bounded-close choices and durable tombstone policy.
class RecoveryProtectionSession final
{
public:
    struct Config final
    {
        RecoveryMutationProducer::Config producer;
        size_t retiredEpochs = 8;
        size_t retainedBytes = 256 * 1024 * 1024;
    };
    struct Health final
    {
        media::IdentitySnapshot observed;
        RecoveryMutationProducer::Health current;
        std::vector<RecoveryMutationProducer::Health> retired;
        Status failure;
        size_t pendingPackets = 0, retainedBytes = 0;
        bool currentProtected = false, checkpointRequired = false, closed = false;
    };
    static Status attachPostCore(SingleTrackDocument&, Config,
        std::unique_ptr<RecoveryProtectionSession>&,
        RecoveryMutationProducer::Admission = RecoveryMutationProducer::Admission::observeClean);
    ~RecoveryProtectionSession();
    Health health() const;
    // Retired failures are not abandoned. Retry identifies exactly one owner;
    // it cannot reopen that epoch for future edits or skip its failed front.
    Status retry(const juce::Uuid& owner, bool collectOrphans = false);
    // Explicit repair after an admission/queue gap captures the current full
    // state/history. Older failed queues remain owned until explicitly drained.
    Status checkpointCurrent();
    // Explicit PostCore readmission of this same Document after a cancelled
    // close. All old producers stay closed; only a new epoch observes edits.
    // Preserve gap actors in this controller until its full checkpoint proves
    // runtime supersession. No deletion or saved-retirement authority is issued.
    Status resumePostCore(size_t maximumRetainedBytes);
    // Owner thread, no I/O. Capture authority from the still-owned exact epoch
    // before takeDrainedEpochs erases it. Health is only a selector, never proof.
    // Failure preserves the actor and caller output; no queue is released here.
    Status prepareSavedRetirement(const juce::Uuid& owner, const SingleTrackDocument::SaveSnapshot&,
        const SingleTrackDocument::SaveWriteResult&, std::unique_ptr<SavedOwnerRetirement>&) const;
    // Actual Save permits a separate asynchronous runtime cancellation, not
    // retirement of a degraded/older durable prefix. Observe cancellation health;
    // a completed cancellation is releasable but deliberately does not drain.
    Status cancelSavedQueue(const juce::Uuid& owner, const SingleTrackDocument::SaveSnapshot&,
        const SingleTrackDocument::SaveWriteResult&);
    // Releases drained runtime state and returns its metadata. A missed-event
    // prefix needs a subsequently captured protected checkpoint before release
    // (even after observation closes); numeric versions are not time order. Its gap
    // remains in returned health. Prepare saved retirement first if needed;
    // these returned facts cannot recreate destroyed producer authority.
    // Completed cancelled epochs are also returned, with their explicit
    // cancellation/failure counters intact; never interpret them as durable.
    // No storage deletion/tombstone/CAS release.
    std::vector<RecoveryMutationProducer::Health> takeDrainedEpochs();
    bool drain(std::chrono::milliseconds totalBudget);
    void close(); // Nonblocking; accepted queues remain owned until destruction.

private:
    RecoveryProtectionSession(SingleTrackDocument&, Config, media::IdentitySnapshot);
    Status startEpoch(const media::IdentitySnapshot&, std::optional<SingleTrackDocument::CommittedStateSnapshot>);
    Status checkRetained(const SingleTrackDocument::CommittedStateSnapshot&, RecoveryMutationProducer&) const;
    void consume(DocumentRecoveryCapture::Capture);
    void fail(Status);
    void note(const juce::Uuid&, RecoveryDiagnosticState::Stage, const Status&, uint64_t sequence = 0) const noexcept;
    SingleTrackDocument& document;
    Config config;
    media::IdentitySnapshot observed;
    std::unique_ptr<DocumentRecoveryCapture> capture;
    std::unique_ptr<RecoveryMutationProducer> current;
    std::vector<std::unique_ptr<RecoveryMutationProducer>> retired;
    Status failure;
    juce::Uuid observationId { nullptr };
    uint64_t eventSequence = 0;
    bool checkpointRequired = false, closed = false;
};
}
