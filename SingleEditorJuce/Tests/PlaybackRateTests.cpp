#include "Audio/PlaybackRateSource.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
using Rate = singleeditor::PlaybackRateSource;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

class Source final : public juce::PositionableAudioSource
{
public:
    Source(int channels, int frames) : samples(channels, frames), available(frames) { samples.clear(); }
    void prepareToPlay(int, double rate) override { preparedRate = rate; }
    void releaseResources() override {}
    void setNextReadPosition(juce::int64 frame) override { position = frame; }
    juce::int64 getNextReadPosition() const override { return position; }
    juce::int64 getTotalLength() const override { return samples.getNumSamples(); }
    bool isLooping() const override { return false; }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& block) override
    {
        ++reads;
        requestedChannels = block.buffer->getNumChannels();
        block.clearActiveBufferRegion();
        const auto count = static_cast<int>(std::clamp<juce::int64>(available - position, 0, block.numSamples));
        for (int channel = 0; channel < std::min(samples.getNumChannels(), block.buffer->getNumChannels()); ++channel)
            block.buffer->copyFrom(channel, block.startSample, samples, channel, static_cast<int>(position), count);
        position += count;
    }
    juce::AudioBuffer<float> samples;
    juce::int64 position = 0;
    int available, reads = 0, requestedChannels = 0;
    double preparedRate = 0.0;
};

std::vector<float> render(Rate& rate, int channels, int blockSize)
{
    std::vector<float> result;
    juce::AudioBuffer<float> block(channels, blockSize);
    for (int attempt = 0; attempt < 100000 && rate.getNextReadPosition() < rate.getTotalLength(); ++attempt)
    {
        const auto before = rate.getNextReadPosition();
        rate.getNextAudioBlock({ &block, 0, blockSize });
        require(rate.snapshot().mode != Rate::Mode::unavailable, "converter reports no failure");
        const auto count = static_cast<int>(std::min<juce::int64>(blockSize, rate.getNextReadPosition() - before));
        if (count == 0) break;
        for (int frame = 0; frame < count; ++frame)
            for (int channel = 0; channel < channels; ++channel) result.push_back(block.getSample(channel, frame));
    }
    return result;
}

void bypass()
{
    Source source(2, 513);
    for (int frame = 0; frame < 513; ++frame)
    {
        source.samples.setSample(0, frame, static_cast<float>(frame - 260) / 257.0f);
        source.samples.setSample(1, frame, frame % 2 ? -0.0f : std::numeric_limits<float>::denorm_min());
    }
    Rate rate(source, 48000.0, 2);
    rate.prepareToPlay(257, 48000.0);
    juce::AudioBuffer<float> block(2, 261);
    block.clear();
    rate.getNextAudioBlock({ &block, 2, 257 });
    require(rate.snapshot().mode == Rate::Mode::bypassed && source.reads == 1 && source.position == 257,
        "exact rate performs one direct read with no lookahead");
    for (int channel = 0; channel < 2; ++channel)
        require(std::memcmp(block.getReadPointer(channel, 2), source.samples.getReadPointer(channel), 257 * sizeof(float)) == 0,
            "bypass preserves float bits, including signed zero and subnormals");
    require(block.getSample(0, 0) == 0 && block.getSample(0, 260) == 0 && rate.snapshot().bufferedLookaheadFrames == 0,
        "bypass touches only the requested region and adds no buffer");
    rate.setNextReadPosition(17);
    rate.getNextAudioBlock({ &block, 0, 128 });
    require(source.position == 145 && rate.getNextReadPosition() == 145, "bypass seek uses exact source frames");
    rate.prepareToPlay(257, 44100.0);
    require(rate.snapshot().mode == Rate::Mode::converting && source.preparedRate == 48000.0,
        "converter prepares the source at its own rate");
    rate.prepareToPlay(257, 48000.0);
    require(rate.snapshot().mode == Rate::Mode::bypassed, "returning to exact rate removes converter");
}

void multichannelAndSeek()
{
    for (int channels : { 8, 12, 16 })
        for (double sourceRate : { 44100.0, 48000.0, 96000.0 })
        {
            Source source(channels, 9600);
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto gain = static_cast<float>((channel % 2 ? -1 : 1) * (channel + 1));
                source.samples.setSample(channel, 1000, gain * 0.02f);
                source.samples.setSample(channel, 3210, gain * -0.01f);
            }
            Rate rate(source, sourceRate, channels);
            rate.prepareToPlay(257, 48000.0);
            const auto output = render(rate, channels, 257);
            require(rate.getNextReadPosition() == rate.getTotalLength(), "converter drains exact duration without a stuck final frame");
            require(output.size() == static_cast<std::size_t>(rate.getTotalLength() * channels), "output length uses device frames");
            std::size_t peakFrame = 0;
            for (std::size_t frame = 1; frame < output.size() / static_cast<std::size_t>(channels); ++frame)
                if (std::abs(output[frame * static_cast<std::size_t>(channels)]) > std::abs(output[peakFrame * static_cast<std::size_t>(channels)]))
                    peakFrame = frame;
            require(std::abs(static_cast<double>(peakFrame) - 1000.0 * 48000.0 / sourceRate) <= 1.0,
                "sinc lookahead preserves the impulse timeline without adding hidden playback delay");
            for (std::size_t frame = 0; frame < output.size() / static_cast<std::size_t>(channels); ++frame)
                for (int channel = 0; channel < channels; ++channel)
                {
                    const auto gain = static_cast<float>((channel % 2 ? -1 : 1) * (channel + 1));
                    require(std::abs(output[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)]
                        - output[frame * static_cast<std::size_t>(channels)] * gain) < 2.0e-6f,
                        "all channels retain shared phase, polarity and gain including independent LFE rows");
                }
            source.position = 0;
            rate.setNextReadPosition(0);
            const auto chunked = render(rate, channels, 64);
            require(chunked.size() == output.size(), "callback block size does not change duration");
            for (std::size_t i = 0; i < output.size(); ++i)
                require(std::abs(chunked[i] - output[i]) < 2.0e-6f, "callback boundaries do not alter conversion");
            juce::AudioBuffer<float> first(channels, 257), second(channels, 257);
            rate.setNextReadPosition(425);
            rate.getNextAudioBlock({ &first, 0, 257 });
            rate.setNextReadPosition(425);
            rate.getNextAudioBlock({ &second, 0, 257 });
            for (int channel = 0; channel < channels; ++channel)
                require(std::memcmp(first.getReadPointer(channel), second.getReadPointer(channel), 257 * sizeof(float)) == 0,
                    "seek and loop reset every channel together with no prior filter tail");
        }
}

double toneRms(double sourceRate, double deviceRate, double frequency)
{
    Source source(2, static_cast<int>(sourceRate));
    for (int frame = 0; frame < source.samples.getNumSamples(); ++frame)
    {
        const auto sample = static_cast<float>(0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * frequency * frame / sourceRate));
        source.samples.setSample(0, frame, sample);
        source.samples.setSample(1, frame, -sample);
    }
    Rate rate(source, sourceRate, 2);
    rate.prepareToPlay(257, deviceRate);
    const auto samples = render(rate, 2, 257);
    double sum = 0.0;
    int count = 0;
    for (std::size_t frame = 1000; frame + 1000 < samples.size() / 2; ++frame)
    {
        sum += static_cast<double>(samples[frame * 2]) * samples[frame * 2];
        ++count;
    }
    return std::sqrt(sum / count);
}

void qualityAndProgressive()
{
    for (const auto rates : { std::pair{44100.0, 48000.0}, std::pair{48000.0, 96000.0}, std::pair{96000.0, 44100.0} })
        require(std::abs(toneRms(rates.first, rates.second, 1000.0) - 0.353553) < 0.0002,
            "representative up/downsample passband preserves amplitude");
    const auto alias = toneRms(96000.0, 48000.0, 30000.0);
    require(alias < 0.0000354, "96k to 48k rejects a 30k tone by at least 80 dB relative to passband");
    std::cout << "30k alias RMS=" << alias << '\n';

    Source source(2, 8000);
    for (int frame = 0; frame < 8000; ++frame)
        for (int channel = 0; channel < 2; ++channel)
            source.samples.setSample(channel, frame, static_cast<float>(std::sin(frame * 0.07) * (channel + 1) * 0.1));
    Rate rate(source, 44100.0, 2);
    rate.prepareToPlay(257, 48000.0);
    const auto full = render(rate, 2, 257);
    rate.setNextReadPosition(0);
    source.available = 511;
    auto partial = render(rate, 2, 257);
    const auto paused = rate.getNextReadPosition();
    require(paused > 0 && paused < rate.getTotalLength(), "temporary unpublished PCM does not finish transport");
    require(render(rate, 2, 257).empty() && rate.getNextReadPosition() == paused, "starvation does not advance through invented silence");
    source.available = 8000;
    const auto remainder = render(rate, 2, 257);
    partial.insert(partial.end(), remainder.begin(), remainder.end());
    require(partial.size() == full.size(), "publishing the next range resumes the full original timeline");
    for (std::size_t i = 0; i < full.size(); ++i)
        require(std::abs(full[i] - partial[i]) < 2.0e-6f, "progressive gaps retain filter state and channel alignment");
    juce::AudioBuffer<float> oversized(2, 258);
    rate.getNextAudioBlock({ &oversized, 0, 258 });
    require(rate.snapshot().mode == Rate::Mode::unavailable && rate.snapshot().error == Rate::Error::blockShape,
        "unexpected callback capacity fails explicitly without reallocating");
    rate.prepareToPlay(257, 0.0);
    require(rate.snapshot().mode == Rate::Mode::unavailable && rate.snapshot().error == Rate::Error::configuration,
        "invalid device rate is rejected instead of silently selecting 44.1k or 48k");
}
}

int main()
{
    try { bypass(); multichannelAndSeek(); qualityAndProgressive(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Playback rate tests passed\n";
    return 0;
}
