#include "Audio/OutputMeterAnalyzer.h"

#include <cmath>

namespace singleeditor
{
namespace
{
constexpr auto minimumLoudnessDb = -120.0f;
constexpr auto loudnessCalibrationDb = -0.691;
}

void OutputMeterAnalyzer::prepare(
    double sampleRate,
    int newChannelCount,
    const std::array<SpeakerRole, OutputMeterSnapshot::maxChannels>& roles)
{
    channelCount = juce::jlimit(0, OutputMeterSnapshot::maxChannels, newChannelCount);
    outputRoles = roles;
    momentaryWindowSamples = juce::jmax(1, juce::roundToInt(sampleRate * 0.4));

    const auto shelf = makeKWeightShelf(sampleRate);
    const auto highPass = makeKWeightHighPass(sampleRate);
    for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
    {
        shelfFilters[static_cast<std::size_t>(channel)].setCoefficients(shelf);
        highPassFilters[static_cast<std::size_t>(channel)].setCoefficients(highPass);
    }

    reset();
}

void OutputMeterAnalyzer::reset() noexcept
{
    previousSamples.fill(0.0f);
    rollingEnergyByChannel.fill(0.0);
    loudnessHistory = {};
    historyWriteIndex = 0;
    historyReadIndex = 0;
    historyCount = 0;
    rollingSampleCount = 0;
    for (auto& filter : shelfFilters)
        filter.reset();
    for (auto& filter : highPassFilters)
        filter.reset();
}

OutputMeterSnapshot OutputMeterAnalyzer::process(const juce::AudioBuffer<float>& buffer,
                                                 int startSample,
                                                 int numSamples) noexcept
{
    OutputMeterSnapshot snapshot;
    snapshot.channelCount = juce::jmin(channelCount, buffer.getNumChannels());
    snapshot.roles = outputRoles;
    snapshot.kWeightedContributionDb.fill(minimumLoudnessDb);
    if (snapshot.channelCount <= 0 || numSamples <= 0)
        return snapshot;

    const auto safeStart = juce::jlimit(0, buffer.getNumSamples(), startSample);
    const auto safeSamples = juce::jlimit(0, buffer.getNumSamples() - safeStart, numSamples);
    if (safeSamples <= 0)
        return snapshot;

    std::array<double, OutputMeterSnapshot::maxChannels> sumSquares {};
    std::array<double, OutputMeterSnapshot::maxChannels> weightedEnergy {};
    for (int channel = 0; channel < snapshot.channelCount; ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel, safeStart);
        auto previous = previousSamples[static_cast<std::size_t>(channel)];
        auto peak = 0.0f;
        auto squares = 0.0;
        auto energy = 0.0;
        auto& shelf = shelfFilters[static_cast<std::size_t>(channel)];
        auto& highPass = highPassFilters[static_cast<std::size_t>(channel)];

        for (int sample = 0; sample < safeSamples; ++sample)
        {
            const auto value = samples[sample];
            peak = juce::jmax(peak, std::abs(value));
            peak = juce::jmax(peak, std::abs(previous + ((value - previous) * 0.25f)));
            peak = juce::jmax(peak, std::abs(previous + ((value - previous) * 0.50f)));
            peak = juce::jmax(peak, std::abs(previous + ((value - previous) * 0.75f)));
            previous = value;
            squares += static_cast<double>(value) * value;

            auto filtered = shelf.processSingleSampleRaw(value);
            filtered = highPass.processSingleSampleRaw(filtered);
            energy += static_cast<double>(filtered) * filtered;
        }

        previousSamples[static_cast<std::size_t>(channel)] = previous;
        sumSquares[static_cast<std::size_t>(channel)] = squares;
        weightedEnergy[static_cast<std::size_t>(channel)] = energy;
        snapshot.truePeakLinear[static_cast<std::size_t>(channel)] = peak;
        snapshot.rmsLinear[static_cast<std::size_t>(channel)] =
            static_cast<float>(std::sqrt(squares / safeSamples));
    }

    appendLoudnessBlock(weightedEnergy, safeSamples);

    auto globalWeightedMean = 0.0;
    for (int channel = 0; channel < snapshot.channelCount; ++channel)
    {
        const auto meanEnergy = rollingSampleCount > 0
                              ? rollingEnergyByChannel[static_cast<std::size_t>(channel)]
                                    / rollingSampleCount
                              : 0.0;
        const auto contribution = meanEnergy * loudnessWeightForRole(outputRoles[static_cast<std::size_t>(channel)]);
        snapshot.kWeightedContributionDb[static_cast<std::size_t>(channel)] = energyToLoudnessDb(contribution);
        globalWeightedMean += contribution;
    }
    snapshot.momentaryLufs = energyToLoudnessDb(globalWeightedMean);
    return snapshot;
}

double OutputMeterAnalyzer::loudnessWeightForRole(SpeakerRole role) noexcept
{
    switch (role)
    {
        case SpeakerRole::LFE:
            return 0.0;
        case SpeakerRole::Ls:
        case SpeakerRole::Rs:
        case SpeakerRole::Lss:
        case SpeakerRole::Rss:
            return 1.41;
        default:
            return 1.0;
    }
}

juce::IIRCoefficients OutputMeterAnalyzer::makeKWeightShelf(double sampleRate) noexcept
{
    constexpr auto frequency = 1681.974450955533;
    constexpr auto gainDb = 3.999843853973347;
    constexpr auto q = 0.7071752369554196;
    const auto k = std::tan(juce::MathConstants<double>::pi * frequency / sampleRate);
    const auto vh = std::pow(10.0, gainDb / 20.0);
    const auto vb = std::pow(vh, 0.4996667741545416);
    const auto a0 = 1.0 + (k / q) + (k * k);
    return { static_cast<float>((vh + (vb * k / q) + (k * k)) / a0),
             static_cast<float>((2.0 * ((k * k) - vh)) / a0),
             static_cast<float>((vh - (vb * k / q) + (k * k)) / a0),
             1.0f,
             static_cast<float>((2.0 * ((k * k) - 1.0)) / a0),
             static_cast<float>((1.0 - (k / q) + (k * k)) / a0) };
}

juce::IIRCoefficients OutputMeterAnalyzer::makeKWeightHighPass(double sampleRate) noexcept
{
    constexpr auto frequency = 38.13547087602444;
    constexpr auto q = 0.5003270373238773;
    const auto k = std::tan(juce::MathConstants<double>::pi * frequency / sampleRate);
    const auto a0 = 1.0 + (k / q) + (k * k);
    return { 1.0f,
             -2.0f,
             1.0f,
             1.0f,
             static_cast<float>((2.0 * ((k * k) - 1.0)) / a0),
             static_cast<float>((1.0 - (k / q) + (k * k)) / a0) };
}

float OutputMeterAnalyzer::energyToLoudnessDb(double energy) noexcept
{
    if (energy <= 1.0e-12)
        return minimumLoudnessDb;

    return static_cast<float>(juce::jlimit<double>(minimumLoudnessDb,
                                                    12.0,
                                                    loudnessCalibrationDb + (10.0 * std::log10(energy))));
}

void OutputMeterAnalyzer::appendLoudnessBlock(
    const std::array<double, OutputMeterSnapshot::maxChannels>& energyByChannel,
    int sampleCount) noexcept
{
    if (historyCount == static_cast<int>(loudnessHistory.size()))
    {
        const auto& oldest = loudnessHistory[static_cast<std::size_t>(historyReadIndex)];
        for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
            rollingEnergyByChannel[static_cast<std::size_t>(channel)] -= oldest.energyByChannel[static_cast<std::size_t>(channel)];
        rollingSampleCount -= oldest.sampleCount;
        historyReadIndex = (historyReadIndex + 1) % static_cast<int>(loudnessHistory.size());
        --historyCount;
    }

    auto& slot = loudnessHistory[static_cast<std::size_t>(historyWriteIndex)];
    slot.energyByChannel = energyByChannel;
    slot.sampleCount = sampleCount;
    historyWriteIndex = (historyWriteIndex + 1) % static_cast<int>(loudnessHistory.size());
    ++historyCount;
    for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
        rollingEnergyByChannel[static_cast<std::size_t>(channel)] += energyByChannel[static_cast<std::size_t>(channel)];
    rollingSampleCount += sampleCount;

    while (historyCount > 1)
    {
        const auto& oldest = loudnessHistory[static_cast<std::size_t>(historyReadIndex)];
        if (rollingSampleCount - oldest.sampleCount < momentaryWindowSamples)
            break;

        for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
            rollingEnergyByChannel[static_cast<std::size_t>(channel)] -= oldest.energyByChannel[static_cast<std::size_t>(channel)];
        rollingSampleCount -= oldest.sampleCount;
        historyReadIndex = (historyReadIndex + 1) % static_cast<int>(loudnessHistory.size());
        --historyCount;
    }
}
} // namespace singleeditor
