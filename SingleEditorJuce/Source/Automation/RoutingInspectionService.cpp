#include "Automation/RoutingInspectionService.h"

#include "Automation/AudioInspectionService.h"
#include "Audio/AmbisonicBinauralRenderer.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicStageGraph.h"
#include "Audio/AudioEngine.h"
#include "Audio/OutputMeterAnalyzer.h"
#include "Document/SingleTrackDocument.h"

#include <cmath>

namespace singleeditor
{
namespace
{
juce::var layoutToJson(const ChannelLayoutState& layout,
                       bool stageGraphActive,
                       AmbisonicOrientation orientation)
{
    auto* result = new juce::DynamicObject();
    result->setProperty("preset_id", layout.selectedPresetId);
    result->setProperty("display_name", layout.displayName);
    result->setProperty("order", layout.orderText);
    result->setProperty("interpretation_schema_version",
                        ChannelLayoutState::interpretationSchemaVersion);
    result->setProperty("interpretation_origin",
                        ChannelLayoutCatalog::interpretationOriginToString(
                            layout.interpretationOrigin));
    result->setProperty("interpretation_id",
                        "source-interpretation-v1:"
                            + ChannelLayoutCatalog::interpretationOriginToString(
                                  layout.interpretationOrigin)
                            + ":" + layout.selectedPresetId
                            + ":" + juce::String(static_cast<int64_t>(layout.waveChannelMask)));
    result->setProperty("wave_channel_mask",
                        layout.waveChannelMask == 0
                            ? juce::var()
                            : juce::var(static_cast<int64_t>(layout.waveChannelMask)));
    result->setProperty("detected_from_metadata", layout.detectedFromMetadata);
    result->setProperty("discrete_fallback", layout.isDiscreteFallback);
    result->setProperty("ambisonic", layout.isAmbisonic);
    result->setProperty("ambisonic_order", layout.ambisonicOrder);
    result->setProperty("normalization",
                        layout.isAmbisonic ? juce::var(layout.ambisonicNormalization)
                                           : juce::var());
    result->setProperty(
        "monitor_decode_status",
        AmbisonicMonitorDecoder::statusToString(
            AmbisonicMonitorDecoder::statusFor(layout)));
    result->setProperty("monitor_decode_mode",
                        AmbisonicMonitorDecoder::modeFor(layout));
    result->setProperty("monitor_decode_capability",
                        AmbisonicMonitorDecoder::capabilityFor(layout));
    result->setProperty("ambisonic_stage_graph_schema",
                        layout.isAmbisonic ? juce::var(AmbisonicStageGraph::schemaVersion)
                                           : juce::var());
    result->setProperty("ambisonic_stage_order",
                        layout.isAmbisonic ? juce::var(AmbisonicStageGraph::stageOrderId())
                                           : juce::var());
    result->setProperty("ambisonic_decoder_profile",
                        layout.isAmbisonic
                            ? juce::var(AmbisonicMonitorDecoder::decoderProfileId())
                            : juce::var());
    result->setProperty("ambisonic_effective_order",
                        layout.isAmbisonic ? juce::var(layout.ambisonicOrder)
                                           : juce::var());
    const auto decodeStatus = AmbisonicMonitorDecoder::statusFor(layout);
    result->setProperty("monitor_decode_runtime_state",
                        !layout.isAmbisonic
                            ? "not_required"
                            : stageGraphActive
                                ? "active"
                                : decodeStatus == AmbisonicMonitorDecodeStatus::ready
                                     ? "inactive"
                                     : "unavailable");
    if (layout.isAmbisonic)
    {
        auto* orientationObject = new juce::DynamicObject();
        orientationObject->setProperty("yaw_degrees", orientation.yawDegrees);
        orientationObject->setProperty("pitch_degrees", orientation.pitchDegrees);
        orientationObject->setProperty("roll_degrees", orientation.rollDegrees);
        result->setProperty("ambisonic_orientation", juce::var(orientationObject));
    }

    juce::Array<juce::var> candidates;
    for (const auto& candidate : layout.candidates)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("preset_id", candidate.id);
        item->setProperty("display_name", candidate.displayName);
        item->setProperty("order", candidate.orderText);
        item->setProperty("ambisonic", candidate.isAmbisonic);
        candidates.add(juce::var(item));
    }
    result->setProperty("candidates", juce::var(candidates));

    juce::Array<juce::var> roles;
    for (const auto role : layout.selectedRoles)
        roles.add(ChannelLayoutCatalog::roleToAbbreviation(role));
    result->setProperty("roles", juce::var(roles));
    return juce::var(result);
}

bool targetHasRole(const ChannelLayoutState& target, SpeakerRole role)
{
    for (const auto candidate : target.selectedRoles)
    {
        if (candidate == role)
            return true;
    }
    return false;
}

bool isExpectedDrop(SpeakerRole sourceRole,
                    const ChannelLayoutState& target,
                    const ChannelPlaybackState& playback)
{
    if (ChannelLayoutCatalog::isAmbisonicRole(sourceRole))
        return true;

    return sourceRole == SpeakerRole::LFE
        && !playback.includeLfeInDownmix
        && !targetHasRole(target, SpeakerRole::LFE);
}

SpeakerRole semanticRoleForOutput(const ChannelLayoutState& target,
                                  const ChannelOutputMapping& mapping,
                                  int outputChannel)
{
    for (const auto role : target.selectedRoles)
    {
        if (ChannelRouter::outputChannelForRole(target, mapping, role) == outputChannel)
            return role;
    }
    return SpeakerRole::unknown;
}

void runRuntimeRenderProbe(RoutingInspectionResult& result,
                           const RoutingInspectionOptions& options)
{
    auto& probe = result.runtimeRenderProbe;
    probe.requested = options.runtimeProbe;
    probe.startSeconds = options.probeStartSeconds;
    if (!probe.requested)
        return;

    SingleTrackDocument document;
    AudioEngine engine(document);
    if (!document.loadFile(result.input))
    {
        probe.errorMessage = "Could not load the document playback source";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }

    if (options.sourcePresetId.isNotEmpty()
        && !document.setChannelLayoutPreset(options.sourcePresetId))
    {
        probe.errorMessage = "Could not apply the requested source preset";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }
    if (options.ambisonicOrientationSpecified
        && !document.setAmbisonicOrientation(options.ambisonicOrientation))
    {
        probe.errorMessage = "Could not apply the requested Ambisonic orientation";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }
    if (result.sourceLayout.isAmbisonic
        && !document.setAmbisonicMonitorMode(options.ambisonicMonitorMode))
    {
        probe.errorMessage = "Could not apply the requested Ambisonic monitor mode";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }

    engine.setIncludeLfeInDownmix(result.playbackState.includeLfeInDownmix);
    engine.setMonitorMode(result.playbackState.monitorMode);
    engine.setOutputMapping(result.playbackState.outputMapping);
    for (int sourceChannel = 0; sourceChannel < result.sourceLayout.sourceChannelCount; ++sourceChannel)
    {
        engine.setSourceChannelMuted(
            sourceChannel,
            result.playbackState.mutedBySourceChannel[static_cast<std::size_t>(sourceChannel)]);
        engine.setSourceChannelSoloed(
            sourceChannel,
            result.playbackState.soloedBySourceChannel[static_cast<std::size_t>(sourceChannel)]);
    }

    AudioEngine::AmbisonicFieldAnalysisLease fieldAnalysisLease;
    if (document.getState()->info.channelLayout.isAmbisonic)
        fieldAnalysisLease = engine.acquireAmbisonicFieldAnalysisLease();

    const auto sampleRate = document.getSampleRate();
    const auto totalSamples = document.getNumSamples();
    const auto durationSeconds = document.getDurationSeconds();
    if (sampleRate <= 0.0 || totalSamples <= 0 || options.probeStartSeconds < 0.0
        || options.probeStartSeconds >= durationSeconds)
    {
        probe.errorMessage = "Probe start is outside the playable file range";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }

    const auto requestedStart = static_cast<int64_t>(std::llround(
        options.probeStartSeconds * sampleRate));
    probe.startSample = requestedStart;
    probe.sampleCount = static_cast<int>(juce::jmin<int64_t>(4096,
                                                             totalSamples - requestedStart));
    probe.renderPath = "transport";
    probe.transportChannelCapacity = engine.getTransportChannelCapacityForDiagnostics();
    juce::AudioBuffer<float> rendered(result.deviceOutputLayout.sourceChannelCount,
                                      probe.sampleCount);
    if (!engine.renderTransportBlockForDiagnostics(probe.startSample, rendered))
    {
        probe.errorMessage = "AudioEngine rejected the diagnostic render";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
        return;
    }
    probe.ambisonicField = engine.getAmbisonicFieldSnapshot();

    probe.channels.reserve(static_cast<std::size_t>(rendered.getNumChannels()));
    for (int channel = 0; channel < rendered.getNumChannels(); ++channel)
    {
        auto role = SpeakerRole::discrete;
        if (juce::isPositiveAndBelow(
                channel,
                static_cast<int>(result.deviceOutputLayout.selectedRoles.size())))
        {
            role = result.deviceOutputLayout.selectedRoles[static_cast<std::size_t>(channel)];
        }

        RuntimeRenderProbeChannel channelResult;
        channelResult.role = role;
        channelResult.peakLinear = rendered.getMagnitude(channel, 0, rendered.getNumSamples());
        channelResult.rmsLinear = rendered.getRMSLevel(channel, 0, rendered.getNumSamples());
        probe.finite = probe.finite
                    && std::isfinite(channelResult.peakLinear)
                    && std::isfinite(channelResult.rmsLinear);
        for (int sample = 0; sample < rendered.getNumSamples() && probe.finite; ++sample)
            probe.finite = std::isfinite(rendered.getSample(channel, sample));
        probe.channels.push_back(channelResult);
    }

    probe.succeeded = probe.finite;
    if (!probe.succeeded)
    {
        probe.errorMessage = "AudioEngine produced non-finite output";
        result.errorMessage = "Runtime render probe failed: " + probe.errorMessage;
    }
}
} // namespace

bool RoutingInspectionResult::succeeded() const noexcept
{
    return opened
        && errorMessage.isEmpty()
        && matrixFinite
        && outputMappingValid
        && (!runtimeRenderProbe.requested || runtimeRenderProbe.succeeded)
        && unexpectedUnroutedSourceChannels.empty();
}

juce::var RoutingInspectionResult::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("input", input.getFullPathName());
    root->setProperty("opened", opened);
    root->setProperty("ok", succeeded());
    root->setProperty("error",
                      errorMessage.isEmpty() ? juce::var() : juce::var(errorMessage));
    root->setProperty("matrix_finite", matrixFinite);
    root->setProperty("output_mapping_valid", outputMappingValid);
    root->setProperty("output_mapping_enabled", playbackState.outputMapping.enabled);
    root->setProperty("include_lfe", playbackState.includeLfeInDownmix);
    root->setProperty("monitor_mode", static_cast<int>(playbackState.monitorMode));
    root->setProperty("routing_contract_version", 1);
    root->setProperty("downmix_profile", ChannelRouter::downmixProfileId());
    auto sourceLayoutJson = layoutToJson(sourceLayout,
                                         ambisonicStageGraphActive,
                                         ambisonicOrientation);
    if (auto* sourceLayoutObject = sourceLayoutJson.getDynamicObject();
        sourceLayoutObject != nullptr && sourceLayout.isAmbisonic
        && ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
    {
        sourceLayoutObject->setProperty("monitor_decode_mode", "binaural_hrtf");
        sourceLayoutObject->setProperty("monitor_decode_runtime_state",
                                        ambisonicBinauralRendererActive ? "active" : "unavailable");
    }
    root->setProperty("source_layout", sourceLayoutJson);
    root->setProperty("target_layout", layoutToJson(targetLayout, false, {}));
    root->setProperty("device_output_layout", layoutToJson(deviceOutputLayout, false, {}));
    root->setProperty("routing_domain",
                      ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf
                          ? "binaural_convolution_with_speaker_reference_matrix"
                          : "speaker_matrix");

    auto* decodeMatrix = new juce::DynamicObject();
    const auto decodeMatrixRequired = sourceLayout.isAmbisonic;
    decodeMatrix->setProperty(
        "state",
        !decodeMatrixRequired
            ? "not_required"
            : !ambisonicDecodeMatrixAvailable
                ? "unavailable"
                : ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf
                    ? "speaker_reference"
                    : "active");
    decodeMatrix->setProperty(
        "kind",
        !decodeMatrixRequired
            ? "not_required"
            : !ambisonicDecodeMatrixAvailable
                ? "unavailable"
                : ambisonicDecodeMatrixUsesCustomGeometry
                    ? "custom_runtime"
                    : "standard_offline");
    decodeMatrix->setProperty(
        "artifact_schema_version",
        ambisonicDecodeMatrixAvailable
            ? juce::var(AmbisonicDecodeMatrixArtifact::schemaVersion)
            : juce::var());
    decodeMatrix->setProperty(
        "artifact_id",
        ambisonicDecodeMatrixAvailable
            ? juce::var(ambisonicDecodeMatrixArtifactId)
            : juce::var());
    decodeMatrix->setProperty(
        "geometry_profile_id",
        ambisonicDecodeMatrixAvailable
            ? juce::var(ambisonicGeometryProfileId)
            : juce::var());
    decodeMatrix->setProperty(
        "geometry_profile_revision",
        ambisonicDecodeMatrixAvailable
            ? juce::var(static_cast<juce::int64>(ambisonicGeometryProfileRevision))
            : juce::var());
    decodeMatrix->setProperty(
        "geometry_profile_hash",
        ambisonicDecodeMatrixAvailable
            ? juce::var(ambisonicGeometryProfileHash)
            : juce::var());
    decodeMatrix->setProperty(
        "cache_key",
        ambisonicDecodeMatrixAvailable
            ? juce::var(ambisonicDecodeMatrixCacheKey)
            : juce::var());
    decodeMatrix->setProperty(
        "generated_offline",
        ambisonicDecodeMatrixAvailable
            ? juce::var(ambisonicDecodeMatrixGeneratedOffline)
            : juce::var());
    decodeMatrix->setProperty("audio_device_required", false);
    decodeMatrix->setProperty("contains_physical_output_binding", false);
    root->setProperty("ambisonic_decode_matrix", juce::var(decodeMatrix));

    auto* meterTopology = new juce::DynamicObject();
    meterTopology->setProperty("sampling_point", "final_device_output_after_ping");
    meterTopology->setProperty("per_channel_loudness", "k_weighted_contribution_db");
    meterTopology->setProperty("global_loudness", "bs1770_5_momentary_400ms_lufs");
    meterTopology->setProperty("channel_count", deviceOutputLayout.sourceChannelCount);
    juce::Array<juce::var> meterChannels;
    for (int outputChannel = 0; outputChannel < deviceOutputLayout.sourceChannelCount; ++outputChannel)
    {
        const auto defaultRole = juce::isPositiveAndBelow(
                                     outputChannel,
                                     static_cast<int>(deviceOutputLayout.selectedRoles.size()))
                               ? deviceOutputLayout.selectedRoles[static_cast<std::size_t>(outputChannel)]
                               : SpeakerRole::discrete;
        auto meterRole = semanticRoleForOutput(deviceOutputLayout,
                                               playbackState.outputMapping,
                                               outputChannel);
        if (meterRole == SpeakerRole::unknown)
            meterRole = defaultRole;

        auto* channel = new juce::DynamicObject();
        channel->setProperty("output_channel", outputChannel + 1);
        channel->setProperty("role", ChannelLayoutCatalog::roleToAbbreviation(meterRole));
        const auto weight = OutputMeterAnalyzer::loudnessWeightForRole(meterRole);
        channel->setProperty("included_in_global_lufs", weight > 0.0);
        channel->setProperty("position_weight", weight);
        meterChannels.add(juce::var(channel));
    }
    meterTopology->setProperty("channels", juce::var(meterChannels));
    root->setProperty("output_meter", juce::var(meterTopology));

    auto* runtimeProbe = new juce::DynamicObject();
    runtimeProbe->setProperty("requested", runtimeRenderProbe.requested);
    runtimeProbe->setProperty("ok",
                              runtimeRenderProbe.requested
                                  ? juce::var(runtimeRenderProbe.succeeded)
                                  : juce::var());
    runtimeProbe->setProperty("finite", runtimeRenderProbe.finite);
    runtimeProbe->setProperty("audio_device_required", false);
    runtimeProbe->setProperty("start_seconds", runtimeRenderProbe.startSeconds);
    runtimeProbe->setProperty("start_sample", runtimeRenderProbe.startSample);
    runtimeProbe->setProperty("sample_count", runtimeRenderProbe.sampleCount);
    runtimeProbe->setProperty("render_path", runtimeRenderProbe.renderPath);
    runtimeProbe->setProperty("transport_channel_capacity",
                              runtimeRenderProbe.transportChannelCapacity);
    runtimeProbe->setProperty("channel_count",
                              static_cast<int>(runtimeRenderProbe.channels.size()));
    runtimeProbe->setProperty("error",
                              runtimeRenderProbe.errorMessage.isEmpty()
                                  ? juce::var()
                                  : juce::var(runtimeRenderProbe.errorMessage));
    juce::Array<juce::var> probeChannels;
    for (int channelIndex = 0;
         channelIndex < static_cast<int>(runtimeRenderProbe.channels.size());
         ++channelIndex)
    {
        const auto& probeChannel = runtimeRenderProbe.channels[static_cast<std::size_t>(channelIndex)];
        auto* channel = new juce::DynamicObject();
        channel->setProperty("output_channel", channelIndex + 1);
        channel->setProperty("role", ChannelLayoutCatalog::roleToAbbreviation(probeChannel.role));
        channel->setProperty("peak_linear", probeChannel.peakLinear);
        channel->setProperty("rms_linear", probeChannel.rmsLinear);
        channel->setProperty("active", probeChannel.peakLinear > 0.000001f);
        probeChannels.add(juce::var(channel));
    }
    runtimeProbe->setProperty("channels", juce::var(probeChannels));

    auto* field = new juce::DynamicObject();
    field->setProperty("valid", runtimeRenderProbe.ambisonicField.valid);
    field->setProperty("order", runtimeRenderProbe.ambisonicField.order);
    field->setProperty("channel_count", runtimeRenderProbe.ambisonicField.channelCount);
    field->setProperty("front_energy",
                       AmbisonicFieldAnalyzer::energyAt(runtimeRenderProbe.ambisonicField,
                                                        0.0f,
                                                        0.0f));
    field->setProperty("left_energy",
                       AmbisonicFieldAnalyzer::energyAt(runtimeRenderProbe.ambisonicField,
                                                        90.0f,
                                                        0.0f));
    field->setProperty("right_energy",
                       AmbisonicFieldAnalyzer::energyAt(runtimeRenderProbe.ambisonicField,
                                                        -90.0f,
                                                        0.0f));
    field->setProperty("rear_energy",
                       AmbisonicFieldAnalyzer::energyAt(runtimeRenderProbe.ambisonicField,
                                                        180.0f,
                                                        0.0f));
    auto dominantEnergy = 0.0f;
    auto dominantAzimuth = 0;
    auto dominantElevation = 0;
    if (runtimeRenderProbe.ambisonicField.valid)
    {
        for (int elevation = -75; elevation <= 75; elevation += 15)
        {
            for (int azimuth = -180; azimuth < 180; azimuth += 15)
            {
                const auto energy = AmbisonicFieldAnalyzer::energyAt(
                    runtimeRenderProbe.ambisonicField,
                    static_cast<float>(azimuth),
                    static_cast<float>(elevation));
                if (energy > dominantEnergy)
                {
                    dominantEnergy = energy;
                    dominantAzimuth = azimuth;
                    dominantElevation = elevation;
                }
            }
        }
    }
    field->setProperty("dominant_energy", dominantEnergy);
    field->setProperty("dominant_azimuth_degrees", dominantAzimuth);
    field->setProperty("dominant_elevation_degrees", dominantElevation);
    runtimeProbe->setProperty("ambisonic_field", juce::var(field));
    root->setProperty("runtime_render_probe", juce::var(runtimeProbe));

    if (ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
    {
        auto* resource = new juce::DynamicObject();
        resource->setProperty("ready", ambisonicBinauralFilterBank != nullptr);
        resource->setProperty("error",
                              ambisonicBinauralError.isEmpty()
                                  ? juce::var()
                                  : juce::var(ambisonicBinauralError));
        if (ambisonicBinauralFilterBank != nullptr)
        {
            resource->setProperty("identity",
                                  ambisonicBinauralFilterBank->getResourceIdentity());
            resource->setProperty("source_md5",
                                  ambisonicBinauralFilterBank->getSourceMd5());
            resource->setProperty("sample_rate",
                                  ambisonicBinauralFilterBank->getSampleRate());
            resource->setProperty("tap_count",
                                  ambisonicBinauralFilterBank->getTapCount());
        }
        root->setProperty("binaural_resource", juce::var(resource));

        auto* selfTest = new juce::DynamicObject();
        selfTest->setProperty("passed", ambisonicBinauralSelfTestPassed);
        selfTest->setProperty("left_energy", ambisonicBinauralLeftEnergy);
        selfTest->setProperty("right_energy", ambisonicBinauralRightEnergy);
        selfTest->setProperty("audio_device_required", false);
        root->setProperty("binaural_self_test", juce::var(selfTest));
    }

    juce::Array<juce::var> routes;
    for (int sourceChannel = 0; sourceChannel < matrix.sourceChannels; ++sourceChannel)
    {
        const auto sourceRole = juce::isPositiveAndBelow(
                                    sourceChannel,
                                    static_cast<int>(sourceLayout.selectedRoles.size()))
                              ? sourceLayout.selectedRoles[static_cast<size_t>(sourceChannel)]
                              : SpeakerRole::discrete;
        auto* route = new juce::DynamicObject();
        route->setProperty("source_channel", sourceChannel + 1);
        route->setProperty("source_role", ChannelLayoutCatalog::roleToAbbreviation(sourceRole));

        juce::Array<juce::var> outputs;
        for (int outputChannel = 0; outputChannel < matrix.outputChannels; ++outputChannel)
        {
            const auto gain = matrix.gain(outputChannel, sourceChannel);
            if (std::abs(gain) <= 0.000001f)
                continue;

            const auto deviceDefaultRole = juce::isPositiveAndBelow(
                                        outputChannel,
                                        static_cast<int>(targetLayout.selectedRoles.size()))
                                  ? targetLayout.selectedRoles[static_cast<size_t>(outputChannel)]
                                  : SpeakerRole::discrete;
            const auto outputRole = semanticRoleForOutput(targetLayout,
                                                          playbackState.outputMapping,
                                                          outputChannel);
            auto* output = new juce::DynamicObject();
            output->setProperty("output_channel", outputChannel + 1);
            output->setProperty("output_role", ChannelLayoutCatalog::roleToAbbreviation(outputRole));
            output->setProperty("device_default_role",
                                ChannelLayoutCatalog::roleToAbbreviation(deviceDefaultRole));
            output->setProperty("gain", gain);
            output->setProperty("gain_db", 20.0 * std::log10(std::abs(static_cast<double>(gain))));
            outputs.add(juce::var(output));
        }

        const auto expectedDrop = outputs.isEmpty()
                               && isExpectedDrop(sourceRole, targetLayout, playbackState);
        const auto expectedSilence = outputs.isEmpty()
                                  && !ChannelRouter::sourceChannelIsAudible(sourceChannel,
                                                                            playbackState);
        route->setProperty("routed", !outputs.isEmpty());
        route->setProperty("expected_drop", expectedDrop);
        route->setProperty("expected_silence", expectedSilence);
        route->setProperty("outputs", juce::var(outputs));
        routes.add(juce::var(route));
    }
    root->setProperty("routes", juce::var(routes));

    auto* physicalMatrix = new juce::DynamicObject();
    physicalMatrix->setProperty("schema_version", PhysicalOutputMatrix::schemaVersion);
    physicalMatrix->setProperty("active_output_channels",
                                physicalOutputMatrix.activeOutputChannels);
    physicalMatrix->setProperty("target_preset_id", physicalOutputMatrix.targetPresetId);
    physicalMatrix->setProperty("valid", physicalOutputMatrix.valid);
    juce::Array<juce::var> physicalRoutes;
    for (const auto& route : physicalOutputMatrix.routes)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty("role", ChannelLayoutCatalog::roleToAbbreviation(route.role));
        entry->setProperty("canonical_channel", route.canonicalChannel + 1);
        entry->setProperty("physical_output_channel", route.physicalOutputChannel + 1);
        physicalRoutes.add(juce::var(entry));
    }
    physicalMatrix->setProperty("routes", juce::var(physicalRoutes));
    root->setProperty("physical_output_matrix", juce::var(physicalMatrix));

    auto* silentProbe = new juce::DynamicObject();
    silentProbe->setProperty("schema_version", SilentRouteProbe::schemaVersion);
    silentProbe->setProperty("method", "unit_impulse_matrix");
    silentProbe->setProperty("audio_device_required", false);
    silentProbe->setProperty("audible_output_emitted", false);
    juce::Array<juce::var> rolePings;
    for (const auto& probe : silentRouteProbes)
    {
        auto* ping = new juce::DynamicObject();
        ping->setProperty("source_channel", probe.sourceChannel + 1);
        ping->setProperty("source_role",
                          ChannelLayoutCatalog::roleToAbbreviation(probe.sourceRole));
        ping->setProperty("finite", probe.finite);
        ping->setProperty("routed", probe.routed);
        juce::Array<juce::var> pingOutputs;
        for (const auto& output : probe.outputs)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty("target_role",
                               ChannelLayoutCatalog::roleToAbbreviation(output.targetRole));
            entry->setProperty("physical_output_channel", output.physicalOutputChannel + 1);
            entry->setProperty("gain", output.gain);
            pingOutputs.add(juce::var(entry));
        }
        ping->setProperty("outputs", juce::var(pingOutputs));
        rolePings.add(juce::var(ping));
    }
    silentProbe->setProperty("role_pings", juce::var(rolePings));
    root->setProperty("silent_route_probe", juce::var(silentProbe));

    juce::Array<juce::var> outputMapping;
    for (int targetChannel = 0;
         targetChannel < static_cast<int>(targetLayout.selectedRoles.size());
         ++targetChannel)
    {
        const auto role = targetLayout.selectedRoles[static_cast<std::size_t>(targetChannel)];
        auto* entry = new juce::DynamicObject();
        entry->setProperty("speaker_role", ChannelLayoutCatalog::roleToAbbreviation(role));
        entry->setProperty("device_default_channel", targetChannel + 1);
        entry->setProperty("output_channel",
                           ChannelRouter::outputChannelForRole(targetLayout,
                                                               playbackState.outputMapping,
                                                               role) + 1);
        outputMapping.add(juce::var(entry));
    }
    root->setProperty("output_mapping", juce::var(outputMapping));

    juce::Array<juce::var> unrouted;
    for (const auto sourceChannel : unexpectedUnroutedSourceChannels)
        unrouted.add(sourceChannel + 1);
    root->setProperty("unexpected_unrouted_source_channels", juce::var(unrouted));
    return juce::var(root);
}

RoutingInspectionResult RoutingInspectionService::inspect(const juce::File& file,
                                                           RoutingInspectionOptions options)
{
    RoutingInspectionResult result;
    result.input = file;
    const auto inspection = AudioInspectionService::inspect(file);
    result.opened = inspection.opened;
    if (!inspection.opened || inspection.errorMessage.isNotEmpty())
    {
        result.errorMessage = inspection.errorMessage.isNotEmpty()
                            ? inspection.errorMessage
                            : "Input could not be opened";
        return result;
    }

    result.sourceLayout = inspection.info.channelLayout;
    result.ambisonicMonitorMode = options.ambisonicMonitorMode;
    const auto requestedPreset = options.sourcePresetId.trim();
    if (requestedPreset.isNotEmpty() && requestedPreset != "auto")
    {
        const auto explicitPreset = ChannelLayoutCatalog::layoutForPresetId(
            inspection.info.numChannels,
            requestedPreset);
        if (explicitPreset.selectedPresetId != requestedPreset)
        {
            result.errorMessage = "Unknown source preset for "
                                + juce::String(inspection.info.numChannels)
                                + " channels: " + requestedPreset;
            return result;
        }
        result.sourceLayout = explicitPreset.isAmbisonic
                            ? explicitPreset
                            : ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(
                                  inspection.info.numChannels,
                                  result.sourceLayout.waveChannelMask,
                                  requestedPreset);
    }

    if (options.ambisonicOrientationSpecified && !result.sourceLayout.isAmbisonic)
    {
        result.errorMessage = "Ambisonic orientation requires an Ambisonic source preset";
        return result;
    }
    if (options.ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf
        && !result.sourceLayout.isAmbisonic)
    {
        result.errorMessage = "Ambisonic binaural monitoring requires an Ambisonic source preset";
        return result;
    }
    result.ambisonicOrientation = options.ambisonicOrientationSpecified
                                ? AmbisonicMonitorDecoder::canonicalizeOrientation(
                                      options.ambisonicOrientation)
                                : AmbisonicOrientation {};

    result.deviceOutputLayout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(
        options.outputChannels);
    if (result.deviceOutputLayout.sourceChannelCount <= 0)
    {
        result.errorMessage = "Unsupported output channel count: "
                            + juce::String(options.outputChannels);
        return result;
    }

    result.targetLayout = options.ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf
                        || options.monitorMode == MonitorMode::forceStereoDownmix
                        ? ChannelLayoutCatalog::stereoOutputLayout()
                        : result.deviceOutputLayout;

    result.playbackState.includeLfeInDownmix = options.includeLfe;
    result.playbackState.monitorMode = options.monitorMode;
    for (const auto sourceChannel : options.mutedSourceChannels)
    {
        if (!juce::isPositiveAndBelow(sourceChannel, result.sourceLayout.sourceChannelCount))
        {
            result.errorMessage = "Mute source channel is outside the input layout";
            return result;
        }
        result.playbackState.mutedBySourceChannel[static_cast<std::size_t>(sourceChannel)] = true;
    }
    for (const auto sourceChannel : options.soloedSourceChannels)
    {
        if (!juce::isPositiveAndBelow(sourceChannel, result.sourceLayout.sourceChannelCount))
        {
            result.errorMessage = "Solo source channel is outside the input layout";
            return result;
        }
        result.playbackState.soloedBySourceChannel[static_cast<std::size_t>(sourceChannel)] = true;
    }

    for (const auto& mappingText : options.outputMappings)
    {
        const auto roleText = mappingText.upToFirstOccurrenceOf("=", false, false).trim();
        const auto outputText = mappingText.fromFirstOccurrenceOf("=", false, false).trim();
        const auto role = ChannelLayoutCatalog::roleFromAbbreviation(roleText);
        const auto outputChannel = outputText.containsOnly("0123456789")
                                 ? outputText.getIntValue() - 1
                                 : -1;
        if (role == SpeakerRole::unknown
            || !targetHasRole(result.targetLayout, role)
            || !juce::isPositiveAndBelow(outputChannel, result.targetLayout.sourceChannelCount)
            || !result.playbackState.outputMapping.setOutputChannel(role, outputChannel))
        {
            result.errorMessage = "Invalid output mapping: " + mappingText;
            return result;
        }
    }

    result.outputMappingValid = ChannelRouter::outputMappingIsValid(
        result.targetLayout,
        result.playbackState.outputMapping);
    if (!result.outputMappingValid)
    {
        result.errorMessage = "Output mapping contains duplicate or invalid device outputs";
        return result;
    }
    result.physicalOutputMatrix = ChannelRouter::buildPhysicalOutputMatrix(
        result.targetLayout,
        result.playbackState.outputMapping);

    const auto ambisonicStageGraph = AmbisonicStageGraph::create(
        result.sourceLayout,
        result.ambisonicOrientation,
        0,
        options.customDecodeArtifact);
    result.ambisonicStageGraphActive = ambisonicStageGraph != nullptr;
    if (options.customDecodeArtifact != nullptr && ambisonicStageGraph == nullptr)
    {
        result.errorMessage = "Custom Ambisonic decode matrix artifact was rejected";
        return result;
    }
    if (ambisonicStageGraph != nullptr)
    {
        result.ambisonicDecodeMatrixArtifactId =
            ambisonicStageGraph->matrixArtifactIdFor(result.targetLayout);
        result.ambisonicGeometryProfileId =
            ambisonicStageGraph->geometryProfileIdFor(result.targetLayout);
        result.ambisonicGeometryProfileHash =
            ambisonicStageGraph->geometryProfileHashFor(result.targetLayout);
        result.ambisonicDecodeMatrixCacheKey =
            ambisonicStageGraph->matrixCacheKeyFor(result.targetLayout);
        result.ambisonicDecodeMatrixAvailable =
            result.ambisonicDecodeMatrixArtifactId.isNotEmpty()
            && result.ambisonicGeometryProfileId.isNotEmpty()
            && result.ambisonicGeometryProfileHash.isNotEmpty()
            && result.ambisonicDecodeMatrixCacheKey.isNotEmpty();
        result.ambisonicDecodeMatrixUsesCustomGeometry =
            ambisonicStageGraph->usesCustomSpeakerGeometry();
        if (result.ambisonicDecodeMatrixUsesCustomGeometry)
        {
            result.ambisonicGeometryProfileRevision =
                options.customDecodeArtifact->geometry.revision;
            result.ambisonicDecodeMatrixGeneratedOffline =
                options.customDecodeArtifact->generatedOffline;
        }
        else if (const auto profile = AmbisonicStandardMatrixCatalog::standardProfile(
                     result.ambisonicGeometryProfileId))
        {
            result.ambisonicGeometryProfileRevision = profile->revision;
            result.ambisonicDecodeMatrixGeneratedOffline = true;
        }
    }
    if (options.ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
    {
        juce::String error;
        result.ambisonicBinauralFilterBank = AmbisonicBinauralFilterBank::loadDefault(error);
        result.ambisonicBinauralError = error;
        if (result.ambisonicBinauralFilterBank == nullptr)
        {
            result.errorMessage = "Ambisonic binaural resource is unavailable: " + error;
            return result;
        }

        auto renderer = AmbisonicBinauralRenderer::create(
            result.ambisonicBinauralFilterBank,
            result.sourceLayout.ambisonicOrder,
            result.ambisonicOrientation,
            48000.0,
            512,
            error);
        if (renderer == nullptr)
        {
            result.ambisonicBinauralError = error;
            result.errorMessage = "Ambisonic binaural renderer could not start: " + error;
            return result;
        }

        const auto sourceChannels = (result.sourceLayout.ambisonicOrder + 1)
                                  * (result.sourceLayout.ambisonicOrder + 1);
        juce::AudioBuffer<float> source(sourceChannels, 512);
        juce::AudioBuffer<float> rendered(2, 512);
        source.clear();
        rendered.clear();
        source.setSample(0, 0, 1.0f);
        if (sourceChannels > 3)
            source.setSample(3, 0, 1.0f);
        renderer->process(source, 0, rendered, 0, 512, result.playbackState);
        for (auto sample = 0; sample < rendered.getNumSamples(); ++sample)
        {
            const auto left = static_cast<double>(rendered.getSample(0, sample));
            const auto right = static_cast<double>(rendered.getSample(1, sample));
            result.ambisonicBinauralLeftEnergy += left * left;
            result.ambisonicBinauralRightEnergy += right * right;
        }
        result.ambisonicBinauralSelfTestPassed =
            std::isfinite(result.ambisonicBinauralLeftEnergy)
            && std::isfinite(result.ambisonicBinauralRightEnergy)
            && result.ambisonicBinauralLeftEnergy > 0.000001
            && result.ambisonicBinauralRightEnergy > 0.000001;
        result.ambisonicBinauralRendererActive = result.ambisonicBinauralSelfTestPassed;
        if (!result.ambisonicBinauralSelfTestPassed)
        {
            result.errorMessage = "Ambisonic binaural renderer self-test produced invalid output";
            return result;
        }
    }
    result.matrix = ambisonicStageGraph != nullptr
                  ? ambisonicStageGraph->buildPhysicalRoutingMatrix(result.targetLayout,
                                                                     result.playbackState)
                  : ChannelRouter::buildRoutingMatrix(result.sourceLayout,
                                                       result.targetLayout,
                                                       result.playbackState);
    result.silentRouteProbes = ChannelRouter::buildSilentRouteProbes(
        result.sourceLayout,
        result.physicalOutputMatrix,
        result.matrix);
    for (int sourceChannel = 0; sourceChannel < result.matrix.sourceChannels; ++sourceChannel)
    {
        auto routed = false;
        for (int outputChannel = 0; outputChannel < result.matrix.outputChannels; ++outputChannel)
        {
            const auto gain = result.matrix.gain(outputChannel, sourceChannel);
            result.matrixFinite = result.matrixFinite && std::isfinite(gain);
            routed = routed || std::abs(gain) > 0.000001f;
        }

        const auto sourceRole = juce::isPositiveAndBelow(
                                    sourceChannel,
                                    static_cast<int>(result.sourceLayout.selectedRoles.size()))
                              ? result.sourceLayout.selectedRoles[static_cast<size_t>(sourceChannel)]
                              : SpeakerRole::discrete;
        if (!routed
            && ChannelRouter::sourceChannelIsAudible(sourceChannel, result.playbackState)
            && !isExpectedDrop(sourceRole, result.targetLayout, result.playbackState))
            result.unexpectedUnroutedSourceChannels.push_back(sourceChannel);
    }

    if (!result.matrixFinite)
        result.errorMessage = "Routing matrix contains non-finite gains";
    else if (!result.unexpectedUnroutedSourceChannels.empty())
        result.errorMessage = "One or more source channels are unexpectedly unrouted";
    else
        runRuntimeRenderProbe(result, options);
    return result;
}
} // namespace singleeditor
