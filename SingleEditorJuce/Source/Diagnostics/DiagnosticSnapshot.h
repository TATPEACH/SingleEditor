#pragma once
#include "DiagnosticBundle.h"
#include <array>
#include <optional>
#include <set>

namespace singleeditor::diagnostics
{
enum class ProviderKind { audio, spatial, codec, cache, recovery };
using ProviderScope = std::optional<std::set<ProviderKind>>; // nullopt = all currently loaded sections
enum class Unavailable { notReported, notApplicable, notSupported, providerBusy, captureFailed, invalidValue, privacyExcluded };
enum class Field
{
    backend, driverVendor, driverProduct, driverVersion, requestedRate, activeRate, requestedBuffer, activeBuffer,
    requestedChannels, activeChannels, speakerLayout, speakerMask, speakerGeometry, channelMapping, driverLatency,
    playbackSourceRate, playbackRateMode, playbackRateAlgorithm, playbackRateLookahead,
    playbackProcessingLatency, playbackAggregateLatency, playbackAggregateLatencyMs, playbackPreviewLookahead,
    negotiationStage, endpointIdentity, channelOrder, outputMatrix, downmixState, decoderState,
    container, selectedReader, fallbackReason, dependencyIdentity, policyOutcome, mediaRate, mediaChannels, mediaFrames,
    cacheKind, cacheSchema, cacheProducer, cachePolicy, cacheResult, cachePublication,
    recoverySchema, checkpointGeneration, recoveryStage, isolationReason, typedOutcome,
    lastOperationModule, lastOperationState, lastOperationProfile, lastOperationReader,
    lastOperationReadyFrames, lastOperationTotalFrames, lastOperationDenied, lastFailureReason,
    cacheLookups, cacheHits, cachePublications, cachePublicationFailures, cacheLookupReason,
    deviceState, requestedBackend, requestedRateMode, requestedBufferMode,
    negotiationRestore, negotiationResources, negotiationTargetRelation, lastNegotiationFailureSource, lastNegotiationFailureRestore,
    requestedChannelPolicy, negotiationOutcome, lastNegotiationFailure,
    negotiationRequestKind, followupRequestedChannels, lastNegotiationFailureStage,
    deviceMetadataState, driverMetadataSource, hardwareManufacturer, speakerLayoutSource,
    deviceProbeAction, deviceProbeChannel, deviceProbePeakDbfs, deviceProbeDurationMs,
    deviceProbeFrames, deviceProbeWorkerFailure, deviceProbeCleanup, deviceProbeRestore,
    bankOpenState, bankCoreState, bankCoreOutcome, bankCorePolicy, bankCoreDenied,
    bankSidecarState, bankSidecarOutcome, bankSidecarPolicy, bankSidecarDenied,
    requestedMonitorMode, outputMatrixSourceChannels, outputMatrixOutputChannels, routingObservation,
    codecFailureSource, codecFailureDetail, bankCoreCircuit, bankSidecarCircuit,
    recoveryFailureStage, recoveryFailureOwner, recoveryFailureOrder, recoveryFailureCoverage,
    recoveryProtectionReason, recoveryMaintenanceReason, recoveryDispositionReason,
    recoveryPendingPackets, recoveryRetainedBytes, recoveryWaitingRetirements, recoveryMaintenanceJobs, recoveryCompletedOwners,
    inspectionOperationId,
    inspectionOwner,
    inspectionObservedHead,
    inspectionSelectedHead,
    inspectionStatus,
    inspectionAttemptsStatus,
    inspectionSourceAction,
    inspectionSourceStatus,
    inspectionSourceConfidence,
    inspectionAudioStatus,
    inspectionObservedSequence,
    inspectionSelectedSequence,
    inspectionMetadataReady,
    inspectionSourceChecked,
    inspectionSourceReason,
    inspectionAudioRequested,
    inspectionAudioVerified,
    inspectionQuarantined,
    inspectionPoints,
    inspectionAssets,
    inspectionAttempts,
    inspectionMetadataBytes,
    inspectionSourceWindowBytes,
    inspectionSourceFullBytes,
    inspectionAudioBytes,
    inspectionVerifiedAssets,
    restoreState,
    restoreLaunchId,
    restoreSchedulingReason,
    restoreAttempt,
    restoreOwner,
    restoreCheckpoint,
    restoreStage,
    restoreFailureStage,
    restoreReason,
    restoreAttemptCloseReason,
    restoreJobCount,
    restoreRunning,
    restoreResidentBytes,
    restoreSequence,
    restoreWaitingAdmission,
    restoreCoreAdmitted,
    outputBindingState, outputBindingRevision, physicalRouteSourceChannels, physicalRouteConnections, physicalRouteEncoding,
    speakerGeometryEncoding, speakerGeometrySource, lfePhysicalOutputs, unplacedPhysicalOutputs, speakerMaskSource
};
struct EvidenceField final
{
    Field field;
    juce::var value;
    Unavailable unavailable = Unavailable::notReported;
};
enum class LocalIdentifierKind { path, fileName, deviceName, endpointGuid, serial };
struct LocalIdentifier final { LocalIdentifierKind kind; juce::String value; };
enum class FailureStage { admission, negotiation, launch, decode, cache, parse, write, restore, cleanup };
struct FailureCapsule final
{
    juce::Uuid operationId;
    FailureStage stage = FailureStage::admission;
    int64_t code = 0;
    std::array<int64_t, 6> counters{};
};
struct ProviderEvidence final
{
    std::vector<EvidenceField> fields;
    std::vector<LocalIdentifier> identifiers;
    std::vector<juce::Uuid> operationIds;
    std::optional<FailureCapsule> latestFailure;
};
class LoadedDiagnosticProvider
{
public:
    virtual ~LoadedDiagnosticProvider() = default;
    // Caller holds an already-loaded owner alive. This method is observational:
    // no device/codec activation, repair, transport changes, or new provider.
    virtual ProviderEvidence snapshot() const = 0;
};
struct LoadedProviderRef final { ProviderKind kind; const LoadedDiagnosticProvider* owner = nullptr; };
struct SnapshotSelection final
{
    std::set<LocalIdentifierKind> localIdentifiers; // explicit category opt-ins; default empty
    bool reproductionConsent = false;
    std::vector<BundleEntry> chosenReproduction; // caller's explicitly chosen, bounded bytes; never swept files
};
struct SnapshotPreview final
{
    juce::Uuid snapshotId;
    PrivacyTier tier = PrivacyTier::supportSafe;
    std::vector<juce::String> categories;
    size_t providerCount = 0, attachmentCount = 0, payloadBytes = 0;
};
class PreparedSnapshot final
{
public:
    BundleStatus status() const noexcept { return state; }
    const SnapshotPreview& preview() const noexcept { return review; }
    const BundleInput& content() const noexcept { return bundle; } // immutable evidence for category/detail preview
private:
    friend class DiagnosticSnapshot;
    BundleStatus state{ BundleError::invalidInput };
    SnapshotPreview review;
    BundleInput bundle;
};
class DiagnosticSnapshot final
{
public:
    static constexpr size_t maximumReproductionFiles = 16;
    // No persistent collector/registry/service. Only explicitly supplied live
    // providers are visited during this call and none is retained on return.
    static PreparedSnapshot capture(const BundleIdentity&, const juce::String& origin,
                                    const std::vector<LoadedProviderRef>&, const SnapshotSelection& = {});
    // The GUI/CLI owner must obtain explicit approval of this exact preview ID.
    // A selection change requires a new capture and a new review.
    static BundleStatus publishReviewed(const PreparedSnapshot&, const juce::Uuid& approvedSnapshot,
                                        const juce::File&, const std::atomic<bool>* cancelled = nullptr);
};
}
