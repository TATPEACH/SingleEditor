#include "Audio/SpatialEnergyMeter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace singleeditor
{
namespace
{
constexpr double calibration = -0.691;
constexpr std::size_t momentaryBlocks = 8, shortBlocks = 60;
double loudness(double power) noexcept { return calibration + SpatialEnergyMeter::powerDb(power); }
}

SpatialEnergyMeter::SpatialEnergyMeter(Layout requested)
    : layout(std::make_shared<const Layout>(std::move(requested))),
      queue(queueCapacity), history(shortBlocks)
{
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    const auto rate = layout->sampleRate;
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000
        || layout->channels.empty() || layout->channels.size() > maxChannels)
        throw std::invalid_argument("Spatial Energy requires a prepared physical layout/rate");
    std::vector<int> physical;
    for (const auto& channel : layout->channels)
    {
        if (channel.physicalOutput < 0
            || std::find(physical.begin(), physical.end(), channel.physicalOutput) != physical.end()
            || (channel.loudnessWeight && (!std::isfinite(*channel.loudnessWeight) || *channel.loudnessWeight < 0)))
            throw std::invalid_argument("Spatial Energy requires unique physical outputs and valid weights");
        physical.push_back(channel.physicalOutput);
        if (channel.placement != Placement::lfe && !channel.loudnessWeight) weightsKnown = false;
    }
    // Analogue parameterisation reproduces BS.1770's 48 kHz coefficients and
    // maps the same two stages to the actual device clock. All double precision.
    const auto k = std::tan(juce::MathConstants<double>::pi * 1681.974450955533 / rate);
    const auto vh = std::pow(10.0, 3.999843853973347 / 20.0);
    const auto vb = std::pow(vh, 0.4996667741545416);
    constexpr auto q = 0.7071752369554196;
    const auto denominator = 1.0 + k / q + k * k;
    Biquad shelfPrototype;
    shelfPrototype.b0 = (vh + vb * k / q + k * k) / denominator;
    shelfPrototype.b1 = 2.0 * (k * k - vh) / denominator;
    shelfPrototype.b2 = (vh - vb * k / q + k * k) / denominator;
    shelfPrototype.a1 = 2.0 * (k * k - 1.0) / denominator;
    shelfPrototype.a2 = (1.0 - k / q + k * k) / denominator;
    const auto h = std::tan(juce::MathConstants<double>::pi * 38.13547087602444 / rate);
    constexpr auto hq = 0.5003270373238773;
    const auto hd = 1.0 + h / hq + h * h;
    Biquad highPassPrototype;
    highPassPrototype.b1 = -2;
    highPassPrototype.b2 = 1;
    highPassPrototype.a1 = 2.0 * (h * h - 1.0) / hd;
    highPassPrototype.a2 = (1.0 - h / hq + h * h) / hd;
    shelf.fill(shelfPrototype);
    highPass.fill(highPassPrototype);
    current.layout = layout;
    current.channels.resize(layout->channels.size());
}

SpatialEnergyMeter::~SpatialEnergyMeter() = default;

double SpatialEnergyMeter::Biquad::process(double x) noexcept
{
    const auto y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

double SpatialEnergyMeter::powerDb(double power) noexcept
{
    return power > 0 ? 10.0 * std::log10(power) : -INFINITY;
}

void SpatialEnergyMeter::resetAudio(std::uint64_t epoch) noexcept
{
    audioEpoch = epoch;
    ++audioRun;
    audioSamples = tick = 0;
    nextEnd = static_cast<std::uint64_t>(std::llround(layout->sampleRate / 20.0));
    partial = {};
    for (std::size_t c = 0; c < layout->channels.size(); ++c)
    {
        shelf[c].reset();
        highPass[c].reset();
    }
    audioInitialised = true;
    restartAfterStop = false;
}

void SpatialEnergyMeter::push(Block& block) noexcept
{
    block.serial = ++serial;
    block.epoch = audioEpoch;
    block.run = audioRun;
    block.endSample = audioSamples;
    block.transport = audioTransport;
    block.droppedBefore = dropped.load(std::memory_order_relaxed);
    const auto w = written.load(std::memory_order_relaxed);
    if (w - read.load(std::memory_order_acquire) == queueCapacity)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue[static_cast<std::size_t>(w % queueCapacity)] = block;
    written.store(w + 1, std::memory_order_release);
}

void SpatialEnergyMeter::process(const juce::AudioBuffer<float>& outputs, int start, int samples,
                                std::uint64_t epoch, Transport transport) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (start < 0 || samples < 0 || start > outputs.getNumSamples()
        || samples > outputs.getNumSamples() - start
        || outputs.getNumChannels() != static_cast<int>(layout->channels.size()))
        transport = Transport::unavailable;
    const auto reset = !audioInitialised || epoch != audioEpoch
        || (restartAfterStop && transport == Transport::running);
    if (reset) resetAudio(epoch);
    if (reset || transport != audioTransport)
    {
        audioTransport = transport;
        Block event;
        push(event);
    }
    if (transport == Transport::stopped || transport == Transport::unavailable) restartAfterStop = true;
    if (transport != Transport::running) return;

    int offset = 0;
    while (offset < samples)
    {
        const auto count = std::min(samples - offset, static_cast<int>(nextEnd - audioSamples));
        for (std::size_t c = 0; c < layout->channels.size(); ++c)
        {
            const auto* input = outputs.getReadPointer(static_cast<int>(c), start + offset);
            for (int s = 0; s < count; ++s)
            {
                const auto value = static_cast<double>(input[s]);
                const auto filtered = highPass[c].process(shelf[c].process(value));
                partial.energy[c] += filtered * filtered;
                partial.rawEnergy[c] += value * value;
                partial.peak[c] = std::max(partial.peak[c], std::abs(input[s]));
            }
        }
        offset += count;
        audioSamples += static_cast<std::uint64_t>(count);
        partial.samples += count;
        if (audioSamples == nextEnd)
        {
            push(partial);
            partial = {};
            ++tick;
            // Absolute sample boundaries prevent callback-size and fractional
            // clock rounding drift. M=8 ticks, S=60; each endpoint nearest sample.
            nextEnd = static_cast<std::uint64_t>(std::llround(static_cast<double>(tick + 1) * layout->sampleRate / 20.0));
        }
    }
}

void SpatialEnergyMeter::resetWorker(const Block& block, bool gap)
{
    current = {};
    current.layout = layout;
    current.channels.resize(layout->channels.size());
    current.epoch = block.epoch;
    current.discontinuity = gap;
    workerRun = block.run;
    acknowledgedDrops = block.droppedBefore;
    lastGateEndSample = 0;
    historyWrite = historyCount = 0;
    momentarySamples = shortSamples = blocks = 0;
    momentaryEnergy.fill(0);
    momentaryRaw.fill(0);
    shortEnergy.fill(0);
    gateEnergies.clear();
    absoluteGateSum = 0;
}

void SpatialEnergyMeter::consume(const Block& block)
{
    const auto gap = workerSerial != 0 && block.serial != workerSerial + 1;
    if (block.run != workerRun || gap) resetWorker(block, gap);
    workerSerial = block.serial;
    current.sequence = block.serial;
    current.transport = block.transport;
    if (block.samples == 0)
    {
        if (block.transport != Transport::running) updateIntegrated();
        return;
    }
    const auto& oldM = history[(historyWrite + shortBlocks - momentaryBlocks) % shortBlocks];
    const auto& oldS = history[historyWrite];
    for (std::size_t c = 0; c < layout->channels.size(); ++c)
    {
        momentaryEnergy[c] += block.energy[c] - (historyCount >= momentaryBlocks ? oldM.energy[c] : 0);
        momentaryRaw[c] += block.rawEnergy[c] - (historyCount >= momentaryBlocks ? oldM.rawEnergy[c] : 0);
        shortEnergy[c] += block.energy[c] - (historyCount == shortBlocks ? oldS.energy[c] : 0);
    }
    momentarySamples += static_cast<std::uint64_t>(block.samples);
    shortSamples += static_cast<std::uint64_t>(block.samples);
    if (historyCount >= momentaryBlocks) momentarySamples -= static_cast<std::uint64_t>(oldM.samples);
    if (historyCount == shortBlocks) shortSamples -= static_cast<std::uint64_t>(oldS.samples);
    history[historyWrite] = block;
    historyWrite = (historyWrite + 1) % shortBlocks;
    historyCount = std::min(historyCount + 1, shortBlocks);
    current.measuredSamples = block.endSample;
    ++blocks;
    updateSnapshot();
    if (current.momentaryReady && weightsKnown && blocks % 2 == 0)
    {
        lastGateEndSample = block.endSample;
        double power = 0;
        for (std::size_t c = 0; c < layout->channels.size(); ++c)
            if (layout->channels[c].placement != Placement::lfe)
                power += momentaryEnergy[c] / static_cast<double>(momentarySamples) * *layout->channels[c].loudnessWeight;
        if (loudness(power) > -70.0)
        {
            gateEnergies.push_back(power);
            absoluteGateSum += power;
        }
        // I is exact over retained gate blocks, refreshed at 1 Hz, first complete
        // gate and transport suspension. M/S and contributions stay at 20 Hz.
        if (blocks == momentaryBlocks || blocks % 20 == 0) updateIntegrated();
    }
}

void SpatialEnergyMeter::updateIntegrated()
{
    if (!weightsKnown || !current.momentaryReady) return;
    current.integratedThroughSamples = lastGateEndSample;
    if (gateEnergies.empty()) { current.integratedLufs = -INFINITY; return; }
    const auto threshold = absoluteGateSum / static_cast<double>(gateEnergies.size()) * 0.1;
    double sum = 0;
    std::size_t count = 0;
    for (const auto energy : gateEnergies)
        if (energy > threshold) { sum += energy; ++count; }
    current.integratedLufs = count > 0 ? loudness(sum / static_cast<double>(count)) : -INFINITY;
}

void SpatialEnergyMeter::updateSnapshot()
{
    current.momentaryReady = historyCount >= momentaryBlocks;
    current.shortTermReady = historyCount >= shortBlocks;
    double m = 0, s = 0;
    for (std::size_t c = 0; c < layout->channels.size(); ++c)
    {
        auto& value = current.channels[c];
        value.kWeightedMeanSquare = std::max(0.0, momentaryEnergy[c]) / static_cast<double>(momentarySamples);
        value.contributionDb = powerDb(value.kWeightedMeanSquare);
        value.rmsDbfs = powerDb(std::max(0.0, momentaryRaw[c]) / static_cast<double>(momentarySamples));
        float peak = 0;
        for (std::size_t b = 0; b < std::min(historyCount, momentaryBlocks); ++b)
            peak = std::max(peak, history[(historyWrite + shortBlocks - 1 - b) % shortBlocks].peak[c]);
        value.samplePeakDbfs = powerDb(static_cast<double>(peak) * peak);
        if (layout->channels[c].placement != Placement::lfe && layout->channels[c].loudnessWeight)
        {
            m += value.kWeightedMeanSquare * *layout->channels[c].loudnessWeight;
            s += std::max(0.0, shortEnergy[c]) / static_cast<double>(shortSamples) * *layout->channels[c].loudnessWeight;
        }
    }
    if (current.momentaryReady && weightsKnown)
    {
        current.momentaryLufs = loudness(m);
        current.maximumMomentaryLufs = std::max(current.maximumMomentaryLufs.value_or(-INFINITY), *current.momentaryLufs);
    }
    if (current.shortTermReady && weightsKnown)
    {
        current.shortTermLufs = loudness(s);
        current.maximumShortTermLufs = std::max(current.maximumShortTermLufs.value_or(-INFINITY), *current.shortTermLufs);
    }
}

std::shared_ptr<const SpatialEnergyMeter::Snapshot> SpatialEnergyMeter::poll()
{
    auto r = read.load(std::memory_order_relaxed);
    const auto end = written.load(std::memory_order_acquire);
    if (r == end) return {};
    while (r != end)
    {
        consume(queue[static_cast<std::size_t>(r % queueCapacity)]);
        read.store(++r, std::memory_order_release);
    }
    current.droppedBlocks = dropped.load(std::memory_order_relaxed);
    // A full queue may have no subsequent block yet. Invalidate immediately;
    // don't present the pre-gap history as current trusted measurement.
    if (current.droppedBlocks != acknowledgedDrops) current.discontinuity = true;
    return std::make_shared<const Snapshot>(current);
}
}
