#pragma once

#include <functional>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "Audio/DecodedWaveCache.h"
#include "Audio/OptionalMediaOperation.h"

namespace singleeditor
{
class VgmstreamDecodeService
{
public:
    using DecodeProgressCallback = std::function<void(juce::int64 readyEndSample,
                                                       bool decodeComplete,
                                                       bool decodeFailed)>;

    struct WemOpenStrategy
    {
        enum class Kind
        {
            directWaveReader = 0,
            decodedFallback
        };

        Kind kind = Kind::decodedFallback;
        juce::String detail;
    };

    static juce::File getCliPath();
    static bool isAvailable();
    static juce::File getDecodeCacheDirectory();
    static juce::File getDecodedWaveFile(const juce::File& inputFile);
    static WemOpenStrategy chooseWemOpenStrategy(const juce::File& inputFile);
    static juce::Result ensureDecodedWave(const juce::File& inputFile,
                                          juce::File& decodedWaveFile,
                                          juce::String* detail = nullptr,
                                          media::OptionalMediaOperation::Ptr bankOperation = {});
    static std::unique_ptr<juce::AudioFormatReader> createReaderForDecodedFile(const juce::File& inputFile,
                                                                               juce::StringArray* diagnostics = nullptr,
                                                                               media::OptionalMediaOperation::Ptr bankOperation = {});
    static std::unique_ptr<juce::AudioFormatReader> createProgressiveReaderForDecodedFile(
        const juce::File& inputFile,
        DecodeProgressCallback progressCallback,
        juce::File& decodedWaveFile,
        juce::StringArray* diagnostics = nullptr,
        media::OptionalMediaOperation::Ptr operation = {});

private:
    static bool hasUsableDecodedWave(const media::DecodedWaveCache::Context&, const juce::File& decodedWaveFile,
                                    const media::WorkCheckpoint& = {}, media::DecodedWaveCache::LookupReason* = nullptr);
};
} // namespace singleeditor
