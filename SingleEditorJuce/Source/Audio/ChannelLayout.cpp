#include "Audio/ChannelLayout.h"

#include <array>
#include <utility>

namespace singleeditor
{
namespace
{
using RoleList = std::vector<SpeakerRole>;

juce::String orderTextForRoles(const std::vector<SpeakerRole>& roles);

juce::CriticalSection preferredPresetLock;
juce::String preferredDefault4ch;
juce::String preferredDefault6ch;
juce::String preferredDefault8ch;
juce::String preferredDefault12ch;

ChannelOrderPreset preset(juce::String id,
                          juce::String displayName,
                          juce::String orderText,
                          int channelCount,
                          RoleList roles,
                          bool isDefault,
                          bool isLegacy = false)
{
    ChannelOrderPreset result;
    result.id = std::move(id);
    result.displayName = std::move(displayName);
    result.orderText = std::move(orderText);
    result.sourceChannelCount = channelCount;
    result.roles = std::move(roles);
    result.isDefaultForGameDolby = isDefault;
    result.isLegacy = isLegacy;
    return result;
}

RoleList ambisonicRoles(int channelCount)
{
    RoleList roles;
    roles.reserve(static_cast<std::size_t>(channelCount));
    const auto firstRole = static_cast<int>(SpeakerRole::ambiACN0);
    for (int channel = 0; channel < channelCount; ++channel)
        roles.push_back(static_cast<SpeakerRole>(firstRole + channel));
    return roles;
}

ChannelOrderPreset ambisonicPreset(juce::String id,
                                   juce::String displayName,
                                   int order)
{
    const auto channelCount = (order + 1) * (order + 1);
    auto roles = ambisonicRoles(channelCount);
    auto orderText = orderTextForRoles(roles);
    auto result = preset(std::move(id),
                         std::move(displayName),
                         std::move(orderText),
                         channelCount,
                         std::move(roles),
                         false);
    result.isAmbisonic = true;
    result.ambisonicOrder = order;
    result.ambisonicNormalization = "SN3D";
    return result;
}

struct WaveMaskRole
{
    juce::uint32 bit = 0;
    SpeakerRole role = SpeakerRole::unknown;
};

constexpr std::array<WaveMaskRole, 18> waveMaskRoles {{
    { 0x00000001u, SpeakerRole::L },
    { 0x00000002u, SpeakerRole::R },
    { 0x00000004u, SpeakerRole::C },
    { 0x00000008u, SpeakerRole::LFE },
    { 0x00000010u, SpeakerRole::Lsr },
    { 0x00000020u, SpeakerRole::Rsr },
    { 0x00000040u, SpeakerRole::Lc },
    { 0x00000080u, SpeakerRole::Rc },
    { 0x00000100u, SpeakerRole::Ls },
    { 0x00000200u, SpeakerRole::Lss },
    { 0x00000400u, SpeakerRole::Rss },
    { 0x00000800u, SpeakerRole::C },
    { 0x00001000u, SpeakerRole::Ltf },
    { 0x00002000u, SpeakerRole::C },
    { 0x00004000u, SpeakerRole::Rtf },
    { 0x00008000u, SpeakerRole::Ltr },
    { 0x00010000u, SpeakerRole::C },
    { 0x00020000u, SpeakerRole::Rtr }
}};

juce::String orderTextForRoles(const std::vector<SpeakerRole>& roles)
{
    juce::String text;
    for (const auto role : roles)
    {
        if (text.isNotEmpty())
            text += " ";
        text += ChannelLayoutCatalog::roleToAbbreviation(role);
    }
    return text;
}

juce::String& preferredDefaultForChannelCount(int channelCount) noexcept
{
    if (channelCount == 4)
        return preferredDefault4ch;
    if (channelCount == 6)
        return preferredDefault6ch;
    if (channelCount == 8)
        return preferredDefault8ch;
    return preferredDefault12ch;
}

bool supportsPreferredDefault(int channelCount) noexcept
{
    return channelCount == 4 || channelCount == 6 || channelCount == 8 || channelCount == 12;
}
} // namespace

juce::String ChannelLayoutCatalog::roleToAbbreviation(SpeakerRole role)
{
    if (const auto channel = ambisonicChannelIndex(role); channel >= 0)
        return "ACN" + juce::String(channel);

    switch (role)
    {
        case SpeakerRole::mono: return "M";
        case SpeakerRole::L: return "L";
        case SpeakerRole::R: return "R";
        case SpeakerRole::C: return "C";
        case SpeakerRole::LFE: return "LFE";
        case SpeakerRole::Ls: return "Ls";
        case SpeakerRole::Rs: return "Rs";
        case SpeakerRole::Lss: return "Lss";
        case SpeakerRole::Rss: return "Rss";
        case SpeakerRole::Lsr: return "Lsr";
        case SpeakerRole::Rsr: return "Rsr";
        case SpeakerRole::Lc: return "Lc";
        case SpeakerRole::Rc: return "Rc";
        case SpeakerRole::Lw: return "Lw";
        case SpeakerRole::Rw: return "Rw";
        case SpeakerRole::Ltm: return "Ltm";
        case SpeakerRole::Rtm: return "Rtm";
        case SpeakerRole::Ltf: return "Ltf";
        case SpeakerRole::Rtf: return "Rtf";
        case SpeakerRole::Ltr: return "Ltr";
        case SpeakerRole::Rtr: return "Rtr";
        case SpeakerRole::discrete: return "Discrete";
        case SpeakerRole::unknown:
        default: break;
    }

    return "?";
}

SpeakerRole ChannelLayoutCatalog::roleFromAbbreviation(const juce::String& abbreviation)
{
    const auto candidate = abbreviation.trim();
    for (int roleIndex = static_cast<int>(SpeakerRole::mono);
         roleIndex <= static_cast<int>(SpeakerRole::ambiACN15);
         ++roleIndex)
    {
        const auto role = static_cast<SpeakerRole>(roleIndex);
        if (roleToAbbreviation(role).equalsIgnoreCase(candidate))
            return role;
    }

    if (candidate.equalsIgnoreCase("Discrete"))
        return SpeakerRole::discrete;
    return SpeakerRole::unknown;
}

juce::String ChannelLayoutCatalog::roleToDisplayName(SpeakerRole role)
{
    if (const auto channel = ambisonicChannelIndex(role); channel >= 0)
    {
        static constexpr std::array<const char*, 4> firstOrderAliases {{ "W", "Y", "Z", "X" }};
        const auto alias = juce::isPositiveAndBelow(channel,
                                                     static_cast<int>(firstOrderAliases.size()))
                         ? " (" + juce::String(firstOrderAliases[static_cast<std::size_t>(channel)]) + ")"
                         : juce::String();
        return "Ambisonic ACN" + juce::String(channel) + alias;
    }

    switch (role)
    {
        case SpeakerRole::mono: return "Mono";
        case SpeakerRole::L: return "Left";
        case SpeakerRole::R: return "Right";
        case SpeakerRole::C: return "Center";
        case SpeakerRole::LFE: return "LFE";
        case SpeakerRole::Ls: return "Left Surround";
        case SpeakerRole::Rs: return "Right Surround";
        case SpeakerRole::Lss: return "Left Side Surround";
        case SpeakerRole::Rss: return "Right Side Surround";
        case SpeakerRole::Lsr: return "Left Rear Surround";
        case SpeakerRole::Rsr: return "Right Rear Surround";
        case SpeakerRole::Lc: return "Left Center";
        case SpeakerRole::Rc: return "Right Center";
        case SpeakerRole::Lw: return "Left Wide";
        case SpeakerRole::Rw: return "Right Wide";
        case SpeakerRole::Ltm: return "Left Top Middle";
        case SpeakerRole::Rtm: return "Right Top Middle";
        case SpeakerRole::Ltf: return "Left Top Front";
        case SpeakerRole::Rtf: return "Right Top Front";
        case SpeakerRole::Ltr: return "Left Top Rear";
        case SpeakerRole::Rtr: return "Right Top Rear";
        case SpeakerRole::discrete: return "Discrete";
        case SpeakerRole::unknown:
        default: break;
    }

    return "Unknown";
}

juce::String ChannelLayoutCatalog::interpretationOriginToString(ChannelInterpretationOrigin origin)
{
    switch (origin)
    {
        case ChannelInterpretationOrigin::implicitDefault: return "implicit_default";
        case ChannelInterpretationOrigin::explicitPreset: return "explicit_preset";
        case ChannelInterpretationOrigin::waveChannelMask: return "wave_channel_mask";
        case ChannelInterpretationOrigin::discreteFallback: return "discrete_fallback";
    }

    return "implicit_default";
}

bool ChannelLayoutCatalog::isAmbisonicRole(SpeakerRole role) noexcept
{
    return ambisonicChannelIndex(role) >= 0;
}

int ChannelLayoutCatalog::ambisonicChannelIndex(SpeakerRole role) noexcept
{
    const auto roleIndex = static_cast<int>(role);
    const auto first = static_cast<int>(SpeakerRole::ambiACN0);
    const auto last = static_cast<int>(SpeakerRole::ambiACN15);
    return roleIndex >= first && roleIndex <= last ? roleIndex - first : -1;
}

std::vector<ChannelOrderPreset> ChannelLayoutCatalog::candidatesForChannelCount(int channelCount)
{
    switch (channelCount)
    {
        case 1:
            return { preset("mono",
                            "Mono",
                            "M",
                            1,
                            { SpeakerRole::mono },
                            true) };
        case 2:
            return { preset("stereo",
                            "Stereo",
                            "L R",
                            2,
                            { SpeakerRole::L, SpeakerRole::R },
                            true) };
        case 4:
            return { preset("quad-lrlsrs",
                            "Quad",
                            "L R Ls Rs",
                            4,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::Ls, SpeakerRole::Rs },
                            true),
                     ambisonicPreset("foa-acn-sn3d", "FOA ACN/SN3D", 1),
                     makeDiscretePreset(4) };
        case 6:
            return { preset("5.1-smpte-dolby",
                            "5.1 SMPTE/Dolby",
                            "L R C LFE Ls Rs",
                            6,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Ls, SpeakerRole::Rs },
                            true),
                     preset("5.1-film-protools",
                            "5.1 Film / Pro Tools",
                            "L C R Ls Rs LFE",
                            6,
                            { SpeakerRole::L, SpeakerRole::C, SpeakerRole::R, SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::LFE },
                            false),
                     preset("5.1-dts",
                            "5.1 DTS",
                            "L R Ls Rs C LFE",
                            6,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::C, SpeakerRole::LFE },
                            false),
                     makeDiscretePreset(6) };
        case 8:
            return { preset("7.1-smpte",
                            "7.1 SMPTE",
                            "L R C LFE Lss Rss Lsr Rsr",
                            8,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Lss, SpeakerRole::Rss, SpeakerRole::Lsr, SpeakerRole::Rsr },
                            true),
                     preset("7.1-microsoft-wave",
                            "7.1 Microsoft WAVE (0x63F)",
                            "L R C LFE Lsr Rsr Lss Rss",
                            8,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::Lss, SpeakerRole::Rss },
                            false),
                     preset("7.1-film-protools",
                            "7.1 Film / Pro Tools",
                            "L C R Lss Rss Lsr Rsr LFE",
                            8,
                            { SpeakerRole::L, SpeakerRole::C, SpeakerRole::R, SpeakerRole::Lss, SpeakerRole::Rss, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::LFE },
                            false),
                     makeDiscretePreset(8) };
        case 9:
            return { makeDiscretePreset(9),
                     ambisonicPreset("2oa-acn-sn3d", "2OA ACN/SN3D", 2) };
        case 12:
            return { preset("7.1.4-dolby-smpte",
                            "7.1.4 Dolby/SMPTE",
                            "L R C LFE Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr",
                            12,
                            { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Lss, SpeakerRole::Rss, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::Ltf, SpeakerRole::Rtf, SpeakerRole::Ltr, SpeakerRole::Rtr },
                            true),
                     preset("7.1.4-film-protools",
                            "7.1.4 Film / Pro Tools",
                            "L C R Lss Rss Lsr Rsr LFE Ltf Rtf Ltr Rtr",
                            12,
                            { SpeakerRole::L, SpeakerRole::C, SpeakerRole::R, SpeakerRole::Lss, SpeakerRole::Rss, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::LFE, SpeakerRole::Ltf, SpeakerRole::Rtf, SpeakerRole::Ltr, SpeakerRole::Rtr },
                            false),
                     makeDiscretePreset(12) };
        case 16:
            return { makeDiscretePreset(16),
                     ambisonicPreset("3oa-acn-sn3d", "3OA ACN/SN3D", 3) };
        default:
            break;
    }

    return { makeDiscretePreset(channelCount) };
}

ChannelLayoutState ChannelLayoutCatalog::defaultLayoutForChannelCount(int channelCount)
{
    auto candidates = candidatesForChannelCount(channelCount);
    const auto preferredPresetId = getPreferredDefaultPresetId(channelCount);
    if (preferredPresetId.isNotEmpty())
    {
        for (const auto& candidate : candidates)
        {
            if (candidate.id == preferredPresetId)
                return makeState(candidate, candidates, candidate.id.startsWith("discrete-"));
        }
    }

    for (const auto& candidate : candidates)
    {
        if (candidate.isDefaultForGameDolby)
            return makeState(candidate, candidates, candidate.id.startsWith("discrete-"));
    }

    return makeState(candidates.empty() ? makeDiscretePreset(channelCount) : candidates.front(),
                     std::move(candidates),
                     true);
}

ChannelLayoutState ChannelLayoutCatalog::layoutForPresetId(int channelCount, const juce::String& presetId)
{
    auto candidates = candidatesForChannelCount(channelCount);
    for (const auto& candidate : candidates)
    {
        if (candidate.id == presetId)
        {
            auto state = makeState(candidate, candidates, candidate.id.startsWith("discrete-"));
            state.interpretationOrigin = state.isDiscreteFallback
                                       ? ChannelInterpretationOrigin::discreteFallback
                                       : ChannelInterpretationOrigin::explicitPreset;
            return state;
        }
    }

    return defaultLayoutForChannelCount(channelCount);
}

ChannelLayoutState ChannelLayoutCatalog::layoutForWaveChannelMask(int channelCount, juce::uint32 channelMask)
{
    if (channelCount <= 0 || channelMask == 0)
        return defaultLayoutForChannelCount(channelCount);

    std::vector<SpeakerRole> roles;
    roles.reserve(static_cast<std::size_t>(channelCount));
    for (const auto& mapping : waveMaskRoles)
    {
        if ((channelMask & mapping.bit) != 0)
            roles.push_back(mapping.role);
    }

    if (static_cast<int>(roles.size()) != channelCount)
        return defaultLayoutForChannelCount(channelCount);

    auto candidates = candidatesForChannelCount(channelCount);
    for (auto candidate = candidates.begin(); candidate != candidates.end();)
    {
        if (candidate->roles == roles)
            candidate = candidates.erase(candidate);
        else
            ++candidate;
    }

    auto detectedPreset = preset("wav-channel-mask-" + juce::String(static_cast<int>(channelMask)),
                                 "WAV channel mask",
                                 orderTextForRoles(roles),
                                 channelCount,
                                 roles,
                                 false);
    candidates.insert(candidates.begin(), detectedPreset);

    auto state = makeState(std::move(detectedPreset), std::move(candidates), false);
    state.detectedFromMetadata = true;
    state.interpretationOrigin = ChannelInterpretationOrigin::waveChannelMask;
    state.waveChannelMask = channelMask;
    return state;
}

ChannelLayoutState ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(int channelCount,
                                                                              juce::uint32 channelMask,
                                                                              const juce::String& presetId)
{
    if (channelMask == 0)
        return presetId.isEmpty()
            ? defaultLayoutForChannelCount(channelCount)
            : layoutForPresetId(channelCount, presetId);

    auto detectedLayout = layoutForWaveChannelMask(channelCount, channelMask);
    if (presetId.isEmpty() || presetId == detectedLayout.selectedPresetId)
        return detectedLayout;

    auto selectedLayout = layoutForPresetId(channelCount, presetId);
    if (selectedLayout.selectedRoles == detectedLayout.selectedRoles)
        return detectedLayout;

    selectedLayout.candidates = detectedLayout.candidates;
    selectedLayout.waveChannelMask = channelMask;
    return selectedLayout;
}

void ChannelLayoutCatalog::setPreferredDefaultPresetId(int channelCount, const juce::String& presetId)
{
    if (!supportsPreferredDefault(channelCount))
        return;

    auto acceptedPresetId = presetId.trim();
    if (acceptedPresetId.isNotEmpty())
    {
        auto found = false;
        for (const auto& candidate : candidatesForChannelCount(channelCount))
        {
            if (candidate.id == acceptedPresetId && !candidate.id.startsWith("discrete-"))
            {
                found = true;
                break;
            }
        }

        if (!found)
            acceptedPresetId = {};
    }

    const juce::ScopedLock sl(preferredPresetLock);
    preferredDefaultForChannelCount(channelCount) = acceptedPresetId;
}

juce::String ChannelLayoutCatalog::getPreferredDefaultPresetId(int channelCount)
{
    if (!supportsPreferredDefault(channelCount))
        return {};

    const juce::ScopedLock sl(preferredPresetLock);
    return preferredDefaultForChannelCount(channelCount);
}

ChannelLayoutState ChannelLayoutCatalog::stereoOutputLayout()
{
    return layoutForPresetId(2, "stereo");
}

ChannelLayoutState ChannelLayoutCatalog::monoOutputLayout()
{
    return layoutForPresetId(1, "mono");
}

ChannelLayoutState ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(int outputChannelCount)
{
    if (outputChannelCount <= 1)
        return monoOutputLayout();

    if (outputChannelCount == 2)
        return stereoOutputLayout();

    switch (outputChannelCount)
    {
        case 4:
            return makeState(preset("windows-wave-quad",
                                    "Windows / WAVE Quad output",
                                    "L R Ls Rs",
                                    4,
                                    { SpeakerRole::L, SpeakerRole::R, SpeakerRole::Ls, SpeakerRole::Rs },
                                    true),
                             {},
                             false);
        case 6:
            return makeState(preset("windows-wave-5.1",
                                    "Windows / WAVE 5.1 output",
                                    "L R C LFE Ls Rs",
                                    6,
                                    { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Ls, SpeakerRole::Rs },
                                    true),
                             {},
                             false);
        case 8:
            return makeState(preset("windows-wave-7.1",
                                    "Windows / WAVE 7.1 output",
                                    "L R C LFE Lsr Rsr Lss Rss",
                                    8,
                                    { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::Lss, SpeakerRole::Rss },
                                    true),
                             {},
                             false);
        case 12:
            return makeState(preset("windows-wave-7.1.4",
                                    "Windows / WAVE 7.1.4 output",
                                    "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr",
                                    12,
                                    { SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE, SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::Lss, SpeakerRole::Rss, SpeakerRole::Ltf, SpeakerRole::Rtf, SpeakerRole::Ltr, SpeakerRole::Rtr },
                                    true),
                             {},
                             false);
        default:
            break;
    }

    return stereoOutputLayout();
}

ChannelLayoutState ChannelLayoutCatalog::makeState(ChannelOrderPreset selected,
                                                   std::vector<ChannelOrderPreset> candidates,
                                                   bool discreteFallback)
{
    ChannelLayoutState state;
    state.sourceChannelCount = selected.sourceChannelCount;
    state.selectedPresetId = selected.id;
    state.displayName = selected.displayName;
    state.orderText = selected.orderText;
    state.selectedRoles = selected.roles;
    state.candidates = std::move(candidates);
    state.isDiscreteFallback = discreteFallback;
    state.interpretationOrigin = discreteFallback
                               ? ChannelInterpretationOrigin::discreteFallback
                               : ChannelInterpretationOrigin::implicitDefault;
    state.isAmbisonic = selected.isAmbisonic;
    state.ambisonicOrder = selected.ambisonicOrder;
    state.ambisonicNormalization = selected.ambisonicNormalization;
    return state;
}

ChannelOrderPreset ChannelLayoutCatalog::makeDiscretePreset(int channelCount)
{
    ChannelOrderPreset result;
    result.id = "discrete-" + juce::String(juce::jmax(0, channelCount));
    result.displayName = "Discrete " + juce::String(juce::jmax(0, channelCount)) + "ch";
    result.sourceChannelCount = juce::jmax(0, channelCount);
    result.isLegacy = true;

    for (int channel = 0; channel < result.sourceChannelCount; ++channel)
    {
        result.roles.push_back(SpeakerRole::discrete);
        if (channel > 0)
            result.orderText += " ";
        result.orderText += juce::String(channel + 1);
    }

    return result;
}
} // namespace singleeditor
