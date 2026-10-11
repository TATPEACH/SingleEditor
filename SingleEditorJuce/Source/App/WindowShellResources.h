#pragma once
#include <juce_graphics/juce_graphics.h>
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
#include <atomic>
#endif

namespace singleeditor::windowShellResources
{
struct TrayImage final
{
    juce::Image image;
    juce::String status;
};

// Invoked only by the visible tray consumer, on its lazy worker after CoreReady.
// The executable-relative typed locator does no I/O by itself.
juce::File trayPackFile();
TrayImage loadTrayImage();

#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
inline std::atomic<int> packReadAttempts { 0 };
inline std::atomic<int> imageDecodes { 0 };
#endif
}
