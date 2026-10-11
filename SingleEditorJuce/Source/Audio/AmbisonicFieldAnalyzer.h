#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Audio/AmbisonicMonitorDecoder.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
struct AmbisonicFieldSnapshot
{
    static constexpr int maxChannels = 16;

    bool valid = false;
    int order = 0;
    int channelCount = 0;
    AmbisonicOrientation orientation;
    std::array<float, maxChannels * maxChannels> covariance {};
    std::uint64_t sequence = 0;
};

class AmbisonicFieldAnalyzer final
{
public:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void observeLifetime(OptionalLifecycleToken token) noexcept { lifecycleToken = std::move(token); }
    ~AmbisonicFieldAnalyzer() { lifecycleToken.release(); }
#endif
    void prepare(double sampleRate) noexcept;
    void requestReset() noexcept;
    void clear() noexcept;

    void process(const juce::AudioBuffer<float>& source,
                 int startSample,
                 int numSamples,
                 const ChannelLayoutState& sourceLayout,
                 AmbisonicOrientation orientation,
                 const ChannelPlaybackState& playbackState) noexcept;

    AmbisonicFieldSnapshot getSnapshot() const noexcept;

    static float energyAt(const AmbisonicFieldSnapshot& snapshot,
                          float azimuthDegrees,
                          float elevationDegrees) noexcept;

private:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleToken lifecycleToken;
#endif
    void publish(bool valid,
                 int order,
                 int channelCount,
                 AmbisonicOrientation orientation) noexcept;

    std::atomic<double> currentSampleRate { 48000.0 };
    int currentOrder = 0;
    int currentChannelCount = 0;
    bool smoothingInitialised = false;
    std::array<float, AmbisonicFieldSnapshot::maxChannels
                          * AmbisonicFieldSnapshot::maxChannels> smoothedCovariance {};
    std::atomic<bool> resetRequested { false };
    std::atomic<std::uint64_t> publishedSequence { 0 };
    std::atomic<bool> publishedValid { false };
    std::atomic<int> publishedOrder { 0 };
    std::atomic<int> publishedChannelCount { 0 };
    std::atomic<float> publishedYaw { 0.0f };
    std::atomic<float> publishedPitch { 0.0f };
    std::atomic<float> publishedRoll { 0.0f };
    std::array<std::atomic<float>, AmbisonicFieldSnapshot::maxChannels
                                        * AmbisonicFieldSnapshot::maxChannels> publishedCovariance {};
};
} // namespace singleeditor
