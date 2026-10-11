#pragma once

#include "UI/Components/OutputMeterDisplayState.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"

namespace singleeditor
{
struct ModernOutputMeterPainter
{
    struct Layout
    {
        juce::Rectangle<int> primary, reset, secondary, topology, channelLabels, scale;
        std::vector<juce::Rectangle<float>> bars;
    };
    static Layout layout(juce::Rectangle<int> bounds, int channels)
    {
        Layout result;
        auto body = bounds.withTrimmedTop(juce::jmin(32, bounds.getHeight())).reduced(12, 7);
        if (body.isEmpty()) return result;
        result.primary = body.removeFromTop(juce::jmin(36, body.getHeight()));
        if (result.primary.getWidth() >= 110 && result.primary.getHeight() >= 30)
            result.reset = result.primary.removeFromRight(44).reduced(0, 6);
        if (bounds.getHeight() >= 150)
        {
            result.topology = body.removeFromBottom(14);
            body.removeFromBottom(4);
        }
        if (bounds.getHeight() >= 260)
        {
            result.secondary = body.removeFromTop(44);
            body.removeFromTop(8);
        }
        body.removeFromTop(4);
        if (body.getHeight() < 20 || channels <= 0) return result;
        channels = juce::jlimit(1, OutputMeterSnapshot::maxChannels, channels);
        if (body.getHeight() >= 32)
            result.channelLabels = body.removeFromBottom(12);
        // Keep the scale and bars centred under the readouts. A right-anchored
        // fixed-width cluster leaves a diagonal gap in a tall, growing Mag.
        const int clusterWidth = juce::jmin(body.getWidth(), 26 + channels * 32);
        body = body.withSizeKeepingCentre(clusterWidth, body.getHeight());
        result.channelLabels.setX(body.getX());
        result.channelLabels.setWidth(body.getWidth());
        result.scale = body.removeFromLeft(juce::jmin(26, body.getWidth() / 4));
        const auto slot = static_cast<float>(body.getWidth()) / static_cast<float>(channels);
        const auto barWidth = juce::jlimit(1.0f, 12.0f, slot * 0.46f);
        for (int ch = 0; ch < channels; ++ch)
            result.bars.emplace_back(static_cast<float>(body.getX()) + slot * (static_cast<float>(ch) + 0.5f) - barWidth * 0.5f,
                static_cast<float>(body.getY()), barWidth, static_cast<float>(body.getHeight()));
        return result;
    }
    static juce::String valueText(float value, const juce::String& unit)
    {
        return (std::isfinite(value) && value > -119.0f ? juce::String(value, 1) : juce::String::fromUTF8("−∞")) + " " + unit;
    }
    static void paint(juce::Graphics& g, juce::Rectangle<int> bounds,
        const OutputMeterDisplayState& state, AudioEngine::MeterMode mode)
    {
        ModernPanelPainter::paint(g, bounds, "Output", ModernPanelPainter::Icon::meter);
        const auto geometry = layout(bounds, state.snapshot.channelCount);
        auto primary = geometry.primary;
        const bool tp = mode == AudioEngine::MeterMode::truePeak;
        const bool loudness = mode == AudioEngine::MeterMode::lufs;
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(11.0f));
        g.drawText(loudness ? "Momentary loudness" : tp ? "True peak" : "RMS max",
            primary.removeFromTop(14), juce::Justification::centredLeft, true);
        const auto value = loudness ? state.snapshot.momentaryLufs : OutputMeterDisplayState::maximum(state.snapshot, tp);
        g.setColour(RendererVisualStyle::accentInk());
        g.setFont(ModernTypography::font(13.0f, juce::Font::bold));
        g.drawText(state.snapshot.channelCount > 0 ? valueText(value, loudness ? "LUFS" : tp ? "dBTP" : "dBFS") : "--",
            primary, juce::Justification::centredLeft, true);
        if (!geometry.secondary.isEmpty())
        {
            auto rows = geometry.secondary;
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(11.0f));
            g.drawText("Peak hold   " + valueText(state.peakSinceResetDb, "dBTP"), rows.removeFromTop(20),
                juce::Justification::centredLeft, true);
            g.drawText(loudness ? "RMS max   " + valueText(OutputMeterDisplayState::maximum(state.snapshot, false), "dBFS")
                               : "Momentary   " + valueText(state.snapshot.momentaryLufs, "LUFS"),
                rows, juce::Justification::centredLeft, true);
        }
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.0f));
        g.drawText(state.snapshot.channelCount > 0 ? juce::String(state.snapshot.channelCount) + " device outputs"
            + (loudness ? " - K-weighted" : "") : "Device outputs sleeping",
            geometry.topology, juce::Justification::centredLeft, true);
        if (geometry.bars.empty()) return;

        const auto& first = geometry.bars.front();
        const auto floor = loudness ? -54.0f : -60.0f;
        const auto ceiling = tp ? 6.0f : 0.0f;
        const std::array<float, 5> ticks { ceiling, 0.0f, -12.0f, -36.0f, floor };
        float lastY = -1000.0f;
        for (auto tick : ticks)
        {
            const float y = first.getBottom() - first.getHeight() * OutputMeterDisplayState::normalise(tick, mode);
            if (y - lastY < 15.0f) continue;
            lastY = y;
            g.setColour(RendererVisualStyle::borderColour().withAlpha(0.42f));
            g.drawHorizontalLine(juce::roundToInt(y), static_cast<float>(geometry.scale.getRight()), geometry.bars.back().getRight());
            g.setColour(RendererVisualStyle::secondaryText());
            g.drawText(juce::String(juce::roundToInt(tick)), geometry.scale.withY(juce::roundToInt(y) - 6).withHeight(12),
                juce::Justification::centredLeft, true);
        }
        for (std::size_t ch = 0; ch < geometry.bars.size(); ++ch)
        {
            const auto bar = geometry.bars[ch];
            g.setColour(RendererVisualStyle::mutedSurface());
            g.fillRoundedRectangle(bar, 2.5f);
            juce::ColourGradient gradient(juce::Colour(0xff12b88a), bar.getBottomLeft(),
                juce::Colour(0xfff05b60), bar.getTopLeft(), false);
            gradient.addColour(loudness ? 0.74 : 0.82, juce::Colour(0xff42c58b));
            gradient.addColour(loudness ? 0.87 : 0.91, juce::Colour(0xffffbf59));
            const auto fill = bar.withTop(bar.getBottom() - bar.getHeight() * state.levels[ch]);
            if (fill.getHeight() >= 0.5f)
            {
                g.setGradientFill(gradient);
                g.fillRoundedRectangle(fill, 2.5f);
            }
            if (state.trails[ch] > 0.002f)
            {
                g.setColour(RendererVisualStyle::primaryText().withAlpha(0.62f));
                g.drawHorizontalLine(juce::roundToInt(bar.getBottom() - bar.getHeight() * state.trails[ch]), bar.getX(), bar.getRight());
            }
            const float slot = geometry.bars.size() > 1 ? geometry.bars[1].getX() - first.getX() : 20.0f;
            if (!geometry.channelLabels.isEmpty() && (slot >= 11.0f || ch % 2 == 0))
            {
                const auto label = slot >= 18.0f ? ChannelLayoutCatalog::roleToAbbreviation(state.snapshot.roles[ch])
                                               : juce::String(static_cast<int>(ch) + 1);
                g.setColour(RendererVisualStyle::secondaryText());
                g.setFont(juce::Font(juce::FontOptions(8.0f)));
                g.drawText(label, juce::Rectangle<float>(bar.getCentreX() - slot * 0.5f,
                    static_cast<float>(geometry.channelLabels.getY() + 3), slot, 11).toNearestInt(),
                    juce::Justification::centred, true);
            }
        }
    }
};
}
