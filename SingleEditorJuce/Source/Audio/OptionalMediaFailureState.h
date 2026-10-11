#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

namespace singleeditor::media
{
// Explicit optional-module owner. No static singleton, timer, disk, logging or
// provider registration. This state is discarded with the process session.
class OptionalMediaFailureState final
{
public:
    enum class Module { packedAudio, videoAudio, bank, sidecar };
    enum class Profile { conservative, expanded };
    enum class Intent { initial, retryOnce, expandBudget };
    enum class Denied { none, invalidKey, busy, retryRequired, retryUsed, circuitOpen,
                        expansionNotAllowed, stateBudget };
    enum class Reason { decoderExit, deadline, memoryQuota, outputQuota, parserBounds,
                        missingDependency, containment, cacheInvalid, ioFailure };
    enum class Stage { admission, launch, decode, cache, parse, cleanup };
    enum class FailureSource { notReported, decoder, bankIndex, bankSidecar };
    struct Key final
    {
        juce::String sourceFingerprint, policyFingerprint;
        Module module = Module::packedAudio;
        Profile profile = Profile::conservative;
        bool operator<(const Key& other) const
        {
            return std::tie(sourceFingerprint, policyFingerprint, module, profile)
                 < std::tie(other.sourceFingerprint, other.policyFingerprint, other.module, other.profile);
        }
    };
    struct Ticket final
    {
        Key key;
        uint64_t sequence = 0;
        Intent intent = Intent::initial;
        std::optional<Key> expandedFrom;
    };
    struct Admission final
    {
        Denied denied = Denied::none;
        std::optional<Ticket> ticket;
        explicit operator bool() const noexcept { return ticket.has_value(); }
    };
    struct Failure final
    {
        Reason reason = Reason::decoderExit;
        Stage stage = Stage::decode;
        uint32_t exitCode = 0;
        uint64_t observedBytes = 0, allowedBytes = 0;
        uint32_t elapsedMilliseconds = 0;
        bool jobCleanupConfirmed = false;
        // Retained with the actual failed attempt, including a later refused retry.
        juce::Uuid operationId { nullptr };
        FailureSource source = FailureSource::notReported;
        int64_t detailCode = 0; // source-specific typed outcome; never native error prose
    };
    struct Capsule final { Key key; Failure failure; uint64_t attempt = 0; };
    struct View final
    {
        int failures = 0;
        bool inFlight = false, retryAvailable = false, expansionAvailable = false, circuitOpen = false;
    };

    Admission begin(const Key&, Intent = Intent::initial, const std::optional<Key>& expandedFrom = {});
    bool succeed(const Ticket&);
    bool fail(const Ticket&, const Failure&);
    bool cancel(const Ticket&);
    std::optional<Capsule> latestFailure() const;
    bool tryLatestFailure(std::optional<Capsule>&) const;
    // Freeze a real failure before ticket admission. This changes only the
    // latest diagnostic outcome, never entries, retry permission or budgets.
    void noteUnadmittedFailure(const Failure&);
    size_t retainedEntries() const;
    View inspect(const Key&) const;
private:
    struct Entry final
    {
        uint64_t inFlight = 0;
        int failures = 0;
        bool unresolved = false, retryUsed = false, expandedGranted = false;
        Reason lastReason = Reason::decoderExit;
        Intent intent = Intent::initial;
        std::optional<Key> expandedFrom;
    };
    static bool valid(const Key&);
    static bool budget(Reason);
    mutable std::mutex mutex;
    std::map<Key, Entry> entries;
    std::optional<Capsule> latest;
    uint64_t sequence = 0;
};
}
