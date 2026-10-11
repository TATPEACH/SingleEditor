#include "Automation/BuiltInOperations.h"
#include "Automation/OperationRegistry.h"
#include "CLI/CliApplication.h"
#include "CLI/CliArguments.h"
#include "CLI/CliBatchAutomation.h"
#include "CLI/CliSingleFileAudioSuite.h"
#include "CLI/CliDiagnostics.h"
#include "Diagnostics/CodecDiagnosticProbe.h"
#include "Diagnostics/DeviceDiagnosticProbe.h"

#include <iostream>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
#if JUCE_WINDOWS
std::atomic<bool> diagnosticCancellation { false };
BOOL WINAPI cancelDiagnostic(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT)
    { diagnosticCancellation.store(true); return TRUE; }
    return FALSE;
}
#endif
struct DiagnosticConsoleCancellation final
{
#if JUCE_WINDOWS
    std::atomic<bool>& cancelled = diagnosticCancellation;
#else
    std::atomic<bool> cancelled { false };
#endif
    DiagnosticConsoleCancellation()
    {
#if JUCE_WINDOWS
        cancelled.store(false); SetConsoleCtrlHandler(cancelDiagnostic, TRUE);
#endif
    }
    ~DiagnosticConsoleCancellation()
    {
#if JUCE_WINDOWS
        SetConsoleCtrlHandler(cancelDiagnostic, FALSE);
#endif
    }
    juce::String confirm(const juce::String& preview)
    {
        std::cout << preview << std::flush;
        // No source lease, child or publication exists while waiting for input.
        // Restore normal Ctrl+C behavior so an idle stdin wait aborts immediately.
#if JUCE_WINDOWS
        SetConsoleCtrlHandler(cancelDiagnostic, FALSE);
#endif
        std::string answer; char character = 0; bool newline = false;
        while (answer.size() <= 64 && std::cin.get(character))
        { if (character == '\n') { newline = true; break; } if (character != '\r') answer += character; }
#if JUCE_WINDOWS
        SetConsoleCtrlHandler(cancelDiagnostic, TRUE);
#endif
        return newline ? juce::String(answer) : juce::String();
    }
};
}

int main(int argc, char* argv[])
{
    juce::StringArray arguments;
    for (int index = 1; index < argc; ++index)
        arguments.add(juce::String::fromUTF8(argv[index]));

    if (const auto worker = singleeditor::diagnostics::CodecDiagnosticProbe::tryWorker(arguments)) return *worker;
    if (const auto worker = singleeditor::diagnostics::DeviceDiagnosticProbe::tryWorker(arguments)) return *worker;
    if (!arguments.isEmpty() && arguments[0] == "diagnostics")
    {
        DiagnosticConsoleCancellation console;
        singleeditor::DiagnosticCliInteraction interaction;
        interaction.cancelled = &console.cancelled;
        interaction.confirm = [&](const juce::String& preview) { return console.confirm(preview); };
        const auto execution = singleeditor::tryExecuteDiagnosticsCli(arguments, interaction);
        if (execution->stdoutText.isNotEmpty()) std::cout << execution->stdoutText;
        if (execution->stderrText.isNotEmpty()) std::cerr << execution->stderrText;
        return static_cast<int>(execution->exitCode);
    }

    // M3 machine commands are resolved before the legacy registry exists.
    // Cold discovery therefore cannot instantiate operations or device work.
    if (const auto execution = singleeditor::tryExecuteM3BatchCli(arguments))
    {
        if (execution->stdoutText.isNotEmpty())
            std::cout << execution->stdoutText;
        if (execution->stderrText.isNotEmpty())
            std::cerr << execution->stderrText;
        return static_cast<int>(execution->exitCode);
    }
    if (const auto execution = singleeditor::tryExecuteM3SingleFileCli(arguments))
    {
        if (execution->stdoutText.isNotEmpty())
            std::cout << execution->stdoutText;
        if (execution->stderrText.isNotEmpty())
            std::cerr << execution->stderrText;
        return static_cast<int>(execution->exitCode);
    }

    singleeditor::OperationRegistry registry;
    singleeditor::registerBuiltInReadOnlyOperations(registry);
    const auto execution = singleeditor::executeCli(
        singleeditor::parseCliArguments(arguments), registry);

    if (execution.stdoutText.isNotEmpty())
        std::cout << execution.stdoutText;
    if (execution.stderrText.isNotEmpty())
        std::cerr << execution.stderrText;
    return static_cast<int>(execution.exitCode);
}
