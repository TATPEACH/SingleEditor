#include "Recovery/RecoveryMutationProducer.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include "Recovery/RecoveryProtectionSession.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool value, const char* message)
{ if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Producer: " << status.code << '\n'; require(static_cast<bool>(status), message); }
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
RecoveryDiagnosticState::Failure diagnosticFailure(const std::shared_ptr<RecoveryDiagnosticState>& diagnostics)
{
    std::optional<RecoveryDiagnosticState::Failure> result;
    require(diagnostics->trySnapshot(result) && result.has_value(), "actual Session failure has a bounded diagnostic result");
    return *result;
}
Snapshot snapshot(SingleTrackDocument& document)
{
    auto result = document.captureCommittedState(document.getMediaIdentity().document);
    require(result.has_value(), "capture independent expected committed state");
    return *result;
}
void compareState(const SingleTrackDocument::StatePtr& actual, const SingleTrackDocument::StatePtr& expected,
                  const SingleTrackDocument::StatePtr& audioOracle = {})
{
    require(actual && expected && actual->changeGeneration == expected->changeGeneration
        && actual->info.numSamples == expected->info.numSamples
        && actual->info.numChannels == expected->info.numChannels
        && actual->envelopePoints.size() == expected->envelopePoints.size(), "restore exact committed state dimensions/generation");
    for (size_t i = 0; i < actual->envelopePoints.size(); ++i)
        require(actual->envelopePoints[i].timeSeconds == expected->envelopePoints[i].timeSeconds
            && actual->envelopePoints[i].gain == expected->envelopePoints[i].gain, "restore exact envelope from that event, not a later Document");
    juce::AudioBuffer<float> decoded;
    const auto& audioState = audioOracle ? audioOracle : expected;
    const juce::AudioBuffer<float>* oracle = audioState->audioBuffer.get();
    if (!oracle || oracle->getNumSamples() == 0)
    {
        auto independent = audioState->sourceFileWitness && audioState->sourceFileWitness->retainedInput()
            ? SingleTrackDocument::createReaderForRetainedSource(audioState->sourceFileWitness)
            : SingleTrackDocument::createReaderForFile(audioState->readerSourceFile);
        require(independent != nullptr, "create separate decoder oracle");
        decoded.setSize(expected->info.numChannels, static_cast<int>(expected->info.numSamples));
        require(independent->read(&decoded, 0, decoded.getNumSamples(), 0, true, true), "decode independent original source oracle");
        oracle = &decoded;
    }
    if (!actual->audioBuffer || actual->audioBuffer->getNumSamples() != oracle->getNumSamples())
        std::cerr << "PCM comparison: actual=" << (actual->audioBuffer ? actual->audioBuffer->getNumSamples() : -1)
            << " oracle=" << oracle->getNumSamples() << " generation=" << actual->changeGeneration
            << " expected=" << expected->changeGeneration << " imported=" << static_cast<bool>(audioOracle) << '\n';
    require(actual->audioBuffer && actual->audioBuffer->getNumSamples() == oracle->getNumSamples(), "restored PCM is complete");
    for (int c = 0; c < oracle->getNumChannels(); ++c)
        require(std::memcmp(actual->audioBuffer->getReadPointer(c), oracle->getReadPointer(c),
            sizeof(float) * static_cast<size_t>(oracle->getNumSamples())) == 0, "automatic producer roundtrip is bit-exact for all PCM");
}
void compare(const Snapshot& actual, const Snapshot& expected, const Snapshot* audioOracle = nullptr)
{
    require(actual.identity.document.sameVersion(expected.identity.document)
        && actual.undo.size() == expected.undo.size() && actual.redo.size() == expected.redo.size(), "restore exact identity and Undo/Redo stack sizes");
    require(!audioOracle || (audioOracle->undo.size() == expected.undo.size() && audioOracle->redo.size() == expected.redo.size()),
        "independent audio oracle identifies the selected history sizes");
    compareState(actual.state, expected.state, audioOracle ? audioOracle->state : nullptr);
    for (size_t i = 0; i < actual.undo.size(); ++i)
        compareState(actual.undo[i], expected.undo[i], audioOracle ? audioOracle->undo[i] : nullptr);
    for (size_t i = 0; i < actual.redo.size(); ++i)
        compareState(actual.redo[i], expected.redo[i], audioOracle ? audioOracle->redo[i] : nullptr);
}
RecoveryMutationProducer::Config config(const juce::File& root, const char* child)
{
    RecoveryMutationProducer::Config value;
    value.root = root.getChildFile(child);
    value.queue.maximumAge = 10s; // Fault tests control worker obstruction, not a timing benchmark.
    return value;
}
void runProtectionSessionTests(const juce::File& root, const juce::File& source)
{
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load real source for capture callback failure");
        int consumed = 0, reported = 0; uint64_t failedSequence = 0; Status reportedStatus;
        DocumentRecoveryCapture capture(document,
            [&](auto) { ++consumed; throw std::runtime_error("owned consumer failure"); },
            [&](const Status& status, uint64_t sequence) { ++reported; reportedStatus = status; failedSequence = sequence; });
        require(document.moveEnvelopePoint(0, 0, 0.75f) && consumed == 1 && reported == 1
            && reportedStatus.code == "capture_consumer_exception" && failedSequence > 0,
            "actual capture consumer exception reports its real event before observation stops");
        require(document.moveEnvelopePoint(0, 0, 0.55f) && consumed == 1 && reported == 1
            && !capture.latestFailure(), "stopped observer and status reads never repeat the failure callback");
    }
    const auto verify = [&] (const RecoveryProtectionSession::Config& settings,
                            const RecoveryMutationProducer::Health& health, const std::vector<Snapshot>& expected)
    {
        RecoveryStore store(settings.producer.root, health.owner);
        const auto view = store.inspect(); good(view.status, "inspect actual session-owned durable epoch");
        require(view.commits.size() == expected.size(), "session preserves each committed mutation across source epochs");
        for (size_t i = 0; i < expected.size(); ++i)
        {
            Snapshot decoded;
            good(RecoveryCheckpointCodec::decode(view.commits[i], store, decoded), "decode session-owned epoch without live state");
            compare(decoded, expected[i]);
        }
    };
    {
        SingleTrackDocument document;
        require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.7f), "prepare actual already dirty session admission");
        RecoveryProtectionSession::Config settings; settings.producer = config(root, "session-dirty-admission");
        settings.producer.queue.diagnostics = std::make_shared<RecoveryDiagnosticState>();
        std::unique_ptr<RecoveryProtectionSession> session;
        require(!RecoveryProtectionSession::attachPostCore(document, settings, session) && !session
            && !settings.producer.root.exists(), "implicit clean admission cannot silently adopt existing dirty edits");
        const auto implicitFailure = diagnosticFailure(settings.producer.queue.diagnostics);
        require(!implicitFailure.operationId.isNull() && implicitFailure.owner.isNull()
            && implicitFailure.stage == RecoveryDiagnosticState::Stage::sessionAdmission
            && implicitFailure.error == Error::staleHead, "real refused admission records no invented owner");
        auto tooSmall = settings; tooSmall.retainedBytes = 1;
        require(!RecoveryProtectionSession::attachPostCore(document, tooSmall, session, RecoveryMutationProducer::Admission::checkpointDirty)
            && !session && !settings.producer.root.exists(), "failed explicit checkpoint admission preserves output and creates no recovery directory");
        const auto budgetFailure = diagnosticFailure(settings.producer.queue.diagnostics);
        require(budgetFailure.operationId != implicitFailure.operationId && budgetFailure.error == Error::budgetExceeded,
            "distinct actual admission attempts carry distinct diagnostics");
        const auto initial = snapshot(document);
        good(RecoveryProtectionSession::attachPostCore(document, settings, session, RecoveryMutationProducer::Admission::checkpointDirty),
            "explicit dirty admission captures actual history without a fake mutation");
        require(diagnosticFailure(settings.producer.queue.diagnostics).operationId == budgetFailure.operationId,
            "successful dirty admission retains its preceding failure");
        const auto* retainedSession = session.get();
        require(!RecoveryProtectionSession::attachPostCore(document, settings, session, RecoveryMutationProducer::Admission::checkpointDirty)
            && session.get() == retainedSession && document.getMediaIdentity().document.sameVersion(initial.identity.document),
            "duplicate admission preserves existing controller and document identity");
        require(document.moveEnvelopePoint(0, 0, 0.3f), "edit once after rejected and successful controller admissions");
        const auto next = snapshot(document);
        require(session->drain(10s) && session->health().currentProtected, "one surviving observer protects actual dirty admission and one later edit");
        verify(settings, session->health().current, { initial, next });
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load ordinary source for continuous protection lifecycle");
        RecoveryProtectionSession::Config settings; settings.producer = config(root, "session-save-epochs");
        std::unique_ptr<RecoveryProtectionSession> session;
        good(RecoveryProtectionSession::attachPostCore(document, settings, session), "attach one post-Core session observer");
        require(!settings.producer.root.exists() && session->health().current.owner.isNull()
            && session->health().retainedBytes == 0 && session->drain(0ms), "clean session has no owner, writer, I/O or drain work");
        require(!document.moveEnvelopePoint(0, 0, 1.0f) && !document.moveEnvelopePoint(-1, 0, 0.7f)
            && !settings.producer.root.exists(), "no-op and failed mutations keep whole session inert");
        require(document.saveAsToFile(root.getChildFile("session-clean-output.wav"))
            && session->health().current.owner.isNull() && session->health().retired.empty()
            && !settings.producer.root.exists(), "saving a read-only document also creates no recovery owner or worker");
        require(document.moveEnvelopePoint(0, 0, 0.8f), "first actual session mutation");
        const auto first = snapshot(document);
        const auto oldSave = document.createSaveSnapshot(root.getChildFile("session-output.wav"), SingleTrackDocument::SaveIntent::saveAs);
        require(document.moveEnvelopePoint(0, 0, 0.6f), "newer edit while older Save As is outstanding");
        const auto second = snapshot(document);
        const auto saved = SingleTrackDocument::writeSaveSnapshot(oldSave);
        require(saved.succeeded && document.markSaveSnapshotCommitted(oldSave, saved), "complete actual older Save As through native receipt");
        const auto rebound = snapshot(document);
        require(document.moveEnvelopePoint(0, 0, 0.4f), "edit immediately inside same owner turn after Save, without pumping messages");
        const auto next = snapshot(document);
        require(session->drain(10s), "continuous observer drains both actual source epochs");
        const auto protectedState = session->health();
        require(protectedState.currentProtected && protectedState.retired.size() == 1
            && protectedState.current.owner != protectedState.retired[0].owner && protectedState.retainedBytes == 0,
            "latest exact dirty version is protected and retired ownership is separate");
        verify(settings, protectedState.retired[0], { first, second });
        verify(settings, protectedState.current, { rebound, next });
        const auto beforeSave = document.getState();
        require(document.saveOverSourceFile() && document.getState() == beforeSave && !document.hasUnsavedChanges(), "real clean Save preserves live state");
        require(session->health().current.owner.isNull() && session->health().current.phase == RecoveryMutationProducer::Phase::idle,
            "clean Save returns current protection to inert observation");
        require(document.undo(), "first Undo after clean Save immediately starts a new owner");
        const auto undone = snapshot(document);
        require(session->drain(10s) && session->health().currentProtected, "Undo receives automatic protection without manual reattachment");
        verify(settings, session->health().current, { undone });
        const auto completed = session->takeDrainedEpochs();
        require(completed.size() == 2 && session->health().retired.empty(), "drained epoch runtime metadata transfers to lifecycle host");
        for (const auto& old : completed)
        {
            RecoveryStore store(settings.producer.root, old.owner);
            require(store.inspect().head == old.writer.safeHead, "runtime release does not tombstone or remove old durable data");
        }
        const auto closedHead = session->health().current.writer.safeHead;
        document.clear();
        require(session->health().closed && session->drain(10s), "document clear retires safely inside its existing callback");
        require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.2f), "open and edit a later document");
        require(!session->health().currentProtected && session->health().current.writer.safeHead == closedHead,
            "closed session cannot claim current later Document protection");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for retired-writer failure and bounded close");
        RecoveryProtectionSession::Config settings; settings.producer = config(root, "session-real-fault");
        require(settings.producer.root.replaceWithText("owned obstruction"), "obstruct only session test storage root");
        std::unique_ptr<RecoveryProtectionSession> session;
        good(RecoveryProtectionSession::attachPostCore(document, settings, session), "read-only session does not inspect broken storage");
        require(document.moveEnvelopePoint(0, 0, 0.8f), "commit before actual storage failure");
        const auto first = snapshot(document);
        const auto request = document.createSaveSnapshot(root.getChildFile("session-fault-output.wav"), SingleTrackDocument::SaveIntent::saveAs);
        require(!session->drain(10s) && !session->health().failure, "real writer failure is visible and never declared protected");
        require(document.moveEnvelopePoint(0, 0, 0.6f), "queue second actual edit behind failed front");
        const auto second = snapshot(document);
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && document.markSaveSnapshotCommitted(request, saved), "Save source changes while recovery storage is unavailable");
        const auto rebound = snapshot(document);
        require(document.moveEnvelopePoint(0, 0, 0.4f), "new source epoch also observes subsequent mutation");
        const auto last = snapshot(document);
        require(!session->drain(10s) && session->health().pendingPackets == 4 && session->takeDrainedEpochs().empty(),
            "both failed immutable queues are retained, no empty safe-head success");
        const auto failed = session->health();
        session->close();
        require(!session->drain(0ms) && !session->retry(juce::Uuid()), "closed queue reports unfinished work and rejects unrelated retry owner");
        require(settings.producer.root.deleteFile(), "remove only explicit owned root obstruction");
        good(session->retry(failed.retired[0].owner), "retry accepted old queue after epoch close");
        good(session->retry(failed.current.owner), "retry accepted current queue after session close");
        require(session->drain(10s) && session->health().pendingPackets == 0 && session->health().retainedBytes == 0,
            "closed session can finish both exact queues without accepting future edits");
        verify(settings, session->health().retired[0], { first, second });
        verify(settings, session->health().current, { rebound, last });
        require(!session->health().current.writer.accepting && !session->health().retired[0].writer.accepting,
            "explicit draining retry never reopens an old writer");
        const auto head = session->health().current.writer.safeHead;
        require(document.undo() && session->health().current.writer.safeHead == head,
            "closed observer ignores later real Undo without altering accepted safe head");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for bounded source epoch count");
        RecoveryProtectionSession::Config settings; settings.producer = config(root, "session-epoch-cap"); settings.retiredEpochs = 1;
        settings.producer.queue.diagnostics = std::make_shared<RecoveryDiagnosticState>();
        std::unique_ptr<RecoveryProtectionSession> session;
        good(RecoveryProtectionSession::attachPostCore(document, settings, session), "attach bounded epoch controller");
        require(document.moveEnvelopePoint(0, 0, 0.8f) && session->drain(10s)
            && document.saveAsToFile(root.getChildFile("session-cap-first.wav")), "first actual saved epoch reaches bounded retired slot");
        require(document.moveEnvelopePoint(0, 0, 0.6f) && session->drain(10s)
            && document.saveAsToFile(root.getChildFile("session-cap-second.wav")), "second actual save preserves product behavior at protection cap");
        require(session->health().failure.code == "session_epoch_budget" && session->health().checkpointRequired
            && session->health().retired.size() == 1, "bounded controller degrades explicitly instead of evicting old ownership");
        const auto transitionFailure = diagnosticFailure(settings.producer.queue.diagnostics);
        require(transitionFailure.stage == RecoveryDiagnosticState::Stage::sessionTransition
            && transitionFailure.error == Error::budgetExceeded && transitionFailure.counters[0] > 0
            && transitionFailure.counters[1] == 1 && transitionFailure.owner == session->health().current.owner,
            "actual failed epoch transition identifies observation, exact actor and event sequence");
        require(session->takeDrainedEpochs().size() == 1 && document.moveEnvelopePoint(0, 0, 0.4f), "host releases drained epoch metadata and document remains editable");
        const auto expected = snapshot(document);
        good(session->checkpointCurrent(), "explicit full current checkpoint repairs the source transition gap");
        require(session->drain(10s) && session->health().currentProtected, "repaired epoch acknowledges exact current version");
        require(diagnosticFailure(settings.producer.queue.diagnostics).operationId == transitionFailure.operationId,
            "actual successful checkpoint and health reads do not overwrite earlier transition failure");
        verify(settings, session->health().current, { expected });
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for session-wide retained-memory bound");
        RecoveryProtectionSession::Config settings; settings.producer = config(root, "session-memory-cap"); settings.retainedBytes = 60000;
        settings.producer.queue.diagnostics = std::make_shared<RecoveryDiagnosticState>();
        require(settings.producer.root.replaceWithText("owned memory fixture obstruction"), "hold first accepted immutable capture with real storage obstruction");
        std::unique_ptr<RecoveryProtectionSession> session;
        good(RecoveryProtectionSession::attachPostCore(document, settings, session), "attach shared-memory-budget session");
        require(document.moveEnvelopePoint(0, 0, 0.8f) && !session->drain(10s), "first real capture remains retained during failure");
        auto lastAccepted = snapshot(document);
        const auto old = session->health().current.owner;
        for (int edit = 0; edit < 16 && !session->health().checkpointRequired; ++edit)
        {
            require(document.moveEnvelopePoint(0, 0, 0.7f - static_cast<float>(edit) * 0.03f), "retain another real metadata snapshot");
            if (!session->health().checkpointRequired) lastAccepted = snapshot(document);
        }
        require(session->health().failure.code == "session_retained_budget"
            && session->health().checkpointRequired && session->health().retainedBytes <= settings.retainedBytes,
            "shared PCM does not exempt accumulated metadata from the unchanged aggregate budget");
        const auto memoryFailure = diagnosticFailure(settings.producer.queue.diagnostics);
        require(memoryFailure.stage == RecoveryDiagnosticState::Stage::sessionTransition && memoryFailure.owner == old
            && memoryFailure.error == Error::budgetExceeded, "actual aggregate-memory rejection supersedes old writer failure");
        require(session->checkpointCurrent().error == Error::budgetExceeded, "full checkpoint cannot bypass retained Session capacity");
        const auto checkpointFailure = diagnosticFailure(settings.producer.queue.diagnostics);
        require(checkpointFailure.stage == RecoveryDiagnosticState::Stage::sessionCheckpoint
            && checkpointFailure.operationId != memoryFailure.operationId, "explicit rejected checkpoint has its own real operation ID");
        const auto expected = snapshot(document);
        require(settings.producer.root.deleteFile(), "remove owned memory-bound root obstruction");
        good(session->retry(old), "missed capture still allows its earlier accepted prefix to drain");
        require(!session->drain(10s) && session->health().pendingPackets == 0,
            "successful prefix drain alone does not repair missed mutation");
        good(session->checkpointCurrent(), "explicit full checkpoint captures every retained history state after memory releases");
        (void) session->drain(10s);
        require(session->health().currentProtected && session->takeDrainedEpochs().size() == 1 && session->drain(0ms),
            "new durable full checkpoint permits runtime release of superseded missed-event prefix");
        require(diagnosticFailure(settings.producer.queue.diagnostics).operationId == checkpointFailure.operationId,
            "successful prefix retry and full repair preserve the original failed checkpoint diagnostic");
        verify(settings, session->health().current, { expected });
        RecoveryStore store(settings.producer.root, old); const auto view = store.inspect();
        good(view.status, "superseded prefix still preserved on disk");
        Snapshot decoded; good(RecoveryCheckpointCodec::decode(view.commits.back(), store, decoded), "decode preserved accepted prefix"); compare(decoded, lastAccepted);
    }
    std::cout << "RecoveryProtectionSessionTests passed (continuous Save/Undo, exact multi-epoch queues, retired retry, close, memory/epoch bounds)\n";
}
}

void runRecoveryMutationProducerTests(const juce::File& root)
{
    const auto source = root.getChildFile("producer-source.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(output,
            juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24)));
        require(writer != nullptr, "create independent owned producer PCM fixture");
        juce::AudioBuffer<float> samples(2, 4800);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < samples.getNumSamples(); ++i)
                samples.setSample(c, i, static_cast<float>((i * 13 + c * 7) % 113 - 56) / 128.0f);
        require(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples()), "write deterministic producer PCM");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load real ordinary Document before producer admission");
        std::unique_ptr<RecoveryMutationProducer> producer;
        auto settings = config(root, "producer-ordered");
        settings.graph.chunkSampleBytes = 4096;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "attach explicit post-Core observer");
        require(producer->health().phase == RecoveryMutationProducer::Phase::idle
            && producer->health().owner.isNull() && !producer->health().writer.workerRunning
            && !settings.root.exists(), "read-only observer creates no owner, files or worker");
        require(!document.moveEnvelopePoint(0, 0.0, 1.0f)
            && producer->health().phase == RecoveryMutationProducer::Phase::idle, "no-op envelope cannot create a recovery owner");
        require(!document.moveEnvelopePoint(-1, 0.0, 0.2f) && !settings.root.exists(), "failed edit cannot start protection");
        std::vector<Snapshot> expected;
        require(document.moveEnvelopePoint(0, 0.0, 0.7f), "commit actual first envelope mutation"); expected.push_back(snapshot(document));
        require(document.moveEnvelopePoint(0, 0.0, 0.4f), "commit actual second envelope mutation"); expected.push_back(snapshot(document));
        require(document.undo(), "actual Undo is itself a committed protection event"); expected.push_back(snapshot(document));
        require(document.redo(), "actual Redo is itself a committed protection event"); expected.push_back(snapshot(document));
        require(document.applySelectionGain({ 0.0, document.getDurationSeconds() }, 0.5f), "commit actual generated PCM mutation");
        expected.push_back(snapshot(document));
        require(producer->drain(10s), "actual mutation events drain without manual packet submission");
        const auto health = producer->health();
        require(health.phase == RecoveryMutationProducer::Phase::protectedState
            && health.writer.protectedIdentity.sameVersion(expected.back().identity.document)
            && health.writer.retainedBytes == 0, "protected health acknowledges the actually published Document version");
        RecoveryStore store(settings.root, health.owner);
        const auto view = store.inspect(); good(view.status, "automatic producer owner verifies through storage");
        require(view.commits.size() == expected.size(), "every successful mutation and history move is represented in order");
        require(view.commits[0].record["operation"] == juce::var("recovery.checkpoint")
            && view.commits[1].record["operation"] == juce::var("recovery.mutation")
            && view.commits[2].record["operation"] == juce::var("recovery.mutation")
            && view.commits[3].record["operation"] == juce::var("recovery.mutation")
            && view.commits[4].record["operation"] == juce::var("recovery.checkpoint"),
            "actual automatic producer writes checkpoint, envelope/Undo/Redo deltas, then new-PCM checkpoint");
        require(view.commits[1].record["state"].toString().getNumBytesAsUTF8() < 1500,
            "actual durable envelope record carries small delta instead of repeated full history");
        for (size_t i = 0; i < expected.size(); ++i)
        {
            Snapshot decoded;
            good(RecoveryCheckpointCodec::decode(view.commits[i], store, decoded), "decode actual asynchronously published checkpoint");
            compare(decoded, expected[i]);
        }
        {
            RecoveryRestoreCandidate::Request request;
            request.observedOwnerHead = view.head;
            request.checkpointHead = view.commits[2].head; // Earlier Undo delta, before latest PCM checkpoint.
            request.scratchDirectory = root.getChildFile("mixed-producer-scratch");
            request.attemptDirectory = root.getChildFile("mixed-producer-attempts");
            request.appBuild = "mixed-producer-tests-v1"; request.limits = settings.graph;
            require(request.scratchDirectory.createDirectory().wasOk(), "create only owned mixed-candidate scratch root");
            RecoveryRestoreCandidate candidate(store, request);
            for (int steps = 0; candidate.getStage() != RecoveryAttemptStage::coreReady && steps < 20; ++steps)
                good(candidate.advance(), "prepare actual earlier delta through detached recovery pipeline");
            require(candidate.getStage() == RecoveryAttemptStage::coreReady, "earlier mixed-chain target reaches actual CoreReady");
            std::shared_ptr<const RecoveredCandidateDocument> acquired;
            good(candidate.acquireCore(candidate.getAttemptId(), acquired), "acquire actual mixed-chain recovered Document and waveform");
            require(acquired && acquired->document->getRecoveryOrigin()
                && acquired->document->getRecoveryOrigin()->sameVersion(expected[2].identity.document)
                && acquired->coreState->getPlayableSampleRange().getEnd() < acquired->coreState->info.numSamples
                && acquired->firstPeaks && !acquired->firstPeaks->loadedSampleRange.isEmpty(),
                "mixed metadata replay keeps selected generation and true progressive audio/waveform readiness");
            Snapshot oracle;
            good(RecoveryCheckpointCodec::decode(view.commits[2], store, oracle), "decode independent expected selected audio");
            const auto partial = snapshot(*acquired->document);
            const auto prefix = acquired->coreState->getPlayableSampleRange();
            auto readmissionSettings = config(root, "producer-recovered-admission");
            readmissionSettings.graph = settings.graph;
            std::unique_ptr<RecoveryMutationProducer> readmitted;
            good(RecoveryMutationProducer::attachPostCore(*acquired->document, readmissionSettings, readmitted,
                RecoveryMutationProducer::Admission::checkpointDirty), "explicitly protect actual recovered dirty copy before complete materialization");
            require(readmitted->drain(10s), "independent recovery checkpoint imports durable CAS without requiring full live coverage");
            const auto protectedCopy = readmitted->health();
            require(protectedCopy.phase == RecoveryMutationProducer::Phase::protectedState
                && protectedCopy.owner != health.owner
                && acquired->document->getMediaIdentity().document.sameVersion(partial.identity.document)
                && acquired->coreState->getPlayableSampleRange() == prefix,
                "checkpoint admission preserves actual recovered identity and partial playable range without inventing an edit");
            RecoveryStore copyStore(readmissionSettings.root, protectedCopy.owner);
            const auto copyView = copyStore.inspect(); good(copyView.status, "verify independent recovered-copy owner and all imported audio");
            require(copyView.commits.size() == 1 && copyView.commits.front().record["operation"] == juce::var("recovery.checkpoint")
                && store.inspect().head == view.head, "new owner starts with full checkpoint and leaves recovery source owner untouched");
            Snapshot protectedSnapshot;
            good(RecoveryCheckpointCodec::decode(copyView.commits.front(), copyStore, protectedSnapshot), "restore independent copy checkpoint");
            compare(protectedSnapshot, partial, &oracle);
            readmitted->close();
            for (int steps = 0; candidate.getStage() != RecoveryAttemptStage::ready && steps < 100; ++steps)
                good(candidate.advance(), "finish actual mixed-chain current/history materialization");
            require(candidate.getStage() == RecoveryAttemptStage::ready, "mixed-chain candidate completes actual current/Undo/Redo audio");
            auto recovered = snapshot(*acquired->document);
            require(recovered.undo.size() == expected[2].undo.size() && recovered.redo.size() == expected[2].redo.size(),
                "earlier Undo delta preserves its own history rather than latest checkpoint history");
            const auto verify = [&] (const auto& actual, const auto& wanted)
            {
                require(actual->streamingReader && actual->envelopePoints.front().gain == wanted->envelopePoints.front().gain,
                    "mixed candidate applies actual selected envelope and recovery reader");
                juce::AudioBuffer<float> audio(actual->info.numChannels, static_cast<int>(actual->info.numSamples));
                require(actual->streamingReader->read(&audio, 0, audio.getNumSamples(), 0, true, true), "read completed mixed-chain recovery audio");
                for (int c = 0; c < audio.getNumChannels(); ++c)
                    require(std::memcmp(audio.getReadPointer(c), wanted->audioBuffer->getReadPointer(c),
                        sizeof(float) * static_cast<size_t>(audio.getNumSamples())) == 0, "mixed-chain recovered current/history PCM is bit-exact");
            };
            verify(recovered.state, oracle.state);
            for (size_t i = 0; i < recovered.undo.size(); ++i) verify(recovered.undo[i], oracle.undo[i]);
            for (size_t i = 0; i < recovered.redo.size(); ++i) verify(recovered.redo[i], oracle.redo[i]);
        }
        require(root.getChildFile("mixed-producer-scratch").findChildFiles(juce::File::findFiles, false).isEmpty(),
            "actual mixed-chain candidate releases all owned scratch mappings after retirement");
        const auto safeHead = health.writer.safeHead;
        require(document.saveAsToFile(root.getChildFile("producer-saved.wav")), "complete actual Save As transaction");
        require(producer->health().phase == RecoveryMutationProducer::Phase::retired
            && producer->health().retirementReason == "source_rebound" && !producer->health().replacementCheckpoint,
            "clean source rebind retires the old owner explicitly without a dirty replacement request");
        require(document.moveEnvelopePoint(0, 0.0, 0.3f), "editing continues after producer retirement");
        require(store.inspect().head == safeHead && producer->retry().code == "producer_retired",
            "retired source epoch cannot silently protect new-source edits or reopen its queue");
        producer.reset();
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for real storage failure");
        auto settings = config(root, "producer-real-fault");
        require(settings.root.replaceWithText("owned obstruction"), "obstruct only the private owner root with a real file");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "constructor does not inspect obstructed root");
        require(document.moveEnvelopePoint(0, 0.0, 0.8f), "first committed edit survives actual storage fault");
        const auto first = snapshot(document);
        require(!producer->drain(10s) && producer->health().phase == RecoveryMutationProducer::Phase::degraded
            && !producer->health().writer.protectedIdentity.isValid()
            && producer->health().writer.safeHead.sequence == 0, "failed write never acknowledges a durable generation");
        require(document.moveEnvelopePoint(0, 0.0, 0.2f), "second edit remains usable behind a failed front packet");
        const auto second = snapshot(document);
        require(producer->health().writer.pendingPackets == 2 && !producer->health().writer.workerRunning,
            "no automatic retry or skipping the failed front edit");
        require(settings.root.deleteFile(), "remove only the owned obstruction");
        good(producer->retry(), "explicit retry resumes retained ordered captures");
        require(producer->drain(10s), "both immutable versions publish after storage recovers");
        const auto health = producer->health();
        RecoveryStore store(settings.root, health.owner);
        const auto view = store.inspect(); good(view.status, "retry owner verifies");
        require(view.commits.size() == 2 && health.writer.protectedIdentity.sameVersion(second.identity.document),
            "retry acknowledges latest exact version only after both commits");
        Snapshot decoded;
        good(RecoveryCheckpointCodec::decode(view.commits.front(), store, decoded), "decode retained first state after later edit"); compare(decoded, first);
        good(RecoveryCheckpointCodec::decode(view.commits.back(), store, decoded), "decode retained second state after retry"); compare(decoded, second);
        producer->close();
        require(document.undo() && producer->health().phase == RecoveryMutationProducer::Phase::retired
            && store.inspect().head == view.head, "closed observer detaches and cannot claim later Undo protection");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for bounded queue admission");
        auto settings = config(root, "producer-gap"); settings.queue.packets = 0;
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "invalid active queue remains inert until an edit");
        require(document.moveEnvelopePoint(0, 0.0, 0.6f), "queue rejection does not revert actual Document edit");
        require(producer->health().phase == RecoveryMutationProducer::Phase::degraded
            && producer->health().checkpointRequired && !settings.root.exists(), "rejected committed packet poisons continuity without disk writes");
        require(document.moveEnvelopePoint(0, 0.0, 0.3f) && producer->retry().code == "checkpoint_required"
            && !producer->drain(1ms), "later edit and retry cannot erase an omitted mutation");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for dirty admission and lifecycle");
        require(document.moveEnvelopePoint(0, 0.0, 0.6f), "real mutation before late attachment");
        std::unique_ptr<RecoveryMutationProducer> producer;
        auto settings = config(root, "producer-late");
        require(RecoveryMutationProducer::attachPostCore(document, settings, producer).code == "producer_dirty_admission"
            && !producer && !settings.root.exists(), "late dirty admission needs explicit fresh checkpoint, not invented first mutation");
        require(document.undo(), "restore clean source generation");
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "clean Undo target permits inert attachment");
        document.clear();
        require(producer->health().phase == RecoveryMutationProducer::Phase::retired
            && producer->health().retirementReason == "cleared" && !settings.root.exists(), "clear retires idle observer with no storage activation");
        require(document.loadFile(source) && document.moveEnvelopePoint(0, 0.0, 0.5f) && !settings.root.exists(),
            "replacement Document cannot activate a retired owner's observer");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load ordinary source for explicit dirty checkpoint admission");
        auto settings = config(root, "producer-explicit-dirty");
        std::unique_ptr<RecoveryMutationProducer> producer;
        require(RecoveryMutationProducer::attachPostCore(document, settings, producer,
            RecoveryMutationProducer::Admission::checkpointDirty).code == "producer_checkpoint_not_dirty"
            && !producer && !settings.root.exists(), "explicit dirty mode cannot activate a clean read-only Document");
        require(document.applySelectionGain({ 0.0, document.getDurationSeconds() }, 0.5f),
            "create real generated audio before explicit checkpoint admission");
        const auto initial = snapshot(document);
        require(RecoveryMutationProducer::attachPostCore(document, settings, producer).code == "producer_dirty_admission"
            && !producer && !settings.root.exists(), "default late observation still refuses dirty state");
        auto invalid = settings; invalid.queue.packets = 0;
        require(RecoveryMutationProducer::attachPostCore(document, invalid, producer,
            RecoveryMutationProducer::Admission::checkpointDirty).code == "invalid_submission_or_budget"
            && !producer && !settings.root.exists(), "initial queue rejection preserves null output and creates no files");
        invalid = settings; invalid.queue.retainedBytes = 1;
        require(RecoveryMutationProducer::attachPostCore(document, invalid, producer,
            RecoveryMutationProducer::Admission::checkpointDirty).code == "producer_retained_budget"
            && !producer && !settings.root.exists(), "initial memory bound rejects before owner activation");
        require(document.getMediaIdentity().document.sameVersion(initial.identity.document)
            && snapshot(document).state == initial.state, "failed admission cannot mutate current audio or history");
        require(settings.root.replaceWithText("owned checkpoint obstruction"), "force real asynchronous admission write failure");
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer,
            RecoveryMutationProducer::Admission::checkpointDirty), "accept dirty snapshot independently from storage availability");
        require(!producer->drain(10s) && producer->health().phase == RecoveryMutationProducer::Phase::degraded
            && !producer->health().writer.protectedIdentity.isValid() && producer->health().writer.pendingPackets == 1,
            "accepted initial snapshot is not misreported as durable after a real storage failure");
        require(document.moveEnvelopePoint(0, 0.0, 0.65f), "real edit follows explicit initial checkpoint despite failed write");
        const auto later = snapshot(document);
        require(producer->health().writer.pendingPackets == 2 && settings.root.loadFileAsString() == "owned checkpoint obstruction",
            "only accepted observer queues subsequent edit and leaves obstruction untouched");
        require(settings.root.deleteFile(), "remove only owned initial checkpoint obstruction");
        good(producer->retry(), "retry actual retained initial snapshot and later edit");
        require(producer->drain(10s), "dirty admission and subsequent commit publish in order");
        const auto health = producer->health();
        RecoveryStore store(settings.root, health.owner);
        const auto view = store.inspect(); good(view.status, "verify actual late admission chain");
        require(view.commits.size() == 2 && view.commits[0].record["operation"] == juce::var("recovery.checkpoint")
            && view.commits[1].record["operation"] == juce::var("recovery.mutation")
            && health.writer.protectedIdentity.sameVersion(later.identity.document),
            "late admission starts with complete generated-audio checkpoint then accepts actual delta");
        Snapshot restored;
        good(RecoveryCheckpointCodec::decode(view.commits[0], store, restored), "decode exact explicit initial checkpoint"); compare(restored, initial);
        good(RecoveryCheckpointCodec::decode(view.commits[1], store, restored), "decode actual subsequent edit"); compare(restored, later);
        const auto unchanged = snapshot(document);
        require(unchanged.identity.document.sameVersion(later.identity.document) && unchanged.state == later.state
            && unchanged.undo == later.undo && unchanged.redo == later.redo,
            "recovery writes leave the exact live current state and Undo/Redo ownership unchanged");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load ordinary source for overlapping Save As and newer edit");
        const auto settings = config(root, "producer-save-in-flight");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "attach old source epoch observer");
        require(document.moveEnvelopePoint(0, 0.0, 0.8f), "commit actual edit selected for Save As");
        const auto saveTarget = root.getChildFile("producer-older-saved.wav");
        const auto toSave = document.createSaveSnapshot(saveTarget, SingleTrackDocument::SaveIntent::saveAs);
        require(document.moveEnvelopePoint(0, 0.0, 0.4f) && producer->drain(10s), "later dirty edit becomes protected while earlier snapshot awaits save");
        const auto oldHealth = producer->health();
        RecoveryStore oldStore(settings.root, oldHealth.owner);
        const auto written = SingleTrackDocument::writeSaveSnapshot(toSave);
        require(written.succeeded, "actually render earlier snapshot to independent Save As target");
        document.markSaveSnapshotCommitted(toSave, written);
        const auto current = snapshot(document);
        const auto retired = producer->health();
        require(retired.phase == RecoveryMutationProducer::Phase::retired && retired.retirementReason == "source_rebound"
            && retired.replacementCheckpoint && current.identity.savedGeneration == toSave.changeGeneration
            && current.identity.savedGeneration != current.identity.document.generation
            && retired.replacementCheckpoint->document.sameVersion(current.identity.document)
            && retired.replacementCheckpoint->source.sameBinding(current.identity.source)
            && retired.replacementCheckpoint->savedGeneration == current.identity.savedGeneration,
            "completion of older Save As exposes the actual still-dirty new-source generation for replacement protection");
        auto savedReader = SingleTrackDocument::createReaderForFile(saveTarget);
        juce::AudioBuffer<float> savedSample(2, 1);
        require(savedReader && savedReader->read(&savedSample, 0, 1, 0, true, true)
            && std::abs(savedSample.getSample(0, 0) - (-56.0f / 128.0f) * 0.8f) < 0.000001f,
            "saved file contains the older selected envelope rather than newer live edit");
        savedReader.reset();
        auto nextSettings = config(root, "producer-saved-dirty-replacement");
        std::unique_ptr<RecoveryMutationProducer> next;
        good(RecoveryMutationProducer::attachPostCore(document, nextSettings, next,
            RecoveryMutationProducer::Admission::checkpointDirty), "checkpoint actual newer dirty state under independent source owner");
        require(next->drain(10s), "new-source dirty checkpoint becomes durable without another edit");
        const auto nextHealth = next->health();
        require(nextHealth.owner != oldHealth.owner && nextHealth.phase == RecoveryMutationProducer::Phase::protectedState
            && document.getMediaIdentity().document.sameVersion(current.identity.document)
            && oldStore.inspect().head == oldHealth.writer.safeHead,
            "new admission leaves current generation and old protected head intact");
        RecoveryStore nextStore(nextSettings.root, nextHealth.owner);
        auto view = nextStore.inspect(); good(view.status, "verify replacement checkpoint after actual Save As");
        require(view.commits.size() == 1 && view.commits[0].record["operation"] == juce::var("recovery.checkpoint"),
            "replacement source epoch does not replay a delta against an old owner");
        Snapshot restored;
        good(RecoveryCheckpointCodec::decode(view.commits[0], nextStore, restored), "decode newer dirty checkpoint after older file saved");
        compare(restored, current);
        require(restored.identity.source.sameBinding(current.identity.source)
            && restored.identity.savedGeneration == current.identity.savedGeneration, "replacement keeps actual source and saved-generation identity");
        require(document.moveEnvelopePoint(0, 0.0, 0.2f) && next->drain(10s), "new source epoch continues protecting real edits");
        view = nextStore.inspect(); good(view.status, "verify continuing replacement epoch");
        require(view.commits.size() == 2 && oldStore.inspect().head == oldHealth.writer.safeHead,
            "only new source owner observes later edits");
        good(RecoveryCheckpointCodec::decode(view.commits.back(), nextStore, restored), "restore subsequent new source edit");
        compare(restored, snapshot(document));
    }
    {
        const auto checkpointSource = root.getChildFile("producer-checkpoint-source.wav");
        require(source.copyFileTo(checkpointSource), "copy private fixture for actual in-place checkpoint save");
        SingleTrackDocument document;
        require(document.loadFile(checkpointSource), "load independent source for old in-place save and new dirty edit");
        const auto settings = config(root, "producer-in-place-save");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "observe source before in-place write");
        require(document.moveEnvelopePoint(0, 0.0, 0.8f), "edit selected for in-place file checkpoint");
        const auto toSave = document.createSaveSnapshot();
        require(document.moveEnvelopePoint(0, 0.0, 0.4f) && producer->drain(10s), "protect later dirty edit before replacing its original source path");
        const auto beforeSave = snapshot(document);
        const auto safe = producer->health();
        RecoveryStore oldStore(settings.root, safe.owner);
        const auto written = SingleTrackDocument::writeSaveSnapshot(toSave);
        require(written.succeeded, "actually replace private source with earlier saved render");
        document.markSaveSnapshotCommitted(toSave, written);
        const auto retired = producer->health();
        const auto identity = document.getMediaIdentity();
        require(retired.phase == RecoveryMutationProducer::Phase::retired && retired.retirementReason == "saved"
            && retired.replacementCheckpoint && identity.savedGeneration != identity.document.generation
            && retired.replacementCheckpoint->document.sameVersion(identity.document),
            "ordinary Save checkpoint also records the exact later dirty generation instead of assuming clean");
        const auto oldView = oldStore.inspect(); good(oldView.status, "previous self-contained owner remains valid after actual source overwrite");
        require(oldView.head == safe.writer.safeHead, "save notification does not tombstone the only protected dirty head");
        Snapshot restored;
        good(RecoveryCheckpointCodec::decode(oldView.commits.back(), oldStore, restored), "recover newer dirty edit from original pre-save protected owner");
        // The original path now names rendered audio, so compare against the
        // checkpoint's independently frozen baseline, not a reopened path oracle.
        Snapshot frozenBefore;
        good(RecoveryCheckpointCodec::decode(oldView.commits.front(), oldStore, frozenBefore), "read independent original-audio baseline after source replacement");
        require(restored.identity.document.sameVersion(beforeSave.identity.document)
            && restored.state->envelopePoints.front().gain == 0.4f
            && restored.undo.size() == beforeSave.undo.size() && restored.redo.size() == beforeSave.redo.size(),
            "old owner retains actual newer envelope and full history after original source has changed");
        compareState(restored.state, beforeSave.state, frozenBefore.state);
        auto nextSettings = config(root, "producer-in-place-dirty-admission");
        std::unique_ptr<RecoveryMutationProducer> next;
        good(RecoveryMutationProducer::attachPostCore(document, nextSettings, next,
            RecoveryMutationProducer::Admission::checkpointDirty), "admit newer dirty state using retained original physical input");
        require(next->drain(10s) && next->health().phase == RecoveryMutationProducer::Phase::protectedState
            && next->health().writer.protectedIdentity.sameVersion(document.getMediaIdentity().document)
            && oldStore.inspect().head == safe.writer.safeHead,
            "actual in-place save no longer breaks continued dirty protection or the previous safe head");
        RecoveryStore newStore(nextSettings.root, next->health().owner);
        auto newView = newStore.inspect(); good(newView.status, "verify complete post-save owner");
        require(newView.commits.size() == 1, "post-save dirty admission creates one independent checkpoint");
        Snapshot afterSave;
        good(RecoveryCheckpointCodec::decode(newView.commits.front(), newStore, afterSave), "decode complete post-save current and Undo audio");
        compare(afterSave, snapshot(document), &restored);
        require(document.moveEnvelopePoint(0, 0.0, 0.2f) && next->drain(10s), "later actual edit reuses the preserved input across source replacement");
        newView = newStore.inspect(); good(newView.status, "verify continuing post-save delta");
        require(newView.commits.size() == 2 && newView.commits.back().record["operation"] == juce::var("recovery.mutation"),
            "post-save source does not force repeated full audio checkpoints");
        good(RecoveryCheckpointCodec::decode(newView.commits.back(), newStore, afterSave), "restore actual later edit after in-place save");
        compare(afterSave, snapshot(document));
        const auto savedAgain = document.createSaveSnapshot();
        const auto secondWrite = SingleTrackDocument::writeSaveSnapshot(savedAgain);
        require(secondWrite.succeeded && document.markSaveSnapshotCommitted(savedAgain, secondWrite),
            "second real in-place save validates the newer target while rendering the original retained input");
        require(!document.hasUnsavedChanges() && document.undo() && document.hasUnsavedChanges(),
            "repeated saves preserve actual Undo semantics and clean generation");
        auto thirdSettings = config(root, "producer-in-place-after-second-save");
        std::unique_ptr<RecoveryMutationProducer> third;
        good(RecoveryMutationProducer::attachPostCore(document, thirdSettings, third,
            RecoveryMutationProducer::Admission::checkpointDirty), "re-protect actual Undo after the second source replacement");
        require(third->drain(10s), "Undo after repeated in-place saves remains durably recoverable");
        RecoveryStore thirdStore(thirdSettings.root, third->health().owner);
        const auto thirdView = thirdStore.inspect(); good(thirdView.status, "verify repeated-save Undo owner");
        good(RecoveryCheckpointCodec::decode(thirdView.commits.front(), thirdStore, afterSave), "decode repeated-save Undo with original PCM");
        compare(afterSave, snapshot(document));
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load source for actual reentrant event race");
        struct NestedEdit final : SingleTrackDocument::Listener
        {
            SingleTrackDocument& document; bool fired = false;
            explicit NestedEdit(SingleTrackDocument& sourceDocument) : document(sourceDocument) {}
            void documentIdentityChanged(const media::IdentityEvent& event) override
            {
                if (!fired && event.kind == media::IdentityEventKind::generationChanged)
                { fired = true; require(document.moveEnvelopePoint(0, 0.0, 0.25f), "commit real nested listener mutation"); }
            }
        } nested(document);
        document.addListener(&nested);
        const auto settings = config(root, "producer-reentrant");
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "attach after reentrant host listener");
        require(document.moveEnvelopePoint(0, 0.0, 0.5f) && nested.fired, "exercise actual nested identity notification");
        require(producer->health().phase == RecoveryMutationProducer::Phase::degraded
            && producer->health().checkpointRequired && !settings.root.exists(), "stale or out-of-order actual event cannot certify a newer unobserved state");
        producer.reset(); document.removeListener(&nested);
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load real source for mixed writer checkpoints");
        const auto settings = config(root, "producer-periodic");
        const juce::Uuid owner;
        RecoveryStore store(settings.root, owner);
        RecoveryCheckpointWriter writer(settings.root, owner);
        SafeHead head;
        std::shared_ptr<const PublishedRecoveryCommit> receipt;
        for (int i = 0; i < 35; ++i)
        {
            require(document.moveEnvelopePoint(0, 0, i % 2 == 0 ? 0.7f : 0.6f), "commit actual operation in long mixed chain");
            auto captured = snapshot(document);
            CommitPacket packet;
            good(writer.prepare(captured, packet), "prepare bounded mixed-chain record");
            const bool checkpoint = i == 0 || i == 33;
            require(packet.operationId == (checkpoint ? "recovery.checkpoint" : "recovery.mutation"),
                "first capture and post-32-delta boundary produce checkpoints without losing operations");
            if (i > 0) require(packet.newAudio.empty() && writer.lastCaptureWork().sampleBytesReadOrCopied == 0,
                "periodic checkpoint does not recopy original audio");
            const auto before = writer.publishedMetadata() ? writer.publishedMetadata()->getHead() : SafeHead {};
            require(before == head, "preparation does not advance publication frontier");
            good(store.append(head, packet, head, &receipt), "durably publish mixed record");
            good(writer.published(receipt), "acknowledge both graph and metadata frontier");
            require(writer.publishedMetadata() && writer.publishedMetadata()->getHead() == head
                && writer.publishedMetadata()->getIdentity().document.sameVersion(captured.identity.document),
                "published metadata frontier identifies actual durable target");
            for (const auto& state : writer.publishedMetadata()->getStates())
                require(!state->audioBuffer && !state->streamingReader && !state->memoryMappedReader && !state->sourceFileWitness,
                    "durable metadata frontier retains no PCM, source reader or witness owner");
        }
        const auto manifest = store.inspectManifest(); good(manifest.status, "read actual long mixed record chain");
        require(manifest.commits.size() == 35, "periodic checkpoints preserve every existing record until reference-safe GC");
        PreparedRecoveryCheckpoint latest;
        good(RecoveryCheckpointCodec::prepareThrough(manifest, head, latest), "replay newest checkpoint plus trailing delta");
        require(latest.getIdentity().document.sameVersion(document.getMediaIdentity().document), "latest mixed point keeps exact Document version");
        PreparedRecoveryCheckpoint earlier;
        good(RecoveryCheckpointCodec::prepareThrough(manifest, manifest.commits[32].head, earlier), "replay complete 32-delta prefix before periodic checkpoint");
        auto altered = manifest;
        altered.commits[1].record = juce::JSON::parse(juce::JSON::toString(altered.commits[1].record, true));
        altered.commits[1].record.getDynamicObject()->setProperty("parent", juce::String::repeatedString("f", 64));
        auto preserved = latest;
        require(RecoveryCheckpointCodec::prepareThrough(altered, altered.commits[2].head, preserved).error == Error::staleHead
            && preserved.getHead() == latest.getHead(), "mixed semantic replay enforces immediate parent rather than matching only generation");
        require(RecoveryCheckpointCodec::prepareThrough(manifest, head, preserved, {}, [] { return true; }).error == Error::cancelled
            && preserved.getHead() == latest.getHead(), "cancelled mixed metadata preparation preserves earlier output");
        require(document.moveEnvelopePoint(0, 0, 0.25f), "commit actual next version for failed publication semantics");
        auto captured = snapshot(document);
        CommitPacket packet;
        require(writer.prepare(captured, packet, [] { return true; }).error == Error::cancelled
            && writer.publishedMetadata()->getHead() == head, "cancelled prepare cannot advance metadata base");
        good(writer.prepare(captured, packet), "retry the same captured mutation after cancellation");
        auto future = juce::JSON::parse(packet.stateJson);
        future.getDynamicObject()->setProperty("schema", 2);
        packet.stateJson = juce::JSON::toString(future, true);
        const auto safe = head;
        good(store.append(head, packet, head, &receipt), "Store frames a future semantic record with valid checksum and parent");
        require(writer.published(receipt).code == "checkpoint_publication_mismatch"
            && writer.publishedMetadata()->getHead() == safe, "different actual published body cannot falsely advance producer frontier");
        const auto futureManifest = store.inspectManifest(); good(futureManifest.status, "future semantic record remains structurally valid Store data");
        require(RecoveryCheckpointCodec::prepareThrough(futureManifest, head, preserved).error == Error::unsupportedSchema
            && preserved.getHead() == latest.getHead(), "valid framing cannot bypass unsupported mutation semantics");
        good(RecoveryCheckpointCodec::prepareThrough(futureManifest, safe, preserved), "explicit earlier safe point remains usable before unsupported tail");
        require(preserved.getHead() == safe, "earlier selection never silently substitutes a different recovery point");
        RecoveryRestoreCandidate::Request request;
        request.observedOwnerHead = head; request.checkpointHead = head;
        request.scratchDirectory = root.getChildFile("mixed-unsupported-scratch");
        request.attemptDirectory = root.getChildFile("mixed-unsupported-attempts"); request.appBuild = "mixed-tests-v1";
        RecoveryRestoreCandidate candidate(store, request);
        for (int i = 0; i < 10 && candidate.getStatus(); ++i) candidate.advance();
        require(candidate.getStage() == RecoveryAttemptStage::failed && candidate.getStatus().error == Error::unsupportedSchema
            && candidate.getAttemptRecord() && candidate.getAttemptRecord()->outcome == AttemptOutcome::failed,
            "actual detached candidate closes its durable attempt as unsupported before any audio adoption");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load ordinary source for real quota cleanup and retry");
        auto settings = config(root, "producer-orphan-retry");
        settings.store.ownerBytes = 1024 * 1024;
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "admit idle observer for maintenance flow");
        require(document.moveEnvelopePoint(0, 0.0, 0.8f) && producer->drain(10s), "first real edit reaches durable protected head");
        const auto protectedState = producer->health();
        const auto ownerDirectory = settings.root.getChildFile(protectedState.owner.toString());
        const auto garbage = ownerDirectory.getChildFile("staging-" + juce::Uuid().toString());
        const juce::MemoryBlock bytes(800 * 1024, true);
        require(garbage.replaceWithData(bytes.getData(), bytes.getSize()), "fill private owner quota with recognized failed staging");
        require(document.moveEnvelopePoint(0, 0.0, 0.4f), "next Document edit stays responsive despite recovery quota pressure");
        const auto expected = snapshot(document);
        require(!producer->drain(10s) && producer->health().writer.failure.code == "owner_budget"
            && producer->health().writer.safeHead == protectedState.writer.safeHead
            && producer->health().writer.pendingPackets == 1, "quota failure retains exact front mutation and earlier safe head");
        good(producer->retry(true), "request bounded orphan maintenance and ordered retry on existing background worker");
        require(producer->drain(10s), "actual producer resumes after collecting only orphan staging");
        const auto recovered = producer->health();
        require(recovered.phase == RecoveryMutationProducer::Phase::protectedState
            && recovered.writer.protectedIdentity.sameVersion(expected.identity.document)
            && recovered.writer.lastCollection.filesRemoved == 1 && recovered.writer.lastCollection.bytesRemoved == bytes.getSize()
            && !garbage.exists() && recovered.writer.retainedBytes == 0,
            "maintenance health reports real reclaimed bytes and only then acknowledges the retained edit");
        RecoveryStore store(settings.root, recovered.owner);
        const auto view = store.inspect(); good(view.status, "certify actual producer after cleanup/retry");
        require(view.commits.size() == 2 && view.commits.front().head == protectedState.writer.safeHead,
            "cleanup never drops the previous recovery/Undo boundary");
        Snapshot decoded;
        good(RecoveryCheckpointCodec::decode(view.commits.back(), store, decoded), "restore actually retried metadata/history/audio");
        compare(decoded, expected);
        producer->close();
        require(!producer->health().writer.workerRunning, "maintenance worker returns all runtime resources at idle close");
    }
    {
        SingleTrackDocument document;
        require(document.loadFile(source), "load real ordinary source for automatic compaction");
        auto settings = config(root, "automatic-checkpoint-compaction");
        settings.store.records = 8;
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(document, settings, producer), "attach real producer before first compaction mutation");
        require(!settings.root.exists(), "compaction introduces no pre-mutation persistence");
        for (int i = 0; i < 30; ++i)
        {
            require(document.moveEnvelopePoint(0, 0.0, 0.2f + 0.01f * static_cast<float>(i)), "commit actual envelope edit for bounded automatic history");
            require(producer->drain(10s), "actual producer drains publication and bounded maintenance");
            const auto health = producer->health();
            good(health.writer.maintenanceFailure, "automatic checkpoint maintenance succeeds");
            require(health.phase == RecoveryMutationProducer::Phase::protectedState
                && health.writer.protectedIdentity.sameVersion(document.getMediaIdentity().document),
                "maintenance never retracts or substitutes the actual durable Document identity");
        }
        require(document.undo(), "Undo produces a lower-generation committed state after multiple compactions");
        require(producer->drain(10s), "protect Undo after compaction without a chronological generation assumption");
        const auto expected = snapshot(document);
        const auto health = producer->health();
        RecoveryStore store(settings.root, health.owner, settings.store);
        const auto view = store.inspect(); good(view.status, "certify the product producer's compacted storage");
        require(view.head.sequence == 31 && view.commits.size() < settings.store.records
            && store.inspectManifest().checkpointCut.isNotEmpty(), "real product producer crosses the old record-count lifetime ceiling");
        Snapshot decoded;
        good(RecoveryCheckpointCodec::decode(view.commits.back(), store, decoded), "restore compacted full current Undo/Redo and audio");
        compare(decoded, expected);
        producer->close();
        require(!producer->health().writer.workerRunning, "compaction releases the existing worker at normal close");
    }
    runProtectionSessionTests(root, source);
    std::cout << "RecoveryMutationProducerTests passed (mixed durable records, progressive dirty admission, overlapping Save As, quota cleanup/retry, failures)\n";
}
