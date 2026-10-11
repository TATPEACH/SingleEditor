#include "Recovery/RecoveryAttemptStore.h"
#include "Recovery/RecoveryDurableFiles.h"
#include <iostream>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <cstring>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#endif

using namespace singleeditor::recovery;
namespace
{
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void good(Status status, const char* message)
{
    if (!status) std::cerr << "Attempt status: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
struct Fixture final
{
    juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    juce::File root = parent.getChildFile("SingleEditor-AttemptTest-" + juce::Uuid().toString());
    juce::Uuid owner;
    SafeHead head { 3, juce::String::repeatedString("a", 64) };
    SafeHead checkpoint { 2, juce::String::repeatedString("b", 64) };
    juce::File directory() const { return root.getChildFile(owner.toString()); }
    RecoveryAttemptStore store(AttemptLimits limits = {}) const { return { root, owner, limits }; }
    std::unique_ptr<RecoveryAttemptWriter> begin(const juce::String& build = "test-v1", bool retry = false) const
    {
        std::unique_ptr<RecoveryAttemptWriter> writer;
        good(store().begin(juce::Uuid(), head, checkpoint, build, writer, retry), "begin durable attempt");
        return writer;
    }
    ~Fixture()
    {
        require(root.getParentDirectory() == parent && root.isAChildOf(parent)
            && root.getFileName().startsWith("SingleEditor-AttemptTest-"), "cleanup only owned resolved UUID fixture");
        require(root.deleteRecursively(), "remove owned attempt fixture");
    }
};
AttemptView inspect(const Fixture& fixture)
{
    auto view = fixture.store().inspect(); good(view.status, "inspect bounded marker metadata"); return view;
}
std::unique_ptr<detail::DurableDirectory> directory(const Fixture& fixture)
{
    std::unique_ptr<detail::DurableDirectory> result;
    good(detail::DurableDirectory::open(fixture.directory(), false, result), "open private framed metadata fixture");
    require(result != nullptr, "fixture directory exists"); return result;
}
void closed(const Fixture& fixture, AttemptOutcome outcome, AttemptFailure failure)
{
    const auto view = inspect(fixture);
    require(view.attempts.size() == 1 && view.attempts[0].record.outcome == outcome
        && view.attempts[0].record.failure == failure && view.attempts[0].health == AttemptHealth::healthy,
        "controlled terminal outcome remains healthy metadata");
}
#if JUCE_WINDOWS
struct Handle final { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); } };
void junction(const juce::File& link, const juce::File& target, const juce::File& root)
{
    require(link.isAChildOf(root) && target.isAChildOf(root) && link.createDirectory().wasOk() && target.createDirectory().wasOk(),
        "junction fixture stays in private UUID root");
    const auto substitute = juce::String("\\??\\") + target.getFullPathName();
    const auto display = target.getFullPathName();
    const auto substituteBytes = static_cast<USHORT>(substitute.length() * 2), displayBytes = static_cast<USHORT>(display.length() * 2);
    juce::MemoryBlock buffer(static_cast<size_t>(20 + substituteBytes + displayBytes), true);
    auto* raw = static_cast<unsigned char*>(buffer.getData());
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT length = static_cast<USHORT>(12 + substituteBytes + displayBytes), displayOffset = static_cast<USHORT>(substituteBytes + 2);
    std::memcpy(raw, &tag, 4); std::memcpy(raw + 4, &length, 2);
    std::memcpy(raw + 10, &substituteBytes, 2); std::memcpy(raw + 12, &displayOffset, 2); std::memcpy(raw + 14, &displayBytes, 2);
    std::memcpy(raw + 16, substitute.toWideCharPointer(), substituteBytes);
    std::memcpy(raw + 16 + displayOffset, display.toWideCharPointer(), displayBytes);
    Handle handle;
    handle.value = CreateFileW(link.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle.value != INVALID_HANDLE_VALUE, "open private junction only");
    DWORD returned = 0;
    require(DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, buffer.getData(), static_cast<DWORD>(buffer.getSize()),
        nullptr, 0, &returned, nullptr) != FALSE, "install real private directory reparse point");
}
void crashChild(const Fixture& fixture, RecoveryAttemptStage stage, const juce::String& build = "test-v1")
{
    auto command = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName().quoted()
        + " --crash " + fixture.root.getFullPathName().quoted() + " " + fixture.owner.toString()
        + " " + juce::String(static_cast<int>(stage)) + " " + build;
    std::wstring buffer(command.toWideCharPointer());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
        "launch only owned attempt test subprocess without foreground");
    Handle child, thread; child.value = process.hProcess; thread.value = process.hThread;
    const auto wait = WaitForSingleObject(child.value, 20000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(child.value, 99);
    require(wait == WAIT_OBJECT_0, "owned abrupt-exit child terminates within bound");
    DWORD code = 0; require(GetExitCodeProcess(child.value, &code) != FALSE && code == 77, "child exits at requested durable stage without destructors");
}
#endif
}

namespace
{
int quotaLockChild(const juce::File& root, bool crash)
{
#if JUCE_WINDOWS
    Handle lock;
    lock.value = CreateFileW(root.getChildFile("quota.lock").getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(lock.value != INVALID_HANDLE_VALUE, "child holds actual established root lock");
    require(root.getChildFile("ready").replaceWithText("root exclusion held"), "signal only private attempt quota fixture");
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
    while (!root.getChildFile("release").existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
    require(root.getChildFile("release").existsAsFile(), "release owned root lock child within bound");
    if (crash) TerminateProcess(GetCurrentProcess(), 78);
#else
    juce::ignoreUnused(root, crash);
#endif
    return 0;
}
void quotaTests()
{
    const auto fill = [] (const juce::File& root, int64_t cap, const juce::File& filler)
    {
        int64_t total = 0;
        for (const auto& file : root.findChildFiles(juce::File::findFiles, true)) total += file.getSize();
        require(total < cap && cap - total < 1024 * 1024, "bounded private attempt quota fixture");
        juce::MemoryBlock bytes(static_cast<size_t>(cap - total), true);
        require(filler.replaceWithData(bytes.getData(), bytes.getSize()), "fill owned bytes to exact attempt root ceiling");
    };
    for (int mode = 0; mode < 5; ++mode)
    {
        Fixture fixture; AttemptLimits limits; limits.rootBytes = 16384;
        if (mode == 1) limits.rootEntries = 7;
        std::unique_ptr<RecoveryAttemptWriter> writer;
        good(fixture.store(limits).begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", writer), "begin actual quota-governed attempt");
        const auto original = writer->getRecord();
        const auto marker = fixture.directory().getChildFile(original.nonce.toString() + ".attempt");
        juce::MemoryBlock originalBytes; require(marker.loadFileAsData(originalBytes), "freeze actual published marker bytes before pressure");
        if (mode == 0 || mode == 1 || mode == 2)
        {
            const auto filler = mode == 2 ? fixture.directory().getChildFile("unknown.lease") : fixture.root.getChildFile("unknown-payload");
            if (mode != 1) fill(fixture.root, limits.rootBytes, filler);
            const auto entries = fixture.root.findChildFiles(juce::File::findFilesAndDirectories, true).size();
            auto higher = limits; higher.rootBytes *= 4; higher.rootEntries *= 4;
            for (int attempt = 0; attempt < 24; ++attempt)
            {
                const juce::Uuid owner;
                RecoveryAttemptStore competing(fixture.root, owner, higher);
                std::unique_ptr<RecoveryAttemptWriter> rejected;
                require(competing.begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", rejected).code
                    == (mode == 1 ? "global_entry_budget" : "global_byte_budget") && !rejected
                    && !fixture.root.getChildFile(owner.toString()).exists(),
                    "larger later callers cannot raise persisted policy or create rejected owner infrastructure");
            }
            require(fixture.root.findChildFiles(juce::File::findFilesAndDirectories, true).size() == entries,
                "repeated rejected distinct owners leave no growing empty directories or leases");
            if (mode != 1)
            {
                require(writer->enter(RecoveryAttemptStage::ready).code == "global_byte_budget"
                    && writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "quota_cancel" }).code == "global_byte_budget"
                    && writer->isActive() && writer->getRecord() == original,
                    "stage and terminal writes both reserve replacement staging without faking a closed record");
                juce::MemoryBlock current; require(marker.loadFileAsData(current) && current == originalBytes,
                    "failed capacity admission preserves byte-exact old marker");
                require(inspect(fixture).attempts[0].health == AttemptHealth::active,
                    "actual held readable lease still proves active during failed publication");
                require(filler.deleteFile(), "remove only owned quota filler before exact writer retry");
            }
            good(writer->enter(RecoveryAttemptStage::ready), "same writer advances after actual capacity relief or within reserved entry ceiling");
            good(writer->finish(AttemptOutcome::succeeded), "same nonce reaches real durable terminal publication after retry");
            closed(fixture, AttemptOutcome::succeeded, AttemptFailure::none);
        }
        else if (mode == 3)
        {
#if JUCE_WINDOWS
            const auto lease = fixture.directory().getChildFile(original.nonce.toString() + ".lease");
            auto attacker = CreateFileW(lease.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            require(attacker == INVALID_HANDLE_VALUE, "readable active lease still excludes external payload writers");
            const auto unknown = fixture.root.getChildFile("unknown-writer");
            require(unknown.replaceWithText("private concurrent writer"), "create owned unknown writer fixture");
            attacker = CreateFileW(unknown.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            require(attacker != INVALID_HANDLE_VALUE, "hold actual ungoverned metadata-root writer");
            const auto failed = writer->enter(RecoveryAttemptStage::ready); CloseHandle(attacker);
            require(failed.code == "global_inventory_entry" && writer->getRecord() == original,
                "quota inventory cannot claim stable bytes across actual unknown writer");
            good(writer->enter(RecoveryAttemptStage::ready), "real unknown writer release permits exact stage retry");
            good(writer->finish(AttemptOutcome::succeeded), "terminal close after read-sharing test");
#endif
        }
        else
        {
            std::unique_ptr<detail::DurableDirectory> root;
            good(detail::DurableDirectory::open(fixture.root, false, root), "open known private quota policy fixture");
            juce::var policy; good(root->readJson("quota-policy", 4096, policy), "read actual persisted attempt root policy");
            auto changed = policy.clone(); changed.getDynamicObject()->setProperty("schema", 99);
            good(root->publishJson("quota-policy", 4096, changed, true), "inject checksum-valid future policy only in owned fixture");
            require(writer->enter(RecoveryAttemptStage::ready).error == Error::unsupportedSchema && writer->getRecord() == original,
                "live writer cannot silently replace an unsupported global policy with defaults");
            good(root->publishJson("quota-policy", 4096, policy, true), "restore only injected private policy for retry");
            good(writer->enter(RecoveryAttemptStage::ready), "retry actual stage after policy restoration");
            good(writer->finish(AttemptOutcome::succeeded), "close actual writer after restored policy");
        }
    }
    for (int mode = 0; mode < 2; ++mode)
    {
        Fixture fixture; AttemptLimits limits;
        if (mode == 0) limits.rootBytes = 1;
        else limits.freeSpaceReserve = INT64_MAX;
        std::unique_ptr<RecoveryAttemptWriter> writer;
        const auto result = fixture.store(limits).begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", writer);
        require(result.code == (mode == 0 ? "global_byte_budget" : "free_space_reserve")
            && !writer && !fixture.directory().exists() && !fixture.root.getChildFile("quota-policy").exists(),
            "unaffordable initial record/policy or reserve fails before owner and lease creation");
    }
    {
        Fixture fixture;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto writer = fixture.begin("quota-stale-leases", true);
            const auto lease = fixture.directory().getChildFile(writer->getRecord().nonce.toString() + ".lease");
            writer.reset();
            require(lease.create().wasOk(), "emulate only owned persisted empty lease after actual writer abandonment");
        }
        require(inspect(fixture).attempts[0].health == AttemptHealth::quarantined,
            "unheld stale lease is not actual running authority");
        std::unique_ptr<RecoveryAttemptWriter> rejected;
        require(fixture.store().begin(juce::Uuid(), fixture.head, fixture.checkpoint, "quota-stale-leases", rejected).code
            == "attempt_quarantined_retry_required" && !rejected,
            "budget inventory read handles cannot impersonate active attempts and bypass interruption quarantine");
    }
    for (int crash = 0; crash < 2; ++crash)
    {
        Fixture fixture; auto writer = fixture.begin(); const auto original = writer->getRecord();
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--quota-lock-child", fixture.root.getFullPathName(), juce::String(crash) }), "start actual separate root-lock process");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
        while (!fixture.root.getChildFile("ready").existsAsFile() && child.isRunning() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
        require(fixture.root.getChildFile("ready").existsAsFile() && child.isRunning(), "second process actually holds established root lock");
        std::unique_ptr<RecoveryAttemptWriter> blocked;
        require(fixture.store().begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", blocked).code == "global_budget_busy"
            && writer->enter(RecoveryAttemptStage::ready).code == "global_budget_busy"
            && writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "quota_busy" }).code == "global_budget_busy"
            && !blocked && writer->getRecord() == original && writer->isActive(),
            "begin stage and finish all respect a real competing process while preserving old live marker");
        require(fixture.root.getChildFile("release").replaceWithText("release owned process"), "release only private quota-lock fixture");
        require(child.waitForProcessToFinish(10000) && child.getExitCode() == (crash ? 78u : 0u),
            "real process exits or dies abruptly without logical root-lock recovery");
        good(writer->enter(RecoveryAttemptStage::ready), "same attempt retries after actual cross-process lock release");
        good(writer->finish(AttemptOutcome::succeeded), "same attempt closes after root-lock crash or normal release");
    }
    std::cout << "Recovery attempt quota tests passed (aggregate root policy, bounded rejected infrastructure, stage/finish retry, real leases, unknown bytes and process exclusion/crash)\n";
}
}

int main(int argc, char** argv)
{
    if (argc == 4 && juce::String(argv[1]) == "--quota-lock-child")
        return quotaLockChild(juce::File(juce::String(argv[2])), juce::String(argv[3]).getIntValue() != 0);
    if (argc == 2 && juce::String(argv[1]) == "--quota-only") { quotaTests(); return 0; }
#if JUCE_WINDOWS
    if (argc == 6 && juce::String(argv[1]) == "--crash")
    {
        RecoveryAttemptStore store(juce::File(juce::String::fromUTF8(argv[2])), juce::Uuid(argv[3]));
        std::unique_ptr<RecoveryAttemptWriter> writer;
        good(store.begin(juce::Uuid(), { 3, juce::String::repeatedString("a", 64) },
            { 2, juce::String::repeatedString("b", 64) }, argv[5], writer), "child flush initial marker");
        good(writer->enter(static_cast<RecoveryAttemptStage>(juce::String(argv[4]).getIntValue())), "child flush stage before abrupt process termination");
        TerminateProcess(GetCurrentProcess(), 77);
        return 98;
    }
#else
    juce::ignoreUnused(argc, argv);
#endif
    {
        Fixture fixture;
        auto store = fixture.store();
        require(!fixture.root.exists(), "construction is inert");
        const auto empty = inspect(fixture);
        require(empty.attempts.empty() && !fixture.root.exists(), "read-only absent inspection creates nothing");
        std::unique_ptr<RecoveryAttemptWriter> output;
        require(store.begin(juce::Uuid(), fixture.head, fixture.checkpoint, "", output).error == Error::invalidPacket
            && !output && !fixture.root.exists(), "invalid request is rejected before any write");
        auto writer = fixture.begin();
        const auto initial = writer->getRecord();
        require(initial.owner == fixture.owner && initial.ownerHead == fixture.head && initial.checkpoint == fixture.checkpoint
            && !initial.nonce.isNull() && !initial.candidate.isNull() && initial.revision == 1 && initial.processStartedTicks > 0
            && initial.startedMilliseconds > 0 && initial.stage == RecoveryAttemptStage::validating, "marker carries durable attempt identity and frozen checkpoint");
        const auto live = inspect(fixture);
        require(live.attempts.size() == 1 && live.attempts[0].record == initial && live.attempts[0].health == AttemptHealth::active,
            "separate inspector reads flushed exact record and held live lease");
        good(writer->enter(RecoveryAttemptStage::validating), "same stage is idempotent");
        require(writer->getRecord().revision == 1, "same-stage CAS chunks do not rewrite markers");
        good(writer->enter(RecoveryAttemptStage::preparingCore), "advance durable stage");
        require(!writer->enter(RecoveryAttemptStage::validating) && writer->getRecord().revision == 2, "backward stage rejected without publication");
        require(!writer->finish(AttemptOutcome::succeeded), "success cannot precede complete ready stage");
        good(writer->enter(RecoveryAttemptStage::ready), "record full ready stage");
        good(writer->finish(AttemptOutcome::succeeded), "durably close successful attempt");
        good(writer->finish(AttemptOutcome::succeeded), "same terminal result idempotent");
        require(!writer->isActive() && !writer->enter(RecoveryAttemptStage::ready), "terminal writer cannot resume");
        closed(fixture, AttemptOutcome::succeeded, AttemptFailure::none);
    }
    for (const auto& pair : { std::pair { Error::corruptJournal, AttemptFailure::corruptJournal },
            { Error::missingOrCorruptAsset, AttemptFailure::missingOrCorruptAsset },
            { Error::unsupportedSchema, AttemptFailure::unsupportedSchema },
            { Error::sourceConflict, AttemptFailure::sourceConflict },
            { Error::staleHead, AttemptFailure::sourceConflict },
            { Error::budgetExceeded, AttemptFailure::resourceOrEnvironmentFault } })
    {
        Fixture fixture; auto writer = fixture.begin();
        good(writer->finish(AttemptOutcome::failed, { pair.first, "typed_failure" }), "close controlled typed failure");
        closed(fixture, AttemptOutcome::failed, pair.second);
    }
    {
        Fixture fixture; auto writer = fixture.begin();
        good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "user_cancelled" }), "close explicit cancellation");
        closed(fixture, AttemptOutcome::cancelled, AttemptFailure::none);
        require(!writer->finish(AttemptOutcome::succeeded), "conflicting terminal result rejected");
    }
    {
        Fixture fixture; auto writer = fixture.begin(); const auto nonce = writer->getRecord().nonce;
        writer.reset();
        auto view = inspect(fixture);
        require(view.attempts.size() == 1 && view.attempts[0].health == AttemptHealth::suspect
            && view.attempts[0].record.outcome == AttemptOutcome::active && view.attempts[0].record.nonce == nonce,
            "abandoned attempt is unclosed even while host process remains alive");
        auto next = fixture.begin(); require(next->getRecord().nonce != nonce, "explicit new attempt has fresh nonce");
        next.reset();
        view = inspect(fixture);
        require(view.attempts.size() == 2 && view.attempts[0].health == AttemptHealth::quarantined
            && view.attempts[1].matchingInterruptions == 2, "matching repeated interruptions quarantine checkpoint/build/stage");
        std::unique_ptr<RecoveryAttemptWriter> output;
        require(fixture.store().begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", output).code == "attempt_quarantined_retry_required"
            && !output && inspect(fixture).attempts.size() == 2, "quarantine blocks implicit retry without rewriting history");
        auto retry = fixture.begin("test-v1", true);
        good(retry->finish(AttemptOutcome::cancelled, { Error::cancelled, "retry_cancelled" }), "explicit Retry Once closes independently");
        require(fixture.store().begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", output).code == "attempt_quarantined_retry_required",
            "Retry Once does not persist a quarantine bypass");
        auto differentBuild = fixture.begin("test-v2");
        good(differentBuild->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "other build has independent health");
        SafeHead earlier { 1, juce::String::repeatedString("c", 64) };
        good(fixture.store().begin(juce::Uuid(), fixture.head, earlier, "test-v1", output), "other safe checkpoint remains available");
        good(output->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "close earlier safe checkpoint attempt");
    }
#if JUCE_WINDOWS
    {
        Fixture fixture;
        const auto link = fixture.root.getChildFile("link"), target = fixture.root.getChildFile("target");
        junction(link, target, fixture.root);
        RecoveryAttemptStore linked(link, fixture.owner);
        std::unique_ptr<RecoveryAttemptWriter> output;
        require(!linked.inspect().status && !linked.begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", output)
            && !output && !target.getChildFile(fixture.owner.toString()).exists(), "marker operations reject actual ancestor junction without target writes");
        require(RemoveDirectoryW(link.getFullPathName().toWideCharPointer()) != FALSE, "remove junction itself without traversing target");
    }
    {
        Fixture fixture; auto writer = fixture.begin();
        const auto moved = fixture.directory().getSiblingFile("moved-owner");
        require(MoveFileExW(fixture.directory().getFullPathName().toWideCharPointer(), moved.getFullPathName().toWideCharPointer(), 0) == FALSE,
            "live directory pins prevent actual owner-directory swap");
        good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "close pinned directory fixture");
        require(MoveFileExW(fixture.directory().getFullPathName().toWideCharPointer(), moved.getFullPathName().toWideCharPointer(), 0) != FALSE,
            "closed writer releases owner-directory pins");
        require(MoveFileExW(moved.getFullPathName().toWideCharPointer(), fixture.directory().getFullPathName().toWideCharPointer(), 0) != FALSE,
            "restore only owned moved directory");
    }
    {
        Fixture fixture;
        crashChild(fixture, RecoveryAttemptStage::replayingCheckpoint);
        require(inspect(fixture).attempts[0].health == AttemptHealth::suspect, "actual dead-process marker is suspect on restart");
        crashChild(fixture, RecoveryAttemptStage::preparingCore);
        auto view = inspect(fixture);
        require(view.attempts.size() == 2 && view.attempts[0].health == AttemptHealth::suspect
            && view.attempts[1].health == AttemptHealth::suspect, "distinct crash stages do not merge quarantine counts");
        crashChild(fixture, RecoveryAttemptStage::replayingCheckpoint);
        view = inspect(fixture);
        int quarantined = 0;
        for (const auto& item : view.attempts) if (item.health == AttemptHealth::quarantined) ++quarantined;
        require(quarantined == 2 && view.attempts.size() == 3, "actual repeated process crashes quarantine only matching stage");
    }
    {
        Fixture fixture; auto writer = fixture.begin();
        const auto marker = fixture.directory().getChildFile(writer->getRecord().nonce.toString() + ".attempt");
        {
            Handle held;
            held.value = CreateFileW(marker.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(held.value != INVALID_HANDLE_VALUE, "hold only private marker against replacement");
            require(!writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }) && writer->isActive()
                && writer->getRecord().revision == 1, "terminal publication failure cannot claim closure");
        }
        require(inspect(fixture).attempts[0].record.outcome == AttemptOutcome::active, "failed terminal publication retains previous complete frame");
        writer.reset();
        require(inspect(fixture).attempts[0].health == AttemptHealth::suspect, "failed terminal write followed by teardown stays visibly unclosed");
    }
    {
        Fixture fixture;
        std::atomic<int> ready { 0 }, admitted { 0 };
        auto attempt = [&]
        {
            ++ready; while (ready.load() != 2) std::this_thread::yield();
            std::unique_ptr<RecoveryAttemptWriter> writer;
            const auto result = fixture.store({ 1, 4096, 2 }).begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", writer);
            if (result) { ++admitted; good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "close admitted concurrent attempt"); }
            else require(result.error == Error::budgetExceeded || result.error == Error::resourceOrEnvironmentFault, "concurrent admission returns explicit bounded fault");
        };
        std::thread first(attempt), second(attempt); first.join(); second.join();
        require(admitted == 1 && inspect(fixture).attempts.size() == 1, "concurrent starters cannot exceed one-entry quota");
    }
#endif
    {
        Fixture fixture;
        std::vector<std::unique_ptr<RecoveryAttemptWriter>> writers;
        for (int i = 0; i < 24; ++i) writers.push_back(fixture.begin());
        std::atomic<bool> inspecting { false };
        std::thread closing([&]
        {
            while (!inspecting.load()) std::this_thread::yield();
            for (auto& writer : writers)
                good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "close real attempts while inspection is reading");
        });
        inspecting.store(true);
        for (int pass = 0; pass < 32; ++pass)
            for (const auto& item : inspect(fixture).attempts)
                require(item.health == AttemptHealth::healthy || item.health == AttemptHealth::active,
                    "concurrent normal terminal publication never creates false crash/quarantine");
        closing.join();
        for (const auto& item : inspect(fixture).attempts)
            require(item.record.outcome == AttemptOutcome::cancelled && item.health == AttemptHealth::healthy,
                "all actual concurrent closes are durably terminal");
    }
    for (int fault = 0; fault < 5; ++fault)
    {
        Fixture fixture; auto writer = fixture.begin();
        const auto name = writer->getRecord().nonce.toString() + ".attempt";
        auto metadata = directory(fixture); juce::var value;
        good(metadata->readJson(name, 4096, value), "read private fault-injection frame");
        if (fault == 0) value.getDynamicObject()->setProperty("owner", juce::Uuid().toString());
        if (fault == 1) value.getDynamicObject()->setProperty("schema", 2);
        if (fault == 2) value.getDynamicObject()->setProperty("nonce", juce::Uuid().toString());
        if (fault == 3) value.getDynamicObject()->setProperty("revision", 99);
        if (fault < 4) good(metadata->publishJson(name, 4096, value, true), "publish checksum-valid semantic fault");
        else require(fixture.directory().getChildFile(name).replaceWithText("SER1-torn"), "inject actual torn private marker");
        const auto changed = writer->enter(RecoveryAttemptStage::replayingCheckpoint);
        require(!changed && writer->getRecord().revision == 1, "changed/malformed marker cannot be overwritten by stale writer");
        if (fault == 3) require(changed.error == Error::staleHead, "checksum-valid revision mismatch is a stale fence");
        else
        {
            const auto view = fixture.store().inspect();
            require(!view.status && view.attempts.empty(), "invalid metadata gives no usable partial inspection");
            if (fault == 1) require(view.status.error == Error::unsupportedSchema, "future schema stays typed unsupported");
        }
    }
    {
        Fixture fixture; auto writer = fixture.begin();
        good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "cancelled" }), "close first quota fixture");
        std::unique_ptr<RecoveryAttemptWriter> output;
        require(fixture.store({ 1, 4096, 2 }).begin(juce::Uuid(), fixture.head, fixture.checkpoint, "test-v1", output).error == Error::budgetExceeded
            && !output, "full history fails closed without deleting protected records");
        require(fixture.directory().getChildFile("unexpected").replaceWithText("owned"), "create private unexpected marker entry");
        require(!fixture.store().inspect().status, "unexpected directory content rejected");
    }
    quotaTests();
    std::cout << "RecoveryAttemptStoreTests passed (durable lifecycle, typed faults, real process crashes, quarantine, concurrent admission, torn/stale records)\n";
    return 0;
}
