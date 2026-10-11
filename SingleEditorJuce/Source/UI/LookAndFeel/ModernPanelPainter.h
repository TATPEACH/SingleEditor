#pragma once

#include "UI/Renderer/RendererVisualStyle.h"
#include "ModernTypography.h"

namespace singleeditor
{
struct ModernPanelPainter final
{
    enum class Icon { playlist, information, meter, equalizer, waveform, markers };
    static constexpr int headerHeight = 32;

    static void paint(juce::Graphics& g, juce::Rectangle<int> bounds,
                      const juce::String& title, Icon icon, int trailingControls = 0)
    {
        const auto body = bounds.toFloat().reduced(0.5f);
        g.setColour(RendererVisualStyle::panelBackground());
        g.fillRoundedRectangle(body, 10.0f);
        auto header = bounds.removeFromTop(juce::jmin(headerHeight, bounds.getHeight()));
        g.setColour(RendererVisualStyle::borderColour().withAlpha(0.6f));
        g.fillRect(header.getX() + 1, header.getBottom() - 1, juce::jmax(0, header.getWidth() - 2), 1);
        const auto glyph = juce::Rectangle<float>(static_cast<float>(header.getX() + 10),
            static_cast<float>(header.getCentreY() - 6), 12, 12);
        g.setColour(RendererVisualStyle::accentBlue());
        if (icon == Icon::waveform)
        {
            juce::Path line;
            line.startNewSubPath(glyph.getX(), glyph.getCentreY());
            line.lineTo(glyph.getX() + 2, glyph.getCentreY());
            line.lineTo(glyph.getX() + 4, glyph.getY());
            line.lineTo(glyph.getX() + 6, glyph.getBottom());
            line.lineTo(glyph.getX() + 8, glyph.getY() + 3);
            line.lineTo(glyph.getX() + 10, glyph.getCentreY());
            line.lineTo(glyph.getRight(), glyph.getCentreY());
            g.strokePath(line, juce::PathStrokeType(1.1f));
        }
        else if (icon == Icon::markers)
        {
            g.drawVerticalLine(juce::roundToInt(glyph.getX() + 2), glyph.getY(), glyph.getBottom());
            juce::Path flag;
            flag.startNewSubPath(glyph.getX() + 2, glyph.getY() + 1);
            flag.lineTo(glyph.getRight() - 1, glyph.getY() + 1);
            flag.lineTo(glyph.getRight() - 3, glyph.getY() + 4);
            flag.lineTo(glyph.getRight() - 1, glyph.getY() + 7);
            flag.lineTo(glyph.getX() + 2, glyph.getY() + 7);
            g.strokePath(flag, juce::PathStrokeType(1.1f));
        }
        else if (icon == Icon::equalizer)
        {
            for (int i = 0; i < 3; ++i)
            {
                const auto x = glyph.getX() + 1.0f + i * 4.5f;
                const auto y = glyph.getY() + (i == 1 ? 8.0f : 4.0f);
                g.drawLine(x, glyph.getY(), x, glyph.getBottom(), 1.0f);
                g.fillRoundedRectangle(x - 1.5f, y - 1.5f, 3, 3, 1);
            }
        }
        else if (icon == Icon::information)
        {
            g.drawEllipse(glyph, 1.1f);
            g.fillEllipse(glyph.getCentreX() - 0.65f, glyph.getY() + 2.4f, 1.3f, 1.3f);
            g.drawVerticalLine(juce::roundToInt(glyph.getCentreX()), glyph.getY() + 5, glyph.getBottom() - 2);
        }
        else if (icon == Icon::meter)
        {
            for (int bar = 0; bar < 3; ++bar)
            {
                const float height = bar == 1 ? 12.0f : bar == 0 ? 7.0f : 9.0f;
                g.fillRoundedRectangle(glyph.getX() + static_cast<float>(bar) * 4.0f,
                    glyph.getBottom() - height, 2.0f, height, 1.0f);
            }
        }
        else
        {
            g.drawRoundedRectangle(glyph.withWidth(9).withX(glyph.getX() + 3), 1.5f, 1.0f);
            for (int row = 0; row < 3; ++row)
            {
                const auto y = glyph.getY() + 3 + static_cast<float>(row) * 3;
                g.drawLine(glyph.getX(), y, glyph.getX() + 1, y, 1.2f);
                g.drawLine(glyph.getX() + 5, y, glyph.getRight() - 2, y, 1.0f);
            }
        }
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(13.0f, juce::Font::bold));
        g.drawText(title, header.withTrimmedLeft(30).withTrimmedRight(30 + trailingControls),
                   juce::Justification::centredLeft, true);
    }
};
} // namespace singleeditor
