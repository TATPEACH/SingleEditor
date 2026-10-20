#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace singleeditor
{
class ClassicLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    static constexpr const char* sansSerifFamily = "Segoe UI";

    ClassicLookAndFeel();

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool highlighted,
                              bool down) override;
    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool highlighted,
                        bool down) override;
    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool highlighted,
                          bool down) override;
    void drawComboBox(juce::Graphics& g,
                      int width,
                      int height,
                      bool isButtonDown,
                      int buttonX,
                      int buttonY,
                      int buttonW,
                      int buttonH,
                      juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& editor) override;
    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& editor) override;
};
} // namespace singleeditor
