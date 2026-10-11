#include "Recovery/RecoveryOwnerLifecycle.h"
#include "Recovery/RecoveryMutationProducer.h"
#include "Recovery/RecoveryProtectionSession.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include "Recovery/RecoveryDurableFiles.h"
#include "Recovery/RecoveryMaintenanceQueue.h"
#include "Recovery/RecoveryStorageLayout.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <array>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Retirement: " << status.code << '\n'; require(static_cast<bool>(status), message); }
void makeSource(const juce::File& source)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create independent retirement audio fixture");
    juce::AudioBuffer<float> pcm(2, 12000);
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < pcm.getNumSamples(); ++frame) pcm.setSample(channel, frame, ((frame * 13 + channel) % 113 - 56) / 128.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write retirement fixture PCM");
}
RecoveryMutationProducer::Config settingsFor(const juce::File& root, const char* name)
{
    RecoveryMutationProducer::Config value; value.root = root.getChildFile(name); value.queue.maximumAge = 10s;
    value.graph.chunkSampleBytes = 4096; return value;
}
}

int runRecoveryOwnerRetirementCrashChild(const juce::File& root, const juce::Uuid& owner, int stage)
{
    RecoveryStore store(root, owner);
    const auto view = store.inspectManifest();
    if (!view.status || view.commits.empty()) return 12;
    store.onRetirementBoundary = [stage] (auto boundary)
    {
        if (static_cast<int>(boundary) == stage) std::_Exit(87);
    };
    OwnerRetirement retired;
    return RecoveryOwnerLifecycle::discard(store, view.head, retired) ? 13 : 14;
}

int runRecoveryTerminalCrashChild(const juce::File& root, const juce::Uuid& owner, const SafeHead& head, int stage)
{
    RecoveryStore store(root, owner);
    store.onTerminalBoundary = [stage] (auto boundary)
    { if (static_cast<int>(boundary) == stage) std::_Exit(88); };
    for (int step = 0; step < 256; ++step)
    {
        TerminalCollectionReport report;
        if (!store.collectTerminal(head, report)) return 15;
        if (report.completed) return 16;
    }
    return 17;
}

int runRecoveryControlQuotaChild(const juce::File& root, const juce::Uuid& owner, const juce::File& signals, int mode)
{
    RecoveryStore store(root, owner);
    const auto view = store.inspectManifest();
    if (!view.status || view.commits.empty()) return 18;
    const auto hold = [&]
    {
        require(signals.getChildFile("ready").replaceWithText("actual control publication holds root lock"), "signal private control quota process");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
        while (!signals.getChildFile("release").existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::Thread::sleep(2);
        require(signals.getChildFile("release").existsAsFile(), "release owned control quota child within bound");
    };
    if (mode == 0)
    {
        store.onRetirementBoundary = [&] (auto boundary) { if (boundary == RecoveryStore::RetirementBoundary::beforePublish) hold(); };
        OwnerRetirement retired;
        return RecoveryOwnerLifecycle::discard(store, view.head, retired) ? 0 : 19;
    }
    if (mode == 1)
    {
        store.onCollectionBoundary = [&] (auto boundary) { if (boundary == RecoveryStore::CollectionBoundary::marked) hold(); };
        OrphanCollectionReport report;
        return store.collectOrphans(view.head, report) ? 0 : 20;
    }
    store.onTerminalBoundary = [&] (auto boundary) { if (boundary == RecoveryStore::TerminalBoundary::planned) hold(); };
    TerminalCollectionReport report;
    return store.collectTerminal(view.head, report) ? 0 : 21;
}

void runRecoveryControlQuotaTests(const juce::File& root)
{
    const auto source = root.getChildFile("control-quota-source.wav"); makeSource(source);
    const auto fill = [] (const juce::File& storage, int64_t limit)
    {
        int64_t total = 0;
        for (const auto& file : storage.findChildFiles(juce::File::findFiles, true)) total += file.getSize();
        require(total < limit && limit - total < 4 * 1024 * 1024, "known private fixture has bounded quota filler");
        juce::MemoryBlock data(static_cast<size_t>(limit - total), true);
        const auto file = storage.getChildFile("unknown-quota-fixture");
        require(file.replaceWithData(data.getData(), data.getSize()), "fill only owned unknown quota fixture to exact root cap");
        return file;
    };
    const auto finish = [] (RecoveryStore& store, const SafeHead& head)
    {
        for (int pass = 0; pass < 256; ++pass)
        {
            TerminalCollectionReport report;
            good(store.collectTerminal(head, report, { 1, 1024 * 1024 }), "resume already durable terminal plan while storage is full");
            require(report.filesRemoved <= 1, "quota-governed terminal resumption retains bounded deletion");
            if (report.completed) return;
        }
        require(false, "bounded control quota fixture collection converges");
    };
    for (int mode = 0; mode < 8; ++mode)
    {
        auto config = settingsFor(root, ("control-quota-" + juce::String(mode)).toRawUTF8());
        config.store.globalBytes = 1024 * 1024;
        config.store.globalEntries = 512;
        SingleTrackDocument document; require(document.loadFile(source), "load actual control quota Document");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "observe real control quota capture");
        require(document.moveEnvelopePoint(0, 0, 0.6f) && document.moveEnvelopePoint(0, 0, 0.4f) && producer->drain(10s),
            "publish real current and Undo before control admission");
        const auto durable = producer->health(); producer->close(); producer.reset();
        auto higher = config.store; higher.globalBytes *= 4;
        RecoveryStore store(config.root, durable.owner, higher); // Cannot raise the persisted root policy.
        const auto head = durable.writer.safeHead;
        const auto directory = config.root.getChildFile(durable.owner.toString());
        OwnerRetirement retired;
        if (mode == 0 || mode == 1)
        {
            juce::File filler;
            if (mode == 0) filler = fill(config.root, config.store.globalBytes);
            else
            {
                const auto entries = config.root.findChildFiles(juce::File::findFilesAndDirectories, true).size();
                for (int index = entries; index < static_cast<int>(config.store.globalEntries); ++index)
                    require(config.root.getChildFile("unknown-entry-" + juce::String(index)).create().wasOk(), "fill bounded private control entry fixture");
            }
            const auto result = RecoveryOwnerLifecycle::discard(store, head, retired);
            require(result.code == (mode == 0 ? "global_byte_budget" : "global_entry_budget")
                && !directory.getChildFile("retired").exists() && store.inspect().head == head,
                "retirement cannot exceed persisted byte or entry policy using a larger caller cap");
            require(document.hasUnsavedChanges() && document.undo() && document.redo(), "quota refusal leaves actual editable history intact");
            if (mode == 0) require(filler.deleteFile(), "remove only own exact-cap filler to retry retirement");
            else for (const auto& file : config.root.findChildFiles(juce::File::findFiles, false, "unknown-entry-*"))
                require(file.deleteFile(), "remove only generated quota entry fixtures");
            good(RecoveryOwnerLifecycle::discard(store, head, retired), "same exact owner can retry after real capacity relief");
        }
        else if (mode == 2)
        {
            const auto stage = directory.getChildFile("staging-" + juce::Uuid().toString());
            require(stage.replaceWithText("owned failed staging"), "create recognized private orphan input");
            auto filler = fill(config.root, config.store.globalBytes);
            OrphanCollectionReport report;
            require(store.collectOrphans(head, report).code == "global_byte_budget" && stage.existsAsFile()
                && report.filesRemoved == 0 && store.inspect().head == head,
                "orphan collection never starts an unbudgeted plan or deletes input on quota refusal");
            require(filler.deleteFile(), "release exact private capacity before orphan plan");
            bool cancel = false;
            store.onCollectionBoundary = [&] (auto boundary)
            {
                if (boundary != RecoveryStore::CollectionBoundary::marked) return;
                filler = fill(config.root, config.store.globalBytes + 4096);
                cancel = true;
            };
            require(store.collectOrphans(head, report, {}, [&] { return cancel; }).error == Error::cancelled && stage.existsAsFile(),
                "cancel after durable orphan plan preserves recoverable transaction");
            store.onCollectionBoundary = {};
            good(store.collectOrphans(head, report), "resume prior orphan plan without new capacity admission");
            require(report.filesRemoved == 1 && !stage.exists() && filler.existsAsFile() && store.inspect().head == head,
                "already authorized orphan deletion frees data even above cap without deleting unknown bytes");
        }
        else if (mode == 3)
        {
            good(RecoveryOwnerLifecycle::discard(store, head, retired), "retire exact selected owner before terminal quota pressure");
            auto filler = fill(config.root, config.store.globalBytes);
            TerminalCollectionReport report;
            const auto intent = config.root.getChildFile("terminal-" + durable.owner.toString());
            require(store.collectTerminal(head, report).code == "global_byte_budget" && !intent.exists()
                && report.filesRemoved == 0 && store.inspect().head == head,
                "terminal inventory publication is admitted using actual full payload size");
            require(filler.deleteFile(), "release private capacity for actual terminal intent");
            good(store.collectTerminal(head, report), "publish terminal plan with existing DELETE guards measured by handles");
            require(report.more && intent.existsAsFile(), "terminal certification remains its separate durable bounded pass");
            filler = fill(config.root, config.store.globalBytes + 4096);
            finish(store, head);
            require(!directory.exists() && !intent.exists() && filler.existsAsFile()
                && config.root.getChildFile("quota-policy").existsAsFile(),
                "full-root terminal resume removes only certified owner and keeps unknown data and root policy");
        }
        else if (mode == 4 || mode == 5)
        {
            const auto writer = directory.getChildFile("writer.lock"), outside = root.getChildFile("control-external-lock");
            if (mode == 4) require(writer.replaceWithText("unknown control lock data"), "inject only owned nonempty control lock");
#if JUCE_WINDOWS
            else require(outside.create().wasOk() && CreateHardLinkW(writer.getFullPathName().toWideCharPointer(),
                outside.getFullPathName().toWideCharPointer(), nullptr), "make actual linked empty control lock fixture");
#endif
            OrphanCollectionReport report;
            require(RecoveryOwnerLifecycle::discard(store, head, retired).code == "owner_lock_invalid"
                && store.collectOrphans(head, report).code == "owner_lock_invalid" && writer.existsAsFile()
                && (mode == 4 ? writer.loadFileAsString() == "unknown control lock data" : outside.existsAsFile()),
                "retirement and orphan paths preserve unknown locks before delete disposition");
        }
        else if (mode == 6)
        {
#if JUCE_WINDOWS
            const auto unknown = config.root.getChildFile("unknown-active-writer");
            require(unknown.replaceWithText("held writer data"), "create only private inventory sharing fixture");
            const auto writer = CreateFileW(unknown.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(writer != INVALID_HANDLE_VALUE, "hold real ungoverned writer during control inventory");
            const auto result = RecoveryOwnerLifecycle::discard(store, head, retired); CloseHandle(writer);
            require(result.code == "global_inventory_entry" && !directory.getChildFile("retired").exists()
                && unknown.loadFileAsString() == "held writer data", "control admission cannot claim stable capacity over an active unknown writer");
            good(RecoveryOwnerLifecycle::discard(store, head, retired), "actual writer release permits control retry");
#endif
        }
        else
        {
            const auto policy = config.root.getChildFile("quota-policy");
            require(policy.deleteFile(), "remove only known fixture policy to emulate a verified legacy owner");
            for (int index = 0; index < 2; ++index)
                require(directory.getChildFile("staging-" + juce::Uuid().toString()).replaceWithText("legacy orphan input"),
                    "create two bounded legacy orphan fixture inputs");
            int publications = 0;
            store.onCollectionBoundary = [&] (auto boundary)
            {
                if (boundary != RecoveryStore::CollectionBoundary::marked) return;
                ++publications;
                require(policy.existsAsFile() && !policy.replaceWithText("competing policy mutation"),
                    "newly published quota policy remains pinned across all control mutations in the batch");
            };
            OrphanCollectionReport report;
            good(store.collectOrphans(head, report), "legacy batch publishes one policy then multiple actual orphan plans");
            require(publications == 2 && report.filesRemoved == 2 && store.inspect().head == head,
                "subsequent admissions reuse the same verified policy without republishing or losing protected data");
        }
    }
    for (int mode = 0; mode < 3; ++mode)
    {
        auto config = settingsFor(root, ("control-process-" + juce::String(mode)).toRawUTF8());
        std::array<juce::Uuid, 2> owners; std::array<SafeHead, 2> heads;
        for (int index = 0; index < 2; ++index)
        {
            SingleTrackDocument document; require(document.loadFile(source), "load real independent owner for process contention");
            std::unique_ptr<RecoveryMutationProducer> producer;
            good(RecoveryMutationProducer::attachPostCore(document, config, producer), "attach real independent process owner capture");
            require(document.moveEnvelopePoint(0, 0, index == 0 ? 0.6f : 0.3f) && producer->drain(10s), "durably publish different real owner");
            owners[index] = producer->health().owner; heads[index] = producer->health().writer.safeHead; producer->close();
        }
        RecoveryStore other(config.root, owners[1]); OwnerRetirement retired;
        if (mode == 1) require(config.root.getChildFile(owners[0].toString()).getChildFile("staging-" + juce::Uuid().toString())
            .replaceWithText("owned child orphan"), "create private orphan for real control child");
        if (mode == 2)
        {
            RecoveryStore selected(config.root, owners[0]);
            good(RecoveryOwnerLifecycle::discard(selected, heads[0], retired), "retire only child owner before real terminal process");
        }
        const auto signals = root.getChildFile("control-signals-" + juce::String(mode));
        require(signals.createDirectory().wasOk(), "create private control process signals outside recovery budget root");
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--control-quota-child", config.root.getFullPathName(), owners[0].toString(), signals.getFullPathName(), juce::String(mode) }),
            "start actual separate process holding control quota through publication");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
        while (!signals.getChildFile("ready").existsAsFile() && child.isRunning() && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::Thread::sleep(2);
        require(signals.getChildFile("ready").existsAsFile() && child.isRunning(), "child reaches actual control write boundary with root exclusion");
        require(RecoveryOwnerLifecycle::discard(other, heads[1], retired).code == "global_budget_busy"
            && other.inspect().head == heads[1] && !config.root.getChildFile(owners[1].toString()).getChildFile("retired").exists(),
            "cross-process different-owner retirement cannot bypass concurrent control capacity reservation");
        require(signals.getChildFile("release").replaceWithText("release only own control child"), "release real owned control publisher");
        require(child.waitForProcessToFinish(15000) && child.getExitCode() == 0, "actual control mutation finishes after root-lock contention");
        good(RecoveryOwnerLifecycle::discard(other, heads[1], retired), "exact competing owner retries after real root lock release");
    }
    std::cout << "RecoveryControlQuotaTests passed (retirement/orphan/terminal capacity, retained history, over-cap resume, real locks and 3 competing process boundaries)\n";
}

namespace
{
void terminalCollectionTests(const juce::File& root, const juce::File& source)
{
    const auto finish = [] (RecoveryStore& store, const SafeHead& head)
    {
        for (int step = 0; step < 256; ++step)
        {
            TerminalCollectionReport report;
            good(store.collectTerminal(head, report, { 1, 1024 * 1024 }), "resume bounded terminal collection");
            require(report.filesRemoved <= 1 && (report.completed || report.more), "every sweep reports bounded progress or completion");
            if (report.completed) return;
        }
        require(false, "terminal collection converges within fixture bound");
    };
    for (int scenario = 0; scenario < 25; ++scenario)
    {
        SingleTrackDocument document; require(document.loadFile(source), "open actual terminal-collection Document");
        auto config = settingsFor(root, ("terminal-" + juce::String(scenario)).toRawUTF8());
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "observe actual collector fixture edits");
        require(document.moveEnvelopePoint(0, 0, 0.6f) && document.moveEnvelopePoint(0, 0, 0.4f) && producer->drain(10s),
            "durably capture actual current and Undo before owner collection");
        const auto protectedState = producer->health();
        std::unique_ptr<SavedOwnerRetirement> savedTicket;
        if (scenario == 18)
        {
            const auto snapshot = document.createSaveSnapshot(root.getChildFile("terminal-saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
            const auto saved = SingleTrackDocument::writeSaveSnapshot(snapshot);
            require(saved.succeeded && document.markSaveSnapshotCommitted(snapshot, saved), "real Save publication cleans exact collector Document");
            good(RecoveryOwnerLifecycle::prepareSaved(document, snapshot, saved, *producer, savedTicket), "capture genuine saved-owner retirement authority");
        }
        producer->close();
        const auto owner = protectedState.owner; const auto head = protectedState.writer.safeHead;
        RecoveryStore store(config.root, owner);
        const auto directory = config.root.getChildFile(owner.toString());
        const auto intent = config.root.getChildFile("terminal-" + owner.toString());
        TerminalCollectionReport report;
        require(store.collectTerminal(head, report).code == "terminal_marker_required" && store.inspect().head == head,
            "active owner cannot be collected based on healthy/durable metadata alone");
        OwnerRetirement retired;
        good(savedTicket ? RecoveryOwnerLifecycle::retireSaved(store, head, *savedTicket, retired)
                         : RecoveryOwnerLifecycle::discard(store, head, retired), "issue actual exact-owner terminal intent");
        if (scenario < 6 || scenario >= 22)
        {
            juce::ChildProcess child;
            require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                "--terminal-crash", config.root.getFullPathName(), owner.toString(), juce::String(head.sequence), head.hash, juce::String(scenario >= 22 ? scenario - 16 : scenario) })
                && child.waitForProcessToFinish(15000) && child.getExitCode() == 88,
                "real process death at each terminal collection boundary");
        }
        else if (scenario == 6)
        {
            require(store.collectTerminal(head, report, {}, [] { return true; }).error == Error::cancelled && !intent.exists(),
                "cancel before inventory performs no deletion or intent publication");
            const auto unknown = directory.getChildFile("future-data"); require(unknown.replaceWithText("preserve"), "create only owned unknown file");
            require(store.collectTerminal(head, report).code == "terminal_unknown_entry" && unknown.loadFileAsString() == "preserve" && !intent.exists(),
                "unknown owner contents fail closed before any data removal");
            require(unknown.deleteFile(), "remove only known injected fixture file");
        }
        else if (scenario == 7)
        {
#if JUCE_WINDOWS
            auto lock = CreateFileW(directory.getChildFile("usage.lease").getFullPathName().toWideCharPointer(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(lock != INVALID_HANDLE_VALUE, "hold real existing usage file from outside collector");
            require(!store.collectTerminal(head, report) && !intent.exists(), "actual usage sharing rejects collection");
            CloseHandle(lock);
            lock = CreateFileW(directory.getChildFile("writer.lock").getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            require(lock != INVALID_HANDLE_VALUE && !store.collectTerminal(head, report) && !intent.exists(), "actual writer file sharing rejects collection");
            CloseHandle(lock);
#endif
        }
        else if (scenario == 8)
        {
            bool checked = false;
            store.onTerminalBoundary = [&] (auto boundary)
            {
#if JUCE_WINDOWS
                if (boundary == RecoveryStore::TerminalBoundary::beforeDelete)
                    for (const auto* name : { "head", "retired" })
                    {
                        const auto path = directory.getChildFile(name);
                        if (!path.exists()) continue;
                        const auto write = CreateFileW(path.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                        require(write == INVALID_HANDLE_VALUE, "head and terminal authority remain write-protected across payload deletion");
                    }
#endif
                if (boundary != RecoveryStore::TerminalBoundary::locksRemoved) return;
                require(!directory.getChildFile("usage.lease").exists() && !directory.getChildFile("writer.lock").exists(),
                    "lock files are actually gone at final race boundary");
                std::shared_ptr<RecoveryOwnerLease> live;
                require(!store.acquireOwnerLease(live), "no new owner lease after lock files disappear");
#if JUCE_WINDOWS
                auto pin = CreateFileW(directory.getFullPathName().toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                require(pin == INVALID_HANDLE_VALUE, "directory DELETE handle still excludes ordinary writer/live-owner pins");
                const auto moved = directory.getSiblingFile("unexpected-move");
                require(!MoveFileExW(directory.getFullPathName().toWideCharPointer(), moved.getFullPathName().toWideCharPointer(), 0),
                    "collector directory cannot be renamed or redirected at final boundary");
#endif
                juce::ChildProcess child;
                require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                    "--terminal-crash", config.root.getFullPathName(), owner.toString(), juce::String(head.sequence), head.hash, "0" })
                    && child.waitForProcessToFinish(10000) && child.getExitCode() == 15,
                    "another process cannot enter collection after lock deletion");
                checked = true;
            };
            finish(store, head);
            require(checked, "exercise actual final lock deletion ordering");
        }
        else if (scenario == 9)
        {
            good(store.collectTerminal(head, report), "publish inventory before physical version fault");
            const auto files = directory.getChildFile("assets").findChildFiles(juce::File::findFiles, false);
            require(!files.isEmpty(), "actual captured audio exists for restamp test");
#if JUCE_WINDOWS
            auto changed = CreateFileW(files[0].getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(changed != INVALID_HANDLE_VALUE, "open owned unleased CAS only after collector yields");
            FILETIME created{}, accessed{}, modified{}; require(GetFileTime(changed, &created, &accessed, &modified), "retain exact mtime");
            unsigned char byte = 0; DWORD count = 0; require(ReadFile(changed, &byte, 1, &count, nullptr) && count == 1, "read only owned fixture byte");
            byte ^= 1; LARGE_INTEGER start{};
            require(SetFilePointerEx(changed, start, nullptr, FILE_BEGIN) && WriteFile(changed, &byte, 1, &count, nullptr)
                && FlushFileBuffers(changed) && SetFileTime(changed, nullptr, nullptr, &modified) && CloseHandle(changed), "change bytes under same ID/size/exact mtime");
#endif
            require(store.collectTerminal(head, report).code == "terminal_file_changed" && files[0].exists() && intent.exists(),
                "restamped file is preserved with resumable intent instead of deleted");
            continue; // Corrupt fixture is retained until enclosing UUID test teardown.
        }
        else if (scenario == 10)
        {
            good(store.collectTerminal(head, report), "persist intent before root-scope copy fault");
            const auto copy = root.getChildFile("terminal-copied-root");
            require(config.root.copyDirectoryTo(copy), "copy only test-owned inactive collection fixture");
            RecoveryStore other(copy, owner);
            require(other.collectTerminal(head, report).code == "terminal_plan_scope" && copy.getChildFile(owner.toString()).exists(),
                "copied plan never transfers deletion authority to another root");
        }
        else if (scenario == 11)
        {
            good(store.collectTerminal(head, report), "persist inventory before cancelled sweep");
            bool stop = false;
            store.onTerminalBoundary = [&] (auto boundary) { if (boundary == RecoveryStore::TerminalBoundary::itemDeleted) stop = true; };
            require(store.collectTerminal(head, report, {}, [&] { return stop; }).error == Error::cancelled
                && report.filesRemoved == 1 && intent.exists(), "cancel after one real deletion retains durable explanation and remaining data");
            store.onTerminalBoundary = {};
        }
        else if (scenario == 12)
        {
            good(store.collectTerminal(head, report), "persist inventory before unsupported intent schema");
            std::unique_ptr<detail::DurableDirectory> meta;
            good(detail::DurableDirectory::open(config.root, false, meta), "pin only test-owned plan parent");
            juce::var json; good(meta->readJson(intent.getFileName(), 1024 * 1024, json), "read owned intent schema fixture");
            json.getDynamicObject()->setProperty("schema", 99);
            good(meta->publishJson(intent.getFileName(), 1024 * 1024, json, true), "publish future checksummed intent");
            require(store.collectTerminal(head, report).error == Error::unsupportedSchema && report.filesRemoved == 0,
                "unknown collection schema preserves entire owner");
            continue;
        }
        else if (scenario == 13)
        {
            // A real reader and Undo can outlive their controller. Injecting a
            // previously valid terminal marker must still not override that lease.
            const auto marker = directory.getChildFile("retired");
            juce::MemoryBlock original; require(marker.loadFileAsData(original) && marker.deleteFile(), "temporarily hide only owned marker for live-reader fault");
            std::shared_ptr<const RecoveredCandidateDocument> recovered;
            {
                RecoveryRestoreCandidate::Request request;
                request.observedOwnerHead = head; request.checkpointHead = head;
                request.scratchDirectory = root.getChildFile("terminal-live-scratch");
                request.attemptDirectory = root.getChildFile("terminal-live-attempts");
                request.appBuild = "terminal-tests-v1"; request.limits = config.graph; request.previewSeconds = 1.0;
                require(request.scratchDirectory.createDirectory().wasOk(), "create owned terminal live-reader scratch");
                RecoveryRestoreCandidate candidate(store, request);
                for (int step = 0; candidate.getStage() != RecoveryAttemptStage::coreReady && step < 24; ++step)
                    good(candidate.advance(), "build actual recovered reader for collection exclusion");
                good(candidate.acquireCore(candidate.getAttemptId(), recovered), "retain actual recovered Document outside controller");
            }
            require(recovered->document->moveEnvelopePoint(0, 0, 0.2f) && recovered->document->undo(), "real recovered current/Undo retains old CAS");
            require(marker.replaceWithData(original.getData(), original.getSize()), "restore actual marker bytes while real reader still owns CAS");
            require(store.collectTerminal(head, report).code == "terminal_directory_busy" && !intent.exists(), "actual progressive current/Undo defeats conflicting terminal deletion");
            juce::ChildProcess child;
            require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                "--terminal-crash", config.root.getFullPathName(), owner.toString(), juce::String(head.sequence), head.hash, "0" })
                && child.waitForProcessToFinish(10000) && child.getExitCode() == 15,
                "actual parent-process reader blocks collector process with terminal marker present");
            recovered.reset();
        }
        else if (scenario == 14)
        {
            good(store.collectTerminal(head, report), "persist directory identity before replacement fault");
            const auto assets = directory.getChildFile("assets"), moved = root.getChildFile("terminal-moved-assets");
            require(assets.moveFileTo(moved) && assets.createDirectory().wasOk(), "replace only owned inactive assets directory with empty object");
            require(store.collectTerminal(head, report).code == "terminal_subdirectory_changed" && report.filesRemoved == 0
                && moved.isDirectory(), "same-path replacement cannot transfer directory deletion authority");
            continue;
        }
        else if (scenario == 15)
        {
            good(store.collectTerminal(head, report), "persist inventory before marker mutation");
            std::unique_ptr<detail::DurableDirectory> meta;
            good(detail::DurableDirectory::open(directory, false, meta), "open only owned terminal metadata");
            juce::var json; good(meta->readJson("retired", 16384, json), "read owned terminal schema");
            json.getDynamicObject()->setProperty("schema", 99);
            good(meta->publishJson("retired", 16384, json, true), "replace marker with future schema between batches");
            meta.reset();
            require(store.collectTerminal(head, report).code == "terminal_authority_changed" && report.filesRemoved == 0,
                "changed terminal authority stops before deleting any payload this batch");
            continue;
        }
        else if (scenario == 16)
        {
            const auto files = directory.getChildFile("assets").findChildFiles(juce::File::findFiles, false);
            require(!files.isEmpty(), "owned CAS exists for hardlink fault");
#if JUCE_WINDOWS
            const auto outside = root.getChildFile("terminal-hardlink-outside");
            require(CreateHardLinkW(outside.getFullPathName().toWideCharPointer(), files[0].getFullPathName().toWideCharPointer(), nullptr) != 0,
                "create fixture-only additional hardlink");
            require(!store.collectTerminal(head, report) && !intent.exists() && outside.exists(), "multiply linked payload fails closed before first delete");
            require(outside.deleteFile(), "remove only owned hardlink fixture");
#endif
        }
        else if (scenario == 17)
        {
            good(store.collectTerminal(head, report), "persist inventory before byte-limited sweep");
            require(store.collectTerminal(head, report, { 128, 1 }).code == "terminal_item_budget", "insufficient per-item budget is explicit instead of an endless no-progress retry");
            require(report.filesRemoved == 0 && report.bytesRead == 0, "byte budget is enforced independently of file budget");
        }
        else if (scenario == 19)
        {
            const auto files = directory.getChildFile("assets").findChildFiles(juce::File::findFiles, false);
            const auto moved = root.getChildFile("terminal-missing-asset");
            require(!files.isEmpty() && files[0].moveFileTo(moved), "hide only owned referenced CAS to simulate corrupt owner");
            require(store.collectTerminal(head, report).code == "terminal_asset_missing" && report.filesRemoved == 0 && !intent.exists(),
                "missing referenced asset cannot be silently treated as normal terminal collection");
            require(moved.moveFileTo(files[0]), "restore only owned missing CAS fixture");
        }
        else if (scenario == 20)
        {
#if JUCE_WINDOWS
            good(store.collectTerminal(head, report), "persist plan before real junction attack");
            const auto assets = directory.getChildFile("assets"), moved = root.getChildFile("terminal-junction-assets");
            const auto outside = root.getChildFile("terminal-junction-outside"), sentinel = outside.getChildFile("keep.txt");
            require(assets.moveFileTo(moved) && assets.createDirectory().wasOk() && outside.createDirectory().wasOk()
                && sentinel.replaceWithText("untouched"), "prepare only test-owned junction and sentinel");
            const auto substitute = juce::String("\\??\\") + outside.getFullPathName(), display = outside.getFullPathName();
            const auto substituteBytes = static_cast<USHORT>(substitute.length() * 2), displayBytes = static_cast<USHORT>(display.length() * 2);
            juce::MemoryBlock buffer(static_cast<size_t>(20 + substituteBytes + displayBytes), true);
            auto* raw = static_cast<unsigned char*>(buffer.getData()); const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
            const USHORT length = static_cast<USHORT>(12 + substituteBytes + displayBytes), displayOffset = static_cast<USHORT>(substituteBytes + 2);
            std::memcpy(raw, &tag, 4); std::memcpy(raw + 4, &length, 2); std::memcpy(raw + 10, &substituteBytes, 2);
            std::memcpy(raw + 12, &displayOffset, 2); std::memcpy(raw + 14, &displayBytes, 2);
            std::memcpy(raw + 16, substitute.toWideCharPointer(), substituteBytes);
            std::memcpy(raw + 16 + displayOffset, display.toWideCharPointer(), displayBytes);
            const auto handle = CreateFileW(assets.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "open owned junction fixture"); DWORD returned = 0;
            const auto installed = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.getData(), static_cast<DWORD>(buffer.getSize()), nullptr, 0, &returned, nullptr);
            CloseHandle(handle); require(installed != FALSE, "install actual directory reparse fixture");
            require(!store.collectTerminal(head, report) && report.filesRemoved == 0 && sentinel.loadFileAsString() == "untouched",
                "collector rejects real junction without crossing selected owner");
            require(RemoveDirectoryW(assets.getFullPathName().toWideCharPointer()) && moved.moveFileTo(assets),
                "remove only the junction itself and restore owned directory before fixture teardown");
#endif
        }
        else if (scenario == 21)
        {
            good(store.collectTerminal(head, report), "persist plan before unknown file arrives between batches");
            const auto extra = directory.getChildFile("future-payload"); require(extra.replaceWithText("keep"), "inject only owned unplanned file");
            require(store.collectTerminal(head, report).code == "terminal_unknown_entry" && report.filesRemoved == 0
                && extra.loadFileAsString() == "keep", "sweep rechecks unknown leftovers before deleting any planned data");
            require(extra.deleteFile(), "remove only owned unplanned test file");
        }
        finish(store, head);
        require(!directory.exists() && !intent.exists() && source.exists(), "only selected terminal owner and temporary intent disappear");
        good(store.collectTerminal(head, report), "completed collection retry is idempotent");
        require(report.completed && report.filesRemoved == 0, "completed retry creates no files or invented progress");
        const auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
        require(snapshot.has_value(), "capture real Document for stale append after whole-owner cleanup");
        CommitPacket packet; good(RecoveryCheckpointCodec::encode(*snapshot, {}, packet, config.graph), "encode stale collected-owner append fixture");
        SafeHead unchanged;
        require(!store.append(head, packet, unchanged) && unchanged == head && !directory.exists(),
            "stale append after cleanup cannot recreate an empty writable owner directory");
        std::shared_ptr<RecoveryOwnerLease> denied;
        require(!store.acquireOwnerLease(denied) && !directory.exists(), "new lease after collection neither succeeds nor creates storage");
    }
    std::cout << "TerminalCollectionTests passed (bounded sweep, exact inventory, real locks/readers, replacement/junction/hardlink, cancellation, 9 process-kill boundaries)\n";
}

void sessionRetirementBridgeTests(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load actual Document for session retirement bridge");
    RecoveryProtectionSession::Config config; config.producer = settingsFor(root, "session-retirement-bridge");
    std::unique_ptr<RecoveryProtectionSession> session;
    good(RecoveryProtectionSession::attachPostCore(document, config, session), "attach continuous observer without exposing producer");
    require(document.moveEnvelopePoint(0, 0, 0.4f) && session->drain(10s), "protect actual dirty state before Save");
    const auto old = session->health().current;
    const auto snapshot = document.createSaveSnapshot(root.getChildFile("session-retirement-output.wav"), SingleTrackDocument::SaveIntent::saveAs);
    const auto saved = SingleTrackDocument::writeSaveSnapshot(snapshot);
    std::unique_ptr<SavedOwnerRetirement> ticket;
    require(saved.succeeded && !session->prepareSavedRetirement(old.owner, snapshot, saved, ticket) && !ticket,
        "unadopted genuine file publication does not authorize retirement of dirty Document");
    require(document.markSaveSnapshotCommitted(snapshot, saved) && session->drain(10s), "actual Save event retires and drains private epoch");
    SingleTrackDocument::SaveWriteResult fabricated { true, snapshot.targetFile, snapshot.changeGeneration, {} };
    require(!session->prepareSavedRetirement(old.owner, snapshot, fabricated, ticket) && !ticket
        && session->health().retired.size() == 1, "fabricated success cannot cross bridge or erase original actor");
    require(session->prepareSavedRetirement(juce::Uuid(), snapshot, saved, ticket).code == "session_retirement_unknown_owner",
        "health from another owner cannot select an internal producer");
    good(session->prepareSavedRetirement(old.owner, snapshot, saved, ticket), "bridge captures exact private producer before runtime release");
    require(ticket && ticket->ownerId() == old.owner && ticket->rootDirectory() == config.producer.root
        && ticket->frozenHead() == old.writer.safeHead, "opaque worker ticket exposes only immutable routing facts");
    const auto* preserved = ticket.get();
    require(!session->prepareSavedRetirement(old.owner, snapshot, saved, ticket) && ticket.get() == preserved,
        "failed duplicate output request preserves issued ticket and runtime owner");
    const auto released = session->takeDrainedEpochs();
    require(released.size() == 1 && session->health().retired.empty(), "host releases drained actor only after authority handoff");
    std::unique_ptr<SavedOwnerRetirement> fabricatedFromHealth;
    require(session->prepareSavedRetirement(released[0].owner, snapshot, saved, fabricatedFromHealth).code == "session_retirement_unknown_owner"
        && !fabricatedFromHealth, "returned Health cannot recreate erased producer credentials");
    require(document.undo() && session->drain(10s) && session->health().currentProtected,
        "later Undo immediately protects a new exact owner despite smaller state version");
    const auto newer = session->health().current;
    RecoveryStore newStore(config.producer.root, newer.owner); OwnerRetirement terminal;
    require(RecoveryOwnerLifecycle::retireSaved(newStore, newer.writer.safeHead, *ticket, terminal).code == "retirement_owner_scope",
        "handed-off ticket cannot retire the later Undo owner");
    session.reset();
    RecoveryStore store(ticket->rootDirectory(), ticket->ownerId());
    good(RecoveryOwnerLifecycle::retireSaved(store, ticket->frozenHead(), *ticket, terminal),
        "worker publishes genuine saved terminal after both actor and Session are destroyed");
    bool done = false;
    for (int step = 0; step < 256 && !done; ++step)
    {
        TerminalCollectionReport report;
        good(store.collectTerminal(ticket->frozenHead(), report), "collect exactly the saved owner after handoff");
        done = report.completed;
    }
    require(done && newStore.inspect().head == newer.writer.safeHead && document.canRedo(),
        "old owner cleanup preserves later Undo recovery data and live full Redo history");
    {
        SingleTrackDocument pendingDoc; require(pendingDoc.loadFile(source), "open separate pending-queue bridge fixture");
        RecoveryProtectionSession::Config settings; settings.producer = settingsFor(root, "session-ticket-pending");
        require(settings.producer.root.replaceWithText("owned obstruction"), "obstruct only this test's recovery storage");
        std::unique_ptr<RecoveryProtectionSession> pending;
        good(RecoveryProtectionSession::attachPostCore(pendingDoc, settings, pending), "clean bridge fixture remains storage-inert");
        require(pendingDoc.moveEnvelopePoint(0, 0, 0.3f) && !pending->drain(10s), "actual filesystem failure leaves accepted queue pending");
        const auto selected = pending->health().current.owner;
        const auto request = pendingDoc.createSaveSnapshot(root.getChildFile("session-ticket-pending-saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
        const auto written = SingleTrackDocument::writeSaveSnapshot(request);
        require(written.succeeded && pendingDoc.markSaveSnapshotCommitted(request, written), "real Save can succeed while recovery queue is degraded");
        std::unique_ptr<SavedOwnerRetirement> proof;
        require(pending->prepareSavedRetirement(selected, request, written, proof).code == "retirement_owner_not_settled"
            && !proof && pending->takeDrainedEpochs().empty(), "clean Save never upgrades pending/degraded queue into settled retirement permission");
        pending->close();
        require(settings.producer.root.deleteFile(), "remove only owned obstruction before explicit retry");
        good(pending->retry(selected), "retry exact accepted prefix after closing Session");
        require(pending->drain(10s), "real failed prefix now drains without reopening admission");
        good(pending->prepareSavedRetirement(selected, request, written, proof), "closed Session can hand off original settled actor after retry");
        require(pending->takeDrainedEpochs().size() == 1, "runtime releases after settled proof is captured");
    }
    {
        SingleTrackDocument cappedDoc; require(cappedDoc.loadFile(source), "open epoch-cap bridge fixture");
        RecoveryProtectionSession::Config settings; settings.producer = settingsFor(root, "session-ticket-cap"); settings.retiredEpochs = 1;
        std::unique_ptr<RecoveryProtectionSession> capped;
        good(RecoveryProtectionSession::attachPostCore(cappedDoc, settings, capped), "attach bounded bridge fixture");
        require(cappedDoc.moveEnvelopePoint(0, 0, 0.3f) && capped->drain(10s)
            && cappedDoc.saveAsToFile(root.getChildFile("session-ticket-cap-saved.wav")), "first saved epoch fills retirement runtime slot");
        require(cappedDoc.moveEnvelopePoint(0, 0, 0.2f) && capped->drain(10s), "second actor protects exact actual mutation before Save");
        const auto selected = capped->health().current.owner;
        const auto request = cappedDoc.createSaveSnapshot(); const auto written = SingleTrackDocument::writeSaveSnapshot(request);
        require(written.succeeded && cappedDoc.markSaveSnapshotCommitted(request, written) && capped->health().checkpointRequired,
            "epoch cap prevents replacing current actor but records actual clean Save");
        (void) capped->drain(10s);
        std::unique_ptr<SavedOwnerRetirement> proof;
        good(capped->prepareSavedRetirement(selected, request, written, proof), "bridge finds exact retired actor still held in current after transition failure");
        require(capped->health().checkpointRequired, "authority handoff does not clear unrelated Session degradation or fake repair");
    }
    std::cout << "SessionRetirementBridgeTests passed (actual Save/private actor, metadata-only ticket, release order, later Undo owner, terminal collection)\n";
}
}

int runRecoveryExpiryLeaseChild(const juce::File& root, const juce::Uuid& owner)
{
    SingleTrackDocument document;
    require(document.loadFile(root.getChildFile("expiry-source.wav")), "child opens actual source");
    auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
    require(snapshot.has_value(), "child captures actual document");
    CommitPacket packet; good(RecoveryCheckpointCodec::encode(*snapshot, {}, packet), "encode child checkpoint");
    packet.timestampMilliseconds = juce::Time::currentTimeMillis() - 2 * 86400000LL;
    RecoveryStore store(root.getChildFile("child"), owner); SafeHead head;
    std::shared_ptr<RecoveryOwnerLease> activity;
    good(store.append({}, packet, head, nullptr, nullptr, &activity), "child publishes with real activity lease");
    require(root.getChildFile("child-ready").replaceWithText(head.hash), "signal held child lease");
    juce::Thread::sleep(30000);
    return 19; // Parent must terminate this private test process.
}

void runRecoveryRetentionTests(const juce::File& root)
{
    require(!root.exists() && root.createDirectory().wasOk(), "fresh private automatic retention fixture");
    const auto source = root.getChildFile("source.wav"); makeSource(source);
    const auto sourceHash = juce::SHA256(source).toHexString();
    SingleTrackDocument document; require(document.loadFile(source), "open actual retention source");
    require(document.moveEnvelopePoint(0, 0, 0.6f) && document.moveEnvelopePoint(0, 0, 0.4f)
        && document.undo() && document.canRedo(), "retain actual complete current Undo/Redo");
    const auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
    require(snapshot.has_value(), "capture actual retention history");
    const auto image = document.captureWorkspaceSnapshot();
    CommitPacket packet; good(RecoveryCheckpointCodec::encode(*snapshot, {}, packet), "encode actual history for retention");
    packet.timestampMilliseconds = juce::Time::currentTimeMillis() - 2 * 86400000LL;
    const auto serviceRoot = root.getChildFile("service");
    RecoveryStorageLayout layout(serviceRoot);
    std::optional<RecoveryStorageLayout::RetentionPolicy> observed;
    good(layout.readRetention(observed), "missing root means policy unconfigured");
    require(!observed && !serviceRoot.exists(), "read does not create recovery storage");
    RecoveryStorageLayout::RetentionPolicy policy;
    good(layout.configureRetention(1, {}, policy), "explicitly configure first one-day policy");
    good(RecoveryStorageLayout(serviceRoot).readRetention(observed), "another layout reads persisted policy");
    require(observed && observed->hash == policy.hash && observed->days == 1, "policy survives object recreation");
    RecoveryStorageLayout::RetentionPolicy rejected;
    require(layout.configureRetention(0, policy.hash, rejected).code == "retention_configuration"
        && layout.configureRetention(7, {}, rejected).code == "retention_policy_changed", "invalid and stale settings cannot overwrite confirmed policy");
    const auto seed = [&](int ageDays, bool legacy = false)
    {
        const juce::Uuid owner; RecoveryStore store(layout.journalDirectory(), owner); SafeHead head;
        auto value = packet; value.timestampMilliseconds = juce::Time::currentTimeMillis() - int64_t(ageDays) * 86400000;
        std::shared_ptr<RecoveryOwnerLease> activity;
        good(store.append({}, value, head, nullptr, nullptr, legacy ? nullptr : &activity), "publish actual recovery owner");
        return owner;
    };
    const auto old = seed(2);
    RecoveryStore oldStore(layout.journalDirectory(), old);
    std::unique_ptr<OwnerDiscardReview> review;
    good(oldStore.reviewConfiguredExpiry(review), "review captures configured policy hash");
    good(layout.configureRetention(30, policy.hash, policy), "extend retention after review");
    OwnerRetirement retired;
    require(RecoveryOwnerLifecycle::discard(oldStore, *review, retired).code == "retention_policy_changed"
        && !oldStore.inspectManifest().retirement, "old shorter review cannot publish after policy extension");
    review.reset(); good(layout.configureRetention(1, policy.hash, policy), "explicitly return to one-day policy");
    const auto recent = seed(0), legacy = seed(2, true), terminal = seed(2);
    RecoveryStore terminalStore(layout.journalDirectory(), terminal);
    good(RecoveryOwnerLifecycle::discard(terminalStore, terminalStore.inspect().head, retired), "seed real already authorized terminal work");
    const juce::Uuid active;
    AsyncRecoveryWriter::Budget writerBudget; writerBudget.maximumAge = 10s;
    AsyncRecoveryWriter writer(layout.journalDirectory(), active, {}, writerBudget);
    AsyncRecoveryWriter::Submission submission; submission.retainedBytes = 1024 * 1024;
    submission.prepare = [packet](CommitPacket& value) { value = packet; return Status{}; };
    good(writer.submit(std::move(submission)), "submit old record through actual live writer");
    require(writer.drain(10s) && writer.progress().accepting && !writer.progress().workerRunning, "actual writer stays alive while idle");
    const auto drain = [](RecoveryMaintenanceQueue& queue)
    {
        std::vector<RecoveryMaintenanceQueue::JobStatus> completed;
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        for (;;)
        {
            auto batch = queue.takeCompleted(); completed.insert(completed.end(), batch.begin(), batch.end());
            const auto health = queue.progress();
            good(health.failure, "automatic queue has no unexplained failure");
            good(health.sweep.failure, "bounded discovery succeeds");
            if (!health.workerRunning && !health.sweep.active) break;
            require(std::chrono::steady_clock::now() < deadline, "automatic sweep is finite");
            juce::Thread::sleep(2);
        }
        auto batch = queue.takeCompleted(); completed.insert(completed.end(), batch.begin(), batch.end());
        return completed;
    };
    RecoveryMaintenanceQueue::Config config; config.jobs = 2; config.batch.files = 1;
    RecoveryMaintenanceQueue queue(config);
    good(queue.sweepTerminals(layout.journalDirectory()), "PostCore-style presence starts existing bounded maintenance");
    const auto complete = drain(queue);
    const auto removed = std::count_if(complete.begin(), complete.end(), [](const auto& job) { return !job.preserved; });
    require(removed == 2 && complete.front().disposition == RecoveryMaintenanceQueue::Disposition::previouslyRetired
        && !layout.journalDirectory().getChildFile(old.toString()).exists()
        && !layout.journalDirectory().getChildFile(terminal.toString()).exists(), "terminal pass precedes automatic expired-owner cleanup across bounded capacity");
    require(RecoveryStore(layout.journalDirectory(), recent).inspect().status
        && RecoveryStore(layout.journalDirectory(), legacy).inspect().status
        && RecoveryStore(layout.journalDirectory(), active).inspect().status
        && std::count_if(complete.begin(), complete.end(), [](const auto& job) { return job.preserved; }) == 3
        && queue.progress().sweep.preservedOwners == 3,
        "recent, historical unproven, and actual idle live writer are preserved without failed jobs");
    writer.close(); require(writer.drain(10s), "release actual idle writer");
    good(queue.sweepTerminals(layout.journalDirectory(), true), "configured sweep can restart after eligibility changes");
    const auto restarted = drain(queue);
    require(std::count_if(restarted.begin(), restarted.end(), [](const auto& job) { return !job.preserved; }) == 1
        && !layout.journalDirectory().getChildFile(active.toString()).exists() && queue.progress().sweep.preservedOwners == 2,
        "fresh pass removes only now inactive eligible owner without a stale preserved-owner notice");
    // Stop a real cursor at its page boundary, publish a longer policy, and
    // restart the same queue. No old work can delete under the shorter policy.
    const auto changed = seed(2);
    auto entered = std::make_shared<juce::WaitableEvent>(), release = std::make_shared<juce::WaitableEvent>();
    std::atomic<bool> once { false };
    RecoveryMaintenanceQueue::Config paused;
    paused.beforeSweepPage = [entered, release, &once] { if (!once.exchange(true)) { entered->signal(); require(release->wait(10000), "release paused policy sweep"); } };
    RecoveryMaintenanceQueue changedQueue(paused);
    good(changedQueue.sweepTerminals(layout.journalDirectory()), "start actual paused page");
    require(entered->wait(10000), "observe actual discovery boundary");
    good(layout.configureRetention(30, policy.hash, policy), "persist longer policy while discovery is pending");
    good(changedQueue.sweepTerminals(layout.journalDirectory(), true), "restart pending cursor for policy change");
    release->signal(); (void) drain(changedQueue);
    good(RecoveryStore(layout.journalDirectory(), changed).inspect().status, "policy restart preserves old short-period candidate");
    // Actual publication interruption: persisted expiry marker is consumed by a
    // new queue even after the setting was extended, with no fresh deletion grant.
    good(layout.configureRetention(1, policy.hash, policy), "choose shorter period for interruption fixture");
    RecoveryMaintenanceQueue::Config interrupted;
    interrupted.afterRetirementPublished = [](juce::Uuid) { throw std::runtime_error("after actual configured expiry publication"); };
    RecoveryMaintenanceQueue failing(interrupted);
    good(failing.sweepTerminals(layout.journalDirectory()), "start actual policy expiry before interruption");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (failing.progress().workerRunning && std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(2);
    const auto fault = failing.progress();
    const auto manifest = RecoveryStore(layout.journalDirectory(), changed).inspectManifest();
    require(!fault.workerRunning && !fault.failure && manifest.status && manifest.retirement
        && manifest.retirement->evidence["target"]["policy_hash"] == juce::var(policy.hash), "actual interrupted marker retains policy-bound authority");
    failing.close();
    good(layout.configureRetention(90, policy.hash, policy), "extend policy after durable retirement");
    RecoveryMaintenanceQueue resumed({}); good(resumed.sweepTerminals(layout.journalDirectory()), "restart from actual persisted marker");
    (void) drain(resumed);
    require(!layout.journalDirectory().getChildFile(changed.toString()).exists(), "restart completes already published terminal cleanup");
    require(juce::SHA256(source).toHexString() == sourceHash && document.captureWorkspaceSnapshot()->sameContents(*image)
        && document.undo() && document.redo() && document.captureWorkspaceSnapshot()->sameContents(*image), "automatic retention preserves source bytes and full executable current history");
    std::cout << "RecoveryRetentionTests passed (persisted policy/CAS, stale review, terminal priority, bounded automatic queue, actual idle writer, preserved counts, policy restart, interrupted publication, source and full history)\n";
}

void runRecoveryExpiryTests(const juce::File& root)
{
    require(!root.exists() && root.createDirectory().wasOk(), "fresh private expiry fixture root");
    const auto source = root.getChildFile("expiry-source.wav"); makeSource(source);
    const auto sourceHash = juce::SHA256(source).toHexString();
    SingleTrackDocument document; require(document.loadFile(source), "open real expiry source");
    require(document.moveEnvelopePoint(0, 0, 0.6f) && document.moveEnvelopePoint(0, 0, 0.4f)
        && document.undo() && document.canRedo(), "retain real current Undo and Redo");
    const auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
    require(snapshot.has_value(), "capture full expiry checkpoint");
    const auto image = document.captureWorkspaceSnapshot();
    CommitPacket packet; good(RecoveryCheckpointCodec::encode(*snapshot, {}, packet), "encode full real history");
    constexpr int64_t day = 86400000;
    packet.timestampMilliseconds = juce::Time::currentTimeMillis() - 2 * day;
    {
        RecoveryStore legacy(root.getChildFile("legacy"), juce::Uuid()); SafeHead head;
        good(legacy.append({}, packet, head), "persist historical protocol without an activity lease");
        std::unique_ptr<OwnerDiscardReview> review;
        require(legacy.reviewExpiry(day, review).code == "expiry_activity_unproven" && !review,
            "old application idle state is not inferred from timestamp or absent lock");
        good(legacy.reviewDiscard(review), "legacy selected-owner Discard remains available");
    }
    {
        const auto directory = root.getChildFile("live"); const juce::Uuid owner;
        AsyncRecoveryWriter::Budget budget; budget.maximumAge = 10s;
        AsyncRecoveryWriter writer(directory, owner, {}, budget);
        for (int i = 0; i < 2; ++i)
        {
            AsyncRecoveryWriter::Submission submission;
            submission.retainedBytes = 1024 * 1024;
            submission.prepare = [packet](CommitPacket& output) { output = packet; return Status{}; };
            good(writer.submit(std::move(submission)), "submit actual backdated crash checkpoint");
            require(writer.drain(10s), "real writer becomes idle with published data");
        }
        RecoveryStore store(directory, owner); const auto head = writer.progress().safeHead;
        require(writer.progress().accepting && !writer.progress().workerRunning, "writer is alive and idle, not running");
        std::unique_ptr<OwnerDiscardReview> review;
        good(store.reviewExpiry(day, review), "review uses committed age on exact live owner");
        OwnerRetirement retired;
        require(RecoveryOwnerLifecycle::discard(store, *review, retired).code == "owner_in_use"
            && !store.inspectManifest().retirement, "idle active writer prevents expiry publication");
        OrphanCollectionReport compact;
        std::shared_ptr<RecoveryOwnerLease> reader;
        good(store.acquireOwnerLease(reader), "actual restored-reader range coexists with writer");
        require(RecoveryCheckpointCodec::compact(store, head, compact).code == "owner_in_use",
            "actual reader still prevents compaction under range protocol");
        reader.reset();
        good(RecoveryCheckpointCodec::compact(store, head, compact), "own active writer permits actual checkpoint compaction");
        good(store.acquireOwnerLease(reader), "restore dependency remains independent of writer activity");
        writer.close(); require(writer.drain(10s), "closed drained writer releases activity");
        require(RecoveryOwnerLifecycle::discard(store, *review, retired).code == "owner_in_use",
            "reader remains a retirement barrier after writer close");
        reader.reset();
        RecoveryMaintenanceQueue::Config config; config.batch.files = 1;
        config.afterRetirementPublished = [](juce::Uuid) { throw std::runtime_error("injected after actual expiry marker"); };
        RecoveryMaintenanceQueue queue(config);
        good(queue.submitConfirmedDiscard(review), "actual confirmed expiry enters existing service queue");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (queue.progress().workerRunning && std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(2);
        const auto failed = queue.progress();
        require(!failed.workerRunning && failed.jobs.size() == 1 && failed.jobs[0].phase == RecoveryMaintenanceQueue::Phase::failed,
            "post-publication interruption is retained as failure");
        const auto terminal = store.inspectManifest();
        require(terminal.status && terminal.retirement && terminal.retirement->evidence["reason"] == juce::var("expired"),
            "durable marker records expiry authority, not fabricated Save or Discard");
        RecoveryMaintenanceQueue resumed({});
        good(resumed.resumeTerminal(directory, owner, head), "new queue resumes actual expired terminal marker");
        while (resumed.progress().workerRunning && std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(2);
        const auto complete = resumed.takeCompleted();
        require(complete.size() == 1 && !directory.getChildFile(owner.toString()).exists(),
            "bounded terminal GC completes expiry after interrupted publication");
    }
    {
        RecoveryStore store(root.getChildFile("age"), juce::Uuid()); SafeHead head;
        std::shared_ptr<RecoveryOwnerLease> activity;
        auto recent = packet; recent.timestampMilliseconds = juce::Time::currentTimeMillis();
        good(store.append({}, recent, head, nullptr, nullptr, &activity), "publish recent activity");
        good(store.append(head, packet, head, nullptr, nullptr, &activity), "publish older clock value without ordering generations");
        activity.reset(); std::unique_ptr<OwnerDiscardReview> review;
        require(store.reviewExpiry(day, review).code == "owner_not_expired" && !review,
            "last record timestamp cannot hide newer retained commit activity");
        OrphanCollectionReport compact;
        good(RecoveryCheckpointCodec::compact(store, head, compact), "compact backward-clock history");
        require(store.reviewExpiry(day, review).code == "owner_not_expired" && !review,
            "compaction retains the newest activity time without retaining every record");
        require(store.reviewExpiry(0, review).code == "expiry_review_request" && !review, "missing policy cannot authorize expiry");
        require(store.reviewExpiry(day, review, [] { return true; }).error == Error::cancelled && !review,
            "cancelled review preserves files and output");
    }
    {
        RecoveryStore store(root.getChildFile("stale"), juce::Uuid()); SafeHead head;
        std::shared_ptr<RecoveryOwnerLease> activity;
        good(store.append({}, packet, head, nullptr, nullptr, &activity), "publish exact stale-review fixture"); activity.reset();
        std::unique_ptr<OwnerDiscardReview> review; good(store.reviewExpiry(day, review), "freeze expiry head");
        good(store.append(head, packet, head, nullptr, nullptr, &activity), "same document generation receives a different true head"); activity.reset();
        OwnerRetirement retired;
        require(RecoveryOwnerLifecycle::discard(store, *review, retired).code == "discard_review_head_changed"
            && !store.inspectManifest().retirement, "same-generation newer head invalidates expiry authority");
    }
    {
        const juce::Uuid owner;
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--expiry-lease-child", root.getFullPathName(), owner.toString() }), "start private real lease process");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!root.getChildFile("child-ready").exists() && child.isRunning() && std::chrono::steady_clock::now() < deadline)
            juce::Thread::sleep(5);
        require(root.getChildFile("child-ready").exists() && child.isRunning(), "child holds actual activity handle after publication");
        RecoveryStore store(root.getChildFile("child"), owner);
        std::unique_ptr<OwnerDiscardReview> review; good(store.reviewExpiry(day, review), "review child-owned old candidate");
        OwnerRetirement retired;
        require(RecoveryOwnerLifecycle::discard(store, *review, retired).code == "owner_in_use", "different process activity blocks expiry");
        require(child.kill() && child.waitForProcessToFinish(5000), "terminate only own lease fixture process");
        good(RecoveryOwnerLifecycle::discard(store, *review, retired), "actual process death releases writer protection");
    }
    {
        SingleTrackDocument savedDocument; require(savedDocument.loadFile(source), "open independent real Save document");
        auto config = settingsFor(root, "saved-with-activity");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(savedDocument, config, producer), "attach real Save protection");
        require(savedDocument.moveEnvelopePoint(0, 0, 0.3f) && producer->drain(10s), "publish actual dirty owner before Save");
        const auto request = savedDocument.createSaveSnapshot(root.getChildFile("saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && savedDocument.markSaveSnapshotCommitted(request, saved), "real Save commits clean document");
        std::unique_ptr<SavedOwnerRetirement> ticket;
        good(RecoveryOwnerLifecycle::prepareSaved(savedDocument, request, saved, *producer, ticket), "real Save drains and closes writer activity");
        RecoveryStore store(config.root, ticket->ownerId()); OwnerRetirement retired;
        good(RecoveryOwnerLifecycle::retireSaved(store, ticket->frozenHead(), *ticket, retired), "activity protocol preserves ordinary exact Save retirement");
    }
    require(juce::SHA256(source).toHexString() == sourceHash && document.captureWorkspaceSnapshot()->sameContents(*image)
        && document.undo() && document.redo() && document.captureWorkspaceSnapshot()->sameContents(*image),
        "source bytes and complete executable document history remain unchanged");
    std::cout << "RecoveryExpiryTests passed (legacy preservation, idle writer, compaction/readers, interrupted terminal queue, age/cancellation, stale head, real process death, source and full history)\n";
}

void runRecoveryOwnerLifecycleTests(const juce::File& root)
{
    runRecoveryControlQuotaTests(root);
    const auto source = root.getChildFile("retirement-source.wav"); makeSource(source);
    terminalCollectionTests(root, source);
    sessionRetirementBridgeTests(root, source);
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "open original native Document for retirement");
        auto config = settingsFor(root, "retirement-saved");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "observe real committed versions");
        require(document.moveEnvelopePoint(0, 0, 0.7f), "commit older unsaved state");
        const auto old = document.createSaveSnapshot(root.getChildFile("retirement-saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
        require(document.moveEnvelopePoint(0, 0, 0.5f) && producer->drain(10s), "newer current state is durably protected");
        const auto durable = producer->health();
        RecoveryStore store(config.root, durable.owner);
        const auto oldSaved = SingleTrackDocument::writeSaveSnapshot(old);
        require(oldSaved.succeeded && document.markSaveSnapshotCommitted(old, oldSaved), "actual older Save As completes without cleaning newer edit");
        std::unique_ptr<SavedOwnerRetirement> ticket;
        require(!RecoveryOwnerLifecycle::prepareSaved(document, old, oldSaved, *producer, ticket) && !ticket,
            "older genuine save cannot authorize deleting newer dirty protection");
        const auto request = document.createSaveSnapshot();
        SingleTrackDocument::SaveWriteResult fake { true, request.targetFile, request.changeGeneration, {} };
        require(!RecoveryOwnerLifecycle::prepareSaved(document, request, fake, *producer, ticket), "fabricated success cannot produce retirement authority");
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && document.markSaveSnapshotCommitted(request, saved), "actual current save is validated and clean");
        good(RecoveryOwnerLifecycle::prepareSaved(document, request, saved, *producer, ticket), "prepare metadata-only saved-version retirement ticket");
        const auto* originalTicket = ticket.get();
        require(!RecoveryOwnerLifecycle::prepareSaved(document, request, saved, *producer, ticket) && ticket.get() == originalTicket,
            "prepare failure preserves already issued ticket");
        OwnerRetirement result; result.head.sequence = 91;
        std::shared_ptr<RecoveryOwnerLease> explicitLease;
        good(store.acquireOwnerLease(explicitLease), "hold explicit live owner dependency");
        require(explicitLease->belongsTo(config.root, durable.owner)
            && RecoveryOwnerLifecycle::retireSaved(store, durable.writer.safeHead, *ticket, result).code == "owner_in_use"
            && result.head.sequence == 91, "live dependency blocks retirement without altering caller output");
        explicitLease.reset();
        {
            SingleTrackDocument other; require(other.loadFile(source), "open same media in a distinct Document runtime");
            auto otherConfig = settingsFor(root, "retirement-distinct-document");
            std::unique_ptr<RecoveryMutationProducer> otherProducer;
            good(RecoveryMutationProducer::attachPostCore(other, otherConfig, otherProducer), "observe independent same-path Document");
            require(other.moveEnvelopePoint(0, 0, 0.3f) && otherProducer->drain(10s), "protect separate Document's real mutation");
            const auto otherHealth = otherProducer->health(); RecoveryStore otherStore(otherConfig.root, otherHealth.owner);
            require(RecoveryOwnerLifecycle::retireSaved(otherStore, otherHealth.writer.safeHead, *ticket, result).code == "retirement_owner_scope",
                "same media pathname cannot transfer retirement authority across Document runtimes");
        }
        {
            auto undoConfig = settingsFor(root, "retirement-later-undo");
            std::unique_ptr<RecoveryMutationProducer> later;
            good(RecoveryMutationProducer::attachPostCore(document, undoConfig, later), "observe next clean source epoch after saved ticket");
            require(document.undo() && later->drain(10s), "real later Undo returns to a numerically smaller Document state version");
            const auto undoHealth = later->health(); RecoveryStore undoStore(undoConfig.root, undoHealth.owner);
            require(undoHealth.observedIdentity.generation < request.documentIdentity.generation
                && RecoveryOwnerLifecycle::retireSaved(undoStore, undoHealth.writer.safeHead, *ticket, result).code == "retirement_owner_scope"
                && !undoStore.inspectManifest().retirement, "old save ticket cannot retire a later dirty owner even when Undo lowers its state number");
            require(document.redo() && later->drain(10s), "restore current saved state after independent Undo-retirement regression");
            later->close();
        }
        std::shared_ptr<const RecoveredCandidateDocument> recovered;
        std::weak_ptr<RecoveryOwnerLease> lifetime;
        {
            RecoveryRestoreCandidate::Request restore;
            restore.observedOwnerHead = durable.writer.safeHead; restore.checkpointHead = durable.writer.safeHead;
            restore.scratchDirectory = root.getChildFile("retirement-live-scratch");
            restore.attemptDirectory = root.getChildFile("retirement-live-attempts");
            restore.appBuild = "retirement-tests-v1"; restore.limits = config.graph; restore.previewSeconds = 1.0;
            require(restore.scratchDirectory.createDirectory().wasOk(), "create owned progressive candidate scratch");
            RecoveryRestoreCandidate candidate(store, restore);
            for (int steps = 0; candidate.getStage() != RecoveryAttemptStage::coreReady && steps < 24; ++steps)
                good(candidate.advance(), "advance real isolated restore until actual waveform/playable CoreReady");
            good(candidate.acquireCore(candidate.getAttemptId(), recovered), "acquire actual recovered dirty Document");
            const auto reference = ProgressiveRecoveryAudio::getDurableReference(recovered->document->getState()->streamingReader);
            require(reference && reference->lease && reference->lease->belongsTo(config.root, durable.owner), "real recovered reader retains owner usage lease");
            lifetime = reference->lease;
        }
        require(!lifetime.expired() && RecoveryOwnerLifecycle::retireSaved(store, durable.writer.safeHead, *ticket, result).code == "owner_in_use",
            "controller exit does not release CAS used by its surviving recovered Document and Undo");
        juce::ChildProcess concurrentDiscard;
        require(concurrentDiscard.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--retirement-crash", config.root.getFullPathName(), durable.owner.toString(), "0" })
            && concurrentDiscard.waitForProcessToFinish(10000) && concurrentDiscard.getExitCode() == 14,
            "actual live restored reader excludes terminal cleanup in another process too");
        recovered.reset();
        require(lifetime.expired(), "owner lease releases when actual recovered reader/history owners retire");
        bool checkedBoundary = false;
        store.onRetirementBoundary = [&] (auto boundary)
        {
            if (boundary != RecoveryStore::RetirementBoundary::beforePublish) return;
#if JUCE_WINDOWS
            const auto cannotWrite = [] (const juce::File& file)
            {
                auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); return false; }
                return true;
            };
            require(cannotWrite(request.targetFile) && cannotWrite(config.root.getChildFile(durable.owner.toString()).getChildFile("head")),
                "saved output and frozen head stay write-protected through marker publication");
#endif
            std::shared_ptr<RecoveryOwnerLease> racing;
            require(!store.acquireOwnerLease(racing) && !racing, "retirement excludes new live dependency admission");
            checkedBoundary = true;
        };
        good(RecoveryOwnerLifecycle::retireSaved(store, durable.writer.safeHead, *ticket, result), "publish saved owner terminal marker atomically");
        require(checkedBoundary && result.head == durable.writer.safeHead && result.evidence["reason"] == juce::var("saved"),
            "saved retirement binds the exact old head and genuine saved evidence");
        const auto marker = config.root.getChildFile(durable.owner.toString()).getChildFile("retired");
        juce::MemoryBlock markerBytes; require(marker.loadFileAsData(markerBytes), "retain exact successful marker bytes");
        store.onRetirementBoundary = {};
        good(RecoveryOwnerLifecycle::retireSaved(store, durable.writer.safeHead, *ticket, result), "saved retirement retry is idempotent");
        juce::MemoryBlock retried; require(marker.loadFileAsData(retried) && retried == markerBytes, "idempotent retry does not rewrite evidence/nonce");
        std::optional<OwnerRetirement> inspected;
        good(store.inspectRetirement(inspected), "independently inspect persisted retirement");
        require(inspected && store.inspectManifest().retirement && store.inspect().head == durable.writer.safeHead,
            "retired owner remains auditable with all original committed audio intact");
        require(store.checkHead(durable.writer.safeHead).code == "owner_retired" && !store.acquireOwnerLease(explicitLease),
            "retired owner cannot start another live recovery");
        auto snapshot = document.captureCommittedState(document.getMediaIdentity().document); require(snapshot.has_value(), "capture actual state for attempted retired append");
        CommitPacket packet; good(RecoveryCheckpointCodec::encode(*snapshot, {}, packet, config.graph), "prepare real checkpoint for retired-append fence");
        SafeHead rejected { 91, "preserved" };
        require(store.append(durable.writer.safeHead, packet, rejected).code == "owner_retired" && rejected == durable.writer.safeHead,
            "retired owner refuses actual new commit without advancing output head");
        std::unique_ptr<detail::DurableDirectory> directory;
        good(detail::DurableDirectory::open(marker.getParentDirectory(), false, directory), "open only owned retirement metadata for unknown-schema fault");
        juce::var json; good(directory->readJson("retired", 16384, json), "read owned marker for schema fault");
        json.getDynamicObject()->setProperty("schema", 99);
        good(directory->publishJson("retired", 16384, json, true), "inject checksummed future retirement schema");
        require(store.inspectRetirement(inspected).error == Error::unsupportedSchema
            && store.inspectManifest().status.error == Error::unsupportedSchema && !store.acquireOwnerLease(explicitLease),
            "future terminal schema is preserved and fails closed across inspection/restore");
        require(marker.getSiblingFile("head").deleteFile(), "remove only owned head to test malformed retired-owner resurrection");
        require(!store.inspectManifest().status && store.append({}, packet, rejected).error == Error::unsupportedSchema,
            "missing head never makes a terminal/future owner look like a fresh writable owner");
    }
    {
        SingleTrackDocument document; require(document.loadFile(source), "load independent target-version fault Document");
        auto config = settingsFor(root, "retirement-target-fault");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "attach target fault producer");
        require(document.moveEnvelopePoint(0, 0, 0.4f) && producer->drain(10s), "publish unsaved version before actual Save");
        const auto old = producer->health(); RecoveryStore store(config.root, old.owner);
        const auto request = document.createSaveSnapshot(root.getChildFile("retirement-target-fault.wav"), SingleTrackDocument::SaveIntent::saveAs);
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && document.markSaveSnapshotCommitted(request, saved), "publish actual target for retirement version fault");
        std::unique_ptr<SavedOwnerRetirement> ticket; good(RecoveryOwnerLifecycle::prepareSaved(document, request, saved, *producer, ticket), "capture real saved version before fault");
#if JUCE_WINDOWS
        const auto facts = saved.publication->targetVersion();
        HANDLE changed = CreateFileW(request.targetFile.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        require(changed != INVALID_HANDLE_VALUE, "open only owned unheld saved target");
        LARGE_INTEGER offset; offset.QuadPart = facts->sizeBytes - 1;
        unsigned char byte = 0; DWORD count = 0;
        require(SetFilePointerEx(changed, offset, nullptr, FILE_BEGIN) && ReadFile(changed, &byte, 1, &count, nullptr) && count == 1,
            "read last owned saved byte"); byte ^= 1;
        FILETIME stamp { static_cast<DWORD>(facts->modifiedTicks), static_cast<DWORD>(static_cast<uint64_t>(facts->modifiedTicks) >> 32) };
        require(SetFilePointerEx(changed, offset, nullptr, FILE_BEGIN) && WriteFile(changed, &byte, 1, &count, nullptr) && count == 1
            && FlushFileBuffers(changed) && SetFileTime(changed, nullptr, nullptr, &stamp) && CloseHandle(changed), "modify saved bytes under same ID/size/exact LastWriteTime");
#endif
        OwnerRetirement terminal;
        require(RecoveryOwnerLifecycle::retireSaved(store, old.writer.safeHead, *ticket, terminal).code == "retirement_saved_target_changed",
            "restamped external save mutation cannot authorize retirement");
        std::optional<OwnerRetirement> absence; good(store.inspectRetirement(absence), "inspect failed retirement absence");
        require(!absence && store.inspect().head == old.writer.safeHead, "failed saved proof retains every recovery record");
        auto wrongHead = old.writer.safeHead; ++wrongHead.sequence;
        require(!RecoveryOwnerLifecycle::discard(store, wrongHead, terminal), "explicit discard still rejects stale selected head");
        const auto usage = config.root.getChildFile(old.owner.toString()).getChildFile("usage.lease");
        require(usage.replaceWithText("unexpected contents"), "inject only owned malformed usage file");
        require(RecoveryOwnerLifecycle::discard(store, old.writer.safeHead, terminal).code == "owner_usage_contract"
            && usage.loadFileAsString() == "unexpected contents", "malformed lock input is preserved rather than truncated or treated as unused");
        require(usage.deleteFile(), "remove only injected malformed usage fixture");
        good(RecoveryOwnerLifecycle::discard(store, old.writer.safeHead, terminal), "explicit exact-owner discard can retire independently of changed source");
        require(terminal.evidence["reason"] == juce::var("discarded") && store.inspect().head == old.writer.safeHead,
            "Discard records intent without deleting source or yet collecting owner data");
    }
    for (int stage = 0; stage < 2; ++stage)
    {
        SingleTrackDocument document; require(document.loadFile(source), "load real Document for retirement kill boundary");
        auto config = settingsFor(root, stage == 0 ? "retirement-crash-before" : "retirement-crash-after");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "observe actual kill-test edit");
        require(document.moveEnvelopePoint(0, 0, 0.6f) && producer->drain(10s), "publish actual PCM/history before kill");
        const auto protectedState = producer->health(); producer->close();
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--retirement-crash", config.root.getFullPathName(), protectedState.owner.toString(), juce::String(stage) })
            && child.waitForProcessToFinish(10000) && child.getExitCode() == 87, "kill only owned console child at real retirement boundary");
        RecoveryStore store(config.root, protectedState.owner);
        std::optional<OwnerRetirement> retired; good(store.inspectRetirement(retired), "inspect retirement after process death");
        require(retired.has_value() == (stage == 1) && store.inspect().head == protectedState.writer.safeHead,
            "death exposes only active old owner or complete terminal marker, all data remain valid");
        OwnerRetirement resumed; good(RecoveryOwnerLifecycle::discard(store, protectedState.writer.safeHead, resumed), "restart safely resumes or reuses actual terminal transaction");
    }
    std::cout << "RecoveryOwnerLifecycleTests passed (real Save proof, live recovered leases, target/head fences, idempotence, terminal schema, Discard, 2 kill boundaries)\n";
}
