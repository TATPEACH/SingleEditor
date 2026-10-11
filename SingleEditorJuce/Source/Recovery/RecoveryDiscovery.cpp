#include "RecoveryDiscovery.h"
#include "RecoveryDurableFiles.h"
#include "Core/NativeFilePath.h"
#include <algorithm>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::recovery
{
struct RecoveryDiscovery::Impl final
{
    juce::File root;
    Limits limits;
    std::unique_ptr<detail::DurableDirectory> directory;
    bool started = false, finished = false;
#if JUCE_WINDOWS
    HANDLE cursor = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW item {};
    ~Impl() { if (cursor != INVALID_HANDLE_VALUE) FindClose(cursor); }
#endif
};
RecoveryDiscovery::RecoveryDiscovery(juce::File root, Limits limits) : impl(std::make_unique<Impl>())
{ impl->root = std::move(root); impl->limits = limits; }
RecoveryDiscovery::~RecoveryDiscovery() = default;

RecoveryDiscovery::Page RecoveryDiscovery::nextPage(Budget budget, std::function<bool()> cancel) try
{
    Page page;
    if (budget.entries < 1 || budget.entries > 256 || budget.recordBytes < impl->limits.recordBytes
        || budget.recordBytes > 16 * 1024 * 1024)
    { page.status = { Error::invalidPacket, "discovery_budget" }; return page; }
    if (cancel && cancel()) { page.status = { Error::cancelled, "discovery_cancelled" }; return page; }
#if JUCE_WINDOWS
    if (!impl->started)
    {
        page.status = detail::DurableDirectory::open(impl->root, false, impl->directory);
        if (!page.status) return page;
        impl->started = true;
        if (!impl->directory) impl->finished = true;
        else
        {
            impl->cursor = FindFirstFileW(files::nativePath(impl->root.getChildFile("*")).toWideCharPointer(), &impl->item);
            if (impl->cursor == INVALID_HANDLE_VALUE)
            {
                impl->finished = true;
                if (GetLastError() != ERROR_FILE_NOT_FOUND)
                    page.status = { Error::resourceOrEnvironmentFault, "discovery_enumeration_open" };
            }
        }
    }
    page.presence = impl->directory != nullptr;
    size_t reservedRecordBytes = 0;
    while (page.status && !impl->finished && page.entriesRead < budget.entries
           && reservedRecordBytes <= budget.recordBytes - impl->limits.recordBytes)
    {
        if (cancel && cancel()) { page.status = { Error::cancelled, "discovery_cancelled" }; break; }
        const juce::String name(impl->item.cFileName);
        if (name != "." && name != "..")
        {
            ++page.entriesRead;
            const auto id = name.startsWith("terminal-") ? name.substring(9) : name;
            const juce::Uuid owner(id);
            if (!owner.isNull() && owner.toString() == id)
            {
                // Reserve the maximum even if a malformed payload fails before
                // readJson can report its successfully decoded byte count.
                reservedRecordBytes += impl->limits.recordBytes;
                auto summary = RecoveryStore(impl->root, owner, impl->limits).inspectSummary(cancel);
                page.recordBytesRead += summary.recordBytesRead;
                page.rows.push_back({ owner, std::move(summary) });
            }
            else ++page.otherEntries;
        }
        if (!FindNextFileW(impl->cursor, &impl->item))
        {
            impl->finished = true;
            if (GetLastError() != ERROR_NO_MORE_FILES)
                page.status = { Error::resourceOrEnvironmentFault, "discovery_enumeration_next" };
        }
    }
    page.more = !impl->finished;
    // A cursor is a bounded observation, not an atomic directory snapshot.
    // Repeated/changed entries across pages are harmless display metadata;
    // every explicit action independently revalidates its exact owner/head.
    return page;
#else
    juce::ignoreUnused(cancel);
    page.status = { Error::resourceOrEnvironmentFault, "durable_store_requires_windows" }; return page;
#endif
}
catch (...) { Page page; page.status = { Error::resourceOrEnvironmentFault, "discovery_exception" }; return page; }
}
