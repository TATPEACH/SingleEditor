#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ModernGlassTokens.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
// OD chrome's static tint, rim light and contact shadow. Backdrop blur and
// refractive glass are separate capabilities, not implied by this painter.
struct ModernGlassPainter final
{
    static void focusOutline(juce::Graphics& g, juce::Rectangle<float> bounds,
                             float radius, bool focused, bool enabled)
    {
        if (!focused || !enabled || bounds.getWidth() < 5.0f || bounds.getHeight() < 5.0f)
            return;
        juce::Graphics::ScopedSaveState state(g);
        const auto ring = bounds.reduced(2.0f);
        const auto corner = juce::jlimit(0.0f, juce::jmin(ring.getWidth(), ring.getHeight()) * 0.5f,
                                        radius - 1.0f);
        // Separate the accent from a selected blue fill without extending past
        // the control bounds or changing its hit target.
        g.setColour(RendererVisualStyle::windowBackground());
        g.drawRoundedRectangle(ring, corner, 3.0f);
        g.setColour(RendererVisualStyle::accentBlue());
        g.drawRoundedRectangle(ring, corner, 1.5f);
    }

    static void surface(juce::Graphics& g, juce::Rectangle<float> bounds,
                        juce::Colour tint, float radius, bool pressed = false)
    {
        if (bounds.isEmpty()) return;
        juce::Graphics::ScopedSaveState state(g);
        const auto body = bounds.reduced(1.0f).withTrimmedBottom(1.0f);
        const bool night = RendererVisualStyle::isNight();
        const auto edge = juce::Colour(night ? 0xff5c7a9f : 0xff8197b5);
        // Vector contours avoid allocating a blurred image per control repaint.
        for (int step = 3; step > 0; --step)
        {
            g.setColour((night ? juce::Colours::black : edge).withAlpha(night ? 0.12f : (pressed ? 0.015f : 0.018f)));
            g.drawRoundedRectangle(body.translated(0.0f, step * 0.55f), radius, 1.2f);
        }
        const auto top = tint.interpolatedWith(juce::Colours::white, night ? (pressed ? 0.01f : 0.035f) : (pressed ? 0.02f : 0.48f));
        const auto bottom = tint.interpolatedWith(juce::Colour(night ? 0xff050e18 : 0xffb6cbe5), pressed ? 0.13f : 0.035f);
        juce::ColourGradient fill(top, body.getCentreX(), body.getY(),
                                  bottom, body.getCentreX(), body.getBottom(), false);
        fill.addColour(0.48, tint);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(body, radius);
        g.setColour(edge.withAlpha(pressed ? 0.30f : 0.23f));
        g.drawRoundedRectangle(body, radius, 0.8f);
        const auto inner = body.reduced(0.8f);
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(night ? (pressed ? 0.06f : 0.14f) : (pressed ? 0.25f : 0.94f)),
                                               inner.getCentreX(), inner.getY(),
                                               juce::Colours::white.withAlpha(0.0f),
                                               inner.getCentreX(), inner.getBottom(), false));
        g.drawRoundedRectangle(inner, juce::jmax(0.0f, radius - 0.8f), 0.8f);
    }

    static void control(juce::Graphics& g, juce::Rectangle<float> bounds,
                        bool enabled, bool hovered, bool pressed, bool selected)
    {
        const auto& palette = RendererVisualStyle::modernPalette();
        auto tint = juce::Colour(palette.surface);
        if (selected)
            tint = tint.interpolatedWith(juce::Colour(palette.accent), 0.16f);
        else if (hovered || pressed)
            tint = tint.interpolatedWith(juce::Colour(palette.accent), pressed ? 0.12f : 0.045f);
        if (!enabled) tint = juce::Colour(palette.control);
        surface(g, bounds, tint, ModernGlassTokens::controlRadius, enabled && pressed);
    }
};
} // namespace singleeditor
