#include "App/StartupFocusPolicy.h"

namespace singleeditor
{
StartupFocusDecision decideStartupFocus(StartupFocusState state) noexcept
{
    if (!state.windowVisible)
        return {};

    if (!state.windowActive)
        return { true, false };

    return { false,
             state.mainContentAvailable
                 && state.focusLocation != StartupFocusLocation::mainContent };
}
} // namespace singleeditor
