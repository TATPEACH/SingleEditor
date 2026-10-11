#include "Diagnostics/DiagnosticSnapshot.h"
#include "Diagnostics/BoundedJson.h"
#include "Diagnostics/ReproductionFiles.h"
#include <iostream>
#include <stdexcept>

using namespace singleeditor::diagnostics;
namespace
{
void require(bool condition, const char* message)
{ if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
BundleIdentity identity()
{ return { "0.3.0", "c6ef07b8", juce::String::repeatedString("a", 64), "Release", "test", "x64", "x64", "10.0.26100", false }; }
struct Provider final : LoadedDiagnosticProvider
{
    mutable int calls = 0;
    bool throws = false;
    ProviderEvidence evidence;
    ProviderEvidence snapshot() const override
    { ++calls; if (throws) throw std::runtime_error("private path must not be exported"); return evidence; }
};
juce::var section(const PreparedSnapshot& capture, const char* id)
{
    for (const auto& e : capture.content().entries)
        if (e.id == "providers/" + juce::String(id) + ".json")
        { juce::var v; require(BoundedJson::parse(e.bytes, v), "parse captured structured section"); return v; }
    require(false, "expected explicitly supplied provider section"); return {};
}
juce::String allText(const PreparedSnapshot& capture)
{
    juce::String text;
    for (const auto& e : capture.content().entries) text += juce::String::fromUTF8(static_cast<const char*>(e.bytes.getData()), static_cast<int>(e.bytes.getSize()));
    return text;
}
}
int main()
{
    Provider audio, codec, recovery;
    require(audio.calls == 0, "owner construction does no diagnostic capture");
    auto invalidIdentity = identity(); invalidIdentity.binarySha256 = "unknown";
    require(!DiagnosticSnapshot::capture(invalidIdentity, "test-fixture", { { ProviderKind::audio, &audio } }).status() && audio.calls == 0,
            "invalid artifact identity fails before provider capture");
    const auto empty = DiagnosticSnapshot::capture(identity(), "test-fixture", {});
    require(empty.status() && empty.preview().providerCount == 0 && empty.content().entries.empty() && audio.calls == 0,
            "absent providers are neither enumerated nor woken");
    Provider cache;
    cache.evidence.fields = { { Field::cacheResult, "not-reused" }, { Field::cacheLookupReason, "seal-unavailable" } };
    const auto observedCache = DiagnosticSnapshot::capture(identity(), "test-fixture", { { ProviderKind::cache, &cache } });
    require(observedCache.status() && section(observedCache, "cache")["fields"]["cacheLookupReason"]["value"].toString() == "seal-unavailable"
            && cache.calls == 1 && audio.calls == 0, "cache reason survives the shared serializer without querying another provider");
    audio.evidence.fields = {
        { Field::backend, "WASAPI" }, { Field::driverVendor, "Fixture Vendor" }, { Field::activeRate, 48000 },
        { Field::endpointIdentity, "{private-endpoint-guid}-user-device" },
        { Field::requestedBuffer, juce::var(), Unavailable::notSupported }
    };
    audio.evidence.identifiers = { { LocalIdentifierKind::path, "C:\\Users\\fixture-private-user\\private-song.wav" },
                                  { LocalIdentifierKind::deviceName, "Private Custom Speakers" },
                                  { LocalIdentifierKind::serial, "private-serial-xyz" } };
    FailureCapsule capsule; capsule.stage = FailureStage::negotiation; capsule.code = -123; capsule.counters = { 1, 2, 3, 4, 5, 6 };
    audio.evidence.latestFailure = capsule; audio.evidence.operationIds.push_back(capsule.operationId);
    codec.evidence.fields = { { Field::container, "WEM" }, { Field::selectedReader, "vgmstream" },
                             { Field::dependencyIdentity, "C:\\private\\decoder.exe" } };
    recovery.throws = true;
    const std::vector<LoadedProviderRef> live { { ProviderKind::audio, &audio }, { ProviderKind::codec, &codec }, { ProviderKind::recovery, &recovery } };
    auto safe = DiagnosticSnapshot::capture(identity(), "gui", live);
    require(safe.status() && safe.preview().tier == PrivacyTier::supportSafe && safe.preview().providerCount == 3
            && safe.preview().categories.size() == 2 && audio.calls == 1 && codec.calls == 1 && recovery.calls == 1,
            "explicit capture visits each already-loaded provider once");
    const auto audioSection = section(safe, "audio"), codecSection = section(safe, "codec"), failedSection = section(safe, "recovery");
    const auto publicFields = audioSection["fields"];
    require(publicFields["activeRate"]["value"] == juce::var(48000)
            && publicFields["requestedBuffer"]["unavailable"].toString() == "not-supported"
            && publicFields["activeChannels"]["unavailable"].toString() == "not-reported", "mandatory matrix emits values or typed unavailable reasons");
    const auto endpoint = publicFields["endpointDigest"]["value"].toString();
    require(endpoint.length() == 64 && endpoint.containsOnly("0123456789abcdef"), "endpoint is represented by a keyed digest");
    require(audioSection["latestFailure"]["code"] == juce::var(static_cast<juce::int64>(-123))
            && audioSection["latestFailure"]["operationId"].toString() == capsule.operationId.toString(), "typed latest failure and operation correlation preserved");
    require(codecSection["fields"]["dependencyIdentity"]["unavailable"].toString() == "invalid-value", "path in a public technical field is excluded");
    require(failedSection["captureStatus"].toString() == "capture-failed"
            && failedSection["fields"]["recoveryStage"]["unavailable"].toString() == "capture-failed", "throwing provider is locally unavailable without leaking exception text");
    const auto safeText = allText(safe);
    for (const auto& forbidden : { "private-endpoint", "private-user", "private-song.wav", "Private Custom Speakers", "private-serial", "C:\\", "private path" })
        require(!safeText.contains(forbidden), "support-safe snapshot excludes private identifiers and arbitrary failure prose");
    auto another = DiagnosticSnapshot::capture(identity(), "gui", live);
    require(section(another, "audio")["fields"]["endpointDigest"]["value"].toString() != endpoint,
            "same device cannot be linked through endpoint digest across bundles");
    require(allText(safe) == safeText, "a later capture does not mutate the prior immutable snapshot");

    SnapshotSelection local;
    local.localIdentifiers.insert(LocalIdentifierKind::deviceName);
    const auto selected = DiagnosticSnapshot::capture(identity(), "gui", live, local);
    require(selected.status() && selected.preview().tier == PrivacyTier::localIdentifiers
            && allText(selected).contains("Private Custom Speakers") && !allText(selected).contains("private-song.wav")
            && !allText(selected).contains("private-serial"), "local opt-in includes only chosen identifier categories");
    SnapshotSelection attachment;
    const char opaque[] = "not opened or executed";
    attachment.chosenReproduction.push_back({ "reproduction/sample.bin", EntryKind::reproduction, 1, juce::MemoryBlock(opaque, sizeof(opaque)) });
    const auto callsBefore = audio.calls;
    require(!DiagnosticSnapshot::capture(identity(), "gui", live, attachment).status() && audio.calls == callsBefore,
            "unconsented reproduction refused before querying providers");
    attachment.reproductionConsent = true;
    auto reproduction = DiagnosticSnapshot::capture(identity(), "gui", live, attachment);
    require(reproduction.status() && reproduction.preview().tier == PrivacyTier::reproductionMaterial
            && reproduction.preview().attachmentCount == 1 && !allText(reproduction).contains("private-song.wav"),
            "reproduction consent does not implicitly grant local identifier consent");
    require(!DiagnosticSnapshot::capture(identity(), "test-fixture", { { ProviderKind::audio, nullptr } }).status(), "no null provider/factory placeholders");
    require(!DiagnosticSnapshot::capture(identity(), "test-fixture", { { ProviderKind::audio, &audio }, { ProviderKind::audio, &audio } }).status(), "duplicate provider rejected without extra callbacks");

    Provider malformed;
    malformed.evidence.fields = { { Field::activeRate, 48000 }, { Field::activeRate, 96000 } };
    const auto invalid = DiagnosticSnapshot::capture(identity(), "test-fixture", { { ProviderKind::audio, &malformed } });
    require(invalid.status() && section(invalid, "audio")["captureStatus"].toString() == "capture-failed", "ambiguous provider fields degrade only that provider");
    Provider vectors;
    juce::Array<juce::var> enormous; for (int i = 0; i < 513; ++i) enormous.add(1.0);
    vectors.evidence.fields = { { Field::outputMatrix, juce::var(enormous) } };
    const auto bounded = DiagnosticSnapshot::capture(identity(), "test-fixture", { { ProviderKind::spatial, &vectors } });
    require(bounded.status() && section(bounded, "spatial")["fields"]["outputMatrix"]["unavailable"].toString() == "invalid-value", "matrix capacity is bounded");

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-SnapshotTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create synthetic review-export fixture directory");
    const auto chosen = root.getChildFile("private-selected.json");
    const juce::String original = "{\"private-content\":\"opaque chosen bytes\"}";
    require(chosen.replaceWithText(original), "write explicitly selected reproduction file");
    std::atomic<bool> stopped { false };
    auto captured = ReproductionFiles::capture({ chosen }, stopped);
    require(captured.status && captured.entries.size() == 1 && captured.entries[0].bytes.toString() == original
        && captured.entries[0].id == "reproduction/file-1.bin", "capture opaque bytes without exporting the private source name");
    const auto replacement = root.getChildFile("replacement.json");
    require(replacement.replaceWithText("replacement after capture") && replacement.replaceFileIn(chosen),
        "capture releases input ownership before review and permits atomic replacement");
    SnapshotSelection frozenSelection; frozenSelection.reproductionConsent = true;
    frozenSelection.chosenReproduction = std::move(captured.entries);
    const auto frozen = DiagnosticSnapshot::capture(identity(), "test-fixture", {}, frozenSelection);
    const auto frozenZip = root.getChildFile("frozen.zip");
    require(frozen.status() && DiagnosticSnapshot::publishReviewed(frozen, frozen.preview().snapshotId, frozenZip),
        "publish selected frozen bytes after original path changes");
    {
        juce::ZipFile zip(frozenZip);
        std::unique_ptr<juce::InputStream> payload(zip.createStreamForEntry(zip.getIndexOfFileName("reproduction/file-1.bin")));
        require(payload && payload->readEntireStreamAsString() == original && chosen.loadFileAsString() == "replacement after capture",
            "reviewed archive keeps exact captured bytes without rewriting the selected source");
    }
    auto missing = ReproductionFiles::capture({ chosen, root.getChildFile("missing.json") }, stopped);
    require(!missing.status && missing.entries.empty(), "a failed selected file cannot silently produce a partial attachment set");
    stopped.store(true);
    require(ReproductionFiles::capture({ chosen }, stopped).status.error == BundleError::cancelled,
        "cancelled capture stops before reading selected contents");
    stopped.store(false);
    const auto large = root.getChildFile("large.bin");
    {
        juce::FileOutputStream out(large);
        require(out.openedOk() && out.setPosition(DiagnosticBundle::maximumEntryBytes) && out.writeByte(1), "create oversized chosen fixture");
    }
    require(ReproductionFiles::capture({ large }, stopped).status.error == BundleError::budget,
        "per-file budget rejects a real oversized source without publishing bytes");
    require(ReproductionFiles::capture(std::vector<juce::File>(ReproductionFiles::maximumFiles + 1, chosen), stopped).status.error == BundleError::budget,
        "file-count budget is checked before reading selections");
    auto maximumSelection = ReproductionFiles::capture(std::vector<juce::File>(ReproductionFiles::maximumFiles, chosen), stopped);
    SnapshotSelection maximumSnapshot; maximumSnapshot.reproductionConsent = true;
    maximumSnapshot.chosenReproduction = std::move(maximumSelection.entries);
    require(maximumSelection.status && DiagnosticSnapshot::capture(identity(), "test-fixture", {}, maximumSnapshot).status(),
        "file selection and snapshot share the same maximum attachment count");
    const auto withinLimit = root.getChildFile("full-entry.bin");
    {
        juce::FileOutputStream out(withinLimit);
        require(out.openedOk() && out.setPosition(DiagnosticBundle::maximumEntryBytes - 1) && out.writeByte(1), "create exact per-file boundary fixture");
    }
    auto tooMuch = ReproductionFiles::capture(std::vector<juce::File>(4, withinLimit), stopped);
    require(tooMuch.status.error == BundleError::budget && tooMuch.entries.empty(),
        "aggregate budget rejects the complete selection even when each source fits individually");
    const auto destination = root.getChildFile("reviewed.zip");
    require(!DiagnosticSnapshot::publishReviewed(selected, safe.preview().snapshotId, destination) && !destination.exists(), "stale approval cannot publish a changed selection");
    require(DiagnosticSnapshot::publishReviewed(safe, safe.preview().snapshotId, destination)
            && DiagnosticBundle::validate(destination).status, "reviewed synthetic snapshot exports through the common bundle validator");
    const auto beforeExport = audio.calls;
    require(DiagnosticBundle::validate(destination).manifest["origin"].toString() == "gui" && audio.calls == beforeExport,
            "export and validation never requery or retain provider owners");
    require(root.deleteRecursively(), "remove successful test-owned fixture directory");
    std::cout << "DiagnosticSnapshotTests passed (loaded-only capture, mandatory matrix, typed failure, privacy tiers, endpoint unlinkability, reviewed export)\n";
}
