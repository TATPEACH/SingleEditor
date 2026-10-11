#pragma once

#include "RecoveryAudioGraph.h"

namespace singleeditor::recovery
{
// Explicit worker-driven restoration of one sequence. No thread, timer, device,
// source-path reopen, or live Document attachment. The owner must retain this
// controller until readers retire off the audio callback.
class ProgressiveRecoveryAudio final
{
public:
    ~ProgressiveRecoveryAudio();
    // Explicit service worker setup: create at most the final directory, with
    // existing ancestors pinned. The reader factory itself never creates it.
    static Status prepareScratchDirectory(const juce::File&);
    // Worker-only. Validates the complete graph before creating a private,
    // delete-on-close scratch mapping in an existing, non-reparse directory.
    // No CAS chunks are read here; initially no samples are playable.
    static Status create(const juce::String& graphJson, size_t stateIndex,
                         const juce::File& scratchDirectory,
                         std::unique_ptr<ProgressiveRecoveryAudio>&,
        AudioGraphLimits = {}, const RecoveryStore* durableSource = nullptr,
        std::shared_ptr<const void> allocationLease = {});

    struct DurableReference final
    {
        RecoveryStore store;
        AudioGraphDescriptor::Sequence sequence;
        std::shared_ptr<RecoveryOwnerLease> lease;
    };
    // Worker-only immutable CAS provenance from our private Reader type. This
    // is a dependency descriptor, not proof every asset still exists. Capture
    // and destination publication must independently verify referenced assets.
    // It never reads the mutable progressive mapping or historical source path.
    static std::shared_ptr<const DurableReference> getDurableReference(const std::shared_ptr<juce::AudioFormatReader>&);

    // One serialized owner worker calls this, one verified chunk per call.
    // Cancellation/failure is terminal for this candidate. The already prepared
    // prefix remains readable; retry requires an explicitly new candidate.
    Status loadNext(const RecoveryStore&, RecoveryAudioGraph::Cancel = {});
    // Serialized owner-worker retirement. Preserves published samples and tells
    // consumers no later prefix will arrive; does not delete durable CAS data.
    void cancel();
    std::shared_ptr<juce::AudioFormatReader> getReader() const noexcept;
    SingleTrackDocument::PlaybackAvailability getAvailability() const noexcept;
    int64_t readyFrames() const noexcept;
    bool isComplete() const noexcept;
    // Bind validated checkpoint metadata to this isolated reader without making
    // a Document or claiming readiness. A later Document factory owns adoption.
    Status bindMetadata(const SingleTrackDocument::State&, SingleTrackDocument::StatePtr&) const;

private:
    struct Storage;
    class Reader;
    ProgressiveRecoveryAudio(std::shared_ptr<Storage>, AudioGraphDescriptor::Sequence, size_t assetBytes);
    std::shared_ptr<Storage> storage;
    std::shared_ptr<juce::AudioFormatReader> reader;
    AudioGraphDescriptor::Sequence sequence;
    size_t maximumAssetBytes = 0, nextChunk = 0;
    Status terminalStatus;
};
} // namespace singleeditor::recovery
