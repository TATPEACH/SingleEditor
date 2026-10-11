#pragma once

#include "Waveform/WaveformModel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
// A post-Core projection of already-owned source peaks; never reads audio or
// builds caches. Navigation changes the visible time range, not the selection.
class WaveformOverviewComponent final : public juce::Component
{
public:
    enum class Action { zoomOut, zoomIn, reset };
    WaveformOverviewComponent();
    void setState(std::shared_ptr<const WaveformModel::PeakCache> peaks,
                  int64_t audioRevision, double durationSeconds,
                  juce::Range<double> visible, double positionSeconds);
    void setNavigationEnabled(bool enabled);
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

    juce::Rectangle<float> getTrackBounds() const noexcept { return trackBounds; }
    juce::Rectangle<float> getViewportBounds() const noexcept;
    std::function<void(double startSeconds)> onPanRequested;
    std::function<void(Action)> onActionRequested;

private:
    void rebuildPeaks();
    void requestPan(double start);
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, reset { "1.0x" };
    std::shared_ptr<const WaveformModel::PeakCache> cache;
    juce::Rectangle<float> trackBounds;
    juce::Range<double> visibleRange;
    juce::Path peakPath;
    double duration = 0.0, cursor = 0.0, dragStart = 0.0;
    float dragAnchorX = 0.0f;
    bool dragging = false, navigationEnabled = true;
};
} // namespace singleeditor
