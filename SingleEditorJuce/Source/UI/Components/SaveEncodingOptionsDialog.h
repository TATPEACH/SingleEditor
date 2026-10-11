#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
// Constructed only after a compressed save/export target has been chosen.
// Keep JUCE's modal lifetime and measured alert layout, with a full-height field.
class SaveEncodingOptionsDialog final : public juce::AlertWindow
{
public:
    SaveEncodingOptionsDialog(juce::Component& owner, const juce::String& fieldName,
                              const juce::StringArray& choices, int selectedIndex)
        : juce::AlertWindow("Encoding Settings",
                            "Choose the encoding settings for this file.",
                            juce::MessageBoxIconType::NoIcon, &owner),
          field(fieldName, choices, selectedIndex)
    {
        setLookAndFeel(&owner.getLookAndFeel());
        addCustomComponent(&field);
        addButton("Continue", 1, juce::KeyPress(juce::KeyPress::returnKey));
        addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        getButton("Continue")->getProperties().set("accent", true);
    }

    ~SaveEncodingOptionsDialog() override
    {
        removeCustomComponent(0);
        setLookAndFeel(nullptr);
    }

    juce::ComboBox& getChoice() noexcept { return field.choice; }

private:
    class ChoiceField final : public juce::Component
    {
    public:
        ChoiceField(const juce::String& fieldName, const juce::StringArray& choices, int index)
        {
            label.setText(fieldName, juce::dontSendNotification);
            label.setFont(juce::Font(juce::FontOptions(13.0f)));
            label.setBorderSize(juce::BorderSize<int>(0));
            label.setMinimumHorizontalScale(1.0f);
            addAndMakeVisible(label);
            choice.setName(fieldName);
            choice.setTitle(fieldName);
            choice.setComponentID("save-encoding.choice");
            choice.addItemList(choices, 1);
            choice.setSelectedItemIndex(index, juce::dontSendNotification);
            addAndMakeVisible(choice);
            setSize(320, 58);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds(area.removeFromTop(18));
            area.removeFromTop(6);
            choice.setBounds(area.removeFromTop(32));
        }

        juce::ComboBox choice;

    private:
        juce::Label label;
    };

    ChoiceField field;
};
}
