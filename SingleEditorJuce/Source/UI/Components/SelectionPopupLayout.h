#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
struct SelectionPopupLayout final
{
    int height = 48;
    juce::Rectangle<int> label, down, up, slider, apply, cancel;

    static SelectionPopupLayout calculate(int width)
    {
        SelectionPopupLayout result;
        const int padding = width < 200 ? 8 : 12;
        auto row = juce::Rectangle<int>(padding, 8, juce::jmax(0, width - padding * 2), 28);
        const auto take = [&row] (int size)
        {
            auto bounds = row.removeFromLeft(juce::jmin(size, row.getWidth()));
            row.removeFromLeft(juce::jmin(6, row.getWidth()));
            return bounds;
        };
        result.label = take(58);
        result.down = take(width < 200 ? 26 : 42);
        result.up = take(width < 200 ? 26 : 42);
        if (width >= 402)
        {
            result.cancel = row.removeFromRight(62);
            row.removeFromRight(6);
            result.apply = row.removeFromRight(54);
            row.removeFromRight(6);
            result.slider = row;
        }
        else
        {
            result.height = width >= 260 ? 80 : 112;
            if (width >= 260) result.slider = row;
            else result.slider = { padding, 44, juce::jmax(0, width - padding * 2), 24 };
            auto actions = juce::Rectangle<int>(padding, result.height - 36, juce::jmax(0, width - padding * 2), 28);
            result.cancel = actions.removeFromRight(juce::jmin(62, (actions.getWidth() - 6) / 2));
            actions.removeFromRight(juce::jmin(6, actions.getWidth()));
            result.apply = actions.removeFromRight(juce::jmin(54, actions.getWidth()));
        }
        return result;
    }
};
} // namespace singleeditor
