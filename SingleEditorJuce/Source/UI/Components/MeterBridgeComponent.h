#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"

namespace singleeditor
{
class MeterBridgeComponent final : public juce::Component,
                                   private juce::Timer
{
public:
    explicit MeterBridgeComponent(AudioEngine& engineToUse);
    ~MeterBridgeComponent() override;
    void setCoreReady(bool);

    void setMeterMode(AudioEngine::MeterMode newMode) noexcept;
    AudioEngine::MeterMode getMeterMode() const noexcept { return mode; }
    void resetPeakHold() noexcept;

    void paint(juce::Graphics& g) override;

private:
    class VisibilityWatcher;
    void refreshActivity();
    static float normaliseMeterValue(AudioEngine::MeterMode meterMode, const AudioEngine& engine) noexcept;

    void timerCallback() override;

    AudioEngine& engine;
    AudioEngine::OutputMeterAnalysisLease lease;
    std::unique_ptr<VisibilityWatcher> watcher;
    bool coreReady = false;
    AudioEngine::MeterMode mode = AudioEngine::MeterMode::truePeak;
    float displayedLevel = 0.0f;
    float heldLevel = 0.0f;
};
} // namespace singleeditor
