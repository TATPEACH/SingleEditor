#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioSuite/TruePeakLimiterProcessor.h"
#include "UI/Workspace/WorkspaceVisualContractTarget.h"

namespace singleeditor
{
class TruePeakLimiterMagComponent final : public juce::Component,
                                          public juce::SettableTooltipClient,
                                          public WorkspaceVisualContractTarget
{
public:
    explicit TruePeakLimiterMagComponent(TruePeakLimiterParameters initialParameters);

    void setParameters(const TruePeakLimiterParameters& parameters);
    [[nodiscard]] const TruePeakLimiterParameters& getParameters() const noexcept;
    void setPreviewing(bool shouldPreview);
    void setApplying(bool shouldApply);
    void setRecallAvailable(bool available);
    void setStatusText(juce::String status);
    void setFeedback(TruePeakLimiterFeedbackSnapshot snapshot);
    void clearFeedback();

    void applyWorkspaceVisualContract(RendererDensity density,
                                      RendererMagSize size,
                                      RendererInformationTier tier,
                                      bool sweetSpotSatisfied) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void(const TruePeakLimiterParameters&)> onParametersChanged;
    std::function<void(bool)> onPreviewRequested;
    std::function<void()> onApplyRequested;
    std::function<void()> onRecallLastAppliedRequested;
    std::function<void(RendererMagSize)> onMagSizeRequested;

    void setEnabledForTesting(bool enabled);
    [[nodiscard]] bool isFeedbackVisibleForTesting() const noexcept;
    [[nodiscard]] const juce::String& getStatusTextForTesting() const noexcept;
    [[nodiscard]] bool areSizeControlsUsableForTesting() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getTransferCurveBounds() const noexcept { return feedbackBounds; }
    // Public-parameter target only, excluding lookahead/release and the TP guard.
    static double nominalOutputDb(const TruePeakLimiterParameters&, double inputDb) noexcept;

private:
    void configureButton(juce::TextButton& button,
                         const juce::String& text,
                         const juce::String& componentId);
    void configureSlider(juce::Slider& slider,
                         double minimum,
                         double maximum,
                         double interval,
                         const juce::String& suffix,
                         const juce::String& name);
    void updateControlsFromModel();
    void updateVisibilityAndLayout();
    void updateModernLayout();
    void paintModernTransferCurve(juce::Graphics&);
    void emitParametersChanged();
    void requestMagSize(RendererMagSize size);
    static juce::String formatDb(double value, const juce::String& suffix);

    TruePeakLimiterParameters parameters;
    TruePeakLimiterFeedbackSnapshot feedback;
    RendererDensity density = RendererDensity::standard;
    RendererMagSize magSize = RendererMagSize::medium;
    RendererInformationTier informationTier = RendererInformationTier::standard;
    bool sweetSpot = true;
    bool previewing = false;
    bool applying = false;
    bool recallAvailable = false;
    bool feedbackVisible = false;
    bool synchronisingControls = false;
    juce::String statusText = "Off | zero processing";
    juce::Rectangle<int> feedbackBounds;
    juce::Rectangle<int> modernStatusBounds;

    juce::ToggleButton enabledButton;
    juce::TextButton smallButton;
    juce::TextButton mediumButton;
    juce::TextButton largeButton;
    juce::Label thresholdLabel;
    juce::Slider thresholdSlider;
    juce::Label ceilingLabel;
    juce::Slider ceilingSlider;
    juce::Label releaseLabel;
    juce::ComboBox releaseModeBox;
    juce::Slider releaseSlider;
    juce::Label lockedFactsLabel;
    juce::TextButton recallButton;
    juce::TextButton previewButton;
    juce::TextButton applyButton;
};
} // namespace singleeditor
