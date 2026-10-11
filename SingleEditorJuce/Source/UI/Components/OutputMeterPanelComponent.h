#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "UI/Components/OutputMeterDisplayState.h"

namespace singleeditor
{
class OutputMeterPanelComponent final : public juce::Component,
                                        private juce::Timer
{
public:
    explicit OutputMeterPanelComponent(AudioEngine& engineToUse);
    ~OutputMeterPanelComponent() override;
    void setCoreReady(bool);

    void setMeterMode(AudioEngine::MeterMode newMode) noexcept;
    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getAccessibilityValueSummary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    class VisibilityWatcher;
    void refreshActivity();
    static juce::String modeLabel(AudioEngine::MeterMode mode);
    void timerCallback() override;

    AudioEngine& engine;
    AudioEngine::OutputMeterAnalysisLease lease;
    std::unique_ptr<VisibilityWatcher> watcher;
    bool coreReady = false;
    AudioEngine::MeterMode mode = AudioEngine::MeterMode::truePeak;
    OutputMeterDisplayState display;
    juce::TextButton resetHoldButton { "Reset" };
};
} // namespace singleeditor
