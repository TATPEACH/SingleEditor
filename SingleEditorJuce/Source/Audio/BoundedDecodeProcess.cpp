#include "Audio/BoundedDecodeProcess.h"
#include <chrono>
#include <array>
#include <string>
#include <vector>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::media
{
#if JUCE_WINDOWS
namespace
{
struct Handle final
{
    HANDLE value = nullptr;
    ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
// Windows CRT argv quoting: quotes, spaces, empty values and trailing slashes
// survive unchanged. No command interpreter or command-string API is exposed.
std::wstring quoted(const juce::String& argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto ch : std::wstring(argument.toWideCharPointer()))
    {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
bool finishJob(HANDLE job)
{
    // ActiveProcesses reaches zero before all process handles necessarily become
    // signalled. Retain owned descendants while they are still enumerable, then
    // wait for their kernel teardown as well as the Job accounting boundary.
    struct ProcessIds { DWORD assigned; DWORD count; ULONG_PTR ids[4]; } ids {};
    std::array<Handle, 4> descendants;
    if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, &ids, sizeof(ids), nullptr))
        return false;
    for (DWORD i = 0; i < ids.count && i < descendants.size(); ++i)
        descendants[i].value = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(ids.ids[i]));
    if (!TerminateJobObject(job, ERROR_CANCELLED)) return false;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting {};
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                       &accounting, sizeof(accounting), nullptr)) return false;
        if (accounting.ActiveProcesses == 0)
        {
            for (const auto& descendant : descendants)
                if (descendant.value != nullptr && WaitForSingleObject(descendant.value, 1000) != WAIT_OBJECT_0)
                    return false;
            return true;
        }
        Sleep(5);
    }
    return false; // Handle close still supplies the kill-on-close backstop.
}
}
#endif

BoundedDecodeProcess::Result BoundedDecodeProcess::run(
    const juce::File& executable, const juce::StringArray& arguments,
    const juce::File& staging, const Limits& limits, const std::atomic<bool>& cancel,
    const std::function<Failure()>& inspect
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    , std::uint64_t observedParentResource, OptionalLifecycleOwner observedOwner
#endif
    )
{
    if (cancel.load(std::memory_order_acquire)) return { Failure::cancelled };
    constexpr uint64_t gib = 1024ULL * 1024 * 1024;
    if (!executable.existsAsFile() || staging == juce::File() || staging.isDirectory()
        || limits.wallTimeMs < 1 || limits.wallTimeMs > 600000
        || limits.memoryBytes < 16 * 1024 * 1024 || limits.memoryBytes > 8 * gib
        || limits.outputBytes == 0 || limits.outputBytes > 8 * gib
        || limits.processes < 1 || limits.processes > 4)
        return { Failure::invalidRequest };
#if JUCE_WINDOWS
    auto command = quoted(executable.getFullPathName());
    for (const auto& argument : arguments) command += L" " + quoted(argument);
    if (command.size() >= 32767) return { Failure::invalidRequest };
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // The bundle begins only at the first real Job acquisition. Its witness
    // outlives the inner scope containing every Job/process/thread/NUL handle,
    // attribute list and finishJob descendant handle. A policy Result is not
    // a resource terminal, and cleanup confirmation remains an independent fact.
    struct ObservedBundle final
    {
        OptionalLifecycleToken token;
        bool acquired = false;
        ~ObservedBundle() { if (acquired) token.release(); else token.failed(); }
    } observedBundle {OptionalLifecycleToken::request(currentOptionalLifecycleContext(), observedOwner,
        OptionalLifecycleResourceKind::boundedDecodeProcess, observedParentResource)};
    const auto runWithOwnedResources = [&]() -> Result
    {
#endif
    Handle job { CreateJobObjectW(nullptr, nullptr) };
    if (job.value == nullptr) return { Failure::containment, 0, GetLastError() };
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    observedBundle.token.created(); observedBundle.acquired = true;
#endif
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitsInfo {};
    limitsInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY
        | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limitsInfo.BasicLimitInformation.ActiveProcessLimit = limits.processes;
    limitsInfo.JobMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes);
    limitsInfo.ProcessMemoryLimit = static_cast<SIZE_T>(limits.memoryBytes);
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                 &limitsInfo, sizeof(limitsInfo)))
        return { Failure::containment, 0, GetLastError() };

    SECURITY_ATTRIBUTES security { sizeof(security), nullptr, TRUE };
    Handle nullFile { CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr) };
    if (nullFile.value == INVALID_HANDLE_VALUE) return { Failure::launch, 0, GetLastError() };
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        return { Failure::launch, 0, GetLastError() };
    struct AttributeOwner final
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeOwner() { DeleteProcThreadAttributeList(value); }
    } attributeOwner { attributes };
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &nullFile.value, sizeof(HANDLE), nullptr, nullptr))
        return { Failure::launch, 0, GetLastError() };
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = nullFile.value;
    startup.StartupInfo.hStdOutput = nullFile.value;
    startup.StartupInfo.hStdError = nullFile.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION created {};
    const auto started = std::chrono::steady_clock::now();
    if (!CreateProcessW(executable.getFullPathName().toWideCharPointer(), command.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, executable.getParentDirectory().getFullPathName().toWideCharPointer(),
                        &startup.StartupInfo, &created))
        return { Failure::launch, 0, GetLastError() };
    Handle process { created.hProcess }, thread { created.hThread };
    // Fail closed before the decoder executes a single instruction.
    if (!AssignProcessToJobObject(job.value, process.value))
    {
        const auto error = GetLastError();
        TerminateProcess(process.value, ERROR_CANCELLED);
        WaitForSingleObject(process.value, 2000);
        return { Failure::containment, 0, error };
    }
    Result result;
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
        result = { Failure::launch, 0, GetLastError() };
    else
    {
        for (;;)
        {
            if (cancel.load(std::memory_order_acquire)) { result.failure = Failure::cancelled; break; }
            if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(limits.wallTimeMs))
                { result.failure = Failure::deadline; break; }
            const auto size = staging.getSize();
            if (size < 0 || static_cast<uint64_t>(size) > limits.outputBytes)
                { result.failure = Failure::outputLimit; break; }
            if (inspect)
            {
                try { result.failure = inspect(); }
                catch (...) { result.failure = Failure::invalidOutput; }
                if (result.failure != Failure::none) break;
            }
            const auto wait = WaitForSingleObject(process.value, 10);
            if (wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0) { result = { Failure::launch, 0, GetLastError() }; break; }
            DWORD code = 0;
            if (!GetExitCodeProcess(process.value, &code))
                result = { Failure::launch, 0, GetLastError() };
            else if (code != 0) result = { Failure::decoderExit, code };
            break;
        }
    }
    // Terminate descendants even when the parent reports success. Staging may
    // only be accepted after this ownership boundary has quiesced.
    result.jobCleanupConfirmed = finishJob(job.value);
    const auto cleanupError = result.jobCleanupConfirmed ? DWORD{} : GetLastError();
    result.elapsedMilliseconds = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    result.observedOutputBytes = static_cast<uint64_t>(juce::jmax<juce::int64>(0, staging.getSize()));
    if (!result.jobCleanupConfirmed)
    { result.failure = Failure::cleanup; result.nativeError = cleanupError; return result; }
    if (result && static_cast<uint64_t>(staging.getSize()) > limits.outputBytes)
        result.failure = Failure::outputLimit;
    if (result && inspect)
    {
        try { result.failure = inspect(); }
        catch (...) { result.failure = Failure::invalidOutput; }
    }
    return result;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    };
    auto observedResult = runWithOwnedResources();
    observedResult.observedResourceId = observedBundle.token.resourceId();
    return observedResult;
#endif
#else
    juce::ignoreUnused(arguments, inspect);
    return { Failure::unsupportedPlatform };
#endif
}

const char* BoundedDecodeProcess::describe(Failure failure) noexcept
{
    switch (failure)
    {
        case Failure::none: return "Decoder completed within its resource policy.";
        case Failure::invalidRequest: return "Decoder request exceeds the supported resource policy.";
        case Failure::unsupportedPlatform: return "Bounded optional decoding is unavailable on this platform.";
        case Failure::launch: return "Unable to start the optional decoder.";
        case Failure::containment: return "Unable to contain the optional decoder; no unbounded fallback was started.";
        case Failure::cancelled: return "Optional decode cancelled.";
        case Failure::deadline: return "Optional decode exceeded its time limit.";
        case Failure::outputLimit: return "Optional decode exceeded its output limit.";
        case Failure::invalidOutput: return "Optional decoder output failed validation.";
        case Failure::decoderExit: return "Optional decoder exited unsuccessfully.";
        case Failure::cleanup: return "Optional decoder process cleanup could not be confirmed.";
    }
    return "Optional decode failed.";
}
}
