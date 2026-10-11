#include "Recovery/RecoveryRestoreQueue.h"
#include "Document/RecoveredPlaylistDocument.h"
#include "Audio/AudioEngine.h"
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using Queue = RecoveryRestoreQueue;
using Stage = RecoveryAttemptStage;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
void require(bool value, const char* message)
{ if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Restore queue: " << status.code << '\n'; require(static_cast<bool>(status), message); }
template<class Predicate> void waitFor(Predicate predicate, const char* message, std::function<void()> onTimeout = {})
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
    while (!predicate() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
    const auto completed = predicate();
    if (!completed && onTimeout) onTimeout();
    require(completed, message);
}
Queue::JobStatus job(const Queue& queue, const juce::Uuid& id)
{
    for (const auto& item : queue.health().jobs) if (item.attempt == id) return item;
    require(false, "accepted attempt remains observable"); return {};
}
void settled(Queue& queue, const juce::Uuid& id, Stage expected)
{
    waitFor([&] { const auto status = job(queue, id); return status.stage == expected && !queue.health().workerRunning; },
        "owned restore worker reaches expected stable stage");
}
std::shared_ptr<const RecoveredCandidateDocument> core(Queue& queue, const juce::Uuid& id)
{
    waitFor([&]
    {
        const auto status = job(queue, id);
        if (!queue.health().workerRunning && !status.failure)
        { std::cerr << "Restore queue stage=" << static_cast<int>(status.stage) << " failure=" << status.failure.code << '\n'; require(false, "candidate must reach real core admission"); }
        return status.waitingForAdmission && !queue.health().workerRunning;
    }, "actual first waveform and playable wait for UI admission", [&]
    {
        Queue::DiagnosticObservation observed;
        const auto copied = queue.tryDiagnosticObservation(id, observed);
        std::cerr << "Restore timeout: snapshot=" << copied << " running=" << observed.workerRunning << " jobs=" << observed.jobs;
        if (observed.selected) std::cerr << " stage=" << static_cast<int>(observed.selected->stage)
            << " waiting=" << observed.selected->waitingForAdmission << " failure=" << observed.selected->failure.code;
        std::cerr << '\n';
    });
    auto values = queue.takePublications();
    require(values.size() == 1 && values[0].attempt == id && values[0].core && !values[0].fullPeaks, "exact attempt offered once");
    require(queue.takePublications().empty() && !queue.needsPoll(), "waiting for UI does not busy poll or duplicate publication");
    return values[0].core;
}
void playback(SingleTrackDocument& document, const SingleTrackDocument::State& expected)
{
    AudioEngine engine(document);
    juce::AudioBuffer<float> output(expected.info.numChannels, 64);
    require(engine.renderDocumentBlockForDiagnostics(32, output), "real restored history plays through production engine");
    for (int channel = 0; channel < output.getNumChannels(); ++channel)
        for (int i = 0; i < output.getNumSamples(); ++i)
        {
            const auto at = i + 32;
            const auto gain = SingleTrackDocument::evaluateEnvelope(expected.envelopePoints, at / expected.info.sampleRate);
            require(std::abs(output.getSample(channel, i) - expected.audioBuffer->getSample(channel, at) * gain) < 1.0e-6f,
                "actual current/Undo/Redo PCM and envelope match independent snapshot");
        }
}
struct Gate final
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    void hold()
    {
        std::unique_lock lock(mutex); entered = true; changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void await() { std::unique_lock lock(mutex); require(changed.wait_for(lock, std::chrono::seconds(15), [&] { return entered; }), "real worker enters owned barrier"); }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
}

void runRecoveryRestoreQueueTests(const Snapshot& original, const juce::File& root, AudioGraphLimits limits)
{
    auto snapshot = original;
    const auto source = root.getChildFile("queue-owned-source.txt");
    require(source.replaceWithText("private stable source for restore queue"), "create isolated source fence");
    snapshot.identity.source.location.file = source;
    snapshot.identity.source.sizeBytes = source.getSize();
    snapshot.identity.source.modifiedMilliseconds = source.getLastModificationTime().toMilliseconds();
    snapshot.identity.source.fastStampValid = true;
    const auto relocate = [&](SingleTrackDocument::StatePtr state)
    { auto copy = std::make_shared<SingleTrackDocument::State>(*state); copy->info.file = source; return SingleTrackDocument::StatePtr(copy); };
    snapshot.state = relocate(snapshot.state);
    for (auto& state : snapshot.undo) state = relocate(state);
    for (auto& state : snapshot.redo) state = relocate(state);
    Queue::Config config;
    config.root = root.getChildFile("restore-service"); config.scratch = root.getChildFile("restore-service-scratch");
    config.graph = limits; config.appBuild = "restore-queue-tests-v1";
    config.diagnostics = std::make_shared<RecoveryDiagnosticState>();
    std::optional<RecoveryDiagnosticState::Failure> diagnosticFailure;
    {
        Queue inert(config);
        Queue::DiagnosticObservation observed;
        require(inert.tryDiagnosticObservation(juce::Uuid(nullptr), observed) && observed.jobs == 0
            && observed.launchOperation.isNull() && observed.residentMappingBytes == 0 && !observed.selected
            && config.diagnostics->trySnapshot(diagnosticFailure) && !diagnosticFailure,
            "inert diagnostics neither invent operations/failures nor load restoration resources");
        require(!inert.needsPoll() && inert.health().jobs.empty() && !config.root.exists() && !config.scratch.exists(),
            "queue construction creates no storage, worker, audio or polling");
        Queue::Request invalid; juce::Uuid id { nullptr };
        require(!inert.start(invalid, id) && id.isNull() && !config.root.exists(), "invalid admission preserves output and absent root");
    }
    good(RecoveryStorageLayout(config.root).prepare(config.storage), "prepare real shared service policy fixture");
    RecoveryStore store(RecoveryStorageLayout(config.root).journalDirectory(), juce::Uuid(), config.store);
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encode(snapshot, {}, packet, limits), "encode current and complete Undo/Redo graph");
    SafeHead head; good(store.append({}, packet, head), "publish real owner for asynchronous restore");
    PreparedRecoveryCheckpoint prepared;
    good(RecoveryCheckpointCodec::prepare(store.inspectManifest().commits.back(), prepared, limits), "inspect real mapping footprint");
    size_t mappingBytes = 0;
    for (const auto& sequence : prepared.getAudioGraph().sequences) mappingBytes += static_cast<size_t>(sequence.frames) * static_cast<size_t>(sequence.channels) * 4;
    require(mappingBytes > 0, "nonempty real mapping footprint");
    config.residentMappingBytes = mappingBytes;
    Queue::Request request; request.owner = store.ownerId(); request.observedHead = head; request.selectedHead = head;
    const auto outcome = [&](const juce::Uuid& attempt)
    {
        const auto view = RecoveryAttemptStore(RecoveryStorageLayout(config.root).attemptDirectory(), store.ownerId()).inspect();
        good(view.status, "independent observer reads durable attempt state");
        for (const auto& item : view.attempts) if (item.record.candidate == attempt) return item.record.outcome;
        require(false, "durable marker exists for started candidate"); return AttemptOutcome::active;
    };
    {
        Queue queue(config); juce::Uuid first;
        good(queue.start(request, first), "accept actual recovery attempt");
        auto candidate = core(queue, first);
        Queue::DiagnosticObservation observed;
        require(queue.tryDiagnosticObservation(first, observed) && observed.selected && observed.selected->attempt == first
            && observed.selected->waitingForAdmission && !observed.workerRunning && observed.residentMappingBytes == mappingBytes
            && !observed.launchOperation.isNull() && observed.launchOperation != first
            && config.diagnostics->trySnapshot(diagnosticFailure) && !diagnosticFailure,
            "actual admission-wait snapshot names real attempt and separate worker launch without inventing a failure");
        require(outcome(first) == AttemptOutcome::active && candidate->document->requiresRecoverySaveAs(),
            "CoreReady has not claimed success and cannot overwrite original");
        auto playlist = RecoveredPlaylistDocument::fromCandidate(*candidate);
        require(playlist != nullptr, "actual playlist adapter accepts complete workspace image");
        good(queue.acknowledgeCore(first, true), "acknowledge actual playlist admission");
        require(!queue.acknowledgeCore(first, true), "duplicate admission rejected");
        settled(queue, first, Stage::ready);
        require(!queue.forget(first), "pending full-waveform publication cannot be forgotten");
        auto full = queue.takePublications();
        require(full.size() == 1 && full[0].fullPeaks && !full[0].fullPeaks->isPreview
            && full[0].fullPeaks->loadedSampleRange.getEnd() == snapshot.state->info.numSamples
            && full[0].originalCore == candidate->coreState && outcome(first) == AttemptOutcome::succeeded,
            "actual full materialization publishes exact original core and durable success");
        {
            SingleTrackDocument restored; juce::String error;
            require(restored.adoptWorkspaceSnapshot(*playlist->snapshot(), error), "select full recovered workspace without loadFile");
            const auto history = restored.captureCommittedState(restored.getMediaIdentity().document);
            require(history && history->undo.size() == snapshot.undo.size() && history->redo.size() == snapshot.redo.size(), "full history survives queue-to-playlist admission");
            playback(restored, *snapshot.state);
            for (auto at = snapshot.undo.rbegin(); at != snapshot.undo.rend(); ++at)
            { require(restored.undo(), "real Undo retained"); playback(restored, **at); }
            for (size_t i = 0; i < snapshot.undo.size(); ++i) require(restored.redo(), "return to initial restored state");
            for (auto at = snapshot.redo.rbegin(); at != snapshot.redo.rend(); ++at)
            { require(restored.redo(), "real Redo retained"); playback(restored, **at); }
        }
        good(queue.forget(first), "remove finished task metadata only");
        candidate.reset(); full.clear();
        require(queue.health().residentMappingBytes == mappingBytes, "playlist/history reference keeps mapping budget after forgotten task");
        juce::Uuid second; good(queue.start(request, second), "second attempt admitted before mapping allocation");
        settled(queue, second, Stage::failed);
        require(job(queue, second).failure.code == "restore_resident_allocation_budget" && outcome(second) == AttemptOutcome::failed,
            "aggregate resident budget refuses actual second graph without evicting live history");
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == second && diagnosticFailure->owner == store.ownerId()
            && diagnosticFailure->stage == RecoveryDiagnosticState::Stage::restoreReplay
            && diagnosticFailure->error == Error::budgetExceeded && diagnosticFailure->counters[1] == static_cast<int64_t>(mappingBytes)
            && diagnosticFailure->counters[2] == head.sequence && job(queue, second).failureStage == Stage::replayingCheckpoint,
            "real mapping-budget failure records actual phase, attempt and retained-reader memory after candidate cleanup");
        const auto budgetFailure = *diagnosticFailure;
        playlist.reset(); require(queue.health().residentMappingBytes == 0, "last real audio/history reference releases reservation");
        juce::Uuid retry; good(queue.retry(second, false, retry), "explicit retry after actual resource release");
        require(retry != second, "controlled failure retry uses distinct attempt identity");
        auto rejected = core(queue, retry);
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == budgetFailure.operationId && diagnosticFailure->counters == budgetFailure.counters,
            "successful actual retry to CoreReady preserves the earlier immutable failure");
        good(queue.acknowledgeCore(retry, false), "simulate actual playlist refusal through admission API");
        settled(queue, retry, Stage::failed);
        require(job(queue, retry).failure.code == "restore_playlist_admission" && outcome(retry) == AttemptOutcome::failed,
            "playlist refusal is a durable controlled failure, never success");
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure && diagnosticFailure->operationId == retry
            && diagnosticFailure->stage == RecoveryDiagnosticState::Stage::restoreAdmission
            && job(queue, retry).failureStage == Stage::coreReady,
            "actual UI refusal replaces the single failure slot without confusing terminal stage with failed phase");
        rejected.reset(); good(queue.forget(retry), "forget observed failed task");
        require(queue.health().residentMappingBytes == 0 && store.checkHead(head), "failed task releases private readers and preserves original owner");
    }
    {
        auto gate = std::make_shared<Gate>(); auto gated = config;
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure, "prior real failure is available before cancellation");
        const auto priorFailure = *diagnosticFailure;
        gated.beforeAdvance = [gate](Stage stage, juce::Uuid) { if (stage == Stage::materializingFull) gate->hold(); };
        Queue queue(gated); juce::Uuid id; good(queue.start(request, id), "start cancellation after CoreReady");
        auto published = core(queue, id);
        good(queue.acknowledgeCore(id, true), "admit before explicit cancel");
        gate->await();
        good(queue.cancel(id), "cancel live full materialization");
        gate->release();
        settled(queue, id, Stage::cancelled);
        require(outcome(id) == AttemptOutcome::cancelled && published->document->captureWorkspaceSnapshot(), "cancel retains actual published document and durable cancellation");
        require(queue.health().residentMappingBytes == mappingBytes, "cancel does not release budget still used by published history");
        published.reset(); queue.takePublications(); good(queue.forget(id), "clear only finished cancelled entry");
        require(queue.health().residentMappingBytes == 0, "cancelled graph releases after final consumer");
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == priorFailure.operationId && diagnosticFailure->counters == priorFailure.counters,
            "successful cancellation and diagnostics reads preserve the earlier real failure");
    }
    {
        auto gate = std::make_shared<Gate>();
        auto gated = config; gated.jobs = 1;
        gated.beforeAdvance = [gate](Stage stage, juce::Uuid) { if (stage == Stage::queued) gate->hold(); };
        auto queue = std::make_unique<Queue>(gated); juce::Uuid id, refused { nullptr };
        good(queue->start(request, id), "accept owned worker before close"); gate->await();
        require(queue->start(request, refused).code == "restore_job_budget" && refused.isNull(), "bounded jobs refuse without changing caller identity");
        const auto before = juce::Time::getMillisecondCounterHiRes(); queue.reset();
        require(juce::Time::getMillisecondCounterHiRes() - before < 250, "queue destruction does not join blocked worker");
        gate->release();
        // A marker is deliberately absent for cancellation before the first
        // advance. Observe completion via the captured worker-owned guard.
        waitFor([&] { return gate.use_count() == 2; }, "detached worker exits and releases its shared state after explicit close");
        good(store.checkHead(head), "closing service never removes original journal");
    }
    {
        auto gate = std::make_shared<Gate>(); auto gated = config; gated.jobs = 2;
        gated.beforeAdvance = [gate](Stage stage, juce::Uuid) { if (stage == Stage::queued) gate->hold(); };
        Queue queue(gated); juce::Uuid first, second;
        good(queue.start(request, first), "start first bounded concurrent job"); gate->await();
        good(queue.start(request, second), "queue a distinct second job while worker is occupied");
        good(queue.cancel(second), "explicitly cancel pending job without touching current worker candidate");
        gate->release();
        auto publication = core(queue, first);
        require(job(queue, second).stage == Stage::cancelled, "round-robin worker drains cancelled pending job while first awaits UI");
        good(queue.forget(second), "cancelled pending job releases its metadata capacity");
        good(queue.cancel(first), "cancel CoreReady before playlist acknowledgment"); settled(queue, first, Stage::cancelled);
        require(!queue.acknowledgeCore(first, true) && queue.takePublications().empty() && outcome(first) == AttemptOutcome::cancelled,
            "late UI acknowledgment cannot resurrect cancelled or duplicate core publication");
        require(queue.health().residentMappingBytes == mappingBytes, "already offered packet retains its real reader lease");
        publication.reset(); good(queue.forget(first), "clear fully observed cancellation");
        require(queue.health().jobs.empty() && queue.health().residentMappingBytes == 0, "both pending and admission-wait jobs finish without retained audio leak");
    }
    {
        Queue queue(config); juce::Uuid id; good(queue.start(request, id), "start exact-head failure fixture");
        auto candidate = core(queue, id);
        SafeHead later; packet.timestampMilliseconds += 1;
        good(store.append(head, packet, later), "real later durable head invalidates frozen request");
        good(queue.acknowledgeCore(id, true), "acknowledge original publication before worker fence");
        settled(queue, id, Stage::failed);
        require(job(queue, id).failure.error == Error::staleHead && outcome(id) == AttemptOutcome::failed, "post-Core actual head change fails current attempt");
        candidate.reset(); juce::Uuid retry;
        good(queue.retry(id, false, retry), "retry retains frozen request"); settled(queue, retry, Stage::failed);
        require(job(queue, retry).failure.error == Error::staleHead && store.checkHead(later), "retry does not silently select a newer head or alter owner");
        require(config.diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure && diagnosticFailure->operationId == retry
            && diagnosticFailure->error == Error::staleHead && diagnosticFailure->stage == RecoveryDiagnosticState::Stage::restoreStorage,
            "frozen-head retry failure records its actual new attempt at the real pre-storage fence");
    }
    std::cout << "Recovery restore queue: actual admission, history PCM, durable outcomes, mapping lifetime, cancellation and frozen-head retry passed\n";
}
