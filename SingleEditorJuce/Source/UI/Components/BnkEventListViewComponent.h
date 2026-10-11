#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Bnk/BnkDocument.h"

namespace singleeditor
{
class BnkEventListViewComponent final : public juce::Component,
                                        private juce::ListBoxModel,
                                        private BnkDocument::Listener
{
public:
    explicit BnkEventListViewComponent(BnkDocument& documentToUse);
    ~BnkEventListViewComponent() override;

    std::function<void(uint32_t)> onEventActivated;
    std::function<void(uint32_t)> onEventPlayRequested;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int rowNumber,
                          juce::Graphics& g,
                          int width,
                          int height,
                          bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void bankDocumentChanged(BnkDocument::StatePtr) override;
    void rebuildVisibleEvents();

    BnkDocument& document;
    juce::ListBox listBox;
    juce::TextEditor searchBox;
    std::vector<uint32_t> visibleEventIds;
};
} // namespace singleeditor
