#pragma once

namespace singleeditor
{
enum class StartupFocusLocation
{
    none,
    windowChrome,
    mainContent
};

struct StartupFocusState
{
    bool windowVisible = false;
    bool windowActive = false;
    bool mainContentAvailable = false;
    StartupFocusLocation focusLocation = StartupFocusLocation::none;
};

struct StartupFocusDecision
{
    bool activateWindow = false;
    bool focusMainContent = false;
};

StartupFocusDecision decideStartupFocus(StartupFocusState state) noexcept;
} // namespace singleeditor
