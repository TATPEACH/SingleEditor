#include "Recovery/RecoverySessionHost.h"
#include "Recovery/RecoveryStorageLayout.h"
#include "Recovery/RecoveryAttemptStore.h"
#include "Recovery/RecoveryMaintenanceQueue.h"
#include "Recovery/RecoveryDurableFiles.h"
#include "Core/NativeFilePath.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Session host: " << status.code << '\n'; require(static_cast<bool>(status), message); }
template <typename Predicate> void await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    require(predicate(), message);
}
void makeSource(const juce::File& path)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(path.createOutputStream());
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create isolated session host audio fixture");
    juce::AudioBuffer<float> pcm(2, 12000);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < pcm.getNumSamples(); ++i) pcm.setSample(c, i, ((i * 17 + c) % 131 - 65) / 150.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write session host source PCM");
}
RecoverySessionHost::Config config(const juce::File& root, const char* child)
{
    RecoverySessionHost::Config value;
    value.session.producer.root = root.getChildFile(child);
    value.session.producer.queue.maximumAge = 30s; // Controlled worker gates, not a latency benchmark.
    value.session.producer.queue.diagnostics = std::make_shared<RecoveryDiagnosticState>();
    return value;
}
RecoveryDiagnosticState::Failure diagnosticFailure(const RecoverySessionHost::Config& settings)
{
    std::optional<RecoveryDiagnosticState::Failure> result;
    require(settings.session.producer.queue.diagnostics->trySnapshot(result) && result.has_value(), "actual Host result has diagnostics");
    return *result;
}
RecoveryMutationProducer::Health current(const RecoverySessionHost& host)
{
    const auto value = host.health();
    for (const auto& item : value.sessions) if (item.active) return item.protection.current;
    require(false, "test expects an active admitted session"); return {};
}
void protect(RecoverySessionHost& host)
{
    await([&]
    {
        const auto value = current(host);
        if (!value.writer.workerRunning && !value.failure)
        {
            std::cerr << "Protection failed: " << value.failure.code << " owner=" << value.owner.toString()
                << " pending=" << value.writer.pendingPackets << '\n';
            require(false, "actual queued recovery retains a reported failure");
        }
        return value.writer.pendingPackets == 0 && !value.writer.workerRunning;
    },
        "actual queued recovery reaches a settled worker");
    const auto value = host.health();
    require(value.failure && value.sessions.front().protection.currentProtected, "actual exact current version is durably protected");
}
void save(DocumentSaveCoordinator& coordinator, SingleTrackDocument::SaveSnapshot snapshot,
          DocumentSaveCoordinator::Completion completion = {})
{
    bool done = false;
    require(coordinator.requestSave(std::move(snapshot), [&] (const auto& result)
    {
        if (!result.succeeded) std::cerr << "Actual Save failed: " << result.errorMessage << '\n';
        require(result.succeeded && result.publication != nullptr, "real asynchronous Save publishes its physical receipt");
        if (completion) completion(result);
        done = true;
    }), "accept actual asynchronous Save");
    await([&] { return done && !coordinator.isSaveInProgress(); }, "actual Save completes on message thread");
}
struct Observer final : DocumentSaveCoordinator::Listener
{
    std::function<void(const SingleTrackDocument::SaveSnapshot&, const DocumentSaveCoordinator::Result&, bool)> callback;
    int calls = 0;
    void saveCompleted(const SingleTrackDocument::SaveSnapshot& snapshot, const DocumentSaveCoordinator::Result& result, bool adopted) override
    { ++calls; if (callback) callback(snapshot, result, adopted); }
};
void savedTicketBeforeSwitch(const juce::File& root, const juce::File& source)
{
    std::vector<std::unique_ptr<SavedOwnerRetirement>> tickets;
    const auto settings = config(root, "saved-before-switch");
    juce::Uuid expectedOwner(nullptr); SafeHead expectedHead;
    {
        SingleTrackDocument document; require(document.loadFile(source), "load clean document for inert PostCore");
        DocumentSaveCoordinator coordinator(document);
        RecoverySessionHost host(document, coordinator, settings);
        const juce::Uuid entry;
        good(host.attachPostCore(entry), "attach clean PostCore host");
        good(host.attachPostCore(entry), "same actual document and row admission is idempotent");
        require(host.attachPostCore(juce::Uuid()).code == "host_entry_changed", "different row cannot relabel active document protection");
        host.poll();
        require(host.health().sessions.size() == 1 && current(host).owner.isNull()
            && host.health().pendingPackets == 0 && !settings.session.producer.root.exists(),
            "construction, clean PostCore, duplicate admission and polling create no root or worker");
        require(!document.moveEnvelopePoint(0, 0, 1.0f) && !settings.session.producer.root.exists(), "no-op mutation remains inert");
        require(document.moveEnvelopePoint(0, 0, 0.7f) && document.moveEnvelopePoint(0, 0, 0.5f), "real edits begin automatic capture");
        protect(host); expectedOwner = current(host).owner; expectedHead = current(host).writer.safeHead;
        const auto snapshot = document.createSaveSnapshot(root.getChildFile("host-saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
        const auto state = document.getState();
        Observer observer;
        observer.callback = [&] (const auto& actual, const auto& result, bool adopted)
        {
            require(adopted && result.succeeded && result.publication && actual.state == state
                && actual.requestSerial == snapshot.requestSerial && !document.hasUnsavedChanges()
                && host.health().pendingRetirements == 1, "observer receives original snapshot after actual adoption and host captures ticket before user switch");
        };
        coordinator.addListener(&observer);
        save(coordinator, snapshot, [&] (const auto&)
        {
            require(observer.calls == 1 && document.canUndo(), "receipt listener runs once before user callback without dropping history");
            document.clear();
        });
        coordinator.removeListener(&observer);
        host.poll();
        require(host.health().sessions.empty(), "replacement releases only genuinely drained closed sessions");
        tickets = host.takeSavedRetirements();
        require(tickets.size() == 1 && tickets.front()->ownerId() == expectedOwner
            && tickets.front()->frozenHead() == expectedHead, "detached ticket preserves exact root owner and head");
        const auto released = host.takeReleasedEpochs();
        require(released.size() == 1 && released.front().entry == entry && released.front().protection.owner == expectedOwner,
            "metadata release retains originating row and actual actor facts");
    }
    RecoveryStore store(settings.session.producer.root, expectedOwner);
    OwnerRetirement retirement;
    good(RecoveryOwnerLifecycle::retireSaved(store, expectedHead, *tickets.front(), retirement),
        "actual Save ticket remains usable after Document coordinator and host destruction");
    bool complete = false;
    for (int i = 0; i < 128 && !complete; ++i)
    {
        TerminalCollectionReport report;
        good(store.collectTerminal(expectedHead, report, { 1, 1024 * 1024 }), "worker consumes exact ticket in bounded resumable collection");
        require(report.filesRemoved <= 1, "host ticket does not bypass collection work budget"); complete = report.completed;
    }
    require(complete && !settings.session.producer.root.getChildFile(expectedOwner.toString()).exists(),
        "saved terminal owner is actually removed while saved output survives");
    require(root.getChildFile("host-saved.wav").existsAsFile(), "owner collection never removes published Save target");
}
void saveScopes(const juce::File& root, const juce::File& source)
{
    for (int mode = 0; mode < 3; ++mode)
    {
        SingleTrackDocument document; require(document.loadFile(source), "load Save scope document");
        juce::WaitableEvent started, release;
        DocumentSaveCoordinator coordinator(document, [&] (const auto& snapshot)
        {
            started.signal(); require(release.wait(15000), "release controlled real Save writer");
            return SingleTrackDocument::writeSaveSnapshot(snapshot);
        });
        const auto settings = config(root, ("scope-" + juce::String(mode)).toRawUTF8());
        RecoverySessionHost host(document, coordinator, settings);
        good(host.attachPostCore(juce::Uuid()), "observe Save scope document");
        require(document.moveEnvelopePoint(0, 0, 0.8f), "first Save scope edit"); protect(host);
        const auto oldOwner = current(host).owner;
        const auto request = document.createSaveSnapshot(root.getChildFile("scope-" + juce::String(mode) + ".wav"),
            mode == 2 ? SingleTrackDocument::SaveIntent::exportCopy : SingleTrackDocument::SaveIntent::saveAs);
        Observer observer;
        observer.callback = [&] (const auto& actual, const auto& result, bool adopted)
        {
            require(actual.requestSerial == request.requestSerial && result.succeeded && result.publication
                && adopted == (mode == 0), "receipt distinguishes older adopted checkpoint, obsolete document and export");
        };
        coordinator.addListener(&observer);
        bool done = false;
        require(coordinator.requestSave(request, [&] (const auto& result) { require(result.succeeded, "scope Save publishes"); done = true; }), "start real scope Save");
        require(started.wait(5000), "real Save is in flight");
        if (mode == 1)
        {
            document.clear(); require(document.loadFile(source), "same path creates different actual Document identity");
            good(host.attachPostCore(juce::Uuid()), "replacement admits independent PostCore session");
        }
        require(document.moveEnvelopePoint(0, 0, 0.4f), "later edits remain unsaved across completion");
        const auto later = document.getMediaIdentity().document;
        release.signal(); await([&] { return done && !coordinator.isSaveInProgress(); }, "scope Save finishes");
        coordinator.removeListener(&observer);
        require(observer.calls == 1 && document.hasUnsavedChanges() && document.getMediaIdentity().document.sameVersion(later)
            && host.health().failure && host.takeSavedRetirements().empty(),
            "older/obsolete/export Save neither discards latest edits nor falsely degrades recovery");
        for (const auto& item : host.health().sessions)
        {
            require(item.protection.current.writer.cancellation == AsyncRecoveryWriter::Cancellation::none, "current recovery queue remains uncancelled");
            for (const auto& epoch : item.protection.retired)
                require(epoch.writer.cancellation == AsyncRecoveryWriter::Cancellation::none, "prior accepted epoch remains uncancelled");
        }
        protect(host); host.beginClose();
        await([&] { host.poll(); return host.health().settled; }, "Save scope accepted queues settle without dropping work");
        RecoveryStore oldStore(settings.session.producer.root, oldOwner);
        require(oldStore.inspect().status && !settings.session.producer.root.getChildFile(oldOwner.toString()).getChildFile("retired").exists(),
            "no terminal marker from older/obsolete/export publication");
    }
}
void pendingCancellation(const juce::File& root, const juce::File& source)
{
    for (int mode = 0; mode < 2; ++mode)
    {
        auto settings = config(root, ("pending-" + juce::String(mode)).toRawUTF8());
        juce::WaitableEvent preparing, release;
        std::atomic<int> prepares { 0 };
        if (mode == 0) settings.session.producer.queue.onBoundary = [&] (auto boundary)
        {
            if (boundary == AsyncRecoveryWriter::Boundary::beforePrepare && ++prepares == 1)
            { preparing.signal(); require(release.wait(15000), "release blocked in-flight recovery packet"); }
        };
        else require(settings.session.producer.root.replaceWithText("owned obstruction"), "inject real recovery filesystem failure");
        SingleTrackDocument document; require(document.loadFile(source), "open pending queue document");
        DocumentSaveCoordinator coordinator(document);
        RecoverySessionHost host(document, coordinator, settings);
        good(host.attachPostCore(juce::Uuid()), "observe pending queue document");
        require(document.moveEnvelopePoint(0, 0, 0.8f), "enqueue first actual mutation");
        if (mode == 0) require(preparing.wait(5000), "recovery writer is in flight");
        else await([&] { return !current(host).writer.workerRunning && !current(host).writer.failure; }, "actual filesystem failure retains first packet");
        require(document.moveEnvelopePoint(0, 0, 0.6f), "enqueue second mutation");
        const auto higher = document.getMediaIdentity().document.generation;
        require(document.undo() && document.getMediaIdentity().document.generation < higher && document.canRedo(),
            "Save uses a lower-numbered exact Undo version");
        const auto owner = current(host).owner;
        save(coordinator, document.createSaveSnapshot(root.getChildFile("pending-saved-" + juce::String(mode) + ".wav"), SingleTrackDocument::SaveIntent::saveAs),
            [&] (const auto&) { document.clear(); });
        host.poll();
        require(host.takeSavedRetirements().empty(), "pending/degraded Save cancellation never invents a disk retirement ticket");
        if (mode == 0)
        {
            require(host.health().sessions.size() == 1 && host.health().pendingPackets == 3 && host.health().retainedBytes > 0,
                "switch retains exact in-flight and queued captures until worker cancellation completes");
            release.signal();
        }
        host.beginClose();
        await([&] { host.poll(); return host.health().settled; }, "worker finishes in-flight prefix then releases cancelled tail");
        const auto released = host.takeReleasedEpochs();
        require(released.size() == 1 && released.front().protection.owner == owner
            && released.front().protection.writer.cancellation == AsyncRecoveryWriter::Cancellation::completed
            && released.front().protection.writer.safeHead.sequence == (mode == 0 ? 1 : 0)
            && released.front().protection.writer.cancelledPackets == (mode == 0 ? 2u : 3u),
            "runtime metadata reports exact cancelled packets and genuine durable prefix");
        require(!settings.session.producer.root.getChildFile(owner.toString()).getChildFile("retired").exists(),
            "runtime cancellation cannot publish terminal storage authority");
    }
}
void switchingAndClose(const juce::File& root, const juce::File& source)
{
    auto settings = config(root, "bounded-sessions"); settings.residentSessions = 1;
    require(settings.session.producer.root.replaceWithText("owned obstruction"), "create failure for departed session retention");
    SingleTrackDocument document; require(document.loadFile(source), "open first bounded host document");
    DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
    const juce::Uuid firstEntry, nextEntry;
    good(host.attachPostCore(firstEntry), "admit first bounded session");
    require(document.moveEnvelopePoint(0, 0, 0.7f), "queue old document mutation");
    await([&] { return !current(host).writer.workerRunning && !current(host).writer.failure; }, "old actor enters actual degraded state");
    const auto oldOwner = current(host).owner;
    document.clear(); require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.4f), "replace with separately dirty same-path document");
    const auto nextImage = document.captureWorkspaceSnapshot();
    const auto nextContents = document.captureCommittedState(document.getMediaIdentity().document);
    require(nextImage && nextContents, "capture actual full history oracle before bounded close");
    require(host.attachPostCore(nextEntry).code == "host_session_budget" && host.health().sessions.size() == 1
        && host.health().sessions.front().entry == firstEntry && host.health().pendingPackets == 1,
        "session capacity refuses admission without dropping departed failed queue");
    const auto capacityFailure = diagnosticFailure(settings);
    require(capacityFailure.stage == RecoveryDiagnosticState::Stage::hostAdmission && capacityFailure.error == Error::budgetExceeded
        && capacityFailure.counters[0] == 1, "actual Host capacity refusal records the retained departed actor count");
    host.beginClose();
    require(!host.health().settled && host.health().closing && host.takeReleasedEpochs().empty(), "bounded close stays unresolved while accepted packet failed");
    require(settings.session.producer.root.deleteFile(), "remove only exact injected obstruction file");
    good(host.retry(oldOwner), "explicit retry locates closed departed actor");
    await([&] { host.poll(); return host.health().settled; }, "closed actor actually replays accepted packet before release");
    require(host.takeReleasedEpochs().size() == 1, "departed retry produces one exact metadata disposition");
    good(host.cancelClose(nextEntry), "cancel close admits actual dirty current image after capacity recovers");
    require(diagnosticFailure(settings).operationId == capacityFailure.operationId,
        "successful close cancellation and readmission retain earlier Host failure");
    require(host.health().failure && !host.health().closing && document.getState() == nextImage->state() && document.canUndo(),
        "cancel close preserves actual current and complete Undo without reloading file");
    protect(host); const auto newOwner = current(host).owner;
    require(newOwner != oldOwner, "same source path does not merge independent document ownership");
    RecoveryStore store(settings.session.producer.root, newOwner);
    const auto view = store.inspect(); good(view.status, "inspect fresh checkpoint after close cancellation");
    SingleTrackDocument::CommittedStateSnapshot restored;
    good(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored), "decode actual admitted image from durable CAS");
    require(restored.identity.document.sameVersion(document.getMediaIdentity().document)
        && restored.undo.size() == nextContents->undo.size() && restored.redo.size() == nextContents->redo.size()
        && restored.state->envelopePoints.front().gain == document.getState()->envelopePoints.front().gain,
        "fresh checkpoint retains actual dirty state and Undo/Redo across close cancellation");
    restored = {}; host.beginClose(); await([&] { host.poll(); return host.health().settled; }, "new session closes only after genuine drain");
}
void coordinatorObserverLifecycle(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load listener lifecycle source");
    juce::WaitableEvent started, release; std::atomic<int> writes { 0 };
    DocumentSaveCoordinator coordinator(document, [&] (const auto& request)
    {
        if (++writes == 1) { started.signal(); require(release.wait(15000), "release first coalesced writer"); }
        return SingleTrackDocument::writeSaveSnapshot(request);
    });
    Observer observer, removed; std::vector<uint64_t> observed;
    observer.callback = [&] (const auto& request, const auto& result, bool adopted)
    { require(result.succeeded && result.publication && !adopted, "real exports carry receipt without adopted checkpoint"); observed.push_back(request.requestSerial); };
    coordinator.addListener(&observer); coordinator.addListener(&removed);
    auto snapshot = [&] (int index) { return document.createSaveSnapshot(root.getChildFile("coalesced-" + juce::String(index) + ".wav"), SingleTrackDocument::SaveIntent::exportCopy); };
    const auto first = snapshot(1), second = snapshot(2), third = snapshot(3);
    int callbacks = 0;
    const auto completion = [&] (const auto& result) { require(result.succeeded, "coalesced callback receives actual completed output"); ++callbacks; };
    require(coordinator.requestSave(first, completion) && started.wait(5000)
        && coordinator.requestSave(second, completion) && coordinator.requestSave(third, completion), "queue three requests with latest pending write coalesced");
    coordinator.removeListener(&removed); release.signal();
    await([&] { return !coordinator.isSaveInProgress(); }, "coalesced actual writes finish");
    coordinator.removeListener(&observer);
    require(writes == 2 && callbacks == 3 && observer.calls == 2 && removed.calls == 0
        && observed == std::vector<uint64_t> { first.requestSerial, third.requestSerial }
        && !root.getChildFile("coalesced-2.wav").exists(), "observer reports only two actual writes, never displaced request or removed listener");
}
void repairedGapAtClose(const juce::File& root, const juce::File& source)
{
    for (int replaced = 0; replaced < 2; ++replaced)
    {
        auto settings = config(root, ("gap-close-" + juce::String(replaced)).toRawUTF8());
        settings.session.producer.queue.packets = 1;
        juce::WaitableEvent started, release; std::atomic<int> prepares { 0 };
        settings.session.producer.queue.onBoundary = [&] (auto boundary)
        {
            if (boundary == AsyncRecoveryWriter::Boundary::beforePrepare && ++prepares == 1)
            { started.signal(); require(release.wait(15000), "release gap fixture's accepted front packet"); }
        };
        SingleTrackDocument document; require(document.loadFile(source), "load capture-gap close fixture");
        DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
        good(host.attachPostCore(juce::Uuid()), "observe gap fixture");
        require(document.moveEnvelopePoint(0, 0, 0.8f) && started.wait(5000)
            && document.moveEnvelopePoint(0, 0, 0.6f) && current(host).checkpointRequired,
            "bounded packet queue really misses a committed mutation while first write is in flight");
        const auto gapOwner = current(host).owner;
        release.signal();
        await([&] { return !current(host).writer.workerRunning; }, "accepted gap prefix actually finishes");
        good(host.checkpointCurrent(), "fresh full checkpoint captures missing edit and complete history");
        await([&] { return !current(host).writer.workerRunning && current(host).writer.pendingPackets == 0; }, "repair checkpoint becomes durable before close");
        require(current(host).writer.protectedIdentity.sameVersion(document.getMediaIdentity().document), "repair protects actual current version");
        if (replaced) document.clear();
        host.beginClose(); host.poll();
        require(host.health().settled, "closing or replacement after durable repair releases superseded runtime gap without needing active observation");
        const auto released = host.takeReleasedEpochs();
        require(released.size() == 2, "closed repaired session reports both exact actor dispositions");
        const auto old = std::find_if(released.begin(), released.end(), [&] (const auto& item) { return item.protection.owner == gapOwner; });
        require(old != released.end() && old->protection.checkpointRequired && !old->protection.failure,
            "runtime release preserves original missed-event failure in metadata");
        RecoveryStore store(settings.session.producer.root, gapOwner);
        require(store.inspect().status && !settings.session.producer.root.getChildFile(gapOwner.toString()).getChildFile("retired").exists(),
            "superseded runtime gap release cannot delete durable prefix or publish terminal marker");
    }
}
void cancelledGapClose(const juce::File& root, const juce::File& source)
{
    for (const bool replaced : { false, true })
    for (const bool pendingFailure : { false, true })
    {
        const auto name = juce::String(replaced ? "gap-cancel-replaced" : "gap-cancel-same") + (pendingFailure ? "-pending" : "-drained");
        auto settings = config(root, name.toRawUTF8());
        settings.session.producer.queue.packets = 1;
        juce::WaitableEvent started, release; std::atomic<int> prepares { 0 };
        settings.session.producer.queue.onBoundary = [&](auto boundary)
        {
            if (boundary == AsyncRecoveryWriter::Boundary::beforePrepare && ++prepares == 1)
            {
                started.signal(); require(release.wait(15000), "release accepted cancelled-close prefix");
                if (pendingFailure) throw std::runtime_error("owned first prepare failure");
            }
        };
        SingleTrackDocument document; require(document.loadFile(source), "load cancelled gap fixture");
        DocumentSaveCoordinator saves(document); RecoverySessionHost host(document, saves, settings);
        const juce::Uuid entry; good(host.attachPostCore(entry), "admit gap fixture before close");
        require(document.moveEnvelopePoint(0, 0, 0.8f) && started.wait(5000)
            && document.moveEnvelopePoint(0, 0, 0.6f) && current(host).checkpointRequired,
            "actual bounded queue misses a mutation before cancelled close");
        const auto oldOwner = current(host).owner;
        release.signal(); await([&] { return !current(host).writer.workerRunning; }, "old gap prefix finishes before close");
        host.beginClose(); require(!host.health().settled, "unrepaired gap stays owned on close");
        if (replaced)
        {
            document.clear(); require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.4f), "same path now has a different real Document identity");
        }
        const auto image = document.captureWorkspaceSnapshot();
        good(host.cancelClose(entry), "cancel close admits actual current full history");
        if (pendingFailure) good(host.retry(oldOwner), "closed admission-gap writer can retry only its already accepted prefix");
        await([&] { host.poll(); return current(host).writer.protectedIdentity.sameVersion(document.getMediaIdentity().document)
            && !current(host).writer.workerRunning && host.health().pendingPackets == 0; }, "new actual checkpoint and accepted old prefix become durable after cancelled close");
        host.poll();
        require(document.captureWorkspaceSnapshot()->sameContents(*image) && current(host).owner != oldOwner,
            "cancelled-close admission preserves history and never reopens the old producer");
        if (replaced)
        {
            require(host.health().sessions.size() == 2 && host.takeReleasedEpochs().empty(),
                "new same-path Document cannot supersede another Document's closed gap");
        }
        else
        {
            const auto released = host.takeReleasedEpochs();
            require(host.health().sessions.size() == 1 && released.size() == 1
                && released.front().protection.owner == oldOwner && released.front().protection.checkpointRequired,
                "same lifetime owner releases only superseded runtime gap and preserves its failure metadata");
            host.beginClose(); host.poll(); require(host.health().settled, "repaired cancelled close no longer remains permanently pending");
        }
        RecoveryStore store(settings.session.producer.root, oldOwner);
        require(store.inspect().status && !settings.session.producer.root.getChildFile(oldOwner.toString()).getChildFile("retired").exists(),
            "runtime gap release never deletes the old journal or grants terminal-marker authority");
    }
}

void hostBudgets(const juce::File& root, const juce::File& source)
{
    {
        auto settings = config(root, "retained-host-budget"); settings.retainedBytes = 130000;
        require(settings.session.producer.root.replaceWithText("owned obstruction"), "obstruct first session to retain accepted capture memory");
        SingleTrackDocument document; require(document.loadFile(source), "open bounded memory fixture");
        DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
        good(host.attachPostCore(juce::Uuid()), "admit first memory-budget observer");
        require(document.moveEnvelopePoint(0, 0, 0.7f), "retain actual first capture");
        await([&] { return !current(host).writer.workerRunning; }, "first memory-budget writer fails on actual obstruction");
        const auto oldOwner = current(host).owner;
        const auto retained = host.health().retainedBytes;
        require(retained > settings.retainedBytes / 2 && retained < settings.retainedBytes, "fixture really occupies most aggregate host capture budget");
        document.clear(); require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.3f), "new independent dirty document requires full checkpoint");
        const juce::Uuid newEntry;
        require(host.attachPostCore(newEntry).error == Error::budgetExceeded
            && host.health().retainedBytes == retained && host.health().sessions.size() == 1,
            "new dirty admission uses remaining total budget and preserves old failed captures when rejected");
        const auto admissionFailure = diagnosticFailure(settings);
        require(admissionFailure.stage == RecoveryDiagnosticState::Stage::sessionAdmission && admissionFailure.error == Error::budgetExceeded,
            "Host preserves actual underlying Session admission failure instead of manufacturing a Health event");
        require(settings.session.producer.root.deleteFile(), "remove exact memory fixture obstruction");
        good(host.retry(oldOwner), "replay accepted old memory-budget packet");
        await([&] { host.poll(); return host.health().sessions.empty(); }, "departed actor releases memory only after actual replay");
        good(host.attachPostCore(newEntry), "admission recovers after actual old captures release");
        require(diagnosticFailure(settings).operationId == admissionFailure.operationId,
            "successful Host readmission preserves original real failure");
        protect(host); host.beginClose(); await([&] { host.poll(); return host.health().settled; }, "bounded memory host closes after real protection");
    }
    {
        auto settings = config(root, "host-metadata-budget"); settings.session.retiredEpochs = 1; settings.metadataEvents = 2;
        SingleTrackDocument document; require(document.loadFile(source), "open metadata backpressure fixture");
        DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
        const juce::Uuid entry; good(host.attachPostCore(entry), "admit metadata-budget observer");
        for (int i = 0; i < 3; ++i)
        {
            require(document.moveEnvelopePoint(0, 0, 0.8f - static_cast<float>(i) * 0.2f), "edit next metadata-budget epoch");
            protect(host);
            save(coordinator, document.createSaveSnapshot(root.getChildFile("metadata-" + juce::String(i) + ".wav"), SingleTrackDocument::SaveIntent::saveAs));
            require(host.takeSavedRetirements().size() == 1, "handoff exact retirement ticket before metadata release");
            host.poll();
            if (i == 0) require(host.health().pendingMetadata == 1, "first released actor occupies bounded metadata slot");
            if (i == 1)
            {
                require(host.health().failure.code == "host_metadata_backlog" && host.health().pendingMetadata == 1
                    && host.health().sessions.front().protection.retired.size() == 1,
                    "insufficient reserved result space keeps exact actor owned instead of erasing its metadata");
                const auto backlog = diagnosticFailure(settings);
                require(backlog.stage == RecoveryDiagnosticState::Stage::hostMetadata && backlog.counters[1] == 1,
                    "real bounded metadata transfer failure reports existing mailbox occupancy");
                host.poll(); host.poll(); (void) host.health();
                require(diagnosticFailure(settings).operationId == backlog.operationId,
                    "repeated poll and Health of one backlog do not manufacture new failures");
                require(host.takeReleasedEpochs().size() == 1, "service consumes queued metadata"); host.poll();
                require(host.health().failure && host.health().pendingMetadata == 1
                    && host.health().sessions.front().protection.retired.empty(), "metadata backpressure clears only after successful bounded transfer");
                require(diagnosticFailure(settings).operationId == backlog.operationId,
                    "successful metadata transfer retains the earlier failure diagnostic");
                host.takeReleasedEpochs();
            }
        }
        host.takeReleasedEpochs(); host.beginClose(); require(host.health().settled, "metadata host closes with inert clean observer");
    }
    {
        auto settings = config(root, "host-ticket-budget"); settings.savedTickets = 1;
        SingleTrackDocument document; require(document.loadFile(source), "open ticket budget fixture");
        DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
        const juce::Uuid entry; good(host.attachPostCore(entry), "admit ticket-budget observer");
        juce::Uuid firstOwner(nullptr);
        for (int i = 0; i < 2; ++i)
        {
            require(document.moveEnvelopePoint(0, 0, i == 0 ? 0.8f : 0.6f), "edit ticket budget epoch"); protect(host);
            if (i == 0) firstOwner = current(host).owner;
            save(coordinator, document.createSaveSnapshot(root.getChildFile("ticket-budget-" + juce::String(i) + ".wav"), SingleTrackDocument::SaveIntent::saveAs));
            if (i == 0) host.poll();
        }
        require(host.health().pendingRetirements == 1 && host.health().failure.code == "host_retirement_backlog"
            && host.health().sessions.front().protection.retired.size() == 1,
            "full ticket mailbox preserves earlier authority, reports failure and does not immediately erase second producer");
        const auto savedFailure = diagnosticFailure(settings);
        require(savedFailure.stage == RecoveryDiagnosticState::Stage::hostSavedDisposition
            && savedFailure.error == Error::budgetExceeded && savedFailure.counters[2] == 1
            && savedFailure.counters[5] > 0 && !savedFailure.owner.isNull() && savedFailure.owner != firstOwner,
            "actual adopted Save disposition reports exact refused owner, saved ticket count and request serial");
        host.leaveDocument(); good(host.attachPostCore(entry), "readmit current clean Document after explicit departure");
        require(host.health().failure.code == "host_retirement_backlog", "successful unrelated admission cannot erase unresolved retirement failure");
        require(diagnosticFailure(settings).operationId == savedFailure.operationId,
            "unrelated Host success cannot replace diagnostic evidence of Save disposition failure");
        const auto tickets = host.takeSavedRetirements(); require(tickets.size() == 1 && tickets.front()->ownerId() == firstOwner, "ticket overflow never overwrites another owner's authority");
        host.takeReleasedEpochs(); host.beginClose(); require(host.health().settled && !host.health().failure,
            "runtime may settle while storage handoff failure remains explicitly visible for service close policy");
    }
}
void renewedCancellationAuthority(const juce::File& root, const juce::File& source)
{
    auto settings = config(root, "renewed-cancellation");
    require(settings.session.producer.root.replaceWithText("owned obstruction"), "retain failed queue for cancellation target failure");
    juce::WaitableEvent checking, release; std::atomic<int> checks { 0 };
    settings.session.producer.queue.onBoundary = [&] (auto boundary)
    {
        if (boundary == AsyncRecoveryWriter::Boundary::beforeCancellationCheck && ++checks == 1)
        { checking.signal(); require(release.wait(15000), "release first actual saved target validation"); }
    };
    SingleTrackDocument document; require(document.loadFile(source), "open renewed cancellation source");
    DocumentSaveCoordinator coordinator(document); RecoverySessionHost host(document, coordinator, settings);
    good(host.attachPostCore(juce::Uuid()), "admit renewed cancellation observer");
    require(document.moveEnvelopePoint(0, 0, 0.7f), "enqueue actual mutation before save");
    await([&] { return !current(host).writer.workerRunning; }, "actual writer retains failed packet");
    const auto owner = current(host).owner;
    const auto epoch = [&]
    {
        for (const auto& session : host.health().sessions)
            for (const auto& value : session.protection.retired) if (value.owner == owner) return value;
        require(false, "failed cancellation actor remains owned for renewed Save"); return RecoveryMutationProducer::Health{};
    };
    const auto first = root.getChildFile("cancellation-target-removed.wav");
    save(coordinator, document.createSaveSnapshot(first, SingleTrackDocument::SaveIntent::saveAs));
    require(checking.wait(5000) && first.deleteFile(), "remove only owned actual saved target before worker checks it");
    release.signal(); await([&] { return !epoch().writer.workerRunning; }, "worker refuses missing actual saved target");
    require(epoch().writer.cancellation == AsyncRecoveryWriter::Cancellation::failed && epoch().writer.pendingPackets == 1
        && host.takeReleasedEpochs().empty(), "failed cancellation retains exact original capture");
    save(coordinator, document.createSaveSnapshot(root.getChildFile("cancellation-new-target.wav"), SingleTrackDocument::SaveIntent::saveAs));
    await([&] { return !epoch().writer.workerRunning; }, "new actual Save provides renewed cancellation authority");
    require(epoch().writer.cancellation == AsyncRecoveryWriter::Cancellation::completed && epoch().writer.cancelledPackets == 1,
        "new actual clean Save can retry previously failed cancellation without fake success or discarded captures");
    host.poll(); require(host.takeReleasedEpochs().size() == 1 && host.takeSavedRetirements().empty(), "renewed cancellation returns metadata only");
    host.beginClose(); require(host.health().settled && host.health().failure, "resolved target failure leaves clean runtime close state");
}
ServiceStorageBudget serviceBudget()
{
    ServiceStorageBudget value; value.bytes = value.journalBytes = 430000 + 65536; value.attemptBytes = 65536;
    value.maintenanceBytes = 65536; value.entries = value.journalEntries = 1024; value.attemptEntries = 128;
    value.maintenanceEntries = 32; value.freeSpaceReserve = 0; return value;
}
void globalBudgetLifecycle(const juce::File& root, const juce::File& source, bool shared = false)
{
    auto settings = config(root, shared ? "shared-service-budget" : "global-session-budget"); settings.session.producer.store.globalBytes = 430000;
    const auto serviceRoot = settings.session.producer.root;
    RecoveryStorageLayout layout(serviceRoot);
    if (shared)
    {
        require(!serviceRoot.exists(), "layout construction and paths are inert");
        good(layout.prepare(serviceBudget()), "explicitly prepare shared service policy and both physical scope bindings");
        settings.session.producer.root = layout.journalDirectory();
        settings.session.producer.store.globalBytes = serviceBudget().journalBytes;
    }
    SingleTrackDocument first, second;
    require(first.loadFile(source) && second.loadFile(source), "load two independent actual documents into one global recovery root");
    DocumentSaveCoordinator firstSaves(first), secondSaves(second);
    RecoverySessionHost firstHost(first, firstSaves, settings), secondHost(second, secondSaves, settings);
    good(firstHost.attachPostCore(juce::Uuid()), "observe first global owner");
    good(secondHost.attachPostCore(juce::Uuid()), "observe second global owner");
    require(first.moveEnvelopePoint(0, 0, 0.7f), "commit first document mutation"); protect(firstHost);
    const auto protectedOwner = current(firstHost).owner; const auto protectedHead = current(firstHost).writer.safeHead;
    std::unique_ptr<RecoveryAttemptWriter> attempt;
    if (shared)
    {
        RecoveryAttemptStore markers(layout.attemptDirectory(), protectedOwner);
        good(markers.begin(juce::Uuid(), protectedHead, protectedHead, "shared-service-v1", attempt),
            "actual Attempt automatically joins durable service quota without caller shared-budget configuration");
    }
    require(second.moveEnvelopePoint(0, 0, 0.4f), "second edit remains immediately visible despite global pressure");
    await([&] { return !current(secondHost).writer.workerRunning; }, "second actual writer reaches global hard gate");
    const auto pendingOwner = current(secondHost).owner;
    require(current(secondHost).failure.code == "global_byte_budget" && secondHost.health().pendingPackets == 1
        && secondHost.health().retainedBytes > 0 && second.hasUnsavedChanges() && second.canUndo(),
        "global hard refusal retains accepted capture, dirty edit and Undo with truthful degraded health");
    RecoveryStore protectedStore(settings.session.producer.root, protectedOwner);
    require(protectedStore.inspect().head == protectedHead && current(secondHost).writer.safeHead.sequence == 0,
        "global pressure neither deletes existing protected owner nor advances failed owner safe head");
    juce::File filler;
    if (shared)
    {
        int64_t total = 0;
        for (const auto& file : files::nativeFile(serviceRoot).findChildFiles(juce::File::findFiles, true)) total += file.getSize();
        const auto normalCap = serviceBudget().bytes - serviceBudget().maintenanceBytes;
        require(total < normalCap, "actual first protected image and marker fit normal shared allocation");
        juce::MemoryBlock data(static_cast<size_t>(normalCap - total), true);
        filler = serviceRoot.getChildFile("unknown-service-data");
        require(files::nativeFile(filler).replaceWithData(data.getData(), data.getSize()), "fill only known fixture to normal shared cap including both areas");
        require(attempt->enter(RecoveryAttemptStage::ready).code == "global_byte_budget" && attempt->isActive(),
            "Attempt sees journal and top-level unknown bytes while maintenance reserve stays unavailable to ordinary growth");
    }
    const auto target = root.getChildFile(shared ? "service-owner-saved.wav" : "global-owner-saved.wav");
    save(firstSaves, first.createSaveSnapshot(target, SingleTrackDocument::SaveIntent::saveAs));
    const auto tickets = firstHost.takeSavedRetirements(); require(tickets.size() == 1, "actual current clean Save yields exact owner retirement authority");
    firstHost.poll(); firstHost.takeReleasedEpochs();
    OwnerRetirement retirement;
    good(RecoveryOwnerLifecycle::retireSaved(protectedStore, protectedHead, *tickets.front(), retirement), "actual Saved authority permits terminal marker under pressure");
    bool complete = false;
    for (int i = 0; i < 128 && !complete; ++i)
    {
        TerminalCollectionReport report;
        good(protectedStore.collectTerminal(protectedHead, report, { 1, 1024 * 1024 }), "bounded real terminal collection frees only eligible saved owner");
        if (shared && i == 0)
        {
            require(report.more && report.filesRemoved == 0 && files::nativeFile(filler).existsAsFile(),
                "reserved maintenance allocation publishes real retirement and complete terminal plan at full normal capacity");
            require(files::nativeFile(filler).deleteFile(), "remove only injected exact-cap filler after proving protected maintenance admission");
        }
        complete = report.completed;
    }
    require(complete && target.existsAsFile() && first.canUndo(), "actual disk reclamation preserves saved target and live Undo");
    good(secondHost.retry(pendingOwner), "explicit retry rechecks actual global usage after authorized collection");
    protect(secondHost);
    require(current(secondHost).owner == pendingOwner && current(secondHost).writer.safeHead.sequence == 1
        && current(secondHost).writer.protectedIdentity.sameVersion(second.getMediaIdentity().document),
        "retained original pending mutation really publishes after space becomes available");
    if (shared)
    {
        good(attempt->enter(RecoveryAttemptStage::ready), "same actual Attempt advances after eligible saved-owner collection");
        good(attempt->finish(AttemptOutcome::succeeded), "actual Attempt closes under shared total quota after reclamation");
    }
    firstHost.beginClose(); secondHost.beginClose();
    await([&] { firstHost.poll(); secondHost.poll(); return firstHost.health().settled && secondHost.health().settled; }, "global-budget sessions close only after actual queue completion");
}

void servicePolicyTests(const juce::File& root)
{
    for (int boundary = 0; boundary < 3; ++boundary)
    {
        const auto path = root.getChildFile("service-crash-" + juce::String(boundary));
        RecoveryStorageLayout layout(path);
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--service-layout-child", path.getFullPathName(), juce::String(boundary), root.getFullPathName() })
            && child.waitForProcessToFinish(15000) && child.getExitCode() == 93,
            "real process dies after service policy or either physical scope binding");
        const juce::Uuid owner;
        const SafeHead head { 1, juce::String::repeatedString("a", 64) };
        std::unique_ptr<RecoveryAttemptWriter> writer;
        RecoveryAttemptStore markers(layout.attemptDirectory(), owner);
        if (boundary < 2)
            require(!markers.begin(juce::Uuid(), head, head, "service-prepare-v1", writer) && !writer,
                "partially prepared service cannot admit an independent fallback Attempt");
        good(layout.prepare(serviceBudget()), "resume exact service initialization after actual process death");
        good(layout.prepare(serviceBudget()), "same service policy and bindings are idempotent");
        good(markers.begin(juce::Uuid(), head, head, "service-prepare-v1", writer), "default caller follows persisted shared binding after restart");
        good(writer->finish(AttemptOutcome::cancelled, { Error::cancelled, "fixture_complete" }), "close actual restarted fixture marker");
        auto changed = serviceBudget(); ++changed.bytes;
        require(layout.prepare(changed).code == "service_policy_configuration", "different service budget cannot silently reconfigure persisted global policy");
        const auto full = path.getChildFile("unknown-full-root");
        juce::MemoryBlock fullBytes(static_cast<size_t>(serviceBudget().bytes), true);
        require(full.replaceWithData(fullBytes.getData(), fullBytes.getSize()), "fill only owned service fixture beyond hard capacity");
        good(layout.prepare(serviceBudget()), "already complete immutable layout needs no new admission even when capacity is exhausted");
        require(markers.begin(juce::Uuid(), head, head, "still-full", writer).code == "global_byte_budget" && full.existsAsFile(),
            "idempotent layout readiness does not claim capacity or bypass ordinary growth limits");
        require(full.deleteFile(), "remove only injected service capacity fixture");
        const auto servicePolicy = path.getChildFile("quota-policy");
        juce::MemoryBlock savedPolicy; require(servicePolicy.loadFileAsData(savedPolicy) && servicePolicy.deleteFile(),
            "remove only known service policy to emulate interrupted control metadata");
        require(layout.prepare(changed).code == "service_binding_conflict" && !servicePolicy.exists(),
            "surviving scope policy hashes forbid rebuilding a missing parent policy with different limits");
        good(layout.prepare(serviceBudget()), "same policy can be restored from exact scope commitments without moving data");
        juce::MemoryBlock restoredPolicy; require(servicePolicy.loadFileAsData(restoredPolicy) && restoredPolicy == savedPolicy,
            "restored service policy is byte-identical to original committed allocation");
        RecoveryAttemptStore wrong(layout.journalDirectory(), juce::Uuid());
        require(wrong.begin(juce::Uuid(), head, head, "wrong-area", writer).code == "service_scope_role",
            "Attempt cannot use journal scope to borrow its different local allowance");
        RecoveryAttemptStore unscoped(path, juce::Uuid());
        require(unscoped.begin(juce::Uuid(), head, head, "unscoped", writer).code == "service_scope_required",
            "service root itself is not a normal unscoped owner directory");
        const auto binding = layout.attemptDirectory().getChildFile("quota-policy");
        juce::MemoryBlock original; require(binding.loadFileAsData(original) && binding.deleteFile(), "remove only known binding to emulate interrupted fixture metadata");
        require(!markers.begin(juce::Uuid(), head, head, "missing-binding", writer), "missing binding cannot disable aggregate budget");
        good(layout.prepare(serviceBudget()), "explicit service repair restores the missing binding for the same physical area");
        juce::MemoryBlock restored; require(binding.loadFileAsData(restored) && restored == original,
            "same root and area restore byte-identical binding without replacing audio or markers");
        const auto unknown = layout.attemptDirectory().getChildFile("unknown-bytes");
        juce::MemoryBlock data(static_cast<size_t>(serviceBudget().attemptBytes), true);
        require(unknown.replaceWithData(data.getData(), data.getSize()), "create counted private data within Attempt local scope");
        require(markers.begin(juce::Uuid(), head, head, "scope-cap", writer).code == "scope_byte_budget" && unknown.existsAsFile(),
            "Attempt local ceiling remains enforced while shared global capacity is still available");
    }
}

void serviceConcurrencyTests(const juce::File& root, const juce::File& source)
{
    for (int crash = 0; crash < 2; ++crash)
    {
        const auto path = root.getChildFile("service-contention-" + juce::String(crash));
        RecoveryStorageLayout layout(path); good(layout.prepare(serviceBudget()), "prepare shared root for actual cross-process contention");
        const auto signals = root.getChildFile("service-signals-" + juce::String(crash));
        require(signals.createDirectory().wasOk(), "create private signals outside combined quota root");
        const juce::Uuid owner; const SafeHead head { 1, juce::String::repeatedString("b", 64) };
        std::unique_ptr<RecoveryAttemptWriter> attempt;
        good(RecoveryAttemptStore(layout.attemptDirectory(), owner).begin(juce::Uuid(), head, head, "shared-process-v1", attempt),
            "begin actual live Attempt before another process takes shared root");
        SingleTrackDocument document; require(document.loadFile(source), "load real Document for journal versus Attempt root contention");
        auto settings = config(root, "unused-service-path"); settings.session.producer.root = layout.journalDirectory();
        DocumentSaveCoordinator saves(document); RecoverySessionHost host(document, saves, settings);
        good(host.attachPostCore(juce::Uuid()), "observe actual document before shared lock contention");
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--service-layout-child", path.getFullPathName(), juce::String(3 + crash), signals.getFullPathName() }),
            "start real process with service transaction");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
        while (!signals.getChildFile("ready").existsAsFile() && child.isRunning() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
        require(signals.getChildFile("ready").existsAsFile() && child.isRunning(), "child holds actual common root lock across layout transaction");
        require(document.moveEnvelopePoint(0, 0, 0.4f), "commit actual edit immediately while shared root is unavailable");
        await([&] { return !current(host).writer.workerRunning; }, "actual journal worker reports bounded shared contention");
        const auto selected = current(host).owner;
        require(current(host).failure.code == "global_budget_busy" && host.health().pendingPackets == 1
            && document.hasUnsavedChanges() && document.canUndo()
            && attempt->enter(RecoveryAttemptStage::ready).code == "global_budget_busy",
            "journal and Attempt both obey the same actual process lock while retaining their original accepted work");
        require(signals.getChildFile("release").replaceWithText("release own service child"), "release only owned service contention process");
        require(child.waitForProcessToFinish(15000) && child.getExitCode() == (crash ? 94u : 0u), "service lock releases on normal completion or actual process death");
        good(host.retry(selected), "same accepted journal owner retries after common root release"); protect(host);
        good(attempt->enter(RecoveryAttemptStage::ready), "same Attempt retries after shared process release");
        good(attempt->finish(AttemptOutcome::succeeded), "same Attempt durably closes after shared root contention");
        host.beginClose(); await([&] { host.poll(); return host.health().settled; }, "shared service host drains before fixture teardown");
    }
}

void lazyServicePreparationTests(const juce::File& root, const juce::File& source)
{
    for (int mode = 0; mode < 3; ++mode)
    {
        const auto serviceRoot = root.getChildFile("lazy-service-" + juce::String(mode));
        RecoveryStorageLayout layout(serviceRoot);
        auto settings = config(root, "unused-lazy-root");
        settings.session.producer.root = layout.journalDirectory();
        struct Preparation final { std::atomic<int> calls { 0 }; juce::WaitableEvent started, release; };
        auto preparation = std::make_shared<Preparation>();
        auto captureLifetime = std::make_shared<int>(1);
        const std::weak_ptr<int> captureReleased = captureLifetime;
        settings.session.producer.prepareStorage = [layout, preparation, captureLifetime, mode]
        {
            juce::ignoreUnused(captureLifetime);
            require(!juce::MessageManager::getInstance()->isThisTheMessageThread(), "service preparation never performs storage I/O on message thread");
            ++preparation->calls;
            preparation->started.signal();
            if (mode == 2) require(preparation->release.wait(15000), "release only owned deferred service preparation");
            return layout.prepare();
        };
        SingleTrackDocument document; require(document.loadFile(source), "load actual document before lazy service admission");
        DocumentSaveCoordinator saves(document);
        auto host = std::make_unique<RecoverySessionHost>(document, saves, settings);
        settings.session.producer.prepareStorage = {};
        captureLifetime.reset();
        good(host->attachPostCore(juce::Uuid()), "PostCore attaches only an observer for clean native document");
        host->poll();
        require(preparation->calls == 0 && !serviceRoot.exists() && current(*host).owner.isNull(),
            "clean admission and polling create no recovery root, prepare call, owner or worker");
        require(!document.moveEnvelopePoint(0, 0, 1.0f) && preparation->calls == 0 && !serviceRoot.exists(),
            "no-op edit does not prepare recovery storage");
        if (mode == 1) require(serviceRoot.replaceWithText("owned lazy setup obstruction"), "obstruct only private service root before first real mutation");
        require(document.moveEnvelopePoint(0, 0, 0.6f) && document.hasUnsavedChanges() && document.canUndo(),
            "first mutation and Undo are immediately visible while service preparation is asynchronous");
        require(preparation->started.wait(5000), "accepted packet actually enters service preparation on worker");
        const auto owner = current(*host).owner;
        if (mode == 2)
        {
            host->beginClose();
            require(!host->health().settled && host->health().pendingPackets == 1, "close cannot call pending storage preparation drained");
            host.reset(); document.clear();
            require(!captureReleased.expired(), "accepted worker owns preparation captures after host and Document release");
            preparation->release.signal();
            await([&] { return captureReleased.expired(); }, "last accepted preparation capture is released after actual worker completes");
            RecoveryStore store(layout.journalDirectory(), owner);
            const auto manifest = store.inspect();
            require(manifest.status && manifest.head.sequence == 1, "detached worker preserves exact accepted edit without a live UI callback");
        }
        else
        {
            if (mode == 1)
            {
                await([&] { return !current(*host).writer.workerRunning; }, "failed service preparation settles the real worker");
                require(!current(*host).failure && host->health().pendingPackets == 1
                    && current(*host).writer.safeHead.sequence == 0 && document.hasUnsavedChanges() && document.canUndo()
                    && !layout.journalDirectory().exists(), "setup failure preserves original queue and cannot fall back to independent journal quota");
                require(serviceRoot.deleteFile(), "remove only injected service setup obstruction");
                good(host->retry(owner), "explicit retry reuses original accepted owner and capture after setup failure");
            }
            protect(*host);
            require(current(*host).owner == owner && current(*host).writer.safeHead.sequence == 1,
                "lazy preparation protects the original owner and actual version");
            host->beginClose(); await([&] { host->poll(); return host->health().settled; }, "prepared actor truly drains on close");
            host.reset();
            require(captureReleased.expired() && preparation->calls == (mode == 1 ? 2 : 1), "no preparation capture or recurring storage task survives settled host");
        }
        require(layout.journalDirectory().getChildFile("quota-policy").existsAsFile()
            && layout.attemptDirectory().getChildFile("quota-policy").existsAsFile(), "first accepted edit establishes both shared service bindings before journal publication");
    }
}

void serviceMaintenanceTests(const juce::File& root, const juce::File& source)
{
    RecoveryStorageLayout layout(root.getChildFile("maintenance-service"));
    const auto makeTicket = [&] (const char* name)
    {
        SingleTrackDocument document; require(document.loadFile(source), "load actual document for maintenance authority");
        DocumentSaveCoordinator saves(document);
        auto settings = config(root, "unused-maintenance-root");
        settings.session.producer.root = layout.journalDirectory();
        settings.session.producer.prepareStorage = [layout] { return layout.prepare(); };
        RecoverySessionHost host(document, saves, settings);
        good(host.attachPostCore(juce::Uuid()), "observe actual maintenance source after CoreReady");
        require(document.moveEnvelopePoint(0, 0, 0.55f), "commit real edit before maintenance Save"); protect(host);
        save(saves, document.createSaveSnapshot(root.getChildFile(juce::String(name) + ".wav"), SingleTrackDocument::SaveIntent::saveAs));
        auto tickets = host.takeSavedRetirements(); require(tickets.size() == 1, "real Save yields exactly one private maintenance ticket");
        host.beginClose(); await([&] { host.poll(); return host.health().settled; }, "host releases only actual drained producer after issuing ticket");
        return std::move(tickets.front());
    };
    using Queue = RecoveryMaintenanceQueue;
    Queue::Config settings; settings.jobs = 1; settings.batch.files = 1;
    Queue queue(settings);
    require(!queue.progress().workerRunning && queue.progress().jobs.empty() && !layout.journalDirectory().exists(),
        "maintenance construction and health reads start no worker or storage");
    auto ticket = makeTicket("maintenance-first"); const auto owner = ticket->ownerId(); const auto head = ticket->frozenHead();
    for (const auto invalidFiles : { true, false })
    {
        auto invalid = settings;
        if (invalidFiles) invalid.batch.files = 129;
        else invalid.store.readBytes = invalid.batch.bytes - 1;
        Queue rejected(invalid);
        require(rejected.submit(ticket).code == "maintenance_configuration" && ticket && ticket->ownerId() == owner
            && !rejected.progress().workerRunning && rejected.progress().jobs.empty(),
            "invalid physical sweep budget is rejected before taking real authority or starting a worker");
    }
    RecoveryStore store(ticket->rootDirectory(), owner);
    std::shared_ptr<RecoveryOwnerLease> live;
    good(store.acquireOwnerLease(live), "hold actual cross-process-capable recovered owner lease");
    good(queue.submit(ticket), "maintenance accepts actual saved ticket"); require(!ticket, "acceptance transfers ticket ownership once");
    await([&] { return !queue.progress().workerRunning; }, "live dependency causes bounded maintenance refusal");
    require(queue.progress().jobs.size() == 1 && queue.progress().jobs.front().phase == Queue::Phase::failed
        && !queue.progress().failure && store.inspect().head == head,
        "live owner blocks retirement and failed task remains owned with original safe data");
    auto second = makeTicket("maintenance-second"); const auto secondOwner = second->ownerId();
    require(queue.submit(second).code == "maintenance_job_budget" && second && second->ownerId() == secondOwner,
        "maintenance backpressure never consumes or drops rejected private ticket");
    queue.close(); require(!queue.progress().settled, "failed accepted cleanup cannot be reported as settled clean close");
    live.reset(); good(queue.retry(layout.journalDirectory(), owner), "explicit retry remains available while draining a closed queue");
    await([&] { return queue.progress().settled; }, "actual saved owner completes bounded terminal deletion");
    const auto completed = queue.takeCompleted();
    require(completed.size() == 1 && completed.front().owner == owner && completed.front().head == head
        && completed.front().filesRemoved > 0 && completed.front().batches > 1
        && !files::nativeFile(layout.journalDirectory().getChildFile(owner.toString())).exists()
        && root.getChildFile("maintenance-first.wav").existsAsFile(),
        "completed metadata names exact owner and real bounded deletion preserves user's saved audio");
    require(queue.submit(second).code == "maintenance_closed" && second, "closed queue preserves caller's new ticket");
    {
        struct Gate final { juce::WaitableEvent ready, release; };
        auto gate = std::make_shared<Gate>(); auto lifetime = std::make_shared<int>(1);
        const std::weak_ptr<int> released = lifetime;
        auto detachedSettings = settings;
        detachedSettings.onBoundary = [gate, lifetime] (Queue::Phase phase, juce::Uuid)
        {
            juce::ignoreUnused(lifetime);
            if (phase == Queue::Phase::retiring) { gate->ready.signal(); require(gate->release.wait(15000), "release owned detached maintenance worker"); }
        };
        auto detached = std::make_unique<Queue>(detachedSettings);
        detachedSettings.onBoundary = {}; lifetime.reset();
        good(detached->submit(second), "second actual ticket enters independently owned service queue");
        require(gate->ready.wait(5000), "accepted cleanup actually reaches worker before service destruction");
        detached.reset(); require(!released.expired(), "worker retains only owned metadata/config after queue destruction");
        gate->release.signal(); await([&] { return released.expired(); }, "worker exits and releases its last captures after accepted cleanup");
        require(!files::nativeFile(layout.journalDirectory().getChildFile(secondOwner.toString())).exists()
            && root.getChildFile("maintenance-second.wav").existsAsFile(), "detached cleanup completes exact owner without borrowing UI lifetime");
    }
    {
        struct Gate final { juce::WaitableEvent ready, release; std::atomic<int> collections { 0 }; };
        auto gate = std::make_shared<Gate>(); auto resumeSettings = settings;
        resumeSettings.onBoundary = [gate] (Queue::Phase phase, juce::Uuid)
        {
            if (phase == Queue::Phase::collecting && ++gate->collections == 1)
            { gate->ready.signal(); require(gate->release.wait(15000), "release first collection after terminal intent is durable"); }
        };
        Queue resume(resumeSettings); auto pending = makeTicket("maintenance-resume"); const auto pendingOwner = pending->ownerId();
        const auto pendingHead = pending->frozenHead();
        good(resume.submit(pending), "accept real terminal collection resume fixture");
        require(gate->ready.wait(5000), "worker reaches next batch after real terminal publication");
        RecoveryStore pendingStore(layout.journalDirectory(), pendingOwner);
        std::optional<OwnerRetirement> retirement;
        require(pendingStore.inspectRetirement(retirement).code == "owner_collecting", "durable intent fences normal owner views during collection");
        {
            std::unique_ptr<detail::DurableDirectory> directory;
            good(detail::DurableDirectory::open(layout.journalDirectory().getChildFile(pendingOwner.toString()), false, directory),
                "open only exact fixture metadata for independent terminal marker verification");
            require(directory != nullptr, "terminal fixture remains present before first deletion");
            juce::var marker; good(directory->readJson("retired", 16384, marker), "verify actual framed and checksummed retirement marker");
            require(marker["kind"] == juce::var("retirement") && marker["owner"] == juce::var(pendingOwner.toString())
                && marker["hash"] == juce::var(pendingHead.hash) && static_cast<int64_t>(marker["sequence"]) == pendingHead.sequence,
                "durable marker names the exact saved owner/head before later collection failure");
        }
        auto lock = root.getChildFile("maintenance-service/quota.lock").createOutputStream();
        require(lock && lock->openedOk(), "hold actual existing service quota lock via incompatible write access");
        gate->release.signal(); await([&] { return !resume.progress().workerRunning; }, "actual root contention stops worker without retry spin");
        require(resume.progress().failure.code == "global_budget_busy" && resume.progress().jobs.front().phase == Queue::Phase::failed,
            "post-marker batch failure preserves job and reports actual root lock cause");
        lock.reset(); require(root.getChildFile("maintenance-resume.wav").deleteFile(), "remove only owned saved-target fixture after terminal authority was published");
        good(resume.retry(layout.journalDirectory(), pendingOwner), "retry resumes published terminal intent without fabricating a new Save proof");
        resume.close(); await([&] { return resume.progress().settled; }, "resumed bounded cleanup reaches genuine settled close");
        require(resume.takeCompleted().size() == 1 && !files::nativeFile(layout.journalDirectory().getChildFile(pendingOwner.toString())).exists(),
            "terminal resume completes exact owner after later saved-path change");
    }
}
}

int runRecoveryServiceLayoutChild(const juce::File& root, int action, const juce::File& signals)
{
    RecoveryStorageLayout layout(root);
    layout.onPrepareBoundary = [&] (int boundary)
    {
        if (action < 3 && boundary == action) std::_Exit(93);
        if (action >= 3 && boundary == 0)
        {
            require(signals.getChildFile("ready").replaceWithText("actual service transaction holds root"), "signal private service preparation boundary");
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
            while (!signals.getChildFile("release").existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
            require(signals.getChildFile("release").existsAsFile(), "parent releases owned service transaction");
            if (action == 4) std::_Exit(94);
        }
    };
    return layout.prepare(serviceBudget()) ? 0 : 95;
}

void runRecoveryServiceQuotaTests(const juce::File& parent)
{
    const auto root = parent.getChildFile("service-quota-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create isolated real service quota fixture");
    const auto source = root.getChildFile("source.wav"); makeSource(source);
    globalBudgetLifecycle(root, source, true);
    const auto longRoot = root.getChildFile("storage-prefix-" + juce::String::repeatedString("p", 64));
    require(longRoot.createDirectory().wasOk(), "create owned long storage prefix outside counted service root");
    require(longRoot.getChildFile("shared-service-budget/journals").getChildFile(juce::Uuid().toString())
        .getChildFile("assets").getChildFile(juce::String::repeatedString("a", 64)).getFullPathName().length() >= 260,
        "long-path regression really crosses legacy MAX_PATH at a content-addressed asset");
    globalBudgetLifecycle(longRoot, source, true);
    servicePolicyTests(root);
    serviceConcurrencyTests(root, source);
    lazyServicePreparationTests(root, source);
    serviceMaintenanceTests(root, source);
    require(root.isAChildOf(parent) && root.getParentDirectory() == parent && root.getFileName().startsWith("service-quota-")
        && files::nativeFile(root).deleteRecursively(), "remove only unique private service fixture after all actors release");
    std::cout << "RecoveryServiceQuotaTests passed (shared journal/Attempt capacity, physical bindings, reserved real Save/GC, exact pending retry, 3 initialization kills and process contention)\n";
}

void runRecoverySessionHostTests(const juce::File& parent)
{
    const auto root = parent.getChildFile("session-host-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create unique owned session host test subtree");
    std::cout << "RecoverySessionHostTests fixture: " << root.getFullPathName() << '\n';
    const auto source = root.getChildFile("source.wav"); makeSource(source);
    savedTicketBeforeSwitch(root, source);
    saveScopes(root, source);
    pendingCancellation(root, source);
    switchingAndClose(root, source);
    coordinatorObserverLifecycle(root, source);
    repairedGapAtClose(root, source);
    cancelledGapClose(root, source);
    hostBudgets(root, source);
    renewedCancellationAuthority(root, source);
    globalBudgetLifecycle(root, source);
    runRecoveryServiceQuotaTests(root);
    require(files::nativeFile(root).deleteRecursively(), "remove only uniquely owned fixture after all host, reader and worker references release");
    std::cout << "RecoverySessionHostTests passed (inert PostCore, exact Save before switch, scope fences, pending/degraded cancellation, bounded departed retry/close, full-history checkpoint, coalesced observer lifecycle)\n";
}
