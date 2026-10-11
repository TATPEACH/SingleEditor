#pragma once

#include "ProgressiveRecoveryAudio.h"
#include "RecoveryCheckpointCodec.h"
#include "RecoverySourceBinding.h"
#include "RecoveryAttemptStore.h"
#include "RecoveryAllocationBudget.h"
#include "Waveform/WaveformModel.h"
#include <atomic>

namespace singleeditor::recovery
{
// A real isolated dirty document and its truthful first-view peaks. The future
// playlist owner retains this packet without selecting/playing it automatically.
// Retire it off the audio callback. Later peak results still name coreState;
// they must not replace peaks for a user-edited document generation.
struct RecoveredCandidateDocument final
{
    juce::Uuid attempt;
    std::unique_ptr<SingleTrackDocument> document;
    SingleTrackDocument::StatePtr coreState;
    std::shared_ptr<WaveformModel::PeakCache> firstPeaks;
};

// Explicit worker state machine for the self-contained detached Copy branch.
// No thread/timer/GUI/device or filesystem work in construction. All methods
// except requestCancel are serialized on one owner worker; the service publishes
// progress/packets to the UI. Scheduling, playlist admission and continued recovery protection are separate
// mandatory integration concerns, not supplied by this class.
class RecoveryRestoreCandidate final
{
public:
    using Stage = RecoveryAttemptStage;
    struct Request final
    {
        SafeHead observedOwnerHead;
        SafeHead checkpointHead; // Selected checkpoint or mutation point on the frozen chain, including an earlier one.
        juce::File scratchDirectory;
        juce::File attemptDirectory; // Separate from journal/CAS and scratch.
        AttemptLimits attemptLimits;
        juce::String appBuild;
        bool retryQuarantinedOnce = false;
        AudioGraphLimits limits;
        std::shared_ptr<RecoveryAllocationBudget> allocationBudget;
        int previewBins = 1024;
        double previewSeconds = 8.0;
        RecoverySourceBinding::Request source;
        // Optional extra service-owner/generation fence. This cannot bypass the
        // built-in source resolver and within-attempt filesystem checks.
        std::function<Status(const media::IdentitySnapshot&)> ownerFence;
    };

    RecoveryRestoreCandidate(RecoveryStore, Request);
    ~RecoveryRestoreCandidate();
    juce::Uuid getAttemptId() const noexcept { return attempt; }
    Stage getStage() const noexcept { return stage; }
    Stage getFailureStage() const noexcept { return failureStage; }
    const Status& getStatus() const noexcept { return status; }
    const Status& getAttemptCloseStatus() const noexcept { return attemptCloseStatus; }
    const std::optional<AttemptRecord>& getAttemptRecord() const noexcept { return attemptRecord; }
    const std::optional<SourceResolution>& getSourceResolution() const noexcept { return sourceResolution; }
    void requestCancel() noexcept { cancellation.store(true, std::memory_order_release); }
    // One bounded stage or one CAS chunk per call; no implicit retry on failure.
    Status advance();
    // Worker handoff, exactly once, after BOTH actual peaks and playable audio.
    // An obsolete request token cannot acquire or cancel the current attempt.
    // Failure preserves caller output. Service still performs atomic playlist
    // insertion on its UI owner; this API never selects or replaces live audio.
    Status acquireCore(const juce::Uuid& expectedAttempt, std::shared_ptr<const RecoveredCandidateDocument>&);
    // Serialized worker rejection (for example UI admission/resource failure),
    // recorded as a controlled failure before terminal success is published.
    Status abort(Status);
    std::shared_ptr<WaveformModel::PeakCache> takeFullPeaks() noexcept;
    std::shared_ptr<const WaveformModel::PeakCache> getFullPeaks() const noexcept { return fullPeaks; }

private:
    Status checkFences();
    Status fail(Status);
    void retire();
    RecoveryStore store;
    Request request;
    const juce::Uuid attempt;
    std::atomic<bool> cancellation { false };
    Stage stage = Stage::queued;
    Stage failureStage = Stage::queued;
    Status status;
    Status attemptCloseStatus;
    std::unique_ptr<RecoveryAttemptWriter> attemptWriter;
    std::optional<AttemptRecord> attemptRecord;
    // Covers semantic preparation before progressive readers take over the
    // same real owner lease. A selected earlier point cannot be pruned mid-restore.
    std::shared_ptr<RecoveryOwnerLease> preparationLease;
    bool hasMetadata = false, acquired = false;
    ManifestCommit selected;
    PreparedRecoveryCheckpoint prepared;
    std::unique_ptr<RecoverySourceBinding> sourceBinding;
    std::optional<SourceResolution> sourceResolution;
    SingleTrackDocument::CommittedStateSnapshot backing;
    std::vector<std::unique_ptr<ProgressiveRecoveryAudio>> sources;
    std::shared_ptr<RecoveredCandidateDocument> core;
    std::shared_ptr<WaveformModel::PeakCache> fullPeaks;
    std::shared_ptr<const void> allocationLease;
};
}
