#pragma once

#include "Automation/AutomationExitCode.h"

#include <juce_core/juce_core.h>

namespace singleeditor
{
struct AutomationPlanApplyOptions
{
    bool inPlace = false;
    juce::File outputDirectory;
    juce::String conflictPolicy { "fail" };
    bool backup = false;
    juce::File backupDirectory;
    bool allowLossyReencode = false;
    bool failFast = false;
    juce::File manifestFile;
};

struct AutomationPlanApplyResult
{
    AutomationExitCode exitCode = AutomationExitCode::invalidArguments;
    juce::String operationId;
    juce::String errorMessage;
    juce::var payload;
};

class AutomationPlanApply final
{
public:
    static AutomationPlanApplyResult run(const juce::File& planFile,
                                         const AutomationPlanApplyOptions& options);
};
} // namespace singleeditor
