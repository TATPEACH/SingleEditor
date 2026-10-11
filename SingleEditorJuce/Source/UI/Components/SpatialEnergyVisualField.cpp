#include "SpatialEnergyVisualField.h"
#include <algorithm>

namespace singleeditor
{
namespace
{
constexpr double pi = 3.14159265358979323846;
void weights(SpatialEnergyVisualField::Point& point, const SpatialEnergyMeter::Layout& layout)
{
    for (std::size_t i = 0; i < layout.channels.size() && i < point.weights.size(); ++i)
    {
        const auto& c = layout.channels[i];
        if (!layout.geometryCurrent || c.placement != SpatialEnergyMeter::Placement::directional) continue;
        const auto d = SpatialEnergyVisualField::direction(c.azimuthDegrees, c.elevationDegrees);
        const auto dot = point.direction.x*d.x + point.direction.y*d.y + point.direction.z*d.z;
        // A compact 60-degree cap, unit gain at the speaker anchor. Independent
        // powers add; no phase or acoustic-pressure/room model is implied.
        const auto w = std::clamp((dot - 0.5f) * 2.0f, 0.0f, 1.0f);
        point.weights[i] = w*w;
    }
}
}
SpatialEnergyVisualField::Direction SpatialEnergyVisualField::direction(float az, float el) noexcept
{
    const auto a = az*pi/180.0, e = el*pi/180.0;
    // Project convention: positive azimuth is listener-left (ACN geometry).
    return {static_cast<float>(-std::sin(a)*std::cos(e)), static_cast<float>(std::sin(e)),
            static_cast<float>(std::cos(a)*std::cos(e))};
}
std::array<float, 2> SpatialEnergyVisualField::project(float az, float el) noexcept
{
    const auto a = -az*pi/180.0, e = el*pi/180.0;
    const auto den = std::sqrt(1.0+std::cos(e)*std::cos(a/2));
    return {static_cast<float>(std::cos(e)*std::sin(a/2)/den), static_cast<float>(std::sin(e)/den)};
}
std::shared_ptr<const SpatialEnergyVisualField> SpatialEnergyVisualField::prepare(
    std::shared_ptr<const SpatialEnergyMeter::Layout> layout, bool includeSphere,
    std::shared_ptr<const SpatialEnergyVisualField> cachedPlane)
{
    if (cachedPlane && cachedPlane->planarBase) cachedPlane=cachedPlane->planarBase;
    if (includeSphere)
    {
        auto base=prepare(layout,false,std::move(cachedPlane));
        if (!base->layout || !base->layout->geometryCurrent) return base;
        auto result=std::make_shared<SpatialEnergyVisualField>();
        result->layout=base->layout; result->planarBase=std::move(base);
        // Equal-area latitude rings. Camera projection stays local to inspection.
        for (int y = 0; y < 24; ++y) for (int x = 0; x < 48; ++x)
        {
            Point p; p.direction = direction((x+0.5f)*360/48-180,
                static_cast<float>(std::asin((y+0.5)*2/24-1)*180/pi));
            weights(p, *result->layout); result->sphere.push_back(p);
        }
        return result;
    }
    if (cachedPlane && cachedPlane->layout==layout && cachedPlane->sphere.empty()) return cachedPlane;
    auto result = std::make_shared<SpatialEnergyVisualField>(); result->layout = std::move(layout);
    if (!result->layout || !result->layout->geometryCurrent) return result;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    {
        Point p; p.x = (x+0.5f)*2/width-1; p.y = 1-(y+0.5f)*2/height;
        if (p.x*p.x+p.y*p.y > 1) continue;
        const auto z = std::sqrt(1-(p.x*p.x+p.y*p.y)/2);
        const auto az = -2*std::atan2(std::sqrt(2.0)*z*p.x, 2*z*z-1);
        const auto el = std::asin(std::clamp(std::sqrt(2.0)*z*p.y, -1.0, 1.0));
        p.direction = direction(static_cast<float>(az*180/pi), static_cast<float>(el*180/pi));
        weights(p, *result->layout); result->map.push_back(p); result->pixelIndices.push_back(y*width+x);
    }
    return result;
}
double SpatialEnergyVisualField::power(const Point& point, const SpatialEnergyMeter::Snapshot& snapshot) noexcept
{
    double result = 0;
    for (std::size_t i = 0; i < snapshot.channels.size() && i < point.weights.size(); ++i)
        result += point.weights[i] * snapshot.channels[i].kWeightedMeanSquare;
    return result;
}
SpatialEnergyVisualSummary SpatialEnergyVisualSummary::from(const SpatialEnergyMeter::Snapshot& s) noexcept
{
    SpatialEnergyVisualSummary r;
    if (!s.layout) return r;
    double total = 0, x = 0, y = 0, z = 0, maximum = -1;
    for (std::size_t i = 0; i < s.channels.size() && i < s.layout->channels.size(); ++i)
    {
        const auto& c = s.layout->channels[i]; const auto& v = s.channels[i];
        r.peakDbfs = std::max(r.peakDbfs, v.samplePeakDbfs);
        if (v.kWeightedMeanSquare > maximum) { maximum = v.kWeightedMeanSquare; r.maximumOutput = c.physicalOutput; }
        if (c.placement == SpatialEnergyMeter::Placement::lfe) { ++r.lfe; continue; }
        if (!s.layout->geometryCurrent || c.placement != SpatialEnergyMeter::Placement::directional) { ++r.unplaced; continue; }
        ++r.directionalCount;
        const auto d = SpatialEnergyVisualField::direction(c.azimuthDegrees, c.elevationDegrees);
        total += v.kWeightedMeanSquare;
        x += d.x*v.kWeightedMeanSquare; y += d.y*v.kWeightedMeanSquare; z += d.z*v.kWeightedMeanSquare;
    }
    if (total > 0)
    {
        const auto magnitude = std::sqrt(x*x+y*y+z*z);
        r.spread = std::clamp(1-magnitude/total, 0.0, 1.0);
        r.directional = magnitude > total*1e-6;
        r.azimuth = std::atan2(-x,z)*180/pi;
        r.elevation = std::atan2(y,std::sqrt(x*x+z*z))*180/pi;
    }
    return r;
}
}
