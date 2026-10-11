#pragma once

#include <array>
#include <vector>

#include <juce_core/juce_core.h>

namespace singleeditor
{
enum class SpeakerRole
{
    unknown = 0,
    discrete,
    mono,
    L,
    R,
    C,
    LFE,
    Ls,
    Rs,
    Lss,
    Rss,
    Lsr,
    Rsr,
    Lc,
    Rc,
    Lw,
    Rw,
    Ltm,
    Rtm,
    Ltf,
    Rtf,
    Ltr,
    Rtr,
    ambiACN0,
    ambiACN1,
    ambiACN2,
    ambiACN3,
    ambiACN4,
    ambiACN5,
    ambiACN6,
    ambiACN7,
    ambiACN8,
    ambiACN9,
    ambiACN10,
    ambiACN11,
    ambiACN12,
    ambiACN13,
    ambiACN14,
    ambiACN15
};

enum class MonitorMode
{
    autoFollowDevice = 0,
    forceMultichannel,
    forceStereoDownmix
};

enum class ChannelInterpretationOrigin
{
    implicitDefault = 0,
    explicitPreset,
    waveChannelMask,
    discreteFallback
};

struct ChannelOrderPreset
{
    juce::String id;
    juce::String displayName;
    juce::String orderText;
    int sourceChannelCount = 0;
    std::vector<SpeakerRole> roles;
    bool isDefaultForGameDolby = false;
    bool isLegacy = false;
    bool isAmbisonic = false;
    int ambisonicOrder = 0;
    juce::String ambisonicNormalization;
};

struct ChannelLayoutState
{
    static constexpr int interpretationSchemaVersion = 1;

    int sourceChannelCount = 0;
    juce::String selectedPresetId;
    juce::String displayName;
    juce::String orderText;
    std::vector<SpeakerRole> selectedRoles;
    std::vector<ChannelOrderPreset> candidates;
    bool detectedFromMetadata = false;
    bool isDiscreteFallback = false;
    bool isAmbisonic = false;
    int ambisonicOrder = 0;
    juce::String ambisonicNormalization;
    ChannelInterpretationOrigin interpretationOrigin = ChannelInterpretationOrigin::implicitDefault;
    juce::uint32 waveChannelMask = 0;
};

struct ChannelOutputMapping
{
    static constexpr int maxRoleSlots = 32;

    ChannelOutputMapping() noexcept
    {
        outputChannelByRole.fill(-1);
    }

    bool setOutputChannel(SpeakerRole role, int zeroBasedOutputChannel) noexcept
    {
        const auto roleIndex = static_cast<int>(role);
        if (!juce::isPositiveAndBelow(roleIndex, maxRoleSlots)
            || !juce::isPositiveAndBelow(zeroBasedOutputChannel, maxRoleSlots))
        {
            return false;
        }

        outputChannelByRole[static_cast<std::size_t>(roleIndex)] = zeroBasedOutputChannel;
        enabled = true;
        return true;
    }

    int getOutputChannel(SpeakerRole role) const noexcept
    {
        const auto roleIndex = static_cast<int>(role);
        return juce::isPositiveAndBelow(roleIndex, maxRoleSlots)
             ? outputChannelByRole[static_cast<std::size_t>(roleIndex)]
             : -1;
    }

    void clear() noexcept
    {
        outputChannelByRole.fill(-1);
        enabled = false;
    }

    bool enabled = false;
    std::array<int, maxRoleSlots> outputChannelByRole {};
};

struct ChannelPlaybackState
{
    std::array<bool, 32> mutedBySourceChannel {};
    std::array<bool, 32> soloedBySourceChannel {};
    bool includeLfeInDownmix = false;
    MonitorMode monitorMode = MonitorMode::autoFollowDevice;
    ChannelOutputMapping outputMapping;
};

class ChannelLayoutCatalog final
{
public:
    static juce::String roleToAbbreviation(SpeakerRole role);
    static juce::String roleToDisplayName(SpeakerRole role);
    static juce::String interpretationOriginToString(ChannelInterpretationOrigin origin);
    static SpeakerRole roleFromAbbreviation(const juce::String& abbreviation);
    static bool isAmbisonicRole(SpeakerRole role) noexcept;
    static int ambisonicChannelIndex(SpeakerRole role) noexcept;
    static std::vector<ChannelOrderPreset> candidatesForChannelCount(int channelCount);
    static ChannelLayoutState defaultLayoutForChannelCount(int channelCount);
    static ChannelLayoutState layoutForPresetId(int channelCount, const juce::String& presetId);
    static ChannelLayoutState layoutForWaveChannelMask(int channelCount, juce::uint32 channelMask);
    static ChannelLayoutState layoutForPresetIdWithWaveChannelMask(int channelCount,
                                                                   juce::uint32 channelMask,
                                                                   const juce::String& presetId);
    static void setPreferredDefaultPresetId(int channelCount, const juce::String& presetId);
    static juce::String getPreferredDefaultPresetId(int channelCount);
    static ChannelLayoutState stereoOutputLayout();
    static ChannelLayoutState monoOutputLayout();
    static ChannelLayoutState deviceDefaultLayoutForOutputChannels(int outputChannelCount);

private:
    static ChannelLayoutState makeState(ChannelOrderPreset selected,
                                        std::vector<ChannelOrderPreset> candidates,
                                        bool discreteFallback);
    static ChannelOrderPreset makeDiscretePreset(int channelCount);
};
} // namespace singleeditor
