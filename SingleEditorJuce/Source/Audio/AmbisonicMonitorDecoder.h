#pragma once

#include <array>
#include <memory>

#include "Audio/AmbisonicMonitorState.h"
#include "Audio/AmbisonicSpeakerGeometry.h"
#include "Audio/ChannelRouter.h"

namespace singleeditor
{
class AmbisonicMonitorDecoder final
{
public:
    static std::shared_ptr<const AmbisonicMonitorDecoder> create(
        const ChannelLayoutState& sourceLayout,
        AmbisonicOrientation orientation = {});
    static AmbisonicOrientation canonicalizeOrientation(
        AmbisonicOrientation orientation) noexcept;
    static AmbisonicMonitorDecodeStatus statusFor(const ChannelLayoutState& sourceLayout) noexcept;
    static juce::String statusToString(AmbisonicMonitorDecodeStatus status);
    static juce::String capabilityFor(const ChannelLayoutState& sourceLayout);
    static juce::String modeFor(const ChannelLayoutState& sourceLayout);
    static juce::String decoderProfileId();
    static float maxReWeightForDegree(int ambisonicOrder, int degree) noexcept;
    static float fixedReferenceGainForOrder(int ambisonicOrder) noexcept;
    static void evaluateSn3dBasis(
        int ambisonicOrder,
        float azimuthDegrees,
        float elevationDegrees,
        AmbisonicOrientation orientation,
        std::array<float, 16>& destination) noexcept;

    RoutingMatrix buildRoutingMatrix(const ChannelLayoutState& targetLayout,
                                     const ChannelPlaybackState& playbackState) const noexcept;
    RoutingMatrix buildRoutingMatrix(const AmbisonicDecodeMatrixArtifact& artifact,
                                     const ChannelPlaybackState& playbackState) const noexcept;
    const StandardAmbisonicMatrixView* standardArtifactFor(
        const ChannelLayoutState& targetLayout) const noexcept;

private:
    AmbisonicMonitorDecoder(int ambisonicOrder,
                            AmbisonicOrientation orientation) noexcept;

    static constexpr int standardArtifactSlots = 6;

    int sourceChannelCount = 0;
    int effectiveOrder = 0;
    float fixedReferenceGain = 0.0f;
    AmbisonicOrientation orientation;
    std::array<StandardAmbisonicMatrixView, standardArtifactSlots> standardArtifacts {};
    int standardArtifactCount = 0;
};
} // namespace singleeditor
