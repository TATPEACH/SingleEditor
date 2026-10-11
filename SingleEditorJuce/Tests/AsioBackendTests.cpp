#include "Audio/JuceAsioBackendAdapter.h"
#include "Audio/SystemAudioDeviceManager.h"

#include <iostream>
#include <stdexcept>

namespace singleeditor
{
struct JuceAsioBackendTestAccess
{
    static DeviceSwitchAccess access()
    {
        auto state = std::make_shared<DeviceSwitchAccess::State>(); state->windowOpen.store(true);
        return DeviceSwitchAccess(std::move(state));
    }
    static void revoke(const DeviceSwitchAccess& access) { access.state->windowOpen.store(false); }
    static std::unique_ptr<JuceAsioBackendAdapter> make(JuceAsioBackendAdapter::Factory factory,
        JuceAsioBackendAdapter::RegistryReader registry = [] { return std::optional<std::vector<AsioDriverRegistration>>{}; },
        JuceAsioBackendAdapter::DeviceFactory device = {})
    {
        return std::unique_ptr<JuceAsioBackendAdapter>(new JuceAsioBackendAdapter(std::move(factory), std::move(registry), std::move(device)));
    }
};
}

namespace
{
using Adapter = singleeditor::JuceAsioBackendAdapter;
void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
struct Observations
{
    int factories = 0, types = 0, scans = 0, creates = 0;
    juce::StringArray names { "Fixture Multichannel", "Fixture Stereo" };
    int aliveDevices = 0, opens = 0, closes = 0;
    bool failedDriver = false, noOutputs = false;
    bool failOpen = false, coerceBuffer = false, changeOutputNames = false;
    std::function<void()> duringOpen;
};

class FixtureType final : public juce::AudioIODeviceType
{
public:
    explicit FixtureType(Observations& value) : AudioIODeviceType("ASIO"), observations(value) { ++observations.types; }
    ~FixtureType() override { --observations.types; }
    void scanForDevices() override { ++observations.scans; }
    juce::StringArray getDeviceNames(bool) const override { return observations.names; }
    int getDefaultDeviceIndex(bool) const override { throw std::runtime_error("discovery must not select a default driver"); }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    { ++observations.creates; return nullptr; }
private:
    Observations& observations;
};

class CapabilityDevice final : public juce::AudioIODevice
{
public:
    explicit CapabilityDevice(Observations& value) : AudioIODevice("Same display name", "ASIO"), observations(value)
    { ++observations.aliveDevices; }
    ~CapabilityDevice() override { close(); --observations.aliveDevices; }
    juce::StringArray getOutputChannelNames() override
    {
        if (observations.noOutputs) return {};
        return observations.changeOutputNames && isOpened ? juce::StringArray{ "Out 2", "Out 1", "Out 3", "Out 4" }
                                                         : juce::StringArray{ "Out 1", "Out 2", "Out 3", "Out 4" };
    }
    juce::StringArray getInputChannelNames() override { return { "In 1" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 64, 128, 256 }; }
    int getDefaultBufferSize() override { return 128; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger& outputs, double rate, int buffer) override
    {
        ++observations.opens;
        if (observations.failOpen) return "Private driver open failure";
        isOpened = true; channels = outputs; currentRate = rate;
        currentBuffer = observations.coerceBuffer ? 512 : buffer;
        if (observations.duringOpen) observations.duringOpen();
        return {};
    }
    void close() override { if (isOpened) ++observations.closes; isOpened = false; }
    bool isOpen() override { return isOpened; }
    void start(juce::AudioIODeviceCallback*) override { throw std::runtime_error("capability prepare must not install a playback callback"); }
    void stop() override {}
    bool isPlaying() override { return false; }
    juce::String getLastError() override { return observations.failedDriver ? "Private vendor error" : juce::String{}; }
    int getCurrentBufferSizeSamples() override { return currentBuffer; }
    double getCurrentSampleRate() override { return currentRate; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { return channels; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 96; }
    int getInputLatencyInSamples() override { return 0; }
    bool hasControlPanel() const override { return true; }
    bool showControlPanel() override { throw std::runtime_error("capability query never opens a panel"); }
private:
    Observations& observations;
    bool isOpened = false;
    double currentRate = 96000;
    int currentBuffer = 0;
    juce::BigInteger channels;
};

void identityAndCapabilities()
{
    Observations observations;
    observations.names = { "Same display name", "Same display name", "Unresolved name" };
    std::vector<singleeditor::AsioDriverRegistration> registry {
        { "Same display name", "{00000000-0000-0000-0000-000000000001}", "First", "First.dll" },
        { "Same display name", "{00000000-0000-0000-0000-000000000002}", "Second", "Second.dll" }
    };
    int registryReads = 0, creates = 0;
    std::string createdId;
    std::function<void()> duringCreate;
    auto adapter = singleeditor::JuceAsioBackendTestAccess::make(
        [&] { ++observations.factories; return std::make_unique<FixtureType>(observations); },
        [&] { ++registryReads; return std::optional{registry}; },
        [&](juce::AudioIODeviceType*, const singleeditor::AsioDriverRegistration& registration)
        {
            ++creates; createdId = registration.classId;
            auto device = std::make_unique<CapabilityDevice>(observations);
            if (duringCreate) duringCreate();
            return device;
        });
    require(!adapter->prepared() && registryReads == 0, "passive identity/capability reads do no work");
    const auto& list = adapter->scanDevices(Adapter::Activation::asioSettings);
    require(list.drivers.size() == 3 && list.drivers[0].stableId != list.drivers[1].stableId
                && list.drivers[2].stableId.empty(), "same-name drivers retain distinct CLSIDs; unresolved identity stays unavailable");
    const auto generation = list.generation;
    const auto id = list.drivers[1].stableId;
    const auto access = singleeditor::JuceAsioBackendTestAccess::access();
    require(adapter->prepareCapabilities(generation, id, {}) == Adapter::PrepareStatus::accessNotGranted
                && creates == 0, "catalog browsing cannot initialize a driver without a switch window");
    require(adapter->prepareCapabilities(generation - 1, id, access) == Adapter::PrepareStatus::staleCatalog && creates == 0,
            "stale display generation cannot initialize a driver");
    require(adapter->prepareCapabilities(generation, "Same display name", access) == Adapter::PrepareStatus::identityUnavailable && creates == 0,
            "display names are never accepted as exact driver credentials");
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::ready
                && creates == 1 && createdId == registry[1].classId && observations.aliveDevices == 1,
            "selected same-name driver is constructed by exact CLSID");
    const auto& caps = adapter->prepared()->capabilities;
    require(caps.outputNames == std::vector<std::string>{"Out 1", "Out 2", "Out 3", "Out 4"}
                && caps.inputNames == std::vector<std::string>{"In 1"}
                && caps.sampleRates == std::vector<double>{44100, 48000, 96000}
                && caps.bufferSizes == std::vector<int>{64, 128, 256}
                && caps.reportedSampleRate == 96000.0 && !caps.reportedBufferSize
                && caps.preferredBufferSize == 128 && caps.hasControlPanel && !caps.clockState,
            "capabilities are reported values; unopened buffer and unavailable clock identity are not invented");
    require(observations.opens == 0, "capability prepare has not requested a playback stream");
    const auto readsBeforePassive = registryReads;
    (void) adapter->prepared();
    require(registryReads == readsBeforePassive && creates == 1, "reading prepared facts does not revisit a driver");
    require(adapter->prepareCapabilities(generation, list.drivers[0].stableId, access) == Adapter::PrepareStatus::candidateBusy
                && adapter->prepared()->stableId == id && creates == 1, "pending owner is preserved until explicit release");
    adapter->releasePrepared();
    require(observations.aliveDevices == 0 && !adapter->prepared(), "release destroys initialized candidate");

    registry[1].serverPath = "Changed.dll";
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::registrationChanged && creates == 1,
            "changed registration is rejected before driver initialization");
    registry[1].serverPath = "Second.dll";
    observations.failedDriver = true;
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::driverUnavailable
                && !adapter->prepared() && observations.aliveDevices == 0, "failed driver is released without a ready snapshot");
    observations.failedDriver = false;
    observations.noOutputs = true;
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::unsupportedCapabilities
                && observations.aliveDevices == 0, "unusable output capabilities cannot become ready");
    observations.noOutputs = false;
    duringCreate = [&] { registry[1].serverPath = "ChangedDuringCreate.dll"; };
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::registrationChanged
                && observations.aliveDevices == 0, "registration is revalidated after driver entry");
    registry[1].serverPath = "Second.dll";
    duringCreate = [&] { adapter->release(); };
    require(adapter->prepareCapabilities(generation, id, access) == Adapter::PrepareStatus::staleCatalog
                && observations.aliveDevices == 0 && !adapter->prepared(), "retired generation cannot publish a late candidate");
}

void candidateStream()
{
    Observations observations;
    observations.names = { "Same display name" };
    const std::vector<singleeditor::AsioDriverRegistration> registry {
        { "Same display name", "{00000000-0000-0000-0000-000000000001}", "Selected", "Selected.dll" }
    };
    auto adapter = singleeditor::JuceAsioBackendTestAccess::make(
        [&] { return std::make_unique<FixtureType>(observations); },
        [&] { return std::optional{registry}; },
        [&](juce::AudioIODeviceType*, const singleeditor::AsioDriverRegistration&) { return std::make_unique<CapabilityDevice>(observations); });
    adapter->scanDevices(Adapter::Activation::userSelection);
    const auto prepare = [&]
    {
        require(adapter->prepareCapabilities(adapter->catalog().generation, adapter->catalog().drivers[0].stableId,
                    singleeditor::JuceAsioBackendTestAccess::access()) == Adapter::PrepareStatus::ready, "prepare selected stream fixture");
    };
    const Adapter::StreamRequest request { 96000.0, 128, { 1, 3 } };
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::noCandidate, "cannot open without an exact prepared owner");
    prepare();
    auto invalid = request; invalid.sampleRate = 88200;
    require(adapter->openPrepared(invalid).status == Adapter::OpenStatus::unsupportedRequest && observations.opens == 0,
            "unsupported requested clock never starts a fallback stream");
    invalid = request; invalid.physicalOutputs = { 1, 1 };
    require(adapter->openPrepared(invalid).status == Adapter::OpenStatus::unsupportedRequest && observations.opens == 0,
            "duplicate physical indices are not silently normalized");
    auto ready = adapter->openPrepared(request);
    require(ready.status == Adapter::OpenStatus::ready && ready.actual
                && ready.actual->sampleRate == 96000.0 && ready.actual->bufferSamples == 128
                && ready.actual->physicalOutputs == std::vector<int>{1, 3}
                && ready.actual->bitDepth == 24 && ready.actual->outputLatencySamples == 96
                && adapter->opened() && observations.opens == 1, "exact noncontiguous stream and actual latency are retained");
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::alreadyOpen && observations.opens == 1,
            "a ready candidate cannot be implicitly reopened");
    adapter->releasePrepared();
    require(!adapter->opened() && !adapter->prepared() && observations.closes == 1 && observations.aliveDevices == 0,
            "releasing a ready candidate closes its real IODevice owner");

    prepare(); observations.failOpen = true;
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::driverFailed
                && !adapter->prepared() && !adapter->opened() && observations.aliveDevices == 0,
            "driver open failure does not publish requested values or leak an initialized owner");
    observations.failOpen = false;
    prepare(); observations.coerceBuffer = true;
    const auto coerced = adapter->openPrepared(request);
    require(coerced.status == Adapter::OpenStatus::configurationChanged && coerced.actual
                && coerced.actual->bufferSamples == 512 && coerced.requested.bufferSamples == 128
                && !adapter->opened() && observations.aliveDevices == 0,
            "coercion reports actual versus requested and rejects the candidate");
    observations.coerceBuffer = false;
    prepare(); observations.changeOutputNames = true;
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::configurationChanged && observations.aliveDevices == 0,
            "changed channel order invalidates the candidate even when counts and mask match");
    observations.changeOutputNames = false;
    prepare();
    observations.duringOpen = [&]
    {
        require(adapter->prepareCapabilities(adapter->catalog().generation, adapter->catalog().drivers[0].stableId,
                    singleeditor::JuceAsioBackendTestAccess::access()) == Adapter::PrepareStatus::candidateBusy,
                "reentrant selection cannot create a second driver during open");
        adapter->releasePrepared();
    };
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::cancelled
                && !adapter->prepared() && !adapter->opened() && observations.aliveDevices == 0,
            "cancellation during driver open rejects late success and releases the local owner");
    observations.duringOpen = {};
    prepare();
    observations.duringOpen = [&] { adapter->scanDevices(Adapter::Activation::asioSettings); };
    require(adapter->openPrepared(request).status == Adapter::OpenStatus::staleCatalog
                && !adapter->prepared() && observations.aliveDevices == 0,
            "a changed catalog during open cannot publish into the new generation");
    observations.duringOpen = {};
}

void capabilityInspection()
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Observations observations;
        observations.names = { "Same display name" };
        const std::vector<singleeditor::AsioDriverRegistration> registry {
            { "Same display name", "{00000000-0000-0000-0000-000000000001}", "Selected", "Selected.dll" }
        };
        std::atomic<bool> cancelled { scenario == 1 };
        auto adapter = singleeditor::JuceAsioBackendTestAccess::make(
            [&] { return std::make_unique<FixtureType>(observations); },
            [&] { return std::optional{registry}; },
            [&](juce::AudioIODeviceType*, const singleeditor::AsioDriverRegistration&)
            {
                auto device = std::make_unique<CapabilityDevice>(observations);
                if (scenario == 2) cancelled.store(true);
                return device;
            });
        const auto& catalog = adapter->scanDevices(Adapter::Activation::userSelection);
        const auto access = singleeditor::JuceAsioBackendTestAccess::access();
        const auto result = adapter->inspectCapabilities(catalog.generation, catalog.drivers[0].stableId, access, cancelled);
        require(!adapter->prepared() && !adapter->opened() && observations.aliveDevices == 0 && observations.opens == 0,
            "inspection disposes temporary device before returning and never requests a playback stream");
        if (scenario == 0)
            require(result.preparation == Adapter::PrepareStatus::ready && result.capabilities
                && result.capabilities->capabilities.outputNames.size() == 4 && result.capabilities->capabilities.reportedSampleRate == 96000.0,
                "detached inspected values retain actual capabilities after release");
        else
            require(result.preparation == Adapter::PrepareStatus::cancelled && !result.capabilities,
                "inspection cancelled before or during driver creation cannot publish review data");
    }
}

void revokedSwitchWindow()
{
    Observations observations;
    observations.names = { "Same display name" };
    const std::vector<singleeditor::AsioDriverRegistration> registrations {
        {"Same display name", "{00000000-0000-0000-0000-000000000001}", "Selected", "Selected.dll"}};
    auto adapter = singleeditor::JuceAsioBackendTestAccess::make(
        [&] { return std::make_unique<FixtureType>(observations); }, [&] { return std::optional{registrations}; },
        [&](juce::AudioIODeviceType*, const singleeditor::AsioDriverRegistration&) { return std::make_unique<CapabilityDevice>(observations); });
    adapter->scanDevices(Adapter::Activation::userSelection);
    const auto id = adapter->catalog().drivers[0].stableId;
    auto access = singleeditor::JuceAsioBackendTestAccess::access();
    require(adapter->prepareCapabilities(adapter->catalog().generation, id, access) == Adapter::PrepareStatus::ready, "prepare under a live window");
    singleeditor::JuceAsioBackendTestAccess::revoke(access);
    require(adapter->openPrepared({96000, 128, {1, 3}}).status == Adapter::OpenStatus::cancelled
        && observations.opens == 0 && observations.aliveDevices == 0, "revoked window cannot open the prepared driver");
    access = singleeditor::JuceAsioBackendTestAccess::access();
    require(adapter->prepareCapabilities(adapter->catalog().generation, id, access) == Adapter::PrepareStatus::ready, "prepare another live window");
    observations.duringOpen = [&] { singleeditor::JuceAsioBackendTestAccess::revoke(access); };
    require(adapter->openPrepared({96000, 128, {1, 3}}).status == Adapter::OpenStatus::cancelled
        && observations.aliveDevices == 0 && !adapter->takeOpenedOutput(), "revocation during driver open cannot publish an output");
}

void discoveryContract()
{
    revokedSwitchWindow();
    Observations observations;
    auto adapter = singleeditor::JuceAsioBackendTestAccess::make([&]()
    {
        ++observations.factories;
        return std::make_unique<FixtureType>(observations);
    });
    for (int i = 0; i != 100; ++i)
        require(adapter->catalog().status == Adapter::Status::inactive && adapter->catalog().generation == 0,
                "passive status does not activate ASIO");
    require(observations.factories == 0 && observations.scans == 0 && observations.creates == 0,
            "unrequested factory, registry and driver work stays zero");
    require(adapter->scanDevices(static_cast<Adapter::Activation>(99)).status == Adapter::Status::invalidActivation
                && observations.factories == 0, "invalid activation cannot start discovery");
    const auto& selected = adapter->scanDevices(Adapter::Activation::userSelection);
    require(selected.status == Adapter::Status::available && selected.drivers.size() == 2
                && selected.drivers[0].displayName == "Fixture Multichannel", "explicit scan returns neutral display entries");
    require(observations.factories == 1 && observations.types == 1 && observations.scans == 1 && observations.creates == 0,
            "scan creates a type exactly once, never an opened or default driver");
    const auto firstGeneration = selected.generation;
    for (int i = 0; i != 100; ++i) (void) adapter->catalog();
    require(observations.scans == 1, "repainting a catalog is not a rescan");
    observations.names = { "Changed driver list" };
    const auto& refreshed = adapter->scanDevices(Adapter::Activation::asioSettings);
    require(refreshed.generation > firstGeneration && refreshed.drivers.size() == 1
                && refreshed.drivers[0].displayName == "Changed driver list" && observations.factories == 1,
            "explicit refresh replaces stale display entries without a second factory");
    const auto refreshedGeneration = refreshed.generation;
    adapter->release();
    require(observations.types == 0 && adapter->catalog().drivers.empty()
                && adapter->catalog().status == Adapter::Status::inactive
                && adapter->catalog().generation > refreshedGeneration, "release destroys type and invalidates catalog");
    observations.names.clear();
    require(adapter->scanDevices(Adapter::Activation::persistedSelection).status == Adapter::Status::noDrivers
                && observations.factories == 2 && observations.creates == 0,
            "persisted explicit intent may rediscover, but no driver is not a fallback selection");
    adapter.reset();
    require(observations.types == 0, "destruction releases discovered type");

    int failedFactories = 0;
    auto unavailable = singleeditor::JuceAsioBackendTestAccess::make([&]() -> std::unique_ptr<juce::AudioIODeviceType>
    { ++failedFactories; return {}; });
    require(unavailable->scanDevices(Adapter::Activation::userSelection).status == Adapter::Status::unavailable,
            "factory absence is explicit, not an invented empty-success device");
    (void) unavailable->catalog();
    require(failedFactories == 1, "failure does not start a retry loop");
}

void realCompiledBoundary(bool scanInstalledDrivers)
{
    juce::ScopedJuceInitialiser_GUI initialise;
    singleeditor::SystemAudioDeviceManager system;
    const auto& types = system.getAvailableDeviceTypes();
    require(!types.isEmpty(), "system device factory remains available");
    for (const auto* type : types)
        require(type->getTypeName() != "ASIO", "normal Windows list never instantiates the compiled ASIO type");
    require(system.getCurrentAudioDevice() == nullptr, "catalog inspection does not open an audio endpoint");
    Adapter real;
    require(real.catalog().status == Adapter::Status::inactive, "real adapter starts inactive");
   #if JUCE_ASIO
    require(Adapter::isCompiled(), "dedicated integration binary links external-SDK ASIO");
    if (scanInstalledDrivers)
    {
        const auto& catalog = real.scanDevices(Adapter::Activation::asioSettings);
        require(catalog.status == Adapter::Status::available || catalog.status == Adapter::Status::noDrivers,
                "explicit installed-driver enumeration reaches the unmodified JUCE adapter");
        auto identified = 0;
        for (const auto& driver : catalog.drivers) if (!driver.stableId.empty()) ++identified;
        std::cout << "Explicit installed ASIO catalog: " << catalog.drivers.size() << " entries, " << identified << " exact identities (none opened)\n";
        real.release();
        require(real.catalog().status == Adapter::Status::inactive, "real type is releasable");
    }
   #else
    (void) scanInstalledDrivers;
    require(!Adapter::isCompiled()
                && real.scanDevices(Adapter::Activation::asioSettings).status == Adapter::Status::notCompiled,
            "ordinary non-ASIO target cannot accidentally initialize optional backend");
   #endif
}
}

int main(int argc, char** argv)
{
    try
    {
        discoveryContract();
        identityAndCapabilities();
        capabilityInspection();
        candidateStream();
        realCompiledBoundary(argc == 2 && std::string(argv[1]) == "--scan-installed");
        std::cout << "AsioBackendTests passed: lazy discovery, exact identities, capabilities, exact candidate stream, failure/coercion/cancel, release, target isolation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "AsioBackendTests FAILED: " << error.what() << '\n';
        return 1;
    }
}
