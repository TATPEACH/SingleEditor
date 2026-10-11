#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include "Waveform/SpectrogramMapping.h"

namespace singleeditor
{
// Shared by the drawing and editing paths, so an axis gutter cannot shift
// the waveform while leaving selection, channel buttons or envelope hits behind.
struct WaveformPlotGeometry final
{
    static juce::Rectangle<float> content(juce::Rectangle<float> plot, bool modern) noexcept
    {
        if (!modern) return plot.reduced(18.0f, 12.0f);
        auto result = plot.reduced(10.0f, 8.0f);
        if (plot.getWidth() >= 180) result.removeFromRight(26.0f);
        return result;
    }
    static float timeToX(double seconds, juce::Rectangle<float> bounds, juce::Range<double> visible) noexcept
    {
        if (visible.isEmpty()) return bounds.getX();
        return bounds.getX() + bounds.getWidth() * static_cast<float>(
            (juce::jlimit(visible.getStart(), visible.getEnd(), seconds) - visible.getStart()) / visible.getLength());
    }
    static double xToTime(float x, juce::Rectangle<float> bounds, juce::Range<double> visible) noexcept
    {
        if (visible.isEmpty() || bounds.getWidth() <= 0) return 0;
        return visible.getStart() + visible.getLength()
            * ((juce::jlimit(bounds.getX(), bounds.getRight(), x) - bounds.getX()) / bounds.getWidth());
    }
    static double rulerStep(double span, float width) noexcept
    {
        if (!std::isfinite(span) || span <= 0 || width <= 0) return 1;
        const auto desired = span / juce::jmax(1.0, static_cast<double>(width / 100.0f));
        constexpr std::array<double, 31> steps { 0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005,
            0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600,
            900, 1800, 3600, 7200, 14400, 21600, 43200, 86400, 172800 };
        for (const auto step : steps) if (step >= desired) return step;
        return std::ceil(desired / 86400.0) * 86400.0;
    }
    static juce::String formatTime(double seconds, double step)
    {
        const int precision = step < 0.001 ? 4 : 3;
        const int unitsPerSecond = precision == 4 ? 10000 : 1000;
        const auto total = std::isfinite(seconds)
            ? static_cast<int64_t>(std::llround(juce::jlimit(0.0, 9007199254740000.0 / unitsPerSecond, seconds) * unitsPerSecond)) : int64_t(0);
        const auto hours = total / (3600LL * unitsPerSecond);
        const auto minutes = (total / (60LL * unitsPerSecond)) % 60;
        const auto secs = (total / unitsPerSecond) % 60;
        auto result = (hours > 0 ? juce::String(hours).paddedLeft('0', 2) + ":" : juce::String())
            + juce::String(minutes).paddedLeft('0', 2) + ":" + juce::String(secs).paddedLeft('0', 2);
        if (step < 1) result += "." + juce::String(total % unitsPerSecond).paddedLeft('0', precision);
        return result;
    }
    static juce::Rectangle<float> rulerLabelBounds(float tickX, juce::Rectangle<float> ruler,
                                                   juce::Rectangle<float> waveform, float textWidth) noexcept
    {
        const auto available = juce::jmin(94.0f, waveform.getRight() - tickX - 3.0f);
        // A terminal tick can remain visible without presenting a misleading
        // fragment of its time value at the right edge.
        if (textWidth <= 0 || textWidth > available) return {};
        return { tickX + 3.0f, ruler.getY() + 1.0f, available, 14.0f };
    }
    struct AmplitudeTick
    {
        float y = 0, db = 0;
        juce::Rectangle<float> label;
        bool zero = false;
    };
    static float peakHalfHeight(juce::Rectangle<float> lane, float zoom) noexcept
    {
        return lane.getHeight() * 0.48f * zoom;
    }
    static float sampleToY(float value, juce::Rectangle<float> lane, float zoom) noexcept
    {
        return juce::jlimit(lane.getY(), lane.getBottom(), lane.getCentreY() - value * peakHalfHeight(lane, zoom));
    }
    static std::vector<AmplitudeTick> amplitudeTicks(juce::Rectangle<float> lane, float zoom)
    {
        std::vector<AmplitudeTick> ticks;
        if (lane.getHeight() < 24 || lane.getWidth() <= 0 || !std::isfinite(zoom) || zoom <= 0) return ticks;
        const auto add = [&](float y, float db, bool zero)
        {
            if (y < lane.getY() || y > lane.getBottom()) return;
            const juce::Rectangle<float> label(lane.getRight() + 5,
                juce::jlimit(lane.getY(), lane.getBottom() - 14.0f, y - 7.0f), 21, 14);
            for (const auto& tick : ticks)
                if (tick.label.expanded(0, 1).intersects(label)) return;
            ticks.push_back({ y, db, label, zero });
        };
        // A bipolar linear waveform has zero amplitude in the centre and equal
        // dB magnitudes on either side. 0 dBFS does not mean its centre line.
        if (lane.getHeight() >= 42) add(lane.getCentreY(), 0, true);
        const auto halfHeight = peakHalfHeight(lane, zoom);
        for (const float db : { 0.0f, 6.0f, 12.0f, -6.0f, -12.0f, -24.0f, -36.0f })
        {
            const auto offset = std::pow(10.0f, db * 0.05f) * halfHeight;
            add(lane.getCentreY() - offset, db, false);
            add(lane.getCentreY() + offset, db, false);
        }
        std::sort(ticks.begin(), ticks.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
        return ticks;
    }

    struct FrequencyTick
    {
        double frequency = 0;
        float y = 0;
        juce::Rectangle<float> label;
        juce::String text;
    };

    static juce::Rectangle<float> spectralUnitBounds(juce::Rectangle<float> lane) noexcept
    {
        return { lane.getRight() + 5, lane.getY(), 21, 14 };
    }

    static std::vector<FrequencyTick> frequencyTicks(juce::Rectangle<float> lane, double sampleRate)
    {
        std::vector<FrequencyTick> ticks;
        if (lane.getHeight() < 42 || lane.getWidth() <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0)
            return ticks;
        const auto unit = spectralUnitBounds(lane);
        // Add lower frequencies first, retaining only complete non-overlapping
        // labels. A short/high-rate view must not pretend its compressed bass
        // range has the same spacing as a logarithmic spectrum.
        for (const double frequency : { 0.0, 100.0, 200.0, 500.0, 1000.0, 2000.0,
                                       5000.0, 10000.0, 20000.0, 40000.0, 80000.0 })
        {
            if (frequency > SpectrogramMapping::maximumFrequency(sampleRate)) continue;
            const auto y = lane.getY() + lane.getHeight()
                * SpectrogramMapping::normalisedYForFrequency(frequency, sampleRate);
            const juce::Rectangle<float> label(lane.getRight() + 5,
                juce::jlimit(lane.getY(), lane.getBottom() - 14.0f, y - 7.0f), 21, 14);
            if (unit.expanded(0, 2).intersects(label)) continue;
            bool overlaps = false;
            for (const auto& tick : ticks)
                if (tick.label.expanded(0, 2).intersects(label)) { overlaps = true; break; }
            if (overlaps) continue;
            ticks.push_back({ frequency, y, label,
                frequency >= 1000 ? juce::String(juce::roundToInt(frequency / 1000)) + "k"
                                  : juce::String(juce::roundToInt(frequency)) });
        }
        std::sort(ticks.begin(), ticks.end(), [](const auto& a, const auto& b) { return a.y < b.y; });
        return ticks;
    }
};
} // namespace singleeditor
