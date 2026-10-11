#pragma once

#include <juce_graphics/juce_graphics.h>

namespace singleeditor::windowPlacement
{
// A window may deliberately span displays or extend below a work area. Recover
// only when there is no usable title span to drag it back, not merely when a
// saved rectangle is partly outside a monitor.
inline bool hasReachableCaption(juce::Rectangle<int> bounds, juce::Rectangle<int> workArea)
{
    if (bounds.isEmpty() || workArea.isEmpty())
        return false;

    const auto caption = bounds.withHeight(juce::jmin(32, bounds.getHeight()));
    const auto visible = caption.getIntersection(workArea);
    return visible.getHeight() == caption.getHeight()
        && visible.getWidth() >= juce::jmin(160, bounds.getWidth());
}

inline juce::Rectangle<int> fitInWorkArea(juce::Rectangle<int> bounds,
                                         juce::Rectangle<int> workArea)
{
    if (bounds.isEmpty() || workArea.isEmpty())
        return bounds;

    bounds.setSize(juce::jmin(bounds.getWidth(), workArea.getWidth()),
                   juce::jmin(bounds.getHeight(), workArea.getHeight()));
    bounds.setPosition(juce::jlimit(workArea.getX(), workArea.getRight() - bounds.getWidth(), bounds.getX()),
                       juce::jlimit(workArea.getY(), workArea.getBottom() - bounds.getHeight(), bounds.getY()));
    return bounds;
}
} // namespace singleeditor::windowPlacement
