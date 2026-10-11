#include "Automation/TrimSilenceAnalysisService.h"

#include "Audio/ChannelLayout.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace singleeditor
{
namespace
{
double gainFromDb(double decibels) noexcept
{
    return std::pow(10.0, decibels / 20.0);
}

juce::Array<juce::var> metadataRisksFor(
    const SingleTrackDocument::FileInfo& info)
{
    juce::Array<juce::var> risks;
    for (const auto& key : info.metadata.getAllKeys())
    {
        const auto lower = key.toLowerCase();
        if (lower.contains("bwf")
            || lower.contains("bwav")
            || lower.contains("ixml")
            || lower.contains("cue")
            || lower.contains("smpl")
            || lower.contains("loop"))
        {
            if (!risks.contains(key))
                risks.add(key);
        }
    }
    return risks;
}
} // namespace

juce::var TrimSilenceAnalysisResult::toPlanItem(
    const TrimSilenceAnalysisOptions& options) const
{
    auto* item = new juce::DynamicObject();
    item->setProperty("input", info.file.getFullPathName());
    item->setProperty("input_identity", identity.toJson());
    item->setProperty("status", succeeded ? "planned" : "failed");
    item->setProperty("error",
                      errorMessage.isEmpty() ? juce::var() : juce::var(errorMessage));

    auto* media = new juce::DynamicObject();
    media->setProperty("format", info.formatName);
    media->setProperty("sample_rate", info.sampleRate);
    media->setProperty("bits_per_sample", info.bitsPerSample);
    media->setProperty("num_channels", info.numChannels);
    media->setProperty("num_samples", info.numSamples);
    media->setProperty("duration_seconds", info.durationSeconds);
    auto* layout = new juce::DynamicObject();
    layout->setProperty("preset_id", info.channelLayout.selectedPresetId);
    layout->setProperty("display_name", info.channelLayout.displayName);
    layout->setProperty("order", info.channelLayout.orderText);
    layout->setProperty("detected_from_metadata", info.channelLayout.detectedFromMetadata);
    media->setProperty("channel_layout", juce::var(layout));
    item->setProperty("media", juce::var(media));

    auto* analysis = new juce::DynamicObject();
    analysis->setProperty("head_only", options.headOnly);
    analysis->setProperty("content_detected", contentDetected);
    analysis->setProperty("content_start_sample", contentStartSample);
    analysis->setProperty("samples_scanned", samplesScanned);
    item->setProperty("analysis", juce::var(analysis));

    auto* range = new juce::DynamicObject();
    range->setProperty("start_sample", static_cast<juce::int64>(0));
    range->setProperty("end_sample", suggestedTrimEndSample);
    item->setProperty("suggested_sample_range", juce::var(range));

    auto* estimated = new juce::DynamicObject();
    const auto outputSamples = juce::jmax<juce::int64>(
        0, info.numSamples - suggestedTrimEndSample);
    estimated->setProperty("num_samples", outputSamples);
    estimated->setProperty(
        "duration_seconds",
        info.sampleRate > 0.0
            ? static_cast<double>(outputSamples) / info.sampleRate
            : 0.0);
    estimated->setProperty("sample_rate", info.sampleRate);
    estimated->setProperty("bits_per_sample", info.bitsPerSample);
    estimated->setProperty("num_channels", info.numChannels);
    estimated->setProperty("format", info.formatName);
    item->setProperty("estimated_output", juce::var(estimated));

    auto* metadata = new juce::DynamicObject();
    metadata->setProperty("policy", "analyze_only");
    metadata->setProperty("risks", juce::var(metadataRisksFor(info)));
    juce::Array<juce::var> unsupported;
    metadata->setProperty("unsupported", juce::var(unsupported));
    item->setProperty("metadata", juce::var(metadata));
    return juce::var(item);
}

TrimSilenceAnalysisResult TrimSilenceAnalysisService::analyze(
    const juce::File& file,
    TrimSilenceAnalysisOptions options)
{
    TrimSilenceAnalysisResult result;
    result.info.file = file;
    result.identity = FileIdentity::capture(file);
    if (!file.existsAsFile())
    {
        result.errorMessage = "Input file does not exist";
        return result;
    }

    auto prepared = SingleTrackDocument::prepareAudioFileForOpen(file);
    auto* reader = prepared.getReader();
    if (reader == nullptr || !prepared.info.isValid())
    {
        result.errorMessage = prepared.errorMessage.isNotEmpty()
                                ? prepared.errorMessage
                                : "No supported audio reader could open the input";
        return result;
    }

    result.info = prepared.info;
    const auto windowSize = juce::jmax<juce::int64>(
        1, static_cast<juce::int64>(std::llround(
               result.info.sampleRate * options.windowMilliseconds / 1000.0)));
    const auto requiredWindows = juce::jmax(
        1, static_cast<int>(std::ceil(options.confirmationMilliseconds
                                   / options.windowMilliseconds)));
    const auto prerollSamples = juce::jmax<juce::int64>(
        0, static_cast<juce::int64>(std::llround(
               result.info.sampleRate * options.prerollMilliseconds / 1000.0)));
    const auto enterGain = gainFromDb(options.thresholdDbfs);
    const auto exitGain = gainFromDb(options.thresholdDbfs - options.hysteresisDb);
    const auto peakGuardGain = gainFromDb(
        juce::jmin(0.0, options.thresholdDbfs + options.hysteresisDb));

    constexpr int blockSize = 65536;
    juce::AudioBuffer<float> block(result.info.numChannels, blockSize);
    std::vector<long double> sumSquares(
        static_cast<size_t>(result.info.numChannels), 0.0L);
    juce::int64 windowStart = 0;
    juce::int64 candidateStart = 0;
    juce::int64 firstAboveThreshold = -1;
    juce::int64 windowSamples = 0;
    double windowPeak = 0.0;
    int activeWindows = 0;
    bool found = false;

    const auto evaluateWindow = [&]()
    {
        if (windowSamples <= 0 || found)
            return;

        double maxRms = 0.0;
        for (const auto sum : sumSquares)
            maxRms = std::max(maxRms, std::sqrt(static_cast<double>(sum / windowSamples)));

        const auto refinedStart = firstAboveThreshold >= 0
                                ? firstAboveThreshold
                                : windowStart;
        if (windowPeak >= peakGuardGain)
        {
            result.contentStartSample = refinedStart;
            found = true;
        }
        else if (activeWindows == 0)
        {
            if (maxRms >= enterGain)
            {
                activeWindows = 1;
                candidateStart = refinedStart;
                if (activeWindows >= requiredWindows)
                {
                    result.contentStartSample = candidateStart;
                    found = true;
                }
            }
        }
        else if (maxRms >= exitGain)
        {
            if (++activeWindows >= requiredWindows)
            {
                result.contentStartSample = candidateStart;
                found = true;
            }
        }
        else
        {
            activeWindows = 0;
        }

        windowStart += windowSamples;
        windowSamples = 0;
        windowPeak = 0.0;
        firstAboveThreshold = -1;
        std::fill(sumSquares.begin(), sumSquares.end(), 0.0L);
    };

    juce::int64 position = 0;
    while (position < result.info.numSamples && !found)
    {
        const auto count = static_cast<int>(std::min<juce::int64>(
            blockSize, result.info.numSamples - position));
        const juce::Range<juce::int64> requestedRange(position, position + count);
        if (prepared.memoryMappedReader != nullptr
            && !prepared.memoryMappedReader->getMappedSection().contains(requestedRange)
            && !prepared.memoryMappedReader->mapSectionOfFile(requestedRange))
        {
            result.errorMessage = "Memory map failed during trim analysis";
            return result;
        }

        block.clear();
        if (!reader->read(&block, 0, count, position, true, true))
        {
            result.errorMessage = "Reader failed during trim analysis";
            return result;
        }

        for (int sample = 0; sample < count && !found; ++sample)
        {
            double samplePeak = 0.0;
            for (int channel = 0; channel < result.info.numChannels; ++channel)
            {
                const auto value = static_cast<double>(block.getSample(channel, sample));
                const auto magnitude = std::abs(value);
                samplePeak = std::max(samplePeak, magnitude);
                sumSquares[static_cast<size_t>(channel)] += value * value;
            }
            windowPeak = std::max(windowPeak, samplePeak);
            if (firstAboveThreshold < 0 && samplePeak >= enterGain)
                firstAboveThreshold = position + sample;

            ++windowSamples;
            ++result.samplesScanned;
            if (windowSamples >= windowSize)
                evaluateWindow();
        }
        position += count;
    }
    evaluateWindow();

    result.contentDetected = found;
    if (found)
    {
        result.suggestedTrimEndSample = juce::jmax<juce::int64>(
            0, result.contentStartSample - prerollSamples);
    }
    else
    {
        result.contentStartSample = result.info.numSamples;
        result.suggestedTrimEndSample = 0;
    }

    const auto finalIdentity = FileIdentity::capture(file);
    if (!result.identity.matches(finalIdentity))
    {
        result.errorMessage = "Input changed during analysis";
        return result;
    }

    result.succeeded = true;
    return result;
}
} // namespace singleeditor
