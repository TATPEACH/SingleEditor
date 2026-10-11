#include "RecoveryRestoreCandidate.h"
#include <algorithm>
#include <cmath>

namespace singleeditor::recovery
{
RecoveryRestoreCandidate::RecoveryRestoreCandidate(RecoveryStore storage, Request parameters)
    : store(std::move(storage)), request(std::move(parameters))
{
}
RecoveryRestoreCandidate::~RecoveryRestoreCandidate() { retire(); }

void RecoveryRestoreCandidate::retire()
{
    for (auto& source : sources) if (source) source->cancel();
    fullPeaks.reset(); core.reset(); backing = {}; sources.clear();
    selected = {}; prepared = {}; hasMetadata = false;
    sourceBinding.reset();
    allocationLease.reset();
    preparationLease.reset();
}

Status RecoveryRestoreCandidate::fail(Status reason)
{
    failureStage = stage;
    status = std::move(reason);
    if (attemptWriter && attemptWriter->isActive())
    {
        auto cause = status;
        if (cause.code.isEmpty() || cause.code.length() > 96
            || !cause.code.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"))
            cause.code = "candidate_failure"; // Original diagnostic stays in status.
        attemptCloseStatus = attemptWriter->finish(status.error == Error::cancelled ? AttemptOutcome::cancelled : AttemptOutcome::failed, cause);
        attemptRecord = attemptWriter->getRecord();
        attemptWriter.reset(); // Failed terminal publication stays honestly unclosed.
    }
    stage = status.error == Error::cancelled ? Stage::cancelled : Stage::failed;
    retire();
    return status;
}

Status RecoveryRestoreCandidate::checkFences()
{
    if (cancellation.load(std::memory_order_acquire)) return { Error::cancelled, "candidate_cancelled" };
    auto result = store.checkHead(request.observedOwnerHead);
    if (!result) return result;
    if (hasMetadata)
    {
        if (sourceBinding)
        {
            result = sourceBinding->check(prepared.getIdentity().source, &cancellation);
            if (!result) return result;
        }
        if (request.ownerFence)
        {
            result = request.ownerFence(prepared.getIdentity());
            if (!result) return result;
            if (sourceBinding)
            {
                result = sourceBinding->check(prepared.getIdentity().source, &cancellation);
                if (!result) return result;
            }
        }
        // Resolver I/O may overlap a publication or cancellation.
        if (cancellation.load(std::memory_order_acquire)) return { Error::cancelled, "candidate_cancelled" };
        result = store.checkHead(request.observedOwnerHead);
    }
    return result;
}

Status RecoveryRestoreCandidate::advance() try
{
    if (stage == Stage::failed || stage == Stage::cancelled) return status;
    if (stage == Stage::ready) return {};
    if (stage == Stage::queued)
    {
        if (cancellation.load(std::memory_order_acquire)) return fail({ Error::cancelled, "candidate_cancelled" });
        if (request.observedOwnerHead.sequence <= 0 || request.checkpointHead.sequence <= 0
            || request.previewBins < 1 || request.previewBins > 8192 || !std::isfinite(request.previewSeconds)
            || request.previewSeconds < 1 || request.previewSeconds > 30)
            return fail({ Error::invalidPacket, "candidate_request" });
        const auto overlaps = [] (const juce::File& a, const juce::File& b)
            { return a == b || a.isAChildOf(b) || b.isAChildOf(a); };
        if (request.attemptDirectory == juce::File() || request.scratchDirectory == juce::File()
            || overlaps(request.attemptDirectory, store.rootDirectory())
            || overlaps(request.attemptDirectory, request.scratchDirectory))
            return fail({ Error::invalidPacket, "candidate_attempt_directory" });
        RecoveryAttemptStore markers(request.attemptDirectory, store.ownerId(), request.attemptLimits);
        const auto marked = markers.begin(attempt, request.observedOwnerHead, request.checkpointHead,
            request.appBuild, attemptWriter, request.retryQuarantinedOnce);
        if (!marked) return fail(marked);
        attemptRecord = attemptWriter->getRecord();
        stage = Stage::validating;
        return {};
    }
    auto result = checkFences();
    if (!result) return fail(result);
    switch (stage)
    {
        case Stage::validating:
        {
            result = store.acquireOwnerLease(preparationLease);
            if (!result) return fail(result);
            const auto manifest = store.inspectManifest([this] { return cancellation.load(std::memory_order_acquire); });
            if (!manifest.status) return fail(manifest.status);
            if (manifest.head != request.observedOwnerHead) return fail({ Error::staleHead, "candidate_manifest_changed" });
            const auto found = std::find_if(manifest.commits.begin(), manifest.commits.end(),
                [this] (const auto& commit) { return commit.head == request.checkpointHead; });
            if (found == manifest.commits.end())
            {
                if (manifest.checkpointCut.isNotEmpty() && request.checkpointHead.sequence < manifest.checkpointFloor.sequence)
                    return fail({ Error::staleHead, "candidate_checkpoint_pruned" });
                return fail({ Error::invalidPacket, "candidate_checkpoint_not_on_chain" });
            }
            selected = *found;
            result = RecoveryCheckpointCodec::prepareThrough(manifest, selected.head, prepared, request.limits,
                [this] { return cancellation.load(std::memory_order_acquire); });
            if (!result) return fail(result);
            hasMetadata = true;
            stage = Stage::resolvingSource;
            break;
        }
        case Stage::resolvingSource:
            result = RecoverySourceBinding::resolve(prepared.getIdentity().source, prepared.getSourceEvidence(),
                request.source, sourceBinding, &cancellation);
            if (!result) return fail(result);
            sourceResolution = sourceBinding->getResolution();
            // This controller still creates a detached Copy. Exact/confirmed
            // Relocated Save requires the separate guarded source-checkpoint
            // consumer; classification alone never changes Document write rights.
            stage = Stage::replayingCheckpoint;
            break;
        case Stage::replayingCheckpoint:
        {
            const auto& graph = prepared.getAudioGraph();
            if (request.allocationBudget)
            {
                size_t total = 0;
                for (const auto& sequence : graph.sequences)
                {
                    if (sequence.channels <= 0 || sequence.frames < 0
                        || static_cast<uint64_t>(sequence.frames) > (request.limits.materialisedBytes - total) / static_cast<size_t>(sequence.channels) / 4)
                        return fail({ Error::budgetExceeded, "candidate_mapping_budget" });
                    total += static_cast<size_t>(sequence.frames) * static_cast<size_t>(sequence.channels) * 4;
                }
                result = request.allocationBudget->reserve(total, allocationLease);
                if (!result) return fail(result);
            }
            sources.resize(graph.sequences.size());
            backing.identity = prepared.getIdentity();
            for (size_t i = 0; i < prepared.getStates().size(); ++i)
            {
                if (cancellation.load(std::memory_order_acquire)) return fail({ Error::cancelled, "candidate_cancelled" });
                auto& source = sources[graph.states[i]];
                if (!source)
                {
                    result = ProgressiveRecoveryAudio::create(prepared.getAudioJson(), i, request.scratchDirectory, source, request.limits, &store, allocationLease);
                    if (!result) return fail(result);
                }
                SingleTrackDocument::StatePtr state;
                result = source->bindMetadata(*prepared.getStates()[i], state);
                if (!result) return fail(result);
                if (i == 0) backing.state = state;
                else if (i <= prepared.getUndoCount()) backing.undo.push_back(state);
                else backing.redo.push_back(state);
            }
            stage = Stage::preparingCore;
            preparationLease.reset(); // Every retained audio sequence now owns its real lease.
            break;
        }
        case Stage::preparingCore:
        {
            auto& current = sources[prepared.getAudioGraph().states.front()];
            result = current->loadNext(store, [this] { return cancellation.load(std::memory_order_acquire); });
            if (!result) return fail(result);
            auto packet = std::make_shared<RecoveredCandidateDocument>();
            packet->attempt = attempt;
            juce::String error;
            packet->document = SingleTrackDocument::createRecoveredCopy(backing, error);
            if (!packet->document) return fail({ Error::corruptJournal, error });
            packet->coreState = packet->document->getState();
            packet->firstPeaks = WaveformModel::buildLeadingPreviewPeakCacheForState(packet->coreState,
                request.previewBins, request.previewSeconds);
            if (!packet->firstPeaks || packet->firstPeaks->loadedSampleRange.isEmpty()
                || packet->firstPeaks->loadedSampleRange.getEnd() > packet->coreState->getPlayableSampleRange().getEnd())
                return fail({ Error::resourceOrEnvironmentFault, "candidate_first_waveform_failed" });
            core = std::move(packet);
            stage = Stage::coreReady;
            break;
        }
        case Stage::coreReady:
            stage = Stage::materializingFull;
            break;
        case Stage::materializingFull:
        {
            // Current audio first, then unique history backings, one chunk per
            // scheduling quantum. User-range priority is a future owner concern.
            const auto currentIndex = prepared.getAudioGraph().states.front();
            auto pending = currentIndex;
            if (sources[pending]->isComplete())
            {
                pending = 0;
                while (pending < sources.size() && sources[pending]->isComplete()) ++pending;
            }
            if (pending < sources.size())
            {
                result = sources[pending]->loadNext(store, [this] { return cancellation.load(std::memory_order_acquire); });
                if (!result) return fail(result);
            }
            else
            {
                fullPeaks = WaveformModel::buildCompletePeakCacheForState(core->coreState,
                    [this] { return cancellation.load(std::memory_order_acquire); });
                if (cancellation.load(std::memory_order_acquire)) return fail({ Error::cancelled, "candidate_cancelled" });
                if (!fullPeaks || fullPeaks->isPreview || fullPeaks->loadedSampleRange.getEnd() != core->coreState->info.numSamples)
                    return fail({ Error::resourceOrEnvironmentFault, "candidate_full_waveform_failed" });
                stage = Stage::ready;
            }
            break;
        }
        default: break;
    }
    result = checkFences();
    if (!result) return fail(result);
    result = attemptWriter->enter(stage);
    if (!result) return fail(result);
    attemptRecord = attemptWriter->getRecord();
    if (stage == Stage::ready)
    {
        result = attemptWriter->finish(AttemptOutcome::succeeded);
        if (!result) return fail(result);
        attemptRecord = attemptWriter->getRecord();
        attemptWriter.reset();
    }
    return {};
}
catch (...) { return fail({ Error::resourceOrEnvironmentFault, "candidate_worker_exception" }); }

Status RecoveryRestoreCandidate::acquireCore(const juce::Uuid& expectedAttempt,
    std::shared_ptr<const RecoveredCandidateDocument>& output) try
{
    if (expectedAttempt != attempt) return { Error::invalidPacket, "candidate_attempt_mismatch" };
    if (stage == Stage::failed || stage == Stage::cancelled) return status;
    if (acquired) return { Error::invalidPacket, "candidate_already_acquired" };
    if (!core || (stage != Stage::coreReady && stage != Stage::materializingFull && stage != Stage::ready))
        return { Error::invalidPacket, "candidate_core_not_ready" };
    const auto result = checkFences();
    if (!result) return fail(result);
    output = core;
    acquired = true;
    return {};
}
catch (...) { return fail({ Error::resourceOrEnvironmentFault, "candidate_acquire_exception" }); }

Status RecoveryRestoreCandidate::abort(Status cause)
{
    if (cause || stage == Stage::ready || stage == Stage::failed || stage == Stage::cancelled)
        return { Error::invalidPacket, "candidate_terminal_or_empty_abort" };
    return fail(std::move(cause));
}
std::shared_ptr<WaveformModel::PeakCache> RecoveryRestoreCandidate::takeFullPeaks() noexcept
{
    return stage == Stage::ready ? std::move(fullPeaks) : nullptr;
}
}
