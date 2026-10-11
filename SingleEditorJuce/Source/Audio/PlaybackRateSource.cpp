#include "Audio/PlaybackRateSource.h"
#include <samplerate.h>
#include <algorithm>
#include <cmath>
#include <new>

namespace singleeditor
{
PlaybackRateSource::PlaybackRateSource(juce::PositionableAudioSource& source, double rate, int capacity)
    : input(source), sourceRate(rate), channels(capacity), publishedChannels(capacity) {}

PlaybackRateSource::~PlaybackRateSource() { releaseResources(); }

void PlaybackRateSource::setRenderChannelCount(int count)
{
    const juce::ScopedLock guard(lock);
    if (channels == count) return;
    releaseResources();
    channels = count;
    publishedChannels.store(count);
}

PlaybackRateSource::Snapshot PlaybackRateSource::snapshot() const noexcept
{
    return { mode.load(), error.load(), sourceRate, deviceRate.load(), publishedChannels.load(), lookahead.load() };
}

void PlaybackRateSource::fail(Error reason) noexcept
{
    error.store(reason);
    mode.store(Mode::unavailable);
}

void PlaybackRateSource::prepareToPlay(int expectedBlockSize, double rate)
{
    const juce::ScopedLock guard(lock);
    const auto previousRate = deviceRate.load();
    const auto seconds = previousRate > 0.0 ? static_cast<double>(outputPosition.load()) / previousRate
        : static_cast<double>(input.getNextReadPosition()) / std::max(1.0, sourceRate);
    releaseResources();
    deviceRate.store(rate);
    error.store(Error::none);
    if (!std::isfinite(rate) || !std::isfinite(sourceRate) || rate <= 0.0 || sourceRate <= 0.0
        || channels < 1 || channels > 32 || expectedBlockSize < 1 || !src_is_valid_ratio(rate / sourceRate))
    {
        fail(Error::configuration);
        return;
    }
    blockCapacity = expectedBlockSize;
    try
    {
        if (sourceRate != rate)
        {
            int status = 0;
            converter = src_new(SRC_SINC_MEDIUM_QUALITY, channels, &status);
            if (!converter || status != 0) { fail(Error::allocation); return; }
            planar.setSize(channels, inputBlockFrames);
            channelViews.reserve(static_cast<std::size_t>(channels));
            for (int count = 1; count <= channels; ++count)
                channelViews.emplace_back(planar.getArrayOfWritePointers(), count, inputBlockFrames);
            interleavedInput.resize(static_cast<std::size_t>(channels * inputBlockFrames));
            interleavedOutput.resize(static_cast<std::size_t>(channels) * static_cast<std::size_t>(blockCapacity));
        }
        input.prepareToPlay(converter ? inputBlockFrames : expectedBlockSize, sourceRate);
        sourcePrepared = true;
    }
    catch (const std::bad_alloc&) { fail(Error::allocation); return; }
    mode.store(converter ? Mode::converting : Mode::bypassed);
    resetAt(static_cast<juce::int64>(std::llround(seconds * rate)));
}

void PlaybackRateSource::releaseResources()
{
    const juce::ScopedLock guard(lock);
    if (sourcePrepared) input.releaseResources();
    sourcePrepared = false;
    converter = src_delete(converter);
    channelViews.clear();
    planar.setSize(0, 0);
    interleavedInput.clear();
    interleavedOutput.clear();
    mode.store(Mode::unprepared);
    lookahead.store(0);
    inputFrames = inputOffset = renderChannels = 0;
    drained = false;
}

void PlaybackRateSource::resetAt(juce::int64 frame)
{
    frame = std::max<juce::int64>(0, frame);
    const auto rate = deviceRate.load();
    input.setNextReadPosition(rate > 0.0
        ? static_cast<juce::int64>(std::llround(static_cast<double>(frame) * sourceRate / rate)) : frame);
    outputPosition.store(frame);
    inputFrames = inputOffset = 0;
    drained = false;
    lookahead.store(0);
    if (converter)
    {
        src_reset(converter);
        src_set_ratio(converter, rate / sourceRate);
    }
}

void PlaybackRateSource::setNextReadPosition(juce::int64 frame)
{
    const juce::ScopedLock guard(lock);
    resetAt(frame);
}

juce::int64 PlaybackRateSource::getNextReadPosition() const { return outputPosition.load(); }

juce::int64 PlaybackRateSource::getTotalLength() const
{
    const auto rate = deviceRate.load();
    return rate > 0.0 && sourceRate > 0.0
        ? static_cast<juce::int64>(std::ceil(static_cast<double>(input.getTotalLength()) * rate / sourceRate))
        : input.getTotalLength();
}

void PlaybackRateSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& block)
{
    const juce::ScopedLock guard(lock);
    if (!block.buffer || block.numSamples <= 0) return;
    const auto activeChannels = block.buffer->getNumChannels();
    if (mode.load() == Mode::unavailable || mode.load() == Mode::unprepared)
    {
        block.clearActiveBufferRegion();
        return;
    }
    if (activeChannels < 1 || activeChannels > channels || block.numSamples > blockCapacity)
    {
        block.clearActiveBufferRegion();
        fail(Error::blockShape);
        return;
    }
    if (!converter)
    {
        input.getNextAudioBlock(block);
        outputPosition.store(input.getNextReadPosition());
        return;
    }
    block.clearActiveBufferRegion();
    if (renderChannels != activeChannels)
    {
        // A different output layout must not inherit buffered rows from the old
        // layout. Views and the converter were allocated during preparation.
        resetAt(outputPosition.load());
        renderChannels = activeChannels;
    }
    if (drained) return;
    int generated = 0;
    const auto ratio = deviceRate.load() / sourceRate;
    while (generated < block.numSamples)
    {
        if (inputOffset == inputFrames)
        {
            inputOffset = inputFrames = 0;
            const auto before = input.getNextReadPosition();
            if (before < input.getTotalLength())
            {
                planar.clear();
                auto& view = channelViews[static_cast<std::size_t>(activeChannels - 1)];
                input.getNextAudioBlock({ &view, 0, inputBlockFrames });
                inputFrames = static_cast<int>(std::clamp<juce::int64>(input.getNextReadPosition() - before, 0, inputBlockFrames));
                for (int frame = 0; frame < inputFrames; ++frame)
                    for (int channel = 0; channel < channels; ++channel)
                        interleavedInput[static_cast<std::size_t>(frame * channels + channel)] = planar.getSample(channel, frame);
            }
        }
        const auto atEnd = input.getNextReadPosition() >= input.getTotalLength();
        SRC_DATA data {};
        data.data_in = interleavedInput.data() + static_cast<std::size_t>(inputOffset * channels);
        data.input_frames = inputFrames - inputOffset;
        data.data_out = interleavedOutput.data() + static_cast<std::size_t>(generated * channels);
        data.output_frames = block.numSamples - generated;
        data.src_ratio = ratio;
        data.end_of_input = atEnd ? 1 : 0;
        if (src_process(converter, &data) != 0) { fail(Error::converter); return; }
        inputOffset += static_cast<int>(data.input_frames_used);
        generated += static_cast<int>(data.output_frames_gen);
        if (data.input_frames_used == 0 && data.output_frames_gen == 0)
        {
            // An unpublished progressive range is a temporary gap. Only real
            // EOF may flush the filter or finish the transport.
            drained = atEnd;
            break;
        }
    }
    for (int channel = 0; channel < activeChannels; ++channel)
        for (int frame = 0; frame < generated; ++frame)
            block.buffer->setSample(channel, block.startSample + frame,
                interleavedOutput[static_cast<std::size_t>(frame * channels + channel)]);
    outputPosition.fetch_add(generated);
    if (drained) outputPosition.store(getTotalLength()); // possible fractional final device frame
    lookahead.store(static_cast<int>(std::max(0.0,
        std::ceil(static_cast<double>(input.getNextReadPosition()) * ratio - static_cast<double>(outputPosition.load())))));
}
}
