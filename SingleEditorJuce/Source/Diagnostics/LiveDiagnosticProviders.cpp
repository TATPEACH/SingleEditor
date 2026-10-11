#include "LiveDiagnosticProviders.h"
#include "DiagnosticTestActivity.h"
#include "NativeDeviceMetadata.h"
#include "Audio/AudioEngine.h"
#include "Audio/SpatialEnergyLayout.h"
#include "Document/SingleTrackDocument.h"
#include "Recovery/RecoveryService.h"
#include <cmath>

namespace singleeditor::diagnostics
{
namespace
{
using Operation = media::OptionalMediaOperation;
using State = media::OptionalMediaFailureState;
const char* knownNativeFormat(const juce::String& name)
{
    for (const auto* format : { "WAV", "AIFF", "FLAC", "Ogg-Vorbis", "MP3" })
        if (name.equalsIgnoreCase(format) || name.equalsIgnoreCase(juce::String(format) + " file")) return format;
    return nullptr;
}
const char* monitorModeName(MonitorMode mode)
{
    return mode == MonitorMode::forceStereoDownmix ? "force-stereo"
        : mode == MonitorMode::forceMultichannel ? "force-multichannel" : "auto-follow-device";
}
void appendPhysicalOutputBinding(ProviderEvidence& evidence, const AudioEngine& engine,
                                 const AudioEngine::DeviceDiagnostics& device)
{
    const auto unavailable=[&](Unavailable reason,const char* state=nullptr)
    {
        evidence.fields.push_back({Field::outputBindingState,state ? juce::var(state) : juce::var(),reason});
        for(const auto field : {Field::speakerLayout,Field::speakerGeometry,Field::speakerLayoutSource,
            Field::speakerGeometrySource,Field::outputBindingRevision,Field::physicalRouteSourceChannels,
            Field::physicalRouteConnections,Field::physicalRouteEncoding,Field::speakerGeometryEncoding,
            Field::lfePhysicalOutputs,Field::unplacedPhysicalOutputs}) evidence.fields.push_back({field,{},reason});
    };
    if (!device.active)
    {
        unavailable(device.availability==AudioEngine::DeviceDiagnostics::Availability::busy
            ? Unavailable::providerBusy : Unavailable::notApplicable);
        return;
    }
    const auto& route=device.outputPlan;
    if (!route) { unavailable(Unavailable::notReported,"no-approved-output-binding"); return; }
    const auto& physical=route->device().physicalOutputs;
    bool matches=device.outputChannelMapping && device.outputChannelMapping->size()==static_cast<int>(physical.size())
        && device.active->sampleRate==route->device().sampleRate && device.active->bufferSamples==route->device().bufferSamples;
    if(matches) for(int slot=0;slot<static_cast<int>(physical.size());++slot)
        if((*device.outputChannelMapping)[slot]!=physical[static_cast<std::size_t>(slot)]) matches=false;
    if (!matches) { unavailable(Unavailable::notReported,"active-output-binding-mismatch"); return; }

    // Reuse immutable audio-owner facts, never a Spatial Energy lease, decoder
    // factory, device enumeration or render-cache read. Names/IDs are not exported.
    const auto artifact=engine.getAmbisonicDecodeMatrixArtifactForDiagnostics();
    const auto geometry=artifact ? std::shared_ptr<const SpeakerGeometryProfile>(artifact,&artifact->geometry)
        : AmbisonicStandardMatrixCatalog::standardProfile(juce::String(route->bus().profileId));
    const auto layout=SpatialEnergyLayout::fromRoute(*route,geometry.get(),0);
    juce::Array<juce::var> connections,angles,lfe,unplaced;
    bool connectionsOverBudget=false;
    for(int row=0;row<static_cast<int>(route->bus().speakers.size());++row)
        for(int slot=0;slot<static_cast<int>(physical.size());++slot)
            if(route->connected(row,slot))
            {
                if(connections.size()>=512) { connectionsOverBudget=true; continue; }
                connections.add(row); connections.add(physical[static_cast<std::size_t>(slot)]);
            }
    juce::StringArray roles;
    for(const auto& channel : layout.channels)
    {
        auto role=SpeakerRole::unknown;
        if(geometry && !channel.speakerId.empty())
            for(const auto& speaker : geometry->speakers)
                if(speaker.stableId.toStdString()==channel.speakerId) { role=speaker.role; break; }
        roles.add(juce::String(channel.physicalOutput)+"="+(role==SpeakerRole::unknown ? "?" : ChannelLayoutCatalog::roleToAbbreviation(role)));
        if(channel.placement==SpatialEnergyMeter::Placement::directional)
        { angles.add(channel.physicalOutput); angles.add(channel.azimuthDegrees); angles.add(channel.elevationDegrees); }
        else if(channel.placement==SpatialEnergyMeter::Placement::lfe) lfe.add(channel.physicalOutput);
        else unplaced.add(channel.physicalOutput);
    }
    const auto order="physical-zero-based "+roles.joinIntoString(" ");
    const auto geometryUnavailable=layout.geometryCurrent ? Unavailable::notSupported : Unavailable::notReported;
    evidence.fields.insert(evidence.fields.end(),{
        {Field::outputBindingState,layout.geometryCurrent ? "approved-route-current-geometry" : "approved-route-unresolved-geometry"},
        {Field::outputBindingRevision,static_cast<juce::int64>(route->bus().revision)},
        {Field::physicalRouteSourceChannels,static_cast<int>(route->bus().speakers.size())},
        {Field::physicalRouteEncoding,"source-row-physical-output-pairs-zero-based"},
        {Field::physicalRouteConnections,connectionsOverBudget ? juce::var() : juce::var(connections),Unavailable::notSupported},
        {Field::speakerLayout,layout.geometryCurrent && order.length()<=256 ? juce::var(order) : juce::var(),geometryUnavailable},
        {Field::speakerLayoutSource,"approved-output-binding"},
        {Field::speakerGeometrySource,geometry ? juce::var(geometry->builtInReadOnly ? "standard-profile-bound-to-output" : "active-custom-artifact-bound-to-output") : juce::var(),Unavailable::notReported},
        {Field::speakerGeometryEncoding,"physical-output-azimuth-elevation-triples-deg-left-positive-zero-based"},
        {Field::speakerGeometry,layout.geometryCurrent && angles.size()<=512 ? juce::var(angles) : juce::var(),geometryUnavailable},
        {Field::lfePhysicalOutputs,juce::var(lfe)}, {Field::unplacedPhysicalOutputs,juce::var(unplaced)} });
}
void appendRenderObservation(ProviderEvidence& evidence, const AudioEngine::RenderDiagnostics& render)
{
    using Availability = AudioEngine::RenderDiagnostics::Availability;
    if (render.availability != Availability::available)
    {
        const auto reason = render.availability == Availability::busy ? Unavailable::providerBusy
            : render.availability == Availability::notSupported ? Unavailable::notSupported : Unavailable::notReported;
        evidence.fields.insert(evidence.fields.end(), {
            { Field::routingObservation, render.availability == Availability::busy ? "busy"
                : render.availability == Availability::notSupported ? "direct-render-not-observed" : "not-rendered" },
            { Field::outputMatrix, {}, reason }, { Field::downmixState, {}, reason }, { Field::decoderState, {}, reason } });
        return;
    }
    // Output-major gains at the Document renderer, before transport resampling,
    // envelope/monitor gain and physical-device channel selection. An HRTF is
    // a convolution renderer and must never be exported as a scalar matrix.
    evidence.fields.insert(evidence.fields.end(), {
        { Field::routingObservation, "last-document-render-output-major" },
        { Field::downmixState, monitorModeName(render.monitorMode) },
        { Field::decoderState, render.binauralRequested ? (render.binauralActive ? "binaural-hrtf" : "binaural-unavailable")
            : render.ambisonic ? (render.matrix ? "ambisonic-speaker" : "ambisonic-unavailable") : "channel-router" } });
    if (!render.matrix)
    {
        evidence.fields.push_back({ Field::outputMatrix, {}, render.binauralRequested ? Unavailable::notApplicable : Unavailable::notReported });
        return;
    }
    const auto& matrix = *render.matrix;
    evidence.fields.push_back({ Field::outputMatrixSourceChannels, matrix.sourceChannels });
    evidence.fields.push_back({ Field::outputMatrixOutputChannels, matrix.outputChannels });
    if (matrix.sourceChannels * matrix.outputChannels > 512)
        evidence.fields.push_back({ Field::outputMatrix, {}, Unavailable::notSupported });
    else
    {
        juce::Array<juce::var> gains;
        for (int output = 0; output < matrix.outputChannels; ++output)
            for (int source = 0; source < matrix.sourceChannels; ++source)
                gains.add(static_cast<double>(matrix.gain(output, source)));
        evidence.fields.push_back({ Field::outputMatrix, juce::var(gains) });
    }
}
void appendCurrentReader(ProviderEvidence& evidence, const SingleTrackDocument::State& documentState)
{
    // Observe the same owned backing used by DocumentAudioSource. An earlier
    // optional decode operation describes a different fact, not this reader.
    if (documentState.audioBuffer != nullptr && documentState.audioBuffer->getNumSamples() > 0)
    {
        evidence.fields.push_back({ Field::selectedReader, "document-pcm-buffer" });
        evidence.fields.push_back({ Field::dependencyIdentity, {}, Unavailable::notApplicable });
        return;
    }
    const auto* reader = documentState.memoryMappedReader != nullptr
        ? documentState.memoryMappedReader.get() : documentState.streamingReader.get();
    if (reader == nullptr)
    {
        evidence.fields.push_back({ Field::selectedReader, {}, Unavailable::notApplicable });
        return;
    }
    if (documentState.audioBackingStore == SingleTrackDocument::State::AudioBackingStore::recoveryStream
        && reader->getFormatName() == "Verified Recovery PCM")
    {
        evidence.fields.push_back({ Field::selectedReader, "verified-recovery-pcm" });
        evidence.fields.push_back({ Field::dependencyIdentity, "SingleEditor-built-in" });
    }
    else if (const auto* format = knownNativeFormat(reader->getFormatName()))
    {
        evidence.fields.push_back({ Field::selectedReader, juce::String("JUCE-") + format
            + (documentState.memoryMappedReader != nullptr ? "-memory-mapped" : "-streaming") });
        evidence.fields.push_back({ Field::dependencyIdentity, juce::SystemStats::getJUCEVersion().replaceCharacter(' ', '-') });
    }
    else evidence.fields.push_back({ Field::selectedReader, {}, Unavailable::notSupported });
    // Reader ownership alone cannot establish why an earlier fallback ran.
}
void appendRecoveryFailure(ProviderEvidence& evidence, const recovery::RecoveryService* service,
                           const recovery::RecoveryDiagnosticState* state)
{
    using Stage = recovery::RecoveryDiagnosticState::Stage;
    evidence.fields.push_back({ Field::recoveryFailureCoverage, "writer-maintenance-inspection-restore-session-host-service" });
    evidence.fields.push_back({ Field::recoveryFailureOrder, "service-result-publication" });
    std::optional<recovery::RecoveryDiagnosticState::Failure> failure;
    if (!(state ? state->trySnapshot(failure) : service && service->tryDiagnosticFailure(failure)))
    {
        evidence.fields.push_back({ Field::recoveryFailureStage, {}, Unavailable::providerBusy });
        return;
    }
    if (!failure) return;
    const char* source = "not-reported";
    auto stage = FailureStage::write;
    switch (failure->stage)
    {
        case Stage::admission: source = "writer-admission"; stage = FailureStage::admission; break;
        case Stage::launch: source = "writer-launch"; stage = FailureStage::launch; break;
        case Stage::maintenanceLaunch: source = "maintenance-launch"; stage = FailureStage::launch; break;
        case Stage::inspect: source = "writer-inspect"; stage = FailureStage::parse; break;
        case Stage::collectOrphans: source = "writer-orphan-collection"; stage = FailureStage::cleanup; break;
        case Stage::prepare: source = "writer-prepare"; break;
        case Stage::append: source = "writer-append"; break;
        case Stage::publication: source = "writer-publication-consumer"; break;
        case Stage::savedCancellation: source = "saved-queue-cancellation"; stage = FailureStage::cleanup; break;
        case Stage::retireSaved: source = "saved-owner-retirement"; stage = FailureStage::cleanup; break;
        case Stage::discard: source = "owner-discard"; stage = FailureStage::cleanup; break;
        case Stage::collectTerminal: source = "terminal-owner-collection"; stage = FailureStage::cleanup; break;
        case Stage::centerInspect: source = "center-inspection"; stage = FailureStage::parse; break;
        case Stage::centerAttempts: source = "center-attempt-inspection"; stage = FailureStage::parse; break;
        case Stage::centerSource: source = "center-source-check"; stage = FailureStage::parse; break;
        case Stage::centerAudio: source = "center-audio-verification"; stage = FailureStage::parse; break;
        case Stage::restoreLaunch: source = "restore-worker-launch"; stage = FailureStage::launch; break;
        case Stage::restoreStorage: source = "restore-storage"; stage = FailureStage::restore; break;
        case Stage::restoreQueued: source = "restore-queued"; stage = FailureStage::restore; break;
        case Stage::restoreValidation: source = "restore-validation"; stage = FailureStage::restore; break;
        case Stage::restoreSource: source = "restore-source"; stage = FailureStage::restore; break;
        case Stage::restoreReplay: source = "restore-replay"; stage = FailureStage::restore; break;
        case Stage::restoreCore: source = "restore-core"; stage = FailureStage::restore; break;
        case Stage::restoreCorePublication: source = "restore-core-publication"; stage = FailureStage::restore; break;
        case Stage::restoreFull: source = "restore-full-materialization"; stage = FailureStage::restore; break;
        case Stage::restoreFinalize: source = "restore-finalize"; stage = FailureStage::restore; break;
        case Stage::restoreAdmission: source = "restore-playlist-admission"; stage = FailureStage::admission; break;
        case Stage::restoreAttemptClose: source = "restore-attempt-close"; stage = FailureStage::cleanup; break;
        case Stage::sessionAdmission: source = "session-admission"; stage = FailureStage::admission; break;
        case Stage::sessionCapture: source = "session-capture"; stage = FailureStage::write; break;
        case Stage::sessionTransition: source = "session-transition"; stage = FailureStage::write; break;
        case Stage::sessionCheckpoint: source = "session-checkpoint"; stage = FailureStage::write; break;
        case Stage::hostConfiguration: source = "host-configuration"; stage = FailureStage::admission; break;
        case Stage::hostAdmission: source = "host-admission"; stage = FailureStage::admission; break;
        case Stage::hostMetadata: source = "host-metadata"; stage = FailureStage::cleanup; break;
        case Stage::hostPoll: source = "host-poll"; stage = FailureStage::cleanup; break;
        case Stage::hostSavedDisposition: source = "host-saved-disposition"; stage = FailureStage::cleanup; break;
        case Stage::hostRetry: source = "host-retry"; stage = FailureStage::admission; break;
        case Stage::hostCheckpoint: source = "host-checkpoint"; stage = FailureStage::admission; break;
        case Stage::maintenanceConfiguration: source = "maintenance-configuration"; stage = FailureStage::admission; break;
        case Stage::serviceConfiguration: source = "service-configuration"; stage = FailureStage::admission; break;
        case Stage::servicePoll: source = "service-poll"; stage = FailureStage::cleanup; break;
        case Stage::serviceTransfer: source = "service-ticket-transfer"; stage = FailureStage::admission; break;
        case Stage::serviceDiscard: source = "service-discard"; stage = FailureStage::admission; break;
        case Stage::serviceRetry: source = "service-retry"; stage = FailureStage::admission; break;
        case Stage::serviceCancel: source = "service-cancel"; stage = FailureStage::cleanup; break;
        case Stage::maintenanceDiscovery: source = "maintenance-discovery"; stage = FailureStage::cleanup; break;
        case Stage::centerStorage: source = "center-storage"; stage = FailureStage::cleanup; break;
    }
    evidence.fields.push_back({ Field::recoveryFailureStage, source });
    if (!failure->owner.isNull())
    {
        const auto owner = failure->owner.toString();
        evidence.fields.push_back({ Field::recoveryFailureOwner,
            juce::SHA256(owner.toRawUTF8(), owner.getNumBytesAsUTF8()).toHexString() });
    }
    if (std::find(evidence.operationIds.begin(), evidence.operationIds.end(), failure->operationId) == evidence.operationIds.end())
        evidence.operationIds.push_back(failure->operationId);
    evidence.latestFailure = FailureCapsule { failure->operationId, stage, static_cast<int64_t>(failure->error), failure->counters };
}
void appendInspection(ProviderEvidence& evidence, const recovery::RecoveryInspection::DiagnosticObservation& report)
{
    if (report.operationId.isNull()) return;
    evidence.operationIds.push_back(report.operationId);
    const auto digest = [](const juce::Uuid& id)
    { const auto text = id.toString(); return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString(); };
    const auto hash = [](const juce::String& text)
    { return text.length() == 64 && text.containsOnly("0123456789abcdef"); };
    evidence.fields.push_back({ Field::inspectionOperationId, report.operationId.toString() });
    if (!report.owner.isNull()) evidence.fields.push_back({ Field::inspectionOwner, digest(report.owner) });
    evidence.fields.push_back(hash(report.observedHead.hash) ? EvidenceField { Field::inspectionObservedHead, report.observedHead.hash }
        : EvidenceField { Field::inspectionObservedHead, {}, Unavailable::notReported });
    evidence.fields.push_back(hash(report.selectedHead.hash) ? EvidenceField { Field::inspectionSelectedHead, report.selectedHead.hash }
        : EvidenceField { Field::inspectionSelectedHead, {}, Unavailable::notReported });
    for (const auto& [field, status] : std::array<std::pair<Field, recovery::Status>, 4> {{
        { Field::inspectionStatus, report.status }, { Field::inspectionAttemptsStatus, report.attemptsStatus },
        { Field::inspectionSourceStatus, report.sourceStatus }, { Field::inspectionAudioStatus, report.audioStatus } }})
    {
        if ((field == Field::inspectionSourceStatus && report.sourceAction == recovery::RecoveryInspection::SourceAction::none)
            || (field == Field::inspectionAudioStatus && !report.audioCheckRequested))
            evidence.fields.push_back({ field, {}, Unavailable::notApplicable });
        else if ((field == Field::inspectionAttemptsStatus && !report.attemptsChecked)
                 || (field == Field::inspectionSourceStatus && !report.sourceCheckAttempted)
                 || (field == Field::inspectionAudioStatus && !report.audioCheckAttempted))
            evidence.fields.push_back({ field, {}, Unavailable::notReported });
        else if (status) evidence.fields.push_back({ field, "none" });
        else if (status.code.isNotEmpty() && status.code.length() <= 96
                 && status.code.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_"))
            evidence.fields.push_back({ field, status.code });
        else evidence.fields.push_back({ field, {}, Unavailable::invalidValue });
    }
    evidence.fields.push_back({ Field::inspectionSourceAction,
        report.sourceAction == recovery::RecoveryInspection::SourceAction::none ? "not-requested"
        : report.sourceAction == recovery::RecoveryInspection::SourceAction::deepVerify ? "deep-verify" : "locate-or-check" });
    const char* confidence = "not-reported";
    using Confidence = recovery::SourceConfidence;
    switch (report.sourceConfidence)
    {
        case Confidence::exact: confidence = "exact"; break;
        case Confidence::relocated: confidence = "relocated"; break;
        case Confidence::changed: confidence = "changed"; break;
        case Confidence::missing: confidence = "missing"; break;
        case Confidence::ambiguous: confidence = "ambiguous"; break;
    }
    evidence.fields.push_back(report.sourceChecked ? EvidenceField { Field::inspectionSourceConfidence, confidence }
        : EvidenceField { Field::inspectionSourceConfidence, {}, Unavailable::notReported });
    evidence.fields.push_back(report.sourceChecked ? EvidenceField { Field::inspectionSourceReason, static_cast<int>(report.sourceReason) }
        : EvidenceField { Field::inspectionSourceReason, {}, Unavailable::notReported });
    evidence.fields.insert(evidence.fields.end(), {
        { Field::inspectionObservedSequence, report.observedHead.sequence }, { Field::inspectionSelectedSequence, report.selectedHead.sequence },
        { Field::inspectionMetadataReady, static_cast<int>(report.metadataReady) }, { Field::inspectionSourceChecked, static_cast<int>(report.sourceChecked) },
        { Field::inspectionAudioRequested, static_cast<int>(report.audioCheckRequested) }, { Field::inspectionAudioVerified, static_cast<int>(report.audioVerified) },
        { Field::inspectionPoints, report.points },
        { Field::inspectionAssets, report.assets }, { Field::inspectionAttempts, report.attempts },
        { Field::inspectionMetadataBytes, report.metadataBytes }, { Field::inspectionAudioBytes, report.audioBytes },
        { Field::inspectionVerifiedAssets, report.verifiedAssets } });
    evidence.fields.push_back(report.attemptsChecked && report.attemptsStatus
        ? EvidenceField { Field::inspectionQuarantined, static_cast<int>(report.quarantined) }
        : EvidenceField { Field::inspectionQuarantined, {}, Unavailable::notReported });
    evidence.fields.push_back(report.sourceWindowBytes >= 0 ? EvidenceField { Field::inspectionSourceWindowBytes, report.sourceWindowBytes }
        : EvidenceField { Field::inspectionSourceWindowBytes, {}, Unavailable::notReported });
    evidence.fields.push_back(report.sourceFullBytes >= 0 ? EvidenceField { Field::inspectionSourceFullBytes, report.sourceFullBytes }
        : EvidenceField { Field::inspectionSourceFullBytes, {}, Unavailable::notReported });
}
const char* restoreStageName(recovery::RecoveryAttemptStage stage)
{
    using Stage = recovery::RecoveryAttemptStage;
    switch (stage)
    {
        case Stage::queued: return "queued";
        case Stage::validating: return "validating";
        case Stage::resolvingSource: return "resolving-source";
        case Stage::replayingCheckpoint: return "replaying-checkpoint";
        case Stage::preparingCore: return "preparing-core";
        case Stage::coreReady: return "core-ready";
        case Stage::materializingFull: return "materializing-full";
        case Stage::ready: return "ready";
        case Stage::failed: return "failed";
        case Stage::cancelled: return "cancelled";
    }
    return "not-reported";
}
void appendRestore(ProviderEvidence& evidence, const recovery::RecoveryRestoreQueue& queue, const juce::Uuid& selected)
{
    recovery::RecoveryRestoreQueue::DiagnosticObservation report;
    if (!queue.tryDiagnosticObservation(selected, report))
    { evidence.fields.push_back({ Field::restoreState, {}, Unavailable::providerBusy }); return; }
    evidence.fields.insert(evidence.fields.end(), {
        { Field::restoreState, "loaded" }, { Field::restoreJobCount, static_cast<juce::int64>(report.jobs) },
        { Field::restoreRunning, static_cast<int>(report.workerRunning) } });
    evidence.fields.push_back(report.residentMappingBytes
        ? EvidenceField { Field::restoreResidentBytes, static_cast<juce::int64>(*report.residentMappingBytes) }
        : EvidenceField { Field::restoreResidentBytes, {}, Unavailable::providerBusy });
    const auto operation = [&](const juce::Uuid& id)
    {
        if (!id.isNull() && std::find(evidence.operationIds.begin(), evidence.operationIds.end(), id) == evidence.operationIds.end())
            evidence.operationIds.push_back(id);
    };
    if (!report.launchOperation.isNull())
    { evidence.fields.push_back({ Field::restoreLaunchId, report.launchOperation.toString() }); operation(report.launchOperation); }
    const auto reason = [&](Field field, const recovery::Status& value)
    {
        if (value) evidence.fields.push_back({ field, "none" });
        else if (value.code.isNotEmpty() && value.code.length() <= 96 && value.code.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_"))
            evidence.fields.push_back({ field, value.code });
        else evidence.fields.push_back({ field, {}, Unavailable::invalidValue });
    };
    reason(Field::restoreSchedulingReason, report.schedulingFailure);
    if (!report.selected) return;
    const auto& job = *report.selected;
    operation(job.attempt);
    const auto owner = job.owner.toString();
    evidence.fields.insert(evidence.fields.end(), {
        { Field::restoreAttempt, job.attempt.toString() },
        { Field::restoreOwner, juce::SHA256(owner.toRawUTF8(), owner.getNumBytesAsUTF8()).toHexString() },
        { Field::restoreSequence, job.checkpoint.sequence }, { Field::restoreStage, restoreStageName(job.stage) },
        { Field::restoreWaitingAdmission, static_cast<int>(job.waitingForAdmission) }, { Field::restoreCoreAdmitted, static_cast<int>(job.coreAdmitted) } });
    evidence.fields.push_back(job.checkpoint.hash.length() == 64 && job.checkpoint.hash.containsOnly("0123456789abcdef")
        ? EvidenceField { Field::restoreCheckpoint, job.checkpoint.hash } : EvidenceField { Field::restoreCheckpoint, {}, Unavailable::notReported });
    evidence.fields.push_back(!job.failure ? EvidenceField { Field::restoreFailureStage, restoreStageName(job.failureStage) }
        : EvidenceField { Field::restoreFailureStage, {}, Unavailable::notApplicable });
    reason(Field::restoreReason, job.failure); reason(Field::restoreAttemptCloseReason, job.attemptCloseFailure);
}
const char* deviceRequestKind(AudioEngine::DeviceRequestKind kind)
{
    switch (kind)
    {
        case AudioEngine::DeviceRequestKind::initialise: return "initialise";
        case AudioEngine::DeviceRequestKind::selectBackend: return "select-backend";
        case AudioEngine::DeviceRequestKind::applySetup: return "apply-setup";
        case AudioEngine::DeviceRequestKind::enableOutputs: return "enable-outputs";
        case AudioEngine::DeviceRequestKind::activateOutput: return "direct-activate";
        case AudioEngine::DeviceRequestKind::inspectOutput: return "direct-inspection";
        case AudioEngine::DeviceRequestKind::inspectPanel: return "direct-panel-inspection";
        case AudioEngine::DeviceRequestKind::maintainOutput: return "direct-maintenance";
        case AudioEngine::DeviceRequestKind::showOutputPanel: return "direct-control-panel";
        case AudioEngine::DeviceRequestKind::suspendOutput: return "direct-suspend";
    }
    return "not-reported";
}
const char* deviceOutcome(AudioEngine::DeviceRequestOutcome outcome)
{
    switch (outcome)
    {
        case AudioEngine::DeviceRequestOutcome::succeeded: return "succeeded";
        case AudioEngine::DeviceRequestOutcome::rejected: return "request-rejected";
        case AudioEngine::DeviceRequestOutcome::noOpenDevice: return "no-open-device";
        case AudioEngine::DeviceRequestOutcome::stopped: return "stopped";
        case AudioEngine::DeviceRequestOutcome::pending: return "pending";
        case AudioEngine::DeviceRequestOutcome::reviewed: return "reviewed";
        case AudioEngine::DeviceRequestOutcome::cancelled: return "cancelled";
        case AudioEngine::DeviceRequestOutcome::superseded: return "superseded";
    }
    return "not-reported";
}
const char* switchPhaseName(DeviceSwitchPhase phase)
{
    switch (phase)
    {
        case DeviceSwitchPhase::inactive: return "inactive";
        case DeviceSwitchPhase::preparing: return "preparing";
        case DeviceSwitchPhase::readyToSwitch: return "ready-to-switch";
        case DeviceSwitchPhase::switching: return "switching";
        case DeviceSwitchPhase::active: return "active";
        case DeviceSwitchPhase::failed: return "failed";
        case DeviceSwitchPhase::restoring: return "restoring";
        case DeviceSwitchPhase::readyForReview: return "ready-for-review";
    }
    return "not-reported";
}
const char* switchErrorName(DeviceSwitchError error)
{
    switch (error)
    {
        case DeviceSwitchError::none: return "none";
        case DeviceSwitchError::invalidSelection: return "invalid-selection";
        case DeviceSwitchError::engineBusy: return "engine-busy";
        case DeviceSwitchError::launchFailed: return "launch-failed";
        case DeviceSwitchError::cancelled: return "cancelled";
        case DeviceSwitchError::superseded: return "superseded";
        case DeviceSwitchError::closeFailed: return "close-failed";
        case DeviceSwitchError::prepareFailed: return "prepare-failed";
        case DeviceSwitchError::staleCandidate: return "stale-candidate";
        case DeviceSwitchError::commitFailed: return "commit-failed";
        case DeviceSwitchError::restoreFailed: return "restore-failed";
        case DeviceSwitchError::driverUnresolved: return "driver-unresolved";
        case DeviceSwitchError::rebindRequired: return "rebind-required";
        case DeviceSwitchError::circuitOpen: return "circuit-open";
    }
    return "not-reported";
}
const char* requestRestoration(const DeviceRequestRecord& request)
{
    return !request.restorationAttempted ? "not-attempted" : !request.resourcesReleased ? "pending"
        : request.restored ? "restored" : "failed";
}
void appendDeviceRequest(ProviderEvidence& evidence, const AudioEngine::DeviceDiagnostics& report)
{
    using Availability = AudioEngine::DeviceDiagnostics::Availability;
    evidence.fields.push_back({ Field::deviceState, report.availability == Availability::busy ? "busy"
        : report.availability == Availability::cold ? "uninitialised" : report.active ? "open" : "no-open-device" });
    if (!report.latestRequest && (report.availability == Availability::busy || report.requestObservationBusy))
        evidence.fields.push_back({ Field::negotiationStage, {}, Unavailable::providerBusy });
    if (report.latestRequest)
    {
        const auto& request = *report.latestRequest;
        evidence.operationIds.push_back(request.operationId);
        evidence.fields.push_back({ Field::negotiationStage, request.direct ? switchPhaseName(request.switchPhase) : deviceRequestKind(request.stage) });
        evidence.fields.push_back({ Field::negotiationRequestKind, deviceRequestKind(request.kind) });
        evidence.fields.push_back({ Field::negotiationOutcome, deviceOutcome(request.outcome) });
        if (request.requestedBackend.isNotEmpty()) evidence.fields.push_back({ Field::requestedBackend, request.requestedBackend });
        const auto noFormatRequest = request.direct && request.kind != DeviceRequestKind::activateOutput;
        evidence.fields.push_back({ Field::requestedRateMode, noFormatRequest ? "not-requested" : request.requestedRate == 0.0 ? "default" : "explicit" });
        evidence.fields.push_back({ Field::requestedBufferMode, noFormatRequest ? "not-requested" : request.requestedBuffer == 0 ? "default" : "explicit" });
        evidence.fields.push_back({ Field::requestedChannelPolicy, noFormatRequest ? "not-requested" : request.defaultOutputChannels ? "device-default" : "explicit-channels" });
        evidence.fields.push_back(request.requestedRate != 0.0 ? EvidenceField{ Field::requestedRate, request.requestedRate }
            : EvidenceField{ Field::requestedRate, {}, Unavailable::notApplicable });
        evidence.fields.push_back(request.requestedBuffer != 0 ? EvidenceField{ Field::requestedBuffer, request.requestedBuffer }
            : EvidenceField{ Field::requestedBuffer, {}, Unavailable::notApplicable });
        if (request.requestedOutputChannels >= 0) evidence.fields.push_back({ Field::requestedChannels, request.requestedOutputChannels });
        if (request.followupOutputChannels >= 0) evidence.fields.push_back({ Field::followupRequestedChannels, request.followupOutputChannels });
        if (request.direct)
        {
            evidence.fields.push_back({ Field::negotiationRestore, requestRestoration(request) });
            evidence.fields.push_back({ Field::negotiationResources, request.resourcesReleased ? "released" : "pending" });
            if (report.active && report.activeOutputIdentity.isNotEmpty() && request.requestedOutputIdentity.isNotEmpty())
                evidence.fields.push_back({ Field::negotiationTargetRelation,
                    report.activeOutputIdentity == request.requestedOutputIdentity && report.active->backend == request.requestedBackend
                        ? "active-target" : "different-target" });
            else evidence.fields.push_back({ Field::negotiationTargetRelation, {},
                report.availability == Availability::busy ? Unavailable::providerBusy
                    : report.active ? Unavailable::notSupported : Unavailable::notApplicable });
        }
    }
    if (report.latestFailure)
    {
        const auto& failure = *report.latestFailure;
        if (!report.latestRequest || failure.operationId != report.latestRequest->operationId) evidence.operationIds.push_back(failure.operationId);
        const auto cancelledRestoreFailure = failure.direct && failure.resourcesReleased && failure.restorationAttempted && !failure.restored
            && (failure.switchError == DeviceSwitchError::cancelled || failure.switchError == DeviceSwitchError::superseded);
        const auto error = cancelledRestoreFailure ? DeviceSwitchError::restoreFailed : failure.switchError;
        const auto phase = cancelledRestoreFailure ? DeviceSwitchPhase::restoring : failure.failurePhase;
        evidence.fields.push_back({ Field::lastNegotiationFailure, failure.direct ? switchErrorName(error) : deviceOutcome(failure.outcome) });
        evidence.fields.push_back({ Field::lastNegotiationFailureStage, failure.direct ? switchPhaseName(phase) : deviceRequestKind(failure.stage) });
        evidence.fields.push_back({ Field::lastNegotiationFailureSource, failure.direct ? "device-switch-coordinator" : "juce-device-manager" });
        if (failure.direct) evidence.fields.push_back({ Field::lastNegotiationFailureRestore, requestRestoration(failure) });
        FailureCapsule capsule;
        capsule.operationId = failure.operationId;
        capsule.stage = FailureStage::negotiation;
        capsule.code = failure.direct ? static_cast<int64_t>(error) : static_cast<int64_t>(failure.outcome); // scoped integration code, never a native driver code
        const auto boundedRate = std::isfinite(failure.requestedRate) && failure.requestedRate > 0.0 && failure.requestedRate < 1.0e9
            ? static_cast<int64_t>(std::llround(failure.requestedRate)) : 0;
        capsule.counters = { boundedRate, failure.requestedBuffer, failure.requestedOutputChannels,
            failure.defaultOutputChannels ? 1 : 0, static_cast<int64_t>(failure.stage), 0 };
        evidence.latestFailure = capsule;
    }
}
const char* moduleName(State::Module module)
{
    switch (module)
    {
        case State::Module::videoAudio: return "video-audio";
        case State::Module::packedAudio: return "packed-audio";
        case State::Module::bank: return "bank-preview";
        case State::Module::sidecar: return "bank-sidecar";
    }
    return "not-reported";
}
const char* operationState(const Operation::Report& report)
{
    if (report.cancelled) return "cancelled";
    if (report.denied != State::Denied::none) return "denied";
    if (report.failure) return "failed";
    if (report.succeeded) return "succeeded";
    if (report.terminal) return "ended";
    if (report.readerAdmitted && !report.decoderComplete) return "reader-ready-decoding";
    if (report.decoderComplete) return "decoded-awaiting-reader";
    return report.key ? "admitted" : "awaiting-admission";
}
const char* failureReason(State::Reason reason)
{
    switch (reason)
    {
        case State::Reason::decoderExit: return "decoder-exit";
        case State::Reason::deadline: return "deadline";
        case State::Reason::memoryQuota: return "memory-quota";
        case State::Reason::outputQuota: return "output-quota";
        case State::Reason::parserBounds: return "parser-bounds";
        case State::Reason::missingDependency: return "missing-dependency";
        case State::Reason::containment: return "containment";
        case State::Reason::cacheInvalid: return "cache-invalid";
        case State::Reason::ioFailure: return "io-failure";
    }
    return "not-reported";
}
FailureStage failureStage(State::Stage stage)
{
    switch (stage)
    {
        case State::Stage::admission: return FailureStage::admission;
        case State::Stage::launch: return FailureStage::launch;
        case State::Stage::decode: return FailureStage::decode;
        case State::Stage::cache: return FailureStage::cache;
        case State::Stage::parse: return FailureStage::parse;
        case State::Stage::cleanup: return FailureStage::cleanup;
    }
    return FailureStage::admission;
}
void appendCodecFailure(ProviderEvidence& evidence, const State::Failure& failure)
{
    evidence.fields.push_back({ Field::lastFailureReason, failureReason(failure.reason) });
    const char* source = failure.source == State::FailureSource::bankIndex ? "bank-index"
        : failure.source == State::FailureSource::bankSidecar ? "bank-sidecar"
        : failure.source == State::FailureSource::decoder ? "media-operation" : "not-reported";
    evidence.fields.push_back({ Field::codecFailureSource, source });
    if (failure.source == State::FailureSource::bankIndex || failure.source == State::FailureSource::bankSidecar)
        evidence.fields.push_back({ Field::codecFailureDetail, static_cast<juce::int64>(failure.detailCode) });
    if (failure.operationId.isNull()) return;
    if (std::find(evidence.operationIds.begin(), evidence.operationIds.end(), failure.operationId) == evidence.operationIds.end())
        evidence.operationIds.push_back(failure.operationId);
    FailureCapsule capsule;
    capsule.operationId = failure.operationId;
    capsule.stage = failureStage(failure.stage); capsule.code = static_cast<int64_t>(failure.reason);
    capsule.counters = { static_cast<int64_t>(failure.exitCode), static_cast<int64_t>(failure.observedBytes),
        static_cast<int64_t>(failure.allowedBytes), static_cast<int64_t>(failure.elapsedMilliseconds),
        failure.jobCleanupConfirmed ? 1 : 0, 0 };
    evidence.latestFailure = capsule;
}
const char* bankSlotState(const BnkOpenRequest::Report& slot, const BnkOpenRequest::DiagnosticReport& request)
{
    if (!slot.key && !slot.terminal && slot.failure == BnkParser::Failure::none) return "not-attempted";
    if (slot.succeeded) return "published";
    if (slot.failure == BnkParser::Failure::cancelled || request.cancelled) return "cancelled";
    if (slot.failure != BnkParser::Failure::none) return "failed";
    if (slot.denied != State::Denied::none) return "denied";
    return slot.terminal || request.abandoned ? "not-published" : "preparing";
}
void appendBankOpen(ProviderEvidence& evidence, const BnkOpenRequest::DiagnosticReport& report)
{
    evidence.operationIds.push_back(report.operationId);
    evidence.fields.push_back({ Field::bankOpenState, report.core.succeeded ? "opened" : report.cancelled ? "cancelled"
        : report.core.failure != BnkParser::Failure::none ? "failed" : report.core.denied != State::Denied::none ? "denied"
        : report.abandoned ? "abandoned" : report.started ? "preparing" : "queued" });
    const auto append = [&](const BnkOpenRequest::Report& slot, Field state, Field outcome, Field profile, Field denied)
    {
        evidence.fields.push_back({ state, bankSlotState(slot, report) });
        evidence.fields.push_back({ outcome, static_cast<int>(slot.failure) });
        evidence.fields.push_back({ denied, static_cast<int>(slot.denied) });
        if (slot.key) evidence.fields.push_back({ profile, slot.key->profile == State::Profile::expanded ? "expanded" : "conservative" });
    };
    append(report.core, Field::bankCoreState, Field::bankCoreOutcome, Field::bankCorePolicy, Field::bankCoreDenied);
    append(report.sidecar, Field::bankSidecarState, Field::bankSidecarOutcome, Field::bankSidecarPolicy, Field::bankSidecarDenied);
    if (report.core.key) evidence.fields.push_back({ Field::bankCoreCircuit, report.core.circuitOpen ? "open" : "closed" });
    if (report.sidecar.key) evidence.fields.push_back({ Field::bankSidecarCircuit, report.sidecar.circuitOpen ? "open" : "closed" });
}
void appendOperation(ProviderEvidence& evidence, const Operation::Report& report)
{
    evidence.operationIds.push_back(report.operationId);
    evidence.fields.push_back({ Field::lastOperationModule, moduleName(report.module) });
    evidence.fields.push_back({ Field::lastOperationState, operationState(report) });
    evidence.fields.push_back({ Field::lastOperationDenied, static_cast<int>(report.denied) });
    if (report.key) evidence.fields.push_back({ Field::lastOperationProfile,
        report.key->profile == State::Profile::expanded ? "expanded" : "conservative" });
    if (report.totalFrames > 0)
    {
        evidence.fields.push_back({ Field::lastOperationReadyFrames, static_cast<juce::int64>(report.readyFrames) });
        evidence.fields.push_back({ Field::lastOperationTotalFrames, static_cast<juce::int64>(report.totalFrames) });
    }
    switch (report.reader)
    {
        case Operation::ReaderKind::notReported: break;
        case Operation::ReaderKind::videoCache: evidence.fields.push_back({ Field::lastOperationReader, "video-cache-reader" }); break;
        case Operation::ReaderKind::packedProgressiveWave: evidence.fields.push_back({ Field::lastOperationReader, "packed-progressive-wave" }); break;
        case Operation::ReaderKind::bankDecodedWave: evidence.fields.push_back({ Field::lastOperationReader, "bank-decoded-wave" }); break;
    }
    if (report.failure) appendCodecFailure(evidence, *report.failure);
    // Physical source/dependency fingerprints are deliberately not public facts.
}
}
void enrichNativeDeviceMetadata(std::vector<CapturedProvider>& providers, const std::atomic<bool>& cancelled)
{
    for (auto& provider : providers)
    {
        if (cancelled.load()) return;
        if (provider.kind != ProviderKind::audio) continue;
        const auto open = std::any_of(provider.evidence.fields.begin(), provider.evidence.fields.end(),
            [](const auto& item) { return item.field == Field::deviceState && item.value.toString() == "open"; });
        if (!open) continue;
        // ASIO facts were copied from the exact active owner's initialized
        // instance. Do not append Windows endpoint fields over that source.
        if (std::any_of(provider.evidence.fields.begin(), provider.evidence.fields.end(),
            [](const auto& item) { return item.field == Field::deviceMetadataState; }))
        { provider.outputEndpoint.clear(); continue; }
        auto metadata = readNativeDeviceMetadata(provider.outputEndpoint, cancelled);
        provider.outputEndpoint.clear();
        provider.evidence.fields.insert(provider.evidence.fields.end(), metadata.fields.begin(), metadata.fields.end());
    }
}
std::vector<CapturedProvider> captureLiveDiagnosticProviders(const AudioEngine& engine, const SingleTrackDocument& document,
                                                            const recovery::RecoveryService* recovery,
                                                            const Operation::DiagnosticHandle& optionalOperation,
                                                            const BnkOpenRequest::DiagnosticHandle& bankOpen,
                                                            const State* optionalPolicy,
                                                            const recovery::RecoveryInspection::DiagnosticObservation* inspection,
                                                            const recovery::RecoveryDiagnosticState* recoveryDiagnostics,
                                                            const recovery::RecoveryRestoreQueue* restoreQueue,
                                                            const juce::Uuid& selectedRestore)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(providers);
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    std::vector<CapturedProvider> result;
    auto& audio = result.emplace_back();
    audio.kind = ProviderKind::audio;
    const auto device = engine.getDeviceDiagnostics();
    if (const auto& metrics = device.active)
    {
        audio.evidence.fields = { { Field::backend, metrics->backend }, { Field::activeRate, metrics->sampleRate },
            { Field::activeBuffer, metrics->bufferSamples }, { Field::activeChannels, metrics->activeOutputChannels } };
        if (metrics->outputLatencySamples >= 0) audio.evidence.fields.push_back({ Field::driverLatency, metrics->outputLatencySamples });
        else audio.evidence.fields.push_back({ Field::driverLatency, {}, Unavailable::notSupported });
        const auto& rate = metrics->playbackRate;
        const auto ratePrepared = rate.mode == PlaybackRateSource::Mode::bypassed || rate.mode == PlaybackRateSource::Mode::converting;
        audio.evidence.fields.push_back({ Field::playbackSourceRate, ratePrepared ? juce::var(rate.sourceRate) : juce::var(),
            ratePrepared ? Unavailable::notReported : Unavailable::notApplicable });
        audio.evidence.fields.push_back({ Field::playbackRateMode,
            rate.mode == PlaybackRateSource::Mode::bypassed ? "bypassed" : rate.mode == PlaybackRateSource::Mode::converting ? "converting"
            : rate.mode == PlaybackRateSource::Mode::unavailable ? "unavailable" : "unprepared" });
        audio.evidence.fields.push_back({ Field::playbackRateAlgorithm,
            rate.mode == PlaybackRateSource::Mode::converting ? juce::var(PlaybackRateSource::Snapshot::algorithm) : juce::var(), Unavailable::notApplicable });
        audio.evidence.fields.push_back({ Field::playbackRateLookahead, ratePrepared ? juce::var(rate.bufferedLookaheadFrames) : juce::var(),
            ratePrepared ? Unavailable::notReported : Unavailable::notApplicable });
        audio.evidence.fields.push_back({ Field::playbackProcessingLatency, ratePrepared ? juce::var(metrics->processingLatencySamples) : juce::var(), Unavailable::notApplicable });
        audio.evidence.fields.push_back({ Field::playbackPreviewLookahead, ratePrepared ? juce::var(metrics->previewLookaheadSourceSamples) : juce::var(), Unavailable::notApplicable });
        const auto missingLatency = ratePrepared ? Unavailable::notSupported : Unavailable::notApplicable;
        audio.evidence.fields.push_back({ Field::playbackAggregateLatency,
            metrics->aggregateOutputLatencySamples ? juce::var(*metrics->aggregateOutputLatencySamples) : juce::var(), missingLatency });
        audio.evidence.fields.push_back({ Field::playbackAggregateLatencyMs,
            metrics->aggregateOutputLatencyMs ? juce::var(*metrics->aggregateOutputLatencyMs) : juce::var(), missingLatency });
        audio.evidence.identifiers.push_back({ LocalIdentifierKind::deviceName, metrics->deviceName });
        audio.outputEndpoint = device.outputEndpoint;
        if (audio.outputEndpoint.isNotEmpty()) audio.evidence.fields.push_back({ Field::endpointIdentity, audio.outputEndpoint });
        else audio.evidence.fields.push_back({ Field::endpointIdentity, {}, Unavailable::notSupported });
        if (const auto& identity = device.driverIdentity; identity && metrics->backend == "ASIO")
        {
            audio.evidence.fields.push_back({ Field::driverProduct,
                identity->product.empty() ? juce::var() : juce::var(juce::String(identity->product)), Unavailable::notReported });
            audio.evidence.fields.push_back({ Field::driverVersion,
                identity->rawAsioVersion ? juce::var(juce::String(*identity->rawAsioVersion)) : juce::var(),
                identity->versionReadFailed ? Unavailable::captureFailed : Unavailable::notReported });
            audio.evidence.fields.push_back({ Field::driverMetadataSource, "initialized-IASIO-name-and-raw-version" });
            audio.evidence.fields.push_back({ Field::deviceMetadataState, "driver-interface-cached" });
            for (const auto field : {Field::driverVendor, Field::hardwareManufacturer, Field::speakerMask, Field::speakerMaskSource})
                audio.evidence.fields.push_back({ field, {}, Unavailable::notSupported });
        }
    }
    if (device.outputChannelMapping)
    {
        juce::Array<juce::var> mapping;
        for (const auto channel : *device.outputChannelMapping) mapping.add(channel);
        audio.evidence.fields.push_back({ Field::channelMapping, juce::var(mapping) });
    }
    else
        audio.evidence.fields.push_back({ Field::channelMapping, {},
            device.availability == AudioEngine::DeviceDiagnostics::Availability::busy ? Unavailable::providerBusy
            : device.active ? Unavailable::notSupported : Unavailable::notApplicable });
    appendDeviceRequest(audio.evidence, device);
    appendPhysicalOutputBinding(audio.evidence,engine,device);
    // No getDeviceManager(), scan, setup negotiation, or new optional owner.
    const auto documentState = document.getState();
    const auto& info = documentState->info;
    const auto operation = optionalOperation ? optionalOperation->snapshot() : std::optional<Operation::Report>{};
    if (document.hasAudio() || optionalOperation || bankOpen || optionalPolicy)
    {
        auto& codec = result.emplace_back(); codec.kind = ProviderKind::codec;
        if (document.hasAudio())
        {
            codec.evidence.fields = { { Field::mediaRate, info.sampleRate }, { Field::mediaChannels, info.numChannels },
                { Field::mediaFrames, static_cast<juce::int64>(info.numSamples) },
                { Field::policyOutcome, info.recoveryBacked ? "recovery-backed" : "document-loaded" } };
            if (const auto* format = knownNativeFormat(info.formatName)) codec.evidence.fields.push_back({ Field::container, format });
            else if (info.formatName == "Wwise WEM" || info.formatName == "WEM") codec.evidence.fields.push_back({ Field::container, "WEM" });
            appendCurrentReader(codec.evidence, *documentState);
            if (info.file != juce::File())
            {
                codec.evidence.identifiers.push_back({ LocalIdentifierKind::path, info.file.getFullPathName() });
                codec.evidence.identifiers.push_back({ LocalIdentifierKind::fileName, info.file.getFileName() });
            }
        }
        if (operation) appendOperation(codec.evidence, *operation);
        else if (optionalOperation) codec.evidence.fields.push_back({ Field::lastOperationState, {}, Unavailable::providerBusy });
        if (bankOpen)
        {
            if (const auto report = bankOpen->snapshot()) appendBankOpen(codec.evidence, *report);
            else codec.evidence.fields.push_back({ Field::bankOpenState, {}, Unavailable::providerBusy });
        }
        if (optionalPolicy)
        {
            // One latest failure across the actual shared optional owner. A
            // newer bank failure must not inherit an old decoder's identity.
            codec.evidence.latestFailure.reset();
            auto& fields = codec.evidence.fields;
            fields.erase(std::remove_if(fields.begin(), fields.end(), [](const auto& item)
                { return item.field == Field::lastFailureReason || item.field == Field::codecFailureSource || item.field == Field::codecFailureDetail; }), fields.end());
            std::optional<State::Capsule> latest;
            if (!optionalPolicy->tryLatestFailure(latest))
                fields.push_back({ Field::lastFailureReason, {}, Unavailable::providerBusy });
            else if (latest) appendCodecFailure(codec.evidence, latest->failure);
        }
    }
    if (document.hasAudio())
    {
        auto& spatial = result.emplace_back(); spatial.kind = ProviderKind::spatial;
        const auto mode = engine.getMonitorMode();
        spatial.evidence.fields.push_back({ Field::requestedMonitorMode, monitorModeName(mode) });
        appendRenderObservation(spatial.evidence, device.render);
        const auto& layout = info.channelLayout;
        if (!layout.isDiscreteFallback && layout.selectedRoles.size() == static_cast<size_t>(info.numChannels)
            && info.numChannels > 0 && info.numChannels <= 32)
        {
            juce::StringArray order;
            for (const auto role : layout.selectedRoles) order.add(ChannelLayoutCatalog::roleToAbbreviation(role));
            spatial.evidence.fields.push_back({ Field::channelOrder, order.joinIntoString(" ") });
        }
        else if (layout.isDiscreteFallback)
            spatial.evidence.fields.push_back({ Field::channelOrder, "discrete-" + juce::String(info.numChannels) });
    }
    if (operation && operation->cache.schema > 0)
    {
        const auto& observed = operation->cache;
        auto& cache = result.emplace_back(); cache.kind = ProviderKind::cache;
        cache.evidence.operationIds.push_back(operation->operationId);
        const char* lookup = observed.lastLookup == Operation::CacheLookup::reused ? "reused"
            : observed.lastLookup == Operation::CacheLookup::miss ? "not-reused"
            : observed.lastLookup == Operation::CacheLookup::unavailable ? "unavailable" : "not-observed";
        cache.evidence.fields = { { Field::cacheKind, "last-operation-decoded-wave" }, { Field::cacheSchema, observed.schema },
            { Field::cacheProducer, operation->module == State::Module::videoAudio ? "ffmpeg-interface" : "vgmstream-interface" },
            { Field::cacheResult, lookup },
            { Field::cacheLookupReason, media::DecodedWaveCache::lookupReasonName(observed.lookupReason) },
            { Field::cachePublication, !observed.lastPublication ? "not-attempted" : *observed.lastPublication ? "published" : "not-published" },
            { Field::cacheLookups, static_cast<juce::int64>(observed.lookups) }, { Field::cacheHits, static_cast<juce::int64>(observed.hits) },
            { Field::cachePublications, static_cast<juce::int64>(observed.publications) },
            { Field::cachePublicationFailures, static_cast<juce::int64>(observed.publicationFailures) } };
        if (operation->key) cache.evidence.fields.push_back({ Field::cachePolicy,
            operation->key->profile == State::Profile::expanded ? "expanded" : "conservative" });
    }
    if (recovery != nullptr)
    {
        const auto health = recovery->health();
        auto& protection = result.emplace_back(); protection.kind = ProviderKind::recovery;
        auto failure = !health.protection.failure ? health.protection.failure : health.dispositionFailure;
        if (failure && !health.maintenance.failure) failure = health.maintenance.failure;
        protection.evidence.fields = { { Field::recoveryStage, !failure ? "degraded" : health.settled ? "closed-settled"
            : health.needsPoll ? "work-pending" : "observing" },
            { Field::typedOutcome, juce::String(static_cast<int>(failure.error)) } };
        for (const auto& [field, status] : std::array<std::pair<Field, recovery::Status>, 3> {{
            { Field::recoveryProtectionReason, health.protection.failure },
            { Field::recoveryMaintenanceReason, health.maintenance.failure },
            { Field::recoveryDispositionReason, health.dispositionFailure } }})
        {
            if (status) protection.evidence.fields.push_back({ field, "none" });
            else if (status.code.isNotEmpty() && status.code.length() <= 96
                     && status.code.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_"))
                protection.evidence.fields.push_back({ field, status.code });
            else protection.evidence.fields.push_back({ field, {}, Unavailable::invalidValue });
        }
        protection.evidence.fields.insert(protection.evidence.fields.end(), {
            { Field::recoveryPendingPackets, static_cast<juce::int64>(health.protection.pendingPackets) },
            { Field::recoveryRetainedBytes, static_cast<juce::int64>(health.protection.retainedBytes) },
            { Field::recoveryWaitingRetirements, static_cast<juce::int64>(health.waitingRetirements + health.protection.pendingRetirements) },
            { Field::recoveryMaintenanceJobs, static_cast<juce::int64>(health.maintenance.jobs.size()) },
            { Field::recoveryCompletedOwners, static_cast<juce::int64>(health.completedOwners) } });
        // checkpointGeneration remains unavailable: journal sequence and a
        // Document version are distinct facts and neither signs authority.
    }
    if (recovery != nullptr || inspection != nullptr || recoveryDiagnostics != nullptr || restoreQueue != nullptr)
    {
        if (recovery == nullptr) result.emplace_back().kind = ProviderKind::recovery;
        auto& evidence = result.back().evidence;
        if (inspection != nullptr) appendInspection(evidence, *inspection);
        if (restoreQueue != nullptr) appendRestore(evidence, *restoreQueue, selectedRestore);
        if (recovery != nullptr || recoveryDiagnostics != nullptr) appendRecoveryFailure(evidence, recovery, recoveryDiagnostics);
    }
    return result;
}
}
