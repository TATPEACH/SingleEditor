#include "RecoveryAttemptStore.h"
#include "Core/NativeFilePath.h"
#include "RecoveryDurableFiles.h"
#include <algorithm>
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
bool hex(const juce::String& value, int size) { return value.length() == size && value.containsOnly("0123456789abcdef"); }
bool label(const juce::String& value) { return value.isNotEmpty() && value.length() <= 96 && value.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"); }
bool number(const juce::var& value, int64_t low, int64_t high = INT64_MAX)
{ return (value.isInt() || value.isInt64()) && static_cast<int64_t>(value) >= low && static_cast<int64_t>(value) <= high; }
bool validHead(const SafeHead& head) { return head.sequence > 0 && hex(head.hash, 64); }
bool validLimits(const AttemptLimits& limits)
{
    return limits.entries > 0 && limits.entries <= 1024 && limits.recordBytes >= 2048 && limits.recordBytes <= 16384
        && limits.quarantineAfter >= 2 && limits.quarantineAfter <= 10
        && limits.rootBytes > 0 && limits.rootBytes <= 1024LL * 1024 * 1024 * 1024
        && limits.rootEntries > 0 && limits.rootEntries <= 1024 * 1024 && limits.freeSpaceReserve >= 0;
}
Limits storageLimits(const AttemptLimits& limits)
{
    Limits storage;
    storage.globalBytes = limits.rootBytes; storage.globalEntries = limits.rootEntries;
    storage.freeSpaceReserve = limits.freeSpaceReserve;
    return storage;
}
AttemptFailure classify(Error error)
{
    switch (error)
    {
        case Error::none: case Error::cancelled: return AttemptFailure::none;
        case Error::invalidPacket: case Error::corruptJournal: return AttemptFailure::corruptJournal;
        case Error::missingOrCorruptAsset: return AttemptFailure::missingOrCorruptAsset;
        case Error::unsupportedSchema: return AttemptFailure::unsupportedSchema;
        case Error::staleHead: case Error::sourceConflict: return AttemptFailure::sourceConflict;
        case Error::resourceOrEnvironmentFault: case Error::budgetExceeded: return AttemptFailure::resourceOrEnvironmentFault;
    }
    return AttemptFailure::unknown;
}
juce::var json(const AttemptRecord& record)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("schema", 1); object->setProperty("kind", "recovery-attempt");
    object->setProperty("owner", record.owner.toString()); object->setProperty("candidate", record.candidate.toString());
    object->setProperty("nonce", record.nonce.toString()); object->setProperty("owner_sequence", record.ownerHead.sequence);
    object->setProperty("owner_hash", record.ownerHead.hash); object->setProperty("checkpoint_sequence", record.checkpoint.sequence);
    object->setProperty("checkpoint_hash", record.checkpoint.hash); object->setProperty("app_build", record.appBuild);
    object->setProperty("pid", record.processId); object->setProperty("process_started_ticks", record.processStartedTicks);
    object->setProperty("started_ms", record.startedMilliseconds); object->setProperty("updated_ms", record.updatedMilliseconds);
    object->setProperty("revision", record.revision); object->setProperty("stage", static_cast<int>(record.stage));
    object->setProperty("outcome", static_cast<int>(record.outcome)); object->setProperty("failure", static_cast<int>(record.failure));
    object->setProperty("error_code", record.errorCode);
    return juce::var(object);
}
Status parse(const juce::var& value, const juce::Uuid& owner, const juce::String& name, AttemptRecord& output)
{
    if (!value.isObject() || !number(value["schema"], 0)) return { Error::corruptJournal, "attempt_shape" };
    if (!number(value["schema"], 1, 1)) return { Error::unsupportedSchema, "attempt_schema" };
    if (value.getDynamicObject()->getProperties().size() != 19 || value["kind"] != juce::var("recovery-attempt"))
        return { Error::corruptJournal, "attempt_shape" };
    for (const auto* key : { "owner", "candidate", "nonce" })
        if (!value[key].isString() || !hex(value[key].toString(), 32) || juce::Uuid(value[key].toString()).isNull())
            return { Error::corruptJournal, "attempt_identity" };
    if (value["owner"] != juce::var(owner.toString()) || name != value["nonce"].toString() + ".attempt"
        || !value["app_build"].isString() || !label(value["app_build"].toString())
        || !number(value["owner_sequence"], 1) || !number(value["checkpoint_sequence"], 1)
        || !value["owner_hash"].isString() || !hex(value["owner_hash"].toString(), 64)
        || !value["checkpoint_hash"].isString() || !hex(value["checkpoint_hash"].toString(), 64)
        || !number(value["pid"], 1, UINT32_MAX) || !number(value["process_started_ticks"], 1)
        || !number(value["started_ms"], 1) || !number(value["updated_ms"], static_cast<int64_t>(value["started_ms"]))
        || !number(value["revision"], 1, 1000000) || !number(value["stage"], 0, static_cast<int>(RecoveryAttemptStage::ready))
        || !number(value["outcome"], 0, 3) || !number(value["failure"], 0, 7)
        || !value["error_code"].isString()) return { Error::corruptJournal, "attempt_contract" };
    AttemptRecord record;
    record.owner = owner; record.candidate = juce::Uuid(value["candidate"].toString()); record.nonce = juce::Uuid(value["nonce"].toString());
    record.ownerHead = { static_cast<int64_t>(value["owner_sequence"]), value["owner_hash"].toString() };
    record.checkpoint = { static_cast<int64_t>(value["checkpoint_sequence"]), value["checkpoint_hash"].toString() };
    if (record.checkpoint.sequence > record.ownerHead.sequence
        || (record.checkpoint.sequence == record.ownerHead.sequence && record.checkpoint.hash != record.ownerHead.hash))
        return { Error::corruptJournal, "attempt_checkpoint_contract" };
    record.appBuild = value["app_build"].toString(); record.processId = static_cast<int64_t>(value["pid"]);
    record.processStartedTicks = static_cast<int64_t>(value["process_started_ticks"]);
    record.startedMilliseconds = static_cast<int64_t>(value["started_ms"]); record.updatedMilliseconds = static_cast<int64_t>(value["updated_ms"]);
    record.revision = static_cast<int64_t>(value["revision"]); record.stage = static_cast<RecoveryAttemptStage>(static_cast<int>(value["stage"]));
    record.outcome = static_cast<AttemptOutcome>(static_cast<int>(value["outcome"]));
    record.failure = static_cast<AttemptFailure>(static_cast<int>(value["failure"])); record.errorCode = value["error_code"].toString();
    const auto closedError = record.outcome == AttemptOutcome::failed || record.outcome == AttemptOutcome::cancelled;
    if ((closedError ? !label(record.errorCode) : record.errorCode.isNotEmpty())
        || (record.outcome == AttemptOutcome::failed ? record.failure == AttemptFailure::none : record.failure != AttemptFailure::none)
        || (record.outcome == AttemptOutcome::succeeded && record.stage != RecoveryAttemptStage::ready))
        return { Error::corruptJournal, "attempt_outcome_contract" };
    output = std::move(record); return {};
}
#if JUCE_WINDOWS
struct Handle final
{
    HANDLE value = INVALID_HANDLE_VALUE;
    bool byteLocked = false;
    ~Handle()
    {
        if (value == INVALID_HANDLE_VALUE || value == nullptr) return;
        if (byteLocked) { OVERLAPPED offset{}; UnlockFileEx(value, 0, 1, 0, &offset); }
        CloseHandle(value);
    }
};
int64_t ticks(const FILETIME& value) { return static_cast<int64_t>((static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime); }
int64_t processStarted(HANDLE process)
{
    FILETIME created{}, ended{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &ended, &kernel, &user) ? ticks(created) : 0;
}
Status isRunning(const AttemptRecord& record, const juce::File& directory, bool& running)
{
    running = false;
    Handle process; process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(record.processId));
    if (process.value == nullptr)
        return GetLastError() == ERROR_INVALID_PARAMETER ? Status{} : Status { Error::resourceOrEnvironmentFault, "attempt_process_access" };
    const auto started = processStarted(process.value);
    if (started == 0) return { Error::resourceOrEnvironmentFault, "attempt_process_time" };
    if (started != record.processStartedTicks) return {};
    const auto wait = WaitForSingleObject(process.value, 0);
    if (wait == WAIT_OBJECT_0) return {};
    if (wait != WAIT_TIMEOUT) return { Error::resourceOrEnvironmentFault, "attempt_process_wait" };
    Handle lease;
    lease.value = CreateFileW(files::nativePath(directory.getChildFile(record.nonce.toString() + ".lease")).toWideCharPointer(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lease.value != INVALID_HANDLE_VALUE)
    {
        // A capacity inventory may hold read handles on empty leases. Only the
        // attempt owner's byte-range lock proves liveness; an observer handle
        // must not turn an abandoned marker into an active/quarantine exemption.
        OVERLAPPED offset{};
        if (LockFileEx(lease.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset))
        { lease.byteLocked = true; return {}; }
        if (GetLastError() == ERROR_LOCK_VIOLATION) { running = true; return {}; }
        return { Error::resourceOrEnvironmentFault, "attempt_lease_probe" };
    }
    const auto error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION) { running = true; return {}; } // Older exclusive-lease writers.
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
    return { Error::resourceOrEnvironmentFault, "attempt_lease_access" };
}
#endif
}

struct RecoveryAttemptWriter::Impl final
{
    std::unique_ptr<detail::DurableDirectory> directory;
    juce::File root;
    Limits storage;
    size_t recordBytes = 0;
#if JUCE_WINDOWS
    Handle lease;
#endif
};
RecoveryAttemptWriter::RecoveryAttemptWriter() : impl(std::make_unique<Impl>()) {}
RecoveryAttemptWriter::~RecoveryAttemptWriter() = default;
bool RecoveryAttemptWriter::isActive() const noexcept { return impl != nullptr && record.outcome == AttemptOutcome::active; }
Status RecoveryAttemptWriter::publishNext(AttemptRecord next) try
{
    if (!isActive()) return { Error::invalidPacket, "attempt_already_closed" };
    std::unique_ptr<detail::DurableMutation> transaction;
    auto status = detail::DurableMutation::open(impl->root, impl->storage, transaction);
    if (!status) return status;
    const auto name = record.nonce.toString() + ".attempt";
    juce::var currentJson; status = impl->directory->readJson(name, impl->recordBytes, currentJson);
    if (!status) return status;
    AttemptRecord current;
    status = parse(currentJson, record.owner, name, current);
    if (!status) return status;
    if (current != record) return { Error::staleHead, "attempt_record_changed" };
    next.revision = record.revision + 1;
    if (next.revision > 1000000) return { Error::budgetExceeded, "attempt_revision_budget" };
    next.updatedMilliseconds = std::max(record.updatedMilliseconds, juce::Time::currentTimeMillis());
    status = transaction->publishJson(*impl->directory, name, impl->recordBytes, json(next), true);
    if (status) record = std::move(next);
    return status;
}
catch (...) { return { Error::resourceOrEnvironmentFault, "attempt_update_exception" }; }
Status RecoveryAttemptWriter::enter(RecoveryAttemptStage stage)
{
    if (!isActive() || stage < record.stage || stage > RecoveryAttemptStage::ready) return { Error::invalidPacket, "attempt_stage_transition" };
    if (stage == record.stage) return {};
    auto next = record; next.stage = stage;
    return publishNext(std::move(next));
}
Status RecoveryAttemptWriter::finish(AttemptOutcome outcome, Status cause)
{
    if (outcome <= AttemptOutcome::active || outcome > AttemptOutcome::cancelled
        || (outcome == AttemptOutcome::succeeded && (!cause || cause.code.isNotEmpty() || record.stage != RecoveryAttemptStage::ready))
        || (outcome == AttemptOutcome::failed && (cause || cause.error == Error::cancelled || !label(cause.code)))
        || (outcome == AttemptOutcome::cancelled && (cause.error != Error::cancelled || !label(cause.code))))
        return { Error::invalidPacket, "attempt_finish_contract" };
    if (!isActive())
        return record.outcome == outcome && record.failure == classify(cause.error) && record.errorCode == cause.code
            ? Status{} : Status { Error::invalidPacket, "attempt_already_closed" };
    auto next = record; next.outcome = outcome; next.failure = classify(cause.error); next.errorCode = cause.code;
    const auto status = publishNext(std::move(next));
    if (status) impl.reset(); // Only after terminal publication; releases lease/pins.
    return status;
}

RecoveryAttemptStore::RecoveryAttemptStore(juce::File directory, juce::Uuid identity, AttemptLimits bounds)
    : root(std::move(directory)), owner(identity), limits(bounds) {}
AttemptView RecoveryAttemptStore::inspect(std::function<bool()> cancelled) const try
{
    AttemptView view;
    const auto fail = [&] (Status status) { view.status = std::move(status); view.attempts.clear(); return view; };
    if (cancelled && cancelled()) return fail({ Error::cancelled, "attempt_inspect_cancelled" });
    if (owner.isNull() || !validLimits(limits) || root == juce::File()) return fail({ Error::invalidPacket, "attempt_store_contract" });
    std::unique_ptr<detail::DurableDirectory> directory;
    auto status = detail::DurableDirectory::open(root.getChildFile(owner.toString()), false, directory);
    if (!status || !directory) return fail(status);
    std::vector<juce::String> names;
    status = directory->listNames(limits.entries * 2 + 16, names);
    if (!status) return fail(status);
    for (const auto& name : names)
    {
        if (cancelled && cancelled()) return fail({ Error::cancelled, "attempt_inspect_cancelled" });
        if (name.startsWith("staging-") || name.endsWith(".lease")) continue;
        if (!name.endsWith(".attempt") || !hex(name.dropLastCharacters(8), 32)) return fail({ Error::corruptJournal, "attempt_entry_name" });
        if (view.attempts.size() >= limits.entries) return fail({ Error::budgetExceeded, "attempt_history_budget" });
        juce::var value;
        status = directory->readJson(name, limits.recordBytes, value);
        if (!status) return fail(status);
        AttemptInspection item;
        status = parse(value, owner, name, item.record);
        if (!status) return fail(status);
        if (item.record.outcome == AttemptOutcome::active)
        {
#if JUCE_WINDOWS
            bool running = false;
            status = isRunning(item.record, directory->path(), running);
            if (!status) return fail(status);
            if (!running)
            {
                // Terminal publication precedes lease release. If the first
                // read overlapped finish(), re-read AFTER observing release so
                // a normal close cannot become a spurious crash/quarantine.
                status = directory->readJson(name, limits.recordBytes, value);
                if (!status) return fail(status);
                status = parse(value, owner, name, item.record);
                if (!status) return fail(status);
            }
            if (item.record.outcome == AttemptOutcome::active)
                item.health = running ? AttemptHealth::active : AttemptHealth::suspect;
#else
            return fail({ Error::resourceOrEnvironmentFault, "attempt_platform_unsupported" });
#endif
        }
        view.attempts.push_back(std::move(item));
    }
    for (auto& item : view.attempts)
    {
        if (cancelled && cancelled()) return fail({ Error::cancelled, "attempt_inspect_cancelled" });
        if (item.health != AttemptHealth::suspect) continue;
        for (const auto& other : view.attempts)
            if ((other.health == AttemptHealth::suspect || other.health == AttemptHealth::quarantined)
                && other.record.checkpoint == item.record.checkpoint && other.record.appBuild == item.record.appBuild
                && other.record.stage == item.record.stage) ++item.matchingInterruptions;
        if (item.matchingInterruptions >= limits.quarantineAfter) item.health = AttemptHealth::quarantined;
    }
    return view;
}
catch (...) { return { { Error::resourceOrEnvironmentFault, "attempt_inspect_exception" }, {} }; }

Status RecoveryAttemptStore::begin(juce::Uuid candidate, const SafeHead& ownerHead, const SafeHead& checkpoint,
    const juce::String& build, std::unique_ptr<RecoveryAttemptWriter>& output, bool explicitRetryOnce) const try
{
    if (candidate.isNull() || owner.isNull() || root == juce::File() || !validLimits(limits) || !label(build)
        || !validHead(ownerHead) || !validHead(checkpoint) || checkpoint.sequence > ownerHead.sequence
        || (checkpoint.sequence == ownerHead.sequence && checkpoint.hash != ownerHead.hash)) return { Error::invalidPacket, "attempt_begin_contract" };
#if JUCE_WINDOWS
    // Root exclusion precedes directory/admission ownership. Reserve the maximum
    // record and bounded owner/lease/staging entries before creating them, so
    // repeated rejected owners cannot grow an unbounded empty directory tree.
    std::unique_ptr<detail::DurableMutation> transaction;
    const auto storage = storageLimits(limits);
    auto status = detail::DurableMutation::open(root, storage, transaction);
    if (!status) return status;
    if (!(status = transaction->reserve(limits.recordBytes + 40, 5))) return status;
    std::unique_ptr<detail::DurableDirectory> directory;
    status = detail::DurableDirectory::open(root.getChildFile(owner.toString()), true, directory);
    if (!status) return status;
    Handle admission;
    admission.value = CreateFileW(files::nativePath(directory->path().getChildFile("admission.lease")).toWideCharPointer(),
        GENERIC_READ, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (admission.value == INVALID_HANDLE_VALUE) return { Error::resourceOrEnvironmentFault, "attempt_admission_busy_or_unavailable" };
    const auto prior = inspect();
    if (!prior.status) return prior.status;
    if (prior.attempts.size() >= limits.entries) return { Error::budgetExceeded, "attempt_history_budget" };
    for (const auto& item : prior.attempts)
        if (!explicitRetryOnce && item.health == AttemptHealth::quarantined && item.record.checkpoint == checkpoint && item.record.appBuild == build)
            return { Error::resourceOrEnvironmentFault, "attempt_quarantined_retry_required" };
    auto writer = std::unique_ptr<RecoveryAttemptWriter>(new RecoveryAttemptWriter());
    auto& record = writer->record;
    record.owner = owner; record.candidate = candidate; record.nonce = juce::Uuid();
    record.ownerHead = ownerHead; record.checkpoint = checkpoint; record.appBuild = build;
    record.processId = GetCurrentProcessId(); record.processStartedTicks = processStarted(GetCurrentProcess());
    if (record.processStartedTicks <= 0) return { Error::resourceOrEnvironmentFault, "attempt_process_identity" };
    record.startedMilliseconds = record.updatedMilliseconds = juce::Time::currentTimeMillis();
    writer->impl->recordBytes = limits.recordBytes;
    writer->impl->root = root; writer->impl->storage = storage;
    writer->impl->directory = std::move(directory);
    const auto leaseFile = writer->impl->directory->path().getChildFile(record.nonce.toString() + ".lease");
    writer->impl->lease.value = CreateFileW(files::nativePath(leaseFile).toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (writer->impl->lease.value == INVALID_HANDLE_VALUE) return { Error::resourceOrEnvironmentFault, "attempt_lease_create" };
    OVERLAPPED offset{};
    if (!LockFileEx(writer->impl->lease.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset))
        return { Error::resourceOrEnvironmentFault, "attempt_lease_lock" };
    writer->impl->lease.byteLocked = true;
    status = transaction->publishJson(*writer->impl->directory, record.nonce.toString() + ".attempt", limits.recordBytes, json(record), false);
    if (!status) return status;
    output = std::move(writer); return {};
#else
    juce::ignoreUnused(output, explicitRetryOnce);
    return { Error::resourceOrEnvironmentFault, "attempt_platform_unsupported" };
#endif
}
catch (...) { return { Error::resourceOrEnvironmentFault, "attempt_begin_exception" }; }
}
