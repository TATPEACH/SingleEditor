#pragma once
#include "CLI/CliSingleFileAudioSuite.h"
#include <atomic>
#include <functional>

namespace singleeditor
{
struct DiagnosticCliInteraction final
{
    // Display the full effect/snapshot preview before reading its exact ID.
    // Missing callback, EOF or any other response refuses the operation.
    std::function<juce::String(const juce::String&)> confirm;
    const std::atomic<bool>* cancelled = nullptr;
    juce::File workerExecutable; // absent = this running executable
};
// Dispatched before any operation registry; unrelated commands are inert.
std::optional<CliSingleFileExecution> tryExecuteDiagnosticsCli(const juce::StringArray&,
                                                              const DiagnosticCliInteraction& = {});
}
