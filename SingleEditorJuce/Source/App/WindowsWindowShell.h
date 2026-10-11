#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace singleeditor
{
class WindowsWindowShell final
{
public:
    void configureWindow(juce::DocumentWindow& window) const;
    static void setSimplePresentation(juce::DocumentWindow& window, bool simple);
    static juce::Component::WindowControlKind findControlAtPoint(
        const juce::DocumentWindow& window, juce::Point<float> point);
    bool usesNativeFrame() const noexcept;
    bool providesWindowTitleBar() const noexcept;
};
} // namespace singleeditor
