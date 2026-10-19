#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "UI/LookAndFeel/StartupCoreSkin.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
// The startup instance draws only neutral window controls. It loads no renderer
// fonts/assets and keeps the same JUCE/Win32 peer when the full skin is attached.
class WindowChromeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr int captionHeight = 32;
    explicit WindowChromeLookAndFeel(bool modern = false) : modernPalette(modern) {}

    std::unique_ptr<juce::DropShadower> createDropShadowerForComponent(juce::Component& component) override
    {
#if JUCE_WINDOWS
        // During the base constructor the object is only a TopLevelWindow;
        // testing DocumentWindow misses that first allocation and starts the
        // software shadow's virtual-desktop watcher before the core window.
        if (dynamic_cast<juce::TopLevelWindow*>(&component) != nullptr && component.isOpaque())
            return nullptr; // DWM owns opaque window shadows; no software helper peers.
#endif
        return juce::LookAndFeel_V4::createDropShadowerForComponent(component);
    }

    struct Palette { juce::Colour background, text, border, hover; };
    static Palette palette(bool modern)
    {
        if (modern)
            return { RendererVisualStyle::windowBackground(), RendererVisualStyle::primaryText(),
                     RendererVisualStyle::borderColour(), RendererVisualStyle::mutedSurface() };

        const auto initial = StartupCoreSkin::palette(!RendererVisualStyle::isClassic(),
                                                      RendererVisualStyle::modernTheme());
        return { initial.windowBackground, initial.primaryText,
                 initial.secondaryText.withAlpha(0.2f), initial.panelBackground };
    }

    class CaptionButton final : public juce::Button
    {
    public:
        CaptionButton(int kindToUse, bool modern)
            : juce::Button(kindToUse == juce::DocumentWindow::closeButton ? "Close window"
                : kindToUse == juce::DocumentWindow::minimiseButton ? "Minimize window"
                : "Maximize or restore window"), kind(kindToUse), modernPalette(modern)
        {
            setTitle(getName());
            setComponentID(kind == juce::DocumentWindow::closeButton ? "application.close"
                : kind == juce::DocumentWindow::minimiseButton ? "application.window.minimize"
                : "application.window.toggle-maximize");
            setWantsKeyboardFocus(true);
        }

        void paintButton(juce::Graphics& g, bool hovered, bool down) override
        {
            const auto chrome = palette(modernPalette);
            const bool close = kind == juce::DocumentWindow::closeButton;
            const auto area = getLocalBounds().toFloat().reduced(2.0f, 3.0f);
            if (hovered || down || hasKeyboardFocus(false))
            {
                g.setColour(close && (hovered || down) ? juce::Colour(down ? 0xffc82e34 : 0xffe7464e)
                                                      : chrome.hover);
                g.fillRoundedRectangle(area, 5.0f);
            }
            g.setColour(close && (hovered || down) ? juce::Colours::white
                : chrome.text.withMultipliedAlpha(isEnabled() ? 0.76f : 0.35f));
            const auto icon = area.withSizeKeepingCentre(9.0f, 9.0f);
            juce::Path path;
            if (close)
            {
                path.startNewSubPath(icon.getTopLeft()); path.lineTo(icon.getBottomRight());
                path.startNewSubPath(icon.getTopRight()); path.lineTo(icon.getBottomLeft());
            }
            else if (kind == juce::DocumentWindow::minimiseButton)
            {
                path.startNewSubPath(icon.getX(), icon.getCentreY() + 2.0f);
                path.lineTo(icon.getRight(), icon.getCentreY() + 2.0f);
            }
            else if (getToggleState())
            {
                path.addRectangle(icon.withTrimmedTop(2).withTrimmedRight(2));
                path.startNewSubPath(icon.getX() + 2, icon.getY());
                path.lineTo(icon.getRight(), icon.getY());
                path.lineTo(icon.getRight(), icon.getBottom() - 2);
            }
            else path.addRectangle(icon);
            g.strokePath(path, juce::PathStrokeType(1.05f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }
    private:
        int kind;
        bool modernPalette;
    };

    juce::Button* createDocumentWindowButton(int type) override
    {
        return new CaptionButton(type, modernPalette);
    }

    void positionDocumentWindowButtons(juce::DocumentWindow&, int x, int y, int width, int height,
        juce::Button* minimise, juce::Button* maximise, juce::Button* close, bool onLeft) override
    {
        const int buttonWidth = juce::jmin(36, juce::jmax(1, (width - 8) / 3));
        auto edge = onLeft ? x + 4 : x + width - 4;
        for (auto* button : { close, maximise, minimise })
            if (button != nullptr)
            {
                if (!onLeft) edge -= buttonWidth;
                button->setBounds(edge, y, buttonWidth, height);
                if (onLeft) edge += buttonWidth;
            }
    }

    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g,
        int width, int height, int titleSpaceX, int titleSpaceWidth, const juce::Image*, bool) override
    {
        const auto chrome = palette(modernPalette);
        if (modernPalette && !SkinStyle::isOriginal())
        {
            SkinStyle::paintTitleBar(g, { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) },
                                     window.getTitleBarArea().getPosition().toFloat(),
                                     window.getLocalBounds().toFloat());
        }
        else
        {
            g.fillAll(chrome.background);
            g.setColour(chrome.border.withMultipliedAlpha(0.45f));
            g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
        }
        g.setColour(chrome.text.withMultipliedAlpha(window.isActiveWindow() ? 0.92f : 0.62f));
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText(window.getName(), juce::Rectangle<int>(titleSpaceX + 8, 0,
            juce::jmax(0, titleSpaceWidth - 16), height - 1), juce::Justification::centredLeft, true);
    }

    void drawResizableFrame(juce::Graphics& g, int width, int height,
        const juce::BorderSize<int>& border) override
    {
        // ResizableWindow already paints our themed frame. The separate resize
        // child otherwise adds LookAndFeel_V2's black outer and inner outlines.
        // Keep the child and its hit areas; suppress only its duplicate paint.
        if (RendererVisualStyle::isClassic())
            juce::LookAndFeel_V4::drawResizableFrame(g, width, height, border);
    }

    void drawResizableWindowBorder(juce::Graphics& g, int width, int height,
        const juce::BorderSize<int>&, juce::ResizableWindow& window) override
    {
        if (window.isFullScreen()) return;
        const auto chrome = palette(modernPalette);
        if (modernPalette && !SkinStyle::isOriginal())
        {
            const juce::Graphics::ScopedSaveState saved(g);
            g.excludeClipRegion({ 3, 3, juce::jmax(0, width - 6), juce::jmax(0, height - 6) });
            const auto frame = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
            SkinStyle::paintWindowBackground(g, frame, {}, frame);
        }
        else
        {
            g.setColour(chrome.background);
            g.drawRect(0, 0, width, height, 3);
        }
        g.setColour(chrome.border.withMultipliedAlpha(0.55f));
        g.drawRoundedRectangle(juce::Rectangle<float>(0.5f, 0.5f,
            static_cast<float>(width) - 1, static_cast<float>(height) - 1), 6.0f, 1.0f);
    }

private:
    bool modernPalette;
};
}
