#pragma once

#include "CLI/CliSingleFileAudioSuite.h"

#include <optional>

namespace singleeditor
{
// Claims only batch-plan and batch-apply. The orchestrator is compiled into the
// separate CLI target and never linked into the GUI.
std::optional<CliSingleFileExecution> tryExecuteM3BatchCli(
    const juce::StringArray& arguments);
} // namespace singleeditor
