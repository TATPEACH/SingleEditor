#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Bnk/BnkDocument.h"

namespace singleeditor
{
class BnkAudioListViewComponent final : public juce::Component,
                                        private juce::ListBoxModel,
                                        private BnkDocument::Listener
{
public:
    explicit BnkAudioListViewComponent(BnkDocument& documentToUse);
    ~BnkAudioListViewComponent() override;

    std::function<void(uint32_t)> onMediaActivated;
    std::function<void(uint32_t)> onMediaPlayRequested;
    std::function<void(uint32_t)> onMediaSelected;

    void setLinkedOnly(bool shouldShowLinkedOnly);
    bool isLinkedOnly() const noexcept { return linkedOnly; }
    void setSwitchTokenFilter(juce::String token);

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
    void showMediaContextMenu(int row);
    void bankDocumentChanged(BnkDocument::StatePtr) override;
    void rebuildVisibleMedia();

    BnkDocument& document;
    juce::ListBox listBox;
    juce::TextEditor searchBox;
    std::vector<BnkDocument::PlayableMediaDescriptor> mediaEntries;
    std::vector<uint32_t> highlightedMediaIds;
    juce::String switchValueToken;
    bool linkedOnly = true;
};
} // namespace singleeditor
