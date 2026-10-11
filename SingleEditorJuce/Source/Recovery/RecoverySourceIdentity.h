#pragma once
#include "RecoveryStore.h"
#include <atomic>

namespace singleeditor::media { struct SourceFileWitness; }

namespace singleeditor::recovery
{
// Facts belong to the source binding, not the current edited document. The
// caller reuses a previously parsed source header/payload span; no second decoder.
struct SourceAudioFacts final
{
    juce::String format, channelLayout;
    double sampleRate = 0;
    int channels = 0, bitsPerSample = 0;
    int64_t frames = 0, payloadOffset = 0, payloadBytes = 0;
    bool operator==(const SourceAudioFacts&) const = default;
};
struct RecoverySourceEvidence final
{
    int schema = 1;
    juce::String algorithm = "payload-3x64k-sha256-v1";
    juce::File location;
    int64_t sizeBytes = 0, modifiedTicks = 0;
    juce::String volumeId, fileId;
    SourceAudioFacts facts;
    juce::String structureSha256, sparseSha256, fullFileSha256;
    juce::var toJson() const;
    static std::optional<RecoverySourceEvidence> fromJson(const juce::var&);
    // Pure validation against the captured source binding, never the edited
    // document's dimensions. No path access or fresh identity inference.
    bool bindsTo(const media::SourceIdentity&) const;
};
// Within-attempt change fence, not an Exact classification. Includes NTFS
// ChangeTime so restoring LastWriteTime alone cannot hide an intervening edit.
struct SourceObservation final
{
    juce::File location;
    bool exists = false;
    int64_t sizeBytes = 0, modifiedTicks = 0, changedTicks = 0;
    juce::String volumeId, fileId;
    bool operator==(const SourceObservation&) const = default;
};
struct SourceObservationResult final { Status status; SourceObservation observation; };
enum class SourceConfidence { exact, relocated, changed, missing, ambiguous };
enum class SourceReason { matched, pathMoved, absent, insufficientEvidence, unsupportedIdentity,
                          factsChanged, fileIdentityChanged, metadataChanged, sparseChanged, fullHashChanged, multipleCandidates };
struct SourceResolution final
{
    SourceConfidence confidence = SourceConfidence::ambiguous;
    SourceReason reason = SourceReason::insufficientEvidence;
    std::optional<size_t> candidate;
    bool permitsReplay(bool relocationConfirmed = false) const noexcept
    { return confidence == SourceConfidence::exact || (confidence == SourceConfidence::relocated && relocationConfirmed); }
};
struct SourceProbe final
{
    Status status;
    std::optional<RecoverySourceEvidence> evidence;
    size_t payloadBytesRead = 0;
    int64_t fullBytesRead = 0;
};
class RecoverySourceIdentity final
{
public:
    enum class Mode { sparseAfterMutation, explicitDeepVerify };
    enum class Coverage { audioPayload, encodedFile };
    // Explicit post-Core worker call. expectedStamp must be the source's saved
    // fast stamp, so stale reader facts cannot silently fingerprint a new file.
    static SourceProbe capture(const media::SourceIdentity& expectedStamp, const SourceAudioFacts&,
                               Mode = Mode::sparseAfterMutation, const std::atomic<bool>* cancelled = nullptr,
                               Coverage = Coverage::audioPayload);
    // Native first-mutation worker: reuse original decoder/physical admission
    // facts. Explicit versioned windows cover the encoded source file when no
    // independently parsed payload offset is available; no second decoder.
    static SourceProbe captureAdmittedSource(const media::SourceIdentity&, const media::SourceFileWitness&,
                                             const std::atomic<bool>* cancelled = nullptr);
    // Explicit worker metadata observation; no audio windows, decoder, write
    // lock held across calls or directory enumeration. Known absence is distinct
    // from permission, sharing, reparse and platform faults.
    static SourceObservationResult observe(const juce::File&, const std::atomic<bool>* cancelled = nullptr);
    // Pure comparisons; no path discovery, directory scan, reader, or replay.
    static SourceResolution compare(const RecoverySourceEvidence&, const std::optional<RecoverySourceEvidence>& candidate);
    static SourceResolution resolve(const RecoverySourceEvidence&, const std::vector<RecoverySourceEvidence>& candidates);
};
}
