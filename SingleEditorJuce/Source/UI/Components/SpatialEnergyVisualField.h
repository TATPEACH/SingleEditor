#pragma once

#include "Audio/SpatialEnergyMeter.h"
#include <array>

namespace singleeditor
{
// Display-only D113 geometry. Construct on the visual worker, never the audio
// callback. The compact view never requests this artifact.
struct SpatialEnergyVisualField final
{
    static constexpr int width = 96, height = 48;
    struct Direction { float x = 0, y = 0, z = 0; };
    struct Point
    {
        float x = 0, y = 0; // normalised Hammer equal-area projection
        Direction direction;
        std::array<float, SpatialEnergyMeter::maxChannels> weights {};
    };
    std::shared_ptr<const SpatialEnergyMeter::Layout> layout;
    // Sphere inspection owns its base, never a second copy of the 2D kernel.
    std::shared_ptr<const SpatialEnergyVisualField> planarBase;
    std::vector<Point> map;
    std::vector<int> pixelIndices;
    std::vector<Point> sphere; // allocated only by explicit 3D inspection
    static Direction direction(float azimuth, float elevation) noexcept;
    static std::array<float, 2> project(float azimuth, float elevation) noexcept;
    static std::shared_ptr<const SpatialEnergyVisualField> prepare(
        std::shared_ptr<const SpatialEnergyMeter::Layout>, bool sphere,
        std::shared_ptr<const SpatialEnergyVisualField> cachedPlane = {});
    const std::vector<Point>& mapPoints() const noexcept { return planarBase ? planarBase->map : map; }
    const std::vector<int>& mapPixels() const noexcept { return planarBase ? planarBase->pixelIndices : pixelIndices; }
    static double power(const Point&, const SpatialEnergyMeter::Snapshot&) noexcept;
};

struct SpatialEnergyVisualSummary
{
    bool directional = false;
    double azimuth = 0, elevation = 0, spread = 0, peakDbfs = -INFINITY;
    int maximumOutput = -1, directionalCount = 0, unplaced = 0, lfe = 0;
    static SpatialEnergyVisualSummary from(const SpatialEnergyMeter::Snapshot&) noexcept;
};

// Renderer parameters only; never changes meter windows or audio state.
struct SpatialEnergyVisualResponseProfile
{
    int framesPerSecond = 20;
    double attackMs = 80, releaseMs = 240, stopHoldMs = 350, stopDecayDbPerSecond = 24;
    double floorDb = -60, ceilingDb = 0;
};
}
