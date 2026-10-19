#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Renderer/CoreActionIcons.h"
#include "UI/Skins/SkinStyle.h"
#include "ModernGlassPainter.h"
#include "ModernTypography.h"

namespace singleeditor
{
namespace
{
juce::Colour withAlpha(juce::Colour colour, float alpha) { return colour.withAlpha(alpha); }
ModernAppleLookAndFeel::SkinMode currentSkinMode = ModernAppleLookAndFeel::SkinMode::full;

bool usesOptionalSkin() noexcept
{
    return !ModernAppleLookAndFeel::isStartupSkin() && !RendererVisualStyle::isClassic() && !SkinStyle::isOriginal();
}

// The original startup frame uses fixed neutral colours; optional skins start
// in their own palette so the CoreReady promotion does not change the canvas.
bool usesStartupConstants() noexcept
{
    return ModernAppleLookAndFeel::isStartupSkin() && RendererSelection::selected() == RendererKind::modern
        && SkinStyle::isOriginal();
}
}

ModernAppleLookAndFeel::ModernAppleLookAndFeel()
    : WindowChromeLookAndFeel(true), preferredFontName(choosePreferredFont())
{
    RendererSelection::noteRendererObject(RendererKind::modern);
    if (preferredFontName.isNotEmpty())
        setDefaultSansSerifTypefaceName(preferredFontName);

    refreshColours();
}

std::unique_ptr<juce::FocusOutline> ModernAppleLookAndFeel::createFocusOutlineForComponent(juce::Component&)
{
    struct RegionOutline final : juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds(juce::Component& component) override
        {
            return component.getScreenBounds();
        }

        void drawOutline(juce::Graphics& g, int width, int height) override
        {
            ModernGlassPainter::focusOutline(g, { 0.0f, 0.0f, float(width), float(height) },
                                             ModernAppleLookAndFeel::cardRadius(), true, true);
        }
    };
    // JUCE owns one outline for the current focus and follows move/hide/reparent.
    // Child regions use a mouse-transparent sibling, not another native window.
    return std::make_unique<juce::FocusOutline>(std::make_unique<RegionOutline>());
}

void ModernAppleLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert,
                                        const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout)
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
    {
        juce::LookAndFeel_V4::drawAlertBox(g, alert, textArea, textLayout);
        return;
    }

    const auto bounds = alert.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(alert.findColour(juce::AlertWindow::backgroundColourId));
    g.fillRoundedRectangle(bounds, 12.0f);
    g.setColour(alert.findColour(juce::AlertWindow::outlineColourId));
    g.drawRoundedRectangle(bounds, 12.0f, 1.0f);

    const auto type = alert.getAlertType();
    const auto hasIcon = type != juce::MessageBoxIconType::NoIcon;
    if (hasIcon)
    {
        // Keep the JUCE layout's reserved icon column, but use a contained glyph.
        // V4's oversized watermark deliberately starts outside the native window.
        const juce::Rectangle<float> tile(24.0f, 30.0f, 36.0f, 36.0f);
        auto ink = type == juce::MessageBoxIconType::WarningIcon
                       ? juce::Colour(ModernNorthStarFoundationTokens::warningArgb) : accentBlue();
        if (RendererVisualStyle::modernTheme() == ModernTheme::night)
            ink = ink.brighter(0.3f);
        g.setColour(ink.withAlpha(0.12f));
        g.fillRoundedRectangle(tile, 10.0f);
        g.setColour(ink.withAlpha(0.25f));
        g.drawRoundedRectangle(tile.reduced(0.5f), 10.0f, 1.0f);
        g.setColour(ink);
        g.setFont(juce::Font(juce::FontOptions(25.0f, juce::Font::bold)));
        g.drawText(type == juce::MessageBoxIconType::WarningIcon ? "!"
                       : type == juce::MessageBoxIconType::InfoIcon ? "i" : "?",
                   tile, juce::Justification::centred, false);
    }

    // Use the existing attributed layout so title/message metrics, wrapping and
    // additional form fields retain JUCE's measured sizing and accessibility.
    const auto left = hasIcon ? 80.0f : 24.0f;
    textLayout.draw(g, { left, 30.0f, juce::jmax(0.0f, float(alert.getWidth()) - left - 24.0f),
                         textLayout.getHeight() });
}

void ModernAppleLookAndFeel::refreshColours()
{
    setColour(juce::ResizableWindow::backgroundColourId, windowBackground());
    setColour(juce::DocumentWindow::backgroundColourId, windowBackground());
    setColour(juce::AlertWindow::backgroundColourId, elevatedPanelBackground());
    setColour(juce::AlertWindow::textColourId, primaryText());
    setColour(juce::AlertWindow::outlineColourId, borderColour());
    setColour(juce::Label::textColourId, primaryText());
    // Slider value labels create TextEditors and copy these colours over the
    // TextEditor defaults. Leaving V4's scheme here made Light edits invisible.
    setColour(juce::Label::textWhenEditingColourId, primaryText());
    setColour(juce::Label::backgroundWhenEditingColourId, mutedSurface());
    setColour(juce::Label::outlineWhenEditingColourId, accentBlue());
    setColour(juce::TextButton::buttonColourId, elevatedPanelBackground());
    setColour(juce::TextButton::buttonOnColourId, accentBlue());
    setColour(juce::TextButton::textColourOffId, primaryText());
    setColour(juce::TextButton::textColourOnId, primaryText());
    setColour(juce::ComboBox::backgroundColourId, mutedSurface());
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::textColourId, primaryText());
    setColour(juce::TextEditor::backgroundColourId, mutedSurface());
    setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::focusedOutlineColourId, accentBlue());
    setColour(juce::TextEditor::textColourId, primaryText());
    // JUCE uses this alpha to request a transparent native popup peer. The body
    // is painted opaque below; only the rounded corners stay transparent.
    setColour(juce::PopupMenu::backgroundColourId, elevatedPanelBackground().withAlpha(
        !isStartupSkin() && !RendererVisualStyle::isClassic() ? 0.995f : 1.0f));
    setColour(juce::PopupMenu::textColourId, primaryText());
    setColour(juce::PopupMenu::headerTextColourId, secondaryText());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, withAlpha(accentBlue(), 0.18f));
    setColour(juce::PopupMenu::highlightedTextColourId, primaryText());
    setColour(juce::Slider::backgroundColourId, borderColour());
    setColour(juce::Slider::trackColourId, accentBlue());
    setColour(juce::Slider::thumbColourId, accentBlue());
    setColour(juce::Slider::textBoxTextColourId, primaryText());
    setColour(juce::Slider::textBoxBackgroundColourId, mutedSurface());
    setColour(juce::Slider::textBoxOutlineColourId, borderColour());
    setColour(juce::TextEditor::highlightColourId, RendererVisualStyle::accentSoft());
    setColour(juce::TextEditor::highlightedTextColourId, RendererVisualStyle::accentInk());
    setColour(juce::ScrollBar::thumbColourId, secondaryText().withAlpha(0.35f));
}

void ModernAppleLookAndFeel::refreshTheme(juce::Component& root)
{
    if (RendererVisualStyle::isClassic()) return;
    if (auto* modern = dynamic_cast<ModernAppleLookAndFeel*>(&root.getLookAndFeel()))
        modern->refreshColours();
    const auto rebind = [] (auto&& self, juce::Component& component) -> void
    {
        if (auto* label = dynamic_cast<juce::Label*>(&component))
        {
            const auto role = static_cast<int>(label->getProperties().getWithDefault("modernLabelRole", 0));
            if (role >= static_cast<int>(LabelRole::primary) && role <= static_cast<int>(LabelRole::accent))
                setLabelRole(*label, static_cast<LabelRole>(role));
        }
        if (auto* editor = dynamic_cast<juce::TextEditor*>(&component))
            // JUCE stores ink in existing text runs. Updating the colour ID
            // alone only affects newly typed text. Do not pin a local colour;
            // inherited defaults and explicit editor overrides remain intact.
            editor->applyColourToAllText(editor->findColour(juce::TextEditor::textColourId), false);
        for (auto* child : component.getChildren()) self(self, *child);
    };
    if (auto* window = dynamic_cast<juce::ResizableWindow*>(&root))
        window->setBackgroundColour(windowBackground());
    root.sendLookAndFeelChange();
    // Child callbacks may first update explicit colours (e.g. the palette).
    rebind(rebind, root);
    root.repaint();
}

juce::Typeface::Ptr ModernAppleLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    // Prefer our chosen system font when available
    if (preferredFontName.isNotEmpty())
        return juce::LookAndFeel_V4::getTypefaceForFont(font);

    // When no preferred font found, use system default instead of embedded Inter
    // This ensures proper Unicode coverage including Chinese characters
    return juce::LookAndFeel_V4::getTypefaceForFont(font);
}

void ModernAppleLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                                  juce::Button& button,
                                                  const juce::Colour&,
                                                  bool shouldDrawButtonAsHighlighted,
                                                  bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto iconOnly = static_cast<bool>(button.getProperties().getWithDefault("iconOnly", false));
    const auto hasIcon = button.getProperties()["iconName"].toString().isNotEmpty();
    const auto isGhost = static_cast<bool>(button.getProperties().getWithDefault("ghostButton", false)) || (iconOnly && hasIcon);

    if (usesOptionalSkin())
    {
        juce::Colour skinText;
        if (SkinStyle::paintButtonBackground(g, button, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, skinText))
        {
            ModernGlassPainter::focusOutline(g, button.getLocalBounds().toFloat(), componentRadius(),
                                             button.hasKeyboardFocus(false), button.isEnabled());
            // Read only by drawButtonText while a skin is active, so switching
            // back to Original leaves the button's own colours untouched.
            button.getProperties().set("skinTextColour", static_cast<juce::int64>(skinText.getARGB()));
            return;
        }
    }

    if (isGhost)
    {
        if (!button.isEnabled())
            return;

        if (button.getToggleState() || shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            auto fill = button.getToggleState() ? accentBlue().withAlpha(0.14f)
                                                : (shouldDrawButtonAsDown ? ghostPressedFill()
                                                                          : ghostHoverFill());
            g.setColour(fill);
            g.fillRoundedRectangle(bounds, componentRadius());
        }

        if (!isStartupSkin() && !RendererVisualStyle::isClassic())
            ModernGlassPainter::focusOutline(g, button.getLocalBounds().toFloat(), componentRadius(),
                                             button.hasKeyboardFocus(false), button.isEnabled());
        return;
    }

    auto background = button.getToggleState() ? accentBlue() : elevatedPanelBackground();
    auto textColour = button.getToggleState() ? juce::Colours::white : primaryText();

    if (auto accent = button.getProperties()["accent"]; accent.isBool() && static_cast<bool>(accent))
    {
        background = accentBlue();
        textColour = juce::Colours::white;
    }

    if (!button.isEnabled())
    {
        background = mutedSurface();
        textColour = disabledText();
    }
    else if (shouldDrawButtonAsDown)
    {
        background = background.darker(0.03f);
    }
    else if (shouldDrawButtonAsHighlighted)
    {
        background = background.brighter(0.015f);
    }

    if (!isStartupSkin() && !RendererVisualStyle::isClassic())
    {
        ModernGlassPainter::surface(g, button.getLocalBounds().toFloat(), background,
                                     componentRadius(), shouldDrawButtonAsDown);
        ModernGlassPainter::focusOutline(g, button.getLocalBounds().toFloat(), componentRadius(),
                                         button.hasKeyboardFocus(false), button.isEnabled());
    }
    else
    {
        g.setColour(background);
        g.fillRoundedRectangle(bounds, componentRadius());
    }
    button.setColour(juce::TextButton::textColourOffId, textColour);
    button.setColour(juce::TextButton::textColourOnId, textColour);
}

void ModernAppleLookAndFeel::drawButtonText(juce::Graphics& g,
                                            juce::TextButton& button,
                                            bool,
                                            bool)
{
    auto bounds = button.getLocalBounds();
    auto font = getTextButtonFont(button, bounds.getHeight());
    const auto text = button.getButtonText();
    if (!isStartupSkin() && !RendererVisualStyle::isClassic() && (text == "+" || text == "-"))
        if (auto* slider = dynamic_cast<juce::Slider*>(button.getParentComponent());
            slider != nullptr && slider->getSliderStyle() == juce::Slider::IncDecButtons)
        {
            // A stacked numeric stepper can be only 14 px tall. Its glyph must
            // not inherit the normal button's height-minus-padding font size.
            const auto centre = bounds.toFloat().getCentre();
            const auto half = juce::jlimit(1.5f, 3.5f, bounds.getHeight() * 0.22f);
            g.setColour(button.isEnabled() ? primaryText() : disabledText());
            g.drawLine(centre.x - half, centre.y, centre.x + half, centre.y, 1.0f);
            if (text == "+") g.drawLine(centre.x, centre.y - half, centre.x, centre.y + half, 1.0f);
            return;
        }
    const auto horizontalPadding = text.length() <= 2 ? 4
        : !isStartupSkin() && !RendererVisualStyle::isClassic() && bounds.getWidth() < 80 ? 6 : 12;
    auto content = bounds.reduced(horizontalPadding, 4);
    const auto iconName = button.getProperties()["iconName"].toString();
    const auto iconOnly = static_cast<bool>(button.getProperties().getWithDefault("iconOnly", false));
    const auto enabledAlpha = button.isEnabled() ? 1.0f : 0.45f;
    auto iconDrawn = false;

    if (iconName.isNotEmpty())
    {
        const auto assetId = RendererVisualStyle::assetIdForFileName(
            iconName.toStdString());
        if (auto* drawable = getCachedButtonIcon(assetId))
        {
            const auto iconSize = juce::jmin(content.getHeight() - 2, iconOnly ? 18 : 16);
            auto iconArea = content;
            if (iconOnly || text.isEmpty())
                iconArea = content.withSizeKeepingCentre(iconSize, iconSize);
            else
                iconArea = content.removeFromLeft(iconSize).withSizeKeepingCentre(iconSize, iconSize);

            const auto sourceBounds = drawable->getDrawableBounds();
            juce::Graphics::ScopedSaveState state(g);
            g.addTransform(juce::RectanglePlacement(juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize)
                               .getTransformToFit(sourceBounds, iconArea.toFloat()));
            drawable->draw(g, enabledAlpha);
            iconDrawn = true;

            if (!iconOnly && text.isNotEmpty())
                content.removeFromLeft(8);
        }
    }

    if ((!iconOnly || !iconDrawn) && text.isNotEmpty())
    {
        g.setFont(font);
        const auto skinText = button.getProperties()["skinTextColour"];
        if (usesOptionalSkin() && skinText.isInt64())
            g.setColour(juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(skinText))));
        else
            g.setColour(button.isEnabled() ? button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId
                                                                                       : juce::TextButton::textColourOffId)
                                           : disabledText());
        g.drawFittedText(text,
                         content,
                         iconName.isNotEmpty() && !iconOnly ? juce::Justification::centredLeft
                                                            : juce::Justification::centred,
                         1);
    }
}

void ModernAppleLookAndFeel::drawToggleButton(juce::Graphics& g,
                                              juce::ToggleButton& button,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
{
    if (!isStartupSkin() && !RendererVisualStyle::isClassic()
        && static_cast<bool>(button.getProperties().getWithDefault("modernCompactSwitch", false)))
    {
        const auto track = button.getLocalBounds().toFloat().removeFromLeft(29.0f).withSizeKeepingCentre(28.0f, 16.0f);
        const auto alpha = button.isEnabled() ? 1.0f : 0.4f;
        if (usesOptionalSkin())
        {
            SkinStyle::paintSwitch(g, track, button.getToggleState(), button.isEnabled(), button.hasKeyboardFocus(true));
            g.setColour(primaryText().withMultipliedAlpha(alpha));
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(35), juce::Justification::centredLeft, true);
            return;
        }
        g.setColour((button.getToggleState() ? accentBlue() : borderColour()).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, 8.0f);
        const auto thumb = juce::Rectangle<float>(track.getX() + (button.getToggleState() ? 14.0f : 2.0f), track.getY() + 2.0f, 12.0f, 12.0f);
        g.setColour(juce::Colours::black.withAlpha(0.13f * alpha));
        g.fillEllipse(thumb.translated(0, 0.7f));
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.fillEllipse(thumb);
        if (button.hasKeyboardFocus(true))
        {
            g.setColour(accentBlue().withAlpha(0.7f));
            g.drawRoundedRectangle(track.expanded(2), 10, 1);
        }
        g.setColour(primaryText().withMultipliedAlpha(alpha));
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        g.drawText(button.getButtonText(), button.getLocalBounds().withTrimmedLeft(35), juce::Justification::centredLeft, true);
        return;
    }
    const auto compact = !isStartupSkin() && !RendererVisualStyle::isClassic()
        && static_cast<bool>(button.getProperties().getWithDefault("modernCompactToggle", false));
    auto bounds = button.getLocalBounds().toFloat();
    auto tickBounds = bounds.removeFromLeft(compact ? 18.0f : 22.0f)
                           .withSizeKeepingCentre(compact ? 16.0f : 18.0f, compact ? 16.0f : 18.0f);
    auto textBounds = compact ? bounds.withTrimmedLeft(3.0f) : bounds.reduced(6.0f, 0.0f);

    const auto enabled = button.isEnabled();
    const auto tickOutline = enabled ? borderColour() : disabledText().withAlpha(0.42f);
    const auto tickFill = button.getToggleState()
                              ? accentBlue()
                              : (shouldDrawButtonAsDown ? ghostPressedFill()
                                                        : (shouldDrawButtonAsHighlighted ? ghostHoverFill()
                                                                                         : panelBackground()));

    g.setColour(tickFill);
    g.fillRoundedRectangle(tickBounds.reduced(0.5f), 5.0f);

    if (!button.getToggleState())
    {
        g.setColour(tickOutline);
        g.drawRoundedRectangle(tickBounds.reduced(0.5f), 5.0f, 1.0f);
    }

    if (button.getToggleState())
    {
        juce::Path tick;
        auto iconArea = tickBounds.reduced(4.5f, 4.5f);
        tick.startNewSubPath(iconArea.getX(), iconArea.getCentreY());
        tick.lineTo(iconArea.getCentreX() - 1.0f, iconArea.getBottom());
        tick.lineTo(iconArea.getRight(), iconArea.getY());
        g.setColour(juce::Colours::white.withAlpha(enabled ? 0.98f : 0.55f));
        g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        ModernGlassPainter::focusOutline(g, tickBounds, 5.0f, button.hasKeyboardFocus(false), enabled);
    g.setColour(enabled ? primaryText().withAlpha(0.92f) : disabledText());
    g.setFont(compact ? ModernTypography::font(12.0f)
                      : juce::Font(juce::FontOptions(13.0f, juce::Font::plain)));
    if (compact)
        g.drawText(button.getButtonText(), textBounds, juce::Justification::centredLeft, true);
    else
        g.drawFittedText(button.getButtonText(), textBounds.toNearestInt(), juce::Justification::centredLeft, 2);
}

void ModernAppleLookAndFeel::drawComboBox(juce::Graphics& g,
                                          int width,
                                          int height,
                                          bool isButtonDown,
                                          int buttonX,
                                          int buttonY,
                                          int buttonW,
                                          int buttonH,
                                          juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    if (usesOptionalSkin())
        SkinStyle::paintComboBox(g, box, isButtonDown);
    else if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        ModernGlassPainter::control(g, box.getLocalBounds().toFloat(), box.isEnabled(),
                                     box.isMouseOver(), isButtonDown, false);
    else
    {
        g.setColour(box.isEnabled() ? mutedSurface() : juce::Colour(0xffeceef2));
        g.fillRoundedRectangle(bounds, componentRadius());
    }
    if (box.hasKeyboardFocus(true))
    {
        if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        {
            g.setColour(accentBlue().withAlpha(0.65f));
            g.drawRoundedRectangle(bounds.reduced(1.0f), componentRadius() - 1.0f, 1.2f);
        }
        else
        {
            g.setColour(accentBlue().withAlpha(0.22f));
            g.fillRoundedRectangle(bounds.reduced(1.0f), componentRadius() - 1.0f);
        }
    }

    auto arrowArea = juce::Rectangle<float>(static_cast<float>(buttonX),
                                            static_cast<float>(buttonY),
                                            static_cast<float>(buttonW),
                                            static_cast<float>(buttonH)).reduced(9.0f, 12.0f);
    if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        arrowArea = { static_cast<float>(width) - 15.0f, height * 0.5f - 2.0f, 6.0f, 4.0f };

    juce::Path arrow;
    arrow.startNewSubPath(arrowArea.getX(), arrowArea.getY());
    arrow.lineTo(arrowArea.getCentreX(), arrowArea.getBottom());
    arrow.lineTo(arrowArea.getRight(), arrowArea.getY());

    g.setColour(primaryText().withAlpha(box.isEnabled() ? 0.8f : 0.35f));
    g.strokePath(arrow, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ModernAppleLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    const bool compact = !isStartupSkin() && !RendererVisualStyle::isClassic() && box.getWidth() <= 80;
    label.setBounds(compact ? 5 : 10, 2, box.getWidth() - (compact ? 23 : 34), box.getHeight() - 4);
    label.setBorderSize(compact ? juce::BorderSize<int>(0) : juce::BorderSize<int>(1, 5, 1, 5));
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, primaryText());
    label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

juce::Font ModernAppleLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        return ModernTypography::font(box.getWidth() <= 80 ? 11.0f : 13.0f);
    return juce::Font(juce::FontOptions(14.0f));
}

juce::Label* ModernAppleLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = WindowChromeLookAndFeel::createSliderTextBox(slider);
    if (!isStartupSkin() && !RendererVisualStyle::isClassic()
        && static_cast<bool>(slider.getProperties().getWithDefault("modernCompactNumeric", false)))
    {
        label->setBorderSize(juce::BorderSize<int>(0, 2, 0, 2));
        label->setFont(ModernTypography::font(10.0f));
        label->setMinimumHorizontalScale(0.9f);
    }
    if (!isStartupSkin() && !RendererVisualStyle::isClassic()
        && static_cast<bool>(slider.getProperties().getWithDefault("modernParameterSlider", false)))
    {
        setLabelRole(*label, LabelRole::accent);
        label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        label->setJustificationType(juce::Justification::centredRight);
        label->setFont(ModernTypography::font(11.5f));
    }
    return label;
}

juce::Slider::SliderLayout ModernAppleLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    if (!isStartupSkin() && !RendererVisualStyle::isClassic()
        && slider.getSliderStyle() == juce::Slider::IncDecButtons
        && slider.getTextBoxPosition() == juce::Slider::TextBoxLeft
        && static_cast<bool>(slider.getProperties().getWithDefault("modernCompactNumeric", false)))
    {
        // JUCE normally reserves 30 horizontal pixels. These vertical steppers
        // need 16; keep the exact numeric value readable beside the gain knob.
        auto area = slider.getLocalBounds();
        juce::Slider::SliderLayout layout;
        layout.sliderBounds = area.removeFromRight(juce::jmin(16, area.getWidth()));
        layout.textBoxBounds = area.withSizeKeepingCentre(area.getWidth(), juce::jmin(22, area.getHeight()));
        return layout;
    }
    return WindowChromeLookAndFeel::getSliderLayout(slider);
}

void ModernAppleLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
    float position, float minimum, float maximum, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (usesOptionalSkin() && style == juce::Slider::LinearHorizontal)
    {
        SkinStyle::paintLinearSlider(g, juce::Rectangle<int>(x, y, width, height).toFloat(), position, slider);
        return;
    }
    if (isStartupSkin() || RendererVisualStyle::isClassic() || style != juce::Slider::LinearHorizontal
        || !static_cast<bool>(slider.getProperties().getWithDefault("modernParameterSlider", false)))
    {
        WindowChromeLookAndFeel::drawLinearSlider(g, x, y, width, height, position, minimum, maximum, style, slider);
        return;
    }
    const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const auto centre = y + height * 0.5f;
    const auto left = static_cast<float>(x), right = static_cast<float>(x + width);
    const auto pos = juce::jlimit(left, right, position);
    g.setColour(borderColour().withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(left, centre - 1.5f, static_cast<float>(width), 3.0f, 1.5f);
    g.setColour(accentBlue().withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(left, centre - 1.5f, pos - left, 3.0f, 1.5f);
    const auto thumb = juce::Rectangle<float>(pos - 5.0f, centre - 5.0f, 10.0f, 10.0f);
    g.setColour(juce::Colours::black.withAlpha(0.1f * alpha));
    g.fillEllipse(thumb.translated(0, 1));
    g.setGradientFill(juce::ColourGradient(accentBlue().brighter(0.22f).withMultipliedAlpha(alpha), pos, centre - 5,
        accentBlue().withMultipliedAlpha(alpha), pos, centre + 5, false));
    g.fillEllipse(thumb);
    g.setColour(juce::Colours::white.withAlpha(0.35f * alpha));
    g.drawEllipse(thumb.reduced(0.5f), 0.6f);
    if (slider.hasKeyboardFocus(true))
    {
        g.setColour(accentBlue().withAlpha(0.45f));
        g.drawEllipse(thumb.expanded(2.5f), 1.0f);
    }
}

juce::PopupMenu::Options ModernAppleLookAndFeel::getOptionsForComboBoxPopupMenu(juce::ComboBox& box,
                                                                              juce::Label& label)
{
    auto options = juce::LookAndFeel_V4::getOptionsForComboBoxPopupMenu(box, label);
    if (isStartupSkin() || RendererVisualStyle::isClassic()) return options;
    // Limit only the width inherited from the control, not long item names.
    // JUCE still sizes to the actual text and available monitor work area.
    return options.withMinimumWidth(juce::jlimit(160, 360, box.getWidth()))
                  .withStandardItemHeight(30);
}

juce::Font ModernAppleLookAndFeel::getPopupMenuFont()
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
        return juce::LookAndFeel_V4::getPopupMenuFont();
    return juce::Font(juce::FontOptions(13.0f));
}

int ModernAppleLookAndFeel::getPopupMenuBorderSize()
{
    return isStartupSkin() || RendererVisualStyle::isClassic()
        ? juce::LookAndFeel_V4::getPopupMenuBorderSize() : 6;
}

int ModernAppleLookAndFeel::getMenuWindowFlags()
{
    // JUCE's external ShadowWindow draws a rectangle beneath transparent
    // popup corners. ModernGlassPainter already shades the rounded contour.
    return isStartupSkin() || RendererVisualStyle::isClassic()
        ? juce::LookAndFeel_V4::getMenuWindowFlags() : 0;
}

void ModernAppleLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool separator,
                                                     int standardHeight, int& width, int& height)
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
    {
        juce::LookAndFeel_V4::getIdealPopupMenuItemSize(text, separator, standardHeight, width, height);
        return;
    }
    height = separator ? 9 : juce::jmax(28, standardHeight);
    width = separator ? 80 : juce::jmax(160, juce::GlyphArrangement::getStringWidthInt(getPopupMenuFont(), text) + 60);
}

void ModernAppleLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
    {
        juce::LookAndFeel_V4::drawPopupMenuBackground(g, width, height);
        return;
    }
    if (!SkinStyle::isOriginal())
    {
        SkinStyle::paintPopupBackground(g, { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) });
        return;
    }
    ModernGlassPainter::surface(g, { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) },
                                elevatedPanelBackground(), 12.0f);
}

void ModernAppleLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
    bool separator, bool active, bool highlighted, bool ticked, bool hasSubMenu,
    const juce::String& text, const juce::String& shortcut, const juce::Drawable* icon,
    const juce::Colour* textColour)
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
    {
        juce::LookAndFeel_V4::drawPopupMenuItem(g, area, separator, active, highlighted,
                                              ticked, hasSubMenu, text, shortcut, icon, textColour);
        return;
    }
    juce::Graphics::ScopedSaveState state(g);
    if (separator)
    {
        g.setColour(borderColour().withAlpha(RendererVisualStyle::isNight() ? 0.7f : 0.45f));
        g.drawHorizontalLine(area.getCentreY(), static_cast<float>(area.getX() + 28),
                             static_cast<float>(area.getRight() - 12));
        return;
    }
    const bool selected = active && highlighted;
    const bool neutralHighlight = selected && !SkinStyle::isOriginal() && !SkinStyle::popupHighlightUsesAccent();
    auto row = area.reduced(4, 2);
    if (neutralHighlight)
        SkinStyle::paintPopupHighlight(g, row.toFloat());
    else if (selected)
    {
        g.setColour(accentBlue());
        g.fillRoundedRectangle(row.toFloat(), 6.0f);
    }
    const auto ink = !active ? disabledText() : selected && !neutralHighlight ? juce::Colours::white
        : textColour != nullptr ? *textColour : primaryText();
    row = row.reduced(7, 0);
    const auto iconArea = row.removeFromLeft(18).toFloat().withSizeKeepingCentre(13.0f, 13.0f);
    row.removeFromLeft(5);
    if (icon != nullptr)
        icon->drawWithin(g, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                         active ? 1.0f : 0.4f);
    else if (ticked)
    {
        juce::Path tick;
        tick.startNewSubPath(iconArea.getX() + 1, iconArea.getCentreY());
        tick.lineTo(iconArea.getX() + 5, iconArea.getBottom() - 3);
        tick.lineTo(iconArea.getRight() - 1, iconArea.getY() + 3);
        g.setColour(ink);
        g.strokePath(tick, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    if (hasSubMenu)
    {
        const auto arrow = row.removeFromRight(12).toFloat().withSizeKeepingCentre(4.0f, 7.0f);
        juce::Path path;
        path.startNewSubPath(arrow.getTopLeft()); path.lineTo(arrow.getRight(), arrow.getCentreY());
        path.lineTo(arrow.getBottomLeft());
        g.setColour(ink);
        g.strokePath(path, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    if (shortcut.isNotEmpty())
    {
        const auto font = juce::Font(juce::FontOptions(11.5f));
        const auto width = juce::jmin(row.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt(font, shortcut) + 8);
        const auto shortcutArea = row.removeFromRight(width);
        row.removeFromRight(10);
        g.setFont(font);
        g.setColour(active ? selected && !neutralHighlight ? juce::Colours::white.withAlpha(0.85f) : secondaryText() : disabledText());
        g.drawText(shortcut, shortcutArea, juce::Justification::centredRight, true);
    }
    g.setColour(ink);
    g.setFont(getPopupMenuFont());
    g.drawText(text, row, juce::Justification::centredLeft, true);
}

void ModernAppleLookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g,
    const juce::Rectangle<int>& area, const juce::String& text)
{
    if (isStartupSkin() || RendererVisualStyle::isClassic())
    {
        juce::LookAndFeel_V4::drawPopupMenuSectionHeader(g, area, text);
        return;
    }
    g.setColour(secondaryText());
    g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    g.drawText(text, area.withTrimmedLeft(16).withTrimmedRight(12), juce::Justification::centredLeft, true);
}

juce::Font ModernAppleLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    if (!isStartupSkin() && !RendererVisualStyle::isClassic())
        return ModernTypography::font(juce::jlimit(9.0f, 13.0f, static_cast<float>(buttonHeight - 14)));
    return juce::Font(juce::FontOptions(static_cast<float>(juce::jmin(15, buttonHeight - 14)), juce::Font::plain));
}

void ModernAppleLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    auto bounds = juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(bounds, componentRadius());
}

void ModernAppleLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (!editor.hasKeyboardFocus(true))
        return;

    auto bounds = juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f);
    g.setColour(accentBlue().withAlpha(0.65f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), componentRadius() - 1.0f, 1.2f);
}

void ModernAppleLookAndFeel::setSkinMode(SkinMode newMode) noexcept
{
    currentSkinMode = newMode;
}

ModernAppleLookAndFeel::SkinMode ModernAppleLookAndFeel::getSkinMode() noexcept
{
    return currentSkinMode;
}

bool ModernAppleLookAndFeel::isStartupSkin() noexcept
{
    return currentSkinMode == SkinMode::startup;
}

juce::Colour ModernAppleLookAndFeel::windowBackground()
{
    return usesStartupConstants()
        ? juce::Colour(0xfff7f7f8)
        : RendererVisualStyle::windowBackground();
}

juce::Colour ModernAppleLookAndFeel::panelBackground()
{
    return usesStartupConstants()
        ? juce::Colour(0xfffdfdfd)
        : RendererVisualStyle::panelBackground();
}

juce::Colour ModernAppleLookAndFeel::elevatedPanelBackground()
{
    return RendererVisualStyle::elevatedPanelBackground();
}

juce::Colour ModernAppleLookAndFeel::workspaceSurface()
{
    return usesStartupConstants()
        ? juce::Colour(0xfffcfcfe)
        : RendererVisualStyle::workspaceSurface();
}

juce::Colour ModernAppleLookAndFeel::mutedSurface()
{
    return usesStartupConstants()
        ? juce::Colour(0xfff3f4f6)
        : RendererVisualStyle::mutedSurface();
}

juce::Colour ModernAppleLookAndFeel::titleBarBackground()
{
    return usesStartupConstants()
        ? juce::Colour(0xfff1f3f6)
        : RendererVisualStyle::titleBarBackground();
}

juce::Colour ModernAppleLookAndFeel::borderColour()
{
    return usesStartupConstants()
        ? juce::Colour(0x06000000)
        : RendererVisualStyle::borderColour();
}
juce::Colour ModernAppleLookAndFeel::primaryText() { return RendererVisualStyle::primaryText(); }
juce::Colour ModernAppleLookAndFeel::secondaryText() { return RendererVisualStyle::secondaryText(); }
juce::Colour ModernAppleLookAndFeel::disabledText() { return secondaryText().withAlpha(0.52f); }
juce::Colour ModernAppleLookAndFeel::accentOrange() { return RendererVisualStyle::accentOrange(); }
juce::Colour ModernAppleLookAndFeel::accentBlue() { return RendererVisualStyle::accentBlue(); }
juce::Colour ModernAppleLookAndFeel::accentPurple() { return RendererVisualStyle::accentPurple(); }
juce::Colour ModernAppleLookAndFeel::waveformBlue() { return RendererVisualStyle::waveformBlue(); }
juce::Colour ModernAppleLookAndFeel::waveformCyan() { return RendererVisualStyle::waveformCyan(); }
juce::Colour ModernAppleLookAndFeel::ghostHoverFill()
{
    return usesStartupConstants()
        ? juce::Colour(0x09000000)
        : RendererVisualStyle::ghostHoverFill();
}

juce::Colour ModernAppleLookAndFeel::ghostPressedFill()
{
    return usesStartupConstants()
        ? juce::Colour(0x14000000)
        : RendererVisualStyle::ghostPressedFill();
}
float ModernAppleLookAndFeel::cardRadius() noexcept { return RendererVisualStyle::cardRadius(); }
float ModernAppleLookAndFeel::componentRadius() noexcept { return RendererVisualStyle::componentRadius(); }
int ModernAppleLookAndFeel::outerPadding() noexcept { return RendererVisualStyle::outerPadding(); }
int ModernAppleLookAndFeel::interPanelGap() noexcept { return RendererVisualStyle::interPanelGap(); }
int ModernAppleLookAndFeel::topBarHeight() noexcept { return RendererVisualStyle::topBarHeight(); }
int ModernAppleLookAndFeel::bottomBarHeight() noexcept { return RendererVisualStyle::bottomBarHeight(); }

juce::Drawable* ModernAppleLookAndFeel::getCachedButtonIcon(ModernNorthStarAssetId assetId) const
{
    if (assetId == ModernNorthStarAssetId::unknown)
        return nullptr;

    const auto token = RendererVisualStyle::assetToken(assetId);
    if (token.fileName[0] == '\0')
        return nullptr;
    const std::string key = std::string(token.fileName) + "|" + primaryText().toString().toStdString();

    const juce::ScopedLock sl(iconCacheLock);
    const auto existing = iconCache.find(key);
    if (existing != iconCache.end())
        return existing->second.get();

    const auto bytes = coreActionIconBytes(assetId);
    if (bytes.data == nullptr || bytes.size <= 0)
        return nullptr;

    auto drawable = juce::Drawable::createFromImageData(bytes.data, static_cast<size_t>(bytes.size));
    if (drawable == nullptr)
        return nullptr;
    drawable->replaceColour(juce::Colours::black, primaryText());

    auto* result = drawable.get();
    iconCache.emplace(key, std::move(drawable));
    return result;
}

juce::String ModernAppleLookAndFeel::choosePreferredFont()
{
    const auto available = juce::Font::findAllTypefaceNames();

    // Round 8 body family order; display faces should not be used for every
    // compact label. Keep installed-font fallback and native Unicode shaping.
    for (const auto* candidate : { "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI" })
        if (available.contains(candidate)) return candidate;
    for (const auto& candidate : { ModernNorthStarVisualContract::Typography::displayPrimary,
                                   ModernNorthStarVisualContract::Typography::bodyPrimary,
                                   ModernNorthStarVisualContract::Typography::displayFallback,
                                   ModernNorthStarVisualContract::Typography::bodyFallback })
    {
        if (available.contains(candidate))
            return candidate;
    }

    // Chinese-friendly system fonts (for Chinese characters)
    for (const auto& candidate : { "Microsoft YaHei UI", "Microsoft YaHei", "PingFang SC", "PingFang" })
    {
        if (available.contains(candidate))
            return candidate;
    }

    // Generic modern fallback
    for (const auto& candidate : { "Segoe UI", "Helvetica Neue", "Helvetica", "Arial" })
    {
        if (available.contains(candidate))
            return candidate;
    }

    return {};
}
} // namespace singleeditor
