#pragma once

#include <array>
#include <juce_graphics/juce_graphics.h>

namespace singleeditor
{
// Measured in JUCE logical pixels. Keep command groups intact at the breakpoint;
// the startup header and Classic renderer have their own unchanged layouts.
struct ModernTransportLayout final
{
    int height = 56;
    juce::Rectangle<int> open, clock, more, undo, redo;
    std::array<juce::Rectangle<int>, 8> transport;
    juce::Rectangle<int> meterMode, meter, peak, monitorLabel, monitor;
    juce::Rectangle<int> generalScene, simpleView;
    juce::Rectangle<int> lightTheme, nightTheme;

    static ModernTransportLayout calculate(int width)
    {
        ModernTransportLayout result;
        const bool twoRows = width < 1224;
        result.height = twoRows ? 92 : 56;
        auto row = juce::Rectangle<int>(10, twoRows ? 10 : 12, juce::jmax(0, width - 20), 32);
        auto take = [](juce::Rectangle<int>& area, int size, int gap = 4)
        {
            auto cell = area.removeFromLeft(juce::jmin(size, area.getWidth()));
            area.removeFromLeft(juce::jmin(gap, area.getWidth()));
            return cell;
        };
        auto secondRow = juce::Rectangle<int>(10, 50, juce::jmax(0, width - 20), 32);
        auto& tools = twoRows ? secondRow : row;
        const auto takeRight = [](juce::Rectangle<int>& area, int size, int gap = 12)
        {
            const auto cell = area.removeFromRight(size);
            area.removeFromRight(gap);
            return cell;
        };
        result.more = takeRight(row, 48);
        result.redo = takeRight(tools, 32, 4);
        result.undo = takeRight(tools, 32);
        // Keep scene choice beside playback whenever it fits. Appearance moves
        // as a whole pair; the lower row keeps balanced padding at every width.
        auto& appearanceRow = !twoRows || width >= 784 ? row : secondRow;
        result.nightTheme = takeRight(appearanceRow, 32, 2);
        result.lightTheme = takeRight(appearanceRow, 32);
        auto& sceneRow = !twoRows || width >= 706 ? row : secondRow;
        result.simpleView = takeRight(sceneRow, 52, 2);
        result.generalScene = takeRight(sceneRow, 60, 4);
        result.open = take(row, 32, 12);
        for (auto& button : result.transport)
            button = take(row, 32);
        row.removeFromLeft(8);
        result.clock = take(row, 156, 12);
        result.meterMode = take(tools, 52, 6);
        result.meter = take(tools, 48, 6);
        result.peak = take(tools, 76, 4);
        result.monitorLabel = take(tools, 48, 4);
        result.monitor = take(tools, 96);
        return result;
    }
};

struct ModernStatusLayout final
{
    static constexpr int height = 28;
    juce::Rectangle<int> file, status, details;

    static ModernStatusLayout calculate(int width)
    {
        auto content = juce::Rectangle<int>(0, 0, juce::jmax(0, width), height).reduced(16, 4);
        const auto quarter = content.getWidth() / 4;
        const auto file = content.removeFromLeft(quarter);
        const auto status = content.removeFromLeft(quarter * 2);
        return { file, status, content };
    }
};
} // namespace singleeditor
