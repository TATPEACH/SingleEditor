#include "Automation/AudioInspectionService.h"

#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicStageGraph.h"
#include "Audio/ChannelLayout.h"

#include <algorithm>
#include <cmath>

namespace singleeditor
{
bool AudioInspectionResult::hasNonFiniteSamples() const noexcept
{
    return std::any_of(channels.begin(), channels.end(), [](const auto& stats)
    {
        return stats.nonFiniteSamples != 0;
    });
}

juce::var AudioInspectionResult::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("input", info.file.getFullPathName());
    root->setProperty("opened", opened);
    root->setProperty("ok", opened && errorMessage.isEmpty());
    root->setProperty("error",
                      errorMessage.isEmpty() ? juce::var() : juce::var(errorMessage));

    auto* identity = new juce::DynamicObject();
    identity->setProperty("size_bytes", static_cast<juce::int64>(info.file.getSize()));
    identity->setProperty(
        "modified_ms",
        static_cast<juce::int64>(info.file.getLastModificationTime().toMilliseconds()));
    root->setProperty("identity", juce::var(identity));

    root->setProperty("format", info.formatName);
    root->setProperty("duration_seconds", info.durationSeconds);
    root->setProperty("sample_rate", info.sampleRate);
    root->setProperty("num_channels", info.numChannels);
    root->setProperty("bits_per_sample", info.bitsPerSample);
    root->setProperty("num_samples", static_cast<juce::int64>(info.numSamples));

    auto* layout = new juce::DynamicObject();
    layout->setProperty("preset_id", info.channelLayout.selectedPresetId);
    layout->setProperty("display_name", info.channelLayout.displayName);
    layout->setProperty("order", info.channelLayout.orderText);
    layout->setProperty("detected_from_metadata", info.channelLayout.detectedFromMetadata);
    layout->setProperty("discrete_fallback", info.channelLayout.isDiscreteFallback);
    layout->setProperty("ambisonic", info.channelLayout.isAmbisonic);
    layout->setProperty("ambisonic_order", info.channelLayout.ambisonicOrder);
    layout->setProperty("normalization",
                        info.channelLayout.isAmbisonic
                            ? juce::var(info.channelLayout.ambisonicNormalization)
                            : juce::var());
    layout->setProperty(
        "monitor_decode_status",
        AmbisonicMonitorDecoder::statusToString(
            AmbisonicMonitorDecoder::statusFor(info.channelLayout)));
    layout->setProperty("monitor_decode_mode",
                        AmbisonicMonitorDecoder::modeFor(info.channelLayout));
    layout->setProperty("monitor_decode_capability",
                        AmbisonicMonitorDecoder::capabilityFor(info.channelLayout));
    layout->setProperty("ambisonic_stage_graph_schema",
                        info.channelLayout.isAmbisonic
                            ? juce::var(AmbisonicStageGraph::schemaVersion)
                            : juce::var());
    layout->setProperty("ambisonic_stage_order",
                        info.channelLayout.isAmbisonic
                            ? juce::var(AmbisonicStageGraph::stageOrderId())
                            : juce::var());
    layout->setProperty("ambisonic_decoder_profile",
                        info.channelLayout.isAmbisonic
                            ? juce::var(AmbisonicMonitorDecoder::decoderProfileId())
                            : juce::var());
    const auto decodeStatus = AmbisonicMonitorDecoder::statusFor(info.channelLayout);
    layout->setProperty("monitor_decode_runtime_state",
                        !info.channelLayout.isAmbisonic
                            ? "not_required"
                            : decodeStatus == AmbisonicMonitorDecodeStatus::ready
                                ? "inactive"
                                : "unavailable");

    juce::Array<juce::var> candidates;
    for (const auto& candidate : info.channelLayout.candidates)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("preset_id", candidate.id);
        item->setProperty("display_name", candidate.displayName);
        item->setProperty("order", candidate.orderText);
        item->setProperty("ambisonic", candidate.isAmbisonic);
        candidates.add(juce::var(item));
    }
    layout->setProperty("candidates", juce::var(candidates));
    juce::Array<juce::var> roles;
    for (const auto role : info.channelLayout.selectedRoles)
        roles.add(ChannelLayoutCatalog::roleToAbbreviation(role));
    layout->setProperty("roles", juce::var(roles));
    root->setProperty("channel_layout", juce::var(layout));

    root->setProperty("sample_stats_scanned", sampleScanCompleted);
    root->setProperty("samples_scanned_per_channel",
                      static_cast<juce::int64>(samplesScannedPerChannel));
    juce::Array<juce::var> channelValues;
    for (size_t index = 0; index < channels.size(); ++index)
    {
        auto* channel = new juce::DynamicObject();
        channel->setProperty("index", static_cast<int>(index + 1));
        const auto role = index < info.channelLayout.selectedRoles.size()
                        ? ChannelLayoutCatalog::roleToAbbreviation(
                              info.channelLayout.selectedRoles[index])
                        : "Ch " + juce::String(static_cast<int>(index + 1));
        channel->setProperty("role", role);
        channel->setProperty("peak", channels[index].peak);
        channel->setProperty("rms", channels[index].rms);
        channel->setProperty("non_finite_samples",
                             static_cast<juce::int64>(channels[index].nonFiniteSamples));
        channelValues.add(juce::var(channel));
    }
    root->setProperty("channels", juce::var(channelValues));
    return juce::var(root);
}

AudioInspectionResult AudioInspectionService::inspect(const juce::File& file,
                                                       AudioInspectionOptions options)
{
    AudioInspectionResult result;
    result.info.file = file;
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

    result.opened = true;
    result.info = prepared.info;
    if (!options.scanSamples)
        return result;

    const auto blockSize = juce::jlimit(1, 1 << 20, options.blockSize);
    juce::AudioBuffer<float> block(result.info.numChannels, blockSize);
    std::vector<long double> sumSquares(static_cast<size_t>(result.info.numChannels),
                                        0.0L);
    result.channels.resize(static_cast<size_t>(result.info.numChannels));

    for (int64_t position = 0; position < result.info.numSamples; position += blockSize)
    {
        const auto count = static_cast<int>(std::min<int64_t>(
            blockSize, result.info.numSamples - position));
        const juce::Range<int64_t> requestedRange(position, position + count);
        if (prepared.memoryMappedReader != nullptr
            && !prepared.memoryMappedReader->getMappedSection().contains(requestedRange)
            && !prepared.memoryMappedReader->mapSectionOfFile(requestedRange))
        {
            result.errorMessage = "Memory map failed during sample scan";
            return result;
        }

        block.clear();
        if (!reader->read(&block, 0, count, position, true, true))
        {
            result.errorMessage = "Reader failed during sample scan";
            return result;
        }

        for (int channel = 0; channel < result.info.numChannels; ++channel)
        {
            const auto* samples = block.getReadPointer(channel);
            auto& stats = result.channels[static_cast<size_t>(channel)];
            for (int sample = 0; sample < count; ++sample)
            {
                const auto value = static_cast<double>(samples[sample]);
                if (!std::isfinite(value))
                {
                    ++stats.nonFiniteSamples;
                    continue;
                }

                stats.peak = std::max(stats.peak, std::abs(value));
                sumSquares[static_cast<size_t>(channel)] += value * value;
            }
        }

        result.samplesScannedPerChannel += count;
    }

    for (size_t channel = 0; channel < result.channels.size(); ++channel)
    {
        const auto finiteSamples = result.samplesScannedPerChannel
                                 - result.channels[channel].nonFiniteSamples;
        if (finiteSamples > 0)
        {
            result.channels[channel].rms = std::sqrt(static_cast<double>(
                sumSquares[channel] / static_cast<long double>(finiteSamples)));
        }
    }

    result.sampleScanCompleted = true;
    return result;
}
} // namespace singleeditor
