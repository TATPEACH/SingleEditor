#include "Recovery/RecoveryCheckpointCodec.h"
#include "Recovery/RecoverySourceBinding.h"
#include "Recovery/RecoveryMutationProducer.h"
#include "Recovery/RecoveryInspection.h"
#include "Recovery/RecoveryOwnerLifecycle.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <map>
#if JUCE_WINDOWS
 #include <windows.h>
 #include <winioctl.h>
#endif

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
    if (!status) std::cerr << "Source binding: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
SourceAudioFacts waveFixture(const juce::File& file)
{
    constexpr int payloadBytes = 512 * 1024;
    juce::FileOutputStream output(file);
    require(output.openedOk(), "open private source-proof WAV");
    output.write("RIFF", 4); output.writeInt(36 + payloadBytes); output.write("WAVEfmt ", 8); output.writeInt(16);
    output.writeShort(1); output.writeShort(2); output.writeInt(48000); output.writeInt(192000);
    output.writeShort(4); output.writeShort(16); output.write("data", 4); output.writeInt(payloadBytes);
    for (int i = 0; i < payloadBytes / 2; ++i) output.writeShort(static_cast<short>((i * 31) % 32767));
    output.flush(); require(output.getStatus().wasOk(), "write complete private source-proof WAV");
    return { "WAV-PCM", "stereo", 48000, 2, 16, payloadBytes / 4, 44, payloadBytes };
}
void restoreWriteTime(const juce::File& file, int64_t ticks)
{
#if JUCE_WINDOWS
    const auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open attributes of owned fixture only");
    const FILETIME time { static_cast<DWORD>(static_cast<uint64_t>(ticks)), static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32) };
    const auto set = SetFileTime(handle, nullptr, nullptr, &time);
    CloseHandle(handle);
    require(set != FALSE, "restore exact private fixture LastWriteTime");
#else
    juce::ignoreUnused(file, ticks);
#endif
}
#if JUCE_WINDOWS
void installOwnedJunction(const juce::File& link, const juce::File& target, const juce::File& root)
{
    require(link.isAChildOf(root) && target.isAChildOf(root) && link.createDirectory().wasOk() && target.createDirectory().wasOk(),
        "source-junction fixture stays inside owned UUID root");
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
    const auto handle = CreateFileW(link.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open only owned junction directory");
    DWORD returned = 0;
    const auto installed = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.getData(), static_cast<DWORD>(buffer.getSize()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    require(installed != FALSE, "install actual source-observation junction fixture");
}
#endif
}

void runRecoverySourceBindingCheckpointTests(const Snapshot& original, const juce::File& root, AudioGraphLimits limits)
{
    const auto wave = root.getChildFile("bound-source.wav");
    const auto facts = waveFixture(wave);
    {
        SingleTrackDocument nativeDocument;
        require(nativeDocument.loadFile(wave), "actual native reader supplies original physical and decoder facts");
        const auto originalSource = nativeDocument.getMediaIdentity().source;
        RecoveryMutationProducer::Config settings; settings.root = root.getChildFile("native-source-proof"); settings.graph = limits;
        settings.graph.chunkSampleBytes = 64 * 1024;
        std::unique_ptr<RecoveryMutationProducer> producer;
        good(RecoveryMutationProducer::attachPostCore(nativeDocument, settings, producer), "attach actual first-mutation source consumer");
        require(!settings.root.exists() && producer->health().owner.isNull(), "clean native source admission performs no fingerprint or journal work");
        require(nativeDocument.moveEnvelopePoint(0, 0, 0.73f), "first real mutation triggers optional bounded source evidence");
        require(producer->drain(std::chrono::seconds(15)), "drain first actual source-bearing checkpoint");
        const auto health = producer->health(); good(health.failure, "actual source-bearing producer reports protected state");
        auto nativeStore = RecoveryStore(settings.root, health.owner);
        auto nativeManifest = nativeStore.inspectManifest(); good(nativeManifest.status, "read actual producer manifest");
        PreparedRecoveryCheckpoint nativePrepared;
        good(RecoveryCheckpointCodec::prepareThrough(nativeManifest, nativeManifest.head, nativePrepared, settings.graph), "prepare actual source-bearing native graph");
        require(nativePrepared.getSourceEvidence().has_value(), "production first checkpoint contains admitted source proof");
        const auto encodedEvidence = *nativePrepared.getSourceEvidence();
        require(encodedEvidence.algorithm == "source-file-3x64k-sha256-v1" && encodedEvidence.facts.payloadOffset == 0
            && encodedEvidence.facts.payloadBytes == wave.getSize() && encodedEvidence.facts.frames == facts.frames
            && encodedEvidence.fullFileSha256.isEmpty() && encodedEvidence.bindsTo(originalSource),
            "native profile records encoded-file coverage and original decoder dimensions without full-file hash");
        std::unique_ptr<RecoverySourceBinding> nativeBinding;
        good(RecoverySourceBinding::resolve(originalSource, encodedEvidence, {}, nativeBinding), "actual resolver consumes native algorithm");
        require(nativeBinding->getResolution().confidence == SourceConfidence::exact && nativeBinding->payloadBytesRead() == 3 * 65536
            && nativeBinding->fullBytesRead() == 0, "real native source proves Exact with fixed windows only");
        auto reviewed = RecoverySourceBinding::Request{}; reviewed.expectedObservation = nativeBinding->getObservation();
        std::unique_ptr<RecoverySourceBinding> checked;
        good(RecoverySourceBinding::resolve(originalSource, encodedEvidence, reviewed, checked), "unchanged inspected source can be revalidated");
        reviewed.expectedObservation->changedTicks += 1;
        require(RecoverySourceBinding::resolve(originalSource, encodedEvidence, reviewed, checked).code == "source_review_obsolete",
            "mismatching physical review observation rejects fresh source acceptance");
        reviewed = {}; reviewed.verification = RecoverySourceIdentity::Mode::explicitDeepVerify;
        good(RecoverySourceBinding::resolve(originalSource, encodedEvidence, reviewed, checked), "explicit native Deep Verify traverses actual source bytes");
        require(checked->fullBytesRead() == wave.getSize() && checked->getResolution().confidence == SourceConfidence::exact,
            "deep verification reports real full-file traversal without changing source write rights");
        const auto firstNativeHead = nativeManifest.head;
        const auto attemptRoot = root.getChildFile("native-inspection-attempts");
        RecoveryInspection::Request inspection; inspection.observedHead = firstNativeHead; inspection.graph = settings.graph;
        const auto diagnostics = std::make_shared<RecoveryDiagnosticState>();
        std::optional<RecoveryDiagnosticState::Failure> diagnosticFailure;
        require(RecoveryInspection::Report().operationId.isNull(), "an unexecuted report has no invented operation");
        auto report = RecoveryInspection::read(nativeStore, attemptRoot, inspection, nullptr, {}, diagnostics);
        require(!report.operationId.isNull() && diagnostics->trySnapshot(diagnosticFailure) && !diagnosticFailure,
            "actual successful inspection gets one operation ID and no fabricated failure");
        good(report.status, "safe inspect actual source-bearing owner without restoring it");
        require(report.metadataReady && report.points.size() == 1 && !report.sourceChecked && !attemptRoot.exists()
            && !report.checkpoint.getStates().front()->hasPlaybackSource(), "safe inspection creates no marker, source reader or playable document");
        const auto asset = nativeStore.rootDirectory().getChildFile(health.owner.toString()).getChildFile("assets").getChildFile(report.assets.front());
        const auto held = asset.getSiblingFile(asset.getFileName() + ".inspection-held");
        require(asset.moveFileTo(held), "hide one exact owned CAS body to verify safe inspection scope");
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        AudioBlock absentAudio;
        require(report.status && report.metadataReady && !nativeStore.readAudio(report.assets.front(), absentAudio),
            "safe inspection verifies reference graph without pretending absent CAS body is healthy or loading it");
        inspection.verifyStoredAudio = true;
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection, nullptr, {}, diagnostics);
        require(report.status && report.metadataReady && !report.audioVerified && report.audioStatus.error == Error::missingOrCorruptAsset
            && report.audioFailureAsset == asset.getFileName(), "explicit audio verification reports the actual missing asset without losing metadata inspection");
        require(diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == report.operationId && diagnosticFailure->owner == health.owner
            && diagnosticFailure->stage == RecoveryDiagnosticState::Stage::centerAudio
            && diagnosticFailure->error == Error::missingOrCorruptAsset
            && diagnosticFailure->counters[0] == static_cast<int64_t>(report.metadataBytesRead)
            && diagnosticFailure->counters[5] == firstNativeHead.sequence,
            "actual missing-audio result supplies the shared slot with exact operation and counters");
        const auto failedInspection = *diagnosticFailure;
        require(held.moveFileTo(asset), "restore only the hidden owned CAS object");
        size_t verifiedQuanta = 0;
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection, nullptr,
            [&](size_t verified, size_t total, size_t) { require(verified <= total, "actual audio progress stays within reference count"); verifiedQuanta = verified; }, diagnostics);
        require(report.status && report.audioStatus && report.audioVerified && report.audioAssetsVerified == report.assets.size()
            && report.audioAssetsVerified > 1 && verifiedQuanta == report.assets.size() && report.audioPayloadBytesRead > 0
            && report.audioPayloadBytesRead <= inspection.audioReadBytes && !attemptRoot.exists()
            && !report.checkpoint.getStates().front()->hasPlaybackSource(), "audio Deep Verify checks real bounded assets without creating playable audio or attempt markers");
        const auto completeAudioRead = report.audioPayloadBytesRead;
        require(report.operationId != failedInspection.operationId && diagnostics->trySnapshot(diagnosticFailure)
            && diagnosticFailure && diagnosticFailure->operationId == failedInspection.operationId
            && diagnosticFailure->counters == failedInspection.counters,
            "real successful recheck gets a new ID while preserving the original immutable failure");
        {
            // Both the manifest and CAS are independently well formed. Only
            // their cross-reference audio dimensions disagree, so hash checks
            // alone cannot establish a recoverable document.
            const auto captured = nativeDocument.captureCommittedState(nativeDocument.getMediaIdentity().document);
            require(captured.has_value(), "capture actual document for mismatched but valid CAS fixture");
            CommitPacket mismatched;
            good(RecoveryCheckpointCodec::encodeWithSourceEvidence(*captured, encodedEvidence, {}, mismatched, settings.graph),
                "encode actual complete checkpoint for cross-reference validation");
            std::map<juce::String, juce::String> remapped;
            for (auto& block : mismatched.newAudio)
            {
                juce::String before, after;
                good(RecoveryStore::identifyAudio(block, settings.graph.assetBytes, before), "identify original legal audio block");
                block.sampleRate += 1000;
                good(RecoveryStore::identifyAudio(block, settings.graph.assetBytes, after), "identify independently legal block with different rate");
                remapped[before] = after;
            }
            auto stateJson = juce::JSON::parse(mismatched.stateJson);
            auto audioJson = juce::JSON::parse(stateJson["audio"].toString());
            for (auto& sequence : *audioJson["sequences"].getArray())
                for (auto& chunk : *sequence["chunks"].getArray())
                    chunk.getDynamicObject()->setProperty("id", remapped.at(chunk["id"].toString()));
            stateJson.getDynamicObject()->setProperty("audio", juce::JSON::toString(audioJson, true));
            mismatched.stateJson = juce::JSON::toString(stateJson, true);
            RecoveryStore mismatchedStore(root.getChildFile("valid-cas-wrong-graph-rate"), juce::Uuid());
            SafeHead mismatchHead;
            good(mismatchedStore.append({}, mismatched, mismatchHead), "publish actual valid record and hash-valid CAS with mismatched reference dimensions");
            auto mismatchRequest = inspection; mismatchRequest.observedHead = mismatchHead; mismatchRequest.selectedHead = mismatchHead;
            const auto mismatchReport = RecoveryInspection::read(mismatchedStore, attemptRoot, mismatchRequest);
            require(mismatchReport.status && mismatchReport.metadataReady && !mismatchReport.audioVerified
                && mismatchReport.audioStatus.code == "inspection_audio_dimensions" && mismatchReport.audioPayloadBytesRead > 0,
                "Deep Verify rejects hash-valid audio that disagrees with the actual selected history graph");
        }
        auto shortBudget = inspection; shortBudget.audioReadBytes = 16;
        report = RecoveryInspection::read(nativeStore, attemptRoot, shortBudget, nullptr, {}, diagnostics);
        require(report.status && report.metadataReady && !report.audioVerified && report.audioStatus.error == Error::budgetExceeded
            && report.audioAssetsVerified == 0 && report.audioPayloadBytesRead == 0, "insufficient explicit read budget is not misreported as asset corruption");
        require(diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == report.operationId && diagnosticFailure->error == Error::budgetExceeded,
            "a later actual budget failure replaces the single prior result");
        const auto budgetFailure = *diagnosticFailure;
        std::atomic<bool> stopAudio { false };
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection, &stopAudio,
            [&](size_t verified, size_t, size_t) { if (verified == 1) stopAudio.store(true); }, diagnostics);
        require(report.status.error == Error::cancelled && !report.audioVerified && report.audioAssetsVerified == 1
            && report.audioPayloadBytesRead < completeAudioRead, "actual audio verification cancellation stops after one block without claiming complete history");
        require(diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure && diagnosticFailure->operationId == budgetFailure.operationId,
            "ordinary cancellation never replaces the prior actual failure");
        juce::MemoryBlock intactAsset;
        require(asset.loadFileAsData(intactAsset), "snapshot only owned asset bytes for corrupt-frame test");
        auto damagedAsset = intactAsset;
        static_cast<char*>(damagedAsset.getData())[damagedAsset.getSize() - 1] ^= 1;
        require(asset.replaceWithData(damagedAsset.getData(), damagedAsset.getSize()), "corrupt one byte of exact owned asset");
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        require(report.status && !report.audioVerified && report.audioStatus.error == Error::missingOrCorruptAsset
            && report.audioPayloadBytesRead > 0 && report.audioFailureAsset == asset.getFileName(),
            "failed actual hash read is counted and identifies the corrupt asset");
        damagedAsset = intactAsset; static_cast<char*>(damagedAsset.getData())[3] = '2';
        require(asset.replaceWithData(damagedAsset.getData(), damagedAsset.getSize()), "install unknown frame version only in owned asset");
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        require(report.status && !report.audioVerified && report.audioStatus.error == Error::unsupportedSchema && report.audioPayloadBytesRead > 0,
            "future stored-audio frame stays typed UnsupportedSchema instead of generic corruption");
        require(asset.replaceWithData(intactAsset.getData(), intactAsset.getSize()), "restore the exact original framed asset after fault tests");
#if JUCE_WINDOWS
        const auto busyAsset = CreateFileW(asset.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(busyAsset != INVALID_HANDLE_VALUE, "hold a real write handle only on the owned asset");
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        CloseHandle(busyAsset);
        require(report.status && !report.audioVerified && report.audioStatus.error == Error::resourceOrEnvironmentFault
            && report.audioStatus.code == "read_open", "actual OS sharing fault remains resource/environment, not corrupt audio");
#endif
        inspection.verifyStoredAudio = false;
        inspection.action = RecoveryInspection::SourceAction::deepVerify;
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        good(report.status, "explicit source Deep Verify report");
        require(report.sourceStatus && report.sourceChecked && report.sourceFullBytesRead == wave.getSize()
            && report.sourceFullHash == juce::SHA256(wave).toHexString(), "inspection reports actual independently checked full source hash");
#if JUCE_WINDOWS
        // The admitted original is correctly protected by its source lease.
        // Occupy a separate external candidate, never weaken that live lease.
        const auto occupiedSource = wave.getSiblingFile("inspection-occupied-source.wav");
        require(wave.copyFileTo(occupiedSource), "copy an owned external source candidate for ordinary sharing contention");
        auto occupiedRequest = inspection; occupiedRequest.source.locatedFile = occupiedSource;
        const auto busySource = CreateFileW(occupiedSource.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(busySource != INVALID_HANDLE_VALUE, "hold an ordinary write handle on the owned source fixture");
        report = RecoveryInspection::read(nativeStore, attemptRoot, occupiedRequest, nullptr, {}, diagnostics);
        CloseHandle(busySource);
        const auto failedSource = report.diagnosticObservation("source-tests-v1");
        require(report.status && !report.sourceStatus && report.sourceCheckAttempted && !report.sourceChecked
            && failedSource.sourceWindowBytes == -1 && failedSource.sourceFullBytes == -1
            && diagnostics->trySnapshot(diagnosticFailure) && diagnosticFailure
            && diagnosticFailure->operationId == report.operationId && diagnosticFailure->stage == RecoveryDiagnosticState::Stage::centerSource
            && diagnosticFailure->counters[1] == -1 && diagnosticFailure->counters[2] == -1,
            "real occupied-source failure remains typed and does not invent zero bytes for unreported resolver counters");
#endif
        for (int i = 0; i < 2; ++i)
        {
            std::unique_ptr<RecoveryAttemptWriter> abandoned;
            good(RecoveryAttemptStore(attemptRoot, health.owner).begin(juce::Uuid(), firstNativeHead, firstNativeHead, "PRIVATE-BUILD-TOKEN", abandoned),
                "create actual interrupted attempt marker for safe inspection");
            abandoned.reset();
        }
        inspection.action = RecoveryInspection::SourceAction::none;
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        require(report.status && report.quarantinedFor("PRIVATE-BUILD-TOKEN") && !report.quarantinedFor("different-build")
            && report.attempts.attempts.size() == 2, "safe inspection classifies actual unleased marker history with exact build/head scope");
        const auto observation = report.diagnosticObservation("PRIVATE-BUILD-TOKEN");
        require(observation.operationId == report.operationId && observation.selectedHead == firstNativeHead
            && observation.quarantined && observation.attempts == 2 && observation.metadataReady
            && observation.attemptsChecked && !observation.sourceCheckAttempted && !observation.audioCheckAttempted,
            "bounded inspection facts preserve exact selected point and actual checks without retaining the full report");
        std::atomic<bool> stopped { true };
        require(RecoveryInspection::read(nativeStore, attemptRoot, inspection, &stopped).status.error == Error::cancelled,
            "explicit inspection cancellation stops before storage reads");
        require(nativeDocument.deleteSelection({ 0.1, 0.2 }), "edit sample count after first source-bearing checkpoint");
        require(producer->drain(std::chrono::seconds(15)), "drain later actual audio edit");
        nativeManifest = nativeStore.inspectManifest(); good(nativeManifest.status, "read later source epoch checkpoint");
        good(RecoveryCheckpointCodec::prepareThrough(nativeManifest, nativeManifest.head, nativePrepared, settings.graph), "prepare later edited native checkpoint");
        require(nativePrepared.getSourceEvidence() && nativePrepared.getSourceEvidence()->facts.frames == facts.frames
            && nativePrepared.getStates().front()->info.numSamples != facts.frames
            && nativePrepared.getSourceEvidence()->sparseSha256 == encodedEvidence.sparseSha256,
            "later edits reuse original source facts and fingerprint rather than edited dimensions");
        {
            auto allHistory = inspection; allHistory.observedHead = nativeManifest.head; allHistory.selectedHead = nativeManifest.head;
            allHistory.verifyStoredAudio = true;
            const auto& graph = nativePrepared.getAudioGraph();
            std::set<juce::String> currentAssets;
            for (const auto& chunk : graph.sequences[graph.states.front()].chunks) currentAssets.insert(chunk.id);
            juce::String historyOnly;
            for (const auto& sequence : graph.sequences) for (const auto& chunk : sequence.chunks)
                if (currentAssets.count(chunk.id) == 0) historyOnly = chunk.id;
            require(historyOnly.isNotEmpty(), "real delete history has an Undo-only asset outside current audio");
            const auto historyAsset = asset.getSiblingFile(historyOnly), historyHeld = asset.getSiblingFile(historyOnly + ".history-held");
            require(historyAsset.moveFileTo(historyHeld), "hide only an exact Undo-history asset");
            report = RecoveryInspection::read(nativeStore, attemptRoot, allHistory);
            require(report.status && report.metadataReady && !report.audioVerified && report.audioFailureAsset == historyOnly,
                "Deep Verify traverses full Undo history, not only currently playable audio");
            require(historyHeld.moveFileTo(historyAsset), "restore exact Undo-history object");
        }
        require(RecoveryInspection::read(nativeStore, attemptRoot, inspection).status.error == Error::staleHead,
            "a newly published actual head invalidates the previous inspection request");
        inspection.observedHead = nativeManifest.head; inspection.selectedHead = firstNativeHead;
        report = RecoveryInspection::read(nativeStore, attemptRoot, inspection);
        require(report.status && report.metadataReady && report.points.size() == 2 && report.selectedHead == firstNativeHead
            && report.checkpoint.getStates().front()->info.numSamples == facts.frames,
            "earlier safe point is selected from verified parent chain and restores that point's complete metadata graph");
        auto changingHead = inspection; changingHead.verifyStoredAudio = true;
        bool changedDuringVerification = false;
        report = RecoveryInspection::read(nativeStore, attemptRoot, changingHead, nullptr,
            [&](size_t verified, size_t, size_t)
            {
                if (verified != 1 || changedDuringVerification) return;
                require(nativeDocument.moveEnvelopePoint(0, 0, 0.79f) && producer->drain(std::chrono::seconds(15)),
                    "actual writer may publish a new head while owner reader lease protects older referenced audio");
                changedDuringVerification = true;
            });
        require(changedDuringVerification && report.status.error == Error::staleHead && !report.audioVerified,
            "head publication during real audio verification invalidates the whole reviewed result");
        nativeManifest = nativeStore.inspectManifest(); good(nativeManifest.status, "read the actually advanced native head");
        inspection.observedHead = nativeManifest.head;
        producer->close(); require(producer->drain(std::chrono::seconds(15)), "release actual native proof producer");
        producer.reset();
        auto leasedInspection = inspection; leasedInspection.verifyStoredAudio = true;
        bool blockedRetirement = false;
        report = RecoveryInspection::read(nativeStore, attemptRoot, leasedInspection, nullptr,
            [&](size_t verified, size_t, size_t)
            {
                if (verified != 0) return;
                OwnerRetirement retired;
                blockedRetirement = RecoveryOwnerLifecycle::discard(nativeStore, nativeManifest.head, retired).code == "owner_in_use";
            });
        require(report.status && report.audioVerified && blockedRetirement, "actual verification holds the shared owner lease after producer retirement");
        OwnerRetirement afterVerification;
        good(RecoveryOwnerLifecycle::discard(nativeStore, nativeManifest.head, afterVerification), "verification releases its owner lease after completion");
        const auto saved = root.getChildFile("native-source-proof-saved.wav");
        require(nativeDocument.saveAsToFile(saved), "actual Save As publishes its validated output facts");
        settings.root = root.getChildFile("native-source-proof-after-save");
        good(RecoveryMutationProducer::attachPostCore(nativeDocument, settings, producer), "admit new source epoch after real Save");
        require(nativeDocument.moveEnvelopePoint(0, 0, 0.81f) && producer->drain(std::chrono::seconds(15)), "first post-Save mutation captures new physical source proof");
        nativeStore = RecoveryStore(settings.root, producer->health().owner);
        nativeManifest = nativeStore.inspectManifest(); good(nativeManifest.status, "inspect actual post-Save source epoch");
        good(RecoveryCheckpointCodec::prepareThrough(nativeManifest, nativeManifest.head, nativePrepared, settings.graph), "prepare post-Save source proof");
        require(nativePrepared.getSourceEvidence() && nativePrepared.getSourceEvidence()->location == saved
            && nativePrepared.getSourceEvidence()->facts.frames == nativeDocument.getState()->info.numSamples
            && nativePrepared.getSourceEvidence()->facts.frames != facts.frames,
            "Save publication preserves actual output dimensions rather than pre-edit source dimensions");
        producer->close(); require(producer->drain(std::chrono::seconds(15)), "drain post-Save source proof producer");
    }
    auto snapshot = original;
    snapshot.identity.source.location.file = wave;
    snapshot.identity.source.sizeBytes = wave.getSize();
    snapshot.identity.source.modifiedMilliseconds = wave.getLastModificationTime().toMilliseconds();
    snapshot.identity.source.fastStampValid = true;
    const auto source = snapshot.identity.source;
    const auto proof = RecoverySourceIdentity::capture(source, facts);
    good(proof.status, "capture actual source proof after explicit mutation work");
    require(proof.evidence && proof.payloadBytesRead == 3 * 65536 && proof.fullBytesRead == 0
        && proof.evidence->bindsTo(source), "captured bounded sparse proof binds to original source facts");
    const auto evidence = *proof.evidence;
    require(evidence.facts.frames != snapshot.state->info.numSamples,
        "fixture distinguishes source dimensions from already edited current audio");
    RecoveryStore store(root.getChildFile("source-bound-checkpoint"), juce::Uuid());
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encodeWithSourceEvidence(snapshot, evidence, {}, packet, limits),
        "persist source facts rather than requiring them to equal edited dimensions");
    require(packet.planSchema == 2 && packet.stateJson.contains("source_evidence"), "source-bearing checkpoint declares explicit schema2");
    SafeHead head;
    good(store.append({}, packet, head), "durably bind source proof and exact audio graph in one hashed record");
    PreparedRecoveryCheckpoint prepared;
    auto manifest = store.inspectManifest();
    good(manifest.status, "read source-bearing manifest without audio allocation");
    good(RecoveryCheckpointCodec::prepare(manifest.commits.back(), prepared, limits), "prepare source-bearing checkpoint");
    require(prepared.getSourceEvidence() && juce::JSON::toString(prepared.getSourceEvidence()->toJson(), true)
        == juce::JSON::toString(evidence.toJson(), true), "full source evidence survives durable record and semantic preparation");
    const auto priorStates = prepared.getStates();
    const auto absentBranch = root.getChildFile("source-observer-never-created");
    const auto absent = RecoverySourceIdentity::observe(absentBranch.getChildFile("missing.wav"));
    good(absent.status, "known absent ordinary parent path can be classified without creating it");
    require(!absent.observation.exists && !absentBranch.exists(), "metadata observation does not activate missing directories");
#if JUCE_WINDOWS
    const auto link = root.getChildFile("source-observer-junction"), target = root.getChildFile("source-observer-target");
    installOwnedJunction(link, target, root);
    require(wave.copyFileTo(target.getChildFile("existing.wav")), "place only an owned file behind test junction");
    const auto linkedExisting = RecoverySourceIdentity::observe(link.getChildFile("existing.wav"));
    const auto linkedMissing = RecoverySourceIdentity::observe(link.getChildFile("missing.wav"));
    require(RemoveDirectoryW(link.getFullPathName().toWideCharPointer()) != FALSE, "remove only junction itself without traversing target");
    require(!linkedExisting.status && !linkedMissing.status
        && linkedMissing.status.code == "source_parent_unavailable_or_reparse" && target.getChildFile("existing.wav").existsAsFile(),
        "existing reparse parent is rejected even when its requested leaf is absent");
#endif

    auto bindingSource = source; bindingSource.location.file = root.getChildFile("wrong-binding.wav");
    auto invalidSnapshot = snapshot; invalidSnapshot.identity.source = bindingSource;
    CommitPacket unchanged = packet;
    require(RecoveryCheckpointCodec::encodeWithSourceEvidence(invalidSnapshot, evidence, {}, unchanged, limits).code == "checkpoint_source_binding"
        && unchanged.stateJson == packet.stateJson, "mismatched source proof cannot overwrite prior complete output");
    std::atomic<bool> cancel { true };
    require(RecoveryCheckpointCodec::encodeWithSourceEvidence(snapshot, evidence, {}, unchanged, limits,
        [&] { return cancel.load(); }).error == Error::cancelled && unchanged.stateJson == packet.stateJson,
        "cancelled source-bearing capture preserves prior output");

    std::unique_ptr<RecoverySourceBinding> exact;
    good(RecoverySourceBinding::resolve(source, prepared.getSourceEvidence(), {}, exact), "resolve persisted proof through real bounded source probe");
    require(exact->getResolution().confidence == SourceConfidence::exact && !exact->requiresSaveAs()
        && exact->payloadBytesRead() == 3 * 65536 && exact->fullBytesRead() == 0, "real Exact decision does not compute a whole-file hash");
    good(exact->check(source), "unchanged physical source passes within-attempt metadata fence");
    require(exact->check(bindingSource).error == Error::sourceConflict, "source-binding replacement invalidates existing resolver owner");
    const auto oldExact = exact.get();
    require(RecoverySourceBinding::resolve(source, evidence, {}, exact, &cancel).error == Error::cancelled && exact.get() == oldExact,
        "pre-cancelled resolution retains prior complete binding");

    const auto observation = RecoverySourceIdentity::observe(wave);
    good(observation.status, "observe actual physical source without audio decode");
    require(observation.observation.exists && observation.observation.changedTicks > 0, "source observation includes physical change time");
    const auto scratch = root.getChildFile("source-bound-candidate-scratch");
    require(scratch.createDirectory().wasOk(), "create owned source-bound candidate scratch");
    RecoveryRestoreCandidate::Request restoreRequest;
    restoreRequest.observedOwnerHead = head; restoreRequest.checkpointHead = head;
    restoreRequest.scratchDirectory = scratch; restoreRequest.limits = limits;
    restoreRequest.attemptDirectory = root.getChildFile("source-bound-attempts"); restoreRequest.appBuild = "source-tests-v1";
    {
        RecoveryRestoreCandidate candidate(store, restoreRequest);
        for (int i = 0; i < 16 && candidate.getStage() != RecoveryRestoreCandidate::Stage::coreReady; ++i)
            good(candidate.advance(), "actual candidate invokes its built-in persisted source resolver");
        require(candidate.getStage() == RecoveryRestoreCandidate::Stage::coreReady && candidate.getSourceResolution()
            && candidate.getSourceResolution()->confidence == SourceConfidence::exact, "candidate resolves Exact without a test source callback");
        // Change bytes outside all three sparse windows, then restore exact size
        // and LastWriteTime. ChangeTime must still invalidate the active attempt.
        constexpr int64_t offset = 44 + 100000;
        char oldByte = 0;
        {
            juce::FileInputStream input(wave); require(input.openedOk() && input.setPosition(offset), "read private changed-byte fixture");
            oldByte = input.readByte();
        }
        {
            juce::FileOutputStream output(wave);
            require(output.openedOk() && output.setPosition(offset) && output.writeByte(static_cast<char>(oldByte ^ 0x7f)), "change owned source outside sparse windows");
            output.flush(); require(output.getStatus().wasOk(), "flush private source mutation");
        }
        restoreWriteTime(wave, evidence.modifiedTicks);
        const auto changed = RecoverySourceIdentity::observe(wave);
        good(changed.status, "observe restamped source");
        require(changed.observation.modifiedTicks == observation.observation.modifiedTicks
            && changed.observation.sizeBytes == observation.observation.sizeBytes
            && changed.observation.changedTicks != observation.observation.changedTicks, "restored mtime does not restore physical ChangeTime");
        require(exact->check(source).error == Error::sourceConflict, "actual observer detects intervening edit even with unchanged mtime/size");
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        require(candidate.acquireCore(candidate.getAttemptId(), acquired).error == Error::sourceConflict && !acquired,
            "actual candidate rejects restamped source mutation before playlist handoff");
        {
            juce::FileOutputStream output(wave);
            require(output.openedOk() && output.setPosition(offset) && output.writeByte(oldByte), "restore only private fixture byte");
            output.flush();
        }
        restoreWriteTime(wave, evidence.modifiedTicks);
    }
    require(scratch.findChildFiles(juce::File::findFiles, false).isEmpty(), "source-conflicted candidate retires owned scratch");

    const auto moved = root.getChildFile("bound-source-relocated.wav");
    require(wave.moveFileTo(moved), "move only the private source fixture on the same volume");
    std::unique_ptr<RecoverySourceBinding> missing, relocated, confirmed, legacy;
    good(RecoverySourceBinding::resolve(source, evidence, {}, missing), "known absent original is a Missing decision");
    require(missing->getResolution().confidence == SourceConfidence::missing && missing->requiresSaveAs(), "Missing cannot grant source replay permission");
    RecoverySourceBinding::Request locate; locate.locatedFile = moved;
    good(RecoverySourceBinding::resolve(source, evidence, locate, relocated), "resolve explicitly located same physical source");
    require(relocated->getResolution().confidence == SourceConfidence::relocated && relocated->requiresSaveAs(), "Relocated requires explicit confirmation");
    locate.relocationConfirmed = true;
    good(RecoverySourceBinding::resolve(source, evidence, locate, confirmed), "confirm known relocation for future guarded Save consumer");
    require(!confirmed->requiresSaveAs(), "confirmation permits source-base replay classification only after strong match");
    good(RecoverySourceBinding::resolve(source, {}, locate, legacy), "legacy no-proof source can still support a detached self-contained copy");
    require(legacy->getResolution().confidence == SourceConfidence::ambiguous && legacy->requiresSaveAs(), "confirmation never upgrades missing evidence");
    require(moved.copyFileTo(wave), "create private same-content replacement at historical locator");
    restoreWriteTime(wave, evidence.modifiedTicks);
    require(missing->check(source).error == Error::sourceConflict, "appearance at previously missing path invalidates frozen decision");
    std::unique_ptr<RecoverySourceBinding> replacement;
    good(RecoverySourceBinding::resolve(source, evidence, {}, replacement), "classify real same-name file replacement");
    require(replacement->getResolution().confidence == SourceConfidence::changed && replacement->requiresSaveAs(),
        "same bytes/name/time on a different file ID never become Exact");
    const auto verified = store.inspect(); good(verified.status, "certify source-bearing checkpoint after real source replacement");
    Snapshot decoded;
    good(RecoveryCheckpointCodec::decode(verified.commits.back(), store, decoded, limits),
        "schema2 self-contained decode does not depend on currently matching source bytes");
    require(decoded.identity.source.sameBinding(source) && decoded.undo.size() == snapshot.undo.size()
        && decoded.redo.size() == snapshot.redo.size(), "schema2 full decode retains original binding and complete history");
    std::vector<SingleTrackDocument::StatePtr> expectedStates { snapshot.state }, actualStates { decoded.state };
    expectedStates.insert(expectedStates.end(), snapshot.undo.begin(), snapshot.undo.end());
    expectedStates.insert(expectedStates.end(), snapshot.redo.begin(), snapshot.redo.end());
    actualStates.insert(actualStates.end(), decoded.undo.begin(), decoded.undo.end());
    actualStates.insert(actualStates.end(), decoded.redo.begin(), decoded.redo.end());
    for (size_t i = 0; i < expectedStates.size(); ++i)
    {
        const auto& expected = *expectedStates[i]; const auto& actual = *actualStates[i];
        require(actual.audioBuffer && actual.info.numSamples == expected.info.numSamples
            && actual.info.numChannels == expected.info.numChannels, "schema2 decoded PCM dimensions match each history state");
        for (int channel = 0; channel < expected.info.numChannels; ++channel)
            require(std::memcmp(actual.audioBuffer->getReadPointer(channel), expected.audioBuffer->getReadPointer(channel),
                static_cast<size_t>(expected.info.numSamples) * sizeof(float)) == 0, "schema2 current/Undo/Redo audio remains bit-exact after source replacement");
    }

    auto movedStamp = source; movedStamp.location.file = moved;
    const auto deep = RecoverySourceIdentity::capture(movedStamp, facts, RecoverySourceIdentity::Mode::explicitDeepVerify);
    good(deep.status, "explicit deep verification of owned relocated source");
    auto deepEvidence = *deep.evidence; deepEvidence.location = wave; // Bind recorded original path, retaining physical ID/hash.
    std::unique_ptr<RecoverySourceBinding> weakDeep, confirmedDeep;
    good(RecoverySourceBinding::resolve(source, deepEvidence, locate, weakDeep), "sparse resolution respects an existing full-hash requirement");
    require(weakDeep->getResolution().confidence == SourceConfidence::ambiguous && weakDeep->requiresSaveAs(), "full-hash proof cannot silently downgrade to sparse Exact");
    locate.verification = RecoverySourceIdentity::Mode::explicitDeepVerify;
    good(RecoverySourceBinding::resolve(source, deepEvidence, locate, confirmedDeep), "explicit deep request can resolve full-hash evidence");
    require(confirmedDeep->getResolution().confidence == SourceConfidence::relocated && !confirmedDeep->requiresSaveAs()
        && confirmedDeep->fullBytesRead() == source.sizeBytes, "deep resolution hashes exact full fixture only when requested");

    const auto checkBad = [&] (CommitPacket bad, const char* expectedCode)
    {
        RecoveryStore corrupt(root.getChildFile("source-proof-invalid"), juce::Uuid());
        SafeHead badHead; good(corrupt.append({}, bad, badHead), "publish checksum-valid semantic source-proof fault");
        const auto badView = corrupt.inspectManifest(); good(badView.status, "fault retains valid record framing/hash");
        require(RecoveryCheckpointCodec::prepare(badView.commits.back(), prepared, limits).code == expectedCode
            && prepared.getStates() == priorStates, "semantic proof fault preserves earlier complete preparation");
    };
    auto bad = packet; auto graph = juce::JSON::parse(bad.stateJson);
    graph["source_evidence"].getDynamicObject()->setProperty("schema", 99); bad.stateJson = juce::JSON::toString(graph, true);
    checkBad(bad, "checkpoint_source_evidence");
    bad = packet; graph = juce::JSON::parse(bad.stateJson);
    graph["source_evidence"].getDynamicObject()->setProperty("path", moved.getFullPathName()); bad.stateJson = juce::JSON::toString(graph, true);
    checkBad(bad, "checkpoint_source_binding");
    bad = packet; graph = juce::JSON::parse(bad.stateJson);
    graph.getDynamicObject()->removeProperty("source_evidence"); bad.stateJson = juce::JSON::toString(graph, true);
    checkBad(bad, "checkpoint_schema");
    good(store.inspect().status, "source changes and proof fault trials leave original durable checkpoint intact");
    std::cout << "RecoverySourceBindingCheckpointTests passed (durable proof, real resolver/candidate, change-time, relocation/deep policy)\n";
}
