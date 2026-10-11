#pragma once
#include "RecoveryStore.h"

namespace singleeditor::recovery
{
// Explicit post-Core, worker-only cursor. The existing journal-root directory
// is a conservative presence hint: absence costs no enumeration or writes;
// presence does not imply that any recoverable owner exists. A stale empty root
// is harmless. No new marker is needed in the publication/GC transaction.
class RecoveryDiscovery final
{
public:
    struct Row final { juce::Uuid owner { nullptr }; OwnerSummary summary; };
    struct Page final
    {
        Status status;
        std::vector<Row> rows;
        size_t entriesRead = 0, otherEntries = 0, recordBytesRead = 0;
        bool presence = false, more = false;
    };
    struct Budget final
    {
        size_t entries = 32;
        size_t recordBytes = 1024 * 1024;
    };
    RecoveryDiscovery(juce::File journalRoot, Limits = {});
    ~RecoveryDiscovery();
    // A page consumes at most Budget::entries direct children and recordBytes
    // of head-record payload. Never recursively enumerates or reads audio.
    // Root pin/cursor survive between pages; each row releases its owner pins.
    Page nextPage(Budget, std::function<bool()> cancel = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
