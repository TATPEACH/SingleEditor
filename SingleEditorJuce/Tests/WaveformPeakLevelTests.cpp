#include "Waveform/WaveformModel.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

void runWaveformPeakLevelTests()
{
    using Model = singleeditor::WaveformModel;
    Model::PeakCache cache;
    cache.binCount = 65536;
    cache.durationSeconds = 240.0;
    for (int bins = cache.binCount / 2; bins >= 128; bins /= 2)
        cache.lodLevels.push_back({ bins, {}, {} });

    const auto check = [&] (bool ok, const char* message)
    {
        if (!ok)
        {
            std::cerr << "WaveformPeakLevelTests failed: " << message << '\n';
            std::exit(1);
        }
    };

    // Exercise the production chooser at actual compact/maximized widths.
    check(Model::choosePeakLevel(cache, { 0.0, 240.0 }, 1720).binCount == 4096,
          "a maximized full-file waveform must not collapse to 128 coarse blocks");
    int previous = 0;
    for (int width = 140; width <= 7680; ++width)
    {
        const auto level = Model::choosePeakLevel(cache, { 0.0, 240.0 }, width);
        check(level.binCount >= std::ceil(width * 1.5), "at least 1.5 real bins per visible pixel");
        check(level.binCount < width * 3.0, "use the coarsest sufficient level to bound paint work");
        check(level.binCount >= previous, "wider windows must never lose detail");
        previous = level.binCount;
    }
    check(Model::choosePeakLevel(cache, { 30.0, 60.0 }, 1720).binCount == 32768,
          "zooming into one eighth of the file selects proportionally finer data");
    const auto deepest = Model::choosePeakLevel(cache, { 0.0, 0.000001 }, 7680);
    check(deepest.binCount == cache.binCount && deepest.channelBins == &cache.channelBins,
          "extreme zoom falls back to the finest available real bins without overflow");
    check(Model::choosePeakLevel(cache, { 0.0, 240.0 }, std::numeric_limits<int>::max()).binCount == cache.binCount,
          "large pixel counts cannot overflow to a coarse level");
    cache.durationSeconds = 1.0 / 96000.0;
    check(Model::choosePeakLevel(cache, { 0.0, cache.durationSeconds }, 1720).binCount == 4096,
          "sub-millisecond files use a valid visible fraction");
    cache.durationSeconds = 0.0;
    check(Model::choosePeakLevel(cache, { 0.0, 1.0 }, 1720).mixedBins == &cache.mixedBins,
          "empty cache retains the existing safe fallback");
    std::cout << "WaveformPeakLevelTests passed (7541 widths, zoom, extreme counts, short file)\n";
}
