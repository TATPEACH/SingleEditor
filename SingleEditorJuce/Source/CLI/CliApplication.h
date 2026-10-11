#pragma once

#include "Automation/OperationRegistry.h"
#include "CLI/CliArguments.h"

namespace singleeditor
{
struct CliExecution
{
    AutomationExitCode exitCode = AutomationExitCode::success;
    juce::String stdoutText;
    juce::String stderrText;
};

CliExecution executeCli(const CliArguments& arguments,
                        const OperationRegistry& registry);
} // namespace singleeditor
