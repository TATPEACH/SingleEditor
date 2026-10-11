#pragma once

#include "RecoveryAudioGraph.h"
#include "RecoverySourceIdentity.h"

namespace singleeditor::recovery
{
// Semantically checked metadata and audio references at a checkpoint or replayed
// mutation head, never a playable Document
// snapshot. It owns no reader/samples and cannot be passed to recovered adoption.
class PreparedRecoveryCheckpoint final
{
public:
    const SafeHead& getHead() const noexcept { return head; }
    const media::IdentitySnapshot& getIdentity() const noexcept { return identity; }
    const std::vector<SingleTrackDocument::StatePtr>& getStates() const noexcept { return states; }
    size_t getUndoCount() const noexcept { return undoCount; }
    size_t getRedoCount() const noexcept { return redoCount; }
    const juce::String& getAudioJson() const noexcept { return audioJson; }
    const AudioGraphDescriptor& getAudioGraph() const noexcept { return audio; }
    const std::optional<RecoverySourceEvidence>& getSourceEvidence() const noexcept { return sourceEvidence; }
private:
    friend class RecoveryCheckpointCodec;
    friend class RecoveryCheckpointWriter;
    SafeHead head;
    media::IdentitySnapshot identity;
    std::vector<SingleTrackDocument::StatePtr> states;
    size_t undoCount = 0, redoCount = 0;
    juce::String audioJson;
    AudioGraphDescriptor audio;
    std::optional<RecoverySourceEvidence> sourceEvidence;
};

// Pure canonical delta codec for explicitly described Document transitions.
// No source/audio I/O, Document mutation, CAS publication or active attachment.
// Encoding verifies the replayed delta against the actual committed snapshot;
// unsupported edits require a checkpoint, never inferred processor semantics.
class RecoveryMutationCodec final
{
public:
    static Status encode(const SingleTrackDocument::CommittedStateSnapshot& base,
                         const SingleTrackDocument::CommittedStateSnapshot& committed,
                         juce::String&, RecoveryAudioGraph::Cancel = {});
    static Status replay(const juce::String&, const SingleTrackDocument::CommittedStateSnapshot& base,
                         SingleTrackDocument::CommittedStateSnapshot&, RecoveryAudioGraph::Cancel = {});
};

// Internal checkpoint schema, not a processor operation replayer or an attach
// API. The snapshot's immutable audio and retained Document states are frozen
// together. Runtime monitor/HRIR/device/view objects are deliberately excluded.
class RecoveryCheckpointCodec final
{
public:
    static Status encode(const SingleTrackDocument::CommittedStateSnapshot&,
                         const std::set<juce::String>& knownAssets, CommitPacket&,
                         AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {}, AudioGraphReuseCache* = nullptr);
    // Schema-2 checkpoint binds previously captured source evidence into the
    // same hashed record. No new probe or source path read occurs here. Schema-1
    // checkpoints remain readable but carry no proof for source-base replay.
    static Status encodeWithSourceEvidence(const SingleTrackDocument::CommittedStateSnapshot&,
                         const RecoverySourceEvidence&, const std::set<juce::String>& knownAssets, CommitPacket&,
                         AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {}, AudioGraphReuseCache* = nullptr);
    // No file I/O/audio allocation. Requires a Store manifest commit; validates
    // full metadata/history and exact declared audio dependency closure before
    // any optional sample loading. Prior output survives failure/cancellation.
    static Status prepare(const ManifestCommit&, PreparedRecoveryCheckpoint&,
                          AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {});
    // Record-only mixed-chain replay from the nearest complete checkpoint to an
    // explicitly selected on-chain safe head. No PCM/source reads or ownership.
    static Status prepareThrough(const ManifestView&, const SafeHead& selected, PreparedRecoveryCheckpoint&,
                                 AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {});
    static Status prepareMutation(const ManifestCommit&, const PreparedRecoveryCheckpoint& base,
                                  PreparedRecoveryCheckpoint&, AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {});
    // Existing worker only: validate the complete retained replay, publish a
    // checkpoint cut, then run one bounded durable cleanup quantum.
    static Status compact(RecoveryStore&, const SafeHead&, OrphanCollectionReport&,
                          AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {});
    // Caller obtains this commit from RecoveryStore::inspect's verified view.
    // No source is opened or live Document changed. All states, not just the
    // current state, must validate before the isolated snapshot is published.
    static Status decode(const VerifiedCommit&, const RecoveryStore&,
                         SingleTrackDocument::CommittedStateSnapshot&,
                         AudioGraphLimits = {}, RecoveryAudioGraph::Cancel = {});
private:
    friend class RecoveryCheckpointWriter;
    static Status applyMutationMetadata(const juce::String&, const PreparedRecoveryCheckpoint&,
                                        PreparedRecoveryCheckpoint&, AudioGraphLimits, RecoveryAudioGraph::Cancel);
};

// Worker-confined publication frontier: metadata-only prepared states plus weak
// audio graph identities. First/unsupported edits and every 32 successful deltas
// produce a checkpoint. Failed preparation/publication never advances the base.
// This does not compact/delete old Store records or provide native lifecycle UI.
class RecoveryCheckpointWriter final
{
public:
    RecoveryCheckpointWriter(juce::File root, juce::Uuid owner, AudioGraphLimits limits = {}, size_t maximumDeltas = 32)
        : audioReuse(std::move(root), owner), bounds(limits), deltaLimit(maximumDeltas > 32 ? 32 : maximumDeltas) {}
    Status prepare(const SingleTrackDocument::CommittedStateSnapshot&, CommitPacket&, RecoveryAudioGraph::Cancel = {});
    Status published(const std::shared_ptr<const PublishedRecoveryCommit>&);
    const std::optional<PreparedRecoveryCheckpoint>& publishedMetadata() const noexcept { return committed; }
    CapturedAudioGraph::Work lastCaptureWork() const noexcept { return audioReuse.lastCaptureWork(); }
private:
    AudioGraphReuseCache audioReuse;
    AudioGraphLimits bounds;
    size_t deltaLimit = 32;
    std::optional<PreparedRecoveryCheckpoint> committed, pending;
    juce::String expectedState, expectedOperation, expectedPlan;
    int expectedSchema = 0;
    size_t deltaDepth = 0;
    bool sourceProbeAttempted = false;
    std::optional<RecoverySourceEvidence> admittedSourceEvidence;
};
} // namespace singleeditor::recovery
