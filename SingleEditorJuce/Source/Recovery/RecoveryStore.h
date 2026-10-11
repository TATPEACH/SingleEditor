#pragma once

#include "Document/MediaIdentity.h"
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace singleeditor::recovery
{
enum class Error
{
    none, invalidPacket, corruptJournal, missingOrCorruptAsset, unsupportedSchema,
    staleHead, resourceOrEnvironmentFault, budgetExceeded, cancelled, sourceConflict
};

// A read-only observation of the gate that refused work, never admission or
// cleanup authority. Early bounded inventories may report only a lower bound.
struct ByteCapacity final
{
    enum class Resource { none, retainedMemory, ownerStorage, globalStorage, scopeStorage, freeDisk, audioGraph };
    Resource resource = Resource::none;
    uint64_t required = 0, limit = 0;
    bool lowerBound = false;
};

struct Status final
{
    Error error = Error::none;
    juce::String code;
    ByteCapacity capacity;
    explicit operator bool() const noexcept { return error == Error::none; }
};

inline Status byteCapacityFailure(const juce::String& code, ByteCapacity::Resource resource,
    uint64_t required, uint64_t limit, bool lowerBound = false)
{
    return { Error::budgetExceeded, code, { resource, required, limit, lowerBound } };
}

// Internal large-audio Pilot: logical audio remains bounded independently of
// per-block working buffers and eager full-graph materialisation.
inline constexpr size_t maximumCommitAssets = 1024;
inline constexpr uint64_t maximumRecoveryAudioBytes = 8ULL * 1024 * 1024 * 1024;

// Internal pilot bounds, not calibrated shipping quota/retention policy.
// Global accounting is worker-only during append, never construction/open.
struct Limits final
{
    size_t recordBytes = 256 * 1024;
    size_t assetBytes = 8 * 1024 * 1024;
    size_t readBytes = maximumRecoveryAudioBytes;
    size_t journalReadBytes = 16 * 1024 * 1024;
    size_t records = 1024;
    int64_t ownerBytes = maximumRecoveryAudioBytes;
    int64_t freeSpaceReserve = 64 * 1024 * 1024;
    int64_t globalBytes = 8LL * 1024 * 1024 * 1024;
    size_t globalEntries = 65536;
};

struct SafeHead final
{
    int64_t sequence = 0;
    juce::String hash;
    bool operator==(const SafeHead&) const = default;
};

struct OwnerRetirement final
{
    SafeHead head;
    juce::String nonce;
    juce::var evidence;
};

// Explicit post-Core live dependency on an owner's durable data. Held by real
// restored readers or an admitted writer, never by ordinary audio or an
// uninstantiated recovery module. Writer activity permits its own compaction.
class RecoveryOwnerLease final
{
public:
    ~RecoveryOwnerLease();
    bool belongsTo(const juce::File&, const juce::Uuid&) const noexcept;
private:
    friend class RecoveryStore;
    RecoveryOwnerLease();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct AudioBlock final
{
    double sampleRate = 0;
    int channels = 0;
    int64_t frames = 0;
    // Interleaved canonical IEEE-754 float32 little-endian bytes, not native floats.
    juce::MemoryBlock samples;
};

// Worker-confined immutable source for one bounded block. The ID is a claim,
// not a proof: append checks the regenerated dimensions, samples and hash before
// publishing it. Captured source leases live through the synchronous append.
struct StreamedAudioBlock final
{
    juce::String id;
    double sampleRate = 0;
    int channels = 0;
    int64_t frames = 0;
    std::function<Status(AudioBlock&)> read;
};

struct CommitPacket final
{
    media::IdentitySnapshot identity;
    juce::String operationId;
    int planSchema = 1;
    // Caller-owned canonical bytes, retained verbatim and independently hashed.
    // Storage does not rewrite a processor's Plan or edit/Undo graph schema.
    juce::String planJson;
    juce::String stateJson;
    int64_t timestampMilliseconds = 0;
    std::vector<AudioBlock> newAudio;
    // References are owner-local. Publication verifies bytes or leases the exact
    // file version previously verified by this Store's successful publication.
    std::vector<juce::String> existingAudio;
    std::vector<StreamedAudioBlock> streamedAudio;
};

// In-process proof of one successful durable publication, issued only by Store.
// It is not a promise that assets remain healthy: every later append checks and
// leases its destination dependencies again. No public constructor or mutable fields.
class PublishedRecoveryCommit final
{
public:
    const juce::File& rootDirectory() const noexcept { return root; }
    juce::Uuid ownerId() const noexcept { return owner; }
    const SafeHead& head() const noexcept { return safeHead; }
    const media::DocumentIdentity& documentIdentity() const noexcept { return identity; }
    const juce::String& stateJson() const noexcept { return state; }
    const std::set<juce::String>& assetIds() const noexcept { return assets; }
    const media::SourceIdentity& sourceIdentity() const noexcept { return source; }
    const juce::String& operationId() const noexcept { return operation; }
    const juce::String& planJson() const noexcept { return plan; }
    int planSchema() const noexcept { return schema; }
private:
    friend class RecoveryStore;
    PublishedRecoveryCommit(juce::File directory, juce::Uuid id, SafeHead head,
                            const CommitPacket& packet, std::set<juce::String> dependencies)
        : root(std::move(directory)), owner(id), safeHead(std::move(head)), identity(packet.identity.document), state(packet.stateJson),
          assets(std::move(dependencies)), source(packet.identity.source), operation(packet.operationId), plan(packet.planJson), schema(packet.planSchema) {}
    juce::File root;
    juce::Uuid owner;
    SafeHead safeHead;
    media::DocumentIdentity identity;
    juce::String state;
    std::set<juce::String> assets;
    media::SourceIdentity source;
    juce::String operation, plan;
    int schema = 0;
};

struct VerifiedCommit final
{
    SafeHead head;
    juce::var record;
};

struct ReadView final
{
    Status status;
    SafeHead head;
    size_t verifiedBytes = 0;
    size_t verifiedRecordBytes = 0;
    std::vector<VerifiedCommit> commits;
};

// Head/record hashes, schemas, identity chain and declared asset IDs validated.
// Audio bodies have NOT been read. Deliberately not convertible to VerifiedCommit
// or ReadView: discovery is not proof of playable audio or a fully healthy owner.
struct ManifestCommit final
{
    SafeHead head;
    juce::var record;
};
struct ManifestView final
{
    Status status;
    SafeHead head;
    size_t verifiedRecordBytes = 0;
    std::vector<ManifestCommit> commits;
    std::optional<OwnerRetirement> retirement;
    // Read-only format facts. Deletion always revalidates them under Store locks.
    juce::String checkpointCut;
    SafeHead checkpointFloor;
    std::vector<SafeHead> supersededRecords;
    std::vector<juce::String> supersededCuts;
};

// Issued only after the checkpoint codec has certified the complete retained
// current/Undo/Redo graph at this exact head. Metadata is not a deletion token.
class RecoveryCompactionProof final
{
    friend class RecoveryCheckpointCodec;
    friend class RecoveryStore;
    RecoveryCompactionProof(juce::File directory, juce::Uuid id, SafeHead end, SafeHead begin)
        : root(std::move(directory)), owner(id), head(std::move(end)), floor(std::move(begin)) {}
    juce::File root;
    juce::Uuid owner;
    SafeHead head, floor;
};

// One frozen head and its record only. No ancestor, source file or CAS reads:
// this is display metadata, never proof of history health or disposal authority.
struct OwnerSummary final
{
    Status status;
    SafeHead head;
    juce::String sourcePath;
    int64_t lastSafeMilliseconds = 0;
    size_t recordBytesRead = 0;
    bool retired = false;
};

// Worker diagnostics for one append, not ordinary-audio startup instrumentation.
// Logical read/retention budgets still count the entire recoverable closure.
struct AppendWork final
{
    size_t audioPayloadBytesRead = 0;
    size_t journalPayloadBytesRead = 0;
    size_t reusedAudioFiles = 0;
    size_t hashedAudioFiles = 0;
    size_t ownerEntriesScanned = 0;
    size_t globalEntriesScanned = 0;
    int64_t globalBytesObserved = 0;
    size_t scopeEntriesScanned = 0;
    int64_t scopeBytesObserved = 0;
    // Derived from the existing append inventory; no additional polling I/O.
    bool compactionSuggested = false;
};

struct OrphanCollectionBudget final
{
    size_t files = 16;
    size_t bytes = 64 * 1024 * 1024;
};
struct OrphanCollectionReport final
{
    SafeHead head;
    size_t filesRemoved = 0, bytesRemoved = 0, bytesRead = 0, retained = 0;
    bool more = false;
};

struct TerminalCollectionReport final
{
    size_t filesRemoved = 0, bytesRemoved = 0, bytesRead = 0;
    bool more = false, completed = false;
};

// Read-only worker review for a selected owner, not a Save or user-consent bit.
// The UI must show these facts and obtain a second Discard confirmation before
// submitting it. Size is the observed physical size, not a promised freed size.
class OwnerDiscardReview final
{
public:
    const juce::File& rootDirectory() const noexcept { return root; }
    juce::Uuid ownerId() const noexcept { return owner; }
    const SafeHead& frozenHead() const noexcept { return head; }
    const juce::String& sourcePath() const noexcept { return source; }
    int64_t lastSafeMilliseconds() const noexcept { return timestamp; }
    int64_t observedBytes() const noexcept { return bytes; }
    int64_t expiryRetentionMilliseconds() const noexcept { return retention; }
    size_t observedEntries() const noexcept { return entries; }
private:
    friend class RecoveryStore;
    friend class RecoveryOwnerLifecycle;
    OwnerDiscardReview() = default;
    juce::File root;
    juce::Uuid owner { nullptr };
    SafeHead head;
    juce::String rootId, directoryId, source, retentionPolicyHash;
    int64_t timestamp = 0, bytes = 0;
    int64_t latestActivity = 0, retention = 0, reviewedAt = 0;
    size_t entries = 0;
};

// Synchronous worker-only storage. Construction/destruction do no I/O and start
// no thread/timer/service. Document capture, queue/drain and restore are separate.
// A read view freezes a verified head; it never exposes file/writer handles.
class RecoveryStore final
{
public:
    RecoveryStore(juce::File rootDirectory, juce::Uuid ownerId, Limits bounds = {});
    const juce::File& rootDirectory() const noexcept { return root; }
    juce::Uuid ownerId() const noexcept { return owner; }
    Status append(const SafeHead& expected, const CommitPacket&, SafeHead& published,
                  std::shared_ptr<const PublishedRecoveryCommit>* receipt = nullptr, AppendWork* work = nullptr,
                  std::shared_ptr<RecoveryOwnerLease>* writerActivity = nullptr);
    ReadView inspect() const;
    // Explicit worker discovery: no asset-directory access or sample reads.
    // Full inspect remains the independent content certification path. Append
    // also checks the full manifest but may reuse leased, unchanged asset proofs.
    ManifestView inspectManifest(std::function<bool()> cancel = {}) const;
    OwnerSummary inspectSummary(std::function<bool()> cancel = {}) const;
    // Explicit selected-owner review only. Bounded metadata enumeration, no CAS
    // body reads or writes. Missing/corrupt/future chains require separate disposal.
    Status reviewDiscard(std::unique_ptr<OwnerDiscardReview>&, std::function<bool()> cancel = {}) const;
    // Explicit user-selected age, not a shipping default or automatic policy.
    // Uses verified commit timestamps; a second confirmation is still required.
    Status reviewExpiry(int64_t retentionMilliseconds, std::unique_ptr<OwnerDiscardReview>&,
                        std::function<bool()> cancel = {}) const;
    // Automatic maintenance uses only the currently persisted root policy.
    // Its exact policy hash is rechecked under the retirement transaction.
    Status reviewConfiguredExpiry(std::unique_ptr<OwnerDiscardReview>&, std::function<bool()> cancel = {}) const;
    // Worker-only stale fence for an already validated/frozen owner head. Reads
    // at most one 4 KiB head payload; never opens commits/assets or creates paths.
    // Success proves only that publication has not changed, not chain health.
    Status checkHead(const SafeHead& expected) const;
    Status acquireOwnerLease(std::shared_ptr<RecoveryOwnerLease>&) const;
    // Metadata inspection remains available for retired owners; new appends,
    // restore fences and live-lease acquisition reject their terminal marker.
    Status inspectRetirement(std::optional<OwnerRetirement>&) const;
    Status readAudio(const juce::String& assetId, AudioBlock&) const;
    // Explicit worker quantum with an additional payload budget. Reports bytes
    // actually read even when framing/hash/decode fails; SER1 frame overhead is
    // a fixed 40 bytes per opened asset. Store retains no block between calls.
    Status readAudioBounded(const juce::String&, size_t maximum, AudioBlock&, size_t& consumed) const;
    // Explicit post-Core worker maintenance. Preserves every on-chain record and
    // all historical/current assets. Only recognized staging and unreferenced
    // CAS are eligible; each deletion has a durable, resumable quarantine plan.
    // This is not owner Discard, checkpoint pruning or global retention policy.
    Status collectOrphans(const SafeHead&, OrphanCollectionReport&, OrphanCollectionBudget = {},
                          std::function<bool()> cancel = {});
    // Atomically advance the retained checkpoint floor without changing the
    // published document/head. A live restored-reader lease prevents the cut.
    // A later bounded collectOrphans quantum handles its proven old records.
    Status compact(const RecoveryCompactionProof&, std::function<bool()> cancel = {});
    // Explicit worker operation for a previously published terminal owner only.
    // First call certifies a bounded inventory (Limits::readBytes/4096 entries);
    // subsequent sweeps obey the separate file/byte budget. A root-local durable
    // intent fences resurrection even after head/retired/locks have been removed.
    // Prior orphan quarantine is admitted only by a supported, exact old plan;
    // its verified physical objects become terminal schema-2 inventory entries.
    // Unknown or changed objects are retained. No retention policy/global scan.
    Status collectTerminal(const SafeHead&, TerminalCollectionReport&, OrphanCollectionBudget = {},
                           std::function<bool()> cancel = {});
    // Explicit worker-only lookup when a durable collection intent outlived
    // the old head/retirement files. Reads at most the existing 2 MiB control
    // format; returns a selector only. collectTerminal independently validates
    // the complete intent and physical inventory before removing anything.
    // An empty head on success means both owner and intent are already absent.
    Status inspectTerminalHead(SafeHead&) const;
    // Same canonical format/hash as append; no I/O or runtime activation.
    static Status identifyAudio(const AudioBlock&, size_t maximum, juce::String& assetId);

#if SINGLE_EDITOR_RECOVERY_TESTING
    enum class Boundary { assetPublished, recordPublished, beforeHeadReplace, headPublished };
    std::function<void(Boundary)> onBoundary;
    enum class CollectionBoundary { marked, quarantined, beforeDelete, itemDeleted, planDeleted };
    std::function<void(CollectionBoundary)> onCollectionBoundary;
    enum class CompactionBoundary { cutPublished, beforeHeadReplace, headPublished };
    std::function<void(CompactionBoundary)> onCompactionBoundary;
    enum class RetirementBoundary { beforePublish, published };
    std::function<void(RetirementBoundary)> onRetirementBoundary;
    enum class TerminalBoundary { planned, beforeDelete, itemDeleted, locksRemoved, ownerRemoved, intentRemoved,
                                  directoryRemoved, headRemoved, markerRemoved };
    std::function<void(TerminalBoundary)> onTerminalBoundary;
#endif

private:
    friend class RecoveryOwnerLifecycle;
    Status publishRetirement(const SafeHead&, const juce::String& nonce, const juce::var& evidence, OwnerRetirement&,
                             const juce::String& expectedRootId = {}, const juce::String& expectedDirectoryId = {});
    // Immutable, publication-bound file facts only: no sample buffers, readers
    // or handles between appends. Copies may share facts but never mutable state.
    struct AppendCache;
    std::shared_ptr<const AppendCache> appendCache;
    juce::File root;
    juce::Uuid owner;
    Limits limits;
};
} // namespace singleeditor::recovery
