#include "RecoveryInspection.h"
#include <algorithm>
#include <map>

namespace singleeditor::recovery
{
bool RecoveryInspection::Report::quarantinedFor(const juce::String& build) const
{
    return attempts.status && std::any_of(attempts.attempts.begin(), attempts.attempts.end(), [&](const auto& item)
    { return item.health == AttemptHealth::quarantined && item.record.appBuild == build && item.record.checkpoint == selectedHead; });
}
RecoveryInspection::Report RecoveryInspection::read(const RecoveryStore& store, const juce::File& attemptRoot,
    const Request& request, const std::atomic<bool>* cancellation, AudioProgress progress,
    std::shared_ptr<RecoveryDiagnosticState> diagnostics)
{
    Report result; result.owner = store.ownerId(); result.observedHead = request.observedHead;
    result.operationId = juce::Uuid(); result.sourceAction = request.action;
    result.audioCheckRequested = request.verifyStoredAudio;
    result.selectedHead = request.selectedHead.sequence > 0 ? request.selectedHead : request.observedHead;
    const auto cancelled = [&] { return cancellation && cancellation->load(std::memory_order_acquire); };
    const auto finish = [&]() -> Report
    {
        if (diagnostics && result.status.error != Error::cancelled)
        {
            using Stage = RecoveryDiagnosticState::Stage;
            const auto error = !result.status ? result.status.error : !result.sourceStatus ? result.sourceStatus.error
                : !result.audioStatus ? result.audioStatus.error : result.attempts.status.error;
            const auto stage = !result.status ? Stage::centerInspect : !result.sourceStatus ? Stage::centerSource
                : !result.audioStatus ? Stage::centerAudio : Stage::centerAttempts;
            diagnostics->record({ result.operationId, result.owner, stage, error,
                { static_cast<int64_t>(result.metadataBytesRead),
                  result.sourceCheckAttempted && !result.sourceChecked ? -1 : static_cast<int64_t>(result.sourceWindowBytesRead),
                  result.sourceCheckAttempted && !result.sourceChecked ? -1 : result.sourceFullBytesRead, static_cast<int64_t>(result.audioPayloadBytesRead),
                  static_cast<int64_t>(result.audioAssetsVerified), result.selectedHead.sequence } });
        }
        return std::move(result);
    };
    const auto fail = [&](Status status) { result.status = std::move(status); result.metadataReady = false; result.audioVerified = false; return finish(); };
    try
    {
        if (cancelled()) return fail({ Error::cancelled, "inspection_cancelled" });
        if (store.ownerId().isNull() || request.observedHead.sequence <= 0 || attemptRoot == juce::File()
            || attemptRoot == store.rootDirectory() || attemptRoot.isAChildOf(store.rootDirectory()) || store.rootDirectory().isAChildOf(attemptRoot))
            return fail({ Error::invalidPacket, "inspection_request" });
        // Inspect attempts even when the selected journal subsequently proves
        // unsupported. This is read-only history, not a restore/retry request.
        result.attempts = RecoveryAttemptStore(attemptRoot, store.ownerId(), request.attempts).inspect(cancelled);
        result.attemptsChecked = true;
        if (cancelled()) return fail({ Error::cancelled, "inspection_cancelled" });
        const auto manifest = store.inspectManifest(cancelled);
        result.metadataBytesRead = manifest.verifiedRecordBytes;
        if (!manifest.status) return fail(manifest.status);
        if (manifest.head != request.observedHead) return fail({ Error::staleHead, "inspection_head_changed" });
        for (auto at = manifest.commits.rbegin(); at != manifest.commits.rend(); ++at)
            result.points.push_back({ at->head, static_cast<int64_t>(at->record["timestamp"]) });
        const auto prepared = RecoveryCheckpointCodec::prepareThrough(manifest, result.selectedHead, result.checkpoint, request.graph, cancelled);
        if (!prepared) return fail(prepared);
        std::set<juce::String> assets;
        for (const auto& sequence : result.checkpoint.getAudioGraph().sequences)
            for (const auto& chunk : sequence.chunks) assets.insert(chunk.id);
        result.assets.assign(assets.begin(), assets.end());
        result.metadataReady = true;
        if (request.verifyStoredAudio)
        {
            const auto verify = [&]() -> Status
            {
                if (request.audioReadBytes == 0 || request.audioReadBytes > 512 * 1024 * 1024)
                    return { Error::invalidPacket, "inspection_audio_budget" };
                std::shared_ptr<RecoveryOwnerLease> lease;
                auto status = store.acquireOwnerLease(lease);
                if (!status) return status;
                status = store.checkHead(request.observedHead);
                if (!status) return status;
                struct Expected final { double rate; int channels; int64_t frames; };
                std::map<juce::String, Expected> expected;
                for (const auto& sequence : result.checkpoint.getAudioGraph().sequences)
                    for (const auto& chunk : sequence.chunks)
                    {
                        const auto [at, inserted] = expected.emplace(chunk.id, Expected { sequence.sampleRate, sequence.channels, chunk.frames });
                        if (!inserted && (at->second.rate != sequence.sampleRate || at->second.channels != sequence.channels || at->second.frames != chunk.frames))
                        { result.audioFailureAsset = chunk.id; return { Error::missingOrCorruptAsset, "inspection_audio_reference_conflict" }; }
                    }
                if (progress) progress(0, expected.size(), 0);
                for (const auto& [id, facts] : expected)
                {
                    if (cancelled()) return { Error::cancelled, "inspection_audio_cancelled" };
                    // Describe already validated products/aggregate sample size.
                    // Reserve the maximum legal audio JSON header before each
                    // read, so a corrupt/short asset cannot evade the budget.
                    const auto maximum = static_cast<size_t>(facts.frames) * static_cast<size_t>(facts.channels) * 4 + 1028;
                    if (maximum > request.audioReadBytes - result.audioPayloadBytesRead)
                        return { Error::budgetExceeded, "inspection_audio_read_budget" };
                    AudioBlock block;
                    size_t consumed = 0;
                    status = store.readAudioBounded(id, maximum, block, consumed);
                    result.audioPayloadBytesRead += consumed;
                    if (!status) { result.audioFailureAsset = id; return status; }
                    if (block.sampleRate != facts.rate || block.channels != facts.channels || block.frames != facts.frames)
                    { result.audioFailureAsset = id; return { Error::missingOrCorruptAsset, "inspection_audio_dimensions" }; }
                    ++result.audioAssetsVerified;
                    if (progress) progress(result.audioAssetsVerified, expected.size(), result.audioPayloadBytesRead);
                    // Only this block is resident; no playback reader, full
                    // audio buffer, live source or recovered Document is built.
                }
                return {};
            };
            result.audioCheckAttempted = true;
            result.audioStatus = verify(); result.audioVerified = static_cast<bool>(result.audioStatus);
        }
        if (request.action != SourceAction::none)
        {
            if (request.action != SourceAction::locateOrCheck && request.action != SourceAction::deepVerify)
                return fail({ Error::invalidPacket, "inspection_source_action" });
            auto source = request.source;
            source.verification = request.action == SourceAction::deepVerify ? RecoverySourceIdentity::Mode::explicitDeepVerify
                : RecoverySourceIdentity::Mode::sparseAfterMutation;
            std::unique_ptr<RecoverySourceBinding> binding;
            result.sourceCheckAttempted = true;
            result.sourceStatus = RecoverySourceBinding::resolve(result.checkpoint.getIdentity().source,
                result.checkpoint.getSourceEvidence(), source, binding, cancellation);
            if (result.sourceStatus && binding)
            {
                result.sourceChecked = true; result.resolution = binding->getResolution(); result.observation = binding->getObservation();
                result.sourceWindowBytesRead = binding->payloadBytesRead(); result.sourceFullBytesRead = binding->fullBytesRead();
                result.sourceFullHash = binding->fullFileHash();
            }
        }
        if (cancelled()) return fail({ Error::cancelled, "inspection_cancelled" });
        const auto fence = store.checkHead(request.observedHead);
        if (!fence) return fail(fence);
        return finish();
    }
    catch (...) { return fail({ Error::resourceOrEnvironmentFault, "inspection_exception" }); }
}
RecoveryInspection::DiagnosticObservation RecoveryInspection::Report::diagnosticObservation(const juce::String& appBuild) const
{
    DiagnosticObservation result;
    result.operationId = operationId; result.owner = owner;
    result.observedHead = observedHead; result.selectedHead = selectedHead;
    result.status = status; result.attemptsStatus = attempts.status;
    result.sourceStatus = sourceStatus; result.audioStatus = audioStatus;
    result.sourceAction = sourceAction; result.metadataReady = metadataReady;
    result.attemptsChecked = attemptsChecked; result.sourceCheckAttempted = sourceCheckAttempted; result.audioCheckAttempted = audioCheckAttempted;
    result.sourceChecked = sourceChecked; result.audioCheckRequested = audioCheckRequested; result.audioVerified = audioVerified;
    result.quarantined = quarantinedFor(appBuild);
    result.sourceConfidence = resolution.confidence; result.sourceReason = resolution.reason;
    result.points = static_cast<int64_t>(points.size()); result.assets = static_cast<int64_t>(assets.size());
    result.attempts = static_cast<int64_t>(attempts.attempts.size());
    result.metadataBytes = static_cast<int64_t>(metadataBytesRead);
    // A failed resolver does not return its read counters. Unknown is distinct
    // from a completed Missing classification that actually read zero bytes.
    result.sourceWindowBytes = sourceCheckAttempted && !sourceChecked ? -1 : static_cast<int64_t>(sourceWindowBytesRead);
    result.sourceFullBytes = sourceCheckAttempted && !sourceChecked ? -1 : sourceFullBytesRead;
    result.audioBytes = static_cast<int64_t>(audioPayloadBytesRead); result.verifiedAssets = static_cast<int64_t>(audioAssetsVerified);
    return result;
}
}
