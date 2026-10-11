#include "ReproductionFiles.h"
#include "DiagnosticTestActivity.h"
#include "Core/NativeFilePath.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::diagnostics
{
ReproductionFiles::Result ReproductionFiles::capture(const std::vector<juce::File>& selected, const std::atomic<bool>& cancelled) try
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(reproduction);
    if (selected.size() > maximumFiles) return { { BundleError::budget }, {} };
    Result result;
    int64_t total = 0;
    for (const auto& file : selected)
    {
        if (cancelled.load()) return { { BundleError::cancelled }, {} };
        if (file == juce::File()) return { { BundleError::invalidInput }, {} };
#if JUCE_WINDOWS
        struct Handle final
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } input { CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr) };
        // Deny in-place writes while reading this object, but allow atomic Save
        // replacement. The resulting snapshot holds the selected old bytes and
        // releases its handle before review; it does not block replacement.
        BY_HANDLE_FILE_INFORMATION information {};
        if (input.value == INVALID_HANDLE_VALUE || GetFileType(input.value) != FILE_TYPE_DISK
            || !GetFileInformationByHandle(input.value, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return { { BundleError::ioFailure }, {} };
        const auto size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
        if (size > static_cast<uint64_t>(DiagnosticBundle::maximumEntryBytes)
            || size > static_cast<uint64_t>(maximumTotalBytes - total)) return { { BundleError::budget }, {} };
        BundleEntry entry;
        entry.id = "reproduction/file-" + juce::String(static_cast<int>(result.entries.size() + 1)) + ".bin";
        entry.kind = EntryKind::reproduction; entry.schemaMajor = 1;
        entry.bytes.setSize(static_cast<size_t>(size));
        size_t position = 0;
        while (position < size)
        {
            if (cancelled.load()) return { { BundleError::cancelled }, {} };
            const auto count = static_cast<DWORD>(std::min<uint64_t>(65536, size - position));
            DWORD read = 0;
            if (!ReadFile(input.value, static_cast<char*>(entry.bytes.getData()) + position, count, &read, nullptr) || read != count)
                return { { BundleError::ioFailure }, {} };
            position += read;
        }
        total += static_cast<int64_t>(size);
        result.entries.push_back(std::move(entry));
#else
        juce::ignoreUnused(file, total);
        return { { BundleError::ioFailure }, {} }; // This native file-ownership path is Windows-specific.
#endif
    }
    if (cancelled.load()) return { { BundleError::cancelled }, {} };
    return result;
}
catch (const std::bad_alloc&) { return { { BundleError::budget }, {} }; }
catch (...) { return { { BundleError::ioFailure }, {} }; }
}
