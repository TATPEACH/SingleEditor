#pragma once
#include "RecoveryStore.h"
#include <optional>

namespace singleeditor::recovery
{
// Explicit uncalibrated service Pilot. Shared total includes both areas and
// unknown contents. Maintenance reserve is admission space, never GC authority.
struct ServiceStorageBudget final
{
    int64_t bytes = 8LL * 1024 * 1024 * 1024;
    size_t entries = 65536;
    int64_t maintenanceBytes = 4 * 1024 * 1024;
    size_t maintenanceEntries = 32;
    int64_t journalBytes = 8LL * 1024 * 1024 * 1024;
    size_t journalEntries = 65536;
    int64_t attemptBytes = 64 * 1024 * 1024;
    size_t attemptEntries = 16384;
    int64_t freeSpaceReserve = 64 * 1024 * 1024;
    bool operator==(const ServiceStorageBudget&) const = default;
};

class RecoveryStorageLayout final
{
public:
    struct RetentionPolicy final
    {
        int days = 0;
        juce::String revision, hash;
    };
    struct UsageBudget final
    {
        size_t entries = 65536;
        int milliseconds = 3000;
    };
    struct Usage final
    {
        Status status, policyStatus;
        std::optional<ServiceStorageBudget> policy;
        int64_t bytes = 0, journalBytes = 0, attemptBytes = 0, otherBytes = 0;
        int64_t freeBytes = -1, observedAtMilliseconds = 0;
        size_t entries = 0, files = 0, skipped = 0;
        bool present = false, complete = false;
    };
    explicit RecoveryStorageLayout(juce::File root) : directory(std::move(root)) {}
    juce::File journalDirectory() const { return directory.getChildFile("journals"); }
    juce::File attemptDirectory() const { return directory.getChildFile("attempts"); }
    // Explicit worker/PostCore call only. Creates or resumes the same immutable
    // policy/bindings, preserving unknown data and conflicting configurations.
    Status prepare(const ServiceStorageBudget& = {}) const;
    // Worker-only, bounded and read-only. Missing is unconfigured, never an
    // implicit deletion permission. The persisted policy binds the physical root.
    Status readRetention(std::optional<RetentionPolicy>&) const;
    // Explicit Settings action. Compare-and-publish the reviewed policy hash;
    // changes in another instance require a fresh review rather than overwriting.
    Status configureRetention(int days, const juce::String& expectedHash, RetentionPolicy&,
                              const ServiceStorageBudget& = {}) const;
    // Explicit user-requested worker inspection. Metadata only, no quota lock,
    // reservation, creation or cleanup. This is an observation across a live
    // directory, never an atomic capacity claim or deletion authority. Partial
    // results retain counted bytes and report why the walk could not finish.
    Usage inspectUsage(UsageBudget, std::function<bool()> cancel = {}) const;
#if SINGLE_EDITOR_RECOVERY_TESTING
    // 0: durable service policy, 1/2: verified journals/attempts binding.
    std::function<void(int)> onPrepareBoundary;
#endif
private:
    juce::File directory;
};
}
