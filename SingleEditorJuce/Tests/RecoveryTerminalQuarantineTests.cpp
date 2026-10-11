#include "Recovery/RecoveryOwnerLifecycle.h"
#include "Recovery/RecoveryMutationProducer.h"
#include "Recovery/RecoveryDurableFiles.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Terminal quarantine: " << status.code << '\n'; require(static_cast<bool>(status), message); }
void makeSource(const juce::File& source)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create only owned quarantine audio");
    juce::AudioBuffer<float> pcm(2, 1200);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < pcm.getNumSamples(); ++i) pcm.setSample(c, i, ((i * 19 + c) % 89 - 44) / 100.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write quarantine fixture samples");
}
void finish(RecoveryStore& store, const SafeHead& head)
{
    for (int step = 0; step < 128; ++step)
    {
        TerminalCollectionReport report;
        good(store.collectTerminal(head, report, { 1, 1024 * 1024 }), "resume terminal owner containing verified orphan quarantine");
        require(report.filesRemoved <= 1 && (report.more || report.completed), "quarantine uses the same bounded deletion protocol");
        if (report.completed) return;
    }
    require(false, "quarantine sweep converges within bounded fixture calls");
}
void mutateRestamped(const juce::File& file)
{
#if JUCE_WINDOWS
    const auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME modified{}; DWORD count = 0; unsigned char value = 0; LARGE_INTEGER start{};
    require(handle != INVALID_HANDLE_VALUE && GetFileTime(handle, nullptr, nullptr, &modified)
        && ReadFile(handle, &value, 1, &count, nullptr) && count == 1, "open only owned quarantine item to inject rewrite");
    value ^= 1;
    require(SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) && WriteFile(handle, &value, 1, &count, nullptr) && count == 1
        && FlushFileBuffers(handle) && SetFileTime(handle, nullptr, nullptr, &modified) && CloseHandle(handle),
        "change quarantine bytes under identical ID/size/mtime");
#else
    juce::ignoreUnused(file);
#endif
}
}

int runRecoveryQuarantineCrashChild(const juce::File& root, const juce::Uuid& owner, int stage)
{
    RecoveryStore store(root, owner);
    const auto view = store.inspectManifest();
    if (!view.status || view.commits.empty()) return 20;
    store.onCollectionBoundary = [stage] (auto boundary)
    { if (static_cast<int>(boundary) == stage) std::_Exit(90); };
    OrphanCollectionReport report;
    return store.collectOrphans(view.head, report) ? 21 : 22;
}

void runRecoveryTerminalQuarantineTests(const juce::File& root)
{
    const auto source = root.getChildFile("quarantine-source.wav"); makeSource(source);
    // Publish genuine canonical CAS through a separate fixture Store. Copying
    // its bytes into this owner's unreferenced set exercises real asset GC,
    // without inventing a plan or removing any on-chain record/reference.
    const auto seedRoot = root.getChildFile("quarantine-cas-seed"); const juce::Uuid seedOwner;
    RecoveryStore seed(seedRoot, seedOwner);
    CommitPacket packet; packet.identity.document = { juce::Uuid(), juce::Uuid(), 1 };
    packet.identity.source.binding = juce::Uuid(); packet.operationId = "test.quarantine-cas";
    packet.planJson = R"({"version":1})"; packet.stateJson = R"({"version":1})"; packet.timestampMilliseconds = 1234;
    AudioBlock audio { 48000, 2, 8, {} }; juce::MemoryOutputStream samples;
    for (int i = 0; i < 16; ++i) samples.writeFloat(static_cast<float>(i - 7) / 32.0f);
    audio.samples = samples.getMemoryBlock(); packet.newAudio.push_back(audio);
    juce::String asset; good(RecoveryStore::identifyAudio(audio, Limits{}.assetBytes, asset), "identify genuine independent seed CAS");
    SafeHead seedHead; good(seed.append({}, packet, seedHead), "publish independent canonical CAS fixture");
    const auto seedFile = seedRoot.getChildFile(seedOwner.toString()).getChildFile("assets").getChildFile(asset);
    // Five actual old-GC kill states, malformed/ambiguous evidence, and terminal
    // cancellation/kill after its independent durable authority was published.
    for (int scenario = 0; scenario < 26; ++scenario)
    {
        SingleTrackDocument document; require(document.loadFile(source), "load actual quarantine test Document");
        RecoveryMutationProducer::Config config; config.root = root.getChildFile("terminal-quarantine-" + juce::String(scenario));
        config.queue.maximumAge = 10s;
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, config, producer), "attach actual quarantine test producer");
        require(document.moveEnvelopePoint(0, 0, 0.7f) && document.moveEnvelopePoint(0, 0, 0.4f) && producer->drain(10s),
            "publish complete real current and Undo graph");
        const auto state = producer->health(); producer->close(); producer.reset();
        const auto owner = state.owner; const auto head = state.writer.safeHead;
        const auto directory = config.root.getChildFile(owner.toString()), quarantine = directory.getChildFile("quarantine");
        const auto original = scenario >= 21 ? directory.getChildFile("assets").getChildFile(asset)
                                             : directory.getChildFile("staging-" + juce::Uuid().toString());
        require(scenario >= 21 ? seedFile.copyFileTo(original) : original.replaceWithText("known failed staging bytes"),
            "create only owned unreferenced canonical CAS or failed staging fixture");
        juce::ChildProcess child;
        const int stage = scenario < 5 ? scenario : scenario >= 21 ? scenario - 21 : 1;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--quarantine-crash", config.root.getFullPathName(), owner.toString(), juce::String(stage) })
            && child.waitForProcessToFinish(15000) && child.getExitCode() == 90, "actual prior GC process terminates at selected boundary");
        RecoveryStore store(config.root, owner);
        require(store.inspect().head == head, "all prior GC crash states preserve complete current and Undo chain");
        auto plans = quarantine.findChildFiles(juce::File::findFiles, false, "plan-*");
        auto objects = quarantine.findChildFiles(juce::File::findFiles, false, "item-*");
        if (scenario >= 5 && scenario <= 20) require(plans.size() == 1 && objects.size() == 1, "fault fixture has genuine persisted plan and quarantined object");
        if (scenario >= 5 && scenario <= 8)
        {
            std::unique_ptr<detail::DurableDirectory> parent;
            good(detail::DurableDirectory::open(quarantine, false, parent), "pin only owned quarantine metadata fixture");
            juce::var json; good(parent->readJson(plans[0].getFileName(), 4096, json), "read genuine prior GC plan");
            if (scenario == 5) json.getDynamicObject()->setProperty("schema", 99);
            if (scenario == 6) json.getDynamicObject()->setProperty("owner", juce::Uuid().toString());
            if (scenario == 7) json.getDynamicObject()->setProperty("head", juce::String::repeatedString("a", 64));
            if (scenario == 8) json.getDynamicObject()->setProperty("content", juce::String::repeatedString("b", 64));
            good(parent->publishJson(plans[0].getFileName(), 4096, json, true), "publish checksummed unsupported or mismatched old plan");
        }
        if (scenario == 9) mutateRestamped(objects[0]);
        if (scenario == 10) require(objects[0].copyFileTo(original), "inject ambiguous original plus quarantined file");
        if (scenario == 11) require(plans[0].deleteFile(), "leave only owned unknown unplanned quarantine object");
        if (scenario == 12) require(quarantine.getChildFile("future-data").replaceWithText("preserve"), "inject unknown quarantine file");
        if (scenario == 13)
        {
            const auto replacement = root.getChildFile("quarantine-replacement");
            require(objects[0].copyFileTo(replacement) && objects[0].deleteFile() && replacement.moveFileTo(objects[0]),
                "replace only owned quarantined object with byte-identical different file ID");
        }
        OwnerRetirement retired;
        good(RecoveryOwnerLifecycle::discard(store, head, retired), "explicit actual selected-owner Discard publishes terminal marker");
        const auto intent = config.root.getChildFile("terminal-" + owner.toString());
        TerminalCollectionReport report;
        if (scenario >= 5 && scenario <= 13)
        {
            const auto rejected = store.collectTerminal(head, report);
            require(!rejected && report.filesRemoved == 0 && !intent.exists() && store.inspect().head == head,
                "unknown/future/changed old quarantine preserves whole owner before terminal intent");
            if (scenario == 5) require(rejected.error == Error::unsupportedSchema, "future orphan schema is explicitly unsupported");
            continue;
        }
        good(store.collectTerminal(head, report), "certify genuine old quarantine into terminal inventory");
        require(report.more && report.filesRemoved == 0 && intent.exists(), "certification remains separate from bounded sweep");
        if (scenario == 18 || scenario == 19)
        {
            std::unique_ptr<detail::DurableDirectory> parent;
            good(detail::DurableDirectory::open(scenario == 18 ? config.root : quarantine, false, parent), "pin owned metadata between terminal batches");
            const auto name = scenario == 18 ? intent.getFileName() : plans[0].getFileName();
            juce::var json;
            if (scenario == 18)
            {
                // The general metadata JSON reader intentionally supports only
                // schema 1. Decode this test-owned terminal-2 envelope directly
                // to inject a checksummed schema fault, without loosening it.
                juce::MemoryBlock framed;
                require(intent.loadFileAsData(framed) && framed.getSize() > 40, "read only owned schema-2 fault fixture");
                const auto* data = static_cast<const char*>(framed.getData());
                const juce::MemoryBlock payload(data + 40, framed.getSize() - 40);
                const auto hash = juce::SHA256(payload).getRawData();
                require(std::memcmp(data, "SER1", 4) == 0 && std::memcmp(data + 8, hash.getData(), 32) == 0
                    && juce::JSON::parse(juce::String::fromUTF8(data + 40, static_cast<int>(payload.getSize())), json).wasOk(),
                    "validate genuine terminal payload before owned schema fault");
            }
            else good(parent->readJson(name, 1024 * 1024, json), "read current owned durable plan before schema fault");
            json.getDynamicObject()->setProperty("schema", scenario == 18 ? 1 : 99);
            good(parent->publishJson(name, 1024 * 1024, json, true), "publish only owned downgraded terminal or future old orphan plan");
            parent.reset();
            require(!store.collectTerminal(head, report) && report.filesRemoved == 0 && objects[0].exists() && plans[0].exists(),
                "old terminal schema cannot authorize quarantine; replaced orphan plan is rejected before any batch deletion");
            continue;
        }
        if (scenario == 20)
        {
            bool guarded = false;
            store.onTerminalBoundary = [&] (auto boundary)
            {
                if (boundary != RecoveryStore::TerminalBoundary::beforeDelete || guarded) return;
#if JUCE_WINDOWS
                const auto write = CreateFileW(plans[0].getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                require(write == INVALID_HANDLE_VALUE, "all surviving orphan plans remain write-excluded through each terminal batch");
#endif
                guarded = true;
            };
            good(store.collectTerminal(head, report, { 1, 1024 * 1024 }), "sweep one item while holding actual old-plan guard");
            require(guarded, "exercise actual predelete quarantine plan exclusion");
            store.onTerminalBoundary = {};
        }
        if (scenario == 14)
        {
            mutateRestamped(objects[0]);
            // Earlier known payload may already be swept, but the changed old
            // quarantine object itself must remain guarded by its inventory.
            const auto rejected = store.collectTerminal(head, report);
            require(!rejected && objects[0].exists() && intent.exists(), "between-batch rewrite is never deleted as old quarantine");
            continue;
        }
        if (scenario == 15)
        {
            bool stop = false;
            store.onTerminalBoundary = [&] (auto boundary)
            { if (boundary == RecoveryStore::TerminalBoundary::itemDeleted && !objects[0].exists()) stop = true; };
            require(store.collectTerminal(head, report, {}, [&] { return stop; }).error == Error::cancelled
                && !objects[0].exists() && plans[0].exists() && intent.exists(), "cancel after quarantined object deletion keeps resumable independent plan");
            store.onTerminalBoundary = {};
        }
        if (scenario == 16 || scenario == 17)
        {
            juce::ChildProcess terminal;
            const int boundary = scenario == 16 ? static_cast<int>(RecoveryStore::TerminalBoundary::markerRemoved)
                                                : static_cast<int>(RecoveryStore::TerminalBoundary::ownerRemoved);
            require(terminal.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                "--terminal-crash", config.root.getFullPathName(), owner.toString(), juce::String(head.sequence), head.hash, juce::String(boundary) })
                && terminal.waitForProcessToFinish(15000) && terminal.getExitCode() == 88, "actual terminal kill after old quarantine and original markers were removed");
        }
        finish(store, head);
        require(!directory.exists() && !intent.exists() && document.canUndo(), "complete known terminal collection preserves live independent Document history");
    }
    require(seed.inspect().head == seedHead && source.exists(), "terminal quarantine never touches another owner or original source");
    std::cout << "RecoveryTerminalQuarantineTests passed (26 scenarios, staging/CAS at 5 real orphan crash states, exact old plans, future/unknown/mismatch preservation, bounded cancellation, terminal kill resume)\n";
}
