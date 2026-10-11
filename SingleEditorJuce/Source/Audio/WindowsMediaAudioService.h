#pragma once

#include <juce_core/juce_core.h>
#include "Audio/OptionalMediaOperation.h"

namespace singleeditor
{
class WindowsMediaAudioService final
{
public:
    static bool isSupportedVideoExtension(const juce::String& extension) noexcept;
    static bool isRuntimeLoadedForTests() noexcept;

    static bool encodeWaveFileToMp3(const juce::File& sourceWaveFile,
                                    const juce::File& targetMp3File,
                                    int bitrateKbps,
                                    juce::String& errorMessage);

    static juce::File getOrCreateVideoAudioCache(const juce::File& videoFile,
                                                 juce::String& errorMessage,
                                                 const media::OptionalMediaOperation::Ptr& operation = {});
};
} // namespace singleeditor
