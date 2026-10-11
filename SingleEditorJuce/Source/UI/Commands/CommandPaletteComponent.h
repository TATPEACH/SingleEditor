#pragma once

#include "UI/Commands/CommandPaletteModel.h"
#include "UI/Components/ModernGlassSurface.h"

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
class CommandPaletteComponent final : public juce::Component,
                                      private juce::ListBoxModel
{
public:
    CommandPaletteComponent(const GuiCommandRegistry& registry,
                            GuiCommandDispatchContext context);
    ~CommandPaletteComponent() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void visibilityChanged() override;
    void setBackdropSource(juce::Component* source, bool effectsAllowed, juce::Component* overlay = nullptr);
    void invalidateBackdrop();
    void focusSearch();

    std::function<void(std::string)> onCommandChosen;
    std::function<void()> onDismiss;

private:
    int getNumRows() override;
    void paintListBoxItem(int rowNumber,
                          juce::Graphics& g,
                          int width,
                          int height,
                          bool rowIsSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void selectedRowsChanged(int) override;
    juce::String getNameForRow(int row) override;

    void refreshMatches();
    void invokeSelected();
    void updateSelectedState();
    bool navigateSearch(const juce::KeyPress& key);

    class SearchEditor final : public juce::TextEditor
    {
    public:
        std::function<bool(const juce::KeyPress&)> onNavigationKey;
        bool keyPressed(const juce::KeyPress& key) override
        {
            if (onNavigationKey && onNavigationKey(key)) return true;
            return juce::TextEditor::keyPressed(key);
        }
    };

    CommandPaletteModel model;
    std::vector<CommandPaletteMatch> matches;
    std::unique_ptr<ModernGlassSurface> materialSurface;
    juce::Label heading, selectionStatus;
    SearchEditor queryEditor;
    juce::ListBox resultsList;
    juce::TextButton runButton { "Run" };
    juce::TextButton closeButton { "Close" };
    bool backdropAllowed = false;
};
} // namespace singleeditor
