#pragma once
#include <juce_cryptography/juce_cryptography.h>
#include <atomic>
#include <vector>
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
 #include <functional>
#endif

namespace singleeditor::diagnostics
{
enum class BundleError { none, invalidInput, budget, malformedArchive, malformedJson, unsupportedSchema,
                         inconsistentManifest, hashMismatch, cancelled, ioFailure, destinationExists };
struct BundleStatus final
{
    BundleError error = BundleError::none;
    bool partialRetained = false; // local publication cleanup failed; never claim a saved bundle
    explicit operator bool() const noexcept { return error == BundleError::none; }
};
enum class PrivacyTier { supportSafe, localIdentifiers, reproductionMaterial };
enum class EntryKind { provider, reproduction };
struct BundleEntry final
{
    juce::String id; // providers/<slug>.json or reproduction/<slug>.bin; never a filesystem path
    EntryKind kind = EntryKind::provider;
    int schemaMajor = 1;
    juce::MemoryBlock bytes;
};
struct BundleIdentity final
{
    juce::String version, commit, binarySha256;
    juce::String configuration, distribution, processArchitecture, osArchitecture, osBuild;
    bool timingInstrumentation = false;
};
struct BundleInput final
{
    juce::Uuid snapshotId;
    int64_t timestampMs = 0;
    BundleIdentity identity;
    PrivacyTier tier = PrivacyTier::supportSafe;
    juce::String origin; // gui, standalone-cli or test-fixture; never implies cross-process capture
    std::vector<BundleEntry> entries;
};
struct BundleView final
{
    BundleStatus status;
    juce::var manifest;
    size_t verifiedEntries = 0;
    size_t verifiedBytes = 0;
    juce::String summary; // deterministic manifest projection; attachments are never opened/extracted
};

// Explicit synchronous worker-only archive primitive, not a consent UI or a
// provider collector. Production callers must supply the reviewed, consented
// snapshot. ToolTestOnly until that owner and privacy-safe snapshot are wired.
class DiagnosticBundle final
{
public:
    static constexpr size_t maximumEntries = 32; // includes the manifest
    static constexpr int64_t maximumArchiveBytes = 64 * 1024 * 1024;
    static constexpr int64_t maximumEntryBytes = 16 * 1024 * 1024;
    static constexpr int currentCoreMajor = 1; // first schema; no released N-1 exists yet
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
    enum class Boundary { partialClosed, validated, beforePublish };
    struct TestHooks final
    {
        std::function<void(Boundary, const juce::File&)> boundary;
        int64_t failOutputAfterBytes = -1;
    };
#endif
    static BundleStatus publish(const BundleInput&, const juce::File& destination,
                                const std::atomic<bool>* cancelled = nullptr
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
                                , const TestHooks* hooks = nullptr
#endif
                                );
    static BundleView validate(const juce::File&, const std::atomic<bool>* cancelled = nullptr);
    static bool logicalId(const juce::String&, EntryKind);
    static bool identityValid(const BundleIdentity&);
};
}
