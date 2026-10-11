#pragma once

#include <array>
#include <vector>

#include "Audio/ChannelLayout.h"

namespace singleeditor
{
struct RoutingMatrix
{
    static constexpr int maxChannels = 32;

    int sourceChannels = 0;
    int outputChannels = 0;
    std::array<float, maxChannels * maxChannels> gains {};

    float gain(int outputChannel, int sourceChannel) const noexcept;
    float& gain(int outputChannel, int sourceChannel) noexcept;
};

struct PhysicalOutputRoute
{
    SpeakerRole role = SpeakerRole::unknown;
    int canonicalChannel = -1;
    int physicalOutputChannel = -1;
};

struct PhysicalOutputMatrix
{
    static constexpr int schemaVersion = 1;

    int activeOutputChannels = 0;
    juce::String targetPresetId;
    bool valid = true;
    std::vector<PhysicalOutputRoute> routes;
};

struct SilentRouteProbeOutput
{
    SpeakerRole targetRole = SpeakerRole::unknown;
    int physicalOutputChannel = -1;
    float gain = 0.0f;
};

struct SilentRouteProbe
{
    static constexpr int schemaVersion = 1;

    int sourceChannel = -1;
    SpeakerRole sourceRole = SpeakerRole::unknown;
    bool finite = true;
    bool routed = false;
    std::vector<SilentRouteProbeOutput> outputs;
};

class ChannelRouter final
{
public:
    static constexpr const char* downmixProfileId() noexcept { return "wwise-ac3-preview-v1"; }
    static constexpr float minusThreeDbGain() noexcept { return 0.70710678118f; }
    static constexpr float minusSixDbGain() noexcept { return 0.5f; }
    static constexpr float minusNineDbGain() noexcept { return 0.35355339059f; }
    static constexpr float lfePreviewGain() noexcept { return 0.25f; }

    static RoutingMatrix buildRoutingMatrix(const ChannelLayoutState& sourceLayout,
                                            const ChannelLayoutState& targetLayout,
                                            const ChannelPlaybackState& playbackState);
    static int outputChannelForRole(const ChannelLayoutState& targetLayout,
                                    const ChannelOutputMapping& outputMapping,
                                    SpeakerRole role) noexcept;
    static bool outputMappingIsValid(const ChannelLayoutState& targetLayout,
                                     const ChannelOutputMapping& outputMapping) noexcept;
    static bool sourceChannelIsAudible(int sourceChannel,
                                       const ChannelPlaybackState& playbackState) noexcept;
    static PhysicalOutputMatrix buildPhysicalOutputMatrix(
        const ChannelLayoutState& targetLayout,
        const ChannelOutputMapping& outputMapping);
    static std::vector<SilentRouteProbe> buildSilentRouteProbes(
        const ChannelLayoutState& sourceLayout,
        const PhysicalOutputMatrix& physicalOutputMatrix,
        const RoutingMatrix& routingMatrix);

private:
    static int findRole(const ChannelLayoutState& layout, SpeakerRole role) noexcept;
    static void addGainForRole(RoutingMatrix& matrix,
                               const ChannelLayoutState& targetLayout,
                               const ChannelOutputMapping& outputMapping,
                               int sourceChannel,
                               SpeakerRole targetRole,
                               float gainToAdd) noexcept;
    static void addStereoFallback(RoutingMatrix& matrix,
                                  int sourceChannel,
                                  SpeakerRole sourceRole,
                                  bool includeLfe) noexcept;
    static void addMonoFallback(RoutingMatrix& matrix,
                                int sourceChannel,
                                SpeakerRole sourceRole,
                                bool includeLfe) noexcept;
    static void addFoldDownToTarget(RoutingMatrix& matrix,
                                    const ChannelLayoutState& targetLayout,
                                    const ChannelOutputMapping& outputMapping,
                                    int sourceChannel,
                                    SpeakerRole sourceRole,
                                    bool includeLfe) noexcept;
};
} // namespace singleeditor
