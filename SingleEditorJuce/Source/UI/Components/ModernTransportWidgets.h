#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include <optional>
#include <cmath>

namespace singleeditor
{
// The existing Label owns the text, tooltip and accessibility value. Only the
// full Modern presentation splits the existing timecode into visual tiers.
class ModernTransportTimeLabel final : public juce::Label
{
public:
    struct TimeParts { juce::String main, frames; };
    static std::optional<TimeParts> splitTimecode(const juce::String& text)
    {
        if (!text.containsOnly("0123456789:")) return {};
        const auto fields = juce::StringArray::fromTokens(text, ":", {});
        if (fields.size() != 4 || fields[0].isEmpty()
            || fields[1].length() != 2 || fields[2].length() != 2 || fields[3].length() != 2)
            return {};
        return TimeParts { text.upToLastOccurrenceOf(":", false, false), ":" + fields[3] };
    }
    void setModernPresentation(bool enabled)
    {
        if (modernPresentation == enabled) return;
        modernPresentation = enabled;
        repaint();
    }
    void paint(juce::Graphics& g) override
    {
        const auto parts = splitTimecode(getText());
        if (!modernPresentation || RendererVisualStyle::isClassic() || !parts)
        {
            juce::Label::paint(g);
            return;
        }
        auto bounds = getLocalBounds().toFloat();
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.6f));
        g.drawVerticalLine(0, 5.0f, bounds.getBottom() - 5.0f);
        bounds.removeFromLeft(10.0f);
        bounds.removeFromRight(2.0f);
        const auto face = juce::Font::getDefaultMonospacedFontName();
        auto major = juce::Font(juce::FontOptions(face, 24.0f, juce::Font::plain).withPointHeight(24.0f));
        const auto minor = juce::Font(juce::FontOptions(face, 12.0f, juce::Font::plain).withPointHeight(12.0f));
        const auto minorWidth = juce::GlyphArrangement::getStringWidth(minor, parts->frames);
        const auto available = juce::jmax(1.0f, bounds.getWidth() - minorWidth - 2.0f);
        const auto naturalWidth = juce::GlyphArrangement::getStringWidth(major, parts->main);
        if (naturalWidth > available) major.setHeight(major.getHeight() * available / naturalWidth);
        const auto majorWidth = juce::GlyphArrangement::getStringWidth(major, parts->main);
        const auto x = bounds.getX() + juce::jmax(0.0f, (bounds.getWidth() - majorWidth - minorWidth - 2) * 0.5f);
        const auto baseline = bounds.getCentreY() + major.getAscent() * 0.5f - major.getDescent() * 0.5f;
        g.setFont(major);
        g.setColour(RendererVisualStyle::accentBlue());
        g.drawSingleLineText(parts->main, juce::roundToInt(x), juce::roundToInt(baseline));
        g.setFont(minor);
        g.setColour(RendererVisualStyle::primaryText());
        g.drawSingleLineText(parts->frames, juce::roundToInt(x + majorWidth + 2), juce::roundToInt(baseline));
    }
private:
    bool modernPresentation = false;
};

class ModernTransportMeterLabel final : public juce::Label
{
public:
    void paint(juce::Graphics& g) override
    {
        if (RendererVisualStyle::isClassic()) { juce::Label::paint(g); return; }
        auto text = getText();
        // The adjacent selector already names the mode. Keep the complete
        // original value in the Label for accessibility and shared callers.
        if (text.startsWith("TP ") || text.startsWith("RMS ") || text.startsWith("LUFS "))
            text = text.fromFirstOccurrenceOf(" ", false, false);
        g.setFont(ModernTypography::font(11.0f));
        g.setColour(RendererVisualStyle::accentBlue());
        g.drawText(text, getLocalBounds().reduced(2, 0), juce::Justification::centredRight, true);
    }
};

// The Label retains the complete source text for callers and accessibility.
// Modern status text uses a fixed readable size; optional concise presentation
// removes toolbar duplicates without squeezing a long sentence into thin glyphs.
class ModernStatusLabel final : public juce::Label
{
public:
    void setModernPresentation(bool enabled)
    {
        if (modernPresentation == enabled) return;
        modernPresentation = enabled;
        repaint();
    }
    void setModernText(juce::String text)
    {
        if (modernText == text) return;
        modernText = std::move(text);
        repaint();
    }
    juce::String textForPresentation() const
    {
        return modernPresentation && !RendererVisualStyle::isClassic() && modernText.isNotEmpty()
            ? modernText : getText();
    }
    juce::String getTooltip() override
    {
        return modernPresentation && !RendererVisualStyle::isClassic() ? getText() : juce::Label::getTooltip();
    }
    void paint(juce::Graphics& g) override
    {
        if (!modernPresentation || RendererVisualStyle::isClassic())
        {
            juce::Label::paint(g);
            return;
        }
        g.setFont(ModernTypography::font(11.0f));
        g.setColour(findColour(juce::Label::textColourId).withMultipliedAlpha(isEnabled() ? 1.0f : 0.5f));
        g.drawText(textForPresentation(), getLocalBounds().reduced(2, 0), getJustificationType(), true);
    }
private:
    bool modernPresentation = false;
    juce::String modernText;
};

class ModernThemeButton final : public juce::TextButton
{
public:
    explicit ModernThemeButton(bool night)
        : juce::TextButton(night ? "Night" : "Light"), nightIcon(night)
    {
        setTitle(night ? "Modern Glass Night theme" : "Modern Glass Light theme");
        setTooltip(getTitle());
        setComponentID(night ? "appearance.theme.night" : "appearance.theme.light");
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        getLookAndFeel().drawButtonBackground(g, *this, findColour(buttonColourId), highlighted, down);
        g.setColour(getToggleState() ? juce::Colours::white : RendererVisualStyle::primaryText());
        const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(16, 16);
        juce::Path icon;
        if (nightIcon)
        {
            icon.startNewSubPath(box.getX() + 9, box.getY() + 1);
            icon.cubicTo(box.getX() - 1, box.getY() + 1, box.getX() - 1, box.getBottom() + 1,
                         box.getX() + 9, box.getBottom() - 1);
            icon.cubicTo(box.getRight() - 1, box.getBottom() - 2, box.getRight(), box.getY() + 9,
                         box.getRight() - 1, box.getY() + 8);
            icon.cubicTo(box.getX() + 8, box.getY() + 12, box.getX() + 5, box.getY() + 5,
                         box.getX() + 9, box.getY() + 1);
            icon.closeSubPath();
        }
        else
        {
            icon.addEllipse(box.reduced(4.5f));
            for (int ray = 0; ray < 8; ++ray)
            {
                const auto angle = juce::MathConstants<float>::twoPi * ray / 8.0f;
                const auto direction = juce::Point<float>(std::cos(angle), std::sin(angle));
                icon.startNewSubPath(box.getCentre() + direction * 6.0f);
                icon.lineTo(box.getCentre() + direction * 7.7f);
            }
        }
        g.strokePath(icon, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
private:
    bool nightIcon;
};
} // namespace singleeditor
