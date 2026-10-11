#include "Recovery/RecoveryRestoreCandidate.h"
#include "Audio/AudioEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <cstring>
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
using Candidate = RecoveryRestoreCandidate;
using Stage = Candidate::Stage;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void good(Status value, const char* message)
{
    if (!value) std::cerr << "Candidate status: " << value.code << '\n';
    require(static_cast<bool>(value), message);
}
void reach(Candidate& candidate, Stage target)
{
    for (int i = 0; i < 256 && candidate.getStage() != target; ++i)
        good(candidate.advance(), "advance real candidate within bounded fixture");
    require(candidate.getStage() == target, "candidate reaches requested lifecycle stage");
}
void peaksMatch(const WaveformModel::PeakCache& cache, const SingleTrackDocument::State& expected)
{
    const auto count = cache.loadedSampleRange.getEnd();
    require(count > 0 && count <= expected.info.numSamples && cache.channelCount == expected.info.numChannels,
        "candidate peaks cover real audio only");
    for (int channel = 0; channel < cache.channelCount; ++channel)
        for (int bin = 0; bin < cache.binCount; ++bin)
        {
            const auto& peak = cache.channelBins[static_cast<size_t>(channel)][static_cast<size_t>(bin)];
            // Sample-to-column membership is floor(sample * bins / frames).
            // Pixel time boundaries are continuous; rounding them to samples
            // would invent a different partition for non-divisible lengths.
            const auto start = (static_cast<int64_t>(bin) * count + cache.binCount - 1) / cache.binCount;
            const auto end = (static_cast<int64_t>(bin + 1) * count + cache.binCount - 1) / cache.binCount;
            require(start >= 0 && end > start && end <= count, "peak time extents end at verified boundary");
            require(peak.startSeconds >= 0 && peak.endSeconds >= peak.startSeconds
                && peak.endSeconds <= count / expected.info.sampleRate + 1.0e-12, "continuous bin times stay within verified audio");
            auto minimum = std::numeric_limits<float>::infinity(), maximum = -minimum;
            for (auto i = start; i < end; ++i)
            {
                const auto sample = expected.audioBuffer->getSample(channel, static_cast<int>(i));
                minimum = std::min(minimum, sample); maximum = std::max(maximum, sample);
            }
            if (std::abs(peak.minValue - minimum) >= 1.0e-6f || std::abs(peak.maxValue - maximum) >= 1.0e-6f)
                std::cerr << "Peak mismatch: preview=" << cache.isPreview << " frames=" << count << " bins=" << cache.binCount
                    << " channel=" << channel << " bin=" << bin << " span=" << start << ':' << end
                    << " actual=" << peak.minValue << ':' << peak.maxValue << " expected=" << minimum << ':' << maximum << '\n';
            require(std::abs(peak.minValue - minimum) < 1.0e-6f && std::abs(peak.maxValue - maximum) < 1.0e-6f,
                "actual candidate peak extrema match independent PCM oracle");
        }
}
}

void runRecoveryRestoreCandidateTests(const Snapshot& original, const juce::File& root, AudioGraphLimits limits)
{
    {
        RecoveryStore protectedStore(root.getChildFile("candidate-compaction-lease"), juce::Uuid());
        CommitPacket complete;
        good(RecoveryCheckpointCodec::encode(original, {}, complete, limits), "encode selected-point compaction fixture");
        SafeHead first, latest;
        good(protectedStore.append({}, complete, first), "publish selected earlier recovery point");
        good(protectedStore.append(first, complete, latest), "publish newer full recovery point");
        Candidate::Request selected;
        selected.observedOwnerHead = latest; selected.checkpointHead = first;
        selected.scratchDirectory = root.getChildFile("candidate-compaction-scratch");
        selected.attemptDirectory = root.getChildFile("candidate-compaction-attempts");
        selected.limits = limits; selected.appBuild = "candidate-compaction-tests";
        Candidate preparing(protectedStore, selected);
        good(preparing.advance(), "start real earlier-point restore attempt");
        good(preparing.advance(), "prepare real earlier-point metadata before reader creation");
        require(preparing.getStage() == Stage::resolvingSource, "candidate has not created progressive audio readers yet");
        OrphanCollectionReport report;
        require(RecoveryCheckpointCodec::compact(protectedStore, latest, report, limits).code == "owner_in_use"
            && protectedStore.inspectManifest().commits.size() == 2,
            "real Restore Copy preparation lease prevents deleting the selected earlier point");
        preparing.requestCancel();
        require(preparing.advance().error == Error::cancelled, "cancel selected-point restore without publishing a document");
        good(RecoveryCheckpointCodec::compact(protectedStore, latest, report, limits), "cancelled preparation releases its real compaction lease");
        require(protectedStore.inspectManifest().commits.size() == 1, "only the surviving latest complete history remains after cancellation and cut");
        Candidate obsolete(protectedStore, selected);
        good(obsolete.advance(), "start a stale earlier-point selection after legitimate compaction");
        require(obsolete.advance().error == Error::staleHead && obsolete.getStatus().code == "candidate_checkpoint_pruned",
            "an expired earlier-point selection reports stale history rather than corrupt recovery data");
    }
    const auto sourceFile = root.getChildFile("candidate-owned-source.txt");
    require(sourceFile.replaceWithText("Owned source fence fixture, not a playable WAV."), "create private resolver evidence fixture");
    juce::MemoryBlock sourceBytes;
    require(sourceFile.loadFileAsData(sourceBytes), "freeze private source bytes");
    auto snapshot = original;
    snapshot.identity.source.location.file = sourceFile;
    snapshot.identity.source.sizeBytes = sourceFile.getSize();
    snapshot.identity.source.modifiedMilliseconds = sourceFile.getLastModificationTime().toMilliseconds();
    snapshot.identity.source.fastStampValid = true;
    const auto relocateMetadata = [&] (SingleTrackDocument::StatePtr state)
    {
        auto copy = std::make_shared<SingleTrackDocument::State>(*state); copy->info.file = sourceFile;
        return SingleTrackDocument::StatePtr(copy);
    };
    snapshot.state = relocateMetadata(snapshot.state);
    for (auto& state : snapshot.undo) state = relocateMetadata(state);
    for (auto& state : snapshot.redo) state = relocateMetadata(state);
    const auto savedIdentity = snapshot.identity.document;
    RecoveryStore store(root.getChildFile("candidate-store"), juce::Uuid());
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encode(snapshot, {}, packet, limits), "encode complete real candidate checkpoint");
    SafeHead head;
    good(store.append({}, packet, head), "publish candidate checkpoint");
    const auto checkpoint = head;
    const auto scratch = root.getChildFile("candidate-scratch");
    require(scratch.createDirectory().wasOk(), "create private candidate scratch directory");
    const auto request = [&]
    {
        Candidate::Request value;
        value.observedOwnerHead = head; value.checkpointHead = checkpoint;
        value.scratchDirectory = scratch; value.limits = limits;
        value.attemptDirectory = root.getChildFile("candidate-attempts"); value.appBuild = "candidate-tests-v1";
        return value;
    };
    const auto publishLater = [&]
    {
        packet.timestampMilliseconds += 1;
        SafeHead next;
        good(store.append(head, packet, next), "publish later private safe point");
        head = next;
    };
    const auto emptyScratch = [&] { return scratch.findChildFiles(juce::File::findFiles, false).isEmpty(); };
    const auto inspectAttempt = [&] (const Candidate& candidate)
    {
        const auto view = RecoveryAttemptStore(request().attemptDirectory, store.ownerId()).inspect();
        good(view.status, "read actual candidate durable marker through independent inspector");
        const auto found = std::find_if(view.attempts.begin(), view.attempts.end(),
            [&] (const auto& item) { return item.record.candidate == candidate.getAttemptId(); });
        require(found != view.attempts.end() && candidate.getAttemptRecord() && found->record == *candidate.getAttemptRecord(),
            "disk marker exactly matches reported candidate record");
        return *found;
    };
    {
        auto invalid = request(); invalid.previewBins = 0;
        Candidate candidate(store, std::move(invalid));
        require(candidate.getStage() == Stage::queued && emptyScratch(), "candidate construction does no restore I/O");
        require(candidate.advance().error == Error::invalidPacket && candidate.getStage() == Stage::failed && emptyScratch(),
            "invalid candidate geometry fails before activation");
        Candidate cancelled(store, request()); cancelled.requestCancel();
        require(cancelled.advance().error == Error::cancelled && emptyScratch(), "queued cancellation creates no readers or peaks");
        require(!request().attemptDirectory.exists() && !candidate.getAttemptRecord() && !cancelled.getAttemptRecord(),
            "invalid/early-cancelled candidate performs no marker I/O");
        auto overlap = request(); overlap.attemptDirectory = store.rootDirectory().getChildFile("attempts");
        Candidate overlapping(store, overlap);
        require(overlapping.advance().code == "candidate_attempt_directory" && !overlap.attemptDirectory.exists(),
            "marker cannot be placed inside journal/CAS");
        auto unavailable = request(); unavailable.attemptDirectory = root.getChildFile("candidate-blocked-markers");
        require(unavailable.attemptDirectory.replaceWithText("private directory obstruction"), "create owned marker-directory fault");
        Candidate blocked(store, unavailable);
        require(!blocked.advance() && blocked.getFailureStage() == Stage::queued && !blocked.getAttemptRecord() && emptyScratch(),
            "marker write failure stops before journal parser or audio materialization");
        auto capped = request(); capped.attemptDirectory = root.getChildFile("candidate-budgeted-markers");
        capped.attemptLimits.rootBytes = 1;
        Candidate budgeted(store, capped);
        require(budgeted.advance().code == "global_byte_budget" && budgeted.getFailureStage() == Stage::queued
            && !budgeted.getAttemptRecord() && emptyScratch() && store.inspect().head == head
            && !capped.attemptDirectory.getChildFile(store.ownerId().toString()).exists(),
            "actual candidate forwards attempt quota and stops before owner infrastructure or audio work");
    }
    {
        auto limited = request(); limited.attemptDirectory = root.getChildFile("candidate-stage-budget");
        limited.attemptLimits.rootBytes = 32768;
        Candidate candidate(store, limited);
        good(candidate.advance(), "admit actual candidate before later metadata pressure");
        const auto originalRecord = *candidate.getAttemptRecord();
        int64_t used = 0;
        for (const auto& file : limited.attemptDirectory.findChildFiles(juce::File::findFiles, true)) used += file.getSize();
        require(used > 0 && used < limited.attemptLimits.rootBytes, "candidate marker has bounded actual disk footprint");
        const auto filler = limited.attemptDirectory.getChildFile("unknown-capacity-fixture");
        juce::MemoryBlock bytes(static_cast<size_t>(limited.attemptLimits.rootBytes - used), true);
        require(filler.replaceWithData(bytes.getData(), bytes.getSize()), "fill only independent owned attempt root");
        require(candidate.advance().code == "global_byte_budget" && candidate.getStage() == Stage::failed
            && candidate.getAttemptCloseStatus().code == "global_byte_budget" && *candidate.getAttemptRecord() == originalRecord
            && emptyScratch() && store.inspect().head == head,
            "real candidate stage and terminal marker refusal retain old record and report unclosed attempt without audio admission");
        const auto observed = RecoveryAttemptStore(limited.attemptDirectory, store.ownerId(), limited.attemptLimits).inspect();
        good(observed.status, "inspect actual candidate after failed terminal publication releases lease");
        require(observed.attempts.size() == 1 && observed.attempts[0].record == originalRecord
            && observed.attempts[0].health == AttemptHealth::suspect && filler.existsAsFile(),
            "failed close remains honestly unclosed and preserves unknown bytes");
        require(filler.deleteFile(), "remove only known private capacity fixture before explicit new retry");
        Candidate retry(store, limited);
        good(retry.advance(), "actual explicit fresh candidate retries after real capacity relief");
        require(retry.getAttemptRecord()->nonce != originalRecord.nonce, "new attempt preserves distinct durable identity");
        retry.requestCancel();
        require(retry.advance().error == Error::cancelled && retry.getAttemptCloseStatus(), "fresh retry can close normally after relief");
    }
    {
        Candidate candidate(store, request());
        const auto commit = store.rootDirectory().getChildFile(store.ownerId().toString()).getChildFile("commits").getChildFile(checkpoint.hash);
        const auto held = commit.getSiblingFile(checkpoint.hash + "-held");
        require(commit.moveFileTo(held), "hide only owned checkpoint body before initial marker");
        good(candidate.advance(), "initial marker requires no journal/CAS parse");
        const auto initial = inspectAttempt(candidate);
        require(initial.health == AttemptHealth::active && initial.record.stage == Stage::validating && emptyScratch(),
            "flushed attempt exists before unavailable journal body is parsed");
        require(candidate.advance().error == Error::corruptJournal && candidate.getFailureStage() == Stage::validating,
            "actual unavailable record fails during manifest validation");
        require(held.moveFileTo(commit), "restore owned checkpoint body");
        const auto failure = inspectAttempt(candidate);
        require(failure.record.outcome == AttemptOutcome::failed && failure.record.failure == AttemptFailure::corruptJournal
            && failure.record.stage == Stage::validating && candidate.getAttemptCloseStatus(), "real parser fault durably closes at last inflight stage");
    }
#if JUCE_WINDOWS
    {
        auto isolated = request(); isolated.appBuild = "candidate-close-fault";
        Candidate candidate(store, isolated); good(candidate.advance(), "start terminal failure fixture");
        const auto record = *candidate.getAttemptRecord();
        const auto marker = isolated.attemptDirectory.getChildFile(store.ownerId().toString()).getChildFile(record.nonce.toString() + ".attempt");
        const auto held = CreateFileW(marker.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(held != INVALID_HANDLE_VALUE, "hold only candidate-owned marker against atomic replace");
        candidate.requestCancel(); const auto cancelled = candidate.advance();
        CloseHandle(held);
        require(cancelled.error == Error::cancelled && !candidate.getAttemptCloseStatus(),
            "original cancellation and secondary terminal persistence fault are both observable");
        const auto interrupted = inspectAttempt(candidate);
        require(interrupted.health == AttemptHealth::suspect && interrupted.record.outcome == AttemptOutcome::active,
            "failed candidate releases live lease and exposes honestly unclosed marker");
    }
#endif
    {
        Candidate candidate(store, request());
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        require(!candidate.acquireCore(candidate.getAttemptId(), acquired) && !acquired, "queued metadata is not CoreReady");
        reach(candidate, Stage::preparingCore);
        require(candidate.getSourceResolution() && candidate.getSourceResolution()->confidence == SourceConfidence::ambiguous,
            "legacy checkpoint without evidence receives actual Ambiguous classification without an injected resolver");
        require(!candidate.acquireCore(candidate.getAttemptId(), acquired) && !acquired, "allocated zero-ready mappings cannot attach");
        good(candidate.advance(), "prepare real first audio and waveform together");
        require(candidate.getStage() == Stage::coreReady, "first actual candidate CoreReady transition");
        require(candidate.acquireCore(juce::Uuid(), acquired).code == "candidate_attempt_mismatch" && !acquired
            && candidate.getStage() == Stage::coreReady, "late UI token cannot acquire or cancel another attempt");
        good(candidate.acquireCore(candidate.getAttemptId(), acquired), "handoff real dirty candidate once");
        auto held = acquired;
        require(candidate.acquireCore(candidate.getAttemptId(), acquired).code == "candidate_already_acquired" && acquired == held,
            "duplicate handoff preserves output and cannot create a second playlist item");
        require(acquired->document->hasUnsavedChanges() && acquired->document->requiresRecoverySaveAs()
            && !acquired->document->getMediaIdentity().document.sameDocument(savedIdentity), "candidate has independent dirty identity and detached Save As");
        auto history = acquired->document->captureCommittedState(acquired->document->getMediaIdentity().document);
        require(history && history->undo.size() == snapshot.undo.size() && history->redo.size() == snapshot.redo.size(),
            "candidate keeps complete actual history at first admission");
        peaksMatch(*acquired->firstPeaks, *snapshot.state);
        AudioEngine engine(*acquired->document);
        juce::AudioBuffer<float> samples(snapshot.state->info.numChannels, 32);
        require(engine.renderDocumentBlockForDiagnostics(32, samples), "candidate is playable through actual production AudioEngine");
        for (int c = 0; c < samples.getNumChannels(); ++c)
            for (int i = 0; i < samples.getNumSamples(); ++i)
            {
                const auto gain = SingleTrackDocument::evaluateEnvelope(snapshot.state->envelopePoints, (i + 32) / snapshot.state->info.sampleRate);
                require(std::abs(samples.getSample(c, i) - snapshot.state->audioBuffer->getSample(c, i + 32) * gain) < 1.0e-6f,
                    "candidate playback never reopens historical source bytes");
            }
        require(!engine.getRuntimeMetricsForDiagnostics().has_value(), "candidate opens no audio device");
        acquired->document->resetEnvelope();
        const auto edited = acquired->document->getState();
        require(edited != acquired->coreState && SingleTrackDocument::isEnvelopeFlatUnity(edited->envelopePoints),
            "user can edit prepared candidate while remaining audio materializes");
        reach(candidate, Stage::ready);
        const auto completeAttempt = inspectAttempt(candidate);
        require(completeAttempt.record.outcome == AttemptOutcome::succeeded && completeAttempt.record.stage == Stage::ready
            && completeAttempt.health == AttemptHealth::healthy, "actual full audio/peaks completion closes success marker");
        require(acquired->document->getState() == edited && acquired->document->hasUnsavedChanges(),
            "completion never replaces a user-edited document or clears dirty state");
        const auto full = candidate.getFullPeaks();
        require(full && !full->isPreview && full->state == acquired->coreState && full->state != edited
            && full->loadedSampleRange.getEnd() == snapshot.state->info.numSamples, "full peaks retain original generation ownership and complete coverage");
        peaksMatch(*full, *snapshot.state);
        int cancellationPolls = 0;
        require(!WaveformModel::buildCompletePeakCacheForState(acquired->coreState, [&] { return ++cancellationPolls >= 2; })
            && cancellationPolls >= 2, "explicit full waveform preparation honours cancellation inside its actual scan");
        require(full->binCount < 512 || !full->lodLevels.empty(), "complete peak preparation builds its normal LOD pyramid");
        require(snapshot.identity.document.sameVersion(savedIdentity) && snapshot.state->audioBuffer == original.state->audioBuffer,
            "independent restoration never modifies original document snapshot");
    }
    require(emptyScratch(), "successful candidate and playback retirement releases exact scratch files");
    {
        Candidate candidate(store, request()); reach(candidate, Stage::coreReady);
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        good(candidate.acquireCore(candidate.getAttemptId(), acquired), "acquire actual partial candidate for re-protection");
        const auto readyBefore = acquired->coreState->getPlayableSampleRange();
        require(readyBefore.getEnd() < acquired->coreState->info.numSamples, "re-protection fixture is genuinely partial");
        acquired->document->resetEnvelope();
        auto captured = *acquired->document->captureCommittedState(acquired->document->getMediaIdentity().document);
        require(std::any_of(captured.undo.begin(), captured.undo.end(), [] (const auto& state) { return state->getPlayableSampleRange().isEmpty(); }),
            "actual retained history contains unprepared audio");
        RecoveryStore protectedCopy(root.getChildFile("candidate-reprotected"), juce::Uuid());
        AudioGraphReuseCache reuse(protectedCopy.rootDirectory(), protectedCopy.ownerId());
        CommitPacket first;
        good(RecoveryCheckpointCodec::encode(captured, {}, first, limits, {}, &reuse), "capture full durable graph from partial recovered edit without reading mapping");
        require(reuse.lastCaptureWork().casBlocksRead > 0 && reuse.lastCaptureWork().reusedSequences == 0,
            "first recovered import independently reads original CAS bodies");
        require(acquired->coreState->getPlayableSampleRange() == readyBefore, "re-protection does not materialize live prefix or invent readiness");
        SafeHead protectedHead;
        std::shared_ptr<const PublishedRecoveryCommit> publication;
        good(protectedCopy.append({}, first, protectedHead, &publication), "new owner durably imports full recovery dependency closure");
        good(reuse.acknowledge(publication), "certify new owner's imported graph for later edits");
        const auto firstView = protectedCopy.inspect(); good(firstView.status, "new owner verifies imported assets");
        std::set<juce::String> known;
        for (const auto& id : *firstView.commits.back().record["assets"].getArray()) known.insert(id.toString());
        require(acquired->document->moveEnvelopePoint(0, 0.0, 0.6f), "commit another actual envelope change during progressive recovery");
        captured = *acquired->document->captureCommittedState(acquired->document->getMediaIdentity().document);
        CommitPacket second;
        good(RecoveryCheckpointCodec::encode(captured, known, second, limits, {}, &reuse), "recapture later mutation with published destination graph");
        require(reuse.lastCaptureWork().reusedSequences > 0 && reuse.lastCaptureWork().casBlocksRead == 0
            && reuse.lastCaptureWork().sampleBytesReadOrCopied == 0, "later recovered edit does not re-read old owner's CAS or incomplete mapping");
        require(second.newAudio.empty() && !second.existingAudio.empty() && second.stateJson != first.stateJson,
            "later actual envelope commit adds changed metadata/history without duplicating PCM assets");
        auto tight = limits; tight.chunkSampleBytes = 256;
        auto unchangedBudget = second;
        require(RecoveryCheckpointCodec::encode(captured, known, unchangedBudget, tight).error == Error::budgetExceeded
            && unchangedBudget.stateJson == second.stateJson, "recapture applies caller chunk bounds before reading referenced bodies");
        SafeHead next;
        good(protectedCopy.append(protectedHead, second, next, &publication), "append later protected generation to new owner");
        good(reuse.acknowledge(publication), "advance imported graph only after second publication");
        const auto assetDirectory = protectedCopy.rootDirectory().getChildFile(protectedCopy.ownerId().toString()).getChildFile("assets");
        require(static_cast<size_t>(assetDirectory.findChildFiles(juce::File::findFiles, false).size()) == known.size(),
            "actual destination asset count stays unchanged after second metadata-only edit");
        PreparedRecoveryCheckpoint originalPlan;
        good(RecoveryCheckpointCodec::prepare(store.inspectManifest().commits.front(), originalPlan, limits), "read original independent audio oracle mapping");
        std::vector<SingleTrackDocument::StatePtr> originals { snapshot.state };
        originals.insert(originals.end(), snapshot.undo.begin(), snapshot.undo.end());
        originals.insert(originals.end(), snapshot.redo.begin(), snapshot.redo.end());
        const auto sameSequence = [] (const auto& a, const auto& b)
        {
            if (a.frames != b.frames || a.channels != b.channels || a.sampleRate != b.sampleRate || a.chunks.size() != b.chunks.size()) return false;
            for (size_t i = 0; i < a.chunks.size(); ++i)
                if (a.chunks[i].id != b.chunks[i].id || a.chunks[i].frames != b.chunks[i].frames) return false;
            return true;
        };
        std::vector<SingleTrackDocument::StatePtr> capturedStates { captured.state };
        capturedStates.insert(capturedStates.end(), captured.undo.begin(), captured.undo.end());
        capturedStates.insert(capturedStates.end(), captured.redo.begin(), captured.redo.end());
        std::vector<std::shared_ptr<juce::AudioBuffer<float>>> oracle;
        for (const auto& state : capturedStates)
        {
            const auto reference = ProgressiveRecoveryAudio::getDurableReference(state->streamingReader);
            require(reference != nullptr, "actual restored history retains private immutable CAS provenance");
            std::shared_ptr<juce::AudioBuffer<float>> expected;
            for (size_t i = 0; i < originals.size(); ++i)
                if (sameSequence(reference->sequence, originalPlan.getAudioGraph().sequences[originalPlan.getAudioGraph().states[i]]))
                    expected = originals[i]->audioBuffer;
            require(expected != nullptr, "each recovered backing maps to original independent in-memory PCM oracle");
            oracle.push_back(expected);
        }
        // Once imported, new owner's durable point is independently restorable.
        // Live usage pins the owner itself. Inject unavailable old CAS by hiding
        // only its assets directory; this is a fault, not a GC authorization.
        const auto originalOwner = store.rootDirectory().getChildFile(store.ownerId().toString());
        require(!originalOwner.moveFileTo(originalOwner.getSiblingFile("held-original-owner")),
            "actual live recovered dependency prevents moving its owner directory");
        const auto originalDirectory = originalOwner.getChildFile("assets");
        const auto hiddenDirectory = originalDirectory.getSiblingFile("held-original-assets");
        require(originalDirectory.moveFileTo(hiddenDirectory), "make only old private CAS unavailable after new publication");
        const auto copyView = protectedCopy.inspect(); good(copyView.status, "new durable owner verifies with original CAS unavailable");
        Snapshot restored;
        good(RecoveryCheckpointCodec::decode(copyView.commits.back(), protectedCopy, restored, limits), "restore latest protected edit independent of original recovery owner");
        require(hiddenDirectory.moveFileTo(originalDirectory), "restore original owned fixture for pending candidate");
        std::vector<SingleTrackDocument::StatePtr> restoredStates { restored.state };
        restoredStates.insert(restoredStates.end(), restored.undo.begin(), restored.undo.end());
        restoredStates.insert(restoredStates.end(), restored.redo.begin(), restored.redo.end());
        require(restoredStates.size() == capturedStates.size() && restored.undo.size() == captured.undo.size()
            && restored.redo.size() == captured.redo.size() && restored.identity.document.sameVersion(captured.identity.document),
            "re-protection preserves actual complete ordered history and mutation identity");
        for (size_t i = 0; i < restoredStates.size(); ++i)
        {
            const auto& actual = *restoredStates[i]->audioBuffer;
            require(actual.getNumChannels() == oracle[i]->getNumChannels() && actual.getNumSamples() == oracle[i]->getNumSamples(), "recaptured PCM dimensions match oracle");
            for (int channel = 0; channel < actual.getNumChannels(); ++channel)
                require(std::memcmp(actual.getReadPointer(channel), oracle[i]->getReadPointer(channel), static_cast<size_t>(actual.getNumSamples()) * sizeof(float)) == 0,
                    "every current/Undo/Redo sample survives partial restore-edit-protect-restore bit-exactly");
            require(restoredStates[i]->envelopePoints.size() == capturedStates[i]->envelopePoints.size(), "recaptured envelope shape survives");
            for (size_t point = 0; point < restoredStates[i]->envelopePoints.size(); ++point)
                require(restoredStates[i]->envelopePoints[point].gain == capturedStates[i]->envelopePoints[point].gain
                    && restoredStates[i]->envelopePoints[point].timeSeconds == capturedStates[i]->envelopePoints[point].timeSeconds,
                    "every actual edited and historical envelope point survives");
        }
        // A missing unprepared asset cannot silently become a protected zero tail.
        const auto reference = ProgressiveRecoveryAudio::getDurableReference(captured.state->streamingReader);
        const auto tail = originalOwner.getChildFile("assets").getChildFile(reference->sequence.chunks.back().id);
        const auto heldTail = tail.getSiblingFile(tail.getFileName() + "-held");
        require(tail.moveFileTo(heldTail), "hide private unprepared source CAS tail");
        auto unchanged = second;
        const auto failed = RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits);
        require(heldTail.moveFileTo(tail), "restore private source CAS tail");
        require(!failed && unchanged.stateJson == second.stateJson && protectedCopy.inspect().head == next,
            "failed recapture preserves caller packet and last independently durable point");
        int polls = 0;
        require(RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits, [&] { return ++polls > 10; }).error == Error::cancelled
            && unchanged.stateJson == second.stateJson, "CAS recapture can cancel without publishing partial history");
        candidate.requestCancel(); require(candidate.advance().error == Error::cancelled, "explicitly stop remaining progressive work after re-protection tests");
        acquired.reset();
    }
    require(emptyScratch(), "re-protected candidate snapshots release only their owned scratch");
    {
        Candidate candidate(store, request()); reach(candidate, Stage::coreReady);
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        good(candidate.acquireCore(candidate.getAttemptId(), acquired), "acquire before cancellation");
        const auto prefix = acquired->coreState->getPlayableSampleRange();
        candidate.requestCancel();
        require(candidate.advance().error == Error::cancelled && candidate.getStage() == Stage::cancelled
            && acquired->coreState->getPlayableSampleRange() == prefix
            && !acquired->coreState->playbackAvailability.canAdvanceContiguousPrefix(), "cancel stops future work but preserves already attached verified audio");
        require(candidate.advance().error == Error::cancelled && !candidate.getFullPeaks(), "cancelled attempt never retries itself");
        require(inspectAttempt(candidate).record.outcome == AttemptOutcome::cancelled && candidate.getAttemptCloseStatus(),
            "actual core cancellation durably closes without a crash suspicion");
        good(store.inspect().status, "cancel never deletes durable protected checkpoint");
        require(!emptyScratch(), "attached reader retains its own scratch mapping");
        acquired.reset();
        require(emptyScratch(), "last attached reader retirement releases exact cancelled scratch");
    }
    {
        Candidate candidate(store, request()); reach(candidate, Stage::preparingCore);
        require(sourceFile.replaceWithText("Source changed during this private attempt"), "change only private resolver fixture");
        require(candidate.advance().error == Error::sourceConflict && candidate.getStage() == Stage::failed
            && candidate.getFailureStage() == Stage::preparingCore && emptyScratch(),
            "changed source evidence discards unacquired candidate and preserves durable data");
        require(sourceFile.replaceWithData(sourceBytes.getData(), sourceBytes.getSize()), "restore private source evidence bytes");
        require(candidate.advance().error == Error::sourceConflict, "source repair never silently retries an old failed attempt");
        require(inspectAttempt(candidate).record.failure == AttemptFailure::sourceConflict, "actual source conflict has distinct durable failure classification");
    }
    {
        Candidate candidate(store, request()); reach(candidate, Stage::coreReady);
        const auto oldAttempt = candidate.getAttemptId();
        publishLater();
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        require(candidate.acquireCore(oldAttempt, acquired).error == Error::staleHead && !acquired && emptyScratch(),
            "head publication between CoreReady and handoff invalidates stale candidate");
        Candidate retry(store, request());
        require(retry.getAttemptId() != oldAttempt, "explicit retry creates a fresh attempt identity");
        reach(retry, Stage::coreReady);
        good(retry.acquireCore(retry.getAttemptId(), acquired), "explicit earlier checkpoint restores on newly confirmed owner head");
        retry.requestCancel(); require(retry.advance().error == Error::cancelled, "owner explicitly cancels remaining earlier-checkpoint background work before retirement");
    }
    require(emptyScratch(), "earlier-point candidate scratch retires off callback");
    {
        auto racing = request();
        bool published = false;
        racing.ownerFence = [&] (const media::IdentitySnapshot&)
        {
            if (!published) { published = true; publishLater(); }
            return Status{};
        };
        Candidate candidate(store, std::move(racing));
        good(candidate.advance(), "queue source-publication race fixture");
        require(candidate.advance().error == Error::staleHead && emptyScratch(),
            "post-resolver head fence catches a publication during source validation");
    }
    {
        // Hide one future CAS body after a truthful first admission; the head
        // fence remains cheap, but the actual later chunk load must fail.
        const auto manifest = store.inspectManifest();
        PreparedRecoveryCheckpoint metadata;
        good(RecoveryCheckpointCodec::prepare(manifest.commits.front(), metadata, limits), "inspect future private CAS dependency");
        const auto& sequence = metadata.getAudioGraph().sequences[metadata.getAudioGraph().states.front()];
        require(sequence.chunks.size() > 1, "fixture has a truly deferred current audio chunk");
        const auto owners = root.getChildFile("candidate-store").findChildFiles(juce::File::findDirectories, false);
        require(owners.size() == 1, "only private test owner is present");
        const auto asset = owners[0].getChildFile("assets").getChildFile(sequence.chunks.back().id);
        const auto heldAsset = asset.getSiblingFile(asset.getFileName() + "-held");
        Candidate candidate(store, request()); reach(candidate, Stage::coreReady);
        std::shared_ptr<const RecoveredCandidateDocument> acquired;
        good(candidate.acquireCore(candidate.getAttemptId(), acquired), "admit real prefix before later asset fault");
        require(asset.moveFileTo(heldAsset), "hide only private future CAS body");
        for (int i = 0; i < 256 && candidate.getStage() != Stage::failed; ++i) candidate.advance();
        require(candidate.getStatus().error == Error::missingOrCorruptAsset && candidate.getStage() == Stage::failed
            && !acquired->coreState->playbackAvailability.canAdvanceContiguousPrefix()
            && !acquired->coreState->getPlayableSampleRange().isEmpty(), "late corrupt/missing body fails candidate without inventing remaining coverage");
        require(heldAsset.moveFileTo(asset), "restore private future CAS body");
        require(inspectAttempt(candidate).record.failure == AttemptFailure::missingOrCorruptAsset
            && candidate.getAttemptRecord()->stage == Stage::materializingFull, "late real CAS fault has durable asset classification and materialization stage");
        good(store.inspect().status, "controlled failure retains durable recovery owner for explicit retry");
        acquired.reset();
    }
    require(emptyScratch(), "all failed candidate scratch readers retire");
    std::cout << "RecoveryRestoreCandidateTests passed (real Document/audio/peaks, history, fences, cancellation, later fault)\n";
}
