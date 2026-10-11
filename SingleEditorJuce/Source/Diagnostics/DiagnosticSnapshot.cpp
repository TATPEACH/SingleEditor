#include "DiagnosticSnapshot.h"
#include "DiagnosticTestActivity.h"
#include "BoundedJson.h"
#include <cmath>
#if JUCE_WINDOWS
 #include <windows.h>
 #include <bcrypt.h>
#endif

namespace singleeditor::diagnostics
{
namespace
{
juce::var object() { return juce::var(new juce::DynamicObject()); }
void put(juce::var& v, const char* key, const juce::var& value) { v.getDynamicObject()->setProperty(key, value); }
enum class ValueType { text, number, numbers, endpoint };
struct Descriptor final { Field field; const char* id; ProviderKind owner; ValueType type; };
constexpr Descriptor descriptors[] {
    { Field::backend, "backend", ProviderKind::audio, ValueType::text },
    { Field::driverVendor, "driverVendor", ProviderKind::audio, ValueType::text },
    { Field::driverProduct, "driverProduct", ProviderKind::audio, ValueType::text },
    { Field::driverVersion, "driverVersion", ProviderKind::audio, ValueType::text },
    { Field::requestedRate, "requestedRate", ProviderKind::audio, ValueType::number },
    { Field::activeRate, "activeRate", ProviderKind::audio, ValueType::number },
    { Field::requestedBuffer, "requestedBuffer", ProviderKind::audio, ValueType::number },
    { Field::activeBuffer, "activeBuffer", ProviderKind::audio, ValueType::number },
    { Field::requestedChannels, "requestedChannels", ProviderKind::audio, ValueType::number },
    { Field::negotiationRestore, "negotiationRestore", ProviderKind::audio, ValueType::text },
    { Field::negotiationResources, "negotiationResources", ProviderKind::audio, ValueType::text },
    { Field::negotiationTargetRelation, "negotiationTargetRelation", ProviderKind::audio, ValueType::text },
    { Field::lastNegotiationFailureSource, "lastNegotiationFailureSource", ProviderKind::audio, ValueType::text },
    { Field::lastNegotiationFailureRestore, "lastNegotiationFailureRestore", ProviderKind::audio, ValueType::text },
    { Field::activeChannels, "activeChannels", ProviderKind::audio, ValueType::number },
    { Field::speakerLayout, "speakerLayout", ProviderKind::audio, ValueType::text },
    { Field::speakerMask, "speakerMask", ProviderKind::audio, ValueType::number },
    { Field::speakerGeometry, "speakerGeometry", ProviderKind::audio, ValueType::numbers },
    { Field::outputBindingState, "outputBindingState", ProviderKind::audio, ValueType::text },
    { Field::outputBindingRevision, "outputBindingRevision", ProviderKind::audio, ValueType::number },
    { Field::physicalRouteSourceChannels, "physicalRouteSourceChannels", ProviderKind::audio, ValueType::number },
    { Field::physicalRouteConnections, "physicalRouteConnections", ProviderKind::audio, ValueType::numbers },
    { Field::physicalRouteEncoding, "physicalRouteEncoding", ProviderKind::audio, ValueType::text },
    { Field::speakerGeometryEncoding, "speakerGeometryEncoding", ProviderKind::audio, ValueType::text },
    { Field::speakerGeometrySource, "speakerGeometrySource", ProviderKind::audio, ValueType::text },
    { Field::lfePhysicalOutputs, "lfePhysicalOutputs", ProviderKind::audio, ValueType::numbers },
    { Field::unplacedPhysicalOutputs, "unplacedPhysicalOutputs", ProviderKind::audio, ValueType::numbers },
    { Field::speakerMaskSource, "speakerMaskSource", ProviderKind::audio, ValueType::text },
    { Field::channelMapping, "channelMapping", ProviderKind::audio, ValueType::numbers },
    { Field::driverLatency, "driverLatency", ProviderKind::audio, ValueType::number },
    { Field::playbackSourceRate, "playbackSourceRate", ProviderKind::audio, ValueType::number },
    { Field::playbackRateMode, "playbackRateMode", ProviderKind::audio, ValueType::text },
    { Field::playbackRateAlgorithm, "playbackRateAlgorithm", ProviderKind::audio, ValueType::text },
    { Field::playbackRateLookahead, "playbackRateLookahead", ProviderKind::audio, ValueType::number },
    { Field::playbackProcessingLatency, "playbackProcessingLatencySamples", ProviderKind::audio, ValueType::number },
    { Field::playbackAggregateLatency, "playbackAggregateLatencySamples", ProviderKind::audio, ValueType::number },
    { Field::playbackAggregateLatencyMs, "playbackAggregateLatencyMs", ProviderKind::audio, ValueType::number },
    { Field::playbackPreviewLookahead, "playbackPreviewLookaheadSourceSamples", ProviderKind::audio, ValueType::number },
    { Field::negotiationStage, "negotiationStage", ProviderKind::audio, ValueType::text },
    { Field::endpointIdentity, "endpointDigest", ProviderKind::audio, ValueType::endpoint },
    { Field::channelOrder, "channelOrder", ProviderKind::spatial, ValueType::text },
    { Field::outputMatrix, "outputMatrix", ProviderKind::spatial, ValueType::numbers },
    { Field::downmixState, "downmixState", ProviderKind::spatial, ValueType::text },
    { Field::decoderState, "decoderState", ProviderKind::spatial, ValueType::text },
    { Field::requestedMonitorMode, "requestedMonitorMode", ProviderKind::spatial, ValueType::text },
    { Field::outputMatrixSourceChannels, "outputMatrixSourceChannels", ProviderKind::spatial, ValueType::number },
    { Field::outputMatrixOutputChannels, "outputMatrixOutputChannels", ProviderKind::spatial, ValueType::number },
    { Field::routingObservation, "routingObservation", ProviderKind::spatial, ValueType::text },
    { Field::container, "container", ProviderKind::codec, ValueType::text },
    { Field::selectedReader, "selectedReader", ProviderKind::codec, ValueType::text },
    { Field::fallbackReason, "fallbackReason", ProviderKind::codec, ValueType::text },
    { Field::dependencyIdentity, "dependencyIdentity", ProviderKind::codec, ValueType::text },
    { Field::policyOutcome, "policyOutcome", ProviderKind::codec, ValueType::text },
    { Field::mediaRate, "mediaRate", ProviderKind::codec, ValueType::number },
    { Field::mediaChannels, "mediaChannels", ProviderKind::codec, ValueType::number },
    { Field::mediaFrames, "mediaFrames", ProviderKind::codec, ValueType::number },
    { Field::cacheKind, "cacheKind", ProviderKind::cache, ValueType::text },
    { Field::cacheSchema, "cacheSchema", ProviderKind::cache, ValueType::number },
    { Field::cacheProducer, "cacheProducer", ProviderKind::cache, ValueType::text },
    { Field::cachePolicy, "cachePolicy", ProviderKind::cache, ValueType::text },
    { Field::cacheResult, "cacheResult", ProviderKind::cache, ValueType::text },
    { Field::cachePublication, "cachePublication", ProviderKind::cache, ValueType::text },
    { Field::recoverySchema, "recoverySchema", ProviderKind::recovery, ValueType::number },
    { Field::checkpointGeneration, "checkpointGeneration", ProviderKind::recovery, ValueType::number },
    { Field::recoveryStage, "recoveryStage", ProviderKind::recovery, ValueType::text },
    { Field::isolationReason, "isolationReason", ProviderKind::recovery, ValueType::text },
    { Field::typedOutcome, "typedOutcome", ProviderKind::recovery, ValueType::text },
    { Field::lastOperationModule, "lastOperationModule", ProviderKind::codec, ValueType::text },
    { Field::lastOperationState, "lastOperationState", ProviderKind::codec, ValueType::text },
    { Field::lastOperationProfile, "lastOperationProfile", ProviderKind::codec, ValueType::text },
    { Field::lastOperationReader, "lastOperationReader", ProviderKind::codec, ValueType::text },
    { Field::lastOperationReadyFrames, "lastOperationReadyFrames", ProviderKind::codec, ValueType::number },
    { Field::lastOperationTotalFrames, "lastOperationTotalFrames", ProviderKind::codec, ValueType::number },
    { Field::lastOperationDenied, "lastOperationDenied", ProviderKind::codec, ValueType::number },
    { Field::lastFailureReason, "lastFailureReason", ProviderKind::codec, ValueType::text },
    { Field::cacheLookups, "cacheLookups", ProviderKind::cache, ValueType::number },
    { Field::cacheHits, "cacheHits", ProviderKind::cache, ValueType::number },
    { Field::cachePublications, "cachePublications", ProviderKind::cache, ValueType::number },
    { Field::cachePublicationFailures, "cachePublicationFailures", ProviderKind::cache, ValueType::number },
    { Field::cacheLookupReason, "cacheLookupReason", ProviderKind::cache, ValueType::text },
    { Field::deviceState, "deviceState", ProviderKind::audio, ValueType::text },
    { Field::requestedBackend, "requestedBackend", ProviderKind::audio, ValueType::text },
    { Field::requestedRateMode, "requestedRateMode", ProviderKind::audio, ValueType::text },
    { Field::requestedBufferMode, "requestedBufferMode", ProviderKind::audio, ValueType::text },
    { Field::requestedChannelPolicy, "requestedChannelPolicy", ProviderKind::audio, ValueType::text },
    { Field::negotiationOutcome, "negotiationOutcome", ProviderKind::audio, ValueType::text },
    { Field::lastNegotiationFailure, "lastNegotiationFailure", ProviderKind::audio, ValueType::text },
    { Field::negotiationRequestKind, "negotiationRequestKind", ProviderKind::audio, ValueType::text },
    { Field::followupRequestedChannels, "followupRequestedChannels", ProviderKind::audio, ValueType::number },
    { Field::lastNegotiationFailureStage, "lastNegotiationFailureStage", ProviderKind::audio, ValueType::text },
    { Field::deviceMetadataState, "deviceMetadataState", ProviderKind::audio, ValueType::text },
    { Field::driverMetadataSource, "driverMetadataSource", ProviderKind::audio, ValueType::text },
    { Field::hardwareManufacturer, "hardwareManufacturer", ProviderKind::audio, ValueType::text },
    { Field::speakerLayoutSource, "speakerLayoutSource", ProviderKind::audio, ValueType::text },
    { Field::deviceProbeAction, "deviceProbeAction", ProviderKind::audio, ValueType::text },
    { Field::deviceProbeChannel, "deviceProbeChannel", ProviderKind::audio, ValueType::number },
    { Field::deviceProbePeakDbfs, "deviceProbePeakDbfs", ProviderKind::audio, ValueType::number },
    { Field::deviceProbeDurationMs, "deviceProbeDurationMs", ProviderKind::audio, ValueType::number },
    { Field::deviceProbeFrames, "deviceProbeFrames", ProviderKind::audio, ValueType::number },
    { Field::deviceProbeWorkerFailure, "deviceProbeWorkerFailure", ProviderKind::audio, ValueType::number },
    { Field::deviceProbeCleanup, "deviceProbeCleanup", ProviderKind::audio, ValueType::text },
    { Field::deviceProbeRestore, "deviceProbeRestore", ProviderKind::audio, ValueType::text },
    { Field::bankOpenState, "bankOpenState", ProviderKind::codec, ValueType::text },
    { Field::bankCoreState, "bankCoreState", ProviderKind::codec, ValueType::text },
    { Field::bankCoreOutcome, "bankCoreOutcome", ProviderKind::codec, ValueType::number },
    { Field::bankCorePolicy, "bankCorePolicy", ProviderKind::codec, ValueType::text },
    { Field::bankCoreDenied, "bankCoreDenied", ProviderKind::codec, ValueType::number },
    { Field::bankSidecarState, "bankSidecarState", ProviderKind::codec, ValueType::text },
    { Field::bankSidecarOutcome, "bankSidecarOutcome", ProviderKind::codec, ValueType::number },
    { Field::bankSidecarPolicy, "bankSidecarPolicy", ProviderKind::codec, ValueType::text },
    { Field::bankSidecarDenied, "bankSidecarDenied", ProviderKind::codec, ValueType::number },
    { Field::codecFailureSource, "codecFailureSource", ProviderKind::codec, ValueType::text },
    { Field::codecFailureDetail, "codecFailureDetail", ProviderKind::codec, ValueType::number },
    { Field::bankCoreCircuit, "bankCoreCircuit", ProviderKind::codec, ValueType::text },
    { Field::bankSidecarCircuit, "bankSidecarCircuit", ProviderKind::codec, ValueType::text },
    { Field::recoveryFailureStage, "recoveryFailureStage", ProviderKind::recovery, ValueType::text },
    { Field::recoveryFailureOwner, "recoveryFailureOwner", ProviderKind::recovery, ValueType::text },
    { Field::recoveryFailureOrder, "recoveryFailureOrder", ProviderKind::recovery, ValueType::text },
    { Field::recoveryFailureCoverage, "recoveryFailureCoverage", ProviderKind::recovery, ValueType::text },
    { Field::recoveryProtectionReason, "recoveryProtectionReason", ProviderKind::recovery, ValueType::text },
    { Field::recoveryMaintenanceReason, "recoveryMaintenanceReason", ProviderKind::recovery, ValueType::text },
    { Field::recoveryDispositionReason, "recoveryDispositionReason", ProviderKind::recovery, ValueType::text },
    { Field::recoveryPendingPackets, "recoveryPendingPackets", ProviderKind::recovery, ValueType::number },
    { Field::recoveryRetainedBytes, "recoveryRetainedBytes", ProviderKind::recovery, ValueType::number },
    { Field::recoveryWaitingRetirements, "recoveryWaitingRetirements", ProviderKind::recovery, ValueType::number },
    { Field::recoveryMaintenanceJobs, "recoveryMaintenanceJobs", ProviderKind::recovery, ValueType::number },
    { Field::recoveryCompletedOwners, "recoveryCompletedOwners", ProviderKind::recovery, ValueType::number },
    { Field::inspectionOperationId, "inspectionOperationId", ProviderKind::recovery, ValueType::text },
    { Field::inspectionOwner, "inspectionOwner", ProviderKind::recovery, ValueType::text },
    { Field::inspectionObservedHead, "inspectionObservedHead", ProviderKind::recovery, ValueType::text },
    { Field::inspectionSelectedHead, "inspectionSelectedHead", ProviderKind::recovery, ValueType::text },
    { Field::inspectionStatus, "inspectionStatus", ProviderKind::recovery, ValueType::text },
    { Field::inspectionAttemptsStatus, "inspectionAttemptsStatus", ProviderKind::recovery, ValueType::text },
    { Field::inspectionSourceAction, "inspectionSourceAction", ProviderKind::recovery, ValueType::text },
    { Field::inspectionSourceStatus, "inspectionSourceStatus", ProviderKind::recovery, ValueType::text },
    { Field::inspectionSourceConfidence, "inspectionSourceConfidence", ProviderKind::recovery, ValueType::text },
    { Field::inspectionAudioStatus, "inspectionAudioStatus", ProviderKind::recovery, ValueType::text },
    { Field::inspectionObservedSequence, "inspectionObservedSequence", ProviderKind::recovery, ValueType::number },
    { Field::inspectionSelectedSequence, "inspectionSelectedSequence", ProviderKind::recovery, ValueType::number },
    { Field::inspectionMetadataReady, "inspectionMetadataReady", ProviderKind::recovery, ValueType::number },
    { Field::inspectionSourceChecked, "inspectionSourceChecked", ProviderKind::recovery, ValueType::number },
    { Field::inspectionSourceReason, "inspectionSourceReason", ProviderKind::recovery, ValueType::number },
    { Field::inspectionAudioRequested, "inspectionAudioRequested", ProviderKind::recovery, ValueType::number },
    { Field::inspectionAudioVerified, "inspectionAudioVerified", ProviderKind::recovery, ValueType::number },
    { Field::inspectionQuarantined, "inspectionQuarantined", ProviderKind::recovery, ValueType::number },
    { Field::inspectionPoints, "inspectionPoints", ProviderKind::recovery, ValueType::number },
    { Field::inspectionAssets, "inspectionAssets", ProviderKind::recovery, ValueType::number },
    { Field::inspectionAttempts, "inspectionAttempts", ProviderKind::recovery, ValueType::number },
    { Field::inspectionMetadataBytes, "inspectionMetadataBytes", ProviderKind::recovery, ValueType::number },
    { Field::inspectionSourceWindowBytes, "inspectionSourceWindowBytes", ProviderKind::recovery, ValueType::number },
    { Field::inspectionSourceFullBytes, "inspectionSourceFullBytes", ProviderKind::recovery, ValueType::number },
    { Field::inspectionAudioBytes, "inspectionAudioBytes", ProviderKind::recovery, ValueType::number },
    { Field::inspectionVerifiedAssets, "inspectionVerifiedAssets", ProviderKind::recovery, ValueType::number },
    { Field::restoreState, "restoreState", ProviderKind::recovery, ValueType::text },
    { Field::restoreLaunchId, "restoreLaunchId", ProviderKind::recovery, ValueType::text },
    { Field::restoreSchedulingReason, "restoreSchedulingReason", ProviderKind::recovery, ValueType::text },
    { Field::restoreAttempt, "restoreAttempt", ProviderKind::recovery, ValueType::text },
    { Field::restoreOwner, "restoreOwner", ProviderKind::recovery, ValueType::text },
    { Field::restoreCheckpoint, "restoreCheckpoint", ProviderKind::recovery, ValueType::text },
    { Field::restoreStage, "restoreStage", ProviderKind::recovery, ValueType::text },
    { Field::restoreFailureStage, "restoreFailureStage", ProviderKind::recovery, ValueType::text },
    { Field::restoreReason, "restoreReason", ProviderKind::recovery, ValueType::text },
    { Field::restoreAttemptCloseReason, "restoreAttemptCloseReason", ProviderKind::recovery, ValueType::text },
    { Field::restoreJobCount, "restoreJobCount", ProviderKind::recovery, ValueType::number },
    { Field::restoreRunning, "restoreRunning", ProviderKind::recovery, ValueType::number },
    { Field::restoreResidentBytes, "restoreResidentBytes", ProviderKind::recovery, ValueType::number },
    { Field::restoreSequence, "restoreSequence", ProviderKind::recovery, ValueType::number },
    { Field::restoreWaitingAdmission, "restoreWaitingAdmission", ProviderKind::recovery, ValueType::number },
    { Field::restoreCoreAdmitted, "restoreCoreAdmitted", ProviderKind::recovery, ValueType::number },
};
const char* providerId(ProviderKind kind)
{
    switch (kind) { case ProviderKind::audio: return "audio"; case ProviderKind::spatial: return "spatial";
        case ProviderKind::codec: return "codec"; case ProviderKind::cache: return "cache"; case ProviderKind::recovery: return "recovery"; }
    return nullptr;
}
const char* reasonName(Unavailable reason)
{
    switch (reason) { case Unavailable::notReported: return "not-reported"; case Unavailable::notApplicable: return "not-applicable";
        case Unavailable::notSupported: return "not-supported"; case Unavailable::providerBusy: return "provider-busy";
        case Unavailable::captureFailed: return "capture-failed"; case Unavailable::invalidValue: return "invalid-value";
        case Unavailable::privacyExcluded: return "privacy-excluded"; }
    return "invalid-value";
}
const char* identifierName(LocalIdentifierKind kind)
{
    switch (kind) { case LocalIdentifierKind::path: return "path"; case LocalIdentifierKind::fileName: return "filename";
        case LocalIdentifierKind::deviceName: return "device-name"; case LocalIdentifierKind::endpointGuid: return "endpoint-guid";
        case LocalIdentifierKind::serial: return "serial"; }
    return nullptr;
}
const char* failureStage(FailureStage stage)
{
    switch (stage) { case FailureStage::admission: return "admission"; case FailureStage::negotiation: return "negotiation";
        case FailureStage::launch: return "launch"; case FailureStage::decode: return "decode"; case FailureStage::cache: return "cache";
        case FailureStage::parse: return "parse"; case FailureStage::write: return "write"; case FailureStage::restore: return "restore";
        case FailureStage::cleanup: return "cleanup"; }
    return nullptr;
}
bool number(const juce::var& v)
{ return (v.isInt() || v.isInt64() || v.isDouble()) && std::isfinite(static_cast<double>(v)) && std::abs(static_cast<double>(v)) <= 1.0e18; }
bool publicText(const juce::var& v)
{
    if (!v.isString()) return false;
    const auto s = v.toString();
    // Providers must supply public vendor/product metadata, not a custom name.
    // Reject obvious paths/addresses/IDs rather than trying to redact free prose.
    if (s.isEmpty() || s.length() > 256 || s.containsAnyOf("\\/:@{}\r\n\t")) return false;
    for (auto c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
class EndpointPseudonym final
{
public:
    EndpointPseudonym() = default; // CNG is opened only if an endpoint is actually present.
    ~EndpointPseudonym()
    {
#if JUCE_WINDOWS
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        SecureZeroMemory(secret.data(), secret.size());
#endif
    }
    juce::String digest(const juce::String& endpoint)
    {
        if (endpoint.isEmpty() || endpoint.getNumBytesAsUTF8() > 1024) return {};
#if JUCE_WINDOWS
        if (algorithm == nullptr)
        {
            if (BCryptGenRandom(nullptr, secret.data(), static_cast<ULONG>(secret.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0
                || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return {};
        }
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, secret.data(), static_cast<ULONG>(secret.size()), 0) < 0) return {};
        struct Close final { BCRYPT_HASH_HANDLE h; ~Close() { BCryptDestroyHash(h); } } close{ hash };
        std::array<uint8_t, 32> output{};
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(endpoint.toRawUTF8())), static_cast<ULONG>(endpoint.getNumBytesAsUTF8()), 0) < 0
            || BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) < 0) return {};
        return juce::String::toHexString(output.data(), static_cast<int>(output.size()), 0);
#else
        return {};
#endif
    }
private:
#if JUCE_WINDOWS
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::array<uint8_t, 32> secret{};
#endif
};
juce::var fieldValue(const Descriptor& descriptor, const EvidenceField* input, Unavailable fallback, EndpointPseudonym& endpoints)
{
    auto result = object();
    auto unavailable = input == nullptr ? fallback : input->unavailable;
    if (input != nullptr && !input->value.isVoid() && !input->value.isUndefined())
    {
        auto value = input->value;
        bool valid = false;
        switch (descriptor.type)
        {
            case ValueType::text: valid = publicText(value); break;
            case ValueType::number: valid = number(value); break;
            case ValueType::numbers:
                if (const auto* array = value.getArray(); array != nullptr && array->size() <= 512)
                    valid = std::all_of(array->begin(), array->end(), [](const auto& n) { return number(n); });
                break;
            case ValueType::endpoint:
                if (value.isString()) { value = endpoints.digest(value.toString()); valid = value.toString().isNotEmpty(); }
                break;
        }
        if (valid) { put(result, "value", value); return result; }
        unavailable = Unavailable::invalidValue;
    }
    put(result, "unavailable", reasonName(unavailable));
    return result;
}
juce::MemoryBlock serialise(const juce::var& v)
{
    const auto text = juce::JSON::toString(v, true);
    return { text.toRawUTF8(), text.getNumBytesAsUTF8() };
}
}

PreparedSnapshot DiagnosticSnapshot::capture(const BundleIdentity& identity, const juce::String& origin,
                                              const std::vector<LoadedProviderRef>& providers, const SnapshotSelection& selection)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(snapshot);
    PreparedSnapshot prepared;
    try
    {
        if (!DiagnosticBundle::identityValid(identity) || providers.size() > 5 || selection.localIdentifiers.size() > 5 || selection.chosenReproduction.size() > maximumReproductionFiles
            || (!selection.chosenReproduction.empty() && !selection.reproductionConsent)
            || (origin != "gui" && origin != "standalone-cli" && origin != "test-fixture")) return prepared;
        for (auto kind : selection.localIdentifiers) if (identifierName(kind) == nullptr) return prepared;
        std::set<ProviderKind> kinds;
        for (const auto& p : providers) if (p.owner == nullptr || providerId(p.kind) == nullptr || !kinds.insert(p.kind).second) return prepared;
        size_t total = 0; std::set<juce::String> attachments;
        for (const auto& a : selection.chosenReproduction)
        {
            total += a.bytes.getSize();
            if (a.kind != EntryKind::reproduction || !DiagnosticBundle::logicalId(a.id, a.kind) || !attachments.insert(a.id).second
                || a.schemaMajor != 1 || a.bytes.getSize() > DiagnosticBundle::maximumEntryBytes
                || total > DiagnosticBundle::maximumArchiveBytes - 2 * 1024 * 1024) return prepared;
        }
        auto& bundle = prepared.bundle;
        bundle.identity = identity; bundle.origin = origin; bundle.timestampMs = juce::Time::currentTimeMillis();
        bundle.tier = !selection.chosenReproduction.empty() ? PrivacyTier::reproductionMaterial
                    : !selection.localIdentifiers.empty() ? PrivacyTier::localIdentifiers : PrivacyTier::supportSafe;
        prepared.review.snapshotId = bundle.snapshotId; prepared.review.tier = bundle.tier;
        prepared.review.categories = { "application-identity", "technical-facts" };
        if (!selection.localIdentifiers.empty()) prepared.review.categories.push_back("selected-local-identifiers");
        if (!selection.chosenReproduction.empty()) prepared.review.categories.push_back("chosen-reproduction-material");
        EndpointPseudonym endpoints;
        for (const auto& provider : providers)
        {
            ProviderEvidence evidence; bool captureFailed = false;
            try { evidence = provider.owner->snapshot(); }
            catch (...) { captureFailed = true; }
            if (evidence.fields.size() > 64 || evidence.identifiers.size() > 16 || evidence.operationIds.size() > 8) captureFailed = true;
            std::set<Field> reported;
            if (!captureFailed)
                for (const auto& f : evidence.fields)
                {
                    const auto found = std::find_if(std::begin(descriptors), std::end(descriptors), [&](const auto& d) { return d.field == f.field && d.owner == provider.kind; });
                    if (found == std::end(descriptors) || !reported.insert(f.field).second) { captureFailed = true; break; }
                }
            if (captureFailed) evidence = ProviderEvidence();
            auto section = object(), fields = object(); bool partial = captureFailed;
            put(section, "providerId", providerId(provider.kind)); put(section, "providerSchemaMajor", 1);
            for (const auto& descriptor : descriptors)
            {
                if (descriptor.owner != provider.kind) continue;
                const auto found = std::find_if(evidence.fields.begin(), evidence.fields.end(), [&](const auto& f) { return f.field == descriptor.field; });
                const auto value = fieldValue(descriptor, found == evidence.fields.end() ? nullptr : &*found,
                                               captureFailed ? Unavailable::captureFailed : Unavailable::notReported, endpoints);
                partial |= value.hasProperty("unavailable"); put(fields, descriptor.id, value);
            }
            put(section, "fields", fields);
            auto ids = juce::var(juce::Array<juce::var>());
            for (const auto& id : evidence.operationIds) if (!id.isNull()) ids.append(id.toString());
            put(section, "operationIds", ids);
            auto identifiers = juce::var(juce::Array<juce::var>());
            for (const auto& id : evidence.identifiers)
                if (selection.localIdentifiers.count(id.kind) != 0 && identifierName(id.kind) != nullptr
                    && id.value.isNotEmpty() && id.value.getNumBytesAsUTF8() <= 1024)
                {
                    auto value = object(); put(value, "kind", identifierName(id.kind)); put(value, "value", id.value); identifiers.append(value);
                }
            if (identifiers.size() > 0) put(section, "localIdentifiers", identifiers);
            if (evidence.latestFailure && !evidence.latestFailure->operationId.isNull() && failureStage(evidence.latestFailure->stage) != nullptr)
            {
                const auto& failure = *evidence.latestFailure;
                auto capsule = object(), counters = juce::var(juce::Array<juce::var>());
                put(capsule, "operationId", failure.operationId.toString()); put(capsule, "stage", failureStage(failure.stage));
                put(capsule, "code", static_cast<juce::int64>(failure.code));
                for (auto counter : failure.counters) counters.append(static_cast<juce::int64>(counter));
                put(capsule, "counters", counters); put(section, "latestFailure", capsule);
            }
            put(section, "captureStatus", captureFailed ? "capture-failed" : partial ? "partial" : "complete");
            auto sectionBytes = serialise(section); juce::var checked;
            if (!BoundedJson::parse(sectionBytes, checked)) { prepared.state = { BundleError::budget }; return prepared; }
            prepared.review.payloadBytes += sectionBytes.getSize();
            bundle.entries.push_back({ "providers/" + juce::String(providerId(provider.kind)) + ".json", EntryKind::provider, 1, std::move(sectionBytes) });
            ++prepared.review.providerCount;
        }
        for (const auto& attachment : selection.chosenReproduction)
        {
            prepared.review.payloadBytes += attachment.bytes.getSize(); bundle.entries.push_back(attachment); ++prepared.review.attachmentCount;
        }
        prepared.state = {};
        return prepared;
    }
    catch (const std::bad_alloc&) { prepared.state = { BundleError::budget }; return prepared; }
    catch (...) { prepared.state = { BundleError::invalidInput }; return prepared; }
}

BundleStatus DiagnosticSnapshot::publishReviewed(const PreparedSnapshot& prepared, const juce::Uuid& approved,
                                                 const juce::File& destination, const std::atomic<bool>* cancelled)
{
    if (!prepared.status() || approved.isNull() || approved != prepared.preview().snapshotId) return { BundleError::invalidInput };
    return DiagnosticBundle::publish(prepared.bundle, destination, cancelled);
}
}
