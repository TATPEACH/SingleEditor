#include "Audio/ChannelRouter.h"

#include <cmath>

namespace singleeditor
{
float RoutingMatrix::gain(int outputChannel, int sourceChannel) const noexcept
{
    if (outputChannel < 0 || outputChannel >= outputChannels || sourceChannel < 0 || sourceChannel >= sourceChannels)
        return 0.0f;

    return gains[static_cast<std::size_t>((outputChannel * sourceChannels) + sourceChannel)];
}

float& RoutingMatrix::gain(int outputChannel, int sourceChannel) noexcept
{
    return gains[static_cast<std::size_t>((outputChannel * sourceChannels) + sourceChannel)];
}

RoutingMatrix ChannelRouter::buildRoutingMatrix(const ChannelLayoutState& sourceLayout,
                                                const ChannelLayoutState& targetLayout,
                                                const ChannelPlaybackState& playbackState)
{
    RoutingMatrix matrix;
    matrix.sourceChannels = juce::jlimit(0, RoutingMatrix::maxChannels, sourceLayout.sourceChannelCount);
    matrix.outputChannels = juce::jlimit(0, RoutingMatrix::maxChannels, targetLayout.sourceChannelCount);
    matrix.gains.fill(0.0f);

    if (matrix.sourceChannels <= 0 || matrix.outputChannels <= 0)
        return matrix;

    if (sourceLayout.isDiscreteFallback)
    {
        if (matrix.outputChannels > 0 && matrix.sourceChannels > 0 && sourceChannelIsAudible(0, playbackState))
            matrix.gain(0, 0) = 1.0f;
        if (matrix.outputChannels > 1 && matrix.sourceChannels > 1 && sourceChannelIsAudible(1, playbackState))
            matrix.gain(1, 1) = 1.0f;
        return matrix;
    }

    for (int sourceChannel = 0; sourceChannel < matrix.sourceChannels; ++sourceChannel)
    {
        if (!sourceChannelIsAudible(sourceChannel, playbackState))
            continue;

        const auto sourceRole = sourceChannel < static_cast<int>(sourceLayout.selectedRoles.size())
            ? sourceLayout.selectedRoles[static_cast<std::size_t>(sourceChannel)]
            : SpeakerRole::unknown;

        if (const auto directTarget = outputChannelForRole(targetLayout,
                                                           playbackState.outputMapping,
                                                           sourceRole);
            directTarget >= 0)
        {
            matrix.gain(directTarget, sourceChannel) += 1.0f;
            continue;
        }

        addFoldDownToTarget(matrix,
                            targetLayout,
                            playbackState.outputMapping,
                            sourceChannel,
                            sourceRole,
                            playbackState.includeLfeInDownmix);
    }

    return matrix;
}

int ChannelRouter::outputChannelForRole(const ChannelLayoutState& targetLayout,
                                        const ChannelOutputMapping& outputMapping,
                                        SpeakerRole role) noexcept
{
    if (outputMapping.enabled)
    {
        const auto patchedOutput = outputMapping.getOutputChannel(role);
        if (patchedOutput >= 0)
            return patchedOutput < targetLayout.sourceChannelCount ? patchedOutput : -1;
    }

    return findRole(targetLayout, role);
}

bool ChannelRouter::outputMappingIsValid(const ChannelLayoutState& targetLayout,
                                         const ChannelOutputMapping& outputMapping) noexcept
{
    if (!outputMapping.enabled)
        return true;

    std::array<bool, RoutingMatrix::maxChannels> assignedOutputs {};
    const auto rolesToCheck = juce::jmin(targetLayout.sourceChannelCount,
                                         static_cast<int>(targetLayout.selectedRoles.size()));
    for (int roleIndex = 0; roleIndex < rolesToCheck; ++roleIndex)
    {
        const auto output = outputChannelForRole(
            targetLayout,
            outputMapping,
            targetLayout.selectedRoles[static_cast<std::size_t>(roleIndex)]);
        if (!juce::isPositiveAndBelow(output, targetLayout.sourceChannelCount)
            || assignedOutputs[static_cast<std::size_t>(output)])
        {
            return false;
        }
        assignedOutputs[static_cast<std::size_t>(output)] = true;
    }
    return true;
}

bool ChannelRouter::sourceChannelIsAudible(int sourceChannel,
                                           const ChannelPlaybackState& playbackState) noexcept
{
    if (sourceChannel < 0 || sourceChannel >= static_cast<int>(playbackState.mutedBySourceChannel.size()))
        return false;

    if (sourceChannel < static_cast<int>(playbackState.mutedBySourceChannel.size())
        && playbackState.mutedBySourceChannel[static_cast<std::size_t>(sourceChannel)])
    {
        return false;
    }

    auto anySolo = false;
    for (const auto soloed : playbackState.soloedBySourceChannel)
        anySolo = anySolo || soloed;

    if (!anySolo)
        return true;

    return sourceChannel < static_cast<int>(playbackState.soloedBySourceChannel.size())
        && playbackState.soloedBySourceChannel[static_cast<std::size_t>(sourceChannel)];
}

PhysicalOutputMatrix ChannelRouter::buildPhysicalOutputMatrix(
    const ChannelLayoutState& targetLayout,
    const ChannelOutputMapping& outputMapping)
{
    PhysicalOutputMatrix result;
    result.activeOutputChannels = juce::jlimit(0,
                                                RoutingMatrix::maxChannels,
                                                targetLayout.sourceChannelCount);
    result.targetPresetId = targetLayout.selectedPresetId;
    result.valid = outputMappingIsValid(targetLayout, outputMapping);

    const auto rolesToResolve = juce::jmin(result.activeOutputChannels,
                                           static_cast<int>(targetLayout.selectedRoles.size()));
    result.routes.reserve(static_cast<std::size_t>(rolesToResolve));
    for (int canonicalChannel = 0; canonicalChannel < rolesToResolve; ++canonicalChannel)
    {
        const auto role = targetLayout.selectedRoles[static_cast<std::size_t>(canonicalChannel)];
        result.routes.push_back({ role,
                                  canonicalChannel,
                                  outputChannelForRole(targetLayout, outputMapping, role) });
    }
    return result;
}

std::vector<SilentRouteProbe> ChannelRouter::buildSilentRouteProbes(
    const ChannelLayoutState& sourceLayout,
    const PhysicalOutputMatrix& physicalOutputMatrix,
    const RoutingMatrix& routingMatrix)
{
    std::vector<SilentRouteProbe> probes;
    probes.reserve(static_cast<std::size_t>(routingMatrix.sourceChannels));
    for (int sourceChannel = 0; sourceChannel < routingMatrix.sourceChannels; ++sourceChannel)
    {
        SilentRouteProbe probe;
        probe.sourceChannel = sourceChannel;
        probe.sourceRole = juce::isPositiveAndBelow(
                               sourceChannel,
                               static_cast<int>(sourceLayout.selectedRoles.size()))
                         ? sourceLayout.selectedRoles[static_cast<std::size_t>(sourceChannel)]
                         : SpeakerRole::discrete;

        for (int outputChannel = 0; outputChannel < routingMatrix.outputChannels; ++outputChannel)
        {
            const auto gain = routingMatrix.gain(outputChannel, sourceChannel);
            probe.finite = probe.finite && std::isfinite(gain);
            if (!std::isfinite(gain) || std::abs(gain) <= 0.000001f)
                continue;

            auto targetRole = SpeakerRole::unknown;
            for (const auto& route : physicalOutputMatrix.routes)
            {
                if (route.physicalOutputChannel == outputChannel)
                {
                    targetRole = route.role;
                    break;
                }
            }

            probe.outputs.push_back({ targetRole, outputChannel, gain });
        }
        probe.routed = !probe.outputs.empty();
        probes.push_back(std::move(probe));
    }
    return probes;
}

int ChannelRouter::findRole(const ChannelLayoutState& layout, SpeakerRole role) noexcept
{
    const auto channelsToSearch = juce::jmin(layout.sourceChannelCount, static_cast<int>(layout.selectedRoles.size()));
    for (int channel = 0; channel < channelsToSearch; ++channel)
    {
        if (layout.selectedRoles[static_cast<std::size_t>(channel)] == role)
            return channel;
    }

    return -1;
}

void ChannelRouter::addGainForRole(RoutingMatrix& matrix,
                                   const ChannelLayoutState& targetLayout,
                                   const ChannelOutputMapping& outputMapping,
                                   int sourceChannel,
                                   SpeakerRole targetRole,
                                   float gainToAdd) noexcept
{
    if (const auto outputChannel = outputChannelForRole(targetLayout, outputMapping, targetRole);
        outputChannel >= 0)
        matrix.gain(outputChannel, sourceChannel) += gainToAdd;
}

void ChannelRouter::addStereoFallback(RoutingMatrix& matrix,
                                      int sourceChannel,
                                      SpeakerRole sourceRole,
                                      bool includeLfe) noexcept
{
    if (matrix.outputChannels <= 0)
        return;

    const auto addLeft = [&](float gainToAdd)
    {
        matrix.gain(0, sourceChannel) += gainToAdd;
    };

    const auto addRight = [&](float gainToAdd)
    {
        if (matrix.outputChannels > 1)
            matrix.gain(1, sourceChannel) += gainToAdd;
        else
            matrix.gain(0, sourceChannel) += gainToAdd;
    };

    const auto addCenter = [&](float gainToAdd)
    {
        addLeft(gainToAdd);
        addRight(gainToAdd);
    };

    switch (sourceRole)
    {
        case SpeakerRole::L:
        case SpeakerRole::Ls:
        case SpeakerRole::Lss:
        case SpeakerRole::Lsr:
        case SpeakerRole::Lc:
        case SpeakerRole::Lw:
        case SpeakerRole::Ltf:
            addLeft(sourceRole == SpeakerRole::L ? 1.0f : minusThreeDbGain());
            break;

        case SpeakerRole::R:
        case SpeakerRole::Rs:
        case SpeakerRole::Rss:
        case SpeakerRole::Rsr:
        case SpeakerRole::Rc:
        case SpeakerRole::Rw:
        case SpeakerRole::Rtf:
            addRight(sourceRole == SpeakerRole::R ? 1.0f : minusThreeDbGain());
            break;

        case SpeakerRole::Ltr:
        case SpeakerRole::Ltm:
            addLeft(sourceRole == SpeakerRole::Ltr ? minusSixDbGain() : minusThreeDbGain());
            break;

        case SpeakerRole::Rtr:
        case SpeakerRole::Rtm:
            addRight(sourceRole == SpeakerRole::Rtr ? minusSixDbGain() : minusThreeDbGain());
            break;

        case SpeakerRole::C:
        case SpeakerRole::mono:
            addCenter(minusThreeDbGain());
            break;

        case SpeakerRole::LFE:
            if (includeLfe)
                addCenter(lfePreviewGain());
            break;

        case SpeakerRole::discrete:
        case SpeakerRole::unknown:
        default:
            break;
    }
}

void ChannelRouter::addMonoFallback(RoutingMatrix& matrix,
                                    int sourceChannel,
                                    SpeakerRole sourceRole,
                                    bool includeLfe) noexcept
{
    if (matrix.outputChannels <= 0)
        return;

    auto gain = 0.0f;
    switch (sourceRole)
    {
        case SpeakerRole::L:
        case SpeakerRole::R:
        case SpeakerRole::Lc:
        case SpeakerRole::Rc:
        case SpeakerRole::Lw:
        case SpeakerRole::Rw:
            gain = minusThreeDbGain();
            break;

        case SpeakerRole::C:
        case SpeakerRole::mono:
            gain = 1.0f;
            break;

        case SpeakerRole::Ls:
        case SpeakerRole::Rs:
        case SpeakerRole::Lss:
        case SpeakerRole::Rss:
        case SpeakerRole::Lsr:
        case SpeakerRole::Rsr:
        case SpeakerRole::Ltf:
        case SpeakerRole::Rtf:
        case SpeakerRole::Ltm:
        case SpeakerRole::Rtm:
            gain = minusSixDbGain();
            break;

        case SpeakerRole::Ltr:
        case SpeakerRole::Rtr:
            gain = minusNineDbGain();
            break;

        case SpeakerRole::LFE:
            gain = includeLfe ? lfePreviewGain() : 0.0f;
            break;

        case SpeakerRole::discrete:
        case SpeakerRole::unknown:
        default:
            break;
    }

    matrix.gain(0, sourceChannel) += gain;
}

void ChannelRouter::addFoldDownToTarget(RoutingMatrix& matrix,
                                        const ChannelLayoutState& targetLayout,
                                        const ChannelOutputMapping& outputMapping,
                                        int sourceChannel,
                                        SpeakerRole sourceRole,
                                        bool includeLfe) noexcept
{
    if (targetLayout.sourceChannelCount == 1)
    {
        addMonoFallback(matrix, sourceChannel, sourceRole, includeLfe);
        return;
    }

    if (targetLayout.sourceChannelCount == 2)
    {
        addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
        return;
    }

    switch (sourceRole)
    {
        case SpeakerRole::Ltf:
            addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::L, minusThreeDbGain());
            break;
        case SpeakerRole::Rtf:
            addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::R, minusThreeDbGain());
            break;
        case SpeakerRole::Ltm:
            addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::L, minusThreeDbGain());
            break;
        case SpeakerRole::Rtm:
            addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::R, minusThreeDbGain());
            break;
        case SpeakerRole::Ltr:
            if (findRole(targetLayout, SpeakerRole::Lsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Lsr, minusThreeDbGain());
            else if (findRole(targetLayout, SpeakerRole::Ls) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Ls, minusThreeDbGain());
            else
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::L, minusSixDbGain());
            break;
        case SpeakerRole::Rtr:
            if (findRole(targetLayout, SpeakerRole::Rsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rsr, minusThreeDbGain());
            else if (findRole(targetLayout, SpeakerRole::Rs) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rs, minusThreeDbGain());
            else
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::R, minusSixDbGain());
            break;
        case SpeakerRole::Ls:
            if (findRole(targetLayout, SpeakerRole::Lss) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Lss, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Lsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Lsr, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::Rs:
            if (findRole(targetLayout, SpeakerRole::Rss) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rss, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Rsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rsr, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::Lss:
            if (findRole(targetLayout, SpeakerRole::Ls) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Ls, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Lss) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Lss, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::Rss:
            if (findRole(targetLayout, SpeakerRole::Rs) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rs, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Rss) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rss, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::Lsr:
            if (findRole(targetLayout, SpeakerRole::Ls) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Ls, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Lsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Lsr, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::Rsr:
            if (findRole(targetLayout, SpeakerRole::Rs) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rs, 1.0f);
            else if (findRole(targetLayout, SpeakerRole::Rsr) >= 0)
                addGainForRole(matrix, targetLayout, outputMapping, sourceChannel, SpeakerRole::Rsr, 1.0f);
            else
                addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
        case SpeakerRole::LFE:
            if (includeLfe)
                addStereoFallback(matrix, sourceChannel, sourceRole, true);
            break;
        default:
            addStereoFallback(matrix, sourceChannel, sourceRole, includeLfe);
            break;
    }
}
} // namespace singleeditor
