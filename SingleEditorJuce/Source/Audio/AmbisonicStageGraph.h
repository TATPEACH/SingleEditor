#pragma once

#include <cstdint>
#include <memory>

#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/DeviceRoutePlan.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
class AmbisonicStageGraph final
{
public:
    static constexpr int schemaVersion = 1;

    static std::shared_ptr<const AmbisonicStageGraph> create(
        const ChannelLayoutState& sourceLayout,
        AmbisonicOrientation orientation,
        std::uint64_t bindingGeneration = 0,
        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customArtifact = {});

    static juce::String stageOrderId();

    RoutingMatrix buildCanonicalSpeakerBusMatrix(
        const ChannelLayoutState& standardSpeakerBus,
        const ChannelPlaybackState& playbackState) const noexcept;
    RoutingMatrix buildPhysicalRoutingMatrix(
        const ChannelLayoutState& physicalTargetLayout,
        const ChannelPlaybackState& playbackState) const noexcept;

    int getEffectiveOrder() const noexcept { return effectiveOrder; }
    int getSourceChannelCount() const noexcept { return sourceChannelCount; }
    std::uint64_t getBindingGeneration() const noexcept { return bindingGeneration; }
    AmbisonicOrientation getOrientation() const noexcept { return orientation; }
    juce::String getDecoderProfileId() const { return decoderProfileId; }
    bool usesCustomSpeakerGeometry() const noexcept { return customArtifact != nullptr; }
    bool matchesCanonicalBus(const DeviceRoutePlan::Bus&) const noexcept;
    juce::String matrixArtifactIdFor(const ChannelLayoutState& standardSpeakerBus) const;
    juce::String geometryProfileIdFor(const ChannelLayoutState& standardSpeakerBus) const;
    juce::String geometryProfileHashFor(const ChannelLayoutState& standardSpeakerBus) const;
    juce::String matrixCacheKeyFor(const ChannelLayoutState& standardSpeakerBus) const;

private:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // First member is destroyed last, after decoder/artifact references.
    struct ObservedLifetime
    {
        OptionalLifecycleToken token;
        ~ObservedLifetime() { token.release(); }
    };
    mutable ObservedLifetime observedLifetime;
#endif
    AmbisonicStageGraph(std::shared_ptr<const AmbisonicMonitorDecoder> decoderToUse,
                        int effectiveOrderToUse,
                        int sourceChannelCountToUse,
                        AmbisonicOrientation orientationToUse,
                        std::uint64_t bindingGenerationToUse,
                        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customArtifactToUse);

    std::shared_ptr<const AmbisonicMonitorDecoder> decoder;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> customArtifact;
    int effectiveOrder = 0;
    int sourceChannelCount = 0;
    AmbisonicOrientation orientation;
    std::uint64_t bindingGeneration = 0;
    juce::String decoderProfileId;
};
} // namespace singleeditor
