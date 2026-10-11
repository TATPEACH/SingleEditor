#pragma once

#include "Audio/AudioEngine.h"
#include <cmath>

namespace singleeditor
{
// UI-only ballistics and resettable hold. Never changes the audio analyzer.
struct OutputMeterDisplayState
{
    OutputMeterSnapshot snapshot;
    std::array<float, OutputMeterSnapshot::maxChannels> levels {}, trails {};
    float peakSinceResetDb = -120.0f;

    static float db(float gain) noexcept
    {
        return std::isfinite(gain) ? juce::Decibels::gainToDecibels(juce::jmax(0.0f, gain), -120.0f) : -120.0f;
    }
    static float maximum(const OutputMeterSnapshot& source, bool truePeak) noexcept
    {
        float result = -120.0f;
        for (int ch = 0; ch < juce::jlimit(0, OutputMeterSnapshot::maxChannels, source.channelCount); ++ch)
            result = juce::jmax(result, db(truePeak ? source.truePeakLinear[ch] : source.rmsLinear[ch]));
        return result;
    }
    static float normalise(float value, AudioEngine::MeterMode mode) noexcept
    {
        const float floor = mode == AudioEngine::MeterMode::lufs ? -54.0f : -60.0f;
        const float ceiling = mode == AudioEngine::MeterMode::truePeak ? 6.0f : 0.0f;
        return std::isfinite(value) ? juce::jlimit(0.0f, 1.0f, (value - floor) / (ceiling - floor)) : 0.0f;
    }
    void resetHold() noexcept { peakSinceResetDb = -120.0f; trails.fill(0.0f); }
    void resetBallistics() noexcept { levels.fill(0.0f); trails.fill(0.0f); }

    bool advance(OutputMeterSnapshot next, AudioEngine::MeterMode mode) noexcept
    {
        next.channelCount = juce::jlimit(0, OutputMeterSnapshot::maxChannels, next.channelCount);
        bool changed = next.channelCount != snapshot.channelCount || next.roles != snapshot.roles;
        changed |= next.truePeakLinear != snapshot.truePeakLinear || next.rmsLinear != snapshot.rmsLinear
            || next.kWeightedContributionDb != snapshot.kWeightedContributionDb || next.momentaryLufs != snapshot.momentaryLufs;
        const auto nextPeak = juce::jmax(peakSinceResetDb, maximum(next, true));
        changed |= nextPeak != peakSinceResetDb;
        peakSinceResetDb = nextPeak;
        snapshot = next;
        for (int ch = 0; ch < OutputMeterSnapshot::maxChannels; ++ch)
        {
            const auto value = ch >= snapshot.channelCount ? -120.0f
                : mode == AudioEngine::MeterMode::lufs ? snapshot.kWeightedContributionDb[ch]
                : db(mode == AudioEngine::MeterMode::truePeak ? snapshot.truePeakLinear[ch] : snapshot.rmsLinear[ch]);
            const auto target = normalise(value, mode);
            auto nextLevel = levels[ch] + (target - levels[ch]) * (target > levels[ch] ? 0.62f : 0.16f);
            if (std::abs(nextLevel - target) < 0.0005f) nextLevel = target;
            const auto nextTrail = juce::jmax(nextLevel, trails[ch] - 0.014f);
            changed |= std::abs(nextLevel - levels[ch]) > 0.0001f || std::abs(nextTrail - trails[ch]) > 0.0001f;
            levels[ch] = nextLevel;
            trails[ch] = nextTrail;
        }
        return changed;
    }
};
}
