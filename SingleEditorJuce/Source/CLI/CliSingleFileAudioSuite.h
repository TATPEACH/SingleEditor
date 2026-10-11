#pragma once

#include "Automation/AutomationExitCode.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace singleeditor
{
struct CliSingleFileExecution
{
    AutomationExitCode exitCode = AutomationExitCode::success;
    juce::String stdoutText;
    juce::String stderrText;
};

// Returns no value when the arguments belong to the legacy M2 CLI surface.
// Recognised M3 commands are handled without constructing OperationRegistry.
std::optional<CliSingleFileExecution> tryExecuteM3SingleFileCli(
    const juce::StringArray& arguments);

// Strictly validates and canonicalises an M3 single-file Plan without opening
// its source or constructing a processor. Used by the bounded batch importer.
bool inspectM3SingleFilePlan(const juce::File& planFile,
                             juce::var& canonicalPlan,
                             juce::String& error);
bool inspectM3SingleFilePlanValue(const juce::var& planValue,
                                  juce::var& canonicalPlan,
                                  juce::String& error);
} // namespace singleeditor
