#pragma once

#include <array>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Audio/ChannelLayout.h"

namespace singleeditor
{
struct OutputMeterSnapshot
{
    static constexpr int maxChannels = 12;

    int channelCount = 0;
    std::array<SpeakerRole, maxChannels> roles {};
    std::array<float, maxChannels> truePeakLinear {};
    std::array<float, maxChannels> rmsLinear {};
    std::array<float, maxChannels> kWeightedContributionDb {};
    float momentaryLufs = -120.0f;
};

class OutputMeterAnalyzer final
{
public:
    void prepare(double sampleRate,
                 int channelCount,
                 const std::array<SpeakerRole, OutputMeterSnapshot::maxChannels>& roles);
    void reset() noexcept;

    OutputMeterSnapshot process(const juce::AudioBuffer<float>& buffer,
                                int startSample,
                                int numSamples) noexcept;

    static double loudnessWeightForRole(SpeakerRole role) noexcept;

private:
    struct LoudnessHistoryEntry
    {
        std::array<double, OutputMeterSnapshot::maxChannels> energyByChannel {};
        int sampleCount = 0;
    };

    static juce::IIRCoefficients makeKWeightShelf(double sampleRate) noexcept;
    static juce::IIRCoefficients makeKWeightHighPass(double sampleRate) noexcept;
    static float energyToLoudnessDb(double energy) noexcept;

    void appendLoudnessBlock(
        const std::array<double, OutputMeterSnapshot::maxChannels>& energyByChannel,
        int sampleCount) noexcept;

    std::array<SpeakerRole, OutputMeterSnapshot::maxChannels> outputRoles {};
    std::array<float, OutputMeterSnapshot::maxChannels> previousSamples {};
    std::array<juce::IIRFilter, OutputMeterSnapshot::maxChannels> shelfFilters;
    std::array<juce::IIRFilter, OutputMeterSnapshot::maxChannels> highPassFilters;
    std::array<LoudnessHistoryEntry, 512> loudnessHistory {};
    std::array<double, OutputMeterSnapshot::maxChannels> rollingEnergyByChannel {};
    int channelCount = 0;
    int historyWriteIndex = 0;
    int historyReadIndex = 0;
    int historyCount = 0;
    int rollingSampleCount = 0;
    int momentaryWindowSamples = 1;
};
} // namespace singleeditor
