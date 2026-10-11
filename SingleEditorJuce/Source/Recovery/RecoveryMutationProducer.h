#pragma once

#include "AsyncRecoveryWriter.h"
#include "RecoveryCheckpointCodec.h"
#include "DocumentRecoveryCapture.h"

namespace singleeditor::recovery
{
// Explicit post-Core consumer of a single Document/source epoch. Construction
// attaches an observer only: no owner, file access, thread, watcher or timer.
// All public methods and Document mutations are serialized on the owning thread;
// only immutable snapshots go to the worker. Must die before its Document.
// This bounded mixed-record producer is not the complete recovery journal or
// shipping lifecycle service. The host must surface health, drain on close and
// fence saved-version durability and live CAS dependencies before tombstoning
// any retired owner or enabling automatic product protection.
class RecoveryMutationProducer final
{
public:
    struct Config final
    {
        juce::File root;
        Limits store;
        AudioGraphLimits graph;
        AsyncRecoveryWriter::Budget queue;
        // Owned worker-only storage preparation, before checkpoint I/O. It is
        // never called by construction/clean admission; failure retains the
        // accepted front packet for explicit retry. Do not capture UI objects.
        std::function<Status()> prepareStorage;
        // Internal large-source Pilot; ordinary captures retain queue.maximumAge.
        // The pending UI and bounded-close choices remain active throughout.
        std::chrono::milliseconds largeSourceMaximumAge { std::chrono::minutes(10) };
    };
    enum class Phase { idle, pending, protectedState, degraded, retired };
    enum class Admission { observeClean, checkpointDirty };
    struct Health final
    {
        Phase phase = Phase::idle;
        juce::Uuid owner { nullptr };
        media::DocumentIdentity observedIdentity;
        AsyncRecoveryWriter::Progress writer;
        Status failure;
        bool checkpointRequired = false;
        juce::String retirementReason;
        // Source-save completion is not necessarily a clean current generation.
        // The host must re-admit this current dirty state under a new owner before
        // claiming protection of the replacement source epoch.
        std::optional<media::IdentitySnapshot> replacementCheckpoint;
    };

    // Caller has already crossed CoreReady. Default observation is inert and
    // clean-only. Explicit dirty admission captures actual current/history once
    // and queues a full first checkpoint without changing Document generation.
    // Success means accepted, not yet durable; callers must inspect health.
    static Status attachPostCore(SingleTrackDocument&, Config,
                                  std::unique_ptr<RecoveryMutationProducer>&, Admission = Admission::observeClean);
    ~RecoveryMutationProducer();
    Health health() const;
    Status retry(bool collectOrphans = false);
    // Detach without deleting any journal/audio. Accepted work owns its captures
    // and may finish after this object dies; no worker callback targets the host.
    void close();
    bool drain(std::chrono::milliseconds);

private:
    friend class RecoveryProtectionSession;
    friend class RecoveryOwnerLifecycle;
    RecoveryMutationProducer(SingleTrackDocument&, Config, media::IdentitySnapshot, bool observe = true);
    void consume(DocumentRecoveryCapture::Capture);
    Status enqueue(SingleTrackDocument::CommittedStateSnapshot);
    void rejectGap(Status);
    void retire(const char* reason);
    Status estimate(const SingleTrackDocument::CommittedStateSnapshot&, size_t&,
                    std::vector<AsyncRecoveryWriter::RetainedBacking>* = nullptr, size_t* logicalAudioBytes = nullptr) const;
    Status projectedRetained(const SingleTrackDocument::CommittedStateSnapshot&, size_t&) const;
    Config config;
    media::IdentitySnapshot observed;
    std::unique_ptr<DocumentRecoveryCapture> capture;
    juce::Uuid owner { nullptr };
    std::unique_ptr<AsyncRecoveryWriter> writer;
    std::shared_ptr<RecoveryCheckpointWriter> checkpointWriter;
    Status failure;
    bool checkpointRequired = false;
    juce::String retirementReason;
    std::optional<media::IdentitySnapshot> replacementCheckpoint;
};
}
