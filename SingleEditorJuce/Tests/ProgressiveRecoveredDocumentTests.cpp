#include "Recovery/RecoveryCheckpointCodec.h"
#include "Recovery/ProgressiveRecoveryAudio.h"
#include "Audio/AudioEngine.h"
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void good(Status status, const char* message)
{
    if (!status) std::cerr << "Progressive Document: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
void checkPlayback(SingleTrackDocument& document, const SingleTrackDocument::State& expected)
{
    AudioEngine engine(document);
    juce::AudioBuffer<float> rendered(expected.info.numChannels, 64);
    require(engine.renderDocumentBlockForDiagnostics(32, rendered), "actual recovered Document renders through production playback source");
    for (int channel = 0; channel < rendered.getNumChannels(); ++channel)
        for (int frame = 0; frame < rendered.getNumSamples(); ++frame)
        {
            const auto at = frame + 32;
            const auto gain = SingleTrackDocument::evaluateEnvelope(expected.envelopePoints, at / expected.info.sampleRate);
            require(std::abs(rendered.getSample(channel, frame) - expected.audioBuffer->getSample(channel, at) * gain) < 1.0e-6f,
                "progressive recovered history renders correct audio and envelope");
        }
    require(!engine.getRuntimeMetricsForDiagnostics().has_value(), "recovered Document test does not open any audio device");
}
}

void runProgressiveRecoveredDocumentTests(const PreparedRecoveryCheckpoint& prepared, const Snapshot& expected,
    const RecoveryStore& store, const juce::File& root, AudioGraphLimits limits)
{
    const auto scratch = root.getChildFile("progressive-document-scratch");
    const auto historical = root.getChildFile("missing-progressive-source.wav");
    const auto target = root.getChildFile("progressive-document-save.wav");
    require(scratch.createDirectory().wasOk() && !historical.exists() && !target.exists(), "prepare isolated missing-source recovery fixture");
    {
        const auto& graph = prepared.getAudioGraph();
        std::vector<std::unique_ptr<ProgressiveRecoveryAudio>> sources(graph.sequences.size());
        Snapshot backing;
        backing.identity = prepared.getIdentity(); backing.identity.source.location.file = historical;
        for (size_t index = 0; index < prepared.getStates().size(); ++index)
        {
            const auto sequence = graph.states[index];
            if (!sources[sequence]) good(ProgressiveRecoveryAudio::create(prepared.getAudioJson(), index, scratch, sources[sequence], limits),
                "allocate each unique isolated recovery sequence once");
            auto metadata = *prepared.getStates()[index]; metadata.info.file = historical;
            SingleTrackDocument::StatePtr state;
            good(sources[sequence]->bindMetadata(metadata, state), "bind verified reader to prepared immutable metadata");
            if (index == 0) backing.state = state;
            else if (index <= prepared.getUndoCount()) backing.undo.push_back(state);
            else backing.redo.push_back(state);
        }
        juce::String error;
        require(!SingleTrackDocument::createRecoveredCopy(backing, error) && error == "recovery_current_audio_not_ready",
            "zero-ready current stream cannot become a usable recovered Document");
        good(sources[graph.states[0]]->loadNext(store), "prepare only the current sequence's first real audio chunk");
        auto recovered = SingleTrackDocument::createRecoveredCopy(backing, error);
        if (!recovered) std::cerr << "Progressive adoption: " << error << '\n';
        require(recovered && recovered->hasAudio() && recovered->hasUnsavedChanges() && recovered->requiresRecoverySaveAs()
            && !recovered->getState()->audioBuffer && !recovered->getState()->playbackAvailability.isRandomAccess(),
            "adopt genuine partial recovery audio as an isolated dirty Document");
        const auto identity = recovered->getMediaIdentity().document;
        require(!identity.sameDocument(prepared.getIdentity().document) && recovered->getRecoveryOrigin()->sameVersion(prepared.getIdentity().document),
            "progressive Document receives independent identity and retains captured origin");
        auto history = recovered->captureCommittedState(identity);
        require(history && history->undo.size() == expected.undo.size() && history->redo.size() == expected.redo.size(),
            "deferred history remains intact during first-prefix adoption");
        checkPlayback(*recovered, *expected.state);

        SingleTrackDocument ordinary;
        auto illicit = std::make_shared<SingleTrackDocument::State>(*backing.state);
        illicit->info.recoveryBacked = false;
        require(!ordinary.adoptLoadedState(illicit), "ordinary open cannot bypass recovery admission by clearing a metadata flag");
        auto invalid = backing;
        auto wrongReader = std::make_shared<SingleTrackDocument::State>(*backing.state);
        wrongReader->info.numSamples += 1; invalid.state = wrongReader;
        require(!SingleTrackDocument::createRecoveredCopy(invalid, error), "reader/metadata dimensions are checked at actual adoption");
        wrongReader = std::make_shared<SingleTrackDocument::State>(*backing.state);
        wrongReader->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::streamingReader; invalid.state = wrongReader;
        require(!SingleTrackDocument::createRecoveredCopy(invalid, error), "generic live streaming readers are not recovery backings");

        const auto untouched = recovered->getState();
        require(!recovered->createSaveSnapshot(target, SingleTrackDocument::SaveIntent::saveAs).isValid()
            && !recovered->saveAsToFile(target)
            && !SingleTrackDocument::writeSaveSnapshot(recovered->createSaveSnapshot(target, SingleTrackDocument::SaveIntent::exportCopy)).succeeded
            && !target.exists() && !historical.exists(),
            "partial recovery cannot create or replace an incomplete whole-file save/export");
        SingleTrackDocument::SaveSnapshot forced;
        forced.state = untouched; forced.targetFile = target; forced.intent = SingleTrackDocument::SaveIntent::exportCopy;
        forced.documentIdentity = identity; forced.changeGeneration = untouched->changeGeneration;
        require(!forced.isValid() && !SingleTrackDocument::writeSaveSnapshot(forced).succeeded && !target.exists(),
            "asynchronous/caller-assembled save snapshot cannot bypass the prepared-range gate");
        require(!recovered->deleteSelection({ 0.0, 0.001 }) && !recovered->applySelectionGain({ 0.0, 0.001 }, 0.5f)
            && !recovered->beginEqualLengthAudioMutation(untouched) && recovered->getState() == untouched,
            "operations currently requiring the whole backing fail before mutation while coverage is incomplete");
        {
            auto envelopeDocument = SingleTrackDocument::createRecoveredCopy(backing, error);
            require(envelopeDocument && envelopeDocument->moveEnvelopePoint(0, 0.0, 0.5f)
                && envelopeDocument->hasUnsavedChanges() && !envelopeDocument->getState()->audioBuffer
                && envelopeDocument->undo(), "non-destructive envelope editing/Undo works on prepared prefix without full materialisation");
            checkPlayback(*envelopeDocument, *expected.state);
        }

        require(!recovered->canRedo() && !recovered->redo() && recovered->getState() == untouched,
            "unprepared Redo is deferred without popping the history entry");
        require(recovered->canUndo() && recovered->undo(), "shared prepared backing allows actual envelope Undo immediately");
        checkPlayback(*recovered, *expected.undo.back());
        const auto beforeDeferred = recovered->getState();
        require(!recovered->canUndo() && !recovered->undo() && recovered->getState() == beforeDeferred,
            "unprepared older Undo is retained and cannot replace the playable state");
        good(sources[graph.states[1]]->loadNext(store), "prepare the older history's first chunk independently");
        require(recovered->canUndo() && recovered->undo() && recovered->hasUnsavedChanges(),
            "existing Undo becomes usable when its own coverage arrives; old clean state remains dirty");
        checkPlayback(*recovered, *expected.undo.front());
        require(recovered->redo() && recovered->redo(), "actual Redo returns through retained prepared history");
        checkPlayback(*recovered, *expected.state);
        good(sources[graph.states.back()]->loadNext(store), "prepare retained deletion-Redo backing");
        require(recovered->canRedo() && recovered->redo()
            && recovered->getState()->info.numSamples == expected.redo.back()->info.numSamples,
            "previously blocked Redo preserves the actual edited frame count");
        checkPlayback(*recovered, *expected.redo.back());
        require(recovered->undo(), "return to original recovered checkpoint before full completion");

        for (auto& source : sources) while (!source->isComplete()) good(source->loadNext(store), "finish background sample materialisation");
        require(recovered->getState()->playbackAvailability.isRandomAccess() && recovered->hasUnsavedChanges(),
            "full coverage arrives without changing dirty state or replacing the current Document");
        require(recovered->saveAsToFile(target) && target.existsAsFile() && !historical.exists() && !recovered->requiresRecoverySaveAs()
            && !recovered->hasUnsavedChanges(), "explicit Save As succeeds only after full required coverage and rebinds the new source");
        auto savedReader = SingleTrackDocument::createReaderForFile(target);
        juce::AudioBuffer<float> saved(expected.state->info.numChannels, 64);
        require(savedReader && savedReader->read(&saved, 0, 64, expected.state->info.numSamples - 64, true, true), "read actual saved recovery tail");
        for (int channel = 0; channel < saved.getNumChannels(); ++channel)
            for (int frame = 0; frame < 64; ++frame)
            {
                const auto at = expected.state->info.numSamples - 64 + frame;
                const auto value = expected.state->audioBuffer->getSample(channel, static_cast<int>(at))
                    * SingleTrackDocument::evaluateEnvelope(expected.state->envelopePoints, static_cast<double>(at) / expected.state->info.sampleRate);
                require(std::abs(saved.getSample(channel, frame) - value) < 2.0e-6f, "late materialised samples survive real Save As with correct envelope");
            }
        require(recovered->applySelectionGain({ 0.0, 0.001 }, 0.5f) && recovered->getState()->audioBuffer
            && recovered->undo() && !recovered->hasUnsavedChanges(), "completed recovery can materialise for ordinary audio edits and preserve the clean Undo boundary");
        recovered->clear();
        require(!recovered->hasAudio() && !recovered->getRecoveryOrigin(), "clear retires progressive recovery identity and active audio");
    }
    require(scratch.getNumberOfChildFiles(juce::File::findFiles) == 0 && !historical.exists(), "all candidate/history readers release only their private scratch files");
    std::cout << "ProgressiveRecoveredDocumentTests passed\n";
}
