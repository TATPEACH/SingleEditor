#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
// A read-only projection of already loaded document facts and transport state.
// No reader, metadata provider, audio runtime or timer belongs to this Mag.
class TimelineMarkersComponent final : public juce::Component, public juce::ListBoxModel
{
public:
    struct Row
    {
        juce::String badge, label;
        double seconds = 0.0;
        bool available = false, selectionBoundary = false;
    };

    TimelineMarkersComponent();
    ~TimelineMarkersComponent() override;
    void setDocument(SingleTrackDocument::StatePtr state);
    void setTransport(double position, std::optional<juce::Range<double>> selection);
    [[nodiscard]] Row rowAt(int index) const;
    [[nodiscard]] bool hasUnreadableCues() const noexcept { return unreadableCues; }
    void paint(juce::Graphics&) override;
    void resized() override;
    int getNumRows() override;
    juce::String getNameForRow(int row) override;
    juce::String getTooltipForRow(int row) override { return getNameForRow(row); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    std::function<void(double)> onSeek;

private:
    void activateRow(int row);
    SingleTrackDocument::StatePtr source;
    std::vector<Row> cues;
    double duration = 0.0, playhead = 0.0;
    std::optional<juce::Range<double>> selection;
    bool available = false, unreadableCues = false;
    juce::ListBox list { "Timeline markers", this };
};
}
