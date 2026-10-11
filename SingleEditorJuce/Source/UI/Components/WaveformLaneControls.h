#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
// One geometry for drawing and hit testing. Tall audio lanes must not stretch
// their Solo/Mute controls into bars covering the waveform.
struct WaveformLaneControls final
{
    juce::Rectangle<float> label, solo, mute;

    static WaveformLaneControls calculate(juce::Rectangle<float> lane, bool modern) noexcept
    {
        if (lane.isEmpty()) return {};
        const auto laneHeight = lane.getHeight();
        if (modern) lane.setHeight(juce::jmin(24.0f, laneHeight));
        auto controls = lane.removeFromLeft(juce::jmin(modern ? 126.0f : 112.0f, lane.getWidth()));
        const auto label = controls.removeFromLeft(juce::jmin(modern ? 64.0f : 50.0f, controls.getWidth()));
        const auto inset = modern ? juce::jmin(2.0f, laneHeight * 0.1f)
                                  : juce::jmax(2.0f, laneHeight * 0.18f);
        const auto takeButton = [&]()
        {
            auto cell = controls.removeFromLeft(juce::jmin(26.0f, controls.getWidth()));
            return cell.reduced(juce::jmin(3.0f, cell.getWidth() * 0.25f), inset);
        };
        const auto solo = takeButton();
        const auto mute = takeButton();
        return { label, solo, mute };
    }
};
} // namespace singleeditor
