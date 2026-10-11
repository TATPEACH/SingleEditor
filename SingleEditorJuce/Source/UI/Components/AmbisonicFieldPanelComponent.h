#pragma once

#include <cstdint>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "UI/Components/SpeakerGeometryMagComponent.h"

namespace singleeditor
{
class AmbisonicFieldPanelComponent final : public juce::Component,
                                           private juce::Timer
{
public:
    enum class ActiveView
    {
        field,
        geometry
    };

    explicit AmbisonicFieldPanelComponent(AudioEngine& engineToUse);
    ~AmbisonicFieldPanelComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void setAmbisonicOrder(int order);
    void setActiveView(ActiveView view);
    ActiveView getActiveView() const noexcept { return activeView; }
    juce::String getAccessibilityValueSummary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    class VisibilityWatcher;
    void focusOfChildComponentChanged(FocusChangeType cause) override;
    void timerCallback() override;
    void refreshAnalysisLease();
    void rebuildEnergyMap();
    static bool directionForHammerPoint(float normalisedX,
                                        float normalisedY,
                                        float& azimuthDegrees,
                                        float& elevationDegrees) noexcept;
    static juce::Point<float> hammerProject(float azimuthDegrees,
                                            float elevationDegrees,
                                            juce::Rectangle<float> mapBounds) noexcept;
    static juce::Colour heatColour(float normalisedEnergy, float alpha);
    static bool speakerAngles(SpeakerRole role,
                               float& azimuthDegrees,
                               float& elevationDegrees) noexcept;
    void drawEnergyMap(juce::Graphics& g, juce::Rectangle<int> mapBounds) const;
    void drawMapGrid(juce::Graphics& g, juce::Rectangle<float> mapBounds) const;
    void drawOutputSpeakers(juce::Graphics& g, juce::Rectangle<float> mapBounds) const;
    void drawLegend(juce::Graphics& g, juce::Rectangle<int> legendBounds) const;

    AudioEngine& engine;
    AudioEngine::AmbisonicFieldAnalysisLease analysisLease;
    AmbisonicFieldSnapshot fieldSnapshot;
    OutputMeterSnapshot outputSnapshot;
    juce::Image energyMap;
    std::vector<float> energySamples;
    float peakEnergyDb = -120.0f;
    juce::TextButton fieldTab { "Field" };
    juce::TextButton geometryTab { "Geometry" };
    SpeakerGeometryMagComponent geometryEditor;
    juce::Viewport geometryViewport;
    ActiveView activeView = ActiveView::field;
    std::unique_ptr<VisibilityWatcher> visibilityWatcher;
};
} // namespace singleeditor
