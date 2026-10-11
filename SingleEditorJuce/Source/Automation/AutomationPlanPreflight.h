#pragma once

#include "Automation/AutomationExitCode.h"

#include <juce_core/juce_core.h>

namespace singleeditor
{
struct AutomationPlanPreflightResult
{
    AutomationExitCode exitCode = AutomationExitCode::invalidArguments;
    juce::String operationId;
    juce::String errorMessage;
    juce::var payload;
};

class AutomationPlanPreflight
{
public:
    static AutomationPlanPreflightResult run(const juce::File& planFile);
};
} // namespace singleeditor
