#include "App/WindowsWindowShell.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"

namespace singleeditor
{
juce::Component::WindowControlKind WindowsWindowShell::findControlAtPoint(
    const juce::DocumentWindow& window, juce::Point<float> point)
{
#if JUCE_WINDOWS
    if (!window.isUsingNativeTitleBar()
        && window.getTitleBarArea().withTop(0).toFloat().contains(point))
    {
        // Keep JUCE's resize-border precedence, including the caption corners.
        for (auto* child : window.getChildren())
            if (const auto* border = dynamic_cast<juce::ResizableBorderComponent*>(child))
                if (juce::ResizableBorderComponent::Zone::fromPositionOnBorder(
                        window.getLocalBounds(), border->getBorderThickness(), point.roundToInt())
                        .getZoneFlags() != 0)
                    return window.juce::DocumentWindow::findControlAtPoint(point);

        // Component::contains reaches the HWND peer/WindowFromPoint, which can
        // send WM_NCHITTEST back here. This query already has a window-local
        // point: use local button geometry, without another system hit test.
        using Kind = juce::Component::WindowControlKind;
        for (const auto& [button, kind] : {
                 std::pair { window.getMinimiseButton(), Kind::minimise },
                 std::pair { window.getMaximiseButton(), Kind::maximise },
                 std::pair { window.getCloseButton(), Kind::close } })
        {
            if (button == nullptr || !button->isVisible())
                continue;
            const auto local = button->getLocalPoint(&window, point);
            const auto pixel = local.roundToInt();
            if (button->getLocalBounds().toFloat().contains(local)
                && button->hitTest(pixel.x, pixel.y))
                return kind;
        }
    }
#endif
    return window.juce::DocumentWindow::findControlAtPoint(point);
}

void WindowsWindowShell::setSimplePresentation(juce::DocumentWindow& window, bool simple)
{
    // Keep required native controls/style bits unchanged. Replacing the buttons
    // recreates the HWND, interrupts focus and needlessly rebuilds its renderer.
    window.setTitleBarHeight(simple ? 0 : WindowChromeLookAndFeel::captionHeight);
    for (auto* button : { window.getMinimiseButton(), window.getMaximiseButton(), window.getCloseButton() })
        if (button != nullptr) button->setVisible(!simple);
}

void WindowsWindowShell::configureWindow(juce::DocumentWindow& window) const
{
#if JUCE_WINDOWS
    window.setUsingNativeTitleBar(usesNativeFrame());
    // JUCE's Win32 peer needs its native-shadow flag to retain WS_CAPTION /
    // WS_THICKFRAME on an opaque custom frame. Without it Windows treats the
    // window as a plain popup: caption dragging and work-area maximization fail.
    // The chrome LAF suppresses only JUCE's software shadow helper windows.
    window.setDropShadowEnabled(!usesNativeFrame());
    if (!usesNativeFrame())
        window.setTitleBarHeight(WindowChromeLookAndFeel::captionHeight);
    window.setTitleBarButtonsRequired(juce::DocumentWindow::allButtons, false);
#else
    window.setUsingNativeTitleBar(false);
    window.setTitleBarHeight(0);
#endif
}

bool WindowsWindowShell::usesNativeFrame() const noexcept
{
#if JUCE_WINDOWS
    return RendererVisualStyle::isClassic();
#else
    return false;
#endif
}

bool WindowsWindowShell::providesWindowTitleBar() const noexcept
{
#if JUCE_WINDOWS
    return true;
#else
    return false;
#endif
}
} // namespace singleeditor
