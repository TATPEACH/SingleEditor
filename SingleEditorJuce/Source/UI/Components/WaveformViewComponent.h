#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "Document/SingleTrackDocument.h"
#include "Waveform/WaveformModel.h"
#include "WaveformOverviewComponent.h"

namespace singleeditor
{
class WaveformViewComponent final : public juce::Component,
                                    private juce::ChangeListener,
                                    private juce::Timer
{
public:
    enum class DisplayMode
    {
        stacked,
        mixed
    };

    enum class ViewMode
    {
        waveform,
        spectral
    };

    WaveformViewComponent(AudioEngine& engineToUse,
                          SingleTrackDocument& documentToUse,
                          WaveformModel& waveformModelToUse);
    ~WaveformViewComponent() override;
    void setSimplePresentation(bool enabled);
    WindowControlKind findControlAtPoint(juce::Point<float> point) const override;
    std::function<void()> onReturnToGeneral;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

    void refreshNow();
    void seekToTime(double seconds);
    void beginPresentationGeneration() noexcept;
    void notifyCoreReady();
    void clearSelection();
    void restoreSelection(std::optional<juce::Range<double>>);
    void selectWholeFile();
    void setSelectedSourceChannels(std::vector<int> sourceChannels);
    std::optional<juce::Range<double>> getSelectionRange() const noexcept;
    std::optional<juce::Rectangle<float>> getSelectionBoundsInLocal() const noexcept;
    ViewMode getViewMode() const noexcept;
    void setViewMode(ViewMode mode);
    DisplayMode getDisplayMode() const noexcept;
    void setDisplayMode(DisplayMode mode);
    void zoomHorizontal(double factor, std::optional<double> focusSeconds = std::nullopt);
    void zoomVertical(float factor) noexcept;
    void resetView() noexcept;
    void setSelectionPreviewGain(float linearGain) noexcept;
    void clearSelectionPreviewGain() noexcept;
    void setLoadingHint(juce::String primaryLine, juce::String secondaryLine);
    void setOpenFailureHint(juce::String fileName, juce::String reason);
    void setCommandAvailability(bool viewModeEnabled, bool displayModeEnabled);
    juce::String getAccessibilityValueSummary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    std::function<void(std::optional<juce::Range<double>>)> onSelectionChanged;
    std::function<void(int sourceChannel, bool extendSelection)> onSourceChannelSelectionGesture;
    std::function<void()> onWholeFileSelectionGesture;
    std::function<void(bool hasFullWaveform)> onWaveformPresentation;
    std::function<void()> onVisualContentChanged;
    std::function<void(ViewMode)> onViewModeRequested;
    std::function<void(DisplayMode)> onDisplayModeRequested;
    std::function<void(WaveformOverviewComponent::Action)> onNavigationRequested;

private:
    friend struct WaveformPresentationTestAccess;
    void prepareRequestedSpectrogram(bool retryAfterFailure = false);
    void repaintAndNotify();
    void repaintAndNotify(juce::Rectangle<int> bounds);
    void refreshOverview();
    juce::Rectangle<int> getOverviewBounds() const;
    std::optional<juce::Range<double>> getLoadedPreviewTimeRange() const noexcept;
    juce::Range<double> getVisibleTimeRange(double durationSeconds) const noexcept;
    void ensureTimeVisible(double seconds, double durationSeconds) noexcept;
    juce::Rectangle<float> getHeaderBounds() const;
    juce::Rectangle<float> getRulerBounds() const;
    juce::Rectangle<float> getPlotBounds() const;
    juce::Rectangle<float> getInteractiveBounds() const;
    float secondsToX(double seconds, juce::Rectangle<float> bounds, juce::Range<double> visibleRange) const;
    double xToSeconds(float x, juce::Rectangle<float> bounds, juce::Range<double> visibleRange) const;
    float gainToY(float gain, juce::Rectangle<float> plotBounds, float zoomFactor) const;
    float yToGain(float y, juce::Rectangle<float> plotBounds, float zoomFactor) const;
    int findEnvelopePointAt(juce::Point<float> position) const;
    struct ChannelButtonHit
    {
        enum class Type
        {
            none,
            solo,
            mute
        };

        int channel = -1;
        Type type = Type::none;
    };
    ChannelButtonHit hitTestChannelButton(juce::Point<float> position) const;
    int hitTestChannelLaneLabel(juce::Point<float> position) const;
    void drawTimeRuler(juce::Graphics& g, juce::Rectangle<float> rulerBounds, juce::Rectangle<float> waveformBounds, double durationSeconds) const;
    void drawBackgroundGrid(juce::Graphics& g, juce::Rectangle<float> plotBounds) const;
    void drawWaveforms(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const;
    void drawSpectrogram(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const;
    void drawEnvelope(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const;
    void drawEmptyState(juce::Graphics& g, juce::Rectangle<float> plotBounds) const;
    void updateSelectionFromPosition(float x);
    void notifySelectionChanged();
    void updateEnvelopePointFromMouse(const juce::MouseEvent& event);
    void panVisibleRangeBy(double deltaSeconds) noexcept;
    void notifyWaveformPresentationIfNeeded() noexcept;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    AudioEngine& engine;
    SingleTrackDocument& document;
    WaveformModel& waveformModel;
    juce::Label waveformTitle;
    juce::Label selectionSummary;
    juce::TextButton viewOptionsButton { "View" };
    juce::TextButton returnToGeneralButton { "Back" };
    bool simplePresentation = false;
    juce::TextButton waveformTab;
    juce::TextButton spectralTab;
    juce::TextButton stackedViewButton;
    juce::TextButton mixedViewButton;
    std::unique_ptr<WaveformOverviewComponent> overview;
    bool navigationEnabled = false;
    ViewMode viewMode = ViewMode::waveform;
    bool corePresentationReady = false;
    DisplayMode displayMode = DisplayMode::stacked;
    std::optional<juce::Range<double>> selectionRange;
    std::vector<int> selectedSourceChannels;
    juce::Point<float> mouseDownPosition;
    double selectionAnchorSeconds = 0.0;
    bool selectionDragActive = false;
    bool panDragActive = false;
    bool pointerMovedDuringSelection = false;
    int draggingEnvelopePoint = -1;
    float previewSelectionGainLinear = 1.0f;
    double lastCursorSeconds = -1.0;
    int64_t lastAutoFocusedPreviewRevision = -1;
    double horizontalZoom = 1.0;
    float verticalZoom = 1.0f;
    double visibleRangeStartSeconds = 0.0;
    float panDragAnchorX = 0.0f;
    double panDragStartVisibleStart = 0.0;
    int64_t lastPresentedWaveformRevision = -1;
    bool lastPresentedWaveformWasFull = false;
    juce::String loadingPrimaryText;
    bool openFailureHint = false;
    juce::String loadingSecondaryText;
};
} // namespace singleeditor
