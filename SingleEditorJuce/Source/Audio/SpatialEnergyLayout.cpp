#include "Audio/SpatialEnergyLayout.h"

#include <algorithm>
#include <cmath>

namespace singleeditor
{
double SpatialEnergyLayout::positionWeight(float azimuth, float elevation) noexcept
{
    // BS.1770 Annex 3: the lateral horizontal sector, not every rear/height
    // channel. This multiplier belongs to global loudness, never field colour.
    const auto angle = std::abs(azimuth);
    return std::abs(elevation) < 30 && angle >= 60 && angle <= 120 ? 1.41 : 1.0;
}

SpatialEnergyMeter::Layout SpatialEnergyLayout::fromRoute(const DeviceRoutePlan& route,
    const SpeakerGeometryProfile* geometry, std::uint64_t generation)
{
    using Placement = SpatialEnergyMeter::Placement;
    SpatialEnergyMeter::Layout result;
    result.generation = generation;
    result.sampleRate = route.device().sampleRate;
    result.geometryCurrent = geometry && geometry->profileId.toStdString() == route.bus().profileId
        && geometry->revision == route.bus().revision;
    std::vector<const SpeakerGeometrySpeaker*> speakers;
    if (result.geometryCurrent)
    {
        for (const auto& speaker : geometry->speakers)
            if (speaker.enabled && speaker.available) speakers.push_back(&speaker);
        if (speakers.size() != route.bus().speakers.size()) result.geometryCurrent = false;
        else for (std::size_t row = 0; row < speakers.size(); ++row)
        {
            const auto& speaker = *speakers[row];
            const auto& bus = route.bus().speakers[row];
            if (speaker.stableId.toStdString() != bus.stableId || speaker.role != bus.role
                || !std::isfinite(speaker.azimuthDegrees) || !std::isfinite(speaker.elevationDegrees)
                || std::abs(speaker.azimuthDegrees) > 180 || std::abs(speaker.elevationDegrees) > 90)
                result.geometryCurrent = false;
        }
    }
    const auto count = static_cast<int>(route.device().physicalOutputs.size());
    for (int row = 0; row < static_cast<int>(route.bus().speakers.size()); ++row)
    {
        bool mapped = false;
        for (int slot = 0; slot < count; ++slot) mapped = mapped || route.connected(row, slot);
        if (!mapped) ++result.unmappedSpeakerCount;
    }
    for (int slot = 0; slot < count; ++slot)
    {
        SpatialEnergyMeter::Channel channel;
        const auto physical = route.device().physicalOutputs[static_cast<std::size_t>(slot)];
        channel.physicalOutput = physical;
        channel.label = route.device().outputNames[static_cast<std::size_t>(physical)];
        int rowMatch = -1, contributors = 0;
        for (int row = 0; row < static_cast<int>(route.bus().speakers.size()); ++row)
            if (route.connected(row, slot)) { rowMatch = row; ++contributors; }
        channel.mapped = contributors != 0;
        channel.ambiguous = contributors > 1;
        const SpeakerGeometrySpeaker* direction = nullptr;
        if (result.geometryCurrent)
        {
            // Actual backend semantics identify the output independently from
            // source rows, including crossed routes and many-to-one summation.
            const auto& trusted = route.trustedOutputRoles();
            const auto role = trusted ? (*trusted)[static_cast<std::size_t>(physical)] : SpeakerRole::unknown;
            if (role > SpeakerRole::discrete && role < SpeakerRole::ambiACN0)
            {
                int matches = 0;
                for (const auto* speaker : speakers)
                    if (speaker->role == role) { direction = speaker; ++matches; }
                if (matches != 1) direction = nullptr;
            }
            // In the absence of authoritative output semantics, a unique
            // explicitly confirmed geometry-to-port binding gives one anchor.
            // Multiple source rows cannot invent one physical speaker direction.
            else if (contributors == 1) direction = speakers[static_cast<std::size_t>(rowMatch)];
        }
        if (direction)
        {
            channel.speakerId = direction->stableId.toStdString();
            if (direction->role == SpeakerRole::LFE)
            {
                channel.placement = Placement::lfe;
                channel.loudnessWeight = 0;
            }
            else
            {
                channel.placement = Placement::directional;
                channel.azimuthDegrees = direction->azimuthDegrees;
                channel.elevationDegrees = direction->elevationDegrees;
                channel.loudnessWeight = positionWeight(channel.azimuthDegrees, channel.elevationDegrees);
            }
        }
        result.channels.push_back(std::move(channel));
    }
    return result;
}
}
