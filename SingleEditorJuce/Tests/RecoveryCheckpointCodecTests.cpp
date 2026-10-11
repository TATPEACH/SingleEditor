#include "Recovery/RecoveryCheckpointCodec.h"
#include "Recovery/ProgressiveRecoveryAudio.h"
#include "Recovery/RecoveryOwnerLifecycle.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

void runRecoveredDocumentTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&, const juce::File&);
void runRecoveryRestoreCandidateTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runRecoveryRestoreQueueTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runRecoverySourceBindingCheckpointTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runProgressiveRecoveredDocumentTests(const singleeditor::recovery::PreparedRecoveryCheckpoint&,
    const singleeditor::SingleTrackDocument::CommittedStateSnapshot&, const singleeditor::recovery::RecoveryStore&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runProgressiveRecoveryWaveformTests(const singleeditor::recovery::PreparedRecoveryCheckpoint&,
    const singleeditor::SingleTrackDocument::CommittedStateSnapshot&, const singleeditor::recovery::RecoveryStore&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void compareState(const SingleTrackDocument::State& expected, const SingleTrackDocument::State& actual)
{
    const auto& a = expected.info;
    const auto& b = actual.info;
    require(a.file == b.file && a.formatName == b.formatName && a.sampleRate == b.sampleRate
        && a.numChannels == b.numChannels && a.bitsPerSample == b.bitsPerSample && a.numSamples == b.numSamples
        && a.durationSeconds == b.durationSeconds && a.metadata.getAllKeys() == b.metadata.getAllKeys()
        && a.metadata.getAllValues() == b.metadata.getAllValues(), "checkpoint preserves every file/audio/metadata fact");
    const auto& x = a.channelLayout; const auto& y = b.channelLayout;
    require(x.sourceChannelCount == y.sourceChannelCount && x.selectedPresetId == y.selectedPresetId
        && x.displayName == y.displayName && x.orderText == y.orderText && x.selectedRoles == y.selectedRoles
        && x.detectedFromMetadata == y.detectedFromMetadata && x.isDiscreteFallback == y.isDiscreteFallback
        && x.isAmbisonic == y.isAmbisonic && x.ambisonicOrder == y.ambisonicOrder
        && x.ambisonicNormalization == y.ambisonicNormalization && x.interpretationOrigin == y.interpretationOrigin
        && x.waveChannelMask == y.waveChannelMask, "checkpoint preserves selected channel interpretation without inferring it");
    require(expected.audioRevision == actual.audioRevision && expected.envelopeRevision == actual.envelopeRevision
        && expected.changeGeneration == actual.changeGeneration && expected.hasTimelineEdits == actual.hasTimelineEdits
        && expected.safeTimelineMetadataKeys == actual.safeTimelineMetadataKeys
        && expected.transformedMetadataKeys == actual.transformedMetadataKeys, "checkpoint preserves revisions and timeline metadata policy");
    require(expected.envelopePoints.size() == actual.envelopePoints.size(), "checkpoint preserves envelope point count");
    for (size_t index = 0; index < expected.envelopePoints.size(); ++index)
        require(std::bit_cast<uint64_t>(expected.envelopePoints[index].timeSeconds) == std::bit_cast<uint64_t>(actual.envelopePoints[index].timeSeconds)
            && std::bit_cast<uint32_t>(expected.envelopePoints[index].gain) == std::bit_cast<uint32_t>(actual.envelopePoints[index].gain),
            "checkpoint preserves exact float envelope gains and double timestamps");
    require(actual.audioBuffer && actual.playbackAvailability.isRandomAccess() && !actual.streamingReader && !actual.memoryMappedReader
        && actual.readerSourceFile == juce::File() && !actual.ambisonicBinauralFilterBank && actual.ambisonicBinauralStatus.isEmpty()
        && actual.ambisonicMonitorMode == AmbisonicMonitorMode::speakerProjection && actual.ambisonicOrientation.isIdentity(),
        "isolated checkpoint has samples, no source reader or monitor resource/runtime state");
    for (int channel = 0; channel < a.numChannels; ++channel)
        for (int sample = 0; sample < a.numSamples; ++sample)
            require(std::bit_cast<uint32_t>(expected.audioBuffer->getSample(channel, sample))
                == std::bit_cast<uint32_t>(actual.audioBuffer->getSample(channel, sample)), "checkpoint retains bit-exact edit samples");
}
juce::String text(const juce::var& value) { return juce::JSON::toString(value, true); }

void checkpointCompaction(const Snapshot& snapshot, const juce::File& root, AudioGraphLimits graphLimits)
{
    Limits bounds; bounds.records = 8;
    RecoveryStore store(root.getChildFile("checkpoint-compaction"), juce::Uuid(), bounds);
    CommitPacket packet;
    require(static_cast<bool>(RecoveryCheckpointCodec::encode(snapshot, {}, packet, graphLimits)), "encode full compaction oracle");
    SafeHead head;
    for (int i = 0; i < 6; ++i) require(static_cast<bool>(store.append(head, packet, head)), "publish real complete checkpoints before cut");
    const auto old = store.inspectManifest();
    require(static_cast<bool>(old.status) && old.commits.size() == 6, "retain original six-record chain");
    const auto directory = store.rootDirectory().getChildFile(store.ownerId().toString());
    juce::MemoryBlock originalHead;
    require(directory.getChildFile("head").loadFileAsData(originalHead), "retain actual head bytes before compaction");
    const auto unchangedHead = [&]
    {
        juce::MemoryBlock actual;
        return directory.getChildFile("head").loadFileAsData(actual) && actual == originalHead;
    };
    const auto compact = [&]
    {
        OrphanCollectionReport report;
        Status status;
        for (int pass = 0; pass < 128; ++pass)
        {
            status = RecoveryCheckpointCodec::compact(store, head, report, graphLimits);
            if (!status || !report.more) break;
        }
        if (!status) std::cerr << "Compaction: " << status.code << '\n';
        return status;
    };
    std::shared_ptr<RecoveryOwnerLease> reader;
    require(static_cast<bool>(store.acquireOwnerLease(reader)), "hold real restored-reader dependency");
    require(!compact() && unchangedHead() && store.inspectManifest().commits.size() == 6,
        "a live reader prevents advancing the protected floor without changing prior durable data");
    reader.reset();
    {
        RecoveryStore futureStore(root.getChildFile("compaction-unknown-history"), juce::Uuid());
        SafeHead futureHead;
        require(static_cast<bool>(futureStore.append({}, packet, futureHead)), "publish known checkpoint before future-schema fixture");
        auto future = packet; future.planSchema = 99;
        require(static_cast<bool>(futureStore.append(futureHead, future, futureHead))
            && static_cast<bool>(futureStore.append(futureHead, packet, futureHead)), "retain unknown historical semantics before a later known complete checkpoint");
        OrphanCollectionReport report;
        require(RecoveryCheckpointCodec::compact(futureStore, futureHead, report, graphLimits).error == Error::unsupportedSchema
            && futureStore.inspectManifest().commits.size() == 3 && futureStore.inspectManifest().checkpointCut.isEmpty(),
            "a known latest checkpoint cannot grant deletion of unknown required historical semantics");
    }
    {
        int64_t occupied = 0;
        for (const auto& file : directory.findChildFiles(juce::File::findFiles, true)) occupied += file.getSize();
        auto tight = bounds; tight.ownerBytes = occupied + 128;
        RecoveryStore limited(store.rootDirectory(), store.ownerId(), tight);
        OrphanCollectionReport report;
        require(RecoveryCheckpointCodec::compact(limited, head, report, graphLimits).code == "compaction_owner_budget"
            && unchangedHead(), "cut control records cannot exceed the existing owner bound or replace protected data");
    }
    for (const auto boundary : { RecoveryStore::CompactionBoundary::cutPublished,
                                RecoveryStore::CompactionBoundary::beforeHeadReplace })
    {
        bool reached = false;
        store.onCompactionBoundary = [&](auto actual)
        {
            if (actual == boundary) { reached = true; throw std::runtime_error("owned compaction interruption"); }
        };
        require(!compact() && reached && unchangedHead() && store.inspectManifest().commits.size() == 6,
            "interrupted cut preparation leaves the old complete chain readable and retryable");
    }
    bool replaced = false;
    store.onCompactionBoundary = [&](auto boundary)
    {
        if (boundary == RecoveryStore::CompactionBoundary::headPublished)
        { replaced = true; throw std::runtime_error("owned interruption after atomic cut"); }
    };
    require(!compact() && replaced, "interrupt after actual atomic head replacement");
    store.onCompactionBoundary = {};
    auto cut = store.inspectManifest();
    require(static_cast<bool>(cut.status) && cut.head == head && cut.commits.size() == 1
        && cut.checkpointFloor == head && cut.checkpointCut.isNotEmpty(), "new complete floor survives interruption with unchanged head/document identity");
    require(directory.getChildFile("commits").getChildFile(old.commits.front().head.hash).existsAsFile(),
        "publishing the cut alone never deletes old records");
    bool quarantined = false;
    store.onCollectionBoundary = [&](auto boundary)
    {
        if (boundary == RecoveryStore::CollectionBoundary::quarantined)
        { quarantined = true; throw std::runtime_error("owned prune interruption"); }
    };
    require(!compact() && quarantined && static_cast<bool>(store.inspect().status),
        "interrupted record quarantine leaves the retained complete history readable");
    store.onCollectionBoundary = {};
    require(static_cast<bool>(compact()), "resume the precise persisted prune transaction");
    for (auto it = old.commits.begin(); it != std::prev(old.commits.end()); ++it)
        require(!directory.getChildFile("commits").getChildFile(it->head.hash).exists(), "only proven superseded records are pruned");
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        for (int i = 0; i < 5; ++i)
            require(static_cast<bool>(store.append(head, packet, head)), "append preserves floor beyond the old lifetime sequence ceiling");
        require(static_cast<bool>(compact()), "repeat bounded compaction without accumulating obsolete cut files");
    }
    require(head.sequence == 26 && store.inspectManifest().commits.size() == 1,
        "sequence remains monotonic while retained-chain count remains bounded");
    const auto view = store.inspect();
    Snapshot restored;
    require(static_cast<bool>(view.status)
        && static_cast<bool>(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored, graphLimits)),
        "restore actual audio and complete history from the compacted chain");
    require(restored.undo.size() == snapshot.undo.size() && restored.redo.size() == snapshot.redo.size(), "compaction preserves both history stacks");
    compareState(*snapshot.state, *restored.state);
    for (size_t i = 0; i < snapshot.undo.size(); ++i) compareState(*snapshot.undo[i], *restored.undo[i]);
    for (size_t i = 0; i < snapshot.redo.size(); ++i) compareState(*snapshot.redo[i], *restored.redo[i]);
    OrphanCollectionReport cancelled;
    require(RecoveryCheckpointCodec::compact(store, head, cancelled, graphLimits, [] { return true; }).error == Error::cancelled,
        "maintenance cancellation preserves the completed compacted head");
#if JUCE_WINDOWS
    for (int boundary = 0; boundary < 4; ++boundary)
    {
        RecoveryStore crashStore(root.getChildFile("compaction-process-" + juce::String(boundary)), juce::Uuid());
        SafeHead crashHead;
        for (int i = 0; i < 6; ++i)
            require(static_cast<bool>(crashStore.append(crashHead, packet, crashHead)), "publish complete private crash-recovery chain");
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--compaction-crash", crashStore.rootDirectory().getFullPathName(), crashStore.ownerId().toString(), juce::String(boundary) })
            && child.waitForProcessToFinish(15000) && child.getExitCode() == 91,
            "terminate actual compaction child at cut/head/quarantine publication boundary");
        const auto afterDeath = crashStore.inspect();
        require(static_cast<bool>(afterDeath.status) && afterDeath.head == crashHead
            && afterDeath.commits.size() == (boundary < 2 ? 6 : 1), "process death preserves the complete old or new read boundary");
        if (boundary == 1)
        {
            OwnerRetirement retired;
            require(static_cast<bool>(RecoveryOwnerLifecycle::discard(crashStore, crashHead, retired)),
                "explicit Discard certifies the original chain after unpublished cut interruption");
            TerminalCollectionReport terminal;
            for (int pass = 0; pass < 128 && !terminal.completed; ++pass)
                require(static_cast<bool>(crashStore.collectTerminal(crashHead, terminal)),
                    "terminal cleanup recognizes only a fully bound unpublished control file");
            require(terminal.completed, "Discard completes without requiring compaction retry");
            continue;
        }
        if (boundary == 0)
            require(static_cast<bool>(crashStore.append(crashHead, packet, crashHead)),
                "new edits can advance the complete checkpoint before retrying an unpublished cut");
        OrphanCollectionReport report;
        for (int pass = 0; pass < 128; ++pass)
        {
            require(static_cast<bool>(RecoveryCheckpointCodec::compact(crashStore, crashHead, report, graphLimits)),
                "fresh owner resumes actual process-interrupted compaction and quarantine");
            if (!report.more) break;
        }
        const auto complete = crashStore.inspect();
        Snapshot recovered;
        require(static_cast<bool>(complete.status) && complete.commits.size() == 1
            && static_cast<bool>(RecoveryCheckpointCodec::decode(complete.commits.back(), crashStore, recovered, graphLimits)),
            "actual process interruption never destroys the protected complete checkpoint");
        compareState(*snapshot.state, *recovered.state);
        require(recovered.undo.size() == snapshot.undo.size() && recovered.redo.size() == snapshot.redo.size(), "process restart retains both history stacks");
        for (size_t i = 0; i < snapshot.undo.size(); ++i) compareState(*snapshot.undo[i], *recovered.undo[i]);
        for (size_t i = 0; i < snapshot.redo.size(); ++i) compareState(*snapshot.redo[i], *recovered.redo[i]);
        require(crashStore.rootDirectory().getChildFile(crashStore.ownerId().toString()).getChildFile("commits")
            .findChildFiles(juce::File::findFiles, false).size() == 2,
            "resumption leaves exactly the retained checkpoint and active cut, including after intervening edits");
    }
#endif
    OwnerRetirement retirement;
    require(static_cast<bool>(RecoveryOwnerLifecycle::discard(store, head, retirement)), "explicit reviewed Discard accepts an intact compacted owner");
    TerminalCollectionReport terminal;
    for (int pass = 0; pass < 128 && !terminal.completed; ++pass)
    {
        const auto collected = store.collectTerminal(head, terminal);
        if (!collected) std::cerr << "Compacted terminal cleanup: " << collected.code << '\n';
        require(static_cast<bool>(collected), "normal terminal cleanup handles the versioned cut and retained assets");
    }
    require(terminal.completed && !directory.exists(), "terminal cleanup removes only the exact compacted fixture owner");
    std::cout << "RecoveryCheckpointCompactionTests passed (actual cut interruption, reader lease, resumed prune, monotonic 26/8, full history PCM)\n";
}
} // namespace

int runRecoveryCompactionCrashChild(const juce::File& root, juce::Uuid owner, int boundary)
{
#if JUCE_WINDOWS
    RecoveryStore store(root, owner);
    const auto manifest = store.inspectManifest();
    if (!manifest.status || manifest.head.sequence < 2) return 21;
    store.onCompactionBoundary = [boundary](auto actual)
    { if (static_cast<int>(actual) == boundary) TerminateProcess(GetCurrentProcess(), 91); };
    store.onCollectionBoundary = [boundary](auto actual)
    { if (boundary == 3 && actual == RecoveryStore::CollectionBoundary::quarantined) TerminateProcess(GetCurrentProcess(), 91); };
    OrphanCollectionReport report;
    const auto status = RecoveryCheckpointCodec::compact(store, manifest.head, report);
    if (!status) std::cerr << "Compaction child: " << status.code << '\n';
    return 22; // The requested abrupt boundary must be reached.
#else
    juce::ignoreUnused(root, owner, boundary);
    return 23;
#endif
}

void runRecoveryCheckpointCodecTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot& sourceSnapshot,
                                    const juce::File& root, singleeditor::recovery::AudioGraphLimits limits)
{
    auto snapshot = sourceSnapshot;
    auto precisionState = std::make_shared<SingleTrackDocument::State>(*snapshot.state);
    precisionState->envelopePoints.insert(precisionState->envelopePoints.begin() + 1, { 1.0 / 48000.0, 0.1234567f });
    precisionState->info.metadata.set("RecoveryFixture", juce::String::fromUTF8("引号\"与换行\n UTF-8 metadata"));
    precisionState->ambisonicMonitorMode = AmbisonicMonitorMode::binauralHrtf;
    precisionState->ambisonicOrientation = { 23.0f, 8.0f, -14.0f };
    precisionState->ambisonicBinauralStatus = "PRIVATE_RUNTIME_STATUS_MUST_NOT_SERIALIZE";
    snapshot.state = precisionState;
    checkpointCompaction(snapshot, root, limits);
    RecoveryStore store(root.getChildFile("checkpoint"), juce::Uuid());
    CommitPacket packet;
    const auto encoded = RecoveryCheckpointCodec::encode(snapshot, {}, packet, limits);
    if (!encoded) std::cerr << "Checkpoint encode: " << encoded.code << '\n';
    require(static_cast<bool>(encoded), "encode actual audio/edit/Undo/Redo checkpoint");
    require(!packet.stateJson.contains("PRIVATE_RUNTIME_STATUS") && !packet.stateJson.contains("binaural")
        && !packet.stateJson.contains("readerSourceFile"), "checkpoint excludes monitor/private runtime and reader bindings");
    SafeHead head;
    require(static_cast<bool>(store.append({}, packet, head)), "publish complete checkpoint with asset closure");
    const auto manifest = store.inspectManifest();
    PreparedRecoveryCheckpoint prepared;
    require(static_cast<bool>(manifest.status) && manifest.commits.size() == 1
        && static_cast<bool>(RecoveryCheckpointCodec::prepare(manifest.commits.front(), prepared, limits)), "prepare complete checkpoint metadata from record-only discovery");
    require(prepared.getHead() == head && prepared.getIdentity().document.sameVersion(snapshot.identity.document)
        && prepared.getUndoCount() == snapshot.undo.size() && prepared.getRedoCount() == snapshot.redo.size()
        && prepared.getStates().size() == 1 + snapshot.undo.size() + snapshot.redo.size(), "prepared metadata preserves captured head, identity and complete history shape");
    for (const auto& state : prepared.getStates())
        require(!state->audioBuffer && !state->hasPlaybackSource() && state->getPlayableSampleRange().isEmpty(),
            "prepared metadata is never an audio-backed or playable document");
    const auto preparedStates = prepared.getStates();
    int preparePolls = 0;
    require(RecoveryCheckpointCodec::prepare(manifest.commits.front(), prepared, limits, [&] { return ++preparePolls == 3; }).error == Error::cancelled
        && prepared.getStates() == preparedStates, "metadata cancellation retains prior complete prepared state");
    const auto view = store.inspect();
    require(static_cast<bool>(view.status) && view.commits.size() == 1, "inspect complete checkpoint chain");
    Snapshot restored;
    const auto decoded = RecoveryCheckpointCodec::decode(view.commits.back(), store, restored, limits);
    if (!decoded) std::cerr << "Checkpoint decode: " << decoded.code << '\n';
    require(static_cast<bool>(decoded), "decode isolated complete checkpoint");
    require(restored.identity.document.sameVersion(snapshot.identity.document)
        && restored.identity.source.sameBinding(snapshot.identity.source)
        && restored.identity.source.location.file == snapshot.identity.source.location.file
        && restored.identity.source.sizeBytes == snapshot.identity.source.sizeBytes
        && restored.identity.source.modifiedMilliseconds == snapshot.identity.source.modifiedMilliseconds
        && restored.identity.source.fastStampValid == snapshot.identity.source.fastStampValid
        && restored.identity.savedGeneration == snapshot.identity.savedGeneration && !restored.identity.asset,
        "checkpoint preserves lineage/runtime provenance, source evidence and saved boundary");
    require(restored.undo.size() == snapshot.undo.size() && restored.redo.size() == snapshot.redo.size(), "checkpoint preserves complete ordered stack shape");
    compareState(*snapshot.state, *restored.state);
    for (size_t index = 0; index < snapshot.undo.size(); ++index) compareState(*snapshot.undo[index], *restored.undo[index]);
    for (size_t index = 0; index < snapshot.redo.size(); ++index) compareState(*snapshot.redo[index], *restored.redo[index]);
    runProgressiveRecoveredDocumentTests(prepared, restored, store, root, limits);
    runProgressiveRecoveryWaveformTests(prepared, restored, store, root, limits);
    runRecoveryRestoreCandidateTests(restored, root, limits);
    runRecoveryRestoreQueueTests(restored, root, limits);
    runRecoverySourceBindingCheckpointTests(restored, root, limits);
    CommitPacket roundtrip;
    std::set<juce::String> known;
    for (const auto& id : *view.commits.back().record["assets"].getArray()) known.insert(id.toString());
    require(static_cast<bool>(RecoveryCheckpointCodec::encode(restored, known, roundtrip, limits))
        && roundtrip.stateJson == packet.stateJson && roundtrip.planJson == packet.planJson
        && roundtrip.newAudio.empty() && roundtrip.existingAudio.size() == known.size(), "canonical semantic roundtrip reuses content without runtime state");

    const auto original = restored;
    const auto rejectGraph = [&] (juce::var changed, const char* reason)
    {
        // Publish a checksum-valid but semantically invalid checkpoint. This
        // tests the Document codec boundary independently of Store's checksums.
        auto bad = roundtrip;
        bad.stateJson = text(changed);
        SafeHead next;
        require(static_cast<bool>(store.append(head, bad, next)), "store accepts syntactically valid fault fixture for semantic decoder");
        head = next;
        const auto alteredView = store.inspect();
        require(static_cast<bool>(alteredView.status), "fault fixture remains storage-valid");
        const auto alteredManifest = store.inspectManifest();
        require(static_cast<bool>(alteredManifest.status)
            && !RecoveryCheckpointCodec::prepare(alteredManifest.commits.back(), prepared, limits)
            && prepared.getStates() == preparedStates, "metadata preparation rejects semantic corruption without touching previous output");
        require(!RecoveryCheckpointCodec::decode(alteredView.commits.back(), store, restored, limits)
            && restored.state == original.state && restored.undo == original.undo && restored.redo == original.redo, reason);
    };
    auto changed = juce::JSON::parse(packet.stateJson);
    changed.getDynamicObject()->setProperty("undo_count", 0);
    rejectGraph(changed, "mismatched stack counts fail without replacing candidate");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0].getDynamicObject()->setProperty("generation", snapshot.identity.document.generation + 1);
    rejectGraph(changed, "current metadata must bind to record generation");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0].getDynamicObject()->setProperty("bits", 128);
    rejectGraph(changed, "invalid original audio format is not silently normalized");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0].getDynamicObject()->setProperty("rate", 96000.0);
    rejectGraph(changed, "state metadata and audio sequence cannot disagree");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0]["layout"]["roles"].getArray()->set(0, 999);
    rejectGraph(changed, "unknown channel role fails closed");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0]["envelope"][1].getArray()->set(1, "7f800000");
    rejectGraph(changed, "nonfinite gain bits fail closed");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0]["envelope"][1].getArray()->set(0, "0000000000000000");
    rejectGraph(changed, "duplicate/unsorted envelope timestamps fail closed");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0].getDynamicObject()->setProperty("new_required_field", true);
    rejectGraph(changed, "unknown required state fields cannot be discarded");
    changed = juce::JSON::parse(packet.stateJson);
    changed["states"][0]["metadata"].getArray()->add(changed["states"][0]["metadata"][0]);
    rejectGraph(changed, "duplicate metadata cannot overwrite another key");
    auto omitted = roundtrip;
    omitted.existingAudio.pop_back();
    SafeHead noClosure;
    require(static_cast<bool>(store.append(head, omitted, noClosure)), "publish fault fixture with incomplete declared asset closure");
    head = noClosure;
    const auto closureView = store.inspect();
    require(static_cast<bool>(closureView.status)
        && RecoveryCheckpointCodec::decode(closureView.commits.back(), store, restored, limits).code == "checkpoint_asset_closure"
        && restored.state == original.state, "valid orphan CAS file cannot conceal incomplete graph asset declarations");
    require(RecoveryCheckpointCodec::prepare(store.inspectManifest().commits.back(), prepared, limits).code == "checkpoint_asset_closure"
        && prepared.getStates() == preparedStates, "manifest-driven preparation enforces exact dependency closure before any sample read");
    int polls = 0;
    require(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored, limits, [&] { return ++polls == 9; }).error == Error::cancelled
        && restored.state == original.state, "whole checkpoint cancellation retains previous complete output");
    auto invalidInput = snapshot;
    auto invalidState = std::make_shared<SingleTrackDocument::State>(*snapshot.state);
    invalidState->info.metadata.set("TooLarge", juce::String::repeatedString("x", 16385));
    invalidInput.state = invalidState;
    auto unchanged = roundtrip;
    require(!RecoveryCheckpointCodec::encode(invalidInput, known, unchanged, limits) && unchanged.stateJson == roundtrip.stateJson,
        "oversized local state fails without publishing a partial packet");
    // End-to-end discovery -> semantic preparation -> first verified PCM, with
    // a genuinely missing later body. This path must not call full inspect.
    const auto partialRoot = root.getChildFile("checkpoint-partial");
    const juce::Uuid partialOwner;
    RecoveryStore partialStore(partialRoot, partialOwner);
    SafeHead partialHead;
    require(static_cast<bool>(partialStore.append({}, packet, partialHead)), "publish independent progressive-checkpoint fixture");
    const auto& sequence = prepared.getAudioGraph().sequences[prepared.getAudioGraph().states[0]];
    require(sequence.chunks.size() > 1, "progressive checkpoint has multiple audio chunks");
    const auto missingAsset = partialRoot.getChildFile(partialOwner.toString()).getChildFile("assets").getChildFile(sequence.chunks[1].id);
    require(missingAsset.deleteFile(), "remove only the later chunk in the private progressive fixture");
    const auto partialManifest = partialStore.inspectManifest();
    PreparedRecoveryCheckpoint partial;
    require(static_cast<bool>(partialManifest.status) && static_cast<bool>(RecoveryCheckpointCodec::prepare(partialManifest.commits.back(), partial, limits)),
        "missing later body does not force discovery or metadata to read all audio");
    const auto scratch = root.getChildFile("checkpoint-partial-scratch");
    require(scratch.createDirectory().wasOk(), "create private checkpoint scratch directory");
    std::unique_ptr<ProgressiveRecoveryAudio> prefix;
    require(static_cast<bool>(ProgressiveRecoveryAudio::create(partial.getAudioJson(), 0, scratch, prefix, limits))
        && static_cast<bool>(prefix->loadNext(partialStore)) && prefix->readyFrames() == sequence.chunks.front().frames && !prefix->isComplete(),
        "record discovery through first real playable prefix completes before the missing later body is requested");
    juce::AudioBuffer<float> firstSamples(snapshot.state->info.numChannels, 64);
    require(prefix->getReader()->read(&firstSamples, 0, 64, 0, true, true), "prepared checkpoint prefix returns real PCM");
    for (int channel = 0; channel < firstSamples.getNumChannels(); ++channel)
        for (int frame = 0; frame < 64; ++frame)
            require(std::bit_cast<uint32_t>(firstSamples.getSample(channel, frame))
                == std::bit_cast<uint32_t>(snapshot.state->audioBuffer->getSample(channel, frame)), "first recovered PCM is bit-exact without complete-asset scanning");
    require(prefix->loadNext(partialStore).error == Error::missingOrCorruptAsset && !partialStore.inspect().status,
        "explicit later load and full certification still detect the missing sample body");
    prefix.reset();
    require(scratch.getNumberOfChildFiles(juce::File::findFiles) == 0, "partial checkpoint reader releases its private scratch mapping");
    runRecoveredDocumentTests(original, root);
    std::cout << "RecoveryCheckpointCodecTests passed\n";
}
