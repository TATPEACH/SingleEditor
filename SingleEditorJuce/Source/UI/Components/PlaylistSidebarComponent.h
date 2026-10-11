#pragma once

#include <functional>
#include <map>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Document/PlaylistModel.h"

namespace singleeditor
{
class PlaylistSidebarComponent final : public juce::Component,
                                       private juce::ListBoxModel,
                                       private PlaylistModel::Listener
{
public:
    explicit PlaylistSidebarComponent(PlaylistModel& playlistToUse);
    ~PlaylistSidebarComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void lookAndFeelChanged() override;
    void updateLoadedDuration(const juce::File& file, double seconds);

    std::function<void(int)> onEntryActivated;
    std::function<void(int)> onEntryRemoveRequested;
    std::function<void(const juce::File&)> onRevealFileRequested;

private:
    int getNumRows() override;
    void paintListBoxItem(int rowNumber,
                          juce::Graphics& g,
                          int width,
                          int height,
                          bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    void activateEntry(int row);
    void showEntryContextMenu(int row, const juce::MouseEvent& event);
    void playlistChanged() override;
    void currentPlaylistIndexChanged(int newIndex) override;

    PlaylistModel& playlist;
    juce::ListBox listBox;
    std::map<juce::String, double> loadedDurations;
};
} // namespace singleeditor
