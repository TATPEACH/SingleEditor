#include "Audio/BoundedDecodeProcess.h"
#include <iostream>
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

using Process = singleeditor::media::BoundedDecodeProcess;
namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
#if JUCE_WINDOWS
int child(const juce::StringArray& args)
{
    const auto mode = args[1];
    const juce::File output(args[2]);
    if (mode == "args") return output.replaceWithText(args.joinIntoString("|", 3)) ? 0 : 11;
    if (mode == "hang") { output.replaceWithText("started"); Sleep(10000); return 0; }
    if (mode == "write") return output.replaceWithText(juce::String::repeatedString("x", 8192)) ? 0 : 12;
    if (mode == "memory")
    {
        auto* block = VirtualAlloc(nullptr, 128 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (block != nullptr) { VirtualFree(block, 0, MEM_RELEASE); return 13; }
        return 0;
    }
    if (mode == "delayed") { Sleep(1000); output.replaceWithText("escaped"); return 0; }
    if (mode == "tree" || mode == "processCap" || mode == "breakaway")
    {
        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        const auto childCommand = "\"" + executable.getFullPathName() + "\" --child delayed \""
                                  + output.getFullPathName() + ".child\"";
        std::wstring command(childCommand.toWideCharPointer());
        STARTUPINFOW startup {}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION created {};
        const auto flags = CREATE_NO_WINDOW | (mode == "breakaway" ? CREATE_BREAKAWAY_FROM_JOB : 0);
        const auto ok = CreateProcessW(executable.getFullPathName().toWideCharPointer(), command.data(),
                                      nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &created);
        if (ok) { CloseHandle(created.hThread); CloseHandle(created.hProcess); }
        if (mode != "tree") return ok ? 14 : 0;
        if (!ok) return 15;
        return output.replaceWithText(juce::String(static_cast<int64_t>(created.dwProcessId))) ? 0 : 16;
    }
    if (mode == "exit") return 37;
    return 17;
}
#endif
}

#if JUCE_WINDOWS
int wmain(int argc, wchar_t** argv)
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String(argv[i]));
    if (args[0] == "--child") return child(args);
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = temp.getChildFile("SingleEditor-DecodeTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private fixture");
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    std::atomic<bool> cancel { false };
    Process::Limits limits;
    limits.wallTimeMs = 5000;
    auto file = root.getChildFile("argv.txt");
    const juce::StringArray payload { "", "two words", "a\"b", "C:\\tail\\", "\\\"", "&|%!(x)", juce::String::fromUTF8("音频") };
    juce::StringArray invocation { "--child", "args", file.getFullPathName() };
    invocation.addArray(payload);
    auto result = Process::run(executable, invocation, file, limits, cancel);
    require(static_cast<bool>(result), "bounded child success");
    require(file.loadFileAsString() == payload.joinIntoString("|"), "argv roundtrip without shell interpretation");
    auto run = [&](const juce::String& mode, const Process::Limits& policy)
    {
        file = root.getChildFile(mode + ".txt");
        return Process::run(executable, { "--child", mode, file.getFullPathName() }, file, policy, cancel);
    };
    auto tightLimits = limits; tightLimits.outputBytes = 4096;
    require(run("write", tightLimits).failure == Process::Failure::outputLimit, "fast output burst checked after exit");
    auto shortTime = limits; shortTime.wallTimeMs = 150;
    require(run("hang", shortTime).failure == Process::Failure::deadline, "hung child deadline");
    const auto cancelledOutput = root.getChildFile("cancel.txt");
    std::thread canceller([&]
    {
        for (int i = 0; i < 300 && !cancelledOutput.existsAsFile(); ++i) Sleep(5);
        cancel.store(true, std::memory_order_release);
    });
    result = Process::run(executable, { "--child", "hang", cancelledOutput.getFullPathName() }, cancelledOutput, limits, cancel);
    canceller.join();
    require(result.failure == Process::Failure::cancelled && cancelledOutput.existsAsFile(), "cancel a running child");
    const auto noLaunch = root.getChildFile("never.txt");
    require(Process::run(executable, { "--child", "write", noLaunch.getFullPathName() }, noLaunch, limits, cancel).failure
            == Process::Failure::cancelled && !noLaunch.existsAsFile(), "pre-cancel does not launch");
    cancel.store(false);
    tightLimits = limits; tightLimits.memoryBytes = 64 * 1024 * 1024;
    require(static_cast<bool>(run("memory", tightLimits)), "Job refuses allocation beyond memory quota");
    require(static_cast<bool>(run("processCap", limits)), "Job enforces active process ceiling");
    require(static_cast<bool>(run("breakaway", limits)), "Job denies explicit breakaway");
    auto tree = limits; tree.processes = 2;
    require(static_cast<bool>(run("tree", tree)), "parent success with owned descendant");
    const auto pid = static_cast<DWORD>(file.loadFileAsString().getLargeIntValue());
    const auto descendant = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (descendant != nullptr)
    {
        require(WaitForSingleObject(descendant, 0) == WAIT_OBJECT_0, "descendant terminated before run returns");
        CloseHandle(descendant);
    }
    Sleep(1100);
    require(!file.getSiblingFile(file.getFileName() + ".child").existsAsFile(), "descendant cannot publish after owner exits");
    result = run("exit", limits);
    require(result.failure == Process::Failure::decoderExit && result.exitCode == 37, "preserve failed exit code");
    file = root.getChildFile("validation.txt");
    require(Process::run(executable, { "--child", "hang", file.getFullPathName() }, file, limits, cancel,
                        [] { return Process::Failure::invalidOutput; }).failure == Process::Failure::invalidOutput,
            "validator aborts the owned job");
    auto invalid = limits; invalid.processes = 0;
    require(run("invalid", invalid).failure == Process::Failure::invalidRequest, "invalid policy fails before launch");
    require(root.isAChildOf(temp) && root.getParentDirectory() == temp
            && root.getFileName().startsWith("SingleEditor-DecodeTest-"), "owned fixture boundary");
    require(root.deleteRecursively(), "remove private fixture");
    std::cout << "BoundedDecodeProcessTests passed (argv, output, deadline, cancellation, memory, process/tree limits, validation)\n";
    return 0;
}
#else
int main() { std::cout << "Windows process containment tests unavailable on this platform\n"; return 77; }
#endif
