#include "Document/WorkspaceSessionSupport.h"
#include "Document/RecoveredPlaylistDocument.h"
#include "Audio/AudioEngine.h"
#include "Recovery/RecoveryMutationProducer.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include "Recovery/RecoveryOwnerLifecycle.h"
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Workspace snapshot: " << status.code << '\n'; require(static_cast<bool>(status), message); }
void makeSource(const juce::File& file)
{
    juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> output(file.createOutputStream());
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create only owned workspace source");
    juce::AudioBuffer<float> pcm(2, 12000);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < pcm.getNumSamples(); ++i) pcm.setSample(c, i, ((i * 23 + c) % 151 - 75) / 160.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write workspace source PCM");
}
void equivalent(const SingleTrackDocument::State& left, const SingleTrackDocument::State& right)
{
    require(left.info.numSamples == right.info.numSamples && left.info.numChannels == right.info.numChannels
        && left.info.sampleRate == right.info.sampleRate && left.envelopePoints.size() == right.envelopePoints.size()
        && left.audioBuffer == right.audioBuffer && left.memoryMappedReader == right.memoryMappedReader
        && left.streamingReader == right.streamingReader && left.sourceFileWitness == right.sourceFileWitness,
        "workspace activation preserves exact immutable audio/backing without decode or PCM duplication");
    for (size_t i = 0; i < left.envelopePoints.size(); ++i)
        require(left.envelopePoints[i].timeSeconds == right.envelopePoints[i].timeSeconds
            && left.envelopePoints[i].gain == right.envelopePoints[i].gain, "workspace activation preserves envelope semantics");
}
struct Observed final : SingleTrackDocument::Listener
{
    SingleTrackDocument& document;
    int identities = 0, changes = 0;
    explicit Observed(SingleTrackDocument& doc) : document(doc) { doc.addListener(this); }
    ~Observed() override { document.removeListener(this); }
    void documentIdentityChanged(const media::IdentityEvent& event) override
    {
        ++identities;
        const auto snapshot = document.captureCommittedState(event.after.document);
        require(snapshot.has_value() && snapshot->identity.document.sameVersion(event.after.document),
            "identity listeners see atomically installed current and history");
    }
    void documentChanged(SingleTrackDocument::StatePtr state) override
    { ++changes; require(state == document.getState(), "document listener sees the exact installed State"); }
};

struct PlaylistObserved final : PlaylistModel::Listener
{
    int changes = 0, selections = 0;
    void playlistChanged() override { ++changes; }
    void currentPlaylistIndexChanged(int) override { ++selections; }
};

void recoveredPlaylist(const juce::File& root, const juce::File& source, const juce::File& other,
    RecoveryStore& store, const RecoveryRestoreCandidate::Request& request,
    RecoveryRestoreCandidate& candidate, const std::shared_ptr<const RecoveredCandidateDocument>& core)
{
    auto recovered = RecoveredPlaylistDocument::fromCandidate(*core);
    require(recovered != nullptr, "real ready candidate mints independent playlist document");
    RecoveredCandidateDocument invalid;
    require(!RecoveredPlaylistDocument::fromCandidate(invalid), "unready candidate cannot create a playable playlist entry");
    PlaylistModel empty; PlaylistObserved emptyEvents; empty.addListener(&emptyEvents);
    require(empty.addRecoveredDocument(recovered) == 0 && empty.getCurrentIndex() == -1
        && emptyEvents.selections == 0 && emptyEvents.changes == 1
        && WorkspaceSessionSupport::serialisePlaylistFiles(empty).isEmpty()
        && WorkspaceSessionSupport::serialisedPlaylistIndex(empty) == -1,
        "recovered insertion into empty playlist neither selects nor persists an ordinary source path");
    empty.removeListener(&emptyEvents); empty.clear();

    SingleTrackDocument host; require(host.loadFile(source), "load actual ordinary host before recovery attachment");
    AudioEngine engine(host);
    require(!engine.getRuntimeMetricsForDiagnostics() && !engine.isPlaying(), "passive engine diagnostic confirms no device runtime or playback");
    juce::AudioBuffer<float> beforeAttach(2, 128), afterAttach(2, 128);
    require(engine.renderDocumentBlockForDiagnostics(0, beforeAttach), "render real ordinary engine source without opening a device");
    const auto ordinaryState = host.getState(); const auto ordinaryIdentity = host.getMediaIdentity().document;
    BnkDocument bank; PlaylistModel playlist; WaveformModel waveform(host); juce::ApplicationProperties properties;
    WorkspaceSessionSupport workspace(properties, host, bank, playlist, waveform);
    playlist.addFiles({ source, other });
    require(workspace.collectUnsavedDocuments().empty(), "clean ordinary workspace has no close-time dirty documents");
    PlaylistObserved events; playlist.addListener(&events);
    const auto firstIndex = playlist.addRecoveredDocument(recovered);
    const auto firstId = recovered->attemptId();
    require(firstIndex == 2 && playlist.getCurrentIndex() == 0 && events.selections == 0
        && host.getState() == ordinaryState && host.getMediaIdentity().document.sameVersion(ordinaryIdentity)
        && !engine.isPlaying() && engine.getLoadedFileInfo().audioRevision == ordinaryState->audioRevision
        && playlist.getEntry(firstIndex)->file == source && playlist.getEntry(firstIndex)->displayName().startsWith("Recovered "),
        "same-path recovered row attaches independently without replacing current audio/state/identity");
    require(engine.renderDocumentBlockForDiagnostics(0, afterAttach), "render existing audio after independent candidate attachment");
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i)
        require(beforeAttach.getSample(c, i) == afterAttach.getSample(c, i), "attachment preserves actual current engine PCM");
    require(playlist.addRecoveredDocument(recovered) == firstIndex && events.changes == 1,
        "same attempt attachment is idempotent without a second selection/change event");
    playlist.addFiles({source});
    require(playlist.getNumEntries() == 3, "ordinary file dedup ignores independent recovered row");
    const auto inactive = workspace.collectUnsavedDocuments();
    require(inactive.size() == 1 && !inactive[0].current && inactive[0].recoveredEntry == firstId
        && inactive[0].image && inactive[0].image->hasUnsavedChanges(),
        "a clean current Document cannot hide an inactive dirty recovery image from close");

    RecoveryRestoreCandidate second(store, request);
    for (int i = 0; i < 256 && second.getStage() != RecoveryAttemptStage::coreReady; ++i)
        good(second.advance(), "prepare second genuine recovery attempt at same source");
    std::shared_ptr<const RecoveredCandidateDocument> secondCore;
    good(second.acquireCore(second.getAttemptId(), secondCore), "acquire independent second candidate");
    auto secondEntry = RecoveredPlaylistDocument::fromCandidate(*secondCore);
    const auto secondId = secondEntry->attemptId();
    require(playlist.addRecoveredDocument(secondEntry) == 3 && firstId != secondId && playlist.getNumEntries() == 4,
        "different genuine attempts at identical path remain distinct documents");
    require(WorkspaceSessionSupport::deserialisePlaylistFiles(WorkspaceSessionSupport::serialisePlaylistFiles(playlist)).size() == 2,
        "workspace file persistence excludes every unsaved recovered attempt");

    const auto independent = workspace.collectUnsavedDocuments();
    require(independent.size() == 2 && !independent[0].sameRevision(independent[1]),
        "same-path independent recovered rows need independent close decisions");
    juce::String error;
    playlist.setCurrentIndex(firstIndex);
    require(workspace.restoreRecoveredDocument(firstId, error) && workspace.isActiveRecoveredDocument(firstId)
        && host.hasUnsavedChanges() && host.requiresRecoverySaveAs() && host.canUndo() && host.canRedo(),
        "explicit activation adopts actual recovered dirty current/Undo/Redo and Save As restriction");
    require(!engine.isPlaying() && engine.getPlayableSampleRange() == host.getPlayableSampleRange()
        && !engine.getRuntimeMetricsForDiagnostics(), "real engine remains stopped and exposes only the genuine ready prefix");
    const auto activatedPreview = waveform.getPeakCache();
    require(waveform.isPeakCacheReady() && activatedPreview && activatedPreview->isPreview
        && activatedPreview->loadedSampleRange.getEnd() < activatedPreview->totalSamples,
        "first-waveform readiness is already true for the real incomplete recovered preview");
    require(WorkspaceSessionSupport::serialisedPlaylistIndex(playlist) == -1, "recovered selection cannot select a different persisted file on restart");
    const auto activated = workspace.collectUnsavedDocuments();
    require(activated.size() == 2 && activated[0].current && activated[0].recoveredEntry == firstId
        && activated[0].sameRevision(workspace.collectUnsavedDocuments()[0]) && !activated[0].sameRevision(inactive[0]),
        "live recovered state appears once; unchanged capture is stable and fresh activation invalidates old decisions");
    require(host.moveEnvelopePoint(0, 0, 0.23f), "edit recovered document before late full peaks complete");
    const auto editedState = host.getState();
    for (int i = 0; i < 256 && candidate.getStage() != RecoveryAttemptStage::ready; ++i)
        good(candidate.advance(), "finish genuine first materialization after a live edit");
    auto firstFull = std::const_pointer_cast<WaveformModel::PeakCache>(candidate.getFullPeaks());
    require(firstFull && !workspace.acceptRecoveredPeaks(firstId, core->coreState, firstFull)
        && host.getState() == editedState && waveform.getPeakCache() != firstFull,
        "late unedited full peaks cannot overwrite a newer active edit even at the same file path");
    const auto peakDeadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    // isPeakCacheReady intentionally includes the incoming Core preview and
    // envelope edits retain its audioRevision. Wait for this test's actual
    // full-waveform contract, without changing the production CoreReady gate.
    auto completedEditedPeaks = [&]()
    {
        const auto peaks = waveform.getPeakCache();
        return waveform.isPeakCacheReady() && peaks && peaks->state == editedState
            && peaks != firstFull && !peaks->isPreview && peaks->binCount > 0
            && peaks->loadedSampleRange == juce::Range<int64_t>(0, editedState->info.numSamples);
    };
    while (!completedEditedPeaks() && juce::Time::getMillisecondCounterHiRes() < peakDeadline) juce::Thread::sleep(2);
    if (!completedEditedPeaks())
    {
        const auto observed = waveform.getPeakCache();
        std::cerr << "Workspace peak fence: ready=" << waveform.isPeakCacheReady() << " cache=" << (observed != nullptr)
            << " currentState=" << (observed && observed->state == editedState) << " rejectedFull=" << (observed == firstFull)
            << " deadlineRemainingMs=" << peakDeadline - juce::Time::getMillisecondCounterHiRes() << '\n';
    }
    require(completedEditedPeaks(), "the current edit's own progressive waveform completes independently after old candidate results are rejected");
    const auto beforeBranch = workspace.collectUnsavedDocuments()[0];
    host.beginEnvelopeGesture();
    const auto duringGesture = workspace.collectUnsavedDocuments();
    require(duringGesture.size() == 2 && duringGesture[0].current && !duringGesture[0].image,
        "a dirty gesture remains in the close inventory even when an immutable image cannot yet be captured");
    host.endEnvelopeGesture();
    require(host.moveEnvelopePoint(0, 0, 0.41f), "create a new real history branch before close");
    const auto laterGeneration = host.getState()->changeGeneration;
    require(host.undo() && host.getState() == editedState && host.getState()->changeGeneration < laterGeneration,
        "actual Undo returns to a lower generation with the identical current State");
    const auto afterBranch = workspace.collectUnsavedDocuments()[0];
    require(afterBranch.image->hasUnsavedChanges() && !afterBranch.sameRevision(beforeBranch),
        "a changed Redo branch invalidates Discard even when the exact current State pointer is unchanged");
    workspace.retainCurrentAudioDocumentIfNeeded();
    require(workspace.collectUnsavedDocuments()[0].sameRevision(afterBranch),
        "retention alone neither edits the image nor invalidates a current close decision");
    require(workspace.reuseAudioDocumentIfAvailable(source) == WorkspaceSessionSupport::AudioDocumentReuseResult::restoredRetained
        && !host.hasUnsavedChanges() && host.getState()->streamingReader == ordinaryState->streamingReader,
        "switching back to same-path ordinary row restores the original document instead of reusing recovery");
    playlist.setCurrentIndex(0);
    require(workspace.restoreRecoveredDocument(firstId, error) && host.hasUnsavedChanges()
        && host.getState()->envelopePoints[0].gain == 0.23f && host.canUndo(),
        "switch back retains actual edited recovered image and complete history");
    playlist.setCurrentIndex(playlist.indexOfEntry(firstId));
    require(host.undo() && host.canRedo(), "recovered Undo remains executable after switching");
    const auto undone = host.getState(); workspace.retainCurrentAudioDocumentIfNeeded();
    require(!workspace.acceptRecoveredPeaks(firstId, core->coreState, firstFull), "an edited retained image rejects old core results across activations");
    const auto activeIdentity = host.getMediaIdentity().document;
    playlist.moveEntry(playlist.indexOfEntry(firstId), 0);
    require(playlist.getCurrentEntry()->id == firstId && workspace.isActiveRecoveredDocument(firstId)
        && host.getMediaIdentity().document.sameVersion(activeIdentity), "row movement preserves stable active document identity");
    playlist.setCurrentIndex(playlist.indexOfEntry(secondId));
    require(WorkspaceSessionSupport::serialisedPlaylistIndex(playlist) == -1, "mapped persistence keeps a recovered selection unselected");

    for (int i = 0; i < 256 && second.getStage() != RecoveryAttemptStage::ready; ++i)
        good(second.advance(), "complete inactive second candidate");
    auto secondFull = std::const_pointer_cast<WaveformModel::PeakCache>(second.getFullPeaks());
    const auto beforeOtherResult = waveform.getPeakCache();
    require(secondFull && !workspace.acceptRecoveredPeaks(firstId, secondCore->coreState, secondFull)
        && workspace.acceptRecoveredPeaks(secondId, secondCore->coreState, secondFull)
        && host.getState() == undone && waveform.getPeakCache() == beforeOtherResult,
        "inactive result is stored only for exact attempt and never replaces the active waveform");
    require(workspace.restoreRecoveredDocument(secondId, error) && waveform.getPeakCache() == secondFull
        && waveform.getPeakCache()->state == host.getState() && host.canUndo() && host.canRedo(),
        "explicit later activation consumes correct full peaks with complete recovered history");
    const auto savedTarget = root.getChildFile("recovered-playlist-saved.wav");
    const auto save = host.createSaveSnapshot(savedTarget, SingleTrackDocument::SaveIntent::saveAs);
    const auto written = SingleTrackDocument::writeSaveSnapshot(save);
    require(written.succeeded && host.markSaveSnapshotCommitted(save, written), "actual recovered Save As publishes before playlist target update");
    const auto afterSave = workspace.collectUnsavedDocuments();
    require(afterSave.size() == 1 && afterSave[0].recoveredEntry == firstId && !afterSave[0].current,
        "real clean Save immediately overrides the active row's stale dirty snapshot while preserving other dirty rows");
    playlist.addFiles({ savedTarget });
    const auto countBeforeSaveUpdate = playlist.getNumEntries();
    require(playlist.replaceCurrentFile(savedTarget) && playlist.getNumEntries() == countBeforeSaveUpdate
        && playlist.getCurrentEntry()->id == secondId && playlist.getCurrentEntry()->recovered,
        "Save As retains independent image/history and does not merge another row at its new target");
    workspace.retainCurrentAudioDocumentIfNeeded();
    require(host.loadFile(other), "leave actually saved recovered document");
    workspace.releaseRecoveredActivation();
    require(workspace.restoreRecoveredDocument(secondId, error) && !host.hasUnsavedChanges()
        && !host.requiresRecoverySaveAs() && host.getFileInfo().file == savedTarget && host.canUndo() && host.canRedo(),
        "saved recovered item restores its real clean point/target and Undo/Redo instead of reopening a path");
    require(playlist.getCurrentEntry()->activationFile() == host.getState()->info.file
        && playlist.getCurrentEntry()->file == host.getFileInfo().file,
        "native activation fences the actual rendered State locator separately from a later Save As display target");
    const auto savedImageState = host.getState();
    require(host.captureWorkspaceSnapshot()->hasUnsavedChanges() == host.hasUnsavedChanges()
        && host.undo() && host.hasUnsavedChanges() && workspace.collectUnsavedDocuments().size() == 2,
        "Undo away from the real clean point restores the active row to the close inventory");
    require(host.redo() && !host.hasUnsavedChanges() && host.getState() == savedImageState
        && workspace.collectUnsavedDocuments().size() == 1,
        "Redo to the exact clean point removes only that active row from the close inventory");
    playlist.removeAt(playlist.indexOfEntry(firstId));
    require(host.getState() == savedImageState && workspace.isActiveRecoveredDocument(secondId)
        && !workspace.acceptRecoveredPeaks(firstId, core->coreState, firstFull),
        "removal of another entry preserves current audio and rejects its delayed results");
    require(workspace.collectUnsavedDocuments().empty(), "removed rows and clean saved history leave no phantom close documents");
    playlist.removeListener(&events);
    workspace.releaseRecoveredActivation(); host.clear(); playlist.clear();
    std::cout << "RecoveredPlaylistTests passed (real independent candidates, no auto-selection, path isolation, dirty history, late peaks, saved history)\n";
}
}

void runWorkspaceSnapshotTests(const juce::File& root)
{
    const auto source = root.getChildFile("workspace-snapshot-source.wav"); makeSource(source);
    const auto other = root.getChildFile("workspace-snapshot-other.wav"); makeSource(other);
    SingleTrackDocument document;
    require(!document.captureWorkspaceSnapshot() && document.loadFile(source), "empty document cannot mint workspace image");
    require(document.moveEnvelopePoint(0, 0, 0.7f) && document.moveEnvelopePoint(0, 0, 0.4f) && document.undo(),
        "create actual nonempty current Undo and Redo");
    const auto target = root.getChildFile("workspace-snapshot-saved.wav");
    auto request = document.createSaveSnapshot(target, SingleTrackDocument::SaveIntent::saveAs);
    const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
    require(saved.succeeded && document.markSaveSnapshotCommitted(request, saved), "real Save establishes clean point inside retained history");
    const auto oldIdentity = document.getMediaIdentity();
    const auto original = document.captureCommittedState(oldIdentity.document);
    auto image = document.captureWorkspaceSnapshot();
    require(image && original && !original->undo.empty() && !original->redo.empty(), "opaque image contains actual clean state with both history directions");
    document.beginEnvelopeGesture();
    require(!document.captureWorkspaceSnapshot(), "unfinished gesture cannot publish an incomplete workspace history");
    document.endEnvelopeGesture();
    require(document.loadFile(other), "switch actual host to unrelated ordinary file");
    const auto unrelated = document.getMediaIdentity();
    Observed observer(document);
    juce::String error;
    require(document.adoptWorkspaceSnapshot(*image, error) && error.isEmpty(), "adopt full workspace image into existing native Document reference");
    require(observer.identities == 1 && observer.changes == 1 && !document.hasUnsavedChanges() && document.canUndo() && document.canRedo()
        && document.getFileInfo().file == target && !document.requiresRecoverySaveAs()
        && !document.getMediaIdentity().document.sameDocument(oldIdentity.document)
        && !document.getMediaIdentity().document.sameDocument(unrelated.document), "single atomic adoption preserves clean/target/history but issues fresh live identity");
    equivalent(*document.getState(), *original->state);
    require(!document.markSaveSnapshotCommitted(request, saved), "genuine preactivation Save receipt cannot cross new document identity");
    require(document.redo() && document.hasUnsavedChanges(), "restored Redo changes actual dirty state");
    equivalent(*document.getState(), *original->redo.back());
    require(document.undo() && !document.hasUnsavedChanges(), "Undo returns to exact retained clean point without numeric ordering");
    require(document.undo() && document.hasUnsavedChanges() && document.canRedo(), "earlier retained Undo remains editable and dirty");
    equivalent(*document.getState(), *original->undo.back());
    require(document.adoptWorkspaceSnapshot(*image, error), "repeated activation retains original immutable image");

    // Exercise the real native retention class with an isolated in-memory app
    // settings object; this test never opens/writes the user's settings file.
    juce::ApplicationProperties properties;
    BnkDocument bank; PlaylistModel playlist;
    WaveformModel waveform(document);
    WorkspaceSessionSupport workspace(properties, document, bank, playlist, waveform);
    playlist.addFiles({ target, other });
    workspace.retainCurrentAudioDocumentIfNeeded();
    require(workspace.audioActivationFile(target) == document.getState()->info.file,
        "ordinary retained Save As uses actual rendered State locator for activation milestones");
    require(document.loadFile(other) && workspace.restoreRetainedAudioDocumentIfAvailable(target),
        "actual WorkspaceSession retention uses full image instead of loadFile/adoptLoadedState");
    require(document.canUndo() && document.canRedo() && !document.hasUnsavedChanges(), "native retention preserves real Undo/Redo and clean point");
    equivalent(*document.getState(), *original->state);
    const auto resumedRequest = document.createSaveSnapshot();
    const auto resumedSave = SingleTrackDocument::writeSaveSnapshot(resumedRequest);
    require(resumedSave.succeeded && document.markSaveSnapshotCommitted(resumedRequest, resumedSave)
        && document.canUndo() && document.canRedo(), "restored native document retains real guarded checkpoint authority and full history");
    auto rendered = SingleTrackDocument::createReaderForFile(target);
    require(rendered && rendered->lengthInSamples == original->state->info.numSamples && rendered->numChannels == 2,
        "resumed checkpoint produces actual readable audio");
    rendered.reset();
    workspace.clearRetainedPlaylistAudio();
    juce::MemoryBlock savedBytes, afterRejectedSave;
    require(target.loadFileAsData(savedBytes) && document.adoptWorkspaceSnapshot(*image, error),
        "activate older genuine image after the actual target has a newer Save publication");
    const auto staleState = document.getState();
    const auto staleSave = SingleTrackDocument::writeSaveSnapshot(document.createSaveSnapshot());
    require(!staleSave.succeeded && target.loadFileAsData(afterRejectedSave) && afterRejectedSave == savedBytes
        && document.getState() == staleState && document.canUndo() && document.canRedo(),
        "fresh workspace identity cannot turn an obsolete physical source witness into overwrite permission");

    // A real progressively restored candidate can be attached without reopening
    // its historical path. Full materialization keeps using the shared backing.
    SingleTrackDocument edited; require(edited.loadFile(source), "load genuine recovery producer fixture");
    RecoveryMutationProducer::Config config; config.root = root.getChildFile("workspace-recovery-store");
    config.graph.chunkSampleBytes = 4096; config.queue.maximumAge = 10s;
    std::unique_ptr<RecoveryMutationProducer> producer;
    good(RecoveryMutationProducer::attachPostCore(edited, config, producer), "attach actual candidate mutation capture");
    require(edited.moveEnvelopePoint(0, 0, 0.8f) && edited.moveEnvelopePoint(0, 0, 0.5f) && edited.undo() && producer->drain(10s),
        "capture real recovery current/Undo/Redo");
    const auto health = producer->health(); producer->close(); producer.reset();
    RecoveryStore store(config.root, health.owner);
    RecoveryRestoreCandidate::Request recovery;
    recovery.observedOwnerHead = health.writer.safeHead; recovery.checkpointHead = health.writer.safeHead;
    recovery.scratchDirectory = root.getChildFile("workspace-recovery-scratch");
    recovery.attemptDirectory = root.getChildFile("workspace-recovery-attempts"); recovery.appBuild = "workspace-snapshot-test-v1";
    recovery.previewSeconds = 1;
    require(recovery.scratchDirectory.createDirectory().wasOk(), "create only owned candidate scratch");
    auto candidate = std::make_unique<RecoveryRestoreCandidate>(store, recovery);
    for (int step = 0; step < 256 && candidate->getStage() != RecoveryAttemptStage::coreReady; ++step)
        good(candidate->advance(), "prepare real progressive candidate CoreReady");
    std::shared_ptr<const RecoveredCandidateDocument> core;
    good(candidate->acquireCore(candidate->getAttemptId(), core), "acquire real candidate with actual peaks/playable prefix");
    require(core && core->document && core->firstPeaks && core->document->canUndo() && core->document->canRedo(),
        "candidate fixture contains genuine full history before workspace adoption");
    require(core->coreState->getPlayableSampleRange().getEnd() > 0
        && core->coreState->getPlayableSampleRange().getEnd() < core->coreState->info.numSamples,
        "fixture exercises genuinely partial CoreReady rather than completed materialization");
    recoveredPlaylist(root, source, other, store, recovery, *candidate, core);
    auto recoveredImage = core->document->captureWorkspaceSnapshot();
    const auto recoveryOrigin = core->document->getRecoveryOrigin();
    require(recoveredImage && document.adoptWorkspaceSnapshot(*recoveredImage, error), "native host accepts recovery-backed full image without ordinary reader path");
    require(document.hasUnsavedChanges() && document.canUndo() && document.canRedo() && document.requiresRecoverySaveAs()
        && document.getRecoveryOrigin()->sameVersion(*recoveryOrigin)
        && document.getState()->getPlayableSampleRange() == core->coreState->getPlayableSampleRange(),
        "adoption retains detached Save As restriction, provenance and truthful ready coverage");
    equivalent(*document.getState(), *core->coreState);
    const auto nativeState = document.getState();
    for (int step = 0; step < 256 && candidate->getStage() != RecoveryAttemptStage::ready; ++step)
        good(candidate->advance(), "complete candidate backing after native image adoption");
    require(candidate->getStage() == RecoveryAttemptStage::ready && document.getState() == nativeState
        && document.getPlayableSampleRange().getEnd() == document.getNumSamples(), "background completion grows shared coverage without replacing native edits/state");
    core.reset(); candidate.reset(); recoveredImage.reset();
    OwnerRetirement terminal;
    require(RecoveryOwnerLifecycle::discard(store, health.writer.safeHead, terminal).code == "owner_in_use",
        "native current/history still hold real owner lease after candidate controller release");
    require(document.redo() && document.undo(), "actual adopted recovery Redo/Undo remains usable after controller destruction");
    require(!document.createSaveSnapshot({}, SingleTrackDocument::SaveIntent::checkpoint).isValid(), "detached recovered history cannot acquire overwrite rights by workspace activation");
    std::cout << "WorkspaceSnapshotTests passed (actual Save/clean history, identity fences, native retention, progressive recovery, owner leases, full Undo/Redo)\n";
}
