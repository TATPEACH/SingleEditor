#pragma once

#include "Document/SingleTrackDocument.h"
#include "RecoveryStore.h"
#include <set>

namespace singleeditor::recovery
{
// Audio part of a checkpoint, not a complete Document/Undo metadata codec.
// State bindings are current, then Undo oldest-to-newest, then Redo in its
// existing stack order. No shared live reader is touched. Recovery backings use
// independent verified CAS reads; ordinary sources require a guarded witness.
struct CapturedAudioGraph final
{
    struct Work final
    {
        size_t reusedSequences = 0, sourceDecodersOpened = 0, casBlocksRead = 0, sampleBytesReadOrCopied = 0;
    };
    juce::String json;
    std::vector<AudioBlock> newAudio;
    std::vector<juce::String> existingAudio;
    std::vector<StreamedAudioBlock> streamedAudio;
    Work work;
};

struct AudioGraphLimits final
{
    size_t states = 32;
    size_t chunks = maximumCommitAssets;
    // Maximum admitted block. Capture keeps 2 MiB blocks for small graphs.
    size_t chunkSampleBytes = 8 * 1024 * 1024 - 1024;
    size_t materialisedBytes = maximumRecoveryAudioBytes;
    size_t jsonBytes = 256 * 1024;
    size_t assetBytes = 8 * 1024 * 1024;
    // Capture hashes one block at a time and retains its guarded read source,
    // rather than accumulating all newly captured PCM until Store append.
    bool streamNewAudio = false;
    size_t eagerBytes = 256 * 1024 * 1024;
};

// Validated schema-2 allocation/read plan. Chunk starts are derived from exact
// frame counts; no reader has to open later assets to locate the first prefix.
struct AudioGraphDescriptor final
{
    struct Chunk final { juce::String id; int64_t start = 0, frames = 0; };
    struct Sequence final
    {
        double sampleRate = 0;
        int channels = 0;
        int64_t frames = 0;
        std::vector<Chunk> chunks;
    };
    std::vector<size_t> states;
    std::vector<Sequence> sequences;
};

// Worker-confined, one destination owner. Only a matching Store-issued receipt
// promotes staged weak backing identities/graph references. No retained PCM or
// reader ownership; replacing/destroying a backing cannot make its address a hit.
// This caches the last published dependency graph, never an unbounded history.
class AudioGraphReuseCache final
{
public:
    AudioGraphReuseCache(juce::File rootDirectory, juce::Uuid ownerId) : root(std::move(rootDirectory)), owner(ownerId) {}
    Status acknowledge(const std::shared_ptr<const PublishedRecoveryCommit>&);
    CapturedAudioGraph::Work lastCaptureWork() const noexcept { return work; }
private:
    friend class RecoveryAudioGraph;
    friend class RecoveryCheckpointCodec;
    friend class RecoveryCheckpointWriter;
    struct Entry final
    {
        std::weak_ptr<const void> backing;
        SingleTrackDocument::State::AudioBackingStore kind;
        int64_t audioRevision = 0;
        AudioGraphDescriptor::Sequence sequence;
    };
    static std::shared_ptr<const void> backingOf(const SingleTrackDocument::StatePtr&);
    const AudioGraphDescriptor::Sequence* lookup(const SingleTrackDocument::StatePtr&) const;
    Status stage(const SingleTrackDocument::CommittedStateSnapshot&, const CapturedAudioGraph&,
                 const juce::String& packetState, AudioGraphLimits);
    void discardPending() { pending.clear(); pendingState.clear(); pendingIdentity = {}; }
    juce::File root;
    juce::Uuid owner;
    std::vector<Entry> committed, pending;
    SafeHead committedHead;
    juce::String pendingState;
    media::DocumentIdentity pendingIdentity;
    CapturedAudioGraph::Work work;
};

class RecoveryAudioGraph final
{
public:
    using Cancel = std::function<bool()>;
    // Worker-only, bounded. Captured buffers must be immutable (Document's
    // committed copy-on-write states). Private recovery readers supply immutable
    // CAS provenance; all referenced bytes/dimensions are verified independently,
    // including unprepared history. Ordinary mapped/streaming source readers
    // require the physical witness from guarded reader admission, then use a
    // separate identity-checked decoder under a short source read lease. No
    // shared playback cursor, whole-buffer materialization or path-only trust.
    // Known IDs are merely dedupe hints: RecoveryStore re-verifies each on append.
    // Optional owner-scoped reuse skips PCM capture only for receipt-certified
    // immutable backings. Ordinary source versions still receive a read lease;
    // destination CAS integrity is still checked by the eventual Store append.
    static Status capture(const SingleTrackDocument::CommittedStateSnapshot&,
                          const std::set<juce::String>& knownAssets,
                          CapturedAudioGraph&, AudioGraphLimits = {}, Cancel = {}, const AudioGraphReuseCache* = nullptr);
    // No file I/O/sample allocation. Validates all dimensions, references and
    // declared chunk lengths before either full or progressive restoration.
    static Status describe(const juce::String&, AudioGraphDescriptor&, AudioGraphLimits = {});
    // Produces isolated buffers only after every chunk passes store verification,
    // dimensions, total length and aggregate allocation checks. Never attaches
    // to the live document. Output remains unchanged on failure/cancellation.
    static Status materialise(const juce::String&, const RecoveryStore&,
                              std::vector<std::shared_ptr<juce::AudioBuffer<float>>>&,
                              AudioGraphLimits = {}, Cancel = {});
};
} // namespace singleeditor::recovery
