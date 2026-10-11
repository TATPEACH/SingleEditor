#include "Document/SingleTrackDocument.h"
#include "Audio/AudioEngine.h"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
using namespace singleeditor;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void sameAudio(const SingleTrackDocument::StatePtr& actual, const SingleTrackDocument::StatePtr& expected)
{
    require(actual && actual->audioBuffer && actual->info.numSamples == expected->info.numSamples
                && actual->info.numChannels == expected->info.numChannels, "recovered live document retains source dimensions");
    for (int channel = 0; channel < actual->info.numChannels; ++channel)
        for (int frame = 0; frame < actual->info.numSamples; ++frame)
            require(std::bit_cast<uint32_t>(actual->audioBuffer->getSample(channel, frame))
                        == std::bit_cast<uint32_t>(expected->audioBuffer->getSample(channel, frame)), "live Undo/Redo retains exact recovered samples");
    require(actual->info.channelLayout.selectedRoles == expected->info.channelLayout.selectedRoles
        && actual->info.metadata.getAllKeys() == expected->info.metadata.getAllKeys()
        && actual->info.metadata.getAllValues() == expected->info.metadata.getAllValues()
        && actual->envelopePoints.size() == expected->envelopePoints.size(), "live Undo/Redo preserves metadata/layout/envelope");
    for (size_t index = 0; index < actual->envelopePoints.size(); ++index)
        require(actual->envelopePoints[index].timeSeconds == expected->envelopePoints[index].timeSeconds
            && actual->envelopePoints[index].gain == expected->envelopePoints[index].gain, "live history preserves exact envelope values");
}
} // namespace

void runRecoveredDocumentTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot& restored, const juce::File& root)
{
    const auto source = restored.identity.source.location.file;
    require(source.getParentDirectory() == root && source.existsAsFile(), "recovery adoption fixture is scoped to owned source");
    const auto originalBytes = source.loadFileAsString();
    const auto beforeFiles = root.getNumberOfChildFiles(juce::File::findFilesAndDirectories);
    juce::String error;
    auto recovered = SingleTrackDocument::createRecoveredCopy(restored, error);
    if (!recovered) std::cerr << "Recovery adoption: " << error << '\n';
    require(recovered && error.isEmpty() && recovered->hasAudio() && recovered->hasUnsavedChanges()
        && recovered->requiresRecoverySaveAs(), "create a usable isolated dirty recovery document");
    require(root.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == beforeFiles && source.loadFileAsString() == originalBytes,
        "adoption creates no media file and cannot overwrite a changed source");
    const auto identity = recovered->getMediaIdentity();
    require(identity.document.isValid() && !identity.document.sameDocument(restored.identity.document)
        && identity.document.lineage != restored.identity.document.lineage
        && !identity.source.sameBinding(restored.identity.source) && !identity.source.fastStampValid
        && identity.savedGeneration == 0 && recovered->getRecoveryOrigin().has_value()
        && recovered->getRecoveryOrigin()->sameVersion(restored.identity.document), "new owner has separate identity and retained origin provenance");
    require(!recovered->canSaveOverSourceFile() && !recovered->createSaveSnapshot().isValid()
        && !recovered->createSaveSnapshot(source).isValid() && !recovered->saveOverSourceFile()
        && source.loadFileAsString() == originalBytes, "default/checkpoint save cannot target historical source");
    require(source.deleteFile() && !source.existsAsFile(), "remove only original private fixture to exercise Missing source");
    require(recovered->hasAudio() && recovered->getFileInfo().isValid(), "recovery document remains usable when historical locator is missing");
    sameAudio(recovered->getState(), restored.state);
    const auto checkPlayback = [&]
    {
        AudioEngine engine(*recovered);
        juce::AudioBuffer<float> rendered(restored.state->info.numChannels, 128);
        require(!engine.getRuntimeMetricsForDiagnostics().has_value(), "recovery test does not open an audio device");
        require(engine.renderDocumentBlockForDiagnostics(32, rendered), "actual playback source renders a recovered document without original file");
        const auto state = recovered->getState();
        for (int channel = 0; channel < rendered.getNumChannels(); ++channel)
            for (int frame = 0; frame < rendered.getNumSamples(); ++frame)
            {
                const auto sourceFrame = 32 + frame;
                const auto gain = SingleTrackDocument::evaluateEnvelope(state->envelopePoints,
                    static_cast<double>(sourceFrame) / state->info.sampleRate);
                require(std::abs(rendered.getSample(channel, frame) - state->audioBuffer->getSample(channel, sourceFrame) * gain) < 1.0e-6f,
                    "actual recovery playback uses correct samples and envelope");
            }
        require(!engine.getRuntimeMetricsForDiagnostics().has_value(), "headless playback leaves device unopened");
    };
    checkPlayback();
    require(recovered->redo(), "actual live Redo is available immediately after recovery");
    sameAudio(recovered->getState(), restored.redo.back());
    require(recovered->undo(), "actual live Undo after Redo");
    sameAudio(recovered->getState(), restored.state);
    checkPlayback();
    require(recovered->undo(), "actual recovered Undo stack is usable");
    sameAudio(recovered->getState(), restored.undo.back());
    require(recovered->undo() && !recovered->canUndo(), "recover oldest Undo boundary without synthesizing extra history");
    sameAudio(recovered->getState(), restored.undo.front());
    require(recovered->hasUnsavedChanges() && recovered->requiresRecoverySaveAs(), "Undo to the original clean generation stays dirty in a recovered owner");
    require(recovered->redo() && recovered->redo(), "restore recovered checkpoint state through actual Redo calls");
    sameAudio(recovered->getState(), restored.state);

    const auto exported = root.getChildFile("recovered-export.wav");
    const auto exportSnapshot = recovered->createSaveSnapshot(exported, SingleTrackDocument::SaveIntent::exportCopy);
    require(exportSnapshot.isValid() && exportSnapshot.recoveryRequiresSaveAs, "explicit export may read verified independent audio");
    const auto exportedResult = SingleTrackDocument::writeSaveSnapshot(exportSnapshot);
    require(exportedResult.succeeded, "write actual export without historical source");
    recovered->markSaveSnapshotCommitted(exportSnapshot, exportedResult);
    require(recovered->hasUnsavedChanges() && recovered->requiresRecoverySaveAs()
        && recovered->getMediaIdentity().document.sameVersion(identity.document) && !source.existsAsFile(),
        "export does not rebind or clean the recovered document or recreate original file");
    auto forgedCheckpoint = exportSnapshot;
    forgedCheckpoint.intent = SingleTrackDocument::SaveIntent::checkpoint;
    forgedCheckpoint.targetFile = source;
    require(!SingleTrackDocument::writeSaveSnapshot(forgedCheckpoint).succeeded && !source.existsAsFile(),
        "captured detached-save requirement survives asynchronous snapshot transport");

    const auto saved = root.getChildFile("recovered-saved.wav");
    require(recovered->saveAsToFile(saved) && saved.existsAsFile() && !source.existsAsFile(), "actual Save As writes the explicit new target only");
    require(!recovered->hasUnsavedChanges() && !recovered->requiresRecoverySaveAs() && recovered->canSaveOverSourceFile()
        && recovered->getFileInfo().file == saved && recovered->getMediaIdentity().source.fastStampValid
        && !recovered->getMediaIdentity().source.sameBinding(identity.source), "successful Save As establishes a normal clean source binding");
    auto reader = SingleTrackDocument::createReaderForFile(saved);
    require(reader && reader->numChannels == static_cast<unsigned int>(restored.state->info.numChannels)
        && reader->lengthInSamples == restored.state->info.numSamples, "saved recovered media reopens with correct dimensions");
    juce::AudioBuffer<float> savedAudio(restored.state->info.numChannels, 128);
    require(reader->read(&savedAudio, 0, 128, 32, true, true), "read the saved recovered samples");
    for (int channel = 0; channel < savedAudio.getNumChannels(); ++channel)
        for (int frame = 0; frame < savedAudio.getNumSamples(); ++frame)
        {
            const auto index = 32 + frame;
            const auto expected = restored.state->audioBuffer->getSample(channel, index)
                * SingleTrackDocument::evaluateEnvelope(restored.state->envelopePoints,
                    static_cast<double>(index) / restored.state->info.sampleRate);
            require(std::abs(savedAudio.getSample(channel, frame) - expected) < 2.0e-6f,
                "Save As renders recovered audio/envelope with bounded PCM quantisation error");
        }
    reader.reset();
    require(recovered->undo() && recovered->hasUnsavedChanges() && recovered->redo() && !recovered->hasUnsavedChanges(),
        "post-Save-As clean boundary follows actual Undo/Redo identities");
    const auto beforeEdit = recovered->getMediaIdentity().document;
    require(recovered->applySelectionGain({ 0.0, 0.001 }, 0.5f) && recovered->hasUnsavedChanges()
        && !recovered->getMediaIdentity().document.sameVersion(beforeEdit), "new edits work on recovered audio with fresh revisions");
    require(recovered->saveOverSourceFile() && !recovered->hasUnsavedChanges() && !source.existsAsFile(),
        "subsequent checkpoint only writes the newly established Save-As source");
    require(restored.state->info.recoveryBacked == false && restored.state->changeGeneration == restored.identity.document.generation,
        "adoption does not mutate the decoded immutable checkpoint");
    recovered->clear();
    require(!recovered->getRecoveryOrigin().has_value() && !recovered->requiresRecoverySaveAs() && !recovered->hasAudio(),
        "clear retires all recovery-only document state");
    require(recovered->loadFile(exported) && !recovered->getRecoveryOrigin().has_value() && !recovered->requiresRecoverySaveAs(),
        "ordinary file load after recovery remains an ordinary document");
    recovered->clear();

    auto invalid = restored;
    auto bad = std::make_shared<SingleTrackDocument::State>(*restored.state);
    bad->playbackAvailability = SingleTrackDocument::PlaybackAvailability::contiguousPrefix(16);
    invalid.state = bad;
    require(!SingleTrackDocument::createRecoveredCopy(invalid, error) && error.isNotEmpty(), "full-memory adoption refuses incomplete playback coverage");
    bad->playbackAvailability = SingleTrackDocument::PlaybackAvailability::randomAccess();
    bad->info.numSamples += 1;
    require(!SingleTrackDocument::createRecoveredCopy(invalid, error), "adoption rejects inconsistent sample dimensions");
    bad = std::make_shared<SingleTrackDocument::State>(*restored.state);
    bad->changeGeneration = std::numeric_limits<int64_t>::max() - 1;
    bad->audioRevision = std::numeric_limits<int64_t>::max() - 1;
    bad->envelopeRevision = std::numeric_limits<int64_t>::max() - 1;
    invalid = restored; invalid.state = bad; invalid.identity.document.generation = bad->changeGeneration;
    auto freshCounters = SingleTrackDocument::createRecoveredCopy(invalid, error);
    require(freshCounters && freshCounters->getState()->changeGeneration < 32
        && freshCounters->applySelectionGain({ 0.0, 0.001 }, 0.5f) && freshCounters->getState()->changeGeneration < 32,
        "recovery never inherits overflowing external revision counters");
    std::cout << "RecoveredDocumentTests passed\n";
}
