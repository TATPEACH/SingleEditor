#include "Audio/JuceAsioBackendAdapter.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace singleeditor
{
namespace
{
using Adapter = JuceAsioBackendAdapter;
std::uint64_t nextStreamGeneration()
{
    static std::atomic<std::uint64_t> next {1}; // Entered only for explicit device work.
    return next.fetch_add(1, std::memory_order_relaxed);
}

Adapter::StreamState readStreamState(juce::AudioIODevice& device)
{
    Adapter::StreamState actual;
    actual.sampleRate = device.getCurrentSampleRate();
    actual.bufferSamples = device.getCurrentBufferSizeSamples();
    actual.bitDepth = device.getCurrentBitDepth();
    const auto active = device.getActiveOutputChannels();
    for (int index = 0; index <= active.getHighestBit() && index < 512; ++index)
        if (active[index]) actual.physicalOutputs.push_back(index);
    const auto inputLatency = device.getInputLatencyInSamples();
    const auto outputLatency = device.getOutputLatencyInSamples();
    if (inputLatency >= 0) actual.inputLatencySamples = inputLatency;
    if (outputLatency >= 0) actual.outputLatencySamples = outputLatency;
   #if JUCE_WINDOWS
    actual.clockState = copyAsioClockState(&device);
   #endif
    return actual;
}

bool matchesStream(juce::AudioIODevice& device, const Adapter::StreamState& actual,
    const Adapter::StreamRequest& request, const std::vector<std::string>& expectedInputs,
    const std::vector<std::string>& expectedOutputs, const std::optional<AsioClockState>& expectedClock)
{
    std::vector<std::string> outputNames, inputNames;
    for (const auto& name : device.getOutputChannelNames()) outputNames.push_back(name.toStdString());
    for (const auto& name : device.getInputChannelNames()) inputNames.push_back(name.toStdString());
    const auto clockChanged = expectedClock && expectedClock->currentIndex
        && (!actual.clockState || actual.clockState->currentIndex != expectedClock->currentIndex);
    return actual.sampleRate == request.sampleRate && actual.bufferSamples == request.bufferSamples && actual.bitDepth > 0
        && device.getActiveOutputChannels().getHighestBit() < 512 && actual.physicalOutputs == request.physicalOutputs
        && device.getActiveInputChannels().isZero() && inputNames == expectedInputs && outputNames == expectedOutputs && !clockChanged;
}

class JuceAsioDeviceOutput final : public DeviceOutput
{
public:
    JuceAsioDeviceOutput(std::unique_ptr<juce::AudioIODeviceType> ownerType, std::unique_ptr<juce::AudioIODevice> ownerDevice,
        const Adapter::PreparedCapabilities& prepared, const Adapter::StreamState& opened)
        : type(std::move(ownerType)), device(std::move(ownerDevice)), capabilities(prepared.capabilities), original(opened), eventGeneration(opened.eventGeneration)
    {
        state = { "ASIO", prepared.stableId, device->getName().toStdString(), opened.sampleRate,
            opened.bufferSamples, opened.bitDepth, opened.physicalOutputs, capabilities.outputNames,
            opened.inputLatencySamples, opened.outputLatencySamples };
        refreshDriverIdentity();
    }
    ~JuceAsioDeviceOutput() override { closeForSwitch(); }
    const Snapshot& snapshot() const noexcept override { return state; }
    bool isOpen() const noexcept override
    { try { return device && device->isOpen(); } catch (...) { return false; } }
    bool attach(juce::AudioIODeviceCallback& callback) override
    {
        if (attached || !isOpen() || outputSuspended()) return false;
        try
        {
            const auto actual = readStreamState(*device);
            if (!matchesStream(*device, actual, {original.sampleRate, original.bufferSamples, original.physicalOutputs},
                    capabilities.inputNames, capabilities.outputNames, original.clockState)) return false;
            state.inputLatencySamples = actual.inputLatencySamples;
            state.outputLatencySamples = actual.outputLatencySamples;
            state.bitDepth = actual.bitDepth;
            device->start(&callback);
            if (device->isPlaying()) { attached = true; return true; }
        }
        catch (...) {}
        detach();
        return false;
    }
    void detach() noexcept override
    {
        try { if (device) device->stop(); } catch (...) {}
        attached = false;
    }
    bool closeForSwitch() noexcept override
    {
        detach();
        if (eventGeneration != 0)
        {
            const auto counters = driverEvents(false, true);
            totalResets = counters.resets; totalXruns = counters.xruns; lastTicks = counters.lastEventTicks;
        }
        eventGeneration = 0;
        try { device->close(); return !device->isOpen(); } catch (...) { return false; }
    }
    bool restore() noexcept override
    {
        if (attached || isOpen()) return false;
        try
        {
           #if JUCE_WINDOWS
            if (original.eventGeneration != 0)
            {
                if (!prepareClosedAsioDriver(device.get())) return false;
                const auto actual = readAsioDriverActualState(device.get());
                if (!actual || actual->inputNames != capabilities.inputNames || actual->outputNames != capabilities.outputNames) return false;
                eventGeneration = nextStreamGeneration();
                if (!armAsioDriverEvents(device.get(), eventGeneration)) { eventGeneration = 0; return false; }
            }
           #endif
            juce::BigInteger outputs;
            for (const auto channel : original.physicalOutputs) outputs.setBit(channel);
            const auto error = device->open({}, outputs, original.sampleRate, original.bufferSamples);
            const auto actual = readStreamState(*device);
            if (error.isEmpty() && device->isOpen()
                && matchesStream(*device, actual, {original.sampleRate, original.bufferSamples, original.physicalOutputs},
                    capabilities.inputNames, capabilities.outputNames, original.clockState) && validateManagedState(actual))
            {
                state.inputLatencySamples = actual.inputLatencySamples;
                state.outputLatencySamples = actual.outputLatencySamples;
                state.bitDepth = actual.bitDepth;
                refreshDriverIdentity();
                return true;
            }
        }
        catch (...) {}
        closeForSwitch();
        return false;
    }
    bool releaseForReplacement() noexcept override
    {
        if (attached || isOpen()) return false;
       #if JUCE_WINDOWS
        try { return !hasManagedEvents() || releaseClosedAsioDriver(device.get()); }
        catch (...) { return false; }
       #else
        return true;
       #endif
    }
    bool hasManagedEvents() const noexcept override { return original.eventGeneration != 0; }
    bool outputSuspended() const noexcept override
    {
       #if JUCE_WINDOWS
        if (hasManagedEvents()) return eventGeneration == 0 || readAsioDriverEvents(device.get(), eventGeneration, false, false).outputSuspended;
       #endif
        return false;
    }
    AsioDriverEvents::Snapshot driverEvents(bool consume, bool counters) noexcept override
    {
       #if JUCE_WINDOWS
        if (eventGeneration != 0)
        {
            auto result = readAsioDriverEvents(device.get(), eventGeneration, consume, counters);
            if (counters)
            {
                const auto add = [](std::uint64_t a, std::uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; };
                result.resets = add(result.resets, totalResets); result.xruns = add(result.xruns, totalXruns);
                result.lastEventTicks = std::max(result.lastEventTicks, lastTicks);
            }
            return result;
        }
       #else
        juce::ignoreUnused(consume, counters);
       #endif
        return {0, 0, counters ? totalResets : 0, counters ? totalXruns : 0, counters ? lastTicks : 0, hasManagedEvents()};
    }
    bool refreshDriverLatency() override
    {
        state.inputLatencySamples.reset(); state.outputLatencySamples.reset();
       #if JUCE_WINDOWS
        try
        {
            if (const auto values = readAsioDriverLatencies(device.get()))
            { state.inputLatencySamples = values->first; state.outputLatencySamples = values->second; return true; }
        }
        catch (...) {}
       #endif
        return false;
    }
    Maintenance maintain(bool panel, const std::atomic<bool>& cancelled, std::atomic<bool>& awaitingPanel) override
    {
        if (!hasManagedEvents() || attached || isOpen() || rollback) return Maintenance::unavailable;
        rollback = SavedConfiguration{original, state, capabilities};
       #if JUCE_WINDOWS
        if (!prepareClosedAsioDriver(device.get())) return Maintenance::unavailable;
        eventGeneration = nextStreamGeneration();
        if (!armAsioDriverEvents(device.get(), eventGeneration)) { eventGeneration = 0; return Maintenance::unavailable; }
        if (panel)
        {
            struct PanelWait { std::atomic<bool>& value; ~PanelWait() { value.store(false); } } guard{awaitingPanel};
            awaitingPanel.store(true);
            if (!showManagedAsioControlPanel(device.get(), eventGeneration)) return Maintenance::unavailable;
        }
        if (cancelled.load()) return Maintenance::cancelled;
        const auto actual = readAsioDriverActualState(device.get());
        if (!actual) return Maintenance::unavailable;
        if (actual->inputNames != capabilities.inputNames || actual->outputNames != capabilities.outputNames) return Maintenance::rebindRequired;
        original.sampleRate = actual->sampleRate; original.clockState = actual->clock;
        original.eventGeneration = eventGeneration;
        // Request the last confirmed buffer, then observe the actual new
        // stream. Panel changes may constrain it; preferred alone is not proof.
        if (!requestAsioDriverReset(device.get(), eventGeneration)) return Maintenance::unavailable;
        juce::BigInteger outputs; for (int channel : original.physicalOutputs) outputs.setBit(channel);
        readAsioDriverEvents(device.get(), eventGeneration, true, false);
        const auto error = device->open({}, outputs, original.sampleRate, original.bufferSamples);
        auto opened = readStreamState(*device);
        if (cancelled.load()) return Maintenance::cancelled;
        if (error.isNotEmpty() || !device->isOpen()
            || opened.bufferSamples < actual->minimumBuffer || opened.bufferSamples > actual->maximumBuffer
            || !matchesStream(*device, opened, {original.sampleRate, opened.bufferSamples, original.physicalOutputs}, capabilities.inputNames, capabilities.outputNames, original.clockState)
            || !validateManagedState(opened)) return Maintenance::unavailable;
        opened.eventGeneration = eventGeneration; original = std::move(opened);
        state.sampleRate = original.sampleRate; state.bufferSamples = original.bufferSamples; state.bitDepth = original.bitDepth;
        state.inputLatencySamples = original.inputLatencySamples; state.outputLatencySamples = original.outputLatencySamples;
        refreshDriverIdentity();
        return Maintenance::ready;
       #else
        juce::ignoreUnused(panel, cancelled, awaitingPanel); return Maintenance::unavailable;
       #endif
    }
    void acceptMaintenance() noexcept override { rollback.reset(); }
    void rollbackMaintenance() noexcept override
    {
        closeForSwitch();
        if (rollback) { original = std::move(rollback->stream); state = std::move(rollback->snapshot); capabilities = std::move(rollback->capabilities); rollback.reset(); }
    }
private:
    void refreshDriverIdentity()
    {
       #if JUCE_WINDOWS
        state.driverIdentity = copyAsioDriverIdentity(device.get());
       #endif
    }
    bool validateManagedState(const Adapter::StreamState& opened)
    {
       #if JUCE_WINDOWS
        if (eventGeneration == 0) return original.eventGeneration == 0;
        const auto events = readAsioDriverEvents(device.get(), eventGeneration, true, false);
        const auto actual = readAsioDriverActualState(device.get());
        if (!actual || actual->sampleRate != opened.sampleRate || actual->inputNames != capabilities.inputNames
            || actual->outputNames != capabilities.outputNames || (events.pending & (AsioDriverEvents::reset | AsioDriverEvents::resync)) != 0) return false;
        return acknowledgeAsioRevalidation(device.get(), eventGeneration);
       #else
        juce::ignoreUnused(opened); return true;
       #endif
    }
    struct SavedConfiguration { Adapter::StreamState stream; Snapshot snapshot; Adapter::Capabilities capabilities; };
    std::optional<SavedConfiguration> rollback;
    // The exact device must die before the JUCE type referenced by its callbacks.
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    Adapter::Capabilities capabilities;
    Adapter::StreamState original;
    Snapshot state;
    std::uint64_t eventGeneration = 0, totalResets = 0, totalXruns = 0, lastTicks = 0;
    bool attached = false;
};
}

JuceAsioBackendAdapter::JuceAsioBackendAdapter() = default;
JuceAsioBackendAdapter::JuceAsioBackendAdapter(Factory factory, RegistryReader registry, DeviceFactory device)
    : testFactory(std::move(factory)), testRegistry(std::move(registry)), testDevice(std::move(device)) {}
JuceAsioBackendAdapter::~JuceAsioBackendAdapter() = default;

bool JuceAsioBackendAdapter::isCompiled() noexcept
{
   #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
    return true;
   #else
    return false;
   #endif
}

const JuceAsioBackendAdapter::Catalog& JuceAsioBackendAdapter::scanDevices(Activation activation)
{
    ++current.generation;
    current.drivers.clear();
    registrations.clear();
    switch (activation)
    {
        case Activation::userSelection:
        case Activation::persistedSelection:
        case Activation::asioSettings: break;
        default:
            current.status = Status::invalidActivation;
            return current;
    }

    if (!testFactory && !isCompiled())
    {
        current.status = Status::notCompiled;
        return current;
    }
    if (!type)
    {
        if (testFactory) type = testFactory();
       #if JUCE_WINDOWS && JUCE_ASIO && JUCE_ASIO_USE_EXTERNAL_SDK
        else type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
       #endif
    }
    if (!type || type->getTypeName() != "ASIO")
    {
        type.reset();
        current.status = Status::unavailable;
        return current;
    }

    type->scanForDevices();
    const auto discovered = readRegistrations();
    for (const auto& name : type->getDeviceNames(false))
    {
        bool resolved = false;
        if (discovered)
            for (const auto& registration : *discovered)
                if (registration.displayName == name.toStdString())
                {
                    resolved = true;
                    const auto alreadyListed = std::any_of(registrations.begin(), registrations.end(),
                        [&](const auto& previous) { return previous.classId == registration.classId; });
                    if (alreadyListed) continue;
                    registrations.push_back(registration);
                    current.drivers.push_back({ registration.displayName, "asio:" + registration.classId });
                }
        if (!resolved) current.drivers.push_back({ name.toStdString(), {} });
    }
    current.status = current.drivers.empty() ? Status::noDrivers : Status::available;
    return current;
}

std::optional<std::vector<AsioDriverRegistration>> JuceAsioBackendAdapter::readRegistrations() const
{
    if (testRegistry) return testRegistry();
   #if JUCE_WINDOWS
    return readAsioDriverRegistrations();
   #else
    return {};
   #endif
}

JuceAsioBackendAdapter::PrepareStatus JuceAsioBackendAdapter::prepareCapabilities(
    std::uint64_t generation, const std::string& stableId, const DeviceSwitchAccess& access, std::atomic<bool>* awaitingPanel)
{
    if (!access.isValid()) return PrepareStatus::accessNotGranted;
    if (driverAccessInProgress) return PrepareStatus::candidateBusy;
    if (generation != current.generation || !type) return PrepareStatus::staleCatalog;
    const auto selected = std::find_if(registrations.begin(), registrations.end(),
        [&](const auto& registration) { return stableId == "asio:" + registration.classId; });
    if (selected == registrations.end()) return PrepareStatus::identityUnavailable;
    if (candidate) return PrepareStatus::candidateBusy;
    const auto selectedRegistration = *selected;
    const auto preparation = preparationGeneration;
    const juce::ScopedValueSetter<bool> accessingDriver(driverAccessInProgress, true);

    const auto stillRegistered = [&]()
    {
        const auto fresh = readRegistrations();
        return fresh && std::find(fresh->begin(), fresh->end(), selectedRegistration) != fresh->end();
    };
    if (!stillRegistered()) return PrepareStatus::registrationChanged;
    std::unique_ptr<juce::AudioIODevice> created;
    if (testDevice) created = testDevice(type.get(), selectedRegistration);
   #if JUCE_WINDOWS
    else created = createAsioDeviceByClassId(type.get(), selectedRegistration);
   #endif
    if (generation != current.generation) return PrepareStatus::staleCatalog;
    if (preparation != preparationGeneration || !access.isValid()) return PrepareStatus::cancelled;
    if (!created || created->getLastError().isNotEmpty()) return PrepareStatus::driverUnavailable;
    if (!stillRegistered()) return PrepareStatus::registrationChanged;

    if (awaitingPanel)
    {
        if (!created->hasControlPanel()) return PrepareStatus::driverUnavailable;
        const auto panelGeneration = nextStreamGeneration();
        bool managed = false, shown = false;
       #if JUCE_WINDOWS
        managed = armAsioDriverEvents(created.get(), panelGeneration);
       #endif
        {
            struct PanelWait { std::atomic<bool>& waiting; ~PanelWait() { waiting.store(false); } } guard{*awaitingPanel};
            awaitingPanel->store(true);
           #if JUCE_WINDOWS
            if (managed) shown = showManagedAsioControlPanel(created.get(), panelGeneration);
            else
           #endif
            if (testFactory || testDevice) shown = created->showControlPanel();
        }
        if (!shown) return PrepareStatus::driverUnavailable;
       #if JUCE_WINDOWS
        if (managed && !refreshAsioDriverCapabilities(created.get())) return PrepareStatus::unsupportedCapabilities;
       #else
        juce::ignoreUnused(panelGeneration, managed);
       #endif
        if (generation != current.generation || preparation != preparationGeneration || !access.isValid()) return PrepareStatus::cancelled;
        if (!stillRegistered()) return PrepareStatus::registrationChanged;
    }

    Capabilities snapshot;
    for (const auto& name : created->getInputChannelNames()) snapshot.inputNames.push_back(name.toStdString());
    for (const auto& name : created->getOutputChannelNames()) snapshot.outputNames.push_back(name.toStdString());
    for (const auto rate : created->getAvailableSampleRates()) snapshot.sampleRates.push_back(rate);
    for (const auto size : created->getAvailableBufferSizes()) snapshot.bufferSizes.push_back(size);
    if (snapshot.outputNames.empty() || snapshot.outputNames.size() > 512
        || snapshot.inputNames.size() > 512 || snapshot.sampleRates.empty() || snapshot.bufferSizes.empty()
        || std::any_of(snapshot.sampleRates.begin(), snapshot.sampleRates.end(), [](auto value) { return !std::isfinite(value) || value <= 0; })
        || std::any_of(snapshot.bufferSizes.begin(), snapshot.bufferSizes.end(), [](auto value) { return value <= 0; }))
        return PrepareStatus::unsupportedCapabilities;
    const auto rate = created->getCurrentSampleRate();
    const auto buffer = created->getCurrentBufferSizeSamples();
    const auto preferred = created->getDefaultBufferSize();
    if (std::isfinite(rate) && rate > 0) snapshot.reportedSampleRate = rate;
    if (buffer > 0) snapshot.reportedBufferSize = buffer;
    if (preferred > 0) snapshot.preferredBufferSize = preferred;
    snapshot.hasControlPanel = created->hasControlPanel();
   #if JUCE_WINDOWS
    snapshot.clockState = copyAsioClockState(created.get());
   #endif
    if (generation != current.generation) return PrepareStatus::staleCatalog;
    if (preparation != preparationGeneration || !access.isValid()) return PrepareStatus::cancelled;
    preparedState = PreparedCapabilities{ stableId, generation, std::move(snapshot) };
    preparedAccess = access;
    candidate = std::move(created);
    return PrepareStatus::ready;
}

void JuceAsioBackendAdapter::releasePrepared() noexcept
{
    const juce::ScopedValueSetter<bool> accessingDriver(driverAccessInProgress, true);
    auto retiring = std::move(candidate);
    ++preparationGeneration;
    preparedState.reset();
    openedState.reset();
    preparedAccess = {};
    if (retiring)
    {
        retiring->stop();
        retiring->close();
    }
}

JuceAsioBackendAdapter::OpenResult JuceAsioBackendAdapter::openPrepared(const StreamRequest& request)
{
    OpenResult result { OpenStatus::noCandidate, request, {} };
    if (driverAccessInProgress) { result.status = OpenStatus::candidateBusy; return result; }
    if (!candidate || !preparedState) return result;
    if (!preparedAccess.isValid()) { result.status = OpenStatus::cancelled; releasePrepared(); return result; }
    if (preparedState->catalogGeneration != current.generation)
    {
        result.status = OpenStatus::staleCatalog;
        releasePrepared();
        return result;
    }
    if (openedState) { result.status = OpenStatus::alreadyOpen; return result; }
    const auto& caps = preparedState->capabilities;
    const auto generation = current.generation;
    const auto preparation = preparationGeneration;
    if (!std::isfinite(request.sampleRate)
        || std::find(caps.sampleRates.begin(), caps.sampleRates.end(), request.sampleRate) == caps.sampleRates.end()
        || std::find(caps.bufferSizes.begin(), caps.bufferSizes.end(), request.bufferSamples) == caps.bufferSizes.end()
        || request.physicalOutputs.empty()
        || !std::is_sorted(request.physicalOutputs.begin(), request.physicalOutputs.end())
        || std::adjacent_find(request.physicalOutputs.begin(), request.physicalOutputs.end()) != request.physicalOutputs.end()
        || request.physicalOutputs.front() < 0
        || request.physicalOutputs.back() >= static_cast<int>(caps.outputNames.size()))
    {
        result.status = OpenStatus::unsupportedRequest;
        return result;
    }
    juce::BigInteger channels;
    for (const auto index : request.physicalOutputs) channels.setBit(index);
    // Keep this owner alive if a driver synchronously causes a new selection or
    // cancellation. It cannot publish into the newer catalog generation.
    const juce::ScopedValueSetter<bool> accessingDriver(driverAccessInProgress, true);
    auto opening = std::move(candidate);
    const auto expectedOutputs = caps.outputNames;
    const auto expectedInputs = caps.inputNames;
    const auto invalidated = [&]()
    {
        if (generation == current.generation && preparation == preparationGeneration && preparedAccess.isValid()) return false;
        result.status = generation != current.generation ? OpenStatus::staleCatalog : OpenStatus::cancelled;
        if (preparedState && preparedState->catalogGeneration == generation) releasePrepared();
        return true;
    };
    std::optional<AsioClockState> clockBefore;
   #if JUCE_WINDOWS
    clockBefore = copyAsioClockState(opening.get());
   #endif
    if (invalidated()) return result;
    std::uint64_t eventGeneration = 0;
   #if JUCE_WINDOWS
    const auto proposedGeneration = nextStreamGeneration();
    if (armAsioDriverEvents(opening.get(), proposedGeneration)) eventGeneration = proposedGeneration;
    else if (isCompiled() && !testFactory && !testDevice) { result.status = OpenStatus::driverFailed; releasePrepared(); return result; }
   #endif
    const auto error = opening->open({}, channels, request.sampleRate, request.bufferSamples);
    if (invalidated()) return result;
    if (error.isNotEmpty() || !opening->isOpen())
    {
        result.status = OpenStatus::driverFailed;
        releasePrepared();
        return result;
    }
    auto actual = readStreamState(*opening);
    actual.eventGeneration = eventGeneration;
    result.actual = actual;
    auto matched = matchesStream(*opening, actual, request, expectedInputs, expectedOutputs, clockBefore);
   #if JUCE_WINDOWS
    if (eventGeneration != 0)
    {
        const auto pending = readAsioDriverEvents(opening.get(), eventGeneration, true, false);
        const auto fresh = readAsioDriverActualState(opening.get());
        matched = matched && fresh && fresh->sampleRate == actual.sampleRate && fresh->inputNames == expectedInputs && fresh->outputNames == expectedOutputs
            && (pending.pending & (AsioDriverEvents::reset | AsioDriverEvents::resync)) == 0 && acknowledgeAsioRevalidation(opening.get(), eventGeneration);
    }
   #endif
    if (invalidated()) return result;
    if (!matched)
    {
        result.status = OpenStatus::configurationChanged;
        releasePrepared();
        return result;
    }
    openedState = std::move(actual);
    candidate = std::move(opening);
    result.status = OpenStatus::ready;
    return result;
}

std::unique_ptr<DeviceOutput> JuceAsioBackendAdapter::takeOpenedOutput()
{
    if (driverAccessInProgress || !candidate || !preparedState || !openedState || !preparedAccess.isValid()
        || preparedState->catalogGeneration != current.generation) return {};
    auto result = std::make_unique<JuceAsioDeviceOutput>(std::move(type), std::move(candidate), *preparedState, *openedState);
    preparedState.reset(); openedState.reset(); registrations.clear(); current.drivers.clear();
    preparedAccess = {};
    current.status = Status::inactive;
    ++current.generation; ++preparationGeneration;
    return result;
}

JuceAsioBackendAdapter::CandidateResult JuceAsioBackendAdapter::prepareSelectedOutput(Activation activation,
    const std::string& stableId, const StreamRequest& request, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled)
{
    CandidateResult result;
    if (cancelled.load() || !access.isValid()) return result;
    JuceAsioBackendAdapter adapter;
    result.discovery = adapter.scanDevices(activation).status;
    if (result.discovery != Status::available || cancelled.load()) return result;
    result.preparation = adapter.prepareCapabilities(adapter.catalog().generation, stableId, access);
    if (result.preparation != PrepareStatus::ready || cancelled.load()) return result;
    result.opening = adapter.openPrepared(request).status;
    if (result.opening != OpenStatus::ready || cancelled.load() || !access.isValid()) return result;
    result.output = adapter.takeOpenedOutput();
    return result;
}

void JuceAsioBackendAdapter::release() noexcept
{
    releasePrepared();
    type.reset();
    registrations.clear();
    ++current.generation;
    current.drivers.clear();
    current.status = Status::inactive;
}

JuceAsioBackendAdapter::InspectionResult JuceAsioBackendAdapter::inspectCapabilities(std::uint64_t generation,
    const std::string& stableId, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled, std::atomic<bool>* awaitingPanel)
{
    InspectionResult result;
    result.discovery = current.status;
    if (cancelled.load()) { result.preparation = PrepareStatus::cancelled; return result; }
    result.preparation = prepareCapabilities(generation, stableId, access, awaitingPanel);
    if (result.preparation != PrepareStatus::ready) return result;
    try { result.capabilities = preparedState; }
    catch (...) { releasePrepared(); throw; }
    releasePrepared(); // must finish before the old output may reopen
    if (cancelled.load() || !access.isValid())
    {
        result.preparation = PrepareStatus::cancelled;
        result.capabilities.reset();
    }
    return result;
}

JuceAsioBackendAdapter::InspectionResult JuceAsioBackendAdapter::inspectSelectedCapabilities(Activation activation,
    const std::string& stableId, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled, std::atomic<bool>* awaitingPanel)
{
    InspectionResult result;
    if (cancelled.load()) { result.preparation = PrepareStatus::cancelled; return result; }
    if (!access.isValid()) return result;
    JuceAsioBackendAdapter adapter;
    result.discovery = adapter.scanDevices(activation).status;
    if (result.discovery != Status::available) return result;
    return adapter.inspectCapabilities(adapter.catalog().generation, stableId, access, cancelled, awaitingPanel);
}
}
