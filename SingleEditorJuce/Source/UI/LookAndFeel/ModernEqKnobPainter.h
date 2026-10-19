#pragma once

#include <cmath>
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
// Native vector counterpart of OD's small pearl dial. The face stays lit from
// above while the indicator follows the real Slider value; no bitmap is loaded.
struct ModernEqKnobPainter final
{
    static juce::Colour bandColour(int index)
    {
        constexpr std::array<juce::uint32, 8> light {
            0xffc69a03, 0xffd69324, 0xffc566cb, 0xff5480c6,
            0xff2aa986, 0xff4298cd, 0xff8063cc, 0xffc45cb5
        };
        constexpr std::array<juce::uint32, 8> night {
            0xffe7be46, 0xffefb762, 0xffdf8de4, 0xff87aaed,
            0xff65cca8, 0xff75bef2, 0xffad97f2, 0xffec8dd8
        };
        return juce::Colour((RendererVisualStyle::isNight() ? night : light)[static_cast<size_t>(std::clamp(index, 0, 7))]);
    }

    static juce::Rectangle<float> dialBounds(juce::Rectangle<float> available, float preferredDiameter)
    {
        const auto diameter = juce::jmax(0.0f, juce::jmin(38.0f, preferredDiameter,
            juce::jmin(available.getWidth(), available.getHeight()) - 4.0f));
        return available.withSizeKeepingCentre(diameter, diameter);
    }

    static void draw(juce::Graphics& g, juce::Rectangle<float> available,
                     float position, float startAngle, float endAngle, juce::Slider& slider)
    {
        const auto dial = dialBounds(available, static_cast<float>(slider.getProperties().getWithDefault("modernKnobDiameter", 32.0)));
        if (dial.getWidth() < 5.0f) return;
        if (!SkinStyle::isOriginal())
        {
            SkinStyle::paintKnob(g, dial, startAngle + std::clamp(position, 0.0f, 1.0f) * (endAngle - startAngle),
                                 bandColour(static_cast<int>(slider.getProperties()["eqBandIndex"])),
                                 slider.isEnabled(), slider.hasKeyboardFocus(false));
            return;
        }
        juce::Graphics::ScopedSaveState save(g);
        const bool night = RendererVisualStyle::isNight();
        const auto accent = bandColour(static_cast<int>(slider.getProperties()["eqBandIndex"]));
        const auto enabled = slider.isEnabled();
        const auto centre = dial.getCentre();
        for (int i = 3; i > 0; --i)
        {
            g.setColour(juce::Colours::black.withAlpha(enabled ? (night ? 0.09f : 0.035f) : 0.02f));
            g.fillEllipse(dial.translated(0, static_cast<float>(i)).expanded(i * 0.3f));
        }
        juce::ColourGradient rim(juce::Colour(night ? 0xff56697d : 0xfffdfeff), centre.x, dial.getY(),
                                 juce::Colour(night ? 0xff13202e : 0xffaeb9c7), centre.x, dial.getBottom(), false);
        rim.addColour(0.48, juce::Colour(night ? 0xff26394c : 0xffd9dfe7));
        g.setGradientFill(rim);
        g.fillEllipse(dial);
        const auto face = dial.reduced(1.6f);
        juce::ColourGradient pearl(juce::Colour(night ? 0xff35485b : 0xffffffff), face.getX(), face.getY(),
                                   juce::Colour(night ? 0xff142334 : 0xffd9e0ea), face.getRight(), face.getBottom(), false);
        pearl.addColour(0.5, juce::Colour(night ? 0xff243548 : 0xfff1f4f8));
        g.setGradientFill(pearl);
        g.fillEllipse(face);
        g.setColour(juce::Colours::white.withAlpha(night ? 0.14f : 0.8f));
        g.drawEllipse(face.reduced(0.3f), 0.65f);
        const auto radius = dial.getWidth() * 0.5f;
        const auto angle = startAngle + std::clamp(position, 0.0f, 1.0f) * (endAngle - startAngle);
        const juce::Point<float> direction(std::sin(angle), -std::cos(angle));
        g.setColour(enabled ? accent : RendererVisualStyle::secondaryText().withAlpha(0.45f));
        g.drawLine({ centre + direction * radius * 0.48f, centre + direction * radius * 0.77f }, juce::jmax(1.25f, dial.getWidth() * 0.045f));
        if (slider.hasKeyboardFocus(false))
        {
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.8f));
            g.drawEllipse(dial.expanded(1.5f), 1.0f);
        }
    }
};
} // namespace singleeditor
