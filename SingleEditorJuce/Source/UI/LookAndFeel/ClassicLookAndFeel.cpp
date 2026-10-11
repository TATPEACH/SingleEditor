#include "UI/LookAndFeel/ClassicLookAndFeel.h"

#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
ClassicLookAndFeel::ClassicLookAndFeel()
{
    RendererSelection::noteRendererObject(RendererKind::classic);
    setDefaultSansSerifTypefaceName("Segoe UI");
    setColour(juce::ResizableWindow::backgroundColourId, RendererVisualStyle::windowBackground());
    setColour(juce::DocumentWindow::backgroundColourId, RendererVisualStyle::windowBackground());
    setColour(juce::Label::textColourId, RendererVisualStyle::primaryText());
    setColour(juce::TextButton::buttonColourId, RendererVisualStyle::elevatedPanelBackground());
    setColour(juce::TextButton::buttonOnColourId, RendererVisualStyle::accentBlue());
    setColour(juce::TextButton::textColourOffId, RendererVisualStyle::primaryText());
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ComboBox::backgroundColourId, RendererVisualStyle::mutedSurface());
    setColour(juce::ComboBox::outlineColourId, RendererVisualStyle::borderColour());
    setColour(juce::ComboBox::textColourId, RendererVisualStyle::primaryText());
    setColour(juce::TextEditor::backgroundColourId, RendererVisualStyle::workspaceSurface());
    setColour(juce::TextEditor::outlineColourId, RendererVisualStyle::borderColour());
    setColour(juce::TextEditor::focusedOutlineColourId, RendererVisualStyle::accentBlue());
    setColour(juce::TextEditor::textColourId, RendererVisualStyle::primaryText());
    setColour(juce::PopupMenu::backgroundColourId, RendererVisualStyle::panelBackground());
    setColour(juce::PopupMenu::textColourId, RendererVisualStyle::primaryText());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, RendererVisualStyle::accentBlue());
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
}

void ClassicLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                              juce::Button& button,
                                              const juce::Colour&,
                                              bool highlighted,
                                              bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = button.getToggleState() ? RendererVisualStyle::accentBlue()
                                        : RendererVisualStyle::elevatedPanelBackground();
    if (!button.isEnabled())
        fill = RendererVisualStyle::mutedSurface();
    else if (down)
        fill = fill.darker(0.12f);
    else if (highlighted)
        fill = fill.brighter(0.08f);

    g.setColour(fill);
    g.fillRect(bounds);
    g.setColour(button.hasKeyboardFocus(true) ? RendererVisualStyle::accentBlue()
                                              : RendererVisualStyle::borderColour());
    g.drawRect(bounds, button.hasKeyboardFocus(true) ? 2.0f : 1.0f);
}

void ClassicLookAndFeel::drawButtonText(juce::Graphics& g,
                                        juce::TextButton& button,
                                        bool,
                                        bool)
{
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(!button.isEnabled() ? RendererVisualStyle::disabledText()
                                    : (button.getToggleState() ? juce::Colours::white
                                                               : RendererVisualStyle::primaryText()));
    g.drawFittedText(button.getButtonText(), button.getLocalBounds().reduced(6, 2),
                     juce::Justification::centred, 1);
}

void ClassicLookAndFeel::drawToggleButton(juce::Graphics& g,
                                          juce::ToggleButton& button,
                                          bool highlighted,
                                          bool down)
{
    auto bounds = button.getLocalBounds();
    auto box = bounds.removeFromLeft(20).withSizeKeepingCentre(14, 14).toFloat();
    g.setColour(down || highlighted ? RendererVisualStyle::workspaceSurface()
                                    : RendererVisualStyle::panelBackground());
    g.fillRect(box);
    g.setColour(RendererVisualStyle::borderColour());
    g.drawRect(box, 1.0f);
    if (button.getToggleState())
    {
        g.setColour(RendererVisualStyle::accentBlue());
        g.fillRect(box.reduced(3.0f));
    }
    g.setColour(button.isEnabled() ? RendererVisualStyle::primaryText()
                                   : RendererVisualStyle::disabledText());
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawFittedText(button.getButtonText(), bounds.reduced(4, 0),
                     juce::Justification::centredLeft, 1);
}

void ClassicLookAndFeel::drawComboBox(juce::Graphics& g,
                                      int width,
                                      int height,
                                      bool isButtonDown,
                                      int buttonX,
                                      int buttonY,
                                      int buttonW,
                                      int buttonH,
                                      juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.5f, 0.5f,
                                         static_cast<float>(width) - 1.0f,
                                         static_cast<float>(height) - 1.0f);
    g.setColour(box.isEnabled() ? RendererVisualStyle::workspaceSurface()
                                : RendererVisualStyle::mutedSurface());
    g.fillRect(bounds);
    g.setColour(box.hasKeyboardFocus(true) ? RendererVisualStyle::accentBlue()
                                           : RendererVisualStyle::borderColour());
    g.drawRect(bounds, box.hasKeyboardFocus(true) ? 2.0f : 1.0f);

    auto arrow = juce::Rectangle<float>(static_cast<float>(buttonX), static_cast<float>(buttonY),
                                        static_cast<float>(buttonW), static_cast<float>(buttonH)).reduced(10.0f, 11.0f);
    juce::Path path;
    path.startNewSubPath(arrow.getX(), arrow.getY());
    path.lineTo(arrow.getCentreX(), arrow.getBottom());
    path.lineTo(arrow.getRight(), arrow.getY());
    g.setColour(RendererVisualStyle::primaryText().withAlpha(isButtonDown ? 1.0f : 0.78f));
    g.strokePath(path, juce::PathStrokeType(1.0f));
}

void ClassicLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(6, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setColour(juce::Label::textColourId, RendererVisualStyle::primaryText());
}

juce::Font ClassicLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return juce::Font(juce::FontOptions(12.0f));
}

juce::Font ClassicLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return juce::Font(juce::FontOptions(static_cast<float>(juce::jlimit(10, 13, buttonHeight - 10))));
}

void ClassicLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                  juce::TextEditor&)
{
    g.setColour(RendererVisualStyle::workspaceSurface());
    g.fillRect(0, 0, width, height);
}

void ClassicLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                               juce::TextEditor& editor)
{
    g.setColour(editor.hasKeyboardFocus(true) ? RendererVisualStyle::accentBlue()
                                              : RendererVisualStyle::borderColour());
    g.drawRect(juce::Rectangle<int>(0, 0, width, height), editor.hasKeyboardFocus(true) ? 2 : 1);
}
} // namespace singleeditor
