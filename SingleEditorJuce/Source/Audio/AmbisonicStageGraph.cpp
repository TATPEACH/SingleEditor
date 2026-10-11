#include "Audio/AmbisonicStageGraph.h"

#include "Audio/ChannelRouter.h"

namespace singleeditor
{
namespace
{
RoutingMatrix multiplyRoutingMatrices(const RoutingMatrix& destinationFromIntermediate,
                                      const RoutingMatrix& intermediateFromSource) noexcept
{
    RoutingMatrix result;
    result.sourceChannels = intermediateFromSource.sourceChannels;
    result.outputChannels = destinationFromIntermediate.outputChannels;
    result.gains.fill(0.0f);

    const auto intermediateChannels = juce::jmin(destinationFromIntermediate.sourceChannels,
                                                  intermediateFromSource.outputChannels);
    for (auto output = 0; output < result.outputChannels; ++output)
    {
        for (auto source = 0; source < result.sourceChannels; ++source)
        {
            auto gain = 0.0f;
            for (auto intermediate = 0; intermediate < intermediateChannels; ++intermediate)
            {
                gain += destinationFromIntermediate.gain(output, intermediate)
                      * intermediateFromSource.gain(intermediate, source);
            }
            result.gain(output, source) = gain;
        }
    }
    return result;
}
} // namespace

AmbisonicStageGraph::AmbisonicStageGraph(
    std::shared_ptr<const AmbisonicMonitorDecoder> decoderToUse,
    int effectiveOrderToUse,
    int sourceChannelCountToUse,
    AmbisonicOrientation orientationToUse,
    std::uint64_t bindingGenerationToUse,
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customArtifactToUse)
    : decoder(std::move(decoderToUse)),
      customArtifact(std::move(customArtifactToUse)),
      effectiveOrder(effectiveOrderToUse),
      sourceChannelCount(sourceChannelCountToUse),
      orientation(orientationToUse),
      bindingGeneration(bindingGenerationToUse),
      decoderProfileId(AmbisonicMonitorDecoder::decoderProfileId())
{
}

std::shared_ptr<const AmbisonicStageGraph> AmbisonicStageGraph::create(
    const ChannelLayoutState& sourceLayout,
    AmbisonicOrientation orientationToUse,
    std::uint64_t bindingGenerationToUse,
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customArtifactToUse)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto lifetime = OptionalLifecycleToken::request(currentOptionalLifecycleContext(),
        OptionalLifecycleOwner::ambisonics, OptionalLifecycleResourceKind::ambisonicStageGraph);
    try
    {
#endif
    auto nextDecoder = AmbisonicMonitorDecoder::create(sourceLayout, orientationToUse);
    if (nextDecoder == nullptr)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        lifetime.failed();
#endif
        return {};
    }
    if (customArtifactToUse != nullptr
        && (!customArtifactToUse->isValid()
            || customArtifactToUse->ambisonicOrder != sourceLayout.ambisonicOrder
            || !customArtifactToUse->channelOrdering.equalsIgnoreCase("ACN")
            || !customArtifactToUse->ambisonicNormalization.equalsIgnoreCase("SN3D")))
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        lifetime.failed();
#endif
        return {};
    }

    orientationToUse = AmbisonicMonitorDecoder::canonicalizeOrientation(orientationToUse);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto result = std::shared_ptr<const AmbisonicStageGraph>(
#else
    return std::shared_ptr<const AmbisonicStageGraph>(
#endif
        new AmbisonicStageGraph(std::move(nextDecoder),
                                sourceLayout.ambisonicOrder,
                                sourceLayout.sourceChannelCount,
                                orientationToUse,
                                bindingGenerationToUse,
                                std::move(customArtifactToUse)));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    lifetime.created();
    result->observedLifetime.token = std::move(lifetime);
    return result;
    }
    catch (...)
    {
        lifetime.failed();
        throw;
    }
#endif
}

juce::String AmbisonicStageGraph::stageOrderId()
{
    return "validate-convert>rotate>analyze>decode-max-re>canonical-speaker-bus>role-mapping>physical-device>meter";
}

bool AmbisonicStageGraph::matchesCanonicalBus(const DeviceRoutePlan::Bus& bus) const noexcept
{
    if (!customArtifact) return true; // standard decoder consumes the explicit target layout
    const auto& geometry = customArtifact->geometry;
    if (bus.profileId != geometry.profileId.toRawUTF8() || bus.revision != geometry.revision) return false;
    std::size_t row = 0;
    for (const auto& speaker : geometry.speakers)
    {
        if (!speaker.enabled || !speaker.available) continue;
        if (row >= bus.speakers.size() || bus.speakers[row].stableId != speaker.stableId.toRawUTF8()
            || bus.speakers[row].role != speaker.role) return false;
        ++row;
    }
    return row == bus.speakers.size();
}

RoutingMatrix AmbisonicStageGraph::buildCanonicalSpeakerBusMatrix(
    const ChannelLayoutState& standardSpeakerBus,
    const ChannelPlaybackState& playbackState) const noexcept
{
    if (decoder == nullptr)
        return {};

    auto decodeState = playbackState;
    decodeState.outputMapping.clear();
    return customArtifact != nullptr
         ? decoder->buildRoutingMatrix(*customArtifact, decodeState)
         : decoder->buildRoutingMatrix(standardSpeakerBus, decodeState);
}

RoutingMatrix AmbisonicStageGraph::buildPhysicalRoutingMatrix(
    const ChannelLayoutState& physicalTargetLayout,
    const ChannelPlaybackState& playbackState) const noexcept
{
    const auto canonicalSpeakerBus = customArtifact != nullptr
                                   ? customArtifact->canonicalSpeakerBus()
                                   : physicalTargetLayout;
    const auto canonicalMatrix = buildCanonicalSpeakerBusMatrix(physicalTargetLayout,
                                                                 playbackState);

    ChannelPlaybackState roleMappingState;
    roleMappingState.includeLfeInDownmix = playbackState.includeLfeInDownmix;
    roleMappingState.monitorMode = playbackState.monitorMode;
    roleMappingState.outputMapping = playbackState.outputMapping;
    const auto roleMapping = ChannelRouter::buildRoutingMatrix(canonicalSpeakerBus,
                                                                physicalTargetLayout,
                                                                roleMappingState);
    return multiplyRoutingMatrices(roleMapping, canonicalMatrix);
}

juce::String AmbisonicStageGraph::matrixArtifactIdFor(
    const ChannelLayoutState& standardSpeakerBus) const
{
    if (customArtifact != nullptr)
        return customArtifact->artifactId;
    const auto* artifact = decoder != nullptr
                         ? decoder->standardArtifactFor(standardSpeakerBus) : nullptr;
    return artifact != nullptr ? juce::String(artifact->artifactId) : juce::String();
}

juce::String AmbisonicStageGraph::geometryProfileIdFor(
    const ChannelLayoutState& standardSpeakerBus) const
{
    if (customArtifact != nullptr)
        return customArtifact->geometry.profileId;
    const auto* artifact = decoder != nullptr
                         ? decoder->standardArtifactFor(standardSpeakerBus) : nullptr;
    return artifact != nullptr ? juce::String(artifact->profileId) : juce::String();
}

juce::String AmbisonicStageGraph::geometryProfileHashFor(
    const ChannelLayoutState& standardSpeakerBus) const
{
    if (customArtifact != nullptr)
        return customArtifact->profileHash;
    const auto* artifact = decoder != nullptr
                         ? decoder->standardArtifactFor(standardSpeakerBus) : nullptr;
    return artifact != nullptr ? juce::String(artifact->profileHash) : juce::String();
}

juce::String AmbisonicStageGraph::matrixCacheKeyFor(
    const ChannelLayoutState& standardSpeakerBus) const
{
    if (customArtifact != nullptr)
        return customArtifact->cacheKey;
    const auto* artifact = decoder != nullptr
                         ? decoder->standardArtifactFor(standardSpeakerBus) : nullptr;
    return artifact != nullptr ? juce::String(artifact->cacheKey) : juce::String();
}
} // namespace singleeditor
