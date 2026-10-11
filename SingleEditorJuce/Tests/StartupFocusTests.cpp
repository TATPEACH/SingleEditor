#include "App/StartupFocusPolicy.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

void runStartupFocusPolicyTests()
{
    auto decision = decideStartupFocus(
        { true, false, false, StartupFocusLocation::none });
    require(decision.activateWindow, "visible cold window requests activation");
    require(!decision.focusMainContent, "missing main content cannot receive focus");

    decision = decideStartupFocus(
        { true, true, true, StartupFocusLocation::none });
    require(!decision.activateWindow, "active window is not raised again");
    require(decision.focusMainContent, "revealed main content receives empty keyboard focus");

    decision = decideStartupFocus(
        { true, true, true, StartupFocusLocation::windowChrome });
    require(!decision.activateWindow && decision.focusMainContent,
            "window chrome focus is transferred to editor content");

    decision = decideStartupFocus(
        { true, true, true, StartupFocusLocation::mainContent });
    require(!decision.activateWindow && !decision.focusMainContent,
            "existing editor child focus is preserved");

    decision = decideStartupFocus(
        { false, false, true, StartupFocusLocation::none });
    require(!decision.activateWindow && !decision.focusMainContent,
            "hidden window never steals focus");
}
}

int main()
{
    runStartupFocusPolicyTests();
    std::cout << "StartupFocusTests passed\n";
    return 0;
}
