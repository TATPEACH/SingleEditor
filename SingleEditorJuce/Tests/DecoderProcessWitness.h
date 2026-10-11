#pragma once

#include <juce_core/juce_core.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

namespace singleeditor::tests
{
inline juce::int64 creationStamp(HANDLE process)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return static_cast<juce::int64>((static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

inline juce::String currentDecoderProcessWitness()
{
    const auto stamp = creationStamp(GetCurrentProcess());
    return stamp > 0 ? juce::String(static_cast<juce::int64>(GetCurrentProcessId())) + " " + juce::String(stamp) : juce::String();
}

// A PID may be reused after the owned child is reaped. Query the same open
// handle for identity and exit; never wait on (or terminate) a later process.
// Missing/malformed evidence and access/query failures remain failures.
inline bool decoderProcessExited(const juce::String& witness, DWORD timeout = 0)
{
    auto fields = juce::StringArray::fromTokens(witness, " \t", "");
    fields.removeEmptyStrings();
    if (fields.size() != 2 || !fields[0].containsOnly("0123456789") || !fields[1].containsOnly("0123456789")) return false;
    const auto pid = fields[0].getLargeIntValue(), created = fields[1].getLargeIntValue();
    if (pid <= 0 || pid > static_cast<juce::int64>(MAXDWORD) || created <= 0) return false;
    const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const auto actual = creationStamp(process);
    const auto result = actual > 0 && (actual != created || WaitForSingleObject(process, timeout) == WAIT_OBJECT_0);
    CloseHandle(process);
    return result;
}
}
#endif
