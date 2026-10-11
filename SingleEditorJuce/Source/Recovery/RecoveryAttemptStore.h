#pragma once
#include "RecoveryStore.h"

namespace singleeditor::recovery
{
enum class RecoveryAttemptStage { queued, validating, resolvingSource, replayingCheckpoint,
    preparingCore, coreReady, materializingFull, ready, failed, cancelled };
enum class AttemptOutcome { active, succeeded, failed, cancelled };
enum class AttemptFailure { none, corruptJournal, missingOrCorruptAsset, unsupportedSchema,
    sourceConflict, restoreCrashSuspect, resourceOrEnvironmentFault, unknown };
enum class AttemptHealth { healthy, active, suspect, quarantined };
struct AttemptRecord final
{
    juce::Uuid owner { nullptr }, candidate { nullptr }, nonce { nullptr };
    SafeHead ownerHead, checkpoint;
    juce::String appBuild;
    int64_t processId = 0, processStartedTicks = 0, startedMilliseconds = 0, updatedMilliseconds = 0, revision = 1;
    RecoveryAttemptStage stage = RecoveryAttemptStage::validating;
    AttemptOutcome outcome = AttemptOutcome::active;
    AttemptFailure failure = AttemptFailure::none;
    juce::String errorCode;
    bool operator==(const AttemptRecord&) const = default;
};
struct AttemptInspection final
{
    AttemptRecord record;
    AttemptHealth health = AttemptHealth::healthy;
    size_t matchingInterruptions = 0;
};
struct AttemptView final { Status status; std::vector<AttemptInspection> attempts; };
// Explicit pilot limits, not an unlimited log or an automatic retention/GC rule.
struct AttemptLimits final
{
    size_t entries = 128, recordBytes = 4096, quarantineAfter = 2;
    // Independent metadata-root Pilot, still subject to service aggregate budget
    // allocation/calibration before product admission. No implicit retention GC.
    int64_t rootBytes = 64 * 1024 * 1024;
    size_t rootEntries = 16384;
    int64_t freeSpaceReserve = 64 * 1024 * 1024;
};

class RecoveryAttemptWriter final
{
public:
    // No implicit success/cancel write in a destructor. An abandoned writer
    // releases its lease, leaving an honestly unclosed marker for inspection.
    ~RecoveryAttemptWriter();
    Status enter(RecoveryAttemptStage);
    Status finish(AttemptOutcome, Status cause = {});
    bool isActive() const noexcept;
    const AttemptRecord& getRecord() const noexcept { return record; }
private:
    friend class RecoveryAttemptStore;
    RecoveryAttemptWriter();
    struct Impl;
    std::unique_ptr<Impl> impl;
    AttemptRecord record;
    Status publishNext(AttemptRecord);
};

// Separate from journal/CAS. Construction is inert; explicit worker calls only.
// Inspection reads bounded marker metadata, never audio/journal/decoder/DSP.
// A dead/unleased active marker is suspicion of interruption, not proof of an
// audio fault. Closed controlled failures are not counted as crash repetitions.
class RecoveryAttemptStore final
{
public:
    RecoveryAttemptStore(juce::File markerRoot, juce::Uuid owner, AttemptLimits = {});
    AttemptView inspect(std::function<bool()> cancelled = {}) const;
    Status begin(juce::Uuid candidate, const SafeHead& ownerHead, const SafeHead& checkpoint,
                 const juce::String& appBuild, std::unique_ptr<RecoveryAttemptWriter>&,
                 bool explicitRetryOnce = false) const;
private:
    juce::File root;
    juce::Uuid owner;
    AttemptLimits limits;
};
}
