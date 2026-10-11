#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "Audio/AmbisonicMonitorState.h"

namespace singleeditor
{
#if SINGLE_EDITOR_AMBISONIC_RESOURCE_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
namespace ambisonicResources
{
inline std::atomic<int> packReadAttempts { 0 }, preparedBanks { 0 }, workerReads { 0 };
}
#endif
class AmbisonicBinauralFilterBank final
{
public:
    static std::shared_ptr<const AmbisonicBinauralFilterBank> loadFromFile(
        const juce::File& file,
        juce::String& errorMessage);
    static std::shared_ptr<const AmbisonicBinauralFilterBank> loadDefault(
        juce::String& errorMessage);
    static juce::File defaultResourceFile();

    int getSampleRate() const noexcept { return sampleRate; }
    int getTapCount() const noexcept { return tapCount; }
    int getSourceChannelCount(int ambisonicOrder) const noexcept;
    float getSample(int ambisonicOrder, int ear, int sourceChannel, int tap) const noexcept;
    const juce::String& getSourceMd5() const noexcept { return sourceMd5; }
    const juce::String& getResourceIdentity() const noexcept { return resourceIdentity; }

private:
    static std::shared_ptr<const AmbisonicBinauralFilterBank> decode(
        const juce::MemoryBlock& bytes, const juce::String& resourceName, juce::String& errorMessage);
    struct Bank
    {
        int order = 0;
        int sourceChannels = 0;
        std::vector<float> samples;
    };

    const Bank* findBank(int ambisonicOrder) const noexcept;

    int sampleRate = 0;
    int tapCount = 0;
    juce::String sourceMd5;
    juce::String resourceIdentity;
    std::array<Bank, 3> banks;
};
} // namespace singleeditor
