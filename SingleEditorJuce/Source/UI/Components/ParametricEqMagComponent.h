#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioSuite/ParametricEqProcessor.h"
#include "UI/Workspace/WorkspaceVisualContractTarget.h"

namespace singleeditor
{
class ParametricEqMagComponent final : public juce::Component,
                                       public WorkspaceVisualContractTarget
{
public:
    enum class ViewMode
    {
        simple,
        advanced
    };

    explicit ParametricEqMagComponent(ParametricEqParameters initialParameters);
    ~ParametricEqMagComponent() override;

    void setParameters(const ParametricEqParameters& parameters);
    [[nodiscard]] const ParametricEqParameters& getParameters() const noexcept;
    [[nodiscard]] ViewMode getViewMode() const noexcept;
    void setViewMode(ViewMode mode, bool requestSweetSpot = true);
    [[nodiscard]] std::size_t getSelectedAdvancedBand() const noexcept;
    void setSelectedAdvancedBand(std::size_t band);
    void setPreviewing(bool shouldPreview);
    void setApplying(bool shouldApply);
    void setStatusText(juce::String status);
    void setSourceSampleRate(double sampleRate);
    void setExactMagnitudeCurve(std::vector<float> magnitudeDb,
                                double minimumHz = 20.0, double maximumHz = 20000.0);
    void clearExactMagnitudeCurve();

    void applyWorkspaceVisualContract(RendererDensity density,
                                      RendererMagSize size,
                                      RendererInformationTier tier,
                                      bool sweetSpotSatisfied) override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;

    std::function<void(const ParametricEqParameters&)> onParametersChanged;
    std::function<void(ViewMode)> onViewModeChanged;
    std::function<void(std::size_t)> onSelectedAdvancedBandChanged;
    std::function<void(bool)> onPreviewRequested;
    std::function<void()> onApplyRequested;
    std::function<void(RendererMagSize)> onMagSizeRequested;

    void setSimpleGainForTesting(int controlIndex, double gainDb);
    void setCompactBandGainForTesting(int bandIndex, double gainDb);
    [[nodiscard]] bool isBandControlVisibleForTesting(int bandIndex) const;
    [[nodiscard]] int visibleCompactBandGainCountForTesting() const noexcept;
    [[nodiscard]] bool compactBandGainsFitBoundsForTesting() const noexcept;
    [[nodiscard]] bool areSizeControlsUsableForTesting() const noexcept;
    [[nodiscard]] juce::Rectangle<int> curveBoundsForTesting() const noexcept { return curveBounds; }
    [[nodiscard]] const auto& bandResponsesForTesting() const noexcept { return bandMagnitudeDb; }

private:
    class NumericSlider final : public juce::Slider, public juce::KeyListener,
                                public juce::AsyncUpdater
    {
    public:
        ~NumericSlider() override;
        void childrenChanged() override;
        void lookAndFeelChanged() override;
        void abandonEditor();
        using juce::Slider::keyPressed;
        bool keyPressed(const juce::KeyPress&, juce::Component*) override;
        std::function<void()> onEditBegin;
        std::function<void(double)> onLiveValueChange;
        std::function<void(bool)> onEditEnd;

    private:
        std::optional<double> parseDraft();
        void handleAsyncUpdate() override;
        void finishEditor(bool cancelled);
        juce::Component::SafePointer<juce::TextEditor> activeEditor;
        std::optional<double> lastLiveValue;
    };

    enum class NumericField { frequency, gain, q };

    struct BandControls
    {
        juce::ToggleButton enabled;
        juce::ComboBox type;
        NumericSlider frequency;
        NumericSlider gain;
        NumericSlider q;
        juce::ComboBox slope;
    };

    void configureButton(juce::TextButton& button,
                         const juce::String& text,
                         const juce::String& componentId);
    void configureSlider(NumericSlider& slider,
                         double minimum,
                         double maximum,
                         double interval,
                         const juce::String& suffix,
                         const juce::String& name);
    void configureBandControls(std::size_t index);
    void updateControlsFromModel();
    void refreshCoefficientCurve();
    void refreshBandCurves();
    void updateVisibilityAndLayout();
    void updateModernLayout();
    void paintModern(juce::Graphics& g);
    void emitParametersChanged();
    void handlePresetChanged();
    void handleBandControlChanged(std::size_t index);
    void handleSimpleGainChanged(std::size_t controlIndex);
    void handleNumericValueChanged(std::size_t index, NumericField field, double value,
                                   bool forceEnabled = false);
    void finishNumericEdit(bool cancelled);
    void abandonNumericEditors();
    void requestMagSize(RendererMagSize size);
    void paintCurve(juce::Graphics& g) const;

    static ParametricEqFilterType filterTypeForId(int id) noexcept;
    static int idForFilterType(ParametricEqFilterType type) noexcept;

    ParametricEqParameters parameters;
    ViewMode viewMode = ViewMode::simple;
    RendererDensity density = RendererDensity::standard;
    RendererMagSize magSize = RendererMagSize::medium;
    RendererInformationTier informationTier = RendererInformationTier::standard;
    bool sweetSpot = true;
    bool previewing = false;
    bool applying = false;
    bool synchronisingControls = false;
    std::optional<ParametricEqParameters> numericEditStart;
    juce::String statusText = "Ready | Preview is audition-only";
    std::vector<float> exactMagnitudeDb;
    std::array<std::vector<float>, ParametricEqParameters::bandCount> bandMagnitudeDb;
    double curveMinimumHz = 20.0, curveMaximumHz = 20000.0;
    double sourceSampleRate = 0.0;
    juce::Rectangle<int> curveBounds;
    std::array<juce::Rectangle<int>, ParametricEqParameters::bandCount> modernBandBounds;
    juce::Rectangle<int> modernStatusBounds;
    bool modernDetailsUnavailable = false;
    bool modernAllBandDetails = false;
    bool compactBandGainLayout = false;
    std::unique_ptr<juce::LookAndFeel_V4> compactKnobLookAndFeel;

    juce::TextButton simpleButton;
    juce::TextButton advancedButton;
    juce::TextButton smallButton;
    juce::TextButton mediumButton;
    juce::TextButton largeButton;
    juce::ComboBox presetBox;
    juce::ComboBox advancedBandBox;
    juce::TextButton previewButton;
    juce::TextButton applyButton;

    std::array<juce::Label, 3> simpleLabels;
    std::array<NumericSlider, 3> simpleGainSliders;
    std::array<BandControls, ParametricEqParameters::bandCount> bandControls;
    std::size_t selectedAdvancedBand = 0;
};
} // namespace singleeditor
