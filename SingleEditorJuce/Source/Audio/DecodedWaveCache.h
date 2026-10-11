#pragma once
#include <juce_cryptography/juce_cryptography.h>
#include <optional>
#include "Audio/CheckedStreamHash.h"

namespace singleeditor::media
{
// Optional-worker-only cache. File names locate entries; a fixed-size complete
// seal binds the source stamp, producer stamp, policy, dimensions and body hash.
class DecodedWaveCache final
{
public:
    static constexpr int schemaVersion = 1; // SEWAVC1 complete-seal format
    struct Context final
    {
        juce::File source, producer;
        juce::String policy;
        juce::String sourceStamp, producerStamp;
    };
    struct WaveFacts final
    {
        uint64_t dataStart = 0, dataBytes = 0;
        uint32_t rate = 0, channels = 0, bytesPerFrame = 0;
        uint64_t frames = 0;
    };
    static constexpr int64_t maximumBytes = 4LL * 1024 * 1024 * 1024;
    // Fixed-size facts from the actual optional-worker lookup. Observers never
    // repeat a lookup merely to explain a miss.
    enum class LookupReason
    {
        notObserved, reused, contextUnavailable, contextChanged, workEnded,
        incompleteMarker, entryUnavailable, entrySizeRejected, sealUnavailable,
        sealInvalid, sourceChanged, producerChanged, policyChanged, sizeChanged,
        waveIncomplete, dimensionsChanged, bodyReadIncomplete, bodyChanged,
        readerUnavailable, readerRejected
    };
    static const char* lookupReasonName(LookupReason) noexcept;
    static std::optional<Context> capture(const juce::File& source, const juce::File& producer,
                                          const juce::String& policy);
    static std::optional<WaveFacts> inspectComplete(const juce::File& wave, const WorkCheckpoint& = {});
    static bool reusable(const Context&, const juce::File& wave, const WorkCheckpoint& = {}, LookupReason* = nullptr);
    // Seal an already completed, exclusively owned wave; progressive callers
    // must keep their in-progress marker until this returns success.
    static bool seal(const Context&, const juce::File& wave, const WorkCheckpoint& = {});
    static juce::File sealFile(const juce::File& wave);
};
}
