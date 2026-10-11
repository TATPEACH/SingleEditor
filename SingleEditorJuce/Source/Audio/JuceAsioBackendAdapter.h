#pragma once

#include "Audio/NativeAsioDevice.h"
#include "Audio/DeviceOutput.h"
#include "Audio/DeviceSwitchAccess.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace juce { class AudioIODeviceType; }

namespace singleeditor
{
// Control-thread discovery seam. Construction, status reads and release do not
// scan the registry or open a driver. No JUCE ASIO class escapes this adapter.
class JuceAsioBackendAdapter final
{
public:
    enum class Activation { userSelection, persistedSelection, asioSettings };
    enum class Status { inactive, available, noDrivers, notCompiled, unavailable, invalidActivation };
    struct Driver
    {
        std::string displayName;
        // Empty when discovery cannot establish a registration. Never derive
        // an identity or a speaker role from the display name/channel index.
        std::string stableId;
    };
    enum class PrepareStatus
    {
        ready, accessNotGranted, staleCatalog, identityUnavailable, registrationChanged,
        driverUnavailable, unsupportedCapabilities, candidateBusy, cancelled
    };
    using Capabilities = DeviceBackendCapabilities;
    using PreparedCapabilities = PreparedDeviceCapabilities;
    struct StreamRequest
    {
        double sampleRate = 0;
        int bufferSamples = 0;
        std::vector<int> physicalOutputs;
    };
    struct StreamState
    {
        double sampleRate = 0;
        int bufferSamples = 0, bitDepth = 0;
        std::vector<int> physicalOutputs;
        std::optional<int> inputLatencySamples, outputLatencySamples;
        std::optional<AsioClockState> clockState;
        std::uint64_t eventGeneration = 0;
    };
    enum class OpenStatus { ready, noCandidate, staleCatalog, alreadyOpen, unsupportedRequest, driverFailed, configurationChanged, candidateBusy, cancelled };
    struct OpenResult
    {
        OpenStatus status = OpenStatus::noCandidate;
        StreamRequest requested;
        std::optional<StreamState> actual;
    };
    struct Catalog
    {
        Status status = Status::inactive;
        std::uint64_t generation = 0;
        std::vector<Driver> drivers;
    };
    struct CandidateResult
    {
        Status discovery = Status::inactive;
        PrepareStatus preparation = PrepareStatus::accessNotGranted;
        OpenStatus opening = OpenStatus::noCandidate;
        std::unique_ptr<DeviceOutput> output;
    };
    struct InspectionResult
    {
        Status discovery = Status::inactive;
        PrepareStatus preparation = PrepareStatus::accessNotGranted;
        std::optional<PreparedCapabilities> capabilities;
    };

    JuceAsioBackendAdapter();
    ~JuceAsioBackendAdapter();
    JuceAsioBackendAdapter(const JuceAsioBackendAdapter&) = delete;
    JuceAsioBackendAdapter& operator=(const JuceAsioBackendAdapter&) = delete;

    static bool isCompiled() noexcept;
    // One worker-owned explicit selection, including disposal on any failure.
    // The coordinator supplies an actual revocable old-owner switch window.
    static CandidateResult prepareSelectedOutput(Activation, const std::string& stableId,
        const StreamRequest&, const DeviceSwitchAccess&, const std::atomic<bool>& cancelled);
    // Worker-only capability inspection. Returns detached values after closing
    // its temporary device; it never opens a requested playback stream.
    static InspectionResult inspectSelectedCapabilities(Activation, const std::string& stableId,
        const DeviceSwitchAccess&, const std::atomic<bool>& cancelled, std::atomic<bool>* awaitingPanel = nullptr);
    InspectionResult inspectCapabilities(std::uint64_t catalogGeneration, const std::string& stableId,
        const DeviceSwitchAccess&, const std::atomic<bool>& cancelled, std::atomic<bool>* awaitingPanel = nullptr);
    const Catalog& catalog() const noexcept { return current; }
    const Catalog& scanDevices(Activation);
    // Construction itself enters the driver. The caller must first hold the
    // D106 exclusive switch window; this is not a passive settings read.
    PrepareStatus prepareCapabilities(std::uint64_t catalogGeneration, const std::string& stableId,
        const DeviceSwitchAccess&, std::atomic<bool>* awaitingPanel = nullptr);
    const std::optional<PreparedCapabilities>& prepared() const noexcept { return preparedState; }
    // Still inside the same caller-owned switch window. Ready means an exact
    // silent candidate stream, not a committed route or playable Document.
    OpenResult openPrepared(const StreamRequest&);
    const std::optional<StreamState>& opened() const noexcept { return openedState; }
    // Transfers the exact silent stream and its owning JUCE type. The caller
    // still needs D105 route validation and an engine commit before playback.
    std::unique_ptr<DeviceOutput> takeOpenedOutput();
    void releasePrepared() noexcept;
    void release() noexcept;

private:
    friend struct JuceAsioBackendTestAccess;
    using Factory = std::function<std::unique_ptr<juce::AudioIODeviceType>()>;
    using RegistryReader = std::function<std::optional<std::vector<AsioDriverRegistration>>() >;
    using DeviceFactory = std::function<std::unique_ptr<juce::AudioIODevice>(juce::AudioIODeviceType*, const AsioDriverRegistration&)>;
    explicit JuceAsioBackendAdapter(Factory, RegistryReader = {}, DeviceFactory = {});
    std::optional<std::vector<AsioDriverRegistration>> readRegistrations() const;
    std::unique_ptr<juce::AudioIODeviceType> type;
    Factory testFactory;
    RegistryReader testRegistry;
    DeviceFactory testDevice;
    std::vector<AsioDriverRegistration> registrations;
    Catalog current;
    // Destroy the device before its owning JUCE type.
    std::unique_ptr<juce::AudioIODevice> candidate;
    std::optional<PreparedCapabilities> preparedState;
    std::optional<StreamState> openedState;
    DeviceSwitchAccess preparedAccess;
    bool driverAccessInProgress = false;
    std::uint64_t preparationGeneration = 0;
};
}
