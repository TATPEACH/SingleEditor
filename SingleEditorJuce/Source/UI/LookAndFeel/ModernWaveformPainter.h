#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ModernGlassTokens.h"
#include "ModernTypography.h"
#include "BrandMark.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Components/WaveformPlotGeometry.h"

namespace singleeditor
{
struct ModernWaveformPainter final
{
    enum class PlaceholderState { empty, loading, failed };
    struct PlaceholderLayout
    {
        juce::Rectangle<float> icon, title, detail;
        float titleEm = 16.0f;
    };

    static PlaceholderLayout placeholderLayout(juce::Rectangle<float> available)
    {
        PlaceholderLayout result;
        if (available.isEmpty()) return result;
        const auto inset = juce::jmin(20.0f, available.getWidth() * 0.06f, available.getHeight() * 0.08f);
        const auto content = available.reduced(inset);
        const auto width = juce::jmin(420.0f, content.getWidth());
        result.titleEm = width < 220.0f ? 14.0f : 16.0f;
        const auto titleHeight = juce::jmin(content.getHeight(), width < 200.0f ? 42.0f : 28.0f);
        const auto iconHeight = content.getHeight() >= 180.0f ? 44.0f : 0.0f;
        const auto detailHeight = content.getHeight() >= 100.0f ? 54.0f : 0.0f;
        const auto totalHeight = titleHeight + (iconHeight > 0 ? iconHeight + 14.0f : 0.0f)
            + (detailHeight > 0 ? detailHeight + 6.0f : 0.0f);
        auto block = content.withSizeKeepingCentre(width, totalHeight);
        if (iconHeight > 0)
        {
            result.icon = block.removeFromTop(iconHeight).withSizeKeepingCentre(iconHeight, iconHeight);
            block.removeFromTop(14.0f);
        }
        result.title = block.removeFromTop(titleHeight);
        if (detailHeight > 0)
        {
            block.removeFromTop(6.0f);
            result.detail = block.removeFromTop(detailHeight);
        }
        return result;
    }

    // Used only before real peaks exist. An empty document is not a waveform,
    // and a pending reader has no percentage or synthetic amplitude to display.
    static void paintPlaceholder(juce::Graphics& g, juce::Rectangle<float> available,
                                 PlaceholderState state, const juce::String& fileName = {},
                                 const juce::String& loadingDetail = {})
    {
        const auto loading = state == PlaceholderState::loading;
        const auto failed = state == PlaceholderState::failed;
        const juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(available.toNearestInt());
        const auto layout = placeholderLayout(available);
        if (!layout.icon.isEmpty() && state == PlaceholderState::empty)
        {
            // A static brand mark, not a preview of audio that has not loaded.
            BrandMark::paint(g, layout.icon);
        }
        else if (!layout.icon.isEmpty())
        {
            g.setColour(failed ? RendererVisualStyle::danger().withAlpha(0.10f) : RendererVisualStyle::accentSoft());
            g.fillRoundedRectangle(layout.icon, 12.0f);
            auto page = layout.icon.reduced(13.0f, 10.0f);
            juce::Path outline;
            outline.startNewSubPath(page.getX(), page.getY());
            outline.lineTo(page.getRight() - 5.0f, page.getY());
            outline.lineTo(page.getRight(), page.getY() + 5.0f);
            outline.lineTo(page.getRight(), page.getBottom());
            outline.lineTo(page.getX(), page.getBottom());
            outline.closeSubPath();
            g.setColour(failed ? RendererVisualStyle::danger() : RendererVisualStyle::accentBlue());
            g.strokePath(outline, juce::PathStrokeType(1.4f));
            if (failed)
            {
                g.drawLine(page.getCentreX(), page.getCentreY() - 3, page.getCentreX(), page.getCentreY() + 2, 1.5f);
                g.fillEllipse(page.getCentreX() - 0.9f, page.getCentreY() + 5, 1.8f, 1.8f);
            }
            else
            {
                g.drawLine(page.getX() + 4, page.getCentreY() + 3, page.getRight() - 4, page.getCentreY() + 3, 1.4f);
                if (!loading)
                    g.drawLine(page.getCentreX(), page.getCentreY() - 1, page.getCentreX(), page.getCentreY() + 7, 1.4f);
            }
        }
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(layout.titleEm, juce::Font::bold));
        g.drawFittedText(failed ? "Could not open audio" : loading ? "Opening audio" : "Open audio", layout.title.toNearestInt(),
                        juce::Justification::centred, 2, 1.0f);
        if (!layout.detail.isEmpty())
        {
            auto detail = loading || failed ? fileName : juce::String("Drop an audio file here, or use Open in the toolbar.");
            if ((loading || failed) && loadingDetail.isNotEmpty())
                detail += (detail.isNotEmpty() ? "\n" : "") + loadingDetail;
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(12.0f));
            g.drawFittedText(detail, layout.detail.toNearestInt(), juce::Justification::centred, 3, 1.0f);
        }
    }

    struct SpectralStatusLayout
    {
        juce::Rectangle<float> background, icon, text;
        juce::String message;
        float textEm = 11.0f;
    };

    static SpectralStatusLayout spectralStatusLayout(juce::Rectangle<float> available, bool failed = false)
    {
        SpectralStatusLayout result;
        if (available.getWidth() < 20 || available.getHeight() < 16) return result;
        const auto content = available.reduced(juce::jmin(4.0f, available.getHeight() * 0.08f));
        const auto height = juce::jmin(26.0f, content.getHeight());
        result.textEm = height < 24 ? 10.0f : 11.0f;
        const auto font = ModernTypography::font(result.textEm);
        result.message = failed ? "Spectrum unavailable. View > Spectral to retry" : "Preparing spectrum";
        auto textWidth = juce::GlyphArrangement::getStringWidth(font, result.message);
        if (failed && textWidth + 38 > content.getWidth())
        {
            result.message = "Spectrum unavailable";
            textWidth = juce::GlyphArrangement::getStringWidth(font, result.message);
        }
        if (textWidth + 38 > content.getWidth())
        {
            result.message = failed ? "Unavailable" : "Preparing";
            textWidth = juce::GlyphArrangement::getStringWidth(font, result.message);
        }
        if (height < 18 || textWidth + 38 > content.getWidth())
        {
            result.message.clear();
            textWidth = 0;
        }
        const auto width = juce::jmin(content.getWidth(), textWidth > 0 ? textWidth + 38.0f : 26.0f);
        result.background = { content.getRight() - width, content.getY(), width, height };
        const auto inner = result.background.reduced(6, 3);
        result.icon = inner.withWidth(juce::jmin(10.0f, inner.getWidth()))
            .withSizeKeepingCentre(juce::jmin(10.0f, inner.getWidth()), juce::jmin(12.0f, inner.getHeight()));
        if (result.message.isNotEmpty())
            result.text = { result.icon.getRight() + 8, result.background.getY(),
                            textWidth + 2, result.background.getHeight() };
        return result;
    }

    static void paintSpectralStatus(juce::Graphics& g, juce::Rectangle<float> available, bool failed = false)
    {
        const juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(available.toNearestInt());
        const auto layout = spectralStatusLayout(available, failed);
        if (layout.background.isEmpty()) return;
        g.setColour(RendererVisualStyle::panelBackground().withAlpha(0.96f));
        g.fillRoundedRectangle(layout.background, 6.0f);
        g.setColour(RendererVisualStyle::borderColour());
        g.drawRoundedRectangle(layout.background.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(failed ? RendererVisualStyle::danger() : RendererVisualStyle::accentBlue());
        if (failed)
        {
            g.drawLine(layout.icon.getCentreX(), layout.icon.getY(),
                       layout.icon.getCentreX(), layout.icon.getBottom() - 4, 1.6f);
            g.fillEllipse(layout.icon.getCentreX() - 1, layout.icon.getBottom() - 2, 2, 2);
        }
        else for (int bar = 0; bar < 3; ++bar)
        {
            const auto x = layout.icon.getX() + layout.icon.getWidth() * (0.15f + 0.35f * bar);
            const auto barHeight = layout.icon.getHeight() * (bar == 1 ? 1.0f : 0.55f);
            g.drawLine(x, layout.icon.getBottom() - barHeight, x, layout.icon.getBottom(), 1.2f);
        }
        if (!layout.text.isEmpty())
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(layout.textEm));
            g.drawText(layout.message, layout.text, juce::Justification::centredLeft, false);
        }
    }

    static void paintSpectralAxis(juce::Graphics& g, juce::Rectangle<float> plot, double sampleRate)
    {
        if (plot.getWidth() < 180 || plot.getHeight() < 58 || !std::isfinite(sampleRate) || sampleRate <= 0)
            return;
        const juce::Graphics::ScopedSaveState saved(g);
        g.reduceClipRegion(plot.toNearestInt());
        const auto lane = WaveformPlotGeometry::content(plot, true);
        g.setFont(ModernTypography::font(10.0f));
        g.setColour(RendererVisualStyle::secondaryText());
        g.drawText("Hz", WaveformPlotGeometry::spectralUnitBounds(lane), juce::Justification::centredLeft, false);
        for (const auto& tick : WaveformPlotGeometry::frequencyTicks(lane, sampleRate))
        {
            g.setColour(juce::Colours::white.withAlpha(0.09f));
            g.drawLine(lane.getX(), tick.y, lane.getRight(), tick.y, 0.6f);
            g.setColour(RendererVisualStyle::secondaryText());
            g.drawLine(lane.getRight(), tick.y, lane.getRight() + 3, tick.y, 1.0f);
            g.drawText(tick.text, tick.label, juce::Justification::centredLeft, false);
        }
    }

    // Prepare the lane and the brush used for the existing real min/max peaks.
    // No resampling, smoothing, synthetic envelope or replacement waveform.
    static void prepareLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour colour)
    {
        g.setColour(RendererVisualStyle::waveformSurface());
        g.fillRect(lane);
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.32f));
        for (const float fraction : { 0.25f, 0.75f })
            g.drawHorizontalLine(juce::roundToInt(lane.getY() + lane.getHeight() * fraction),
                                  lane.getX(), lane.getRight());
        g.setColour(colour.withAlpha(0.20f));
        g.drawHorizontalLine(juce::roundToInt(lane.getCentreY()), lane.getX(), lane.getRight());

        const auto edge = colour.interpolatedWith(juce::Colour(RendererVisualStyle::isNight() ? 0xff365775 : 0xffe1edff), 0.60f);
        juce::ColourGradient peaks(edge.withAlpha(0.38f), lane.getCentreX(), lane.getY(),
                                   edge.withAlpha(0.38f), lane.getCentreX(), lane.getBottom(), false);
        peaks.addColour(0.45, colour.withAlpha(RendererVisualStyle::isNight() ? 0.82f : 0.70f));
        peaks.addColour(0.55, colour.withAlpha(RendererVisualStyle::isNight() ? 0.82f : 0.70f));
        g.setGradientFill(peaks);
    }
};
} // namespace singleeditor
