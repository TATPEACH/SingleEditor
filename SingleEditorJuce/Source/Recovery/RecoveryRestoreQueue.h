#pragma once
#include "RecoveryRestoreCandidate.h"
#include "RecoveryStorageLayout.h"
#include "RecoveryDiagnosticState.h"

namespace singleeditor::recovery
{
// Owner-thread admission and publication mailbox; one detached, value-owned
// worker advances bounded candidate quanta. Closing a view does not own/cancel
// these jobs. Closing the service requests cancellation without waiting on I/O.
class RecoveryRestoreQueue final
{
public:
    struct Config final
    {
        juce::File root, scratch;
        ServiceStorageBudget storage;
        Limits store;
        AudioGraphLimits graph;
        // Logical file-mapping reservation, not a promise of resident RAM use.
        size_t jobs = 8, residentMappingBytes = maximumRecoveryAudioBytes;
        juce::String appBuild;
        std::shared_ptr<RecoveryDiagnosticState> diagnostics;
#if SINGLE_EDITOR_RECOVERY_TESTING
        std::function<void(RecoveryAttemptStage, juce::Uuid)> beforeAdvance;
#endif
    };
    struct Request final
    {
        juce::Uuid owner { nullptr };
        SafeHead observedHead, selectedHead;
        RecoverySourceBinding::Request source;
        bool retryQuarantinedOnce = false;
    };
    struct JobStatus final
    {
        juce::Uuid attempt { nullptr }, owner { nullptr };
        SafeHead checkpoint;
        RecoveryAttemptStage stage = RecoveryAttemptStage::queued;
        RecoveryAttemptStage failureStage = RecoveryAttemptStage::queued;
        Status failure, attemptCloseFailure;
        bool waitingForAdmission = false, coreAdmitted = false;
    };
    struct Health final
    {
        std::vector<JobStatus> jobs;
        Status failure;
        size_t residentMappingBytes = 0;
        bool workerRunning = false;
    };
    struct Publication final
    {
        juce::Uuid attempt { nullptr };
        std::shared_ptr<const RecoveredCandidateDocument> core;
        SingleTrackDocument::StatePtr originalCore;
        std::shared_ptr<WaveformModel::PeakCache> fullPeaks;
    };
    struct DiagnosticObservation final
    {
        std::optional<JobStatus> selected;
        juce::Uuid launchOperation { nullptr };
        Status schedulingFailure;
        size_t jobs = 0;
        std::optional<size_t> residentMappingBytes;
        bool workerRunning = false;
    };
    explicit RecoveryRestoreQueue(Config);
    ~RecoveryRestoreQueue();
    Status start(Request, juce::Uuid& attempt);
    Status cancel(const juce::Uuid&);
    Status retry(const juce::Uuid&, bool explicitRetryOnce, juce::Uuid& newAttempt);
    Status forget(const juce::Uuid&); // Finished UI entry only; never storage deletion.
    // The worker pauses at CoreReady until the actual UI playlist admission
    // succeeds or fails. No automatic selection, autoplay or loadFile shortcut.
    Status acknowledgeCore(const juce::Uuid&, bool admitted);
    bool needsPoll() const;
    Health health() const;
    // Explicit, nonblocking copy; no candidate, reader, lease or callback escapes.
    bool tryDiagnosticObservation(const juce::Uuid& selected, DiagnosticObservation&) const;
    std::vector<Publication> takePublications();
    void close();
private:
    struct State;
    static Status admit(const std::shared_ptr<State>&, Request, juce::Uuid&);
    static void launch(const std::shared_ptr<State>&);
    static void run(const std::shared_ptr<State>&);
    std::shared_ptr<State> state;
};
}
