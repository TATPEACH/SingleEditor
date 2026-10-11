#include "AsioDriverHook.h"
#include "Audio/NativeAsioDevice.h"
#include "Audio/JuceAsioBackendAdapter.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <iasiodrv.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>

namespace singleeditor
{
struct AsioDriverEventsTestAccess
{
    static void nearSaturation(AsioDriverEvents& events)
    { events.resetCount.store(std::numeric_limits<std::uint64_t>::max() - 1); events.xrunCount.store(std::numeric_limits<std::uint64_t>::max() - 1); }
};
struct JuceAsioBackendTestAccess
{
    static DeviceSwitchAccess access()
    {
        auto state = std::make_shared<DeviceSwitchAccess::State>(); state->windowOpen.store(true);
        return DeviceSwitchAccess(std::move(state));
    }
    static std::unique_ptr<JuceAsioBackendAdapter> native(const AsioDriverRegistration& registration)
    {
        auto result = std::unique_ptr<JuceAsioBackendAdapter>(new JuceAsioBackendAdapter({}, [registration] { return std::optional{std::vector{registration}}; }));
        result->type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
        result->registrations = { registration };
        result->current = { JuceAsioBackendAdapter::Status::available, 1, {{registration.displayName, "asio:" + registration.classId}} };
        return result;
    }
    static std::unique_ptr<JuceAsioBackendAdapter> make(JuceAsioBackendAdapter::Factory factory,
        JuceAsioBackendAdapter::RegistryReader registry, JuceAsioBackendAdapter::DeviceFactory device)
    {
        return std::unique_ptr<JuceAsioBackendAdapter>(new JuceAsioBackendAdapter(std::move(factory), std::move(registry), std::move(device)));
    }
};
}

namespace
{
const CLSID fixtureId { 0x9373d307, 0x0fca, 0x4b34, {0x85, 0xd9, 0x69, 0x16, 0x78, 0xd9, 0x6a, 0x19} };
constexpr auto fixtureText = "{9373D307-0FCA-4B34-85D9-691678D96A19}";
struct DriverState
{
    int instances = 0, creates = 0, unexpectedCreates = 0, starts = 0, stops = 0, clockWrites = 0, rateWrites = 0;
    int currentClock = 7; // External is deliberately not clocks[0].
    double rate = 48000.0;
    bool markCurrentClock = true;
    bool changeClockDuringStreamStart = false;
    int outputLatency = 64, preferredBuffer = 128, panelCalls = 0;
    bool changedOutputNames = false;
    bool singleInstance = false;
    int rejectedConcurrentCreates = 0;
    int nameReads = 0, versionReads = 0;
    std::string product = "IASIO reported product";
    long version = 1;
    bool versionUnavailable = false, initFails = false;
    ASIOCallbacks notifications {}, retiredNotifications {};
    std::function<void()> duringPanel;
    std::function<void()> pump;
    std::function<float(int)> outputSample;
};
DriverState state;

void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

// Implements the real IASIO SDK interface. JUCE's actual constructor, open,
// buffer negotiation and callbacks execute through the product overlay; no physical driver or
// endpoint is opened. Clock assertions observe actual SDK method calls.
class Driver final : public IASIO
{
public:
    Driver() { ++state.instances; }
    ~Driver() { --state.instances; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        if (iid != IID_IUnknown && iid != fixtureId) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IASIO*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    { const auto left = --references; if (left == 0) delete this; return left; }
    ASIOBool init(void*) override { return state.initFails ? ASIOFalse : ASIOTrue; }
    void getDriverName(char* name) override { ++state.nameReads; strcpy_s(name, 32, state.product.c_str()); }
    long getDriverVersion() override
    { ++state.versionReads; if (state.versionUnavailable) throw std::runtime_error("optional version unavailable"); return state.version; }
    void getErrorMessage(char* text) override { strcpy_s(text, 124, "fixture error"); }
    ASIOError start() override
    {
        ++state.starts;
        if (state.changeClockDuringStreamStart && state.starts > 1) state.currentClock = 3;
        if (!callbacks.bufferSwitch) return ASE_InvalidMode;
        callbacks.bufferSwitch(0, ASIOTrue);
        return ASE_OK;
    }
    ASIOError stop() override { ++state.stops; return ASE_OK; }
    ASIOError getChannels(long* inputs, long* outputs) override { *inputs = 0; *outputs = 2; return ASE_OK; }
    ASIOError getLatencies(long* input, long* output) override { *input = 0; *output = state.outputLatency; return ASE_OK; }
    ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override
    { *minimum = 64; *maximum = 256; *preferred = state.preferredBuffer; *granularity = -1; return ASE_OK; }
    ASIOError canSampleRate(ASIOSampleRate rate) override
    { return rate == 44100 || rate == 48000 || rate == 96000 || rate == 384000 ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* rate) override { *rate = state.rate; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate rate) override
    {
        ++state.rateWrites;
        if (state.currentClock == 7 && rate != state.rate) return ASE_NoClock;
        state.rate = rate; return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override
    {
        if (*count < 2) return ASE_InvalidParameter;
        clocks[0] = {}; clocks[0].index = 3;
        clocks[0].associatedChannel = clocks[0].associatedGroup = -1;
        clocks[0].isCurrentSource = state.markCurrentClock && state.currentClock == 3;
        strcpy_s(clocks[0].name, "Internal");
        clocks[1] = {}; clocks[1].index = 7;
        clocks[1].associatedChannel = clocks[1].associatedGroup = -1;
        clocks[1].isCurrentSource = state.markCurrentClock && state.currentClock == 7;
        strcpy_s(clocks[1].name, "External");
        *count = 2; return ASE_OK;
    }
    ASIOError setClockSource(long index) override { ++state.clockWrites; state.currentClock = static_cast<int>(index); return ASE_OK; }
    ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* timestamp) override
    { *samples = {}; *timestamp = {}; return ASE_OK; }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override
    {
        if (info->isInput || info->channel < 0 || info->channel > 1) return ASE_InvalidParameter;
        info->isActive = ASIOTrue; info->channelGroup = 0; info->type = ASIOSTFloat32LSB;
        strcpy_s(info->name, state.changedOutputNames ? "Changed output" : info->channel == 0 ? "Output 1" : "Output 2");
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long samples, ASIOCallbacks* provided) override
    {
        callbacks = *provided;
        state.notifications = callbacks;
        buffers.resize(static_cast<size_t>(count));
        for (long channel = 0; channel < count; ++channel)
            for (int side = 0; side < 2; ++side)
            {
                auto& buffer = buffers[static_cast<size_t>(channel)][static_cast<size_t>(side)];
                buffer.assign(static_cast<size_t>(samples), 0.0f);
                infos[channel].buffers[side] = buffer.data();
            }
        state.pump = [this] { callbacks.bufferSwitch(0, ASIOTrue); };
        state.outputSample = [this](int channel) { return buffers.at(static_cast<size_t>(channel))[0].back(); };
        return ASE_OK;
    }
    ASIOError disposeBuffers() override { state.retiredNotifications = callbacks; state.notifications = {}; state.pump = {}; state.outputSample = {}; callbacks = {}; buffers.clear(); return ASE_OK; }
    ASIOError controlPanel() override { ++state.panelCalls; if (state.duringPanel) state.duringPanel(); return ASE_OK; }
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_NotPresent; }
private:
    ULONG references = 1;
    ASIOCallbacks callbacks {};
    std::vector<std::array<std::vector<float>, 2>> buffers;
};

void managedEventMailboxCase()
{
    using Events = singleeditor::AsioDriverEvents;
    Events events;
    require(events.generation() == 0 && !events.outputSuspended() && !events.publish(1, Events::reset, 1), "unarmed event mailbox stays inert");
    require(events.arm(1), "control owner arms a fresh stream generation");
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < 3; ++producer)
        producers.emplace_back([&] { for (int i = 0; i < 4000; ++i) events.publish(1, Events::overload, static_cast<std::uint64_t>(i + 1)); });
    producers.clear();
    auto snapshot = events.read(1, true);
    require(snapshot.pending == Events::overload && snapshot.xruns == 12000 && snapshot.lastEventTicks == 4000 && !snapshot.outputSuspended,
        "concurrent overloads coalesce one flag, preserve every count and monotonic latest time without stopping playback");
    events.publish(1, Events::reset, 4100);
    snapshot = events.read(1, true);
    require(snapshot.pending == Events::reset && snapshot.resets == 1 && snapshot.outputSuspended && events.outputSuspended(),
        "consuming reset notification never unmutes the stream");
    events.publish(1, Events::sampleRate, 4200);
    require(!events.acknowledgeRevalidation(1) && events.outputSuspended(), "an event arriving during validation rejects an obsolete acknowledgement");
    events.read(1, true);
    require(events.acknowledgeRevalidation(1) && !events.outputSuspended(), "fresh validation can release a drained event fence");
    singleeditor::AsioDriverEventsTestAccess::nearSaturation(events);
    for (int i = 0; i < 4; ++i) { events.publish(1, Events::reset, 4300); events.publish(1, Events::overload, 4300); }
    snapshot = events.read(1, true);
    require(snapshot.resets == std::numeric_limits<std::uint64_t>::max() && snapshot.xruns == std::numeric_limits<std::uint64_t>::max(), "reset/xrun counters saturate without wrapping");
    std::atomic<bool> release{false};
    std::jthread retiringProducer([&] { while (!release.load()) events.publish(1, Events::reset, 4500); });
    events.retire();
    require(events.arm(2), "control owner starts a new generation after retiring admitted publishers");
    release.store(true); retiringProducer.join();
    require(events.read(1, true).generation == 0 && !events.publish(1, Events::reset, 9999) && !events.acknowledgeRevalidation(1)
        && events.read(2, false).pending == 0 && events.read(2, false).resets == 0 && !events.outputSuspended(), "retired writer and reader tokens cannot affect a new generation");
    std::cout << "ASIO event mailbox passed: concurrent coalescing, monotonic/saturating counts, sticky output fence, validation race and retired generations\n";
}

void managedNativeEventsCase()
{
    using Events = singleeditor::AsioDriverEvents;
    require(state.instances == 0, "prior SDK owner drained before managed-event case"); state = {};
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    auto device = singleeditor::createAsioDeviceByClassId(type.get(), {"Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only"});
    require(device && device->getLastError().isEmpty() && singleeditor::readAsioDriverEvents(device.get(), 101, false).generation == 0,
        "real JUCE constructor does not arm managed event infrastructure");
    require(singleeditor::armAsioDriverEvents(device.get(), 101), "arm exact stopped native owner before opening its stream");
    juce::BigInteger outputs; outputs.setRange(0, 2, true);
    require(device->open({}, outputs, 48000, 128).isEmpty(), "open exact managed SDK stream");
    struct Callback final : juce::AudioIODeviceCallback
    {
        int blocks = 0;
        void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* channels, int count, int frames, const juce::AudioIODeviceCallbackContext&) override
        { ++blocks; for (int i = 0; i < count; ++i) std::fill_n(channels[i], frames, .25f); }
    } callback;
    device->start(&callback); state.pump();
    require(callback.blocks == 1 && state.outputSample(0) == .25f, "managed stream still reaches real SDK output buffers");
    const auto starts = state.starts, stops = state.stops, creates = state.creates;
    state.notifications.asioMessage(kAsioOverload, 0, nullptr, nullptr);
    state.notifications.asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
    state.outputLatency = 192;
    auto event = singleeditor::readAsioDriverEvents(device.get(), 101, true);
    const auto latency = singleeditor::readAsioDriverActualState(device.get());
    require(event.pending == (Events::overload | Events::latency) && event.xruns == 1 && !event.outputSuspended
        && latency && latency->outputLatency == 192 && device->getOutputLatencyInSamples() == 64,
        "native read returns fresh SDK latency rather than JUCE's old cached value without resetting playback");
    state.pump(); require(callback.blocks == 2, "latency-only and xrun notifications do not suspend audio");
    state.notifications.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    state.notifications.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    state.notifications.asioMessage(kAsioBufferSizeChange, 0, nullptr, nullptr);
    state.notifications.asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
    state.notifications.sampleRateDidChange(96000);
    event = singleeditor::readAsioDriverEvents(device.get(), 101, true);
    require(event.pending == (Events::reset | Events::bufferSize | Events::resync | Events::sampleRate) && event.resets == 2 && event.outputSuspended,
        "actual SDK notifications merge exact flags and reset count");
    state.pump(); require(callback.blocks == 2 && state.outputSample(0) == 0 && state.outputSample(1) == 0,
        "critical native event silences physical buffers and stops advancing the renderer even after notifications were read");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
    require(state.starts == starts && state.stops == stops && state.creates == creates,
        "managed SDK events never trigger JUCE's delayed automatic stream reopen");
    state.notifications.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    require(!singleeditor::acknowledgeAsioRevalidation(device.get(), 101), "new SDK event prevents stale validation from releasing audio");
    singleeditor::readAsioDriverEvents(device.get(), 101, true);
    require(singleeditor::acknowledgeAsioRevalidation(device.get(), 101), "owner explicitly acknowledges verified actual state");
    state.pump(); require(callback.blocks == 3 && state.outputSample(0) == .25f, "validated native stream resumes actual samples");
    require(!singleeditor::showManagedAsioControlPanel(device.get(), 100) && state.panelCalls == 0,
        "stale generation cannot invoke the driver's control panel");
    state.duringPanel = [&] { state.pump(); require(state.outputSample(0) == 0 && callback.blocks == 3, "panel suspends physical output before driver mutation"); state.rate = 96000; state.preferredBuffer = 256; state.changedOutputNames = true; };
    require(singleeditor::showManagedAsioControlPanel(device.get(), 101) && state.panelCalls == 1, "explicit managed panel calls actual SDK once");
    const auto actual = singleeditor::readAsioDriverActualState(device.get());
    event = singleeditor::readAsioDriverEvents(device.get(), 101, true);
    require(actual && actual->sampleRate == 96000 && actual->preferredBuffer == 256
        && actual->outputNames == std::vector<std::string>{"Changed output (1)", "Changed output (2)"}
        && device->getCurrentSampleRate() == 48000 && device->getCurrentBufferSizeSamples() == 128
        && event.pending == Events::controlPanel && event.outputSuspended,
        "panel requery reports actual rate/preferred buffer/channel names separately from the obsolete stream cache");
    const auto late = state.notifications; device->close();
    late.asioMessage(kAsioResetRequest, 0, nullptr, nullptr); late.sampleRateDidChange(48000);
    require(singleeditor::readAsioDriverEvents(device.get(), 101, true).generation == 0, "native close retires event generation before driver disposal");
    const auto closedStarts = state.starts;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
    require(state.starts == closedStarts && !device->isOpen(), "late notifications after close cannot schedule a legacy restart");
    device.reset(); type.reset();
    require(late.asioMessage(kAsioResetRequest, 0, nullptr, nullptr) == 0 && state.instances == 0,
        "cleared native slot ignores callbacks after owner destruction");
    std::cout << "Managed native ASIO events passed: SDK flags, silence fence, no timer reopen, actual-state requery, explicit panel, close retirement\n";
}

void driverIdentityCase()
{
    using namespace singleeditor;
    require(state.instances == 0, "identity starts without a retained SDK owner"); state = {};
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    const AsioDriverRegistration registration { "Registry display alias", fixtureText, "process-local", "fixture-only" };
    auto device = createAsioDeviceByClassId(type.get(), registration);
    const auto initial = copyAsioDriverIdentity(device.get());
    require(device && device->getLastError().isEmpty() && initial && initial->product == state.product
        && initial->product != registration.displayName && initial->rawAsioVersion == 1 && !initial->versionReadFailed,
        "identity comes from the initialized IASIO interface, independently of display alias");
    const auto names = state.nameReads, versions = state.versionReads, creates = state.creates;
    state.product = "New initialized product"; state.version = 0;
    for (int i = 0; i < 10; ++i)
        require(copyAsioDriverIdentity(device.get())->product == initial->product, "passive copies preserve the initialized instance facts");
    require(state.nameReads == names && state.versionReads == versions && state.creates == creates,
        "passive observation performs no SDK identity calls or instantiation");
    require(releaseClosedAsioDriver(device.get()) && state.instances == 0 && !copyAsioDriverIdentity(device.get()),
        "released SDK instance clears the passive cache");
    require(prepareClosedAsioDriver(device.get()), "explicit restoration initializes the replacement instance");
    const auto replacement = copyAsioDriverIdentity(device.get());
    require(replacement && replacement->product == state.product && replacement->rawAsioVersion == 0
        && state.nameReads == names + 1 && state.versionReads == versions + 1,
        "new instance refreshes identity and preserves a valid raw zero version");
    require(releaseClosedAsioDriver(device.get()), "release before optional version read failure");
    state.versionUnavailable = true;
    require(prepareClosedAsioDriver(device.get()), "unavailable optional identity does not prevent driver preparation");
    const auto partial = copyAsioDriverIdentity(device.get());
    require(partial && partial->product == state.product && !partial->rawAsioVersion && partial->versionReadFailed,
        "failed version getter preserves product and explicit unavailability");
    juce::BigInteger outputs; outputs.setRange(0, 2, true);
    require(device->open({}, outputs, 48000, 128).isEmpty() && device->isOpen(),
        "optional version failure leaves ordinary exact stream open usable");
    device->close();
    require(releaseClosedAsioDriver(device.get()), "release before failed initialization");
    state.initFails = true;
    require(!prepareClosedAsioDriver(device.get()) && !copyAsioDriverIdentity(device.get()),
        "failed initialization cannot expose a previous driver's identity");
    device.reset(); type.reset();
    require(state.instances == 0 && state.unexpectedCreates == 0, "identity case releases its exact fixture instance");
    std::cout << "ASIO identity cache passed: actual name/raw version, passive copies, recreation, optional absence and release\n";
}

bool clockCase(bool currentClockIsReported, double requestedRate)
{
    require(state.instances == 0, "previous fixture driver released");
    state = {};
    state.markCurrentClock = currentClockIsReported;
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    auto device = singleeditor::createAsioDeviceByClassId(type.get(),
        { "Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only" });
    require(device && device->getLastError().isEmpty() && state.creates == 1 && state.unexpectedCreates == 0,
            "real JUCE ASIO constructor used the process-local SDK driver");
    require(state.starts == 1 && state.stops >= 1, "characterize JUCE constructor's dummy start/stop");
    juce::BigInteger outputs; outputs.setRange(0, 2, true);
    const auto error = device->open({}, outputs, requestedRate, 128);
    const auto actualRate = device->getCurrentSampleRate();
    const auto clockState = singleeditor::copyAsioClockState(device.get());
    require(clockState && clockState->sources.size() == 2
                && clockState->sources[0].index == 3 && clockState->sources[1].index == 7,
            "typed clock state copies actual driver-reported indices");
    require(currentClockIsReported ? clockState->currentIndex == state.currentClock : !clockState->currentIndex,
            "clock identity is known only when the driver identifies its current source");
    const auto writes = state.clockWrites;
    const auto clock = state.currentClock;
    std::cout << "Clock case marked=" << currentClockIsReported << " requested=" << requestedRate
              << " actual=" << actualRate << " clock=" << clock << " clockWrites=" << writes
              << " rateWrites=" << state.rateWrites << " openError=" << error.isNotEmpty() << '\n';
    device.reset(); type.reset();
    require(state.instances == 0, "real JUCE destructor releases fixture IASIO owner");
    return writes == 0 && clock == 7 && actualRate == 48000.0;
}

bool constructorRateCase(double initialRate, bool expectValid)
{
    require(state.instances == 0, "previous constructor case released");
    state = {}; state.currentClock = 3; state.rate = initialRate;
    std::unique_ptr<juce::AudioIODeviceType> type(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    auto device = singleeditor::createAsioDeviceByClassId(type.get(),
        { "Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only" });
    require(device != nullptr && state.unexpectedCreates == 0, "construct exact fixture driver");
    const auto failed = device->getLastError().isNotEmpty();
    const auto preserved = state.rate == initialRate && state.rateWrites == 0 && state.clockWrites == 0;
    std::cout << "Constructor initialRate=" << initialRate << " actualRate=" << state.rate
              << " rateWrites=" << state.rateWrites << " clockWrites=" << state.clockWrites
              << " error=" << failed << '\n';
    device.reset(); type.reset();
    require(state.instances == 0, "constructor case releases SDK owner");
    return preserved && failed != expectValid;
}

class FixtureCatalog final : public juce::AudioIODeviceType
{
public:
    FixtureCatalog() : AudioIODeviceType("ASIO") {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return { "Single Editor ASIO fixture" }; }
    int getDefaultDeviceIndex(bool) const override { return -1; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    { throw std::runtime_error("adapter must use exact CLSID creation"); }
};

void adapterClockCases()
{
    using Adapter = singleeditor::JuceAsioBackendAdapter;
    const singleeditor::AsioDriverRegistration registration { "Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only" };
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        require(state.instances == 0, "previous adapter case released");
        state = {};
        std::unique_ptr<juce::AudioIODeviceType> nativeType(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
        auto adapter = singleeditor::JuceAsioBackendTestAccess::make(
            [] { return std::make_unique<FixtureCatalog>(); },
            [&] { return std::optional{std::vector{registration}}; },
            [&](juce::AudioIODeviceType*, const singleeditor::AsioDriverRegistration& selected)
            { return singleeditor::createAsioDeviceByClassId(nativeType.get(), selected); });
        const auto& catalog = adapter->scanDevices(Adapter::Activation::userSelection);
        require(adapter->prepareCapabilities(catalog.generation, catalog.drivers[0].stableId,
                    singleeditor::JuceAsioBackendTestAccess::access()) == Adapter::PrepareStatus::ready,
                "real JUCE ASIO constructor prepares typed adapter capabilities");
        require(adapter->prepared()->capabilities.clockState
                    && adapter->prepared()->capabilities.clockState->currentIndex == 7,
                "prepared adapter reports actual external clock index");
        state.changeClockDuringStreamStart = scenario == 2;
        const auto opened = adapter->openPrepared({ scenario == 0 ? 96000.0 : 48000.0, 128, {0, 1} });
        const auto expected = scenario == 1 ? Adapter::OpenStatus::ready : Adapter::OpenStatus::configurationChanged;
        require(opened.status == expected && opened.actual && opened.actual->sampleRate == 48000.0
                    && opened.actual->clockState && opened.actual->clockState->currentIndex == (scenario == 2 ? 3 : 7),
                "adapter rejects a wrong actual rate or changed clock and reports observed facts");
        require(state.clockWrites == 0 && state.unexpectedCreates == 0,
                "adapter does not issue clock changes or open any real driver");
        require(scenario == 1 ? adapter->opened().has_value() : !adapter->opened(),
                "only exact unchanged-clock candidate becomes ready");
        adapter->release();
        require(state.instances == 0, "adapter release drains the real JUCE/SDK owner");
        std::cout << "Adapter clock scenario=" << scenario << " passed\n";
    }
}

void transferredOutputCase()
{
    require(state.instances == 0, "previous driver owner drained"); state = {};
    const singleeditor::AsioDriverRegistration registration { "Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only" };
    auto adapter = singleeditor::JuceAsioBackendTestAccess::native(registration);
    require(adapter->prepareCapabilities(1, "asio:" + registration.classId, singleeditor::JuceAsioBackendTestAccess::access())
        == singleeditor::JuceAsioBackendAdapter::PrepareStatus::ready, "actual native type prepares an exact fixture driver");
    require(!adapter->takeOpenedOutput(), "unopened candidate cannot be transferred");
    require(adapter->openPrepared({48000, 128, {0, 1}}).status == singleeditor::JuceAsioBackendAdapter::OpenStatus::ready,
        "transfer starts from a fully validated silent stream");
    auto output = adapter->takeOpenedOutput();
    require(output && !adapter->opened() && !adapter->takeOpenedOutput(), "output transfers once with its owning JUCE type");
    require(output->snapshot().driverIdentity && output->snapshot().driverIdentity->product == state.product
        && output->snapshot().driverIdentity->rawAsioVersion == state.version,
        "production adapter transfers the initialized SDK identity with its exact output");
    adapter.reset();
    struct Callback final : juce::AudioIODeviceCallback
    {
        int starts = 0, stops = 0, blocks = 0;
        void audioDeviceAboutToStart(juce::AudioIODevice*) override { ++starts; }
        void audioDeviceStopped() override { ++stops; }
        void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int channels, int frames,
            const juce::AudioIODeviceCallbackContext&) override
        {
            ++blocks;
            for (int c = 0; c < channels; ++c) if (outputs[c]) std::fill_n(outputs[c], frames, 0.25f * static_cast<float>(c + 1));
        }
    } callback;
    state.currentClock = 3;
    require(!output->attach(callback) && callback.starts == 0,
        "clock change between prepare and commit refuses the renderer before callback attachment");
    state.currentClock = 7;
    require(state.instances == 1 && output->isOpen() && output->snapshot().stableId == "asio:" + registration.classId
        && output->attach(callback) && callback.starts == 1, "destroyed discovery adapter does not destroy the transferred output");
    state.pump();
    require(callback.blocks == 1 && state.outputSample(0) == 0.25f && state.outputSample(1) == 0.5f,
        "real JUCE ASIO callback conversion reaches the actual SDK output buffers");
    output->detach(); state.pump();
    require(callback.stops == 1 && callback.blocks == 1, "detached output no longer calls its previous renderer");
    require(output->closeForSwitch() && output->restore() && output->attach(callback),
        "transferred owner reopens the exact rate/buffer/mask/clock and accepts its callback again");
    state.pump(); require(callback.blocks == 2, "restored native stream actually delivers another callback");
    require(output->hasManagedEvents(), "production transfer arms the managed native bridge before stream open");
    state.notifications.asioMessage(kAsioOverload, 0, nullptr, nullptr);
    state.outputLatency = 192;
    state.notifications.asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
    require(!output->outputSuspended() && output->refreshDriverLatency() && output->snapshot().outputLatencySamples == 192,
        "latency-only notification updates actual typed latency without suspending or reopening");
    state.notifications.sampleRateDidChange(96000);
    state.rate = 96000;
    require(output->outputSuspended(), "native critical notification gates the transferred owner immediately");
    state.pump(); require(callback.blocks == 2 && state.outputSample(0) == 0, "suspended transfer no longer renders source samples");
    require(output->closeForSwitch(), "close the exact output before maintenance");
    state.product = "Reinitialized driver"; state.version = 42;
    std::atomic<bool> cancelled {false}, awaitingPanel {false};
    require(output->maintain(false, cancelled, awaitingPanel) == singleeditor::DeviceOutput::Maintenance::ready
        && output->snapshot().sampleRate == 96000 && output->snapshot().bufferSamples == 128 && state.instances == 1,
        "same native output revalidates actual external rate without a duplicate live SDK owner");
    require(output->snapshot().driverIdentity && output->snapshot().driverIdentity->product == state.product
        && output->snapshot().driverIdentity->rawAsioVersion == 42,
        "maintenance refreshes identity from the reinitialized SDK instance");
    require(output->attach(callback), "revalidated output accepts renderer"); output->acceptMaintenance();
    state.pump(); require(callback.blocks == 3 && state.outputSample(0) == .25f, "actual SDK samples resume after revalidation");
    require(output->driverEvents(false, true).xruns == 1, "xrun counters survive stream retirement and reopen");
    require(output->closeForSwitch(), "panel receives a closed stream");
    state.duringPanel = [&]
    {
        require(awaitingPanel.load(), "panel dwell is explicitly reported to the application");
        state.rate = 48000; state.preferredBuffer = 256; state.outputLatency = 224;
    };
    require(output->maintain(true, cancelled, awaitingPanel) == singleeditor::DeviceOutput::Maintenance::ready
        && !awaitingPanel.load() && state.panelCalls == 1 && output->snapshot().sampleRate == 48000
        && output->snapshot().bufferSamples == 256 && output->snapshot().outputLatencySamples == 224,
        "panel close reads actual rate, latency and reopened stream buffer; it does not report preferred as an already-open buffer");
    require(output->attach(callback), "panel-validated output attaches"); output->acceptMaintenance();
    require(output->closeForSwitch(), "close before topology-changing panel");
    state.duringPanel = [&] { state.changedOutputNames = true; };
    require(output->maintain(true, cancelled, awaitingPanel) == singleeditor::DeviceOutput::Maintenance::rebindRequired,
        "changed actual physical names require rebind before any renderer can resume");
    output->rollbackMaintenance();
    require(!output->restore() && !output->isOpen(), "rollback cannot reopen a route against changed actual ports");
    state.changedOutputNames = false; state.duringPanel = {};
    require(output->restore(), "same exact old ports allow explicit restore after fixture reset");
    require(output->closeForSwitch(), "stop restored stream for a changed clock");
    state.currentClock = 3;
    require(!output->restore() && !output->isOpen() && state.clockWrites == 0,
        "old output cannot claim restoration after driver clock identity changed");
    output.reset();
    require(state.instances == 0 && !state.pump && state.unexpectedCreates == 0,
        "transferred driver and callback type drain in dependency order");
    std::cout << "Transferred ASIO output passed: actual SDK buffers, exact owner lifetime and restore\n";
}

void singleInstanceReplacementAndPanelCase()
{
    using Adapter = singleeditor::JuceAsioBackendAdapter;
    require(state.instances == 0, "previous SDK owners released"); state = {}; state.singleInstance = true;
    const singleeditor::AsioDriverRegistration registration {"Single Editor ASIO fixture", fixtureText, "process-local", "fixture-only"};
    const auto access = singleeditor::JuceAsioBackendTestAccess::access();
    const auto id = "asio:" + registration.classId;
    auto adapter = singleeditor::JuceAsioBackendTestAccess::native(registration);
    require(adapter->prepareCapabilities(1, id, access) == Adapter::PrepareStatus::ready
        && adapter->openPrepared({48000, 128, {0, 1}}).status == Adapter::OpenStatus::ready, "prepare single-instance driver");
    auto oldOutput = adapter->takeOpenedOutput(); adapter.reset();
    require(!oldOutput->releaseForReplacement(), "live output cannot release its driver through a replacement shortcut");
    require(oldOutput->closeForSwitch() && state.instances == 1, "JUCE stream close retains the SDK object");
    auto collision = singleeditor::JuceAsioBackendTestAccess::native(registration);
    require(collision->prepareCapabilities(1, id, access) == Adapter::PrepareStatus::driverUnavailable
        && state.rejectedConcurrentCreates == 1, "single-instance fixture reproduces a second-owner failure after stream-only close");
    collision.reset();
    require(oldOutput->releaseForReplacement() && state.instances == 0, "replacement releases the closed SDK owner");
    state.product = "Restored driver product"; state.version = -101;
    std::atomic<bool> cancelled {false}, awaitingPanel {false};
    auto inspection = singleeditor::JuceAsioBackendTestAccess::native(registration);
    const auto plain = inspection->inspectCapabilities(1, id, access, cancelled);
    require(plain.capabilities && state.instances == 0 && oldOutput->restore(), "single-instance inspection disposes its temporary SDK before exact rollback reload");
    require(oldOutput->snapshot().driverIdentity && oldOutput->snapshot().driverIdentity->product == state.product
        && oldOutput->snapshot().driverIdentity->rawAsioVersion == -101,
        "restored output refreshes initialized identity and preserves signed driver-defined version");
    require(oldOutput->closeForSwitch() && oldOutput->releaseForReplacement(), "release original owner for selected panel");
    state.duringPanel = [&]
    {
        require(awaitingPanel.load() && state.instances == 1, "selected panel owns exactly one SDK object and reports dwell");
        state.rate = 96000; state.preferredBuffer = 256; state.changedOutputNames = true;
    };
    auto panel = singleeditor::JuceAsioBackendTestAccess::native(registration);
    const auto fresh = panel->inspectCapabilities(1, id, access, cancelled, &awaitingPanel);
    require(fresh.capabilities && !awaitingPanel.load() && state.instances == 0
        && fresh.capabilities->capabilities.reportedSampleRate == 96000
        && fresh.capabilities->capabilities.preferredBufferSize == 256 && !fresh.capabilities->capabilities.reportedBufferSize
        && fresh.capabilities->capabilities.outputNames == std::vector<std::string>{"Changed output (1)", "Changed output (2)"},
        "selected panel publishes freshly read detached capabilities and no fictitious current stream buffer");
    require(!oldOutput->restore() && !oldOutput->isOpen(), "old physical names cannot be restored after the panel changed them");
    require(oldOutput->closeForSwitch() && oldOutput->releaseForReplacement() && state.instances == 0,
        "failed old restore releases before a new explicit binding");
    auto replacement = singleeditor::JuceAsioBackendTestAccess::native(registration);
    require(replacement->prepareCapabilities(1, id, access) == Adapter::PrepareStatus::ready
        && replacement->openPrepared({96000, 256, {0, 1}}).status == Adapter::OpenStatus::ready,
        "explicit fresh binding opens the same single-instance driver at its actual new format");
    auto newOutput = replacement->takeOpenedOutput(); replacement.reset(); oldOutput.reset();
    require(newOutput && newOutput->isOpen() && state.instances == 1 && state.rejectedConcurrentCreates == 1,
        "retiring the old wrapper does not stop or release the replacement SDK object");
    newOutput.reset(); require(state.instances == 0, "replacement output drains fully");
    state.duringPanel = [&] { cancelled.store(true); };
    auto cancelledPanel = singleeditor::JuceAsioBackendTestAccess::native(registration);
    cancelled.store(false);
    const auto cancelledResult = cancelledPanel->inspectCapabilities(1, id, access, cancelled, &awaitingPanel);
    require(cancelledResult.preparation == Adapter::PrepareStatus::cancelled && !cancelledResult.capabilities
        && !awaitingPanel.load() && state.instances == 0, "cancelled panel releases its SDK and publishes no capabilities");
    state.duringPanel = {};
    std::cout << "Single-instance ASIO passed: reproduction, SDK release/reload, selected panel fresh capabilities, same-driver replacement and cancel\n";
}
}

HRESULT WINAPI singleeditor::tests::createDriverInstance(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != fixtureId || iid != fixtureId || outer || context != CLSCTX_INPROC_SERVER)
    { ++state.unexpectedCreates; return REGDB_E_CLASSNOTREG; }
    if (state.singleInstance && state.instances != 0) { ++state.rejectedConcurrentCreates; return E_FAIL; }
    ++state.creates;
    *result = static_cast<IASIO*>(new Driver());
    return S_OK;
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI initialise;
        driverIdentityCase();
        const auto noClockPreserved = clockCase(true, 96000.0);
        const auto unknownClockPreserved = clockCase(false, 48000.0);
        const auto highRatePreserved = constructorRateCase(384000.0, true);
        const auto invalidRateRejected = constructorRateCase(0.0, false);
        require(noClockPreserved && unknownClockPreserved && highRatePreserved && invalidRateRejected,
                "D107: ASIO must preserve driver clock/rate and reject an invalid reported rate");
        adapterClockCases();
        transferredOutputCase();
        singleInstanceReplacementAndPanelCase();
        managedEventMailboxCase();
        managedNativeEventsCase();
        std::cout << "AsioDriverTests passed: real JUCE/IASIO clock preservation and lifecycle\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "AsioDriverTests FAILED: " << error.what() << '\n'; return 1; }
}
