#include "Audio/AudioEngine.h"
#include "Diagnostics/LiveDiagnosticProviders.h"
#include "Diagnostics/NativeDeviceMetadata.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace singleeditor
{
void verifyActualNativeDeviceMetadata(AudioEngine& engine, const juce::var& reviewedAudio, const juce::File& artifacts)
{
    using namespace diagnostics;
    const auto require = [](bool ok, const char* message) { if (!ok) throw std::runtime_error(message); };
    const auto before = engine.getDeviceDiagnostics();
    require(before.active && before.outputEndpoint.isNotEmpty(), "actual open WASAPI owner provides its exact binding");
    const auto fields = reviewedAudio["fields"];
    require(fields["deviceMetadataState"]["value"].toString() == "adapter-resolved",
        "real product worker resolves the captured endpoint's connected adapter");
    require(fields["driverVersion"]["value"].toString().isNotEmpty()
        && fields["driverMetadataSource"]["value"].toString() == "endpoint-connector-adapter-devnode-installed-INF",
        "real installed adapter package version reaches the reviewed product snapshot with source");
    require(!juce::JSON::toString(reviewedAudio).contains(before.outputEndpoint), "raw actual endpoint ID is absent from default preview");
    ProviderEvidence missing, cancelledResult;
    std::atomic<bool> cancelled { false };
    std::thread worker([&]
    {
        missing = readNativeDeviceMetadata("{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}", cancelled);
        cancelled = true;
        cancelledResult = readNativeDeviceMetadata(before.outputEndpoint, cancelled);
    });
    worker.join();
    const auto state = [](const ProviderEvidence& evidence)
    {
        for (const auto& field : evidence.fields) if (field.field == Field::deviceMetadataState) return field.value.toString();
        return juce::String();
    };
    require(state(missing) == "endpoint-unavailable", "missing exact endpoint does not fall back to default or a similarly named device");
    require(state(cancelledResult) == "cancelled", "cancelled metadata request returns without collection");
    CapturedProvider copied;
    copied.kind = ProviderKind::audio;
    copied.evidence.fields.push_back({ Field::endpointIdentity, before.outputEndpoint });
    const BundleIdentity identity { "0.3.0", "native-device", juce::String::repeatedString("a", 64), "Release", "test", "x64", "x64", "test", false };
    const auto first = DiagnosticSnapshot::capture(identity, "test-fixture", { { ProviderKind::audio, &copied } });
    const auto second = DiagnosticSnapshot::capture(identity, "test-fixture", { { ProviderKind::audio, &copied } });
    require(first.status() && second.status(), "actual endpoint is accepted by bundle-scoped identity conversion");
    const auto digest = [](const PreparedSnapshot& snapshot)
    {
        for (const auto& entry : snapshot.content().entries) if (entry.id == "providers/audio.json")
            return juce::JSON::parse(entry.bytes.toString())["fields"]["endpointDigest"]["value"].toString();
        return juce::String();
    };
    require(digest(first).isNotEmpty() && digest(first) != digest(second)
        && digest(first) != fields["endpointDigest"]["value"].toString(), "separate snapshots cannot link the same actual endpoint by digest");
    const auto after = engine.getDeviceDiagnostics();
    require(after.active && before.latestRequest && after.latestRequest && after.outputEndpoint == before.outputEndpoint
        && after.latestRequest->operationId == before.latestRequest->operationId
        && after.active->sampleRate == before.active->sampleRate && after.active->bufferSamples == before.active->bufferSamples,
        "native metadata reads leave device ownership, last request and active configuration unchanged");
    require(artifacts.getChildFile("native-device-metadata.json").replaceWithText(juce::JSON::toString(reviewedAudio, false)),
        "retain default-safe actual metadata evidence");
    std::cout << "ActualNativeDeviceMetadata passed (exact WASAPI binding, connected adapter INF, unavailable endpoint, cancellation, scoped digest, unchanged active device)\n";
}
}
