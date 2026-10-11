#include "RecoveryStore.h"
#include "RecoveryDurableFiles.h"
#include "RecoveryStorageLayout.h"
#include "Core/NativeFilePath.h"

#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <optional>
#include <set>
#if SINGLE_EDITOR_RECOVERY_TESTING
 #include <iostream>
#endif

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::recovery
{
namespace
{
constexpr size_t frameHeaderBytes = 40;
Status fail(Error error, const char* code) { return { error, code }; }
bool validLimits(const Limits& limits)
{
    return limits.recordBytes >= 4096 && limits.recordBytes <= 1024 * 1024
        && limits.assetBytes >= 4096 && limits.assetBytes <= 16 * 1024 * 1024
        && limits.readBytes >= limits.recordBytes && limits.readBytes <= maximumRecoveryAudioBytes
        && limits.journalReadBytes >= limits.recordBytes && limits.journalReadBytes <= 32 * 1024 * 1024
        && limits.records > 0 && limits.records <= 4096
        && limits.ownerBytes > 0 && limits.ownerBytes <= 8LL * 1024 * 1024 * 1024
        && limits.freeSpaceReserve >= 0
        && limits.globalBytes > 0 && limits.globalBytes <= 1024LL * 1024 * 1024 * 1024
        && limits.globalEntries > 0 && limits.globalEntries <= 1024 * 1024;
}
bool token(const juce::String& text, int length)
{
    return text.length() == length && text.containsOnly("0123456789abcdef");
}
bool uuid(const juce::var& value)
{
    return value.isString() && token(value.toString(), 32) && !juce::Uuid(value.toString()).isNull();
}
bool integer(const juce::var& value, int64_t minimum = 0)
{
    return (value.isInt() || value.isInt64()) && static_cast<int64_t>(value) >= minimum;
}
bool keys(const juce::var& value, std::initializer_list<const char*> names)
{
    auto* object = value.getDynamicObject();
    if (object == nullptr || object->getProperties().size() != static_cast<int>(names.size()))
        return false;
    for (const auto* name : names)
        if (!object->hasProperty(name)) return false;
    return true;
}
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* result = new juce::DynamicObject();
    for (const auto& [key, value] : properties) result->setProperty(key, value);
    return juce::var(result);
}

// Reject excessive nesting before calling the recursive JSON parser.
bool boundedJson(const juce::String& text)
{
    int depth = 0;
    bool quoted = false, escaped = false;
    for (auto p = text.getCharPointer(); !p.isEmpty(); ++p)
    {
        const auto c = *p;
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 24) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && !quoted;
}
juce::var sorted(const juce::var& value)
{
    if (auto* input = value.getDynamicObject())
    {
        std::vector<juce::String> names;
        for (const auto& property : input->getProperties()) names.push_back(property.name.toString());
        std::sort(names.begin(), names.end());
        auto* output = new juce::DynamicObject();
        for (const auto& name : names) output->setProperty(name, sorted(input->getProperty(name)));
        return juce::var(output);
    }
    if (auto* input = value.getArray())
    {
        juce::Array<juce::var> output;
        for (const auto& element : *input) output.add(sorted(element));
        return juce::var(output);
    }
    return value;
}
juce::String canonical(const juce::var& value) { return juce::JSON::toString(sorted(value), true); }
bool parse(const juce::String& text, juce::var& output)
{
    return boundedJson(text) && juce::JSON::parse(text, output).wasOk() && output.isObject();
}
juce::MemoryBlock bytes(const juce::String& text)
{
    return { text.toRawUTF8(), text.getNumBytesAsUTF8() };
}
juce::String digest(const juce::MemoryBlock& payload)
{
    return juce::SHA256(payload).toHexString();
}
// A synchronous view of immutable caller-owned bytes. Audio headers and PCM
// need not be concatenated merely to hash or publish the existing wire format.
class PayloadStream final : public juce::InputStream
{
public:
    PayloadStream(const juce::MemoryBlock& first, const juce::MemoryBlock* second)
        : prefix(first), body(second) {}
    juce::int64 getTotalLength() override { return static_cast<juce::int64>(prefix.getSize() + (body ? body->getSize() : 0)); }
    juce::int64 getPosition() override { return static_cast<juce::int64>(position); }
    bool isExhausted() override { return getPosition() == getTotalLength(); }
    bool setPosition(juce::int64 next) override
    {
        if (next < 0 || next > getTotalLength()) return false;
        position = static_cast<size_t>(next); return true;
    }
    int read(void* destination, int maximum) override
    {
        if (maximum <= 0) return 0;
        auto* output = static_cast<char*>(destination);
        size_t remaining = static_cast<size_t>(maximum), copied = 0;
        if (position < prefix.getSize())
        {
            const auto count = std::min(remaining, prefix.getSize() - position);
            std::memcpy(output, static_cast<const char*>(prefix.getData()) + position, count);
            position += count; copied += count; remaining -= count;
        }
        if (body && remaining > 0)
        {
            const auto offset = position - prefix.getSize();
            const auto count = std::min(remaining, body->getSize() - offset);
            if (count > 0) std::memcpy(output + copied, static_cast<const char*>(body->getData()) + offset, count);
            position += count; copied += count;
        }
        return static_cast<int>(copied);
    }
private:
    const juce::MemoryBlock& prefix;
    const juce::MemoryBlock* body;
    size_t position = 0;
};
juce::String audioDigest(const juce::MemoryBlock& header, const juce::MemoryBlock& samples)
{
    PayloadStream stream(header, &samples);
    return juce::SHA256(stream).toHexString();
}
juce::MemoryBlock frameHeader(const juce::MemoryBlock& prefix, const juce::MemoryBlock* body,
    const juce::MemoryBlock* verifiedChecksum = nullptr)
{
    PayloadStream payload(prefix, body);
    juce::MemoryOutputStream stream;
    stream.write("SER1", 4);
    stream.writeInt(static_cast<int>(payload.getTotalLength()));
    const auto checksum = verifiedChecksum ? *verifiedChecksum : juce::SHA256(payload).getRawData();
    stream.write(checksum.getData(), checksum.getSize());
    return stream.getMemoryBlock();
}

#if JUCE_WINDOWS
struct Handle final
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
using DirectoryPins = std::vector<std::unique_ptr<Handle>>;
Status pinDirectories(juce::File leaf, bool create, DirectoryPins& pins)
{
    std::vector<juce::File> chain;
    for (;;)
    {
        chain.push_back(leaf);
        const auto parent = leaf.getParentDirectory();
        if (parent == leaf) break;
        leaf = parent;
    }
    std::reverse(chain.begin(), chain.end());
    for (const auto& directory : chain)
    {
        auto handle = std::make_unique<Handle>();
        const auto open = [&]
        {
            return CreateFileW(files::nativePath(directory).toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        };
        handle->value = open();
        if (handle->value == INVALID_HANDLE_VALUE && create && GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            if (!CreateDirectoryW(files::nativePath(directory).toWideCharPointer(), nullptr)
                && GetLastError() != ERROR_ALREADY_EXISTS)
                return fail(Error::resourceOrEnvironmentFault, "directory_create");
            handle->value = open();
        }
        BY_HANDLE_FILE_INFORMATION information {};
        if (handle->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle->value, &information)
            || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return fail(Error::resourceOrEnvironmentFault, "directory_pin");
        // No FILE_SHARE_DELETE: each ancestor remains the same directory until
        // this operation ends, so a concurrent junction swap cannot redirect I/O.
        pins.push_back(std::move(handle));
    }
    return {};
}
Status openWriterLock(const juce::File& directory, Handle& lock)
{
    lock.value = CreateFileW(files::nativePath(directory.getChildFile("writer.lock")).toWideCharPointer(),
        GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock.value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "owner_busy");
    // Existing unknown data must survive refusal. Arm deletion only after the
    // actual exclusive handle proves this is an empty, unlinked regular lock.
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock.value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || info.nNumberOfLinks != 1 || info.nFileSizeHigh != 0 || info.nFileSizeLow != 0)
        return fail(Error::resourceOrEnvironmentFault, "owner_lock_invalid");
    FILE_DISPOSITION_INFO disposition { TRUE };
    if (!SetFileInformationByHandle(lock.value, FileDispositionInfo, &disposition, sizeof(disposition)))
        return fail(Error::resourceOrEnvironmentFault, "owner_lock_disposition");
    return {};
}
// Serializes mutation admission across all owners/processes of this physical
// recovery root. The persistent empty lock has no owner authority and survives
// process death; OS handle closure releases exclusion. No startup use.
Status readJson(const juce::File&, size_t, juce::var&, juce::String*, size_t*, bool acceptHeadV2 = false);
Status publish(const juce::File&, const juce::MemoryBlock&, bool);
struct QuotaHandle final
{
    QuotaHandle(HANDLE handle, bool certifiedEmptyDeletion = false) : value(handle), deletingEmptyLock(certifiedEmptyDeletion) {}
    HANDLE value;
    bool deletingEmptyLock;
};
using QuotaHandles = std::map<juce::String, QuotaHandle>;
juce::String directoryId(HANDLE);
bool validServiceBudget(const ServiceStorageBudget& policy)
{
    const auto validBytes = [] (int64_t n) { return n > 0 && n <= 1024LL * 1024 * 1024 * 1024; };
    const auto validEntries = [] (size_t n) { return n > 0 && n <= 1024 * 1024; };
    return validBytes(policy.bytes) && validBytes(policy.journalBytes) && validBytes(policy.attemptBytes)
        && validEntries(policy.entries) && validEntries(policy.journalEntries) && validEntries(policy.attemptEntries)
        && policy.maintenanceBytes >= 0 && policy.maintenanceBytes < std::min(policy.bytes, policy.journalBytes)
        && policy.maintenanceEntries < std::min(policy.entries, policy.journalEntries) && policy.freeSpaceReserve >= 0;
}
juce::var servicePolicy(const juce::String& rootId, const ServiceStorageBudget& policy)
{
    return object({ { "kind", "service_quota" }, { "schema", 1 }, { "root_id", rootId },
        { "global_bytes", policy.bytes }, { "global_entries", static_cast<int64_t>(policy.entries) },
        { "maintenance_bytes", policy.maintenanceBytes }, { "maintenance_entries", static_cast<int64_t>(policy.maintenanceEntries) },
        { "journal_bytes", policy.journalBytes }, { "journal_entries", static_cast<int64_t>(policy.journalEntries) },
        { "attempt_bytes", policy.attemptBytes }, { "attempt_entries", static_cast<int64_t>(policy.attemptEntries) },
        { "free_space_reserve", policy.freeSpaceReserve } });
}
juce::var scopePolicy(const juce::String& serviceId, const juce::String& areaId, const juce::String& area, const juce::String& policyHash)
{
    return object({ { "kind", "delegated_quota" }, { "schema", 1 }, { "service_id", serviceId },
        { "area_id", areaId }, { "area", area }, { "policy_hash", policyHash } });
}
Status pinServiceScope(const juce::File& root, const juce::String& rootId, const juce::String& policyHash,
    const juce::String& area, DirectoryPins& pins, bool allowMissing, bool& present)
{
    present = false;
    const auto path = root.getChildFile(area), policy = path.getChildFile("quota-policy");
    const auto attributes = GetFileAttributesW(files::nativePath(path).toWideCharPointer());
    if (attributes == INVALID_FILE_ATTRIBUTES && allowMissing
        && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) return {};
    auto status = pinDirectories(path, false, pins);
    if (!status) return status;
    const auto expected = scopePolicy(rootId, directoryId(pins.back()->value), area, policyHash);
    auto guard = std::make_unique<Handle>();
    guard->value = CreateFileW(files::nativePath(policy).toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (guard->value == INVALID_HANDLE_VALUE)
        return allowMissing && GetLastError() == ERROR_FILE_NOT_FOUND ? Status{}
            : fail(Error::resourceOrEnvironmentFault, "service_binding_access");
    pins.push_back(std::move(guard));
    juce::var confirmed;
    if (!(status = readJson(policy, 4096, confirmed, nullptr, nullptr))) return status;
    if (canonical(confirmed) != canonical(expected)) return fail(Error::corruptJournal, "service_binding_conflict");
    present = true; return {};
}
Status decodeServicePolicy(const juce::var& json, const juce::String& rootId, ServiceStorageBudget& out)
{
    if (!keys(json, { "kind", "schema", "root_id", "global_bytes", "global_entries", "maintenance_bytes", "maintenance_entries",
        "journal_bytes", "journal_entries", "attempt_bytes", "attempt_entries", "free_space_reserve" })
        || json["kind"] != juce::var("service_quota") || json["root_id"] != juce::var(rootId))
        return fail(Error::corruptJournal, "service_policy_identity");
    for (const auto* field : { "global_bytes", "global_entries", "maintenance_bytes", "maintenance_entries",
        "journal_bytes", "journal_entries", "attempt_bytes", "attempt_entries", "free_space_reserve" })
        if (!integer(json[field])) return fail(Error::corruptJournal, "service_policy_contract");
    out.bytes = static_cast<int64_t>(json["global_bytes"]); out.entries = static_cast<size_t>(static_cast<int64_t>(json["global_entries"]));
    out.maintenanceBytes = static_cast<int64_t>(json["maintenance_bytes"]);
    out.maintenanceEntries = static_cast<size_t>(static_cast<int64_t>(json["maintenance_entries"]));
    out.journalBytes = static_cast<int64_t>(json["journal_bytes"]); out.journalEntries = static_cast<size_t>(static_cast<int64_t>(json["journal_entries"]));
    out.attemptBytes = static_cast<int64_t>(json["attempt_bytes"]); out.attemptEntries = static_cast<size_t>(static_cast<int64_t>(json["attempt_entries"]));
    out.freeSpaceReserve = static_cast<int64_t>(json["free_space_reserve"]);
    return validServiceBudget(out) ? Status{} : fail(Error::corruptJournal, "service_policy_bounds");
}
struct RootMutationQuota final
{
    DirectoryPins pins;
    DirectoryPins inventoryPins;
    Handle lock;
    juce::File lockFile;
    juce::File policyFile;
    int64_t byteLimit = 0;
    int64_t freeReserve = 0;
    size_t entryLimit = 0;
    bool newPolicy = false;
    juce::File scopeRoot, inventoryRoot;
    juce::String serviceArea, physicalRootId, servicePolicyHash;
    juce::var proposedPolicy;
    int64_t scopeByteLimit = 0, maintenanceBytes = 0;
    size_t scopeEntryLimit = 0, maintenanceEntries = 0;

    Status acquire(const juce::File& root, const Limits& limits, const juce::String& requiredArea = "journals",
                   const ServiceStorageBudget* requestedService = nullptr)
    {
        scopeRoot = inventoryRoot = root;
        const auto ownPolicy = root.getChildFile("quota-policy");
        // A missing binding under an existing budget root is not permission to
        // establish an independent child cap (including partially prepared layouts).
        if (GetFileAttributesW(files::nativePath(ownPolicy).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES
            && GetFileAttributesW(files::nativePath(root.getParentDirectory().getChildFile("quota-policy")).toWideCharPointer()) != INVALID_FILE_ATTRIBUTES)
            return fail(Error::resourceOrEnvironmentFault, "budget_parent_requires_scope");
        auto status = pinDirectories(root, true, pins);
        if (!status) return status;
        const auto scopeId = directoryId(pins.back()->value);
        const auto pinPolicy = [&] (const juce::File& file, juce::var& json) -> Status
        {
            auto guard = std::make_unique<Handle>();
            guard->value = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (guard->value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "global_policy_busy");
            pins.push_back(std::move(guard));
            return readJson(file, 4096, json, nullptr, nullptr);
        };
        juce::var initial;
        if (GetFileAttributesW(files::nativePath(ownPolicy).toWideCharPointer()) != INVALID_FILE_ATTRIBUTES)
        {
            if (!(status = pinPolicy(ownPolicy, initial))) return status;
            if (initial["kind"] == juce::var("delegated_quota"))
            {
                if (requestedService || !keys(initial, { "kind", "schema", "service_id", "area_id", "area", "policy_hash" })
                    || initial["area_id"] != juce::var(scopeId) || initial["area"] != juce::var(root.getFileName())
                    || (root.getFileName() != "journals" && root.getFileName() != "attempts"))
                    return fail(Error::corruptJournal, "service_scope_identity");
                serviceArea = root.getFileName();
                if (serviceArea != requiredArea) return fail(Error::invalidPacket, "service_scope_role");
                inventoryRoot = root.getParentDirectory();
                if (!(status = pinDirectories(inventoryRoot, false, pins))) return status;
                if (initial["service_id"] != juce::var(directoryId(pins.back()->value)))
                    return fail(Error::corruptJournal, "service_scope_parent");
            }
        }
        if (!(status = pinDirectories(inventoryRoot, false, pins))) return status;
        physicalRootId = directoryId(pins.back()->value);
        if (!token(physicalRootId, 48)) return fail(Error::resourceOrEnvironmentFault, "global_root_identity");
        lockFile = inventoryRoot.getChildFile("quota.lock");
        const auto deadline = GetTickCount64() + 250;
        for (;;)
        {
            lock.value = CreateFileW(files::nativePath(lockFile).toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (lock.value != INVALID_HANDLE_VALUE) break;
            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION)
                return fail(Error::resourceOrEnvironmentFault, "global_lock_open");
            if (GetTickCount64() >= deadline) return fail(Error::resourceOrEnvironmentFault, "global_budget_busy");
            juce::Thread::sleep(2); // Worker-only bounded contention, never message/audio thread.
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(lock.value, &information)
            || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
            || information.nNumberOfLinks != 1 || information.nFileSizeHigh != 0 || information.nFileSizeLow != 0)
            return fail(Error::resourceOrEnvironmentFault, "global_lock_invalid");
        byteLimit = scopeByteLimit = limits.globalBytes;
        entryLimit = scopeEntryLimit = limits.globalEntries; freeReserve = limits.freeSpaceReserve;
        policyFile = inventoryRoot.getChildFile("quota-policy");
        if (GetFileAttributesW(files::nativePath(policyFile).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return fail(Error::resourceOrEnvironmentFault, "global_policy_access");
            if (serviceArea.isNotEmpty()) return fail(Error::corruptJournal, "service_policy_missing");
            newPolicy = true;
            proposedPolicy = requestedService ? servicePolicy(physicalRootId, *requestedService)
                : object({ { "kind", "root_quota" }, { "schema", 1 }, { "global_bytes", byteLimit },
                    { "global_entries", static_cast<int64_t>(entryLimit) } });
        }
        else
        {
            juce::var policy;
            status = pinPolicy(policyFile, policy);
            if (!status) return status;
            proposedPolicy = policy;
            if (policy["kind"] == juce::var("delegated_quota"))
                return fail(Error::resourceOrEnvironmentFault, "budget_scope_changed");
            if (policy["kind"] != juce::var("service_quota"))
            {
            if (requestedService || serviceArea.isNotEmpty() || !keys(policy, { "kind", "schema", "global_bytes", "global_entries" })
                || policy["kind"] != juce::var("root_quota") || !integer(policy["global_bytes"], 1)
                || !integer(policy["global_entries"], 1)
                || static_cast<int64_t>(policy["global_bytes"]) > 1024LL * 1024 * 1024 * 1024
                || static_cast<int64_t>(policy["global_entries"]) > 1024 * 1024)
                return fail(Error::corruptJournal, "global_policy_contract");
            // Another caller/process cannot silently raise the root's policy.
            // Changing a persisted policy requires an explicit service action.
            byteLimit = std::min(byteLimit, static_cast<int64_t>(policy["global_bytes"]));
            entryLimit = std::min(entryLimit, static_cast<size_t>(static_cast<int64_t>(policy["global_entries"])));
            scopeByteLimit = byteLimit; scopeEntryLimit = entryLimit;
            }
        }
        if (proposedPolicy["kind"] == juce::var("service_quota"))
        {
            if (!requestedService && serviceArea.isEmpty()) return fail(Error::invalidPacket, "service_scope_required");
            ServiceStorageBudget policy;
            if (!(status = decodeServicePolicy(proposedPolicy, physicalRootId, policy))) return status;
            if (requestedService && canonical(proposedPolicy) != canonical(servicePolicy(physicalRootId, *requestedService)))
                return fail(Error::staleHead, "service_policy_configuration");
            servicePolicyHash = digest(bytes(canonical(proposedPolicy)));
            byteLimit = policy.bytes; entryLimit = policy.entries;
            maintenanceBytes = policy.maintenanceBytes; maintenanceEntries = policy.maintenanceEntries;
            freeReserve = std::max(freeReserve, policy.freeSpaceReserve);
            scopeByteLimit = serviceArea.isEmpty() ? byteLimit : std::min(limits.globalBytes,
                serviceArea == "journals" ? policy.journalBytes : policy.attemptBytes);
            scopeEntryLimit = serviceArea.isEmpty() ? entryLimit : std::min(limits.globalEntries,
                serviceArea == "journals" ? policy.journalEntries : policy.attemptEntries);
            if (!requestedService)
                for (const auto* area : { "journals", "attempts" })
                {
                    bool present = false;
                    if (!(status = pinServiceScope(inventoryRoot, physicalRootId, servicePolicyHash, area, pins, false, present))) return status;
                }
        }
        return {};
    }

    Status account(const juce::File& root, int64_t needed, size_t neededEntries,
                   AppendWork& work, const QuotaHandles& held,
                   const juce::File& replaceable = {}, bool maintenance = false)
    {
        if (root != scopeRoot) return fail(Error::invalidPacket, "global_account_scope");
        work.globalEntriesScanned = 0; work.globalBytesObserved = 0;
        work.scopeEntriesScanned = 0; work.scopeBytesObserved = 0;
        inventoryPins.clear();
        if (newPolicy) { needed += 4096 + frameHeaderBytes; ++neededEntries; }
        const auto allowedBytes = byteLimit - (maintenance ? 0 : maintenanceBytes);
        const auto allowedEntries = entryLimit - (maintenance ? 0 : maintenanceEntries);
        const auto scopeBytes = std::max(int64_t{0}, scopeByteLimit - (!maintenance && serviceArea == "journals" ? maintenanceBytes : 0));
        const auto scopeEntries = scopeEntryLimit - std::min(scopeEntryLimit, !maintenance && serviceArea == "journals" ? maintenanceEntries : size_t{0});
        struct Pending final { juce::File directory; size_t depth; };
        std::vector<Pending> pending { { inventoryRoot, 0 } };
        while (!pending.empty())
        {
            const auto current = std::move(pending.back()); pending.pop_back();
            WIN32_FIND_DATAW entry{};
            const auto search = current.directory.getChildFile("*");
            const auto finder = FindFirstFileW(files::nativePath(search).toWideCharPointer(), &entry);
            if (finder == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                return fail(Error::resourceOrEnvironmentFault, "global_inventory_open");
            }
            struct Finder final { HANDLE value; ~Finder() { FindClose(value); } } closer { finder };
            for (;;)
            {
                const juce::String name(entry.cFileName);
                if (name != "." && name != "..")
                {
                    if (++work.globalEntriesScanned > allowedEntries)
                        return fail(Error::budgetExceeded, "global_entry_budget");
                    const auto file = current.directory.getChildFile(name);
                    const bool inScope = file.isAChildOf(scopeRoot);
                    if (inScope && ++work.scopeEntriesScanned > scopeEntries)
                        return fail(Error::budgetExceeded, "scope_entry_budget");
                    // Only the root lock is exempt (already certified empty).
                    // Other caller-owned handles are measured, never skipped.
                    if (file != lockFile)
                    {
                        auto pin = std::make_unique<Handle>();
                        const bool isDirectory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        // Attribute-only access does not establish the Windows
                        // data-sharing fence. Request read access (without
                        // reading payloads) to exclude concurrent file writers.
                        const auto existing = held.find(file.getFullPathName());
                        if (existing == held.end())
                            pin->value = CreateFileW(files::nativePath(file).toWideCharPointer(),
                                isDirectory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
                                FILE_SHARE_READ | (isDirectory ? FILE_SHARE_WRITE : FILE_SHARE_DELETE), nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                        // Collector WRITE/DELETE locks and DELETE directory
                        // guards exclude reopening. They already pin these exact
                        // paths; querying the real handles preserves that fence.
                        const auto handle = existing == held.end() ? pin->value : existing->second.value;
                        BY_HANDLE_FILE_INFORMATION info{};
                        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)
                            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
                            || ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != isDirectory)
                            return fail(Error::resourceOrEnvironmentFault, "global_inventory_entry");
                        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                            if (current.depth >= 8) return fail(Error::budgetExceeded, "global_depth_budget");
                            // The directory handle excludes rename for the
                            // entire child enumeration and append transaction.
                            pending.push_back({ file, current.depth + 1 });
                        }
                        else
                        {
                            // Windows reports zero links after the certified
                            // empty writer lock is marked delete-pending. This
                            // exception comes only from openWriterLock's actual
                            // exclusive handle, never from a filename or Health.
                            const bool deletingLock = existing != held.end() && existing->second.deletingEmptyLock
                                && info.nNumberOfLinks == 0 && info.nFileSizeHigh == 0 && info.nFileSizeLow == 0;
                            if ((info.nNumberOfLinks != 1 && !deletingLock) || GetFileType(handle) != FILE_TYPE_DISK)
                                return fail(Error::resourceOrEnvironmentFault, "global_inventory_file");
                            const auto size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                            if (size > static_cast<uint64_t>(allowedBytes - work.globalBytesObserved))
                                return fail(Error::budgetExceeded, "global_byte_budget");
                            work.globalBytesObserved += static_cast<int64_t>(size);
                            if (inScope)
                            {
                                if (size > static_cast<uint64_t>(scopeBytes - work.scopeBytesObserved))
                                    return fail(Error::budgetExceeded, "scope_byte_budget");
                                work.scopeBytesObserved += static_cast<int64_t>(size);
                            }
                        }
                        // The current head is the one mutable replacement in
                        // this owner-locked transaction. Keeping an attribute
                        // handle open prevents its write-through atomic rename
                        // on Windows. Count it, but leave replacement ownership
                        // to writer.lock and the existing publication protocol.
                        // The reserved 8192-byte metadata margin covers old/new
                        // head overlap; all immutable files remain guarded.
                        if (file != replaceable && existing == held.end()) inventoryPins.push_back(std::move(pin));
                    }
                }
                if (FindNextFileW(finder, &entry)) continue;
                if (GetLastError() != ERROR_NO_MORE_FILES) return fail(Error::resourceOrEnvironmentFault, "global_inventory_next");
                break;
            }
        }
        if (needed > allowedBytes || work.globalBytesObserved > allowedBytes - needed)
            return byteCapacityFailure("global_byte_budget", ByteCapacity::Resource::globalStorage,
                static_cast<uint64_t>(work.globalBytesObserved) + needed, allowedBytes);
        if (neededEntries > allowedEntries || work.globalEntriesScanned > allowedEntries - neededEntries)
            return fail(Error::budgetExceeded, "global_entry_budget");
        if (needed > scopeBytes || work.scopeBytesObserved > scopeBytes - needed)
            return byteCapacityFailure("scope_byte_budget", ByteCapacity::Resource::scopeStorage,
                static_cast<uint64_t>(work.scopeBytesObserved) + needed, scopeBytes);
        if (neededEntries > scopeEntries || work.scopeEntriesScanned > scopeEntries - neededEntries)
            return fail(Error::budgetExceeded, "scope_entry_budget");
        const auto free = files::nativeFile(inventoryRoot).getBytesFreeOnVolume();
        if (free < 0) return fail(Error::budgetExceeded, "free_space_reserve");
        if (needed > free || freeReserve > free - needed)
            return byteCapacityFailure("free_space_reserve", ByteCapacity::Resource::freeDisk,
                static_cast<uint64_t>(needed) + freeReserve, free);
        if (newPolicy)
        {
            const auto expectedPolicy = proposedPolicy;
            auto status = publish(policyFile, bytes(canonical(expectedPolicy)), false);
            if (!status) return status;
            auto guard = std::make_unique<Handle>();
            guard->value = CreateFileW(files::nativePath(policyFile).toWideCharPointer(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (guard->value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "global_policy_busy");
            juce::var confirmed;
            if (!(status = readJson(policyFile, 4096, confirmed, nullptr, nullptr))) return status;
            if (canonical(confirmed) != canonical(expectedPolicy)) return fail(Error::staleHead, "global_policy_changed");
            pins.push_back(std::move(guard));
            newPolicy = false;
        }
        return {};
    }

    Status admitControl(const juce::File& root, const juce::MemoryBlock& payload, const QuotaHandles& held)
    {
        AppendWork work;
        // Account the actual framed payload and staging entry. Keep inventory
        // stable through publication; the caller releases these pins before
        // collector rename/delete. Root/policy locks still outlive the mutation.
        return account(root, static_cast<int64_t>(payload.getSize() + frameHeaderBytes), 2, work, held, {}, true);
    }

    void releaseInventory() { inventoryPins.clear(); }
};
bool linkedPath(juce::File path)
{
    for (;;)
    {
        const auto attributes = GetFileAttributesW(files::nativePath(path).toWideCharPointer());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return true;
        const auto parent = path.getParentDirectory();
        if (parent == path) break;
        path = parent;
    }
    return false;
}

Status readFrameHandle(HANDLE handle, size_t maximum, juce::MemoryBlock& payload, size_t* payloadReadBytes = nullptr)
{
    BY_HANDLE_FILE_INFORMATION information {};
    LARGE_INTEGER length {};
    if (!GetFileInformationByHandle(handle, &information)
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
        || !GetFileSizeEx(handle, &length) || length.QuadPart < static_cast<int64_t>(frameHeaderBytes)
        || static_cast<uint64_t>(length.QuadPart) > maximum + frameHeaderBytes)
        return fail(Error::corruptJournal, "frame_bounds");
    juce::MemoryBlock input(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    const auto readOk = ReadFile(handle, input.getData(), static_cast<DWORD>(input.getSize()), &read, nullptr);
    if (payloadReadBytes) *payloadReadBytes += read > frameHeaderBytes ? read - frameHeaderBytes : 0;
    if (!readOk || read != input.getSize()) return fail(Error::resourceOrEnvironmentFault, "short_read");
    const auto* data = static_cast<const char*>(input.getData());
    if (std::memcmp(data, "SER1", 4) != 0) return fail(Error::unsupportedSchema, "frame_version");
    const auto size = juce::ByteOrder::littleEndianInt(data + 4);
    if (size != input.getSize() - frameHeaderBytes) return fail(Error::corruptJournal, "frame_length");
    juce::MemoryBlock candidate(data + frameHeaderBytes, size);
    const auto checksum = juce::SHA256(candidate).getRawData();
    if (std::memcmp(data + 8, checksum.getData(), 32) != 0) return fail(Error::corruptJournal, "frame_hash");
    payload = std::move(candidate);
    return {};
}

Status readFrame(const juce::File& file, size_t maximum, juce::MemoryBlock& payload, size_t* consumed = nullptr)
{
    if (linkedPath(file)) return fail(Error::corruptJournal, "linked_path");
    Handle handle;
    handle.value = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        return fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                        ? Error::corruptJournal : Error::resourceOrEnvironmentFault, "read_open");
    }
    return readFrameHandle(handle.value, maximum, payload, consumed);
}

// Staging is in the same owner directory/volume. Flush every immutable object
// before publishing its name; replace the head only after all dependencies.
Status publishParts(const juce::File& target, const juce::MemoryBlock& prefix, const juce::MemoryBlock* body, bool replace,
    const juce::MemoryBlock* verifiedChecksum = nullptr)
{
    if (verifiedChecksum && verifiedChecksum->getSize() != 32) return fail(Error::invalidPacket, "publication_checksum");
    if (linkedPath(target)) return fail(Error::resourceOrEnvironmentFault, "linked_path");
    const auto staging = target.getSiblingFile("staging-" + juce::Uuid().toString());
    const auto header = frameHeader(prefix, body, verifiedChecksum);
    {
        Handle handle;
        handle.value = CreateFileW(files::nativePath(staging).toWideCharPointer(), GENERIC_WRITE, 0,
                                  nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr);
        if (handle.value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "stage_open");
        for (const auto* part : std::array<const juce::MemoryBlock*, 3> { &header, &prefix, body })
        {
            if (!part || part->getSize() == 0) continue;
            DWORD written = 0;
            if (!WriteFile(handle.value, part->getData(), static_cast<DWORD>(part->getSize()), &written, nullptr)
                || written != part->getSize()) return fail(Error::resourceOrEnvironmentFault, "stage_write_or_flush");
        }
        if (!FlushFileBuffers(handle.value)) return fail(Error::resourceOrEnvironmentFault, "stage_write_or_flush");
    }
    const auto flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(files::nativePath(staging).toWideCharPointer(), files::nativePath(target).toWideCharPointer(), flags))
    {
#if SINGLE_EDITOR_RECOVERY_TESTING
        std::cerr << "Recovery publication rename failed: win32=" << GetLastError()
            << " target=" << target.getFullPathName() << '\n';
#endif
        return fail(Error::resourceOrEnvironmentFault, "stage_rename");
    }
    return {};
}
Status publish(const juce::File& target, const juce::MemoryBlock& payload, bool replace)
{
    return publishParts(target, payload, nullptr, replace);
}
#else
bool linkedPath(const juce::File&) { return false; }
Status readFrame(const juce::File&, size_t, juce::MemoryBlock&, size_t* = nullptr)
{
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
}
Status publish(const juce::File&, const juce::MemoryBlock&, bool)
{
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
}
#endif

Status readJson(const juce::File& file, size_t maximum, juce::var& json, juce::String* hash = nullptr,
                size_t* consumed = nullptr, bool acceptHeadV2)
{
    juce::MemoryBlock payload;
    auto result = readFrame(file, maximum, payload);
    if (!result) return result;
    const auto text = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize()));
    if (!parse(text, json) || canonical(json) != text || bytes(text) != payload)
        return fail(Error::corruptJournal, "noncanonical_json");
    if (hash != nullptr) *hash = digest(payload);
    if (consumed != nullptr) *consumed = payload.getSize();
    if (!integer(json["schema"], 1) || (static_cast<int64_t>(json["schema"]) != 1
        && !(acceptHeadV2 && json["schema"] == juce::var(2) && json["kind"] == juce::var("head"))))
        return fail(Error::unsupportedSchema, "payload_schema");
    return {};
}

Status decodeHead(const juce::File& file, const juce::Uuid& owner, SafeHead& output, juce::String* cut = nullptr)
{
    juce::var head;
    const auto status = readJson(file, 4096, head, nullptr, nullptr, true);
    if (!status) return status;
    const bool compacted = head["schema"] == juce::var(2);
    if (compacted && (!keys(head, { "kind", "schema", "owner", "sequence", "hash", "checkpoint_cut" })
        || !head["checkpoint_cut"].isString() || !token(head["checkpoint_cut"].toString(), 64)))
        return fail(Error::unsupportedSchema, "head_cut_contract");
    if ((!compacted && !keys(head, { "kind", "schema", "owner", "sequence", "hash" }))
        || head["kind"] != juce::var("head") || head["owner"] != juce::var(owner.toString())
        || !integer(head["sequence"], 1) || !head["hash"].isString() || !token(head["hash"].toString(), 64))
        return fail(Error::corruptJournal, "head_contract");
    output = { static_cast<int64_t>(head["sequence"]), head["hash"].toString() };
    if (cut) *cut = compacted ? head["checkpoint_cut"].toString() : juce::String();
    return {};
}

juce::MemoryBlock headPayload(juce::Uuid owner, const SafeHead& head, const juce::String& cut)
{
    auto value = object({ { "kind", "head" }, { "schema", cut.isEmpty() ? 1 : 2 }, { "owner", owner.toString() },
                         { "sequence", head.sequence }, { "hash", head.hash } });
    if (cut.isNotEmpty()) value.getDynamicObject()->setProperty("checkpoint_cut", cut);
    return bytes(canonical(value));
}

Status decodeCheckpointCut(const juce::var& value, juce::Uuid owner, const juce::String& name,
    const juce::String& rootId, const juce::String& ownerId, ManifestView& view)
{
    if (value["schema"] != juce::var(1) || !keys(value, { "kind", "schema", "owner", "root_id", "owner_id", "floor_sequence",
            "floor_hash", "records", "cuts" }) || value["kind"] != juce::var("checkpoint-cut")
        || value["owner"] != juce::var(owner.toString()) || value["root_id"] != juce::var(rootId)
        || value["owner_id"] != juce::var(ownerId) || !integer(value["floor_sequence"], 1)
        || !value["floor_hash"].isString() || !token(value["floor_hash"].toString(), 64)
        || !value["records"].isArray() || !value["cuts"].isArray()
        || value["records"].size() + value["cuts"].size() > 4096)
        return fail(Error::corruptJournal, "checkpoint_cut_contract");
    view.checkpointCut = name;
    view.checkpointFloor = { static_cast<int64_t>(value["floor_sequence"]), value["floor_hash"].toString() };
    std::set<juce::String> seen { name, view.checkpointFloor.hash };
    for (const auto& record : *value["records"].getArray())
    {
        if (!keys(record, { "sequence", "hash" }) || !integer(record["sequence"], 1)
            || static_cast<int64_t>(record["sequence"]) >= view.checkpointFloor.sequence
            || !record["hash"].isString() || !token(record["hash"].toString(), 64)
            || !seen.insert(record["hash"].toString()).second)
            return fail(Error::corruptJournal, "checkpoint_cut_record");
        view.supersededRecords.push_back({ static_cast<int64_t>(record["sequence"]), record["hash"].toString() });
    }
    for (const auto& cut : *value["cuts"].getArray())
    {
        if (!cut.isString() || !token(cut.toString(), 64) || !seen.insert(cut.toString()).second)
            return fail(Error::corruptJournal, "checkpoint_cut_ancestor");
        view.supersededCuts.push_back(cut.toString());
    }
    return {};
}

Status readCheckpointCut(const juce::File& directory, juce::Uuid owner, const juce::String& name,
    const juce::String& rootId, const juce::String& ownerId, size_t maximum, ManifestView& view, size_t& consumed)
{
    juce::var value; juce::String actual;
    auto status = readJson(directory.getChildFile("commits").getChildFile(name), maximum, value, &actual, &consumed);
    if (!status) return status;
    if (actual != name) return fail(Error::corruptJournal, "checkpoint_cut_hash");
    return decodeCheckpointCut(value, owner, name, rootId, ownerId, view);
}

// A process may die after publishing a cut but before replacing head. Only the
// unused control file is collectible: its claims never grant deletion of audio
// or records. Every reference must already be known to the current manifest.
bool unpublishedCut(const ManifestView& cut, const ManifestView& manifest)
{
    if (cut.checkpointCut == manifest.checkpointCut
        || std::none_of(manifest.commits.begin(), manifest.commits.end(), [&](const auto& commit)
            { return commit.head == cut.checkpointFloor && commit.record["operation"] == juce::var("recovery.checkpoint"); }))
        return false;
    for (const auto& old : cut.supersededRecords)
        if (std::none_of(manifest.commits.begin(), manifest.commits.end(), [&](const auto& commit) { return commit.head == old; })
            && std::find(manifest.supersededRecords.begin(), manifest.supersededRecords.end(), old) == manifest.supersededRecords.end())
            return false;
    for (const auto& old : cut.supersededCuts)
        if (old != manifest.checkpointCut
            && std::find(manifest.supersededCuts.begin(), manifest.supersededCuts.end(), old) == manifest.supersededCuts.end())
            return false;
    return true;
}

Status retirementEvidence(const juce::var& value)
{
    if (!keys(value, { "reason", "document", "target" })) return fail(Error::corruptJournal, "retirement_evidence");
    const auto document = value["document"];
    if (!keys(document, { "version", "lineage", "runtime_instance", "generation" })
        || !integer(document["version"], 1) || static_cast<int64_t>(document["version"]) != 1
        || !uuid(document["lineage"]) || !uuid(document["runtime_instance"]) || !integer(document["generation"], 1))
        return fail(Error::corruptJournal, "retirement_document");
    if (value["reason"] == juce::var("discarded"))
        return value["target"].isVoid() ? Status{} : fail(Error::corruptJournal, "retirement_discard_target");
    if (value["reason"] == juce::var("expired"))
    {
        const auto expiry = value["target"];
        const bool manual = keys(expiry, { "retention_ms", "reviewed_at", "last_activity" });
        const bool configured = keys(expiry, { "retention_ms", "reviewed_at", "last_activity", "policy_hash" })
            && token(expiry["policy_hash"].toString(), 64);
        if ((!manual && !configured)
            || !integer(expiry["retention_ms"], 1) || !integer(expiry["reviewed_at"], 1)
            || !integer(expiry["last_activity"], 1)
            || static_cast<int64_t>(expiry["last_activity"]) > static_cast<int64_t>(expiry["reviewed_at"])
            || static_cast<int64_t>(expiry["retention_ms"]) > static_cast<int64_t>(expiry["reviewed_at"])
                                                           - static_cast<int64_t>(expiry["last_activity"]))
            return fail(Error::corruptJournal, "retirement_expiry_target");
        return {};
    }
    if (value["reason"] != juce::var("saved")) return fail(Error::unsupportedSchema, "retirement_reason");
    const auto target = value["target"];
    if (!keys(target, { "path", "volume", "file_id", "size", "modified", "changed" })
        || !target["path"].isString() || target["path"].toString().length() > 4096
        || !juce::File::isAbsolutePath(target["path"].toString()) || !target["volume"].isString()
        || !token(target["volume"].toString(), 16) || !target["file_id"].isString()
        || !token(target["file_id"].toString(), 32) || target["file_id"].toString() == juce::String::repeatedString("0", 32)
        || !integer(target["size"], 1)
        || !integer(target["modified"], 1) || !integer(target["changed"], 1))
        return fail(Error::corruptJournal, "retirement_saved_target");
    return {};
}

Status readRetirementFile(const juce::File& directory, const juce::Uuid& owner, std::optional<OwnerRetirement>& output)
{
    const auto file = directory.getChildFile("retired");
#if JUCE_WINDOWS
    if (GetFileAttributesW(files::nativePath(file).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { output.reset(); return {}; }
        return fail(Error::resourceOrEnvironmentFault, "retirement_access");
    }
#else
    if (!file.exists()) { output.reset(); return {}; }
#endif
    juce::var record;
    auto status = readJson(file, 16384, record);
    if (!status) return status;
    if (!keys(record, { "kind", "schema", "owner", "sequence", "hash", "nonce", "evidence" })
        || record["kind"] != juce::var("retirement") || record["owner"] != juce::var(owner.toString())
        || !integer(record["sequence"], 1) || !token(record["hash"].toString(), 64) || !uuid(record["nonce"]))
        return fail(Error::corruptJournal, "retirement_contract");
    status = retirementEvidence(record["evidence"]);
    if (!status) return status;
    output = OwnerRetirement { { static_cast<int64_t>(record["sequence"]), record["hash"].toString() },
        record["nonce"].toString(), record["evidence"] };
    return {};
}

#if JUCE_WINDOWS
enum class Usage { reader, writer, compaction, retirement };
Status openUsageLock(const juce::File& directory, Usage use, Handle& handle)
{
    const bool exclusive = use == Usage::compaction || use == Usage::retirement;
    handle.value = CreateFileW(files::nativePath(directory.getChildFile("usage.lease")).toWideCharPointer(),
        GENERIC_READ | (exclusive ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle.value == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        return fail(Error::resourceOrEnvironmentFault,
            error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? "owner_in_use" : "owner_usage_access");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(handle.value, &info) || !GetFileSizeEx(handle.value, &size)
        || size.QuadPart != 0 || info.nNumberOfLinks != 1
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        return fail(Error::corruptJournal, "owner_usage_contract");
    // Byte 0 protects recovered readers from compaction. Byte 1 keeps a live
    // writer protected while idle, without blocking its own compaction. A
    // retirement takes both; handle closure (including process death) releases
    // the locks. Old exclusive-share operations conservatively refuse overlap.
    OVERLAPPED range{};
    range.Offset = use == Usage::writer ? 1 : 0;
    if (!LockFileEx(handle.value, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0),
                    0, use == Usage::retirement ? 2 : 1, 0, &range))
        return fail(Error::resourceOrEnvironmentFault, "owner_in_use");
    return {};
}
#endif

Status encodeAudioHeader(const AudioBlock& block, size_t maximum, juce::MemoryBlock& prefix, bool readSamples = true)
{
    if (!std::isfinite(block.sampleRate) || block.sampleRate < 1 || block.sampleRate > 768000
        || block.channels < 1 || block.channels > 64 || block.frames < 1
        || block.frames > static_cast<int64_t>(maximum / (4 * static_cast<size_t>(block.channels)))
        || (readSamples && block.samples.getSize() != static_cast<size_t>(block.frames) * static_cast<size_t>(block.channels) * 4))
        return fail(Error::invalidPacket, "audio_bounds");
    const auto* data = static_cast<const char*>(block.samples.getData());
    for (size_t offset = 0; readSamples && offset < block.samples.getSize(); offset += 4)
        if ((juce::ByteOrder::littleEndianInt(data + offset) & 0x7f800000u) == 0x7f800000u)
            return fail(Error::invalidPacket, "nonfinite_sample");
    const auto header = bytes(canonical(object({ { "kind", "audio" }, { "schema", 1 },
        { "format", "interleaved-f32le" }, { "rate", block.sampleRate }, { "channels", block.channels },
        { "frames", block.frames } })));
    juce::MemoryOutputStream stream;
    stream.writeInt(static_cast<int>(header.getSize()));
    stream.write(header.getData(), header.getSize());
    const auto sampleBytes = static_cast<size_t>(block.frames) * static_cast<size_t>(block.channels) * 4;
    if (stream.getDataSize() + sampleBytes > maximum) return fail(Error::budgetExceeded, "asset_budget");
    prefix = stream.getMemoryBlock();
    return {};
}

Status decodeAudio(const juce::MemoryBlock& payload, size_t maximum, AudioBlock& output)
{
    if (payload.getSize() < 4) return fail(Error::missingOrCorruptAsset, "audio_header");
    const auto* data = static_cast<const char*>(payload.getData());
    const auto length = juce::ByteOrder::littleEndianInt(data);
    if (length > 1024 || length > payload.getSize() - 4) return fail(Error::missingOrCorruptAsset, "audio_header_bounds");
    juce::var header;
    if (!parse(juce::String::fromUTF8(data + 4, static_cast<int>(length)), header)
        || !keys(header, { "kind", "schema", "format", "rate", "channels", "frames" })
        || header["kind"] != juce::var("audio") || header["schema"] != juce::var(1)
        || header["format"] != juce::var("interleaved-f32le")
        || !(header["rate"].isDouble() || header["rate"].isInt() || header["rate"].isInt64())
        || !integer(header["channels"], 1) || !integer(header["frames"], 1))
        return fail(Error::missingOrCorruptAsset, "audio_schema");
    AudioBlock candidate { static_cast<double>(header["rate"]), static_cast<int>(header["channels"]),
                           static_cast<int64_t>(header["frames"]), { data + 4 + length, payload.getSize() - 4 - length } };
    juce::MemoryBlock encodedHeader;
    if (!encodeAudioHeader(candidate, maximum, encodedHeader) || encodedHeader.getSize() != 4 + length
        || std::memcmp(encodedHeader.getData(), payload.getData(), encodedHeader.getSize()) != 0)
        return fail(Error::missingOrCorruptAsset, "audio_format_or_samples");
    output = std::move(candidate);
    return {};
}

bool validDocument(const juce::var& document)
{
    return keys(document, { "version", "lineage", "runtime_instance", "generation" })
        && integer(document["version"], 1) && static_cast<int64_t>(document["version"]) == 1
        && uuid(document["lineage"]) && uuid(document["runtime_instance"]) && integer(document["generation"], 1);
}
bool validSource(const juce::var& source)
{
    return keys(source, { "binding", "path", "size", "modified", "fast_stamp_valid" })
        && uuid(source["binding"]) && source["path"].isString() && source["path"].toString().length() <= 32768
        && integer(source["size"], -1) && integer(source["modified"])
        && source["fast_stamp_valid"].isBool();
}
Status recordSchema(const juce::var& record, const juce::String& owner, int64_t sequence)
{
    if (!keys(record, { "kind", "schema", "owner", "sequence", "parent", "document", "source", "operation",
                         "plan_schema", "plan", "plan_hash", "state", "state_hash", "timestamp", "assets" })
        && !(keys(record, { "kind", "schema", "owner", "sequence", "parent", "document", "source", "operation",
                            "plan_schema", "plan", "plan_hash", "state", "state_hash", "timestamp", "assets", "activity_protocol" })
             && keys(record["activity_protocol"], { "version", "latest" })
             && record["activity_protocol"]["version"] == juce::var(1)
             && integer(record["activity_protocol"]["latest"])
             && static_cast<int64_t>(record["activity_protocol"]["latest"]) >= static_cast<int64_t>(record["timestamp"])))
        return fail(Error::unsupportedSchema, "record_fields");
    const auto* assets = record["assets"].getArray();
    if (record["kind"] != juce::var("commit") || record["owner"] != juce::var(owner)
        || !integer(record["sequence"], 1) || static_cast<int64_t>(record["sequence"]) != sequence
        || !record["parent"].isString() || (sequence == 1 ? record["parent"].toString().isNotEmpty()
                                                          : !token(record["parent"].toString(), 64))
        || !validDocument(record["document"]) || !validSource(record["source"])
        || !record["operation"].isString() || record["operation"].toString().isEmpty()
        || record["operation"].toString().length() > 256 || !integer(record["plan_schema"], 1)
        || !record["plan"].isString() || !record["state"].isString() || !integer(record["timestamp"])
        || assets == nullptr || static_cast<size_t>(assets->size()) > maximumCommitAssets)
        return fail(Error::corruptJournal, "record_contract");
    juce::var plan, state;
    if (!record["plan_hash"].isString() || !record["state_hash"].isString()
        || record["plan_hash"].toString() != digest(bytes(record["plan"].toString()))
        || record["state_hash"].toString() != digest(bytes(record["state"].toString()))
        || !parse(record["plan"].toString(), plan) || !parse(record["state"].toString(), state))
        return fail(Error::corruptJournal, "plan_or_state_bytes");
    std::set<juce::String> seen;
    for (const auto& asset : *assets)
        if (!asset.isString() || !token(asset.toString(), 64) || !seen.insert(asset.toString()).second)
            return fail(Error::corruptJournal, "asset_reference");
    return {};
}

#if JUCE_WINDOWS
struct AssetVersion final
{
    uint64_t volume = 0;
    std::array<unsigned char, 16> fileId {};
    int64_t size = 0, modified = 0, changed = 0;
    bool operator==(const AssetVersion&) const = default;
};
std::optional<AssetVersion> assetVersion(HANDLE handle, int64_t minimumSize = static_cast<int64_t>(frameHeaderBytes))
{
    FILE_ID_INFO id {};
    FILE_BASIC_INFO basic {};
    LARGE_INTEGER size {};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))
        || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileSizeEx(handle, &size) || size.QuadPart < minimumSize
        || basic.LastWriteTime.QuadPart <= 0 || basic.ChangeTime.QuadPart <= 0)
        return {};
    AssetVersion result;
    result.volume = id.VolumeSerialNumber;
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.fileId.begin());
    if (std::all_of(result.fileId.begin(), result.fileId.end(), [] (auto byte) { return byte == 0; })) return {};
    result.size = size.QuadPart;
    result.modified = basic.LastWriteTime.QuadPart;
    result.changed = basic.ChangeTime.QuadPart;
    return result;
}
struct AssetProof final
{
    std::optional<AssetVersion> version;
    size_t payloadBytes = 0;
};
using AssetProofs = std::map<juce::String, AssetProof>;

// One append only. Existing and newly published CAS dependencies stay read-only
// and non-deletable until the head publication returns. Unknown/changed physical
// versions undergo the same full frame/hash/format/sample checks as cold reads.
class AppendAssets final
{
public:
    AppendAssets(juce::File directory, const AssetProofs* previous, AppendWork& diagnostics)
        : assets(std::move(directory)), prior(previous), work(diagnostics) {}

    Status verify(const juce::String& id, size_t maximum)
    {
        if (const auto found = proofs.find(id); found != proofs.end())
            return found->second.payloadBytes <= maximum ? Status{} : fail(Error::budgetExceeded, "read_budget");
        if (proofs.size() >= 4096) return fail(Error::budgetExceeded, "asset_lease_budget");
        const auto file = assets.getChildFile(id);
        auto lease = std::make_unique<Handle>();
        lease->value = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (lease->value == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            return fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                            ? Error::missingOrCorruptAsset : Error::resourceOrEnvironmentFault, "asset_lease_open");
        }
        BY_HANDLE_FILE_INFORMATION info {};
        if (!GetFileInformationByHandle(lease->value, &info)
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return fail(Error::missingOrCorruptAsset, "asset_lease_type");
        AssetProof proof { assetVersion(lease->value), 0 };
        const auto known = prior ? prior->find(id) : AssetProofs::const_iterator{};
        if (prior && known != prior->end() && proof.version && known->second.version == proof.version)
        {
            proof.payloadBytes = known->second.payloadBytes;
            if (proof.payloadBytes > maximum) return fail(Error::budgetExceeded, "read_budget");
            ++work.reusedAudioFiles;
        }
        else
        {
            juce::MemoryBlock payload;
            const auto status = readFrameHandle(lease->value, maximum, payload, &work.audioPayloadBytesRead);
            ++work.hashedAudioFiles;
            if (!status || digest(payload) != id) return fail(Error::missingOrCorruptAsset, "asset_hash_or_read");
            AudioBlock audio;
            if (!decodeAudio(payload, maximum, audio)) return fail(Error::missingOrCorruptAsset, "asset_format_or_samples");
            proof.payloadBytes = payload.getSize();
        }
        proofs.emplace(id, std::move(proof));
        leases.emplace(id, std::move(lease));
        return {};
    }
    Status recheck() const
    {
        for (const auto& [id, proof] : proofs)
            if (proof.version && assetVersion(leases.at(id)->value) != proof.version)
                return fail(Error::missingOrCorruptAsset, "asset_lease_changed");
        return {};
    }
    AssetProofs proofs;
private:
    juce::File assets;
    const AssetProofs* prior;
    AppendWork& work;
    std::map<juce::String, std::unique_ptr<Handle>> leases;
};

bool stagingName(const juce::String& name)
{ return name.startsWith("staging-") && token(name.substring(8), 32) && !juce::Uuid(name.substring(8)).isNull(); }
bool quarantineName(const juce::String& name)
{
    return (name.startsWith("plan-") || name.startsWith("item-"))
        && token(name.substring(5), 32) && !juce::Uuid(name.substring(5)).isNull();
}
struct CollectionItem final
{
    juce::String area, name, nonce, contentHash;
    SafeHead base;
    AssetVersion version;
    bool checkpointPrune = false;
};
juce::var collectionJson(const CollectionItem& item, juce::Uuid owner)
{
    return object({ { "kind", item.checkpointPrune ? "checkpoint-prune" : "orphan-collection" }, { "schema", 1 }, { "owner", owner.toString() },
        { "area", item.area }, { "name", item.name }, { "nonce", item.nonce }, { "content", item.contentHash },
        { "sequence", item.base.sequence }, { "head", item.base.hash },
        { "volume", juce::String::toHexString(item.version.volume).paddedLeft('0', 16) },
        { "file_id", juce::String::toHexString(item.version.fileId.data(), static_cast<int>(item.version.fileId.size()), 0) },
        { "size", item.version.size }, { "modified", item.version.modified }, { "changed", item.version.changed } });
}
Status parseCollection(const juce::var& value, juce::Uuid owner, const juce::String& nonce, CollectionItem& item, bool terminal = false)
{
    item.checkpointPrune = !terminal && value["kind"] == juce::var("checkpoint-prune");
    if (!keys(value, { "kind", "schema", "owner", "area", "name", "nonce", "content", "sequence", "head",
                         "volume", "file_id", "size", "modified", "changed" })
        || value["kind"] != juce::var(terminal ? "terminal-item" : item.checkpointPrune ? "checkpoint-prune" : "orphan-collection") || !integer(value["schema"], 1)
        || static_cast<int64_t>(value["schema"]) != 1)
        return fail(Error::unsupportedSchema, "collection_schema");
    if (value["owner"] != juce::var(owner.toString()) || value["nonce"] != juce::var(nonce)
        || !token(nonce, 32) || juce::Uuid(nonce).isNull() || !value["name"].isString() || !value["area"].isString()
        || !value["content"].isString() || !token(value["content"].toString(), 64)
        || !value["volume"].isString() || !token(value["volume"].toString(), 16)
        || !value["file_id"].isString() || !token(value["file_id"].toString(), 32)
        || !integer(value["size"]) || !integer(value["modified"], 1) || !integer(value["changed"], 1)
        || !integer(value["sequence"]) || !value["head"].isString())
        return fail(Error::corruptJournal, "collection_contract");
    item.area = value["area"].toString(); item.name = value["name"].toString(); item.nonce = nonce;
    if (item.checkpointPrune && (item.area != "commits" || !token(item.name, 64)))
        return fail(Error::corruptJournal, "checkpoint_prune_path");
    const bool regular = (item.area == "root" || item.area == "assets" || item.area == "commits")
        && (stagingName(item.name) || (item.area == "assets" && token(item.name, 64))
            || (item.checkpointPrune && item.area == "commits" && token(item.name, 64))
            || (terminal && ((item.area == "commits" && token(item.name, 64))
                || (item.area == "root" && (item.name == "head" || item.name == "retired" || item.name == "usage.lease")))));
    if (!regular && !(terminal && item.area == "quarantine" && quarantineName(item.name)))
        return fail(Error::corruptJournal, "collection_path");
    item.base = { static_cast<int64_t>(value["sequence"]), value["head"].toString() };
    if (item.base.sequence == 0 ? item.base.hash.isNotEmpty() : !token(item.base.hash, 64))
        return fail(Error::corruptJournal, "collection_head");
    item.contentHash = value["content"].toString();
    item.version.volume = static_cast<uint64_t>(value["volume"].toString().getHexValue64());
    for (size_t i = 0; i < item.version.fileId.size(); ++i)
        item.version.fileId[i] = static_cast<unsigned char>(value["file_id"].toString().substring(static_cast<int>(i * 2), static_cast<int>(i * 2 + 2)).getHexValue32());
    item.version.size = static_cast<int64_t>(value["size"]);
    item.version.modified = static_cast<int64_t>(value["modified"]);
    item.version.changed = static_cast<int64_t>(value["changed"]);
    return {};
}
Status openCollectionFile(const juce::File& file, std::unique_ptr<Handle>& output)
{
    auto handle = std::make_unique<Handle>();
    handle->value = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle->value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "collection_file_open");
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(handle->value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || info.nNumberOfLinks != 1)
        return fail(Error::resourceOrEnvironmentFault, "collection_file_identity_or_shared");
    output = std::move(handle);
    return {};
}
Status collectionHash(HANDLE handle, size_t maximum, juce::String& output, size_t& consumed)
{
    LARGE_INTEGER length {}, beginning {};
    if (!GetFileSizeEx(handle, &length) || length.QuadPart < 0 || static_cast<uint64_t>(length.QuadPart) > maximum)
        return fail(Error::budgetExceeded, "collection_byte_budget");
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) return fail(Error::resourceOrEnvironmentFault, "collection_seek");
    juce::MemoryBlock data(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    if (data.getSize() != 0 && (!ReadFile(handle, data.getData(), static_cast<DWORD>(data.getSize()), &read, nullptr)
        || read != data.getSize())) return fail(Error::resourceOrEnvironmentFault, "collection_read");
    consumed += read;
    output = digest(data);
    return {};
}
Status renameCollectionFile(HANDLE handle, const juce::File& destination)
{
    const auto path = files::nativePath(destination);
    const auto pathBytes = std::wcslen(path.toWideCharPointer()) * sizeof(wchar_t);
    juce::MemoryBlock data(sizeof(FILE_RENAME_INFO) + pathBytes, true);
    auto* info = static_cast<FILE_RENAME_INFO*>(data.getData());
    info->ReplaceIfExists = FALSE;
    info->FileNameLength = static_cast<DWORD>(pathBytes);
    std::memcpy(info->FileName, path.toWideCharPointer(), pathBytes);
    // Rename the already guarded object, never close it and reopen an unchecked
    // path. The durable plan precedes this operation; failures retain the object.
    if (!SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(data.getSize()))
        || !FlushFileBuffers(handle)) return fail(Error::resourceOrEnvironmentFault, "collection_quarantine");
    return {};
}
Status removeCollectionFile(std::unique_ptr<Handle>& handle)
{
    FILE_DISPOSITION_INFO disposition { TRUE };
    if (!SetFileInformationByHandle(handle->value, FileDispositionInfo, &disposition, sizeof(disposition)))
        return fail(Error::resourceOrEnvironmentFault, "collection_delete");
    handle.reset();
    return {};
}

Status terminalIntentFence(const juce::File& root, const juce::Uuid& owner)
{
    const auto path = root.getChildFile("terminal-" + owner.toString());
    if (GetFileAttributesW(files::nativePath(path).toWideCharPointer()) != INVALID_FILE_ATTRIBUTES)
        return fail(Error::staleHead, "owner_collecting");
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status{}
        : fail(Error::resourceOrEnvironmentFault, "terminal_intent_access");
}

juce::String directoryId(HANDLE handle)
{
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) return {};
    return juce::String::toHexString(info.VolumeSerialNumber).paddedLeft('0', 16)
        + juce::String::toHexString(info.FileId.Identifier, 16, 0);
}

Status openDeletingDirectory(const juce::File& path, std::unique_ptr<Handle>& output)
{
    auto handle = std::make_unique<Handle>();
    // Existing live-owner pins deny DELETE. This DELETE handle, in turn, denies
    // every new normal pin because normal pins do not share DELETE. It outlives
    // writer.lock/usage.lease, so final lock-file deletion never reopens admission.
    handle->value = CreateFileW(files::nativePath(path).toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle->value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "terminal_directory_busy");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle->value, &info)
        || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || directoryId(handle->value).length() != 48)
        return fail(Error::resourceOrEnvironmentFault, "terminal_directory_identity");
    output = std::move(handle);
    return {};
}

Status terminalNames(const juce::File& path, std::vector<juce::String>& names, size_t maximum = 4096)
{
    struct Find final { HANDLE value = INVALID_HANDLE_VALUE; ~Find() { if (value != INVALID_HANDLE_VALUE) FindClose(value); } } search;
    WIN32_FIND_DATAW item{};
    search.value = FindFirstFileW(files::nativePath(path.getChildFile("*")).toWideCharPointer(), &item);
    if (search.value == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? Status{} : fail(Error::resourceOrEnvironmentFault, "terminal_enumeration");
    do
    {
        const juce::String name(item.cFileName);
        if (name == "." || name == "..") continue;
        if (names.size() >= maximum) return fail(Error::budgetExceeded, "terminal_entry_budget");
        names.push_back(name);
    } while (FindNextFileW(search.value, &item));
    if (GetLastError() != ERROR_NO_MORE_FILES) return fail(Error::resourceOrEnvironmentFault, "terminal_enumeration");
    std::sort(names.begin(), names.end());
    return {};
}
#endif
} // namespace

struct RecoveryStore::AppendCache final
{
    SafeHead head;
#if JUCE_WINDOWS
    AssetProofs assets;
#endif
};

namespace detail
{
struct DurableDirectory::Impl final
{
    juce::File directory;
#if JUCE_WINDOWS
    DirectoryPins pins;
#endif
};
DurableDirectory::DurableDirectory() : impl(std::make_unique<Impl>()) {}
DurableDirectory::~DurableDirectory() = default;
const juce::File& DurableDirectory::path() const noexcept { return impl->directory; }
Status DurableDirectory::open(const juce::File& directory, bool create, std::unique_ptr<DurableDirectory>& output) try
{
    if (directory == juce::File() || directory.getFullPathName().length() > 4096 || linkedPath(directory))
        return fail(Error::resourceOrEnvironmentFault, "metadata_directory_path");
#if JUCE_WINDOWS
    if (!create && GetFileAttributesW(files::nativePath(directory).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { output.reset(); return {}; }
        return fail(Error::resourceOrEnvironmentFault, "metadata_directory_access");
    }
    auto value = std::unique_ptr<DurableDirectory>(new DurableDirectory());
    value->impl->directory = directory;
    const auto status = pinDirectories(directory, create, value->impl->pins);
    if (!status) return status;
    output = std::move(value);
    return {};
#else
    juce::ignoreUnused(create, output);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_directory_exception"); }

bool metadataChild(const juce::String& child)
{
    return child.isNotEmpty() && child.length() <= 96 && child != "." && child != ".."
        && child.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789.-");
}
Status DurableDirectory::readJson(const juce::String& child, size_t maximum, juce::var& output) const try
{
    if (!metadataChild(child) || maximum < 1 || maximum > 1024 * 1024) return fail(Error::invalidPacket, "metadata_read_request");
    juce::var candidate;
    const auto status = recovery::readJson(impl->directory.getChildFile(child), maximum, candidate);
    if (status) output = std::move(candidate);
    return status;
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_read_exception"); }

Status DurableDirectory::publishJson(const juce::String& child, size_t maximum, const juce::var& value, bool replace) const try
{
    if (!metadataChild(child) || maximum < 1 || maximum > 1024 * 1024) return fail(Error::invalidPacket, "metadata_publish_request");
    const auto payload = bytes(canonical(value));
    if (payload.getSize() > maximum) return fail(Error::budgetExceeded, "metadata_payload_budget");
    return publish(impl->directory.getChildFile(child), payload, replace);
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_publish_exception"); }

struct DurableMutation::Impl final
{
    juce::File root;
#if JUCE_WINDOWS
    RootMutationQuota quota;
#endif
};
DurableMutation::DurableMutation() : impl(std::make_unique<Impl>()) {}
DurableMutation::~DurableMutation() = default;
Status DurableMutation::open(const juce::File& root, const Limits& limits, std::unique_ptr<DurableMutation>& output) try
{
    if (root == juce::File() || root.getFullPathName().length() > 4096 || !validLimits(limits))
        return fail(Error::invalidPacket, "metadata_budget_request");
#if JUCE_WINDOWS
    auto value = std::unique_ptr<DurableMutation>(new DurableMutation());
    value->impl->root = root;
    const auto status = value->impl->quota.acquire(root, limits, "attempts");
    if (!status) return status;
    output = std::move(value); return {};
#else
    juce::ignoreUnused(output);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_budget_exception"); }

Status DurableMutation::reserve(size_t byteCount, size_t entries) try
{
    if (byteCount > 1024 * 1024 || entries == 0 || entries > 16)
        return fail(Error::invalidPacket, "metadata_reservation_request");
#if JUCE_WINDOWS
    AppendWork work;
    return impl->quota.account(impl->root, static_cast<int64_t>(byteCount), entries, work, {});
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_reservation_exception"); }

Status DurableMutation::publishJson(const DurableDirectory& directory, const juce::String& child,
    size_t maximum, const juce::var& value, bool replace) try
{
    if (!metadataChild(child) || maximum < 1 || maximum > 1024 * 1024
        || !directory.path().isAChildOf(impl->root)) return fail(Error::invalidPacket, "metadata_scoped_publish_request");
    const auto payload = bytes(canonical(value));
    if (payload.getSize() > maximum) return fail(Error::budgetExceeded, "metadata_payload_budget");
#if JUCE_WINDOWS
    const auto target = directory.path().getChildFile(child);
    AppendWork work;
    const auto status = impl->quota.account(impl->root, static_cast<int64_t>(payload.getSize() + frameHeaderBytes),
        2, work, {}, replace ? target : juce::File());
    if (!status) return status;
    return publish(target, payload, replace);
#else
    juce::ignoreUnused(replace);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_scoped_publish_exception"); }

Status DurableDirectory::listNames(size_t maximumEntries, std::vector<juce::String>& output) const try
{
    if (maximumEntries < 1 || maximumEntries > 4096) return fail(Error::invalidPacket, "metadata_entry_budget");
#if JUCE_WINDOWS
    struct FindHandle final { HANDLE value = INVALID_HANDLE_VALUE; ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); } } handle;
    WIN32_FIND_DATAW item{};
    handle.value = FindFirstFileW(files::nativePath(impl->directory.getChildFile("*")).toWideCharPointer(), &item);
    if (handle.value == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) { output.clear(); return {}; }
        return fail(Error::resourceOrEnvironmentFault, "metadata_enumeration_open");
    }
    std::vector<juce::String> names;
    for (;;)
    {
        const juce::String name(item.cFileName);
        if (name != "." && name != "..")
        {
            if (names.size() >= maximumEntries) return fail(Error::budgetExceeded, "metadata_entry_budget");
            names.push_back(name);
        }
        if (!FindNextFileW(handle.value, &item))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES) return fail(Error::resourceOrEnvironmentFault, "metadata_enumeration_next");
            break;
        }
    }
    output = std::move(names);
    return {};
#else
    juce::ignoreUnused(output);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "metadata_enumeration_exception"); }
} // namespace detail

Status RecoveryStorageLayout::prepare(const ServiceStorageBudget& budget) const try
{
    if (directory == juce::File() || directory.getFullPathName().length() > 4096)
        return fail(Error::invalidPacket, "service_layout_request");
#if JUCE_WINDOWS
    if (!validServiceBudget(budget)) return fail(Error::invalidPacket, "service_layout_request");
    Limits limits; limits.globalBytes = budget.bytes; limits.globalEntries = budget.entries;
    limits.freeSpaceReserve = budget.freeSpaceReserve;
    RootMutationQuota quota;
    auto status = quota.acquire(directory, limits, {}, &budget);
    if (!status) return status;
    DirectoryPins pins;
    bool complete = !quota.newPolicy;
    for (const auto* area : { "journals", "attempts" })
    {
        bool present = false;
        if (!(status = pinServiceScope(directory, quota.physicalRootId, quota.servicePolicyHash, area, pins, true, present))) return status;
        complete = complete && present;
    }
    if (complete)
    {
        // Readiness is not a capacity claim. An already complete layout adds
        // no data and must remain usable to schedule GC when growth is blocked.
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onPrepareBoundary) { onPrepareBoundary(0); onPrepareBoundary(1); onPrepareBoundary(2); }
#endif
        return {};
    }
    AppendWork work;
    // Covers both missing scope directories, framed bindings and transient
    // stages before any are created. Existing data is counted and preserved.
    if (!(status = quota.account(directory, 2 * (4096 + frameHeaderBytes), 8, work, {}, {}, true))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onPrepareBoundary) onPrepareBoundary(0);
#endif
#if SINGLE_EDITOR_RECOVERY_TESTING
    int boundary = 0;
#endif
    for (const auto* area : { "journals", "attempts" })
    {
        const auto path = directory.getChildFile(area), policy = path.getChildFile("quota-policy");
        if (!(status = pinDirectories(path, true, pins))) return status;
        const auto expected = scopePolicy(quota.physicalRootId, directoryId(pins.back()->value), area, quota.servicePolicyHash);
        const auto attributes = GetFileAttributesW(files::nativePath(policy).toWideCharPointer());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return fail(Error::resourceOrEnvironmentFault, "service_binding_access");
            if (!(status = publish(policy, bytes(canonical(expected)), false))) return status;
        }
        auto guard = std::make_unique<Handle>();
        guard->value = CreateFileW(files::nativePath(policy).toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (guard->value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "service_binding_busy");
        pins.push_back(std::move(guard));
        juce::var confirmed;
        if (!(status = readJson(policy, 4096, confirmed, nullptr, nullptr))) return status;
        if (canonical(confirmed) != canonical(expected)) return fail(Error::corruptJournal, "service_binding_conflict");
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onPrepareBoundary) onPrepareBoundary(++boundary);
#endif
    }
    return {};
#else
    juce::ignoreUnused(budget);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "service_layout_exception"); }

Status RecoveryStorageLayout::readRetention(std::optional<RetentionPolicy>& output) const try
{
    if (directory == juce::File()) return fail(Error::invalidPacket, "retention_root");
#if JUCE_WINDOWS
    if (GetFileAttributesW(files::nativePath(directory).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { output.reset(); return {}; }
        return fail(Error::resourceOrEnvironmentFault, "retention_root_access");
    }
    DirectoryPins pins;
    auto status = pinDirectories(directory, false, pins);
    if (!status) return status;
    const auto file = directory.getChildFile("retention-policy");
    if (GetFileAttributesW(files::nativePath(file).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return fail(Error::resourceOrEnvironmentFault, "retention_policy_access");
        output.reset(); return {};
    }
    juce::var record; juce::String hash;
    if (!(status = readJson(file, 4096, record, &hash))) return status;
    if (!keys(record, { "kind", "schema", "root", "revision", "days", "configured_at" })
        || record["kind"] != juce::var("recovery-retention") || !uuid(record["revision"])
        || !integer(record["days"], 1) || static_cast<int64_t>(record["days"]) > 99999
        || !integer(record["configured_at"], 1)) return fail(Error::unsupportedSchema, "retention_policy_contract");
    if (record["root"].toString() != directoryId(pins.back()->value))
        return fail(Error::staleHead, "retention_policy_root_changed");
    output = RetentionPolicy { static_cast<int>(record["days"]), record["revision"].toString(), hash };
    return {};
#else
    juce::ignoreUnused(output);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "retention_policy_read_exception"); }

Status RecoveryStorageLayout::configureRetention(int days, const juce::String& expectedHash,
    RetentionPolicy& output, const ServiceStorageBudget& budget) const try
{
    if (directory == juce::File() || days < 1 || days > 99999
        || (expectedHash.isNotEmpty() && !token(expectedHash, 64)))
        return fail(Error::invalidPacket, "retention_configuration");
#if JUCE_WINDOWS
    // Explicit configuration may create this private service layout. Constructors
    // and ordinary file open never call this path.
    if (!validServiceBudget(budget)) return fail(Error::invalidPacket, "retention_configuration");
    auto status = prepare(budget);
    if (!status) return status;
    Limits limits; limits.globalBytes = budget.bytes; limits.globalEntries = budget.entries;
    limits.freeSpaceReserve = budget.freeSpaceReserve;
    RootMutationQuota quota;
    if (!(status = quota.acquire(directory, limits, {}, &budget))) return status;
    std::optional<RetentionPolicy> current;
    if (!(status = readRetention(current))) return status;
    if ((current ? current->hash : juce::String()) != expectedHash)
        return fail(Error::staleHead, "retention_policy_changed");
    const auto record = object({ { "kind", "recovery-retention" }, { "schema", 1 },
        { "root", quota.physicalRootId }, { "revision", juce::Uuid().toString() }, { "days", days },
        { "configured_at", juce::Time::currentTimeMillis() } });
    const auto payload = bytes(canonical(record));
    const auto file = directory.getChildFile("retention-policy");
    AppendWork work;
    if (!(status = quota.account(directory, static_cast<int64_t>(payload.getSize() + frameHeaderBytes),
                                  2, work, {}, file, true))) return status;
    if (!(status = publish(file, payload, current.has_value()))) return status;
    std::optional<RetentionPolicy> confirmed;
    if (!(status = readRetention(confirmed))) return status;
    if (!confirmed || confirmed->hash != digest(payload)) return fail(Error::staleHead, "retention_policy_confirmation");
    output = *confirmed;
    return {};
#else
    juce::ignoreUnused(output, budget);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "retention_policy_write_exception"); }

RecoveryStorageLayout::Usage RecoveryStorageLayout::inspectUsage(UsageBudget budget, std::function<bool()> cancel) const
{
    Usage result;
    result.observedAtMilliseconds = juce::Time::currentTimeMillis();
    if (directory == juce::File() || directory.getFullPathName().length() > 4096
        || budget.entries == 0 || budget.entries > 1024 * 1024 || budget.milliseconds < 1 || budget.milliseconds > 30000)
    { result.status = fail(Error::invalidPacket, "storage_usage_request"); return result; }
#if JUCE_WINDOWS
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget.milliseconds);
    const auto stopped = [&]
    {
        if (cancel && cancel()) result.status = fail(Error::cancelled, "storage_usage_cancelled");
        else if (std::chrono::steady_clock::now() >= deadline) result.status = fail(Error::budgetExceeded, "storage_usage_time_budget");
        return !result.status;
    };
    try
    {
        if (stopped()) return result;
        auto volumePath = directory;
        for (int depth = 0; depth < 64; ++depth)
        {
            const auto attributes = GetFileAttributesW(files::nativePath(volumePath).toWideCharPointer());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            { result.freeBytes = files::nativeFile(volumePath).getBytesFreeOnVolume(); break; }
            const auto parent = volumePath.getParentDirectory();
            if (parent == volumePath || stopped()) break;
            volumePath = parent;
        }
        if (stopped()) return result;
        const auto attributes = GetFileAttributesW(files::nativePath(directory).toWideCharPointer());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) result.complete = true;
            else result.status = fail(Error::resourceOrEnvironmentFault, "storage_usage_root_access");
            return result;
        }
        result.present = true;
        DirectoryPins rootPins;
        if (!(result.status = pinDirectories(directory, false, rootPins))) return result;
        const auto policyFile = directory.getChildFile("quota-policy");
        if (GetFileAttributesW(files::nativePath(policyFile).toWideCharPointer()) != INVALID_FILE_ATTRIBUTES)
        {
            juce::var json;
            result.policyStatus = readJson(policyFile, 4096, json, nullptr, nullptr);
            if (result.policyStatus && json["schema"] != juce::var(1))
                result.policyStatus = fail(Error::unsupportedSchema, "storage_usage_policy_schema");
            ServiceStorageBudget persisted;
            if (result.policyStatus) result.policyStatus = decodeServicePolicy(json, directoryId(rootPins.back()->value), persisted);
            if (result.policyStatus) result.policy = persisted;
        }
        else if (GetLastError() != ERROR_FILE_NOT_FOUND)
            result.policyStatus = fail(Error::resourceOrEnvironmentFault, "storage_usage_policy_access");
        struct Pending final { juce::File directory; size_t depth; int area; };
        std::vector<Pending> pending { { directory, 0, 0 } };
        while (!pending.empty())
        {
            if (stopped()) return result;
            auto current = std::move(pending.back()); pending.pop_back();
            DirectoryPins pins;
            if (!(result.status = pinDirectories(current.directory, false, pins))) return result;
            WIN32_FIND_DATAW entry {};
            const auto finder = FindFirstFileW(files::nativePath(current.directory.getChildFile("*")).toWideCharPointer(), &entry);
            if (finder == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                result.status = fail(Error::resourceOrEnvironmentFault, "storage_usage_enumeration"); return result;
            }
            struct CloseFind final { HANDLE value; ~CloseFind() { FindClose(value); } } closer { finder };
            for (;;)
            {
                if (stopped()) return result;
                const juce::String name(entry.cFileName);
                if (name != "." && name != "..")
                {
                    if (result.entries >= budget.entries)
                    { result.status = fail(Error::budgetExceeded, "storage_usage_entry_budget"); return result; }
                    ++result.entries;
                    const int area = current.depth == 0 ? (name == "journals" ? 1 : name == "attempts" ? 2 : 0) : current.area;
                    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) ++result.skipped;
                    else if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        if (current.depth >= 8) ++result.skipped;
                        else pending.push_back({ current.directory.getChildFile(name), current.depth + 1, area });
                    }
                    else
                    {
                        const auto size = (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
                        if (size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - result.bytes))
                        { result.status = fail(Error::budgetExceeded, "storage_usage_byte_range"); return result; }
                        ++result.files; result.bytes += static_cast<int64_t>(size);
                        (area == 1 ? result.journalBytes : area == 2 ? result.attemptBytes : result.otherBytes) += static_cast<int64_t>(size);
                    }
                }
                if (FindNextFileW(finder, &entry)) continue;
                if (GetLastError() != ERROR_NO_MORE_FILES)
                { result.status = fail(Error::resourceOrEnvironmentFault, "storage_usage_enumeration_next"); return result; }
                break;
            }
        }
        result.complete = result.skipped == 0;
        if (!result.complete) result.status = fail(Error::budgetExceeded, "storage_usage_unmeasured_entries");
    }
    catch (...) { result.status = fail(Error::resourceOrEnvironmentFault, "storage_usage_exception"); }
#else
    juce::ignoreUnused(cancel);
    result.status = fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
    return result;
}

RecoveryStore::RecoveryStore(juce::File directory, juce::Uuid ownerId, Limits bounds)
    : root(std::move(directory)), owner(ownerId), limits(bounds)
{
}

struct RecoveryOwnerLease::Impl final
{
    juce::File root;
    juce::Uuid owner { nullptr };
    bool writerActivity = false;
#if JUCE_WINDOWS
    DirectoryPins pins;
    Handle usage;
#endif
};
RecoveryOwnerLease::RecoveryOwnerLease() : impl(std::make_unique<Impl>()) {}
RecoveryOwnerLease::~RecoveryOwnerLease() = default;
bool RecoveryOwnerLease::belongsTo(const juce::File& root, const juce::Uuid& owner) const noexcept
{ return impl->root == root && impl->owner == owner; }

Status RecoveryStore::acquireOwnerLease(std::shared_ptr<RecoveryOwnerLease>& output) const try
{
    if (output || root == juce::File() || owner.isNull() || !validLimits(limits))
        return fail(Error::invalidPacket, "owner_lease_request");
#if JUCE_WINDOWS
    if (auto fence = terminalIntentFence(root, owner); !fence) return fence;
    auto candidate = std::shared_ptr<RecoveryOwnerLease>(new RecoveryOwnerLease());
    candidate->impl->root = root; candidate->impl->owner = owner;
    const auto directory = root.getChildFile(owner.toString());
    auto status = pinDirectories(directory, false, candidate->impl->pins);
    if (status) status = openUsageLock(directory, Usage::reader, candidate->impl->usage);
    std::optional<OwnerRetirement> retirement;
    if (status) status = readRetirementFile(directory, owner, retirement);
    if (!status) return status;
    if (retirement) return fail(Error::staleHead, "owner_retired");
    output = std::move(candidate);
    return {};
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "owner_lease_exception"); }

Status RecoveryStore::inspectRetirement(std::optional<OwnerRetirement>& output) const try
{
    if (root == juce::File() || owner.isNull() || !validLimits(limits)) return fail(Error::invalidPacket, "retirement_request");
#if JUCE_WINDOWS
    if (auto fence = terminalIntentFence(root, owner); !fence) return fence;
#endif
    const auto directory = root.getChildFile(owner.toString());
    std::unique_ptr<detail::DurableDirectory> pinned;
    const auto opened = detail::DurableDirectory::open(directory, false, pinned);
    if (!opened) return opened;
    if (!pinned) { output.reset(); return {}; }
    std::optional<OwnerRetirement> candidate;
    auto status = readRetirementFile(directory, owner, candidate);
    if (status && candidate)
    {
        SafeHead head;
        status = decodeHead(directory.getChildFile("head"), owner, head);
        if (status && head != candidate->head) status = fail(Error::corruptJournal, "retirement_head_changed");
    }
    if (status) output = std::move(candidate);
    return status;
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "retirement_inspect_exception"); }

Status RecoveryStore::reviewDiscard(std::unique_ptr<OwnerDiscardReview>& output, std::function<bool()> cancel) const try
{
    if (output || root == juce::File() || owner.isNull() || !validLimits(limits))
        return fail(Error::invalidPacket, "discard_review_request");
#if JUCE_WINDOWS
    const auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) return fail(Error::cancelled, "discard_review_cancelled");
    const auto directory = root.getChildFile(owner.toString());
    DirectoryPins pins;
    auto status = pinDirectories(directory, false, pins);
    if (!status) return status;
    const auto view = inspectManifest(cancel);
    if (!view.status) return view.status;
    if (view.head.sequence < 1 || view.commits.empty() || view.retirement)
        return fail(Error::staleHead, "discard_review_not_open_chain");
    auto review = std::unique_ptr<OwnerDiscardReview>(new OwnerDiscardReview());
    review->root = root; review->owner = owner; review->head = view.head;
    review->rootId = directoryId(pins[pins.size() - 2]->value);
    review->directoryId = directoryId(pins.back()->value);
    if (review->rootId.isEmpty() || review->directoryId.isEmpty())
        return fail(Error::resourceOrEnvironmentFault, "discard_review_directory_identity");
    review->source = view.commits.back().record["source"]["path"].toString();
    review->timestamp = static_cast<int64_t>(view.commits.back().record["timestamp"]);
    for (const auto& commit : view.commits)
        review->latestActivity = std::max(review->latestActivity, std::max(static_cast<int64_t>(commit.record["timestamp"]),
            static_cast<int64_t>(commit.record["activity_protocol"]["latest"])));
    const auto maximum = std::min<size_t>(4096, limits.globalEntries);
    std::vector<juce::File> pending { directory };
    for (size_t next = 0; next < pending.size(); ++next)
    {
        if (cancelled()) return fail(Error::cancelled, "discard_review_cancelled");
        std::vector<juce::String> names;
        if (!(status = terminalNames(pending[next], names, maximum - review->entries))) return status;
        review->entries += names.size();
        for (const auto& name : names)
        {
            if (cancelled()) return fail(Error::cancelled, "discard_review_cancelled");
            const auto file = pending[next].getChildFile(name);
            Handle handle;
            handle.value = CreateFileW(files::nativePath(file).toWideCharPointer(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION information{};
            if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value, &information)
                || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return fail(Error::resourceOrEnvironmentFault, "discard_review_entry");
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                if (next != 0 || (name != "assets" && name != "commits" && name != "quarantine"))
                    return fail(Error::unsupportedSchema, "discard_review_directory_layout");
                if (!(status = pinDirectories(file, false, pins))) return status;
                pending.push_back(file);
                continue;
            }
            const auto bytes = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
            if (bytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - review->bytes))
                return fail(Error::budgetExceeded, "discard_review_size_overflow");
            review->bytes += static_cast<int64_t>(bytes);
        }
    }
    if (cancelled()) return fail(Error::cancelled, "discard_review_cancelled");
    if (!(status = checkHead(view.head))) return status;
    output = std::move(review);
    return {};
#else
    juce::ignoreUnused(cancel);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "discard_review_exception"); }

Status RecoveryStore::reviewExpiry(int64_t retentionMilliseconds, std::unique_ptr<OwnerDiscardReview>& output,
                                  std::function<bool()> cancel) const
{
    if (output || retentionMilliseconds <= 0) return fail(Error::invalidPacket, "expiry_review_request");
    std::unique_ptr<OwnerDiscardReview> candidate;
    const auto status = reviewDiscard(candidate, cancel);
    if (!status) return status;
    const auto view = inspectManifest(cancel);
    if (!view.status) return view.status;
    if (view.head != candidate->head || view.commits.empty()) return fail(Error::staleHead, "expiry_review_head_changed");
    // Older applications did not retain an idle writer lease. Their journals
    // remain restorable and explicitly discardable; age alone cannot establish
    // inactivity. Old readers reject this required field before appending.
    if (view.commits.back().record["activity_protocol"]["version"] != juce::var(1))
        return fail(Error::unsupportedSchema, "expiry_activity_unproven");
    const auto now = juce::Time::currentTimeMillis();
    if (candidate->latestActivity <= 0 || candidate->latestActivity > now
        || retentionMilliseconds > now - candidate->latestActivity)
        return fail(Error::staleHead, "owner_not_expired");
    candidate->retention = retentionMilliseconds; candidate->reviewedAt = now;
    output = std::move(candidate);
    return {};
}

Status RecoveryStore::reviewConfiguredExpiry(std::unique_ptr<OwnerDiscardReview>& output, std::function<bool()> cancel) const
{
    if (output || root.getFileName() != "journals") return fail(Error::invalidPacket, "retention_review_scope");
    std::optional<RecoveryStorageLayout::RetentionPolicy> policy;
    auto status = RecoveryStorageLayout(root.getParentDirectory()).readRetention(policy);
    if (!status) return status;
    if (!policy) return fail(Error::staleHead, "retention_policy_missing");
    std::unique_ptr<OwnerDiscardReview> review;
    status = reviewExpiry(int64_t(policy->days) * 86400000, review, std::move(cancel));
    if (!status) return status;
    review->retentionPolicyHash = policy->hash;
    output = std::move(review);
    return {};
}

Status RecoveryStore::publishRetirement(const SafeHead& expected, const juce::String& nonce,
    const juce::var& evidence, OwnerRetirement& output,
    const juce::String& expectedRootId, const juce::String& expectedDirectoryId) try
{
    if (root == juce::File() || owner.isNull() || !validLimits(limits) || expected.sequence < 1
        || !token(expected.hash, 64) || !uuid(juce::var(nonce)) || !retirementEvidence(evidence))
        return fail(Error::invalidPacket, "retirement_publish_request");
#if JUCE_WINDOWS
    const auto directory = root.getChildFile(owner.toString());
    RootMutationQuota globalQuota;
    auto status = globalQuota.acquire(root, limits);
    if (!status) return status;
    DirectoryPins pins;
    status = pinDirectories(directory, false, pins);
    if (!status) return status;
    if ((!expectedRootId.isEmpty() && (pins.size() < 2 || directoryId(pins[pins.size() - 2]->value) != expectedRootId))
        || (!expectedDirectoryId.isEmpty() && directoryId(pins.back()->value) != expectedDirectoryId))
        return fail(Error::staleHead, "retirement_review_directory_changed");
    Handle writer, usage, head;
    if (!(status = openWriterLock(directory, writer))) return status;
    status = openUsageLock(directory, Usage::retirement, usage);
    if (!status) return status;
    head.value = CreateFileW(files::nativePath(directory.getChildFile("head")).toWideCharPointer(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (head.value == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "retirement_head_busy");
    const auto view = inspectManifest();
    if (!view.status) return view.status;
    if (view.head != expected || view.commits.empty()) return fail(Error::staleHead, "retirement_stale_head");
    if (evidence["reason"] == juce::var("expired"))
    {
        int64_t latest = 0;
        for (const auto& commit : view.commits)
            latest = std::max(latest, std::max(static_cast<int64_t>(commit.record["timestamp"]),
                static_cast<int64_t>(commit.record["activity_protocol"]["latest"])));
        const auto expiry = evidence["target"];
        if (!expiry["policy_hash"].isVoid())
        {
            // The root quota transaction also serializes policy publication in
            // other instances. A stale shorter policy cannot retire this owner.
            std::optional<RecoveryStorageLayout::RetentionPolicy> policy;
            if (root.getFileName() != "journals") return fail(Error::invalidPacket, "retention_review_scope");
            if (!(status = RecoveryStorageLayout(root.getParentDirectory()).readRetention(policy))) return status;
            if (!policy || policy->hash != expiry["policy_hash"].toString()
                || int64_t(policy->days) * 86400000 != static_cast<int64_t>(expiry["retention_ms"]))
                return fail(Error::staleHead, "retention_policy_changed");
        }
        const auto now = juce::Time::currentTimeMillis();
        if (view.commits.back().record["activity_protocol"]["version"] != juce::var(1)
            || latest != static_cast<int64_t>(expiry["last_activity"])
            || now < static_cast<int64_t>(expiry["reviewed_at"]))
            return fail(Error::staleHead, "retirement_expiry_changed");
    }
    const auto document = view.commits.back().record["document"], saved = evidence["document"];
    if (document["lineage"] != saved["lineage"] || document["runtime_instance"] != saved["runtime_instance"]
        || document["generation"] != saved["generation"])
        return fail(Error::staleHead, "retirement_document_version");
    if (view.retirement)
    {
        if (canonical(view.retirement->evidence) != canonical(evidence)) return fail(Error::staleHead, "retirement_already_terminal");
        output = *view.retirement;
        return {};
    }
    const auto record = object({ { "kind", "retirement" }, { "schema", 1 }, { "owner", owner.toString() },
        { "sequence", expected.sequence }, { "hash", expected.hash }, { "nonce", nonce }, { "evidence", evidence } });
    const auto payload = bytes(canonical(record));
    if (payload.getSize() > 16384) return fail(Error::budgetExceeded, "retirement_marker_budget");
    if (!(status = globalQuota.admitControl(root, payload, {
        { directory.getChildFile("writer.lock").getFullPathName(), { writer.value, true } },
        { directory.getChildFile("usage.lease").getFullPathName(), usage.value } }))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onRetirementBoundary) onRetirementBoundary(RetirementBoundary::beforePublish);
#endif
    status = publish(directory.getChildFile("retired"), payload, false);
    if (!status) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onRetirementBoundary) onRetirementBoundary(RetirementBoundary::published);
#endif
    std::optional<OwnerRetirement> confirmed;
    status = readRetirementFile(directory, owner, confirmed);
    if (!status || !confirmed) return status ? fail(Error::corruptJournal, "retirement_confirmation_missing") : status;
    if (confirmed->head != expected || confirmed->nonce != nonce || canonical(confirmed->evidence) != canonical(evidence))
        return fail(Error::corruptJournal, "retirement_confirmation_changed");
    output = std::move(*confirmed);
    return {};
#else
    juce::ignoreUnused(output, expectedRootId, expectedDirectoryId);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "retirement_publish_exception"); }

Status RecoveryStore::readAudio(const juce::String& assetId, AudioBlock& output) const
{
    size_t consumed = 0;
    return readAudioBounded(assetId, limits.assetBytes, output, consumed);
}

Status RecoveryStore::readAudioBounded(const juce::String& assetId, size_t maximum,
                                     AudioBlock& output, size_t& consumed) const
{
    consumed = 0;
    if (root == juce::File() || !validLimits(limits) || owner.isNull() || !token(assetId, 64))
        return fail(Error::missingOrCorruptAsset, "asset_token_or_limits");
#if JUCE_WINDOWS
    DirectoryPins pins;
    if (!pinDirectories(root.getChildFile(owner.toString()).getChildFile("assets"), false, pins))
        return fail(Error::missingOrCorruptAsset, "asset_directory");
#endif
    juce::MemoryBlock payload;
    auto status = readFrame(root.getChildFile(owner.toString()).getChildFile("assets").getChildFile(assetId),
                            std::min(maximum, limits.assetBytes), payload, &consumed);
    if (!status)
        return status.error == Error::resourceOrEnvironmentFault || status.error == Error::unsupportedSchema
            ? status : fail(Error::missingOrCorruptAsset, "asset_hash_or_read");
    if (digest(payload) != assetId) return fail(Error::missingOrCorruptAsset, "asset_hash_or_read");
    return decodeAudio(payload, limits.assetBytes, output);
}

Status RecoveryStore::checkHead(const SafeHead& expected) const try
{
    if (root == juce::File() || owner.isNull() || !validLimits(limits)
        || expected.sequence < 0 || (expected.sequence == 0 ? expected.hash.isNotEmpty() : !token(expected.hash, 64)))
        return fail(Error::invalidPacket, "head_fence_request");
    std::optional<OwnerRetirement> retirement;
    const auto retired = inspectRetirement(retirement);
    if (!retired) return retired;
    if (retirement) return fail(Error::staleHead, "owner_retired");
    const auto directory = root.getChildFile(owner.toString());
    if (linkedPath(directory)) return fail(Error::corruptJournal, "owner_path");
    const auto file = directory.getChildFile("head");
#if JUCE_WINDOWS
    if (GetFileAttributesW(files::nativePath(file).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return expected.sequence == 0 ? Status{} : fail(Error::staleHead, "head_fence_missing");
        return fail(Error::resourceOrEnvironmentFault, "head_access");
    }
    DirectoryPins pins;
    const auto pinned = pinDirectories(directory, false, pins);
    if (!pinned) return pinned;
#else
    if (!file.exists()) return expected.sequence == 0 ? Status{} : fail(Error::staleHead, "head_fence_missing");
#endif
    SafeHead actual;
    const auto status = decodeHead(file, owner, actual);
    if (!status) return status;
    return actual == expected ? Status{} : fail(Error::staleHead, "head_fence_changed");
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "head_fence_exception"); }

OwnerSummary RecoveryStore::inspectSummary(std::function<bool()> cancel) const try
{
    OwnerSummary result;
    if (cancel && cancel()) { result.status = fail(Error::cancelled, "summary_cancelled"); return result; }
    if (root == juce::File() || owner.isNull() || !validLimits(limits))
    { result.status = fail(Error::invalidPacket, "summary_request"); return result; }
    std::optional<OwnerRetirement> retirement;
    result.status = inspectRetirement(retirement);
    if (!result.status) return result;
    if (retirement)
    {
        result.retired = true;
        result.head = retirement->head;
        return result;
    }
    const auto directory = root.getChildFile(owner.toString());
    std::unique_ptr<detail::DurableDirectory> pinned;
    result.status = detail::DurableDirectory::open(directory, false, pinned);
    if (!result.status || !pinned) return result;
#if JUCE_WINDOWS
    const auto attributes = GetFileAttributesW(files::nativePath(directory.getChildFile("head")).toWideCharPointer());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            result.status = fail(Error::resourceOrEnvironmentFault, "summary_head_access");
        return result;
    }
#endif
    SafeHead frozen;
    result.status = decodeHead(directory.getChildFile("head"), owner, frozen);
    if (!result.status) return result;
    std::unique_ptr<detail::DurableDirectory> commits;
    result.status = detail::DurableDirectory::open(directory.getChildFile("commits"), false, commits);
    if (!result.status) return result;
    if (!commits) { result.status = fail(Error::corruptJournal, "summary_commits_missing"); return result; }
    juce::var record; juce::String hash;
    result.status = readJson(commits->path().getChildFile(frozen.hash),
                            std::min({ limits.recordBytes, limits.journalReadBytes, limits.readBytes }),
                            record, &hash, &result.recordBytesRead);
    if (!result.status) return result;
    if (hash != frozen.hash) { result.status = fail(Error::corruptJournal, "summary_record_hash"); return result; }
    result.status = recordSchema(record, owner.toString(), frozen.sequence);
    if (!result.status) return result;
    if (cancel && cancel()) { result.status = fail(Error::cancelled, "summary_cancelled"); return result; }
    result.status = checkHead(frozen);
    if (!result.status) return result;
    result.head = frozen;
    result.sourcePath = record["source"]["path"].toString();
    result.lastSafeMilliseconds = static_cast<int64_t>(record["timestamp"]);
    return result;
}
catch (...) { OwnerSummary result; result.status = fail(Error::resourceOrEnvironmentFault, "summary_exception"); return result; }

ManifestView RecoveryStore::inspectManifest(std::function<bool()> cancel) const try
{
    ManifestView result;
    if (cancel && cancel()) { result.status = fail(Error::cancelled, "manifest_cancelled"); return result; }
    if (root == juce::File() || !validLimits(limits))
    { result.status = fail(Error::invalidPacket, "store_path_or_limits"); return result; }
#if JUCE_WINDOWS
    result.status = terminalIntentFence(root, owner);
    if (!result.status) return result;
#endif
    const auto directory = root.getChildFile(owner.toString());
    const auto headFile = directory.getChildFile("head");
    if (owner.isNull() || linkedPath(directory)) { result.status = fail(Error::corruptJournal, "owner_path"); return result; }
#if JUCE_WINDOWS
    if (GetFileAttributesW(files::nativePath(headFile).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            result.status = inspectRetirement(result.retirement);
            return result;
        }
        result.status = fail(Error::resourceOrEnvironmentFault, "head_access");
        return result;
    }
    DirectoryPins pins;
    result.status = pinDirectories(directory, false, pins);
    if (!result.status) return result;
    const auto rootIdentity = directoryId(pins[pins.size() - 2]->value);
    const auto ownerIdentity = directoryId(pins.back()->value);
    result.status = pinDirectories(directory.getChildFile("commits"), false, pins);
    if (!result.status) return result;
#else
    if (!headFile.exists()) return result;
#endif
    SafeHead frozen;
    juce::String checkpointCut;
    result.status = decodeHead(headFile, owner, frozen, &checkpointCut);
    if (!result.status) return result;
    size_t cutBytes = 0;
#if JUCE_WINDOWS
    if (checkpointCut.isNotEmpty())
    {
        result.status = readCheckpointCut(directory, owner, checkpointCut, rootIdentity, ownerIdentity,
            std::min({ limits.recordBytes, limits.journalReadBytes, limits.readBytes }), result, cutBytes);
        if (!result.status) return result;
    }
#endif
    const auto firstSequence = result.checkpointCut.isEmpty() ? int64_t(1) : result.checkpointFloor.sequence;
    if (firstSequence > frozen.sequence || static_cast<uint64_t>(frozen.sequence - firstSequence) >= limits.records)
    { result.status = fail(Error::budgetExceeded, "chain_budget"); return result; }
    auto hash = frozen.hash;
    size_t remaining = limits.readBytes - cutBytes;
    size_t remainingJournal = limits.journalReadBytes - cutBytes;
    juce::String lineage, runtime;
    for (auto sequence = frozen.sequence; sequence >= firstSequence; --sequence)
    {
        if (cancel && cancel()) { result.status = fail(Error::cancelled, "manifest_cancelled"); break; }
        juce::var record;
        juce::String actual;
        if (remaining == 0 || remainingJournal == 0)
        { result.status = fail(Error::budgetExceeded, "read_budget"); break; }
        size_t consumed = 0;
        result.status = readJson(directory.getChildFile("commits").getChildFile(hash),
                                 std::min({ limits.recordBytes, remaining, remainingJournal }), record, &actual, &consumed);
        if (!result.status) break;
        remaining -= consumed;
        remainingJournal -= consumed;
        if (actual != hash) { result.status = fail(Error::corruptJournal, "record_name_hash"); break; }
        result.status = recordSchema(record, owner.toString(), sequence);
        if (!result.status) break;
        if (sequence == firstSequence && result.checkpointCut.isNotEmpty()
            && (hash != result.checkpointFloor.hash || record["operation"] != juce::var("recovery.checkpoint")))
        { result.status = fail(Error::corruptJournal, "checkpoint_floor_record"); break; }
        const auto doc = record["document"];
        if (sequence == frozen.sequence) { lineage = doc["lineage"].toString(); runtime = doc["runtime_instance"].toString(); }
        if (lineage != doc["lineage"].toString() || runtime != doc["runtime_instance"].toString())
        { result.status = fail(Error::corruptJournal, "document_chain"); break; }
        result.commits.push_back({ { sequence, hash }, record });
        hash = record["parent"].toString();
    }
    if (result.status && cancel && cancel()) result.status = fail(Error::cancelled, "manifest_cancelled");
    if (!result.status) { result.commits.clear(); return result; }
    std::reverse(result.commits.begin(), result.commits.end());
    result.status = readRetirementFile(directory, owner, result.retirement);
    if (result.status && result.retirement && result.retirement->head != frozen)
        result.status = fail(Error::corruptJournal, "retirement_head_changed");
    if (!result.status) { result.commits.clear(); return result; }
    result.head = frozen;
    result.verifiedRecordBytes = limits.journalReadBytes - remainingJournal;
    return result;
}
catch (...)
{
    ManifestView result;
    result.status = fail(Error::resourceOrEnvironmentFault, "manifest_read_exception");
    return result;
}

ReadView RecoveryStore::inspect() const try
{
    ReadView result;
    const auto manifest = inspectManifest();
    result.status = manifest.status;
    if (!result.status) return result;
    auto remaining = limits.readBytes - manifest.verifiedRecordBytes;
    std::set<juce::String> verifiedAssets;
    // Match the original newest-to-oldest asset verification order and combined
    // journal+sample byte budget. Only this full pass can yield VerifiedCommit.
    for (auto it = manifest.commits.rbegin(); it != manifest.commits.rend(); ++it)
        for (const auto& asset : *it->record["assets"].getArray())
        {
            const auto id = asset.toString();
            if (!verifiedAssets.insert(id).second) continue;
            if (remaining == 0) { result.status = fail(Error::budgetExceeded, "read_budget"); return result; }
            AudioBlock audio;
            size_t consumed = 0;
            result.status = readAudioBounded(id, std::min(remaining, limits.assetBytes), audio, consumed);
            if (!result.status) return result;
            remaining -= consumed;
        }
    for (const auto& commit : manifest.commits) result.commits.push_back({ commit.head, commit.record });
    result.head = manifest.head;
    result.verifiedBytes = limits.readBytes - remaining;
    result.verifiedRecordBytes = manifest.verifiedRecordBytes;
    return result;
}
catch (...)
{
    ReadView result;
    result.status = fail(Error::resourceOrEnvironmentFault, "store_inspect_exception");
    return result;
}

Status RecoveryStore::identifyAudio(const AudioBlock& block, size_t maximum, juce::String& assetId)
{
    juce::MemoryBlock header;
    const auto status = encodeAudioHeader(block, maximum, header);
    if (status) assetId = audioDigest(header, block.samples);
    return status;
}

Status RecoveryStore::compact(const RecoveryCompactionProof& proof, std::function<bool()> cancel) try
{
    const auto cancelled = [&] { return cancel && cancel(); };
    if (proof.root != root || proof.owner != owner || owner.isNull() || !validLimits(limits)
        || proof.floor.sequence < 1 || proof.floor.sequence > proof.head.sequence)
        return fail(Error::invalidPacket, "compaction_proof_binding");
    if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
#if JUCE_WINDOWS
    if (auto fence = terminalIntentFence(root, owner); !fence) return fence;
    RootMutationQuota quota;
    auto status = quota.acquire(root, limits);
    if (!status) return status;
    const auto directory = root.getChildFile(owner.toString());
    DirectoryPins pins;
    if (!(status = pinDirectories(directory, false, pins))) return status;
    const auto rootId = directoryId(pins[pins.size() - 2]->value), ownerId = directoryId(pins.back()->value);
    if (rootId.isEmpty() || ownerId.isEmpty()) return fail(Error::resourceOrEnvironmentFault, "compaction_directory_identity");
    Handle writer, usage;
    if (!(status = openWriterLock(directory, writer)) || !(status = openUsageLock(directory, Usage::compaction, usage))) return status;
    const auto view = inspectManifest(cancel);
    if (!view.status) return view.status;
    if (view.head != proof.head || view.retirement) return fail(Error::staleHead, "compaction_head_changed");
    const auto floor = std::find_if(view.commits.begin(), view.commits.end(),
        [&] (const auto& commit) { return commit.head == proof.floor; });
    if (floor == view.commits.end() || floor->record["operation"] != juce::var("recovery.checkpoint"))
        return fail(Error::staleHead, "compaction_floor_changed");
    if (floor == view.commits.begin()) return {};
    // Finish prior durable per-file transactions before replacing their base.
    // An interruption can leak; it never licenses an old plan under a new cut.
    std::unique_ptr<detail::DurableDirectory> quarantine;
    if (!(status = detail::DurableDirectory::open(directory.getChildFile("quarantine"), false, quarantine))) return status;
    if (quarantine)
    {
        std::vector<juce::String> names;
        if (!(status = quarantine->listNames(4096, names))) return status;
        if (std::any_of(names.begin(), names.end(), [] (const auto& name) { return name.startsWith("plan-"); }))
            return fail(Error::resourceOrEnvironmentFault, "compaction_pending_collection");
    }
    const auto commits = directory.getChildFile("commits"), assets = directory.getChildFile("assets");
    if (!(status = pinDirectories(commits, false, pins)) || !(status = pinDirectories(assets, false, pins))) return status;
    // The codec certifies all metadata/history. Store independently certifies
    // every retained physical audio dependency before changing the read floor.
    AppendWork work;
    AppendAssets closure(assets, appendCache && appendCache->head == view.head ? &appendCache->assets : nullptr, work);
    std::set<juce::String> protectedAssets;
    size_t requiredRead = view.verifiedRecordBytes;
    for (auto retained = floor; retained != view.commits.end(); ++retained)
        for (const auto& asset : *retained->record["assets"].getArray()) protectedAssets.insert(asset.toString());
    for (const auto& asset : protectedAssets)
    {
        if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
        if (requiredRead >= limits.readBytes) return fail(Error::budgetExceeded, "compaction_read_budget");
        if (!(status = closure.verify(asset, std::min(limits.assetBytes, limits.readBytes - requiredRead)))) return status;
        requiredRead += closure.proofs.at(asset).payloadBytes;
    }
    juce::Array<juce::var> records, cuts;
    std::set<juce::String> seen;
    const auto existing = [&](const juce::String& name, bool& present) -> Status
    {
        const auto attributes = GetFileAttributesW(files::nativePath(commits.getChildFile(name)).toWideCharPointer());
        present = attributes != INVALID_FILE_ATTRIBUTES;
        if (!present && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            return fail(Error::resourceOrEnvironmentFault, "compaction_inventory_access");
        if (present && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
            return fail(Error::corruptJournal, "compaction_inventory_kind");
        return {};
    };
    const auto addRecord = [&](const SafeHead& old) -> Status
    {
        bool present = false;
        const auto checked = existing(old.hash, present);
        if (!checked) return checked;
        if (present && seen.insert(old.hash).second)
            records.add(object({ { "sequence", old.sequence }, { "hash", old.hash } }));
        return {};
    };
    for (const auto& old : view.supersededRecords) if (!(status = addRecord(old))) return status;
    for (auto old = view.commits.begin(); old != floor; ++old) if (!(status = addRecord(old->head))) return status;
    auto previousCuts = view.supersededCuts;
    if (view.checkpointCut.isNotEmpty()) previousCuts.push_back(view.checkpointCut);
    for (const auto& old : previousCuts)
    {
        bool present = false;
        if (!(status = existing(old, present))) return status;
        if (present && seen.insert(old).second) cuts.add(old);
    }
    if (seen.size() > 4096) return fail(Error::budgetExceeded, "compaction_inventory_budget");
    const auto payload = bytes(canonical(object({ { "kind", "checkpoint-cut" }, { "schema", 1 },
        { "owner", owner.toString() }, { "root_id", rootId }, { "owner_id", ownerId },
        { "floor_sequence", floor->head.sequence }, { "floor_hash", floor->head.hash },
        { "records", juce::var(records) }, { "cuts", juce::var(cuts) } })));
    if (payload.getSize() > limits.recordBytes) return fail(Error::budgetExceeded, "compaction_record_budget");
    const auto cutHash = digest(payload);
    const auto head = headPayload(owner, view.head, cutHash);
    // Control files consume real owner storage too. Reserve their conservative
    // peak without borrowing the protected set or widening the existing bound.
    int64_t used = 0;
    size_t entries = 0;
    std::vector<juce::File> inventory { directory };
    for (size_t next = 0; next < inventory.size(); ++next)
    {
        for (const auto& entry : juce::RangedDirectoryIterator(files::nativeFile(inventory[next]), false, "*", juce::File::findFilesAndDirectories))
        {
            if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
            const auto file = entry.getFile();
            if (++entries > 4096 + 8 || linkedPath(file)) return fail(Error::budgetExceeded, "compaction_owner_inventory");
            if (entry.isDirectory())
            {
                if (next != 0 || (file.getFileName() != "assets" && file.getFileName() != "commits" && file.getFileName() != "quarantine"))
                    return fail(Error::corruptJournal, "compaction_owner_layout");
                inventory.push_back(file);
            }
            else
            {
                const auto size = entry.getFileSize();
                if (size < 0 || size > limits.ownerBytes - used)
                    return fail(Error::budgetExceeded, "compaction_owner_budget");
                used += size;
            }
        }
    }
    const auto additional = static_cast<int64_t>(payload.getSize() + head.getSize() + 2 * frameHeaderBytes);
    if (additional > limits.ownerBytes - used)
        return byteCapacityFailure("compaction_owner_budget", ByteCapacity::Resource::ownerStorage,
            static_cast<uint64_t>(used) + additional, limits.ownerBytes);
    const QuotaHandles held {
        { directory.getChildFile("writer.lock").getFullPathName(), { writer.value, true } },
        { directory.getChildFile("usage.lease").getFullPathName(), usage.value } };
    if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
    if (!(status = quota.admitControl(root, payload, held))) return status;
    const auto cutFile = commits.getChildFile(cutHash);
    if (files::nativeFile(cutFile).exists())
    {
        juce::MemoryBlock previous;
        if (!(status = readFrame(cutFile, limits.recordBytes, previous))) return status;
        if (previous != payload) return fail(Error::corruptJournal, "compaction_cut_conflict");
    }
    else if (!(status = publish(cutFile, payload, false))) return status;
    quota.releaseInventory();
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onCompactionBoundary) onCompactionBoundary(CompactionBoundary::cutPublished);
#endif
    if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
    if (!(status = quota.admitControl(root, head, held))) return status;
    // The inventory pins must not deny replacement of the old head. Its exact
    // identity is fenced by the owner writer and root transaction throughout.
    quota.releaseInventory();
    if (!(status = closure.recheck()) || !(status = checkHead(view.head))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onCompactionBoundary) onCompactionBoundary(CompactionBoundary::beforeHeadReplace);
#endif
    if (cancelled()) return fail(Error::cancelled, "compaction_cancelled");
    if (!(status = publish(directory.getChildFile("head"), head, true))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onCompactionBoundary) onCompactionBoundary(CompactionBoundary::headPublished);
#endif
    return {};
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "compaction_exception"); }

Status RecoveryStore::collectOrphans(const SafeHead& expected, OrphanCollectionReport& report,
                                    OrphanCollectionBudget budget, std::function<bool()> cancel) try
{
    report = {};
    if (root == juce::File() || owner.isNull() || !validLimits(limits) || budget.files == 0 || budget.files > 128
        || budget.bytes == 0 || budget.bytes > limits.readBytes || expected.sequence < 0
        || (expected.sequence == 0 ? expected.hash.isNotEmpty() : !token(expected.hash, 64)))
        return fail(Error::invalidPacket, "collection_request");
    const auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
#if JUCE_WINDOWS
    const auto exists = [] (const juce::File& file, bool& present) -> Status
    {
        const auto attributes = GetFileAttributesW(files::nativePath(file).toWideCharPointer());
        if (attributes != INVALID_FILE_ATTRIBUTES) { present = true; return {}; }
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { present = false; return {}; }
        return fail(Error::resourceOrEnvironmentFault, "collection_path_access");
    };
    const auto directory = root.getChildFile(owner.toString());
    bool present = false;
    auto status = exists(directory, present);
    if (!status) return status;
    if (!present) return expected.sequence == 0 ? Status{} : fail(Error::staleHead, "collection_owner_missing");
    RootMutationQuota globalQuota;
    if (!(status = globalQuota.acquire(root, limits))) return status;
    DirectoryPins pins;
    if (!(status = pinDirectories(directory, false, pins))) return status;
    const auto rootId = directoryId(pins[pins.size() - 2]->value), ownerId = directoryId(pins.back()->value);
    Handle writerLock;
    if (!(status = openWriterLock(directory, writerLock))) return status;
    const auto manifest = inspectManifest(cancel);
    if (!manifest.status) return manifest.status;
    if (manifest.head != expected) return fail(Error::staleHead, "collection_head_changed");
    report.head = manifest.head;
    std::set<juce::String> protectedAssets;
    for (const auto& commit : manifest.commits)
        for (const auto& id : *commit.record["assets"].getArray()) protectedAssets.insert(id.toString());
    std::map<juce::String, int64_t> prunableRecords;
    std::set<juce::String> prunableCuts(manifest.supersededCuts.begin(), manifest.supersededCuts.end());
    for (const auto& record : manifest.supersededRecords) prunableRecords.emplace(record.hash, record.sequence);
    const auto quarantine = directory.getChildFile("quarantine");
    std::map<juce::String, std::vector<juce::String>> names;
    size_t entries = 0;
    for (const auto* area : { "root", "assets", "commits", "quarantine" })
    {
        const auto path = juce::String(area) == "root" ? directory : directory.getChildFile(area);
        std::unique_ptr<detail::DurableDirectory> inspected;
        if (!(status = detail::DurableDirectory::open(path, false, inspected))) return status;
        if (!inspected) continue;
        if (!(status = inspected->listNames(4096, names[area]))) return status;
        entries += names[area].size();
        // Append counts files; enumeration also includes the few known owner
        // directories. Keep room for those so the exact file ceiling is cleanable.
        if (entries > 4096 + 8) return fail(Error::budgetExceeded, "collection_entry_budget");
        if (!(status = pinDirectories(path, false, pins))) return status;
        std::sort(names[area].begin(), names[area].end());
    }
    std::set<juce::String> covered;
    // A missing head is not proof that pre-existing canonical records lost all
    // references. Preserve such an owner for diagnosis, never infer an empty mark.
    if (manifest.head.sequence == 0 && std::any_of(names["commits"].begin(), names["commits"].end(),
            [] (const auto& name) { return token(name, 64); }))
        return fail(Error::corruptJournal, "collection_headless_records");
    if (manifest.head.sequence == 0)
        report.retained += static_cast<size_t>(std::count_if(names["assets"].begin(), names["assets"].end(),
            [] (const auto& name) { return token(name, 64); }));
    for (const auto& name : names["commits"])
    {
        if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
        if (!token(name, 64) || name == manifest.checkpointCut || prunableRecords.contains(name) || prunableCuts.contains(name)
            || std::any_of(manifest.commits.begin(), manifest.commits.end(), [&](const auto& commit) { return commit.head.hash == name; }))
            continue;
        const auto remaining = budget.bytes - std::min(budget.bytes, report.bytesRead);
        if (remaining < limits.recordBytes) return fail(Error::budgetExceeded, "collection_control_budget");
        ManifestView cut; size_t consumed = 0;
        const auto checked = readCheckpointCut(directory, owner, name, rootId, ownerId, limits.recordBytes, cut, consumed);
        report.bytesRead += consumed;
        if (checked && unpublishedCut(cut, manifest)) prunableCuts.insert(name);
    }
    const auto sourcePath = [&] (const CollectionItem& item)
    { return (item.area == "root" ? directory : directory.getChildFile(item.area)).getChildFile(item.name); };
    const auto process = [&] (const juce::String& nonce, std::unique_ptr<Handle> prepared = {}) -> Status
    {
        if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
        std::unique_ptr<Handle> plan;
        auto result = openCollectionFile(quarantine.getChildFile("plan-" + nonce), plan);
        if (!result) return result;
        juce::MemoryBlock payload;
        if (!(result = readFrameHandle(plan->value, 4096, payload))) return result;
        const auto text = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize()));
        juce::var json;
        if (!parse(text, json) || canonical(json) != text || bytes(text) != payload)
            return fail(Error::corruptJournal, "collection_noncanonical");
        CollectionItem item;
        if (!(result = parseCollection(json, owner, nonce, item))) return result;
        covered.insert(item.area + "/" + item.name);
        if (item.base.sequence != 0 && std::none_of(manifest.commits.begin(), manifest.commits.end(),
                [&] (const auto& commit) { return commit.head == item.base; }))
            return fail(Error::staleHead, "collection_base_changed");
        if (item.area == "assets" && token(item.name, 64)
            && (manifest.head.sequence == 0 || protectedAssets.contains(item.name))) { ++report.retained; return {}; }
        const bool unlistedCut = item.checkpointPrune && !prunableRecords.contains(item.name) && !prunableCuts.contains(item.name);
        if (report.filesRemoved >= budget.files) { report.more = true; return {}; }
        const auto original = sourcePath(item), target = quarantine.getChildFile("item-" + nonce);
        bool hasOriginal = false, hasTarget = false;
        if (!(result = exists(original, hasOriginal)) || !(result = exists(target, hasTarget))) return result;
        if (hasOriginal && hasTarget) { ++report.retained; return {}; }
        if (!hasOriginal && !hasTarget) return removeCollectionFile(plan);
        if (!prepared && !(result = openCollectionFile(hasTarget ? target : original, prepared))) return result;
        const auto observed = assetVersion(prepared->value, 0);
        if (!observed || observed->volume != item.version.volume || observed->fileId != item.version.fileId
            || observed->size != item.version.size || observed->modified != item.version.modified
            || (!hasTarget && observed->changed != item.version.changed))
        { ++report.retained; return {}; }
        const auto remainingRead = budget.bytes - std::min(budget.bytes, report.bytesRead);
        if (static_cast<uint64_t>(item.version.size) > remainingRead / (item.checkpointPrune ? 2 : 1))
        { report.more = true; return {}; }
        if (item.checkpointPrune)
        {
            juce::MemoryBlock recordPayload;
            LARGE_INTEGER beginning {};
            if (!SetFilePointerEx(prepared->value, beginning, nullptr, FILE_BEGIN))
                return fail(Error::resourceOrEnvironmentFault, "checkpoint_prune_seek");
            result = readFrameHandle(prepared->value, limits.recordBytes, recordPayload);
            report.bytesRead += static_cast<size_t>(item.version.size);
            if (!result) return result;
            juce::var record;
            const auto recordText = juce::String::fromUTF8(static_cast<const char*>(recordPayload.getData()), static_cast<int>(recordPayload.getSize()));
            if (digest(recordPayload) != item.name || !parse(recordText, record) || canonical(record) != recordText
                || record["owner"] != juce::var(owner.toString()) || record["schema"] != juce::var(1))
                return fail(Error::corruptJournal, "checkpoint_prune_content");
            if (const auto found = prunableRecords.find(item.name); found != prunableRecords.end())
            {
                if (!(result = recordSchema(record, owner.toString(), found->second))) return result;
            }
            else if (record["kind"] != juce::var("checkpoint-cut"))
                return fail(Error::corruptJournal, "checkpoint_prune_cut_kind");
            if (unlistedCut)
            {
                ManifestView cut;
                if (!decodeCheckpointCut(record, owner, item.name, rootId, ownerId, cut) || !unpublishedCut(cut, manifest))
                { ++report.retained; return {}; }
            }
        }
        juce::String content;
        if (!(result = collectionHash(prepared->value, std::min(limits.assetBytes + frameHeaderBytes,
                budget.bytes - std::min(budget.bytes, report.bytesRead)), content, report.bytesRead))) return result;
        if (content != item.contentHash) { ++report.retained; return {}; }
        if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
        if (!hasTarget)
        {
            if (!(result = renameCollectionFile(prepared->value, target))) return result;
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (onCollectionBoundary) onCollectionBoundary(CollectionBoundary::quarantined);
#endif
        }
        if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
        if (!(result = checkHead(expected))) return result;
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onCollectionBoundary) onCollectionBoundary(CollectionBoundary::beforeDelete);
#endif
        if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
        if (!(result = checkHead(expected))) return result;
        if (!(result = removeCollectionFile(prepared))) return result;
        ++report.filesRemoved;
        report.bytesRemoved += static_cast<size_t>(item.version.size);
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onCollectionBoundary) onCollectionBoundary(CollectionBoundary::itemDeleted);
#endif
        if (!(result = removeCollectionFile(plan))) return result;
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onCollectionBoundary) onCollectionBoundary(CollectionBoundary::planDeleted);
#endif
        return {};
    };
    for (const auto& name : names["quarantine"])
    {
        if (name.startsWith("plan-") && token(name.substring(5), 32))
        { if (!(status = process(name.substring(5)))) return status; }
        else if (!name.startsWith("item-")
            || std::find(names["quarantine"].begin(), names["quarantine"].end(), "plan-" + name.substring(5)) == names["quarantine"].end())
            ++report.retained;
    }
    // Fixed order: failed staging, unreferenced CAS, then records explicitly
    // certified by the current checkpoint cut. Unknown canonical records stay.
    for (const int pass : { 0, 1, 2 })
        for (const auto* area : { "root", "assets", "commits" })
            for (const auto& name : names[area])
            {
                if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
                const bool prune = pass == 2 && juce::String(area) == "commits"
                    && (prunableRecords.contains(name) || prunableCuts.contains(name));
                const bool eligible = pass == 0 ? stagingName(name) : pass == 1
                    ? manifest.head.sequence > 0 && juce::String(area) == "assets" && token(name, 64) && !protectedAssets.contains(name)
                    : prune;
                if (!eligible || covered.contains(juce::String(area) + "/" + name)) continue;
                if (report.filesRemoved >= budget.files) { report.more = true; return {}; }
                CollectionItem item; item.area = area; item.name = name; item.nonce = juce::Uuid().toString(); item.base = expected;
                item.checkpointPrune = prune;
                std::unique_ptr<Handle> original;
                if (!(status = openCollectionFile(sourcePath(item), original))) return status;
                const auto version = assetVersion(original->value, 0);
                if (!version) { ++report.retained; continue; }
                item.version = *version;
                // Budget includes both initial hash and the independent check
                // under the persisted plan before quarantine/deletion.
                const auto remaining = budget.bytes - std::min(budget.bytes, report.bytesRead);
                if (static_cast<uint64_t>(item.version.size) > remaining / (prune ? 3 : 2)) { report.more = true; return {}; }
                if (!(status = collectionHash(original->value, std::min(limits.assetBytes + frameHeaderBytes, remaining / 2),
                    item.contentHash, report.bytesRead))) return status;
                if (cancelled()) return fail(Error::cancelled, "collection_cancelled");
                if (!(status = pinDirectories(quarantine, true, pins))) return status;
                const auto plan = bytes(canonical(collectionJson(item, owner)));
                if (plan.getSize() > 4096) return fail(Error::budgetExceeded, "collection_plan_budget");
                if (!(status = globalQuota.admitControl(root, plan, {
                    { directory.getChildFile("writer.lock").getFullPathName(), { writerLock.value, true } },
                    { sourcePath(item).getFullPathName(), original->value } }))) return status;
                if (!(status = publish(quarantine.getChildFile("plan-" + item.nonce), plan, false))) return status;
                globalQuota.releaseInventory();
#if SINGLE_EDITOR_RECOVERY_TESTING
                if (onCollectionBoundary) onCollectionBoundary(CollectionBoundary::marked);
#endif
                if (!(status = process(item.nonce, std::move(original)))) return status;
            }
    return {};
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "collection_exception"); }

Status RecoveryStore::inspectTerminalHead(SafeHead& output) const try
{
    if (root == juce::File() || owner.isNull() || !validLimits(limits))
        return fail(Error::invalidPacket, "terminal_head_request");
#if JUCE_WINDOWS
    const auto absent = [] (const juce::File& file, bool& missing) -> Status
    {
        missing = GetFileAttributesW(files::nativePath(file).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES;
        if (!missing) return {};
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status{}
            : fail(Error::resourceOrEnvironmentFault, "terminal_path_access");
    };
    bool ownerMissing = false, intentMissing = false;
    auto status = absent(root.getChildFile(owner.toString()), ownerMissing);
    if (!status || !(status = absent(root.getChildFile("terminal-" + owner.toString()), intentMissing))) return status;
    if (ownerMissing && intentMissing) { output = {}; return {}; }
    DirectoryPins pins;
    status = pinDirectories(root, false, pins);
    if (!status) return status;
    juce::MemoryBlock payload;
    if (!(status = readFrame(root.getChildFile("terminal-" + owner.toString()),
        std::min<size_t>(2 * 1024 * 1024, limits.readBytes), payload))) return status;
    const auto text = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize()));
    juce::var plan;
    if (!parse(text, plan) || canonical(plan) != text || bytes(text) != payload)
        return fail(Error::corruptJournal, "terminal_plan_json");
    if (!keys(plan, { "kind", "schema", "owner", "root", "root_id", "directory_id", "nonce", "sequence", "head", "evidence", "items", "directories" })
        || plan["kind"] != juce::var("terminal-collection") || !integer(plan["schema"], 1)
        || (static_cast<int64_t>(plan["schema"]) != 1 && static_cast<int64_t>(plan["schema"]) != 2))
        return fail(Error::unsupportedSchema, "terminal_plan_schema");
    if (plan["owner"] != juce::var(owner.toString()) || plan["root"] != juce::var(root.getFullPathName())
        || pins.empty() || plan["root_id"] != juce::var(directoryId(pins.back()->value))
        || !token(plan["directory_id"].toString(), 48) || !uuid(plan["nonce"])
        || !integer(plan["sequence"], 1) || !token(plan["head"].toString(), 64)
        || !retirementEvidence(plan["evidence"]) || !plan["items"].isArray() || !plan["directories"].isArray())
        return fail(Error::corruptJournal, "terminal_plan_scope");
    output = { static_cast<int64_t>(plan["sequence"]), plan["head"].toString() };
    return {};
#else
    juce::ignoreUnused(output);
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "terminal_head_exception"); }

Status RecoveryStore::collectTerminal(const SafeHead& expected, TerminalCollectionReport& report,
    OrphanCollectionBudget budget, std::function<bool()> cancel) try
{
    report = {};
    if (root == juce::File() || owner.isNull() || !validLimits(limits) || expected.sequence < 1 || !token(expected.hash, 64)
        || budget.files == 0 || budget.files > 128 || budget.bytes == 0 || budget.bytes > limits.readBytes)
        return fail(Error::invalidPacket, "terminal_request");
    const auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
#if JUCE_WINDOWS
    const auto exists = [] (const juce::File& file, bool& present) -> Status
    {
        present = GetFileAttributesW(files::nativePath(file).toWideCharPointer()) != INVALID_FILE_ATTRIBUTES;
        if (present) return {};
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Status{}
            : fail(Error::resourceOrEnvironmentFault, "terminal_path_access");
    };
    const auto directory = root.getChildFile(owner.toString()), intentPath = root.getChildFile("terminal-" + owner.toString());
    bool hasOwner = false, hasIntent = false;
    auto status = exists(directory, hasOwner);
    if (!status || !(status = exists(intentPath, hasIntent))) return status;
    if (!hasOwner && !hasIntent) { report.completed = true; return {}; }
    RootMutationQuota globalQuota;
    if (!(status = globalQuota.acquire(root, limits))) return status;
    DirectoryPins rootPins;
    if (!(status = pinDirectories(root, false, rootPins))) return status;
    const auto rootIdentity = directoryId(rootPins.back()->value);
    if (!token(rootIdentity, 48)) return fail(Error::resourceOrEnvironmentFault, "terminal_root_identity");

    // Only the first pass can inspect the complete chain; the durable inventory
    // then becomes the authority for resumption after intentional record deletion.
    ManifestView initial;
    if (!hasIntent)
    {
        initial = inspectManifest(cancel);
        if (!initial.status) return initial.status;
        if (!initial.retirement) return fail(Error::staleHead, "terminal_marker_required");
        if (initial.head != expected || initial.commits.empty()) return fail(Error::staleHead, "terminal_head_changed");
    }
    std::unique_ptr<Handle> ownerGuard, intent, writer, usage;
    if (hasOwner && !(status = openDeletingDirectory(directory, ownerGuard))) return status;
    if (hasIntent && !(status = openCollectionFile(intentPath, intent))) return status;
    const auto ownerIdentity = ownerGuard ? directoryId(ownerGuard->value) : juce::String();
    const auto sourcePath = [&] (const CollectionItem& item)
    { return (item.area == "root" ? directory : directory.getChildFile(item.area)).getChildFile(item.name); };

    // Own both historical lock files while the directory DELETE handle prevents
    // new admissions. Do not create, truncate or delete malformed lock objects.
    if (hasOwner)
        for (const auto* name : { "writer.lock", "usage.lease" })
        {
            bool present = false;
            const auto file = directory.getChildFile(name);
            if (!(status = exists(file, present))) return status;
            if (!present) continue;
            auto& lock = juce::String(name) == "writer.lock" ? writer : usage;
            if (!(status = openCollectionFile(file, lock))) return status;
            const auto version = assetVersion(lock->value, 0);
            if (!version || version->size != 0) return fail(Error::corruptJournal, "terminal_lock_contract");
        }

    juce::var plan;
    std::vector<CollectionItem> items;
    std::map<juce::String, std::unique_ptr<Handle>> subdirectories;
    std::map<juce::String, std::unique_ptr<Handle>> authorityGuards;
    if (hasOwner)
        for (const auto* name : { "assets", "commits", "quarantine" })
        {
            bool present = false;
            if (!(status = exists(directory.getChildFile(name), present))) return status;
            if (present && !(status = openDeletingDirectory(directory.getChildFile(name), subdirectories[name]))) return status;
        }
    if (!hasIntent)
    {
        if (!usage) return fail(Error::corruptJournal, "terminal_usage_missing");
        SafeHead frozen;
        std::optional<OwnerRetirement> retired;
        if (!(status = decodeHead(directory.getChildFile("head"), owner, frozen))
            || !(status = readRetirementFile(directory, owner, retired))) return status;
        if (frozen != expected || !retired || retired->head != expected || retired->nonce != initial.retirement->nonce
            || canonical(retired->evidence) != canonical(initial.retirement->evidence))
            return fail(Error::staleHead, "terminal_authority_changed");
        std::set<juce::String> commits, protectedAssets;
        std::map<juce::String, int64_t> supersededRecords;
        std::set<juce::String> cuts(initial.supersededCuts.begin(), initial.supersededCuts.end());
        if (initial.checkpointCut.isNotEmpty()) cuts.insert(initial.checkpointCut);
        for (const auto& old : initial.supersededRecords)
        {
            supersededRecords.emplace(old.hash, old.sequence);
            commits.insert(old.hash);
        }
        commits.insert(cuts.begin(), cuts.end());
        for (const auto& commit : initial.commits)
        {
            commits.insert(commit.head.hash);
            for (const auto& id : *commit.record["assets"].getArray()) protectedAssets.insert(id.toString());
        }
        std::map<juce::String, std::vector<juce::String>> names;
        size_t entries = 0;
        report.bytesRead = initial.verifiedRecordBytes;
        for (const auto* area : { "root", "assets", "commits", "quarantine" })
        {
            if (juce::String(area) != "root" && !subdirectories.contains(area)) continue;
            if (!(status = terminalNames(juce::String(area) == "root" ? directory : directory.getChildFile(area), names[area]))) return status;
            entries += names[area].size();
            if (entries > 4096) return fail(Error::budgetExceeded, "terminal_entry_budget");
            for (const auto& name : names[area])
            {
                if (juce::String(area) == "root" && (name == "writer.lock" || name == "assets" || name == "commits" || name == "quarantine")) continue;
                if (juce::String(area) == "commits" && token(name, 64) && !commits.contains(name))
                {
                    ManifestView cut; size_t consumed = 0;
                    const auto checked = readCheckpointCut(directory, owner, name, rootIdentity, ownerIdentity,
                        std::min(limits.recordBytes, limits.readBytes - std::min(limits.readBytes, report.bytesRead)), cut, consumed);
                    report.bytesRead += consumed;
                    if (checked && unpublishedCut(cut, initial)) { commits.insert(name); cuts.insert(name); }
                }
                if (!(stagingName(name) && juce::String(area) != "quarantine")
                    && !(juce::String(area) == "assets" && token(name, 64))
                    && !(juce::String(area) == "commits" && commits.contains(name))
                    && !(juce::String(area) == "quarantine" && quarantineName(name))
                    && !(juce::String(area) == "root" && (name == "head" || name == "retired" || name == "usage.lease")))
                    return fail(Error::corruptJournal, "terminal_unknown_entry");
            }
        }
        for (const auto& commit : initial.commits)
        {
            if (!std::binary_search(names["commits"].begin(), names["commits"].end(), commit.head.hash))
                return fail(Error::corruptJournal, "terminal_record_missing");
            for (const auto& asset : *commit.record["assets"].getArray())
                if (!std::binary_search(names["assets"].begin(), names["assets"].end(), asset.toString()))
                    return fail(Error::missingOrCorruptAsset, "terminal_asset_missing");
        }
        // Recognize a prior orphan transaction by its full supported plan and
        // original physical object, not merely plan-/item- filenames. Capture
        // current versions/hashes for the later independent terminal inventory.
        std::map<juce::String, CollectionItem> quarantineProofs;
        std::set<juce::String> orphanSources;
        const auto remember = [&] (const juce::String& area, const juce::String& name,
                                   HANDLE handle, CollectionItem& proof) -> Status
        {
            proof.area = area; proof.name = name;
            const auto version = assetVersion(handle, 0);
            if (!version) return fail(Error::resourceOrEnvironmentFault, "terminal_quarantine_version");
            proof.version = *version;
            return collectionHash(handle, std::min(limits.assetBytes + frameHeaderBytes,
                limits.readBytes - std::min(limits.readBytes, report.bytesRead)), proof.contentHash, report.bytesRead);
        };
        for (const auto& name : names["quarantine"])
        {
            if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
            const auto nonce = name.substring(5);
            if (name.startsWith("item-"))
            {
                if (!std::binary_search(names["quarantine"].begin(), names["quarantine"].end(), "plan-" + nonce))
                    return fail(Error::corruptJournal, "terminal_quarantine_unplanned");
                continue;
            }
            std::unique_ptr<Handle> oldPlan;
            if (!(status = openCollectionFile(directory.getChildFile("quarantine").getChildFile(name), oldPlan))) return status;
            juce::MemoryBlock oldPayload; size_t consumed = 0;
            if (!(status = readFrameHandle(oldPlan->value, std::min(size_t{4096},
                limits.readBytes - std::min(limits.readBytes, report.bytesRead)), oldPayload, &consumed))) return status;
            report.bytesRead += consumed;
            const auto oldText = juce::String::fromUTF8(static_cast<const char*>(oldPayload.getData()), static_cast<int>(oldPayload.getSize()));
            juce::var json;
            if (!parse(oldText, json) || canonical(json) != oldText || bytes(oldText) != oldPayload)
                return fail(Error::corruptJournal, "terminal_quarantine_json");
            CollectionItem old;
            if (!(status = parseCollection(json, owner, nonce, old))) return status;
            if (old.base.sequence != 0 && std::none_of(initial.commits.begin(), initial.commits.end(),
                    [&] (const auto& commit) { return commit.head == old.base; }))
                return fail(Error::staleHead, "terminal_quarantine_base");
            if (!orphanSources.insert(old.area + "/" + old.name).second
                || (old.area == "assets" && protectedAssets.contains(old.name)))
                return fail(Error::corruptJournal, "terminal_quarantine_reference");
            CollectionItem planProof;
            if (!(status = remember("quarantine", name, oldPlan->value, planProof))) return status;
            quarantineProofs.emplace("quarantine/" + name, std::move(planProof));
            const bool hasOriginal = std::binary_search(names[old.area].begin(), names[old.area].end(), old.name);
            const bool hasTarget = std::binary_search(names["quarantine"].begin(), names["quarantine"].end(), "item-" + nonce);
            if (hasOriginal && hasTarget) return fail(Error::staleHead, "terminal_quarantine_ambiguous");
            if (!hasOriginal && !hasTarget) continue; // Prior GC died after its object delete.
            const auto area = hasTarget ? juce::String("quarantine") : old.area;
            const auto selectedName = hasTarget ? "item-" + nonce : old.name;
            const auto path = (area == "root" ? directory : directory.getChildFile(area)).getChildFile(selectedName);
            std::unique_ptr<Handle> file;
            if (!(status = openCollectionFile(path, file))) return status;
            CollectionItem proof;
            if (!(status = remember(area, selectedName, file->value, proof))) return status;
            if (proof.version.volume != old.version.volume || proof.version.fileId != old.version.fileId
                || proof.version.size != old.version.size || proof.version.modified != old.version.modified
                || (!hasTarget && proof.version.changed != old.version.changed) || proof.contentHash != old.contentHash)
                return fail(Error::staleHead, "terminal_quarantine_changed");
            // A genuine rename changes ChangeTime. From this point onward its
            // newly certified ChangeTime is exact, including resumed batches.
            quarantineProofs.emplace(area + "/" + selectedName, std::move(proof));
        }
        juce::Array<juce::var> inventory;
        // Payload first. The root intent remains durable across removal of head,
        // terminal marker, usage lock and the now-empty directory itself.
        for (const auto* area : { "assets", "commits", "quarantine", "root" })
            for (const auto& name : names[area])
            {
                if (juce::String(area) == "root" && (name == "writer.lock" || subdirectories.contains(name))) continue;
                if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
                CollectionItem item; item.area = area; item.name = name; item.nonce = retired->nonce; item.base = expected;
                std::unique_ptr<Handle> file;
                const bool authority = item.area == "root" && (name == "head" || name == "retired");
                auto& selected = name == "usage.lease" ? usage : authority ? authorityGuards[name] : file;
                if (!selected && !(status = openCollectionFile(sourcePath(item), selected))) return status;
                const auto handle = selected->value;
                const auto version = assetVersion(handle, 0);
                if (!version) return fail(Error::resourceOrEnvironmentFault, "terminal_file_version");
                item.version = *version;
                if (!(status = collectionHash(handle, std::min(limits.assetBytes + frameHeaderBytes,
                    limits.readBytes - std::min(limits.readBytes, report.bytesRead)), item.contentHash, report.bytesRead))) return status;
                const auto prior = quarantineProofs.find(item.area + "/" + name);
                if ((item.area == "quarantine" && prior == quarantineProofs.end())
                    || (prior != quarantineProofs.end() && (item.version != prior->second.version || item.contentHash != prior->second.contentHash)))
                    return fail(Error::staleHead, "terminal_quarantine_changed");
                // Supported canonical CAS/records must still match their names;
                // do not turn a terminal marker into implicit corrupt-owner Discard.
                if (token(name, 64) || authority)
                {
                    LARGE_INTEGER start{};
                    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) return fail(Error::resourceOrEnvironmentFault, "terminal_seek");
                    juce::MemoryBlock payload;
                    size_t additional = 0;
                    if (!(status = readFrameHandle(handle, std::min(limits.assetBytes,
                        limits.readBytes - std::min(limits.readBytes, report.bytesRead)), payload, &additional))) return status;
                    report.bytesRead += additional;
                    if (token(name, 64) && digest(payload) != name) return fail(Error::corruptJournal, "terminal_content_changed");
                    if (authority)
                    {
                        const auto expectedPayload = name == "head"
                            ? headPayload(owner, expected, initial.checkpointCut)
                            : bytes(canonical(object({ { "kind", "retirement" }, { "schema", 1 }, { "owner", owner.toString() },
                                { "sequence", expected.sequence }, { "hash", expected.hash },
                                { "nonce", retired->nonce }, { "evidence", retired->evidence } })));
                        if (payload != expectedPayload) return fail(Error::staleHead, "terminal_authority_changed");
                    }
                    if (item.area == "commits" && (cuts.contains(name) || supersededRecords.contains(name)))
                    {
                        juce::var record;
                        const auto body = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize()));
                        if (!parse(body, record) || canonical(record) != body || record["schema"] != juce::var(1)
                            || record["owner"] != juce::var(owner.toString()))
                            return fail(Error::corruptJournal, "terminal_compacted_record");
                        if (const auto old = supersededRecords.find(name); old != supersededRecords.end())
                        {
                            if (!(status = recordSchema(record, owner.toString(), old->second))) return status;
                        }
                        else if (!keys(record, { "kind", "schema", "owner", "root_id", "owner_id", "floor_sequence",
                                    "floor_hash", "records", "cuts" }) || record["kind"] != juce::var("checkpoint-cut")
                            || record["root_id"] != juce::var(rootIdentity) || record["owner_id"] != juce::var(ownerIdentity))
                            return fail(Error::unsupportedSchema, "terminal_checkpoint_cut");
                    }
                    if (item.area == "assets")
                    {
                        AudioBlock checked;
                        if (!(status = decodeAudio(payload, limits.assetBytes, checked))) return status;
                    }
                }
                auto json = collectionJson(item, owner);
                json.getDynamicObject()->setProperty("kind", "terminal-item");
                inventory.add(json);
            }
        juce::Array<juce::var> directories;
        for (const auto& [name, handle] : subdirectories)
            directories.add(object({ { "name", name }, { "id", directoryId(handle->value) } }));
        plan = object({ { "kind", "terminal-collection" }, { "schema", names["quarantine"].empty() ? 1 : 2 }, { "owner", owner.toString() },
            { "root", root.getFullPathName() }, { "root_id", rootIdentity }, { "directory_id", ownerIdentity },
            { "nonce", retired->nonce }, { "sequence", expected.sequence }, { "head", expected.hash },
            { "evidence", retired->evidence }, { "items", juce::var(inventory) }, { "directories", juce::var(directories) } });
        const auto payload = bytes(canonical(plan));
        if (payload.getSize() > 2 * 1024 * 1024) return fail(Error::budgetExceeded, "terminal_plan_budget");
        if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
        QuotaHandles held { { directory.getFullPathName(), ownerGuard->value } };
        if (writer) held.emplace(directory.getChildFile("writer.lock").getFullPathName(), writer->value);
        if (usage) held.emplace(directory.getChildFile("usage.lease").getFullPathName(), usage->value);
        for (const auto& [name, handle] : subdirectories)
            held.emplace(directory.getChildFile(name).getFullPathName(), handle->value);
        for (const auto& [name, handle] : authorityGuards)
            held.emplace(directory.getChildFile(name).getFullPathName(), handle->value);
        if (!(status = globalQuota.admitControl(root, payload, held))) return status;
        if (!(status = publish(intentPath, payload, false)) || !(status = openCollectionFile(intentPath, intent))) return status;
        globalQuota.releaseInventory();
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::planned);
#endif
        // Certification is a separate bounded pass, never hidden in sweep budget.
        report.more = true;
        return {};
    }

    juce::MemoryBlock payload;
    if (!(status = readFrameHandle(intent->value, 2 * 1024 * 1024, payload))) return status;
    const auto text = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize()));
    if (!parse(text, plan) || canonical(plan) != text || bytes(text) != payload)
        return fail(Error::corruptJournal, "terminal_plan_json");
    if (!keys(plan, { "kind", "schema", "owner", "root", "root_id", "directory_id", "nonce", "sequence", "head", "evidence", "items", "directories" })
        || plan["kind"] != juce::var("terminal-collection") || !integer(plan["schema"], 1)
        || (static_cast<int64_t>(plan["schema"]) != 1 && static_cast<int64_t>(plan["schema"]) != 2))
        return fail(Error::unsupportedSchema, "terminal_plan_schema");
    if (plan["owner"] != juce::var(owner.toString()) || plan["root"] != juce::var(root.getFullPathName())
        || plan["root_id"] != juce::var(rootIdentity) || !token(plan["directory_id"].toString(), 48)
        || (hasOwner && plan["directory_id"] != juce::var(ownerIdentity)) || !uuid(plan["nonce"])
        || !integer(plan["sequence"], 1) || static_cast<int64_t>(plan["sequence"]) != expected.sequence
        || plan["head"] != juce::var(expected.hash) || !retirementEvidence(plan["evidence"])
        || !plan["items"].isArray() || plan["items"].size() < 3 || plan["items"].size() > 4096
        || !plan["directories"].isArray() || plan["directories"].size() > 3)
        return fail(Error::corruptJournal, "terminal_plan_scope");
    std::set<juce::String> directoryNames;
    for (const auto& entry : *plan["directories"].getArray())
    {
        const auto name = entry["name"].toString();
        if (!keys(entry, { "name", "id" }) || (name != "assets" && name != "commits" && name != "quarantine")
            || !token(entry["id"].toString(), 48) || !directoryNames.insert(name).second
            || (subdirectories.contains(name) && entry["id"] != juce::var(directoryId(subdirectories.at(name)->value))))
            return fail(Error::staleHead, "terminal_subdirectory_changed");
    }
    for (const auto& [name, handle] : subdirectories)
    {
        juce::ignoreUnused(handle);
        if (!directoryNames.contains(name)) return fail(Error::staleHead, "terminal_subdirectory_changed");
    }
    std::set<juce::String> paths;
    for (const auto& json : *plan["items"].getArray())
    {
        CollectionItem item;
        if (!(status = parseCollection(json, owner, plan["nonce"].toString(), item, true))) return status;
        if (item.area == "quarantine" && static_cast<int64_t>(plan["schema"]) != 2)
            return fail(Error::unsupportedSchema, "terminal_quarantine_plan_schema");
        if (item.base != expected || !paths.insert(item.area + "/" + item.name).second)
            return fail(Error::corruptJournal, "terminal_plan_items");
        items.push_back(std::move(item));
    }
    if (!paths.contains("root/head") || !paths.contains("root/retired") || !paths.contains("root/usage.lease"))
        return fail(Error::corruptJournal, "terminal_plan_markers");
    if (hasOwner)
    {
        // Check surviving authority files before deleting any payload in this
        // batch. If another process replaced a terminal marker with a future or
        // corrupt version, preserve the owner rather than noticing only at tail.
        for (const auto& item : items)
            if ((item.area == "root" && (item.name == "head" || item.name == "retired"))
                || (item.area == "quarantine" && item.name.startsWith("plan-")))
            {
                bool present = false;
                if (!(status = exists(sourcePath(item), present))) return status;
                if (!present) continue;
                auto& file = authorityGuards[item.name];
                if (!(status = openCollectionFile(sourcePath(item), file))) return status;
                if (assetVersion(file->value, 0) != item.version) return fail(Error::staleHead, "terminal_authority_changed");
                size_t checkedBytes = 0; juce::String hash;
                if (!(status = collectionHash(file->value, 16384 + frameHeaderBytes, hash, checkedBytes))) return status;
                if (hash != item.contentHash) return fail(Error::staleHead, "terminal_authority_changed");
            }
        // Reject new/unknown objects before each batch. Never chase an unplanned
        // path or recursively remove a nonempty directory at finalization.
        for (const auto* area : { "root", "assets", "commits", "quarantine" })
        {
            if (juce::String(area) != "root" && !subdirectories.contains(area)) continue;
            std::vector<juce::String> names;
            if (!(status = terminalNames(juce::String(area) == "root" ? directory : directory.getChildFile(area), names))) return status;
            for (const auto& name : names)
                if (!paths.contains(juce::String(area) + "/" + name)
                    && !(juce::String(area) == "root" && (subdirectories.contains(name) || name == "writer.lock")))
                    return fail(Error::corruptJournal, "terminal_unknown_entry");
        }
        for (const auto& item : items)
        {
            if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
            bool present = false;
            if (!(status = exists(sourcePath(item), present))) return status;
            if (!present) continue; // Durable inventory explains a preceding kill after delete.
            if (report.filesRemoved == 0 && static_cast<uint64_t>(item.version.size) > budget.bytes)
                return fail(Error::budgetExceeded, "terminal_item_budget");
            if (report.filesRemoved >= budget.files || static_cast<uint64_t>(item.version.size) > budget.bytes - report.bytesRead)
            { report.more = true; return {}; }
            std::unique_ptr<Handle> file;
            auto& selected = item.area == "root" && item.name == "usage.lease" ? usage
                : authorityGuards.contains(item.name) ? authorityGuards.at(item.name) : file;
            if (!selected && !(status = openCollectionFile(sourcePath(item), selected))) return status;
            if (assetVersion(selected->value, 0) != item.version) return fail(Error::staleHead, "terminal_file_changed");
            juce::String hash;
            if (!(status = collectionHash(selected->value, budget.bytes - report.bytesRead, hash, report.bytesRead))) return status;
            if (hash != item.contentHash) return fail(Error::staleHead, "terminal_file_changed");
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::beforeDelete);
#endif
            if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
            if (!(status = removeCollectionFile(selected))) return status;
            ++report.filesRemoved;
            report.bytesRemoved += static_cast<size_t>(item.version.size);
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::itemDeleted);
            if (onTerminalBoundary && item.area == "root" && item.name == "head") onTerminalBoundary(TerminalBoundary::headRemoved);
            if (onTerminalBoundary && item.area == "root" && item.name == "retired") onTerminalBoundary(TerminalBoundary::markerRemoved);
#endif
        }
        if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
        if (writer && !(status = removeCollectionFile(writer))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::locksRemoved);
#endif
        if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
        for (auto& [name, handle] : subdirectories)
        {
            juce::ignoreUnused(name);
            if (!(status = removeCollectionFile(handle))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
            if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::directoryRemoved);
#endif
        }
        if (!(status = removeCollectionFile(ownerGuard))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::ownerRemoved);
#endif
    }
    if (cancelled()) return fail(Error::cancelled, "terminal_cancelled");
    bool remainingOwner = false;
    if (!(status = exists(directory, remainingOwner))) return status;
    if (remainingOwner) return fail(Error::staleHead, "terminal_owner_reappeared");
    if (!(status = removeCollectionFile(intent))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onTerminalBoundary) onTerminalBoundary(TerminalBoundary::intentRemoved);
#endif
    report.completed = true;
    return {};
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
}
catch (...) { return fail(Error::resourceOrEnvironmentFault, "terminal_collection_exception"); }

Status RecoveryStore::append(const SafeHead& expected, const CommitPacket& packet, SafeHead& published,
                            std::shared_ptr<const PublishedRecoveryCommit>* receipt, AppendWork* work,
                            std::shared_ptr<RecoveryOwnerLease>* writerActivity)
{
    AppendWork ignored;
    auto& measured = work ? *work : ignored;
    measured = {};
    published = expected;
#if JUCE_WINDOWS
    if (auto fence = terminalIntentFence(root, owner); !fence) return fence;
#endif
    if (root == juce::File() || !validLimits(limits) || owner.isNull()
        || !packet.identity.document.isValid() || packet.identity.source.binding.isNull()
        || packet.identity.asset.has_value() || packet.operationId.isEmpty() || packet.operationId.length() > 256
        || packet.planSchema < 1 || packet.timestampMilliseconds < 0
        || packet.newAudio.size() + packet.existingAudio.size() + packet.streamedAudio.size() > maximumCommitAssets
        || expected.sequence < 0 || expected.sequence == std::numeric_limits<int64_t>::max()
        || (expected.sequence == 0 ? expected.hash.isNotEmpty() : !token(expected.hash, 64))
        || packet.planJson.getNumBytesAsUTF8() > limits.recordBytes || packet.stateJson.getNumBytesAsUTF8() > limits.recordBytes)
        return fail(Error::invalidPacket, "packet_contract");
    juce::var plan, state;
    if (!parse(packet.planJson, plan) || !parse(packet.stateJson, state)) return fail(Error::invalidPacket, "packet_json");
    struct PreparedAudio final
    {
        juce::String id;
        juce::MemoryBlock header;
        const AudioBlock* audio = nullptr;
        const StreamedAudioBlock* streamed = nullptr;
        size_t payloadBytes() const
        {
            return header.getSize() + (audio ? audio->samples.getSize()
                : static_cast<size_t>(streamed->frames) * static_cast<size_t>(streamed->channels) * 4);
        }
    };
    // The synchronous append borrows its immutable packet. Keep only canonical
    // headers/IDs, never a second full set of the packet's audio samples.
    std::vector<PreparedAudio> blocks;
    std::set<juce::String> ids;
    int64_t needed = static_cast<int64_t>(limits.recordBytes + 8192);
    size_t declaredAudioBytes = 0;
    for (const auto& block : packet.newAudio)
    {
        juce::MemoryBlock header;
        const auto status = encodeAudioHeader(block, limits.assetBytes, header);
        if (!status) return status;
        const auto id = audioDigest(header, block.samples);
        if (ids.insert(id).second)
        {
            const auto payloadBytes = header.getSize() + block.samples.getSize();
            needed += static_cast<int64_t>(payloadBytes + frameHeaderBytes);
            declaredAudioBytes += payloadBytes;
            if (needed > limits.ownerBytes || declaredAudioBytes > limits.readBytes)
                return fail(Error::budgetExceeded, "packet_budget");
            blocks.push_back({ id, std::move(header), &block });
        }
    }
    for (const auto& id : packet.existingAudio)
    {
        if (!token(id, 64)) return fail(Error::invalidPacket, "asset_token");
        ids.insert(id);
    }
    for (const auto& block : packet.streamedAudio)
    {
        if (!block.read || !token(block.id, 64)) return fail(Error::invalidPacket, "streamed_audio_contract");
        const AudioBlock shape { block.sampleRate, block.channels, block.frames, {} };
        juce::MemoryBlock header;
        const auto declared = encodeAudioHeader(shape, limits.assetBytes, header, false);
        if (!declared) return declared;
        if (ids.insert(block.id).second)
        {
            PreparedAudio prepared { block.id, std::move(header), nullptr, &block };
            needed += static_cast<int64_t>(prepared.payloadBytes() + frameHeaderBytes);
            declaredAudioBytes += prepared.payloadBytes();
            if (needed > limits.ownerBytes || declaredAudioBytes > limits.readBytes)
                return fail(Error::budgetExceeded, "packet_budget");
            blocks.push_back(std::move(prepared));
        }
    }
    juce::Array<juce::var> references;
    for (const auto& id : ids) references.add(id);
    const auto& source = packet.identity.source;
    const auto record = object({ { "kind", "commit" }, { "schema", 1 }, { "owner", owner.toString() },
        { "sequence", expected.sequence + 1 }, { "parent", expected.hash }, { "document", packet.identity.document.toJson() },
        { "source", object({ { "binding", source.binding.toString() }, { "path", source.location.file.getFullPathName() },
            { "size", source.sizeBytes }, { "modified", source.modifiedMilliseconds }, { "fast_stamp_valid", source.fastStampValid } }) },
        { "operation", packet.operationId }, { "plan_schema", packet.planSchema },
        { "plan", packet.planJson }, { "plan_hash", digest(bytes(packet.planJson)) },
        { "state", packet.stateJson }, { "state_hash", digest(bytes(packet.stateJson)) },
        { "timestamp", packet.timestampMilliseconds }, { "assets", juce::var(references) } });
    if (writerActivity) record.getDynamicObject()->setProperty("activity_protocol",
        object({ { "version", 1 }, { "latest", packet.timestampMilliseconds } }));
    auto recordBytes = bytes(canonical(record));
    if (recordBytes.getSize() > limits.recordBytes || !boundedJson(canonical(record)))
        return fail(Error::budgetExceeded, "record_budget");
    auto status = recordSchema(record, owner.toString(), expected.sequence + 1);
    if (!status) return status;
    const auto directory = root.getChildFile(owner.toString());
    if (linkedPath(directory)) return fail(Error::resourceOrEnvironmentFault, "owner_directory");
#if JUCE_WINDOWS
    RootMutationQuota globalQuota;
    status = globalQuota.acquire(root, limits);
    if (!status) return status;
    bool admittedNewOwner = false;
    if (expected.sequence == 0
        && GetFileAttributesW(files::nativePath(directory).toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            return fail(Error::resourceOrEnvironmentFault, "directory_pin");
        // Reserve owner/assets/commits directories and the transient writer lock
        // along with the complete first append, before creating any of them.
        // Keep this root transaction and its inventory pins until publication;
        // a second scan would merely recount this writer's admitted additions.
        status = globalQuota.account(root, needed, blocks.size() + (writerActivity ? 8 : 7), measured, {});
        if (!status) return status;
        admittedNewOwner = true;
    }
    DirectoryPins pins;
    status = pinDirectories(directory, expected.sequence == 0, pins);
    if (!status) return status;
    Handle lock;
    if (!(status = openWriterLock(directory, lock))) return status;
#else
    return fail(Error::resourceOrEnvironmentFault, "durable_store_requires_windows");
#endif
#if JUCE_WINDOWS
    std::optional<OwnerRetirement> terminal;
    status = readRetirementFile(directory, owner, terminal);
    if (!status) return status;
    if (terminal) return fail(Error::staleHead, "owner_retired");
    const auto current = inspectManifest();
    measured.journalPayloadBytesRead += current.verifiedRecordBytes;
    if (!current.status) return current.status;
    if (current.retirement) return fail(Error::staleHead, "owner_retired");
    if (current.head != expected) return fail(Error::staleHead, "head_changed");
    if (current.commits.size() >= limits.records) return fail(Error::budgetExceeded, "chain_budget");
    if (!current.commits.empty())
    {
        const auto document = current.commits.back().record["document"];
        if (document["lineage"].toString() != packet.identity.document.lineage.toString()
            || document["runtime_instance"].toString() != packet.identity.document.runtimeInstance.toString())
            return fail(Error::staleHead, "document_changed");
    }
    if (writerActivity)
    {
        // Carry the newest observed commit time through checkpoint compaction;
        // a backward clock/Undo must never make a live history look older.
        auto latest = packet.timestampMilliseconds;
        for (const auto& commit : current.commits)
            latest = std::max(latest, std::max(static_cast<int64_t>(commit.record["timestamp"]),
                static_cast<int64_t>(commit.record["activity_protocol"]["latest"])));
        record["activity_protocol"].getDynamicObject()->setProperty("latest", latest);
        recordBytes = bytes(canonical(record));
        if (recordBytes.getSize() > limits.recordBytes) return fail(Error::budgetExceeded, "record_budget");
    }
    size_t requiredReadBytes = current.verifiedRecordBytes + recordBytes.getSize();
    if (current.verifiedRecordBytes + recordBytes.getSize() > limits.journalReadBytes)
        return fail(Error::budgetExceeded, "journal_read_budget");
    std::set<juce::String> oldAssets;
    for (const auto& commit : current.commits)
        for (const auto& id : *commit.record["assets"].getArray()) oldAssets.insert(id.toString());
    const auto assets = directory.getChildFile("assets"), commits = directory.getChildFile("commits");
    const bool emptyOwner = current.head.sequence == 0;
    if (!(status = pinDirectories(assets, emptyOwner, pins)) || !(status = pinDirectories(commits, emptyOwner, pins))) return status;
    // Facts are reusable only from this Store's last successful publication at
    // the actual expected head. A cold/restarted/externally advanced writer hashes
    // every dependency. An inspect or a failed append never admits cache entries.
    const auto* previous = appendCache && appendCache->head == expected ? &appendCache->assets : nullptr;
    AppendAssets closure(assets, previous, measured);
    for (const auto& id : oldAssets)
    {
        if (requiredReadBytes >= limits.readBytes) return fail(Error::budgetExceeded, "read_budget");
        status = closure.verify(id, std::min(limits.assetBytes, limits.readBytes - requiredReadBytes));
        if (!status) return status;
        requiredReadBytes += closure.proofs.at(id).payloadBytes;
    }
    for (const auto& block : blocks)
        if (!oldAssets.contains(block.id)) requiredReadBytes += block.payloadBytes();
    for (const auto& id : packet.existingAudio)
        if (!oldAssets.contains(id) && std::none_of(blocks.begin(), blocks.end(), [&] (const auto& block) { return block.id == id; }))
        {
            if (requiredReadBytes >= limits.readBytes) return fail(Error::budgetExceeded, "read_budget");
            status = closure.verify(id, std::min(limits.assetBytes, limits.readBytes - requiredReadBytes));
            if (!status) return status;
            requiredReadBytes += closure.proofs.at(id).payloadBytes;
            oldAssets.insert(id);
        }
    if (requiredReadBytes > limits.readBytes) return fail(Error::budgetExceeded, "read_budget");
    int64_t used = 0;
    size_t fileCount = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(files::nativeFile(directory), false, "*", juce::File::findFilesAndDirectories))
    {
        const auto file = entry.getFile();
        if (linkedPath(file)) return fail(Error::resourceOrEnvironmentFault, "linked_path");
        if (file.isDirectory())
        {
            if (file.getFileName() != "assets" && file.getFileName() != "commits" && file.getFileName() != "quarantine")
                return fail(Error::corruptJournal, "unexpected_directory");
            for (const auto& child : juce::RangedDirectoryIterator(file, false, "*", juce::File::findFilesAndDirectories))
            {
                ++measured.ownerEntriesScanned;
                if (++fileCount > 4096 || child.isDirectory() || linkedPath(child.getFile()))
                    return fail(Error::budgetExceeded, "owner_entries");
                if (child.getFileSize() < 0) return fail(Error::budgetExceeded, "owner_budget");
                if (child.getFileSize() > limits.ownerBytes - used)
                    return byteCapacityFailure("owner_budget", ByteCapacity::Resource::ownerStorage,
                        static_cast<uint64_t>(used) + child.getFileSize(), limits.ownerBytes, true);
                used += child.getFileSize();
            }
        }
        else
        {
            ++measured.ownerEntriesScanned;
            if (file.getSize() < 0) return fail(Error::budgetExceeded, "owner_budget");
            if (file.getSize() > limits.ownerBytes - used)
                return byteCapacityFailure("owner_budget", ByteCapacity::Resource::ownerStorage,
                    static_cast<uint64_t>(used) + file.getSize(), limits.ownerBytes, true);
            used += file.getSize();
            if (++fileCount > 4096) return fail(Error::budgetExceeded, "owner_entries");
        }
    }
    if (needed > limits.ownerBytes || used > limits.ownerBytes - needed)
        return byteCapacityFailure("owner_budget", ByteCapacity::Resource::ownerStorage,
            static_cast<uint64_t>(used) + needed, limits.ownerBytes);
    const auto free = files::nativeFile(directory).getBytesFreeOnVolume();
    if (free < 0) return fail(Error::budgetExceeded, "free_space_reserve");
    if (needed > free || limits.freeSpaceReserve > free - needed)
        return byteCapacityFailure("free_space_reserve", ByteCapacity::Resource::freeDisk,
            static_cast<uint64_t>(needed) + limits.freeSpaceReserve, free);
    // Reserve final new assets/record/head and one transient atomic staging
    // entry; conservative reuse accounting must never undercount peak growth.
    if (!admittedNewOwner)
    {
        status = globalQuota.account(root, needed, blocks.size() + (writerActivity && !*writerActivity ? 4 : 3), measured,
            { { directory.getChildFile("writer.lock").getFullPathName(), { lock.value, true } } }, directory.getChildFile("head"));
        if (!status) return status;
    }
    if (writerActivity)
    {
        if (*writerActivity && (!(*writerActivity)->belongsTo(root, owner) || !(*writerActivity)->impl->writerActivity))
            return fail(Error::invalidPacket, "writer_activity_scope");
        if (!*writerActivity)
        {
            auto activity = std::shared_ptr<RecoveryOwnerLease>(new RecoveryOwnerLease());
            activity->impl->root = root; activity->impl->owner = owner;
            activity->impl->writerActivity = true;
            if (!(status = pinDirectories(directory, false, activity->impl->pins))
                || !(status = openUsageLock(directory, Usage::writer, activity->impl->usage))) return status;
            // Established while the root/writer transaction is still held and
            // before head publication. No idle gap exposes a live crash owner.
            *writerActivity = std::move(activity);
        }
    }
    for (const auto& block : blocks)
    {
        const auto& id = block.id;
        const auto target = assets.getChildFile(id);
        AudioBlock loaded;
        const auto* audio = block.audio;
        if (block.streamed)
        {
            try { status = block.streamed->read(loaded); }
            catch (...) { return fail(Error::resourceOrEnvironmentFault, "streamed_audio_read_exception"); }
            if (!status) return status;
            juce::MemoryBlock verifiedHeader;
            if (!(status = encodeAudioHeader(loaded, limits.assetBytes, verifiedHeader))) return status;
            if (verifiedHeader != block.header || audioDigest(verifiedHeader, loaded.samples) != id)
                return fail(Error::sourceConflict, "streamed_audio_changed");
            audio = &loaded;
        }
        if (files::nativeFile(target).exists())
        {
            // Both the prepared block and the verified file hash to this exact
            // content-addressed ID. Already leased dependencies need no reread.
            status = closure.verify(id, limits.assetBytes);
            if (!status) return status;
        }
        else
        {
            // The asset ID is the SHA256 of this exact canonical header+PCM:
            // eager blocks were hashed while preparing this append; streamed
            // blocks were regenerated and checked immediately above. SER1 uses
            // the same digest, so do not hash the unchanged local payload again.
            juce::MemoryBlock checksum;
            checksum.loadFromHexString(id);
            if (!(status = publishParts(target, block.header, &audio->samples, false, &checksum))) return status;
        }
#if SINGLE_EDITOR_RECOVERY_TESTING
        if (onBoundary) onBoundary(Boundary::assetPublished);
#endif
    }
    for (const auto& id : ids)
    {
        if (!(status = closure.verify(id, limits.assetBytes))) return status;
    }
    const SafeHead next { expected.sequence + 1, digest(recordBytes) };
    auto nextCache = std::make_shared<AppendCache>();
    nextCache->head = next;
    nextCache->assets = closure.proofs;
    // Allocate before publishing; output receipt survives all failed attempts.
    const auto publication = receipt ? std::shared_ptr<const PublishedRecoveryCommit>(new PublishedRecoveryCommit(
        root, owner, next, packet, ids)) : nullptr;
    const auto recordFile = commits.getChildFile(next.hash);
    if (files::nativeFile(recordFile).exists())
    {
        juce::MemoryBlock existing;
        if (!readFrame(recordFile, limits.recordBytes, existing) || existing != recordBytes)
            return fail(Error::corruptJournal, "existing_record_conflict");
    }
    else if (!(status = publish(recordFile, recordBytes, false))) return status;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onBoundary) onBoundary(Boundary::recordPublished);
    if (onBoundary) onBoundary(Boundary::beforeHeadReplace);
#endif
    const auto head = headPayload(owner, next, current.checkpointCut);
    if (!(status = closure.recheck())) return status;
    if (!(status = publish(directory.getChildFile("head"), head, true))) return status;
    published = next;
    if (receipt) *receipt = publication;
    appendCache = std::move(nextCache);
    // Internal Pilot soft watermarks, derived from the existing hard limits.
    // No higher admission, startup scan or timer is introduced. The callback
    // below is present only for the real complete-checkpoint producer.
    measured.compactionSuggested = current.commits.size() + 1 >= std::min<size_t>(256, std::max<size_t>(2, limits.records / 2))
        || used + needed >= limits.ownerBytes / 4 * 3;
#if SINGLE_EDITOR_RECOVERY_TESTING
    if (onBoundary) onBoundary(Boundary::headPublished);
#endif
#endif // JUCE_WINDOWS
    return {};
}
} // namespace singleeditor::recovery
