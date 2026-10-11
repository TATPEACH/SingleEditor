#include "Audio/AmbisonicFieldAnalyzer.h"

#include <cmath>

namespace singleeditor
{
void AmbisonicFieldAnalyzer::prepare(double sampleRate) noexcept
{
    currentSampleRate.store(sampleRate > 0.0 ? sampleRate : 48000.0,
                            std::memory_order_relaxed);
    requestReset();
}

void AmbisonicFieldAnalyzer::requestReset() noexcept
{
    resetRequested.store(true, std::memory_order_release);
}

void AmbisonicFieldAnalyzer::clear() noexcept
{
    currentOrder = 0;
    currentChannelCount = 0;
    smoothingInitialised = false;
    smoothedCovariance.fill(0.0f);
    publish(false, 0, 0, {});
}

void AmbisonicFieldAnalyzer::process(const juce::AudioBuffer<float>& source,
                                     int startSample,
                                     int numSamples,
                                     const ChannelLayoutState& sourceLayout,
                                     AmbisonicOrientation orientation,
                                     const ChannelPlaybackState& playbackState) noexcept
{
    const auto expectedChannels = (sourceLayout.ambisonicOrder + 1)
                                * (sourceLayout.ambisonicOrder + 1);
    if (!sourceLayout.isAmbisonic
        || !juce::isPositiveAndBelow(sourceLayout.ambisonicOrder, 4)
        || sourceLayout.sourceChannelCount != expectedChannels
        || source.getNumChannels() < expectedChannels)
    {
        clear();
        return;
    }

    const auto safeStart = juce::jlimit(0, source.getNumSamples(), startSample);
    const auto safeSamples = juce::jlimit(0, source.getNumSamples() - safeStart, numSamples);
    if (safeSamples <= 0)
        return;

    if (resetRequested.exchange(false, std::memory_order_acq_rel)
        || currentOrder != sourceLayout.ambisonicOrder
        || currentChannelCount != expectedChannels)
    {
        currentOrder = sourceLayout.ambisonicOrder;
        currentChannelCount = expectedChannels;
        smoothingInitialised = false;
        smoothedCovariance.fill(0.0f);
    }

    constexpr int maximumSamplesPerBlock = 64;
    const auto stride = juce::jmax(1, safeSamples / maximumSamplesPerBlock);
    std::array<bool, AmbisonicFieldSnapshot::maxChannels> sourceAudible {};
    for (auto channel = 0; channel < currentChannelCount; ++channel)
    {
        sourceAudible[static_cast<std::size_t>(channel)] =
            ChannelRouter::sourceChannelIsAudible(channel, playbackState);
    }
    std::array<double, AmbisonicFieldSnapshot::maxChannels
                           * AmbisonicFieldSnapshot::maxChannels> blockCovariance {};
    auto sampledFrames = 0;
    for (int sample = 0; sample < safeSamples; sample += stride)
    {
        for (int row = 0; row < currentChannelCount; ++row)
        {
            const auto rowValue = sourceAudible[static_cast<std::size_t>(row)]
                                    ? source.getSample(row, safeStart + sample)
                                    : 0.0f;
            for (int column = 0; column < currentChannelCount; ++column)
            {
                const auto columnValue = sourceAudible[static_cast<std::size_t>(column)]
                                           ? source.getSample(column, safeStart + sample)
                                           : 0.0f;
                blockCovariance[static_cast<std::size_t>(row * AmbisonicFieldSnapshot::maxChannels
                                                          + column)]
                    += static_cast<double>(rowValue)
                     * columnValue;
            }
        }
        ++sampledFrames;
    }

    if (sampledFrames <= 0)
        return;

    const auto blockDuration = static_cast<double>(safeSamples)
                             / currentSampleRate.load(std::memory_order_relaxed);
    const auto smoothing = smoothingInitialised
                         ? static_cast<float>(1.0 - std::exp(-blockDuration / 0.12))
                         : 1.0f;
    for (int row = 0; row < currentChannelCount; ++row)
    {
        for (int column = 0; column < currentChannelCount; ++column)
        {
            const auto index = static_cast<std::size_t>(row * AmbisonicFieldSnapshot::maxChannels
                                                        + column);
            const auto blockValue = static_cast<float>(blockCovariance[index] / sampledFrames);
            smoothedCovariance[index] += smoothing * (blockValue - smoothedCovariance[index]);
        }
    }
    smoothingInitialised = true;
    publish(true,
            currentOrder,
            currentChannelCount,
            AmbisonicMonitorDecoder::canonicalizeOrientation(orientation));
}

AmbisonicFieldSnapshot AmbisonicFieldAnalyzer::getSnapshot() const noexcept
{
    AmbisonicFieldSnapshot snapshot;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto sequenceBefore = publishedSequence.load(std::memory_order_acquire);
        if ((sequenceBefore & 1u) != 0u)
            continue;

        snapshot.valid = publishedValid.load(std::memory_order_relaxed);
        snapshot.order = publishedOrder.load(std::memory_order_relaxed);
        snapshot.channelCount = publishedChannelCount.load(std::memory_order_relaxed);
        snapshot.orientation = {
            publishedYaw.load(std::memory_order_relaxed),
            publishedPitch.load(std::memory_order_relaxed),
            publishedRoll.load(std::memory_order_relaxed)
        };
        for (std::size_t index = 0; index < snapshot.covariance.size(); ++index)
            snapshot.covariance[index] = publishedCovariance[index].load(std::memory_order_relaxed);

        const auto sequenceAfter = publishedSequence.load(std::memory_order_acquire);
        if (sequenceBefore == sequenceAfter && (sequenceAfter & 1u) == 0u)
        {
            snapshot.sequence = sequenceAfter;
            return snapshot;
        }
    }

    snapshot.valid = false;
    return snapshot;
}

float AmbisonicFieldAnalyzer::energyAt(const AmbisonicFieldSnapshot& snapshot,
                                       float azimuthDegrees,
                                       float elevationDegrees) noexcept
{
    if (!snapshot.valid
        || !juce::isPositiveAndBelow(snapshot.order, 4)
        || snapshot.channelCount != (snapshot.order + 1) * (snapshot.order + 1))
    {
        return 0.0f;
    }

    std::array<float, AmbisonicFieldSnapshot::maxChannels> basis {};
    AmbisonicMonitorDecoder::evaluateSn3dBasis(snapshot.order,
                                                azimuthDegrees,
                                                elevationDegrees,
                                                snapshot.orientation,
                                                basis);
    auto energy = 0.0;
    for (int row = 0; row < snapshot.channelCount; ++row)
    {
        for (int column = 0; column < snapshot.channelCount; ++column)
        {
            const auto index = static_cast<std::size_t>(row * AmbisonicFieldSnapshot::maxChannels
                                                        + column);
            energy += static_cast<double>(basis[static_cast<std::size_t>(row)])
                    * snapshot.covariance[index]
                    * basis[static_cast<std::size_t>(column)];
        }
    }
    return static_cast<float>(juce::jmax(0.0, energy));
}

void AmbisonicFieldAnalyzer::publish(bool valid,
                                     int order,
                                     int channelCount,
                                     AmbisonicOrientation orientation) noexcept
{
    publishedSequence.fetch_add(1, std::memory_order_acq_rel);
    publishedValid.store(valid, std::memory_order_relaxed);
    publishedOrder.store(order, std::memory_order_relaxed);
    publishedChannelCount.store(channelCount, std::memory_order_relaxed);
    publishedYaw.store(orientation.yawDegrees, std::memory_order_relaxed);
    publishedPitch.store(orientation.pitchDegrees, std::memory_order_relaxed);
    publishedRoll.store(orientation.rollDegrees, std::memory_order_relaxed);
    for (std::size_t index = 0; index < smoothedCovariance.size(); ++index)
        publishedCovariance[index].store(smoothedCovariance[index], std::memory_order_relaxed);
    publishedSequence.fetch_add(1, std::memory_order_release);
}
} // namespace singleeditor
