#pragma once
#include "RecoveryCheckpointCodec.h"
#include "RecoveryAttemptStore.h"
#include "RecoverySourceBinding.h"
#include "RecoveryDiagnosticState.h"

namespace singleeditor::recovery
{
// Explicit Center worker inspection only. Safe Inspect does not read CAS audio.
// Optional Deep Verify reads one bounded CAS block at a time, without a Document,
// decoder, DSP, scratch, writer or attempt. Results are observations, never a
// reusable source-write, restoration or retirement credential.
class RecoveryInspection final
{
public:
    enum class SourceAction { none, locateOrCheck, deepVerify };
    struct Request final
    {
        SafeHead observedHead, selectedHead;
        RecoverySourceBinding::Request source;
        SourceAction action = SourceAction::none;
        bool verifyStoredAudio = false;
        // Pilot payload budget: graph sample bound plus bounded JSON headers.
        size_t audioReadBytes = 256 * 1024 * 1024 + 128 * 1024;
        AudioGraphLimits graph;
        AttemptLimits attempts;
    };
    struct Point final { SafeHead head; int64_t milliseconds = 0; };
    // Bounded copied facts only: no checkpoint, source locator, audio, lease or
    // attempt records cross into the diagnostics worker.
    struct DiagnosticObservation final
    {
        juce::Uuid operationId { nullptr }, owner { nullptr };
        SafeHead observedHead, selectedHead;
        Status status, attemptsStatus, sourceStatus, audioStatus;
        SourceAction sourceAction = SourceAction::none;
        bool metadataReady = false, sourceChecked = false, audioCheckRequested = false, audioVerified = false;
        bool attemptsChecked = false, sourceCheckAttempted = false, audioCheckAttempted = false;
        bool quarantined = false;
        SourceConfidence sourceConfidence = SourceConfidence::missing;
        SourceReason sourceReason {};
        int64_t points = 0, assets = 0, attempts = 0, metadataBytes = 0, sourceWindowBytes = 0,
                sourceFullBytes = 0, audioBytes = 0, verifiedAssets = 0;
    };
    struct Report final
    {
        Status status, sourceStatus, audioStatus;
        juce::Uuid operationId { nullptr }, owner { nullptr };
        SourceAction sourceAction = SourceAction::none;
        SafeHead observedHead, selectedHead;
        std::vector<Point> points; // Verified parent-chain order, newest first.
        std::vector<juce::String> assets;
        PreparedRecoveryCheckpoint checkpoint;
        AttemptView attempts;
        SourceResolution resolution;
        SourceObservation observation;
        bool metadataReady = false, sourceChecked = false;
        bool attemptsChecked = false, sourceCheckAttempted = false, audioCheckAttempted = false;
        size_t metadataBytesRead = 0, sourceWindowBytesRead = 0;
        int64_t sourceFullBytesRead = 0;
        juce::String sourceFullHash;
        bool audioCheckRequested = false, audioVerified = false;
        size_t audioAssetsVerified = 0, audioPayloadBytesRead = 0;
        juce::String audioFailureAsset;
        bool quarantinedFor(const juce::String& appBuild) const;
        DiagnosticObservation diagnosticObservation(const juce::String& appBuild) const;
    };
    using AudioProgress = std::function<void(size_t verifiedAssets, size_t totalAssets, size_t payloadBytes)>;
    static Report read(const RecoveryStore&, const juce::File& attemptRoot, const Request&,
                       const std::atomic<bool>* cancelled = nullptr, AudioProgress = {},
                       std::shared_ptr<RecoveryDiagnosticState> diagnostics = {});
};
}
