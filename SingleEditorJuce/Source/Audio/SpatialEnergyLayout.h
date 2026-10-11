#pragma once

#include "Audio/SpatialEnergyMeter.h"
#include "Audio/DeviceRoutePlan.h"
#include "Audio/AmbisonicSpeakerGeometry.h"

namespace singleeditor
{
// Control/worker-side projection of an already approved immutable binding.
// No device enumeration, matrix generation, route mutation or fallback layout.
class SpatialEnergyLayout final
{
public:
    static SpatialEnergyMeter::Layout fromRoute(const DeviceRoutePlan&,
        const SpeakerGeometryProfile* activeGeometry, std::uint64_t generation);
    static double positionWeight(float azimuthDegrees, float elevationDegrees) noexcept;
};
}
