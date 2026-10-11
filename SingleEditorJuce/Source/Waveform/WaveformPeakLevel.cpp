#include "Waveform/WaveformModel.h"

#include <cmath>

namespace singleeditor
{
WaveformModel::PeakLevelView WaveformModel::choosePeakLevel(const PeakCache& cache,
                                                          juce::Range<double> visibleTimeRange,
                                                          int targetPixelColumns) noexcept
{
    PeakLevelView view { cache.binCount, &cache.channelBins, &cache.mixedBins };
    if (cache.binCount <= 0 || !std::isfinite(cache.durationSeconds)
        || cache.durationSeconds <= 0.0 || targetPixelColumns <= 0)
        return view;

    constexpr double peakLodOversampleFactor = 1.5;
    const auto fraction = visibleTimeRange.getLength() / cache.durationSeconds;
    if (!std::isfinite(fraction))
        return view;
    const auto visibleRatio = juce::jlimit(0.000001, 1.0, fraction);
    // Keep this in double: deep zoom or large windows can request more bins
    // than an int can hold. Such requests must retain the finest available data.
    const auto desiredVisibleBins = juce::jmax(64.0,
        std::ceil(static_cast<double>(targetPixelColumns) * peakLodOversampleFactor));
    const auto desiredTotalBinCount = std::ceil(desiredVisibleBins / visibleRatio);
    for (const auto& level : cache.lodLevels)
    {
        if (level.binCount < desiredTotalBinCount)
            break;
        view = { level.binCount, &level.channelBins, &level.mixedBins };
    }
    return view;
}
}
