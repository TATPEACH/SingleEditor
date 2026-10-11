#include "Audio/AudioEngine.h"
#include "Audio/DeviceSwitchCoordinator.h"
#include "Audio/WindowsOutputBackend.h"
#include "Diagnostics/LiveDiagnosticProviders.h"
#include "Diagnostics/NativeDeviceMetadata.h"
#include "Diagnostics/GuiDeviceProbe.h"
#include "UI/Components/PreferencesComponent.h"
#include "UI/Components/AsioSettingsComponent.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/MainComponent.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace singleeditor
{
struct SpatialEnergyPanelTestAccess
{
    static void tick(SpatialEnergyPanelComponent& panel) { panel.timerCallback(); }
    static bool fieldReady(const SpatialEnergyPanelComponent& panel) { return panel.mapImage.isValid(); }
    static void overload(SpatialEnergyPanelComponent& panel) { panel.overloadUntilMs=juce::Time::getMillisecondCounterHiRes()+1500; }
    static const void* planeIdentity(const SpatialEnergyPanelComponent& panel)
    { return panel.field ? (panel.field->planarBase ? panel.field->planarBase.get() : panel.field.get()) : nullptr; }
    static std::weak_ptr<const SpatialEnergyVisualField> visual(const SpatialEnergyPanelComponent& panel) { return panel.field; }
    static int rowCount(const SpatialEnergyPanelComponent& panel) { return static_cast<int>(panel.rows.size()); }
    static bool rowsReadOnly(SpatialEnergyPanelComponent& panel)
    {
        juce::KeyboardFocusTraverser traverser;
        const auto focusable=traverser.getAllComponents(&panel);
        for (const auto& row : panel.rows)
        {
            auto* handler=row->getAccessibilityHandler();
            if (row->isEditable() || !row->getWantsKeyboardFocus() || !handler
                || !handler->getValueInterface() || !handler->getValueInterface()->isReadOnly()) return false;
            if (std::find(focusable.begin(),focusable.end(),row.get())==focusable.end()) return false;
        }
        return !panel.rows.empty();
    }
    static void displayControls(SpatialEnergyPanelComponent& panel)
    { panel.reading.setSelectedId(2,juce::sendNotificationSync); panel.floorControl.setValue(-72,juce::sendNotificationSync); }
    static void momentaryReadout(SpatialEnergyPanelComponent& panel) { panel.reading.setSelectedId(1,juce::sendNotificationSync); }
    static bool focusRow(SpatialEnergyPanelComponent& panel,int index)
    {
        auto& row=*panel.rows.at(static_cast<std::size_t>(index));
        row.grabKeyboardFocus();
        return row.hasKeyboardFocus(false) && panel.viewport.getViewArea().getY()<=row.getY()
            && panel.viewport.getViewArea().getBottom()>=row.getBottom();
    }
    static bool rowTraversalKeepsFocusVisible(SpatialEnergyPanelComponent& panel)
    {
        if (!focusRow(panel,0)) return false;
        for(std::size_t i=1;i<panel.rows.size();++i)
        {
            panel.rows[i-1]->moveKeyboardFocusToSibling(true);
            const auto& row=*panel.rows[i];
            if (!row.hasKeyboardFocus(false) || !panel.viewport.getViewArea().contains(row.getBounds().withWidth(1))) return false;
        }
        for(std::size_t i=panel.rows.size()-1;i>0;--i)
        {
            panel.rows[i]->moveKeyboardFocusToSibling(false);
            const auto& row=*panel.rows[i-1];
            if (!row.hasKeyboardFocus(false) || !panel.viewport.getViewArea().contains(row.getBounds().withWidth(1))) return false;
        }
        return true;
    }
    static void focusReading(SpatialEnergyPanelComponent& panel) { panel.reading.grabKeyboardFocus(); }
    static juce::var visualResources(const SpatialEnergyPanelComponent& panel)
    {
        juce::DynamicObject::Ptr facts=new juce::DynamicObject;
        const auto* base=panel.field ? (panel.field->planarBase ? panel.field->planarBase.get() : panel.field.get()) : nullptr;
        const auto bytes=base ? base->map.capacity()*sizeof(SpatialEnergyVisualField::Point)+base->pixelIndices.capacity()*sizeof(int) : 0;
        const auto sphereBytes=panel.field ? panel.field->sphere.capacity()*sizeof(SpatialEnergyVisualField::Point) : 0;
        facts->setProperty("planarVectorCapacityBytes",static_cast<juce::int64>(bytes));
        facts->setProperty("sphereVectorCapacityBytes",static_cast<juce::int64>(sphereBytes));
        facts->setProperty("visualWorker",panel.hasVisualWorker()); facts->setProperty("sphereActive",panel.hasSphereResource());
        facts->setProperty("rasterReady",panel.mapImage.isValid()); facts->setProperty("timerIntervalMs",panel.getTimerInterval());
        facts->setProperty("measurementConsumers",panel.engine.getSpatialEnergyDiagnostics().consumers);
        facts->setProperty("visualDegraded",panel.overloadUntilMs>juce::Time::getMillisecondCounterHiRes());
        return juce::var(facts.get());
    }
};
struct AsioSettingsTestAccess
{
    static bool followPending(const AsioSettingsSession& session) { return session.followPending; }
    static void install(MainComponent& main, std::shared_ptr<AsioSettingsSession> session) { main.asioSettingsSession = std::move(session); }
    static void poll(MainComponent& main) { main.timerCallback(); }
    static bool coreReady(const MainComponent& main) { return main.coreLifecycle.isCoreReady(main.activeDocumentGeneration); }
    static bool spatialEnabled(const MainComponent& main)
    { return main.resolveWorkspaceCommandState("workspace.module.spatial-energy.toggle",{}).enabled; }
    static WorkspaceModuleHostComponent* workspace(MainComponent& main)
    { main.promoteDeferredChromeIfNeeded(); main.ensureDeferredOutputMeter(); return main.workspaceModuleHost.get(); }
    static bool workspaceCommand(MainComponent& main, const char* id)
    { return main.dispatchGuiCommand(id,GuiCommandSource::palette,"workspace.modules").succeeded(); }
    static void observeWaveform(MainComponent& main, bool& painted)
    {
        main.waveformView.onWaveformPresentation = [&painted, previous = main.waveformView.onWaveformPresentation](bool full)
        { painted = true; if (previous) previous(full); };
    }
    static std::shared_ptr<AsioSettingsSession> make(AudioEngine& engine, juce::ApplicationProperties& properties,
        std::function<JuceAsioBackendAdapter::Catalog()> discover, DeviceSwitchCoordinator::Inspect inspect,
        DeviceSwitchCoordinator::Prepare prepare, DeviceSwitchCoordinator::PanelInspect panel = {})
    { return makeForBackends(engine, properties, [discover = std::move(discover)](const std::string&) -> AsioSettingsSession::Catalog { return discover(); }, std::move(inspect), std::move(prepare), std::move(panel)); }
    static std::shared_ptr<AsioSettingsSession> makeForBackends(AudioEngine& engine, juce::ApplicationProperties& properties,
        std::function<AsioSettingsSession::Catalog(const std::string&)> discover, DeviceSwitchCoordinator::Inspect inspect,
        DeviceSwitchCoordinator::Prepare prepare, DeviceSwitchCoordinator::PanelInspect panel = {})
    { return std::shared_ptr<AsioSettingsSession>(new AsioSettingsSession(engine, properties, {std::move(discover), std::move(inspect), std::move(prepare), std::move(panel)})); }
};
struct JuceAsioBackendTestAccess
{
    static std::unique_ptr<JuceAsioBackendAdapter> make(JuceAsioBackendAdapter::Factory factory,
        JuceAsioBackendAdapter::RegistryReader registry, JuceAsioBackendAdapter::DeviceFactory device)
    { return std::unique_ptr<JuceAsioBackendAdapter>(new JuceAsioBackendAdapter(std::move(factory), std::move(registry), std::move(device))); }
};
struct WindowsOutputBackendTestAccess
{
    static std::unique_ptr<WindowsOutputBackend> make(WindowsOutputBackend::Factory factory,
        WindowsOutputBackend::CatalogReader catalog, WindowsOutputBackend::Creator creator,
        WindowsOutputBackend::IdentityReader identity)
    { return std::unique_ptr<WindowsOutputBackend>(new WindowsOutputBackend(std::move(factory), std::move(catalog), std::move(creator), std::move(identity))); }
};
struct AudioEngineDeviceTestAccess
{
    static DeviceRoutePlan::Bus bus()
    { return {"fixture-canonical-reordered", 7, {{"right", SpeakerRole::R}, {"lfe", SpeakerRole::LFE, false}, {"left", SpeakerRole::L}}}; }
    static bool commit(AudioEngine& engine, std::unique_ptr<AudioEngine::DeviceLease>& lease, std::unique_ptr<DeviceOutput>& output,
        std::shared_ptr<const DeviceRoutePlan> plan, const DeviceRoutePlan::Bus& current)
    { return engine.commitDeviceLease(lease, output, std::move(plan), current); }
    static bool commit(AudioEngine& engine, std::unique_ptr<AudioEngine::DeviceLease>& lease, std::unique_ptr<DeviceOutput>& output)
    {
        const auto current = bus();
        auto draft = DeviceRoutePlan::propose(current, output->snapshot());
        draft.outputsBySpeaker = {{3}, {}, {1}};
        return commit(engine, lease, output, DeviceRoutePlan::validate(draft, output->snapshot(), current).approved(), current);
    }
    static std::shared_ptr<const DeviceRoutePlan> route(AudioEngine& engine) { return engine.deviceRoutePlan.load(); }
    static bool warmupPending(const AudioEngine& engine) { return engine.deviceWarmupTask.isPending(); }
    static void install(AudioEngine& engine, std::unique_ptr<juce::AudioDeviceManager> manager)
    {
        if (engine.deviceManager || engine.audioDeviceReady.load() || engine.deviceWarmupTask.isPending())
            throw std::runtime_error("fixture requires a cold independent engine");
        engine.deviceManager = std::move(manager);
    }
};
namespace
{
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
struct Controls
{
    std::atomic<int> opens { 0 }, scans { 0 }, aliveDevices { 0 };
    std::atomic<bool> failNext { false }, blockNext { false }, alwaysFail { false }, coerceNext { false };
    std::atomic<bool> failAttach { false };
    std::atomic<int> pingFrames {0};
    std::atomic<float> pingPeak {0};
    std::function<void()> duringOpen;
    bool managed = false;
    std::atomic<int> maintenanceCalls {0}, counterReads {0}, latencyReads {0};
    std::atomic<int> replacementReleases {0};
    bool renderDuringAttach = false;
    std::atomic<int> attachCallbacks {0};
    double maintenanceRate = 48000;
    DeviceOutput::Maintenance maintenanceResult = DeviceOutput::Maintenance::ready;
    std::function<void(bool, std::atomic<bool>&)> duringMaintenance;
    juce::WaitableEvent entered, release;
};
// Real JUCE manager negotiations against a controlled local IODevice. It has
// no OS endpoint or audio thread, and makes no physical device acceptance claim.
class FixtureDevice final : public juce::AudioIODevice
{
public:
    explicit FixtureDevice(Controls& value, const juce::String& backend = "Fixture Audio", int outputCount = 4)
        : AudioIODevice("Private Fixture Speakers", backend), controls(value), physicalOutputs(outputCount) { ++controls.aliveDevices; }
    ~FixtureDevice() override { --controls.aliveDevices; }
    juce::StringArray getOutputChannelNames() override
    { juce::StringArray names; for(int i=0;i<physicalOutputs;++i) names.add(juce::String(i+1)); return names; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512 }; }
    int getDefaultBufferSize() override { return 128; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger& outputs, double requestedRate, int requestedBuffer) override
    {
        ++controls.opens;
        if (controls.duringOpen) controls.duringOpen();
        if (controls.blockNext.exchange(false)) { controls.entered.signal(); controls.release.wait(2000); }
        if (controls.failNext.exchange(false) || controls.alwaysFail.load()) return "Private driver details C:\\fixture-user\\device.conf";
        channels = outputs; channels.setRange(physicalOutputs, juce::jmax(0, channels.getHighestBit() - physicalOutputs + 1), false);
        if (controls.coerceNext.exchange(false)) { channels.clear(); channels.setBit(0); }
        if (controls.managed) { sampleRate = requestedRate; bufferSamples = requestedBuffer; }
        opened = true; return {};
    }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* value) override
    { if (controls.failAttach.load()) return; callback = value; playing = true; if (callback) callback->audioDeviceAboutToStart(this); }
    void stop() override
    { playing = false; if (auto* old = std::exchange(callback, nullptr)) old->audioDeviceStopped(); }
    bool isPlaying() override { return playing; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSamples; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 16; }
    juce::BigInteger getActiveOutputChannels() const override { return channels; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 96; }
    int getInputLatencyInSamples() override { return 0; }
    juce::AudioBuffer<float> renderOutputs()
    {
        juce::AudioBuffer<float> result(physicalOutputs, 128); result.clear();
        // AudioDeviceManager's callback consumes a compact array whose size
        // equals the active channel count. The pointed-to sample buffers still
        // belong to the corresponding physical outputs.
        std::vector<float*> outputs(static_cast<std::size_t>(physicalOutputs)); int count = 0;
        for (int channel = 0; channel < physicalOutputs; ++channel)
            if (channels[channel]) outputs[count++] = result.getWritePointer(channel);
        require(callback != nullptr, "real manager has installed its callback on the fixture device");
        callback->audioDeviceIOCallbackWithContext(nullptr, 0, outputs.data(), count, 128, {});
        return result;
    }
private:
    Controls& controls;
    juce::AudioIODeviceCallback* callback = nullptr;
    int physicalOutputs = 4;
    juce::BigInteger channels;
    bool opened = false, playing = false;
    double sampleRate = 48000;
    int bufferSamples = 128;
};
class FixtureType final : public juce::AudioIODeviceType
{
public:
    explicit FixtureType(Controls& value, const juce::String& typeName = "Fixture Audio") : AudioIODeviceType(typeName), controls(value) {}
    void scanForDevices() override { ++controls.scans; }
    juce::StringArray getDeviceNames(bool input) const override { return input ? juce::StringArray{} : juce::StringArray{ "Private Fixture Speakers" }; }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool input) const override { return input ? -1 : 0; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output, const juce::String&) override
    { return output == "Private Fixture Speakers" ? new FixtureDevice(controls) : nullptr; }
private:
    Controls& controls;
};
class FixtureManager final : public juce::AudioDeviceManager
{
public:
    explicit FixtureManager(Controls& value) : controls(value) {}
    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override { types.add(new FixtureType(controls)); }
private:
    Controls& controls;
};
class FixtureOutput final : public DeviceOutput
{
public:
    explicit FixtureOutput(Controls& value, Snapshot requested = {}) : controls(value), device(std::make_unique<FixtureDevice>(value,
        "Fixture Audio", requested.outputNames.empty() ? 4 : static_cast<int>(requested.outputNames.size())))
    {
        state = { "Fixture Direct", "fixture:direct-output", "Private Fixture Speakers", 48000, 128, 16,
            {1, 3}, {"1", "2", "3", "4"}, 0, 96 };
        if (!requested.stableId.empty()) { state = std::move(requested); state.bitDepth = 16; }
        if (controls.managed) events.arm(1);
        require(restore(), "prepare a controlled direct candidate");
    }
    ~FixtureOutput() override { closeForSwitch(); }
    const Snapshot& snapshot() const noexcept override { return state; }
    bool isOpen() const noexcept override { return device->isOpen(); }
    bool attach(juce::AudioIODeviceCallback& callback) override
    {
        const juce::ScopedLock lock(callbackLock);
        if (attached || controls.failAttach || !isOpen() || outputSuspended()) return false;
        device->start(&callback); attached = true;
        if (controls.renderDuringAttach && state.physicalOutputs.size() > 1)
        { device->renderOutputs(); ++controls.attachCallbacks; }
        if (state.physicalOutputs.size() == 1)
            for (int block = 0; block < static_cast<int>(state.sampleRate * .25 / 128) + 2; ++block)
            {
                const auto audio = device->renderOutputs();
                controls.pingFrames.fetch_add(128);
                controls.pingPeak.store(std::max(controls.pingPeak.load(), audio.getMagnitude(0, 128)));
            }
        return true;
    }
    void detach() noexcept override
    { const juce::ScopedLock lock(callbackLock); device->stop(); attached = false; }
    bool closeForSwitch() noexcept override { detach(); device->close(); return !device->isOpen(); }
    bool releaseForReplacement() noexcept override
    { if (isOpen()) return false; ++controls.replacementReleases; return true; }
    bool restore() noexcept override
    {
        juce::BigInteger channels; for (int physical : state.physicalOutputs) channels.setBit(physical);
        if (device->open({}, channels, controls.managed ? state.sampleRate : 48000, state.bufferSamples).isEmpty() && device->getActiveOutputChannels() == channels)
        { events.read(1, true); events.acknowledgeRevalidation(1); return true; }
        device->close(); return false;
    }
    juce::AudioBuffer<float> render()
    {
        const juce::ScopedLock lock(callbackLock);
        if (attached && !outputSuspended()) return device->renderOutputs();
        juce::AudioBuffer<float> silent(4, 128); silent.clear(); return silent;
    }
    bool hasManagedEvents() const noexcept override { return controls.managed; }
    bool outputSuspended() const noexcept override { return controls.managed && (!isOpen() || events.outputSuspended()); }
    AsioDriverEvents::Snapshot driverEvents(bool consume, bool counters) noexcept override
    { if (counters) ++controls.counterReads; return events.read(1, consume, counters); }
    bool refreshDriverLatency() override { ++controls.latencyReads; state.outputLatencySamples = 192; return true; }
    void setReportedLatency(std::optional<int> value) { state.outputLatencySamples = value; }
    void notify(AsioDriverEvents::Flag flag) { events.publish(1, flag, static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks())); }
    Maintenance maintain(bool panel, const std::atomic<bool>& cancelled, std::atomic<bool>& awaitingPanel) override
    {
        require(!isOpen() && !attached, "maintenance owns the closed exact fixture");
        previousState = state; ++controls.maintenanceCalls;
        if (controls.duringMaintenance) controls.duringMaintenance(panel, awaitingPanel);
        if (cancelled.load()) return Maintenance::cancelled;
        if (controls.maintenanceResult != Maintenance::ready) return controls.maintenanceResult;
        state.sampleRate = controls.maintenanceRate;
        return restore() ? Maintenance::ready : Maintenance::unavailable;
    }
    void acceptMaintenance() noexcept override { previousState.reset(); }
    void rollbackMaintenance() noexcept override
    { closeForSwitch(); if (previousState) { state = *previousState; previousState.reset(); } }
private:
    Controls& controls;
    std::unique_ptr<FixtureDevice> device;
    Snapshot state;
    juce::CriticalSection callbackLock;
    bool attached = false;
    AsioDriverEvents events;
    std::optional<Snapshot> previousState;
};
juce::var snapshot(AudioEngine&, SingleTrackDocument&, const char* section = "audio", bool nativeMetadata = false);

void runAsioIdentityDiagnosticsTests(const juce::File& artifacts)
{
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Controls controls;
        SingleTrackDocument document; AudioEngine engine(document);
        require(!engine.getDeviceDiagnostics().driverIdentity, "cold engine does not acquire an ASIO identity");
        require(engine.reserveExplicitStartupOutput(), "identity fixture keeps the default output cold");
        DeviceOutput::Snapshot target {"ASIO", "asio:identity-fixture", "Private Fixture Speakers", 48000, 128, 16,
            {1, 3}, {"1", "2", "3", "4"}, 0, 96};
        if (scenario != 2)
            target.driverIdentity = DeviceOutput::DriverIdentity {"Actual IASIO product",
                scenario == 0 ? std::optional<std::int32_t>{0} : std::nullopt, scenario == 1};
        std::unique_ptr<DeviceOutput> output = std::make_unique<FixtureOutput>(controls, target);
        auto lease = engine.takeDeviceLeaseForSwitch();
        require(lease && lease->closeForSwitch() && AudioEngineDeviceTestAccess::commit(engine, lease, output),
            "identity fixture transfers an exact approved output to the actual engine");
        lease.reset();
        const auto opens = controls.opens.load(), scans = controls.scans.load();
        const auto documentState = document.getState();
        auto captured = diagnostics::captureLiveDiagnosticProviders(engine, document, nullptr);
        std::atomic<bool> cancelled {false};
        diagnostics::enrichNativeDeviceMetadata(captured, cancelled);
        std::set<diagnostics::Field> fields;
        for (const auto& item : captured.front().evidence.fields)
            require(fields.insert(item.field).second, "native enrichment must not duplicate ASIO metadata fields");
        require(fields.size() <= 64, "ASIO metadata remains within the original provider field budget");
        const auto observed = snapshot(engine, document, "audio", true);
        const auto metadata = observed["fields"];
        if (scenario != 2)
        {
            require(engine.getDeviceDiagnostics().driverIdentity
                && metadata["driverProduct"]["value"].toString() == "Actual IASIO product"
                && metadata["driverMetadataSource"]["value"].toString() == "initialized-IASIO-name-and-raw-version"
                && metadata["deviceMetadataState"]["value"].toString() == "driver-interface-cached",
                "actual owner facts reach the serialized public product/source fields without a display alias");
            require(scenario == 0 ? metadata["driverVersion"]["value"].toString() == "0"
                : metadata["driverVersion"]["unavailable"].toString() == "capture-failed",
                "raw zero version and unavailable optional version stay distinct");
            for (const auto* field : {"driverVendor", "hardwareManufacturer", "speakerMask", "speakerMaskSource"})
                require(metadata[field]["unavailable"].toString() == "not-supported",
                    "IASIO identity does not fabricate vendor, hardware or Windows speaker-mask provenance");
        }
        else
            require(!engine.getDeviceDiagnostics().driverIdentity
                && metadata["deviceMetadataState"]["value"].toString() == "not-supported"
                && metadata["driverProduct"]["unavailable"].toString() == "not-reported",
                "ASIO owner without interface evidence remains explicitly unavailable");
        require(controls.opens == opens && controls.scans == scans && document.getState() == documentState
            && !engine.getDeviceManagerForConfiguration(), "metadata capture leaves device factories and the document unchanged");
        require(artifacts.getChildFile("asio-driver-identity-" + juce::String(scenario) + ".json")
            .replaceWithText(juce::JSON::toString(observed)), "retain actual serialized identity evidence");
        lease = engine.takeDeviceLeaseForSwitch();
        require(lease && !engine.getDeviceDiagnostics().driverIdentity,
            "leased owner cannot leak an identity as if its output were currently observable");
        require(lease->closeForSwitch() && lease->restore() && engine.returnDeviceLease(lease),
            "return the exact owner through its normal lease lifecycle after the passive busy check");
    }
    std::cout << "ASIO identity diagnostics passed: owner cache, raw version, typed absence, no duplicate enrichment or device opens\n";
}
#include "SpatialEnergyEngineTests.cpp.inc"

void runFollowSourceTests(const juce::File& artifacts)
{
    using Switch = DeviceSwitchCoordinator;
    Controls controls; controls.managed = true;
    controls.renderDuringAttach = true;
    SingleTrackDocument document; AudioEngine engine(document);
    require(engine.reserveExplicitStartupOutput(), "follow fixture reserves explicit output without default warmup");
    const auto load = [&](int sourceRate)
    {
        const auto file = artifacts.getChildFile("follow-source-" + juce::String(sourceRate) + ".wav");
        juce::MemoryOutputStream wave;
        wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 4); wave.write("WAVEfmt ", 8);
        wave.writeInt(16); wave.writeShort(1); wave.writeShort(2); wave.writeInt(sourceRate);
        wave.writeInt(sourceRate * 4); wave.writeShort(4); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 4);
        for (int frame = 0; frame < 4096; ++frame) { wave.writeShort(4096); wave.writeShort(-4096); }
        require(file.replaceWithData(wave.getData(), wave.getDataSize()) && engine.loadFile(file), "load real Follow Source waveform");
        engine.seekToSeconds(0);
    };
    load(44100);
    DeviceOutput::Snapshot target {"ASIO", "asio:follow-fixture", "Follow ASIO fixture", 48000, 128, 16,
        {1, 3}, {"1", "2", "3", "4"}, 0, 96};
    auto initial = std::make_unique<FixtureOutput>(controls, target);
    std::atomic<FixtureOutput*> activeView {initial.get()};
    std::unique_ptr<DeviceOutput> candidate = std::move(initial);
    auto lease = engine.takeDeviceLeaseForSwitch();
    require(lease && lease->closeForSwitch() && AudioEngineDeviceTestAccess::commit(engine, lease, candidate), "install Follow Source approved route");
    lease.reset();
    juce::ApplicationProperties properties; juce::PropertiesFile::Options options;
    options.applicationName = "FollowSourceSession"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("follow-source-private").getFullPathName(); properties.setStorageParameters(options);
    std::atomic<int> requests {0}; std::atomic<double> failRate {0}, holdRate {0};
    juce::WaitableEvent entered, release;
    Switch::Prepare prepare = [&](const Switch::Selection& choice, const DeviceSwitchAccess&, const std::atomic<bool>& cancelled) -> Switch::Prepared
    {
        ++requests;
        require(choice.expected.stableId == target.stableId && choice.expected.backend == "ASIO"
            && choice.expected.bufferSamples == 128 && choice.expected.physicalOutputs == target.physicalOutputs,
            "Follow Source requests the active exact route/buffer, independently of settings draft");
        if (choice.expected.sampleRate == holdRate.load()) { entered.signal(); release.wait(4000); }
        if (cancelled.load()) return {{}, Switch::Error::cancelled};
        if (choice.expected.sampleRate == failRate.load()) return {{}, Switch::Error::prepareFailed};
        auto output = std::make_unique<FixtureOutput>(controls, choice.expected);
        activeView.store(output.get());
        return {std::move(output), Switch::Error::none};
    };
    auto session = AsioSettingsTestAccess::make(engine, properties, [] { return JuceAsioBackendAdapter::Catalog{}; }, {}, prepare);
    const auto drain = [&]
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        do { session->poll(); juce::Thread::sleep(2); }
        while ((session->busy() || AsioSettingsTestAccess::followPending(*session)) && juce::Time::getMillisecondCounterHiRes() < deadline);
        require(!session->busy() && !AsioSettingsTestAccess::followPending(*session), "Follow Source drains exact transaction and latest pending intent");
    };
    require(!session->followsSourceSampleRate(), "Follow Source defaults off");
    drain(); require(requests == 0 && engine.activeOutputSnapshot()->sampleRate == 48000, "default source change keeps confirmed hardware clock");
    auto metrics = engine.getRuntimeMetricsForDiagnostics();
    require(metrics && metrics->aggregateOutputLatencySamples == 96 && metrics->aggregateOutputLatencyMs == 2.0
        && metrics->processingLatencySamples == 0 && metrics->playbackRate.mode == PlaybackRateSource::Mode::converting,
        "aggregate latency uses actual device units, excludes read-ahead and does not add another buffer");
    activeView.load()->setReportedLatency({});
    metrics = engine.getRuntimeMetricsForDiagnostics();
    require(metrics && !metrics->aggregateOutputLatencySamples && !metrics->aggregateOutputLatencyMs, "unknown driver latency does not become zero aggregate latency");
    activeView.load()->setReportedLatency(0);
    require(engine.getRuntimeMetricsForDiagnostics()->aggregateOutputLatencyMs == 0.0, "reported zero latency remains available");
    activeView.load()->setReportedLatency(96);
    session->selectBackend("Windows Audio");
    auto component = std::make_unique<AsioSettingsComponent>(session);
    component->setSize(820, 600); component->addToDesktop(juce::ComponentPeer::windowIsTemporary); component->setVisible(true);
    auto* toggle = dynamic_cast<juce::ToggleButton*>(component->findChildWithID("audio-output.follow-source"));
    require(toggle && toggle->isEnabled() && !toggle->getToggleState(), "native expert toggle is initially off");
    const auto original = document.getState();
    engine.play(); activeView.load()->render();
    toggle->setToggleState(true, juce::dontSendNotification); toggle->onClick(); drain();
    require(requests == 1 && engine.activeOutputSnapshot()->sampleRate == 44100 && engine.isPlaying()
        && document.getState() == original && session->selectedBackend() == "Windows Audio", "explicit toggle switches active output and preserves unrelated draft, Document and transport");
    activeView.load()->render(); const auto played = activeView.load()->render();
    metrics = engine.getRuntimeMetricsForDiagnostics();
    require(metrics && metrics->playbackRate.mode == PlaybackRateSource::Mode::bypassed
        && std::abs(*metrics->aggregateOutputLatencyMs - 96000.0 / 44100.0) < .000001
        && played.getMagnitude(1, 0, 128) > .001f, "actual 44.1k stream plays physical samples and reports rate-correct latency");
    engine.pause(); session->setFollowSourceSampleRate(false);
    const auto fixedOpens = controls.opens.load(); load(96000); drain();
    require(requests == 1 && controls.opens == fixedOpens && engine.activeOutputSnapshot()->sampleRate == 44100, "disabling follow keeps current confirmed rate for new files");
    session->setFollowSourceSampleRate(true); drain();
    require(requests == 2 && engine.activeOutputSnapshot()->sampleRate == 96000, "re-enabling follows current 96k source");
    holdRate.store(48000); load(48000); session->poll(); require(entered.wait(2000), "rate switch reaches held worker");
    load(44100); const auto latest = document.getState(); engine.seekToSeconds(.005); release.signal(); drain(); holdRate.store(0);
    std::cout << "Follow latest-source: requests=" << requests.load() << " rate=" << (engine.activeOutputSnapshot() ? engine.activeOutputSnapshot()->sampleRate : 0)
        << " document=" << (document.getState() == latest) << " position=" << engine.getCurrentPositionSeconds()
        << " owners=" << controls.aliveDevices.load() << " status=" << session->status() << '\n';
    require(requests == 4 && engine.activeOutputSnapshot()->sampleRate == 44100 && document.getState() == latest
        && std::abs(engine.getCurrentPositionSeconds() - .005) < .0001 && controls.aliveDevices == 1 && controls.attachCallbacks >= 4,
        "new source supersedes held rate request, releases temporary owner and preserves latest seek");
    const auto saved = juce::JSON::parse(properties.getUserSettings()->getValue(AsioSettingsSession::bindingKey));
    require(static_cast<double>(saved["sampleRate"]) == 44100 && properties.getUserSettings()->getBoolValue(AsioSettingsSession::followSourceKey),
        "only committed actual format and explicit follow preference are persisted");
    failRate.store(88200); load(88200); drain(); const auto failedRequests = requests.load();
    require(engine.isSelectedOutputReady() && engine.activeOutputSnapshot()->sampleRate == 44100 && session->status().contains("previous output returned"),
        "unsupported rate returns last known good output and exposes the failure");
    for (int poll = 0; poll < 20; ++poll) session->poll();
    require(requests == failedRequests, "failed source does not cause a polling retry loop");
    failRate.store(0); session->retryOutput(); drain();
    require(engine.activeOutputSnapshot()->sampleRate == 88200, "explicit Retry requests failed source again");
    holdRate.store(48000); load(48000); session->poll(); require(entered.wait(2000), "cancellable source switch reaches worker");
    engine.seekToSeconds(.006); session->setFollowSourceSampleRate(false); release.signal(); drain(); holdRate.store(0);
    require(!session->followsSourceSampleRate() && engine.activeOutputSnapshot()->sampleRate == 88200
        && std::abs(engine.getCurrentPositionSeconds() - .006) < .0001, "turning Follow Source off cancels pending request without changing latest seek");
    session->setFollowSourceSampleRate(true); drain();
    juce::Thread::sleep(1050); session->poll();
    require(session->liveOutputStatus().contains("48000 Hz source") && session->liveOutputStatus().contains("aggregate latency: 2.00 ms"),
        "visible device UI reports source/actual output rates and aggregate latency");
    session->sendSynchronousChangeMessage();
    juce::PNGImageFormat png; const auto shot = artifacts.getChildFile("follow-source-latency-minimum.png");
    if (auto stream = shot.createOutputStream()) require(png.writeImageToStream(component->createComponentSnapshot(component->getLocalBounds()), *stream), "retain native Follow Source controls");
    component.reset(); session->shutdown();
    session = AsioSettingsTestAccess::make(engine, properties, [] { return JuceAsioBackendAdapter::Catalog{}; }, {}, prepare);
    require(session->followsSourceSampleRate(), "new settings session restores explicit preference"); drain();
    failRate.store(96000); controls.alwaysFail.store(true); load(96000); const auto failedDocument = document.getState(); drain();
    require(!engine.isSelectedOutputReady() && !engine.getRuntimeMetricsForDiagnostics() && document.getState() == failedDocument
        && session->status().contains("Output is unavailable"), "failed candidate and failed restore leave honest unavailable output without reloading Document");
    controls.alwaysFail.store(false); session->setFollowSourceSampleRate(false); session->shutdown();
    std::cout << "Follow Source passed: default fixed clock, native toggle, exact 44.1/48/96, latest source, cancel, failure/Retry/restore, persistence and aggregate latency\n";
}

void runManagedOutputSessionTests(const juce::File& artifacts)
{
    Controls controls, otherControls; controls.managed = true; otherControls.managed = true;
    SingleTrackDocument document; AudioEngine engine(document);
    require(engine.reserveExplicitStartupOutput() && engine.loadFile(artifacts.getChildFile("physical-route-source.wav")),
        "managed output test loads a real document without default device warmup");
    DeviceOutput::Snapshot target {"ASIO", "asio:managed-fixture", "Managed ASIO fixture", 48000, 128, 16,
        {1, 3}, {"1", "2", "3", "4"}, 0, 96};
    auto output = std::make_unique<FixtureOutput>(controls, target); auto* view = output.get();
    std::unique_ptr<DeviceOutput> candidate = std::move(output);
    auto lease = engine.takeDeviceLeaseForSwitch();
    require(lease && lease->closeForSwitch() && AudioEngineDeviceTestAccess::commit(engine, lease, candidate), "install exact managed owner");
    lease.reset();
    const auto original = document.getState();
    juce::ApplicationProperties settings; juce::PropertiesFile::Options options;
    options.applicationName = "ManagedOutputSession"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("managed-output-private").getFullPathName(); settings.setStorageParameters(options);
    auto session = AsioSettingsTestAccess::make(engine, settings, [] { return JuceAsioBackendAdapter::Catalog{}; }, {}, {});
    const auto drain = [&]
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline)
        { session->poll(); juce::Thread::sleep(2); }
        require(!session->busy(), "managed transaction drains without retaining the engine lease");
    };
    session->selectBackend("Windows Audio"); // Draft browsing must not retarget driver recovery.
    view->notify(AsioDriverEvents::overload); view->notify(AsioDriverEvents::latency);
    session->poll();
    require(!session->busy() && controls.maintenanceCalls == 0 && controls.counterReads == 0 && controls.latencyReads == 1
        && engine.activeOutputSnapshot()->outputLatencySamples == 192, "hidden latency-only events do not reopen or read trend counters");
    bool visibleConsumer = false;
    session->setDeviceUiConsumer(&visibleConsumer, [&] { return visibleConsumer; }); session->poll();
    require(controls.counterReads == 0, "registered but hidden or minimised window reads no trend counters");
    visibleConsumer = true; session->poll();
    require(controls.counterReads == 1 && session->liveOutputStatus().contains("xruns 1"), "visible consumer reads actual event counters on demand");
    session->setDeviceUiConsumer(&visibleConsumer);
    controls.maintenanceRate = 96000;
    controls.duringMaintenance = [&](bool, std::atomic<bool>&) { controls.entered.signal(); controls.release.wait(3000); };
    engine.play(); view->render();
    view->notify(AsioDriverEvents::reset); view->notify(AsioDriverEvents::bufferSize); view->notify(AsioDriverEvents::resync);
    require(!engine.isSelectedOutputReady(), "critical event immediately invalidates selected-output readiness");
    const auto begin = juce::Time::getMillisecondCounterHiRes(); session->poll();
    require(juce::Time::getMillisecondCounterHiRes() - begin < 500 && controls.entered.wait(2000), "gated callback admission does not wait for JUCE fade-out");
    engine.seekToSeconds(.002); engine.pause(); engine.play();
    const auto latestPlayStart = engine.getPlayStartSeconds();
    controls.release.signal(); drain(); controls.duringMaintenance = {};
    require(controls.maintenanceCalls == 1 && controls.aliveDevices == 1 && engine.isSelectedOutputReady()
        && engine.isPlaying() && std::abs(engine.getCurrentPositionSeconds() - .002) < .0001
        && engine.getPlayStartSeconds() == latestPlayStart && document.getState() == original,
        "coalesced events reuse one driver and preserve document and the user's latest play-start/transport intent");
    view->render(); const auto audio = view->render();
    const auto metrics = engine.getRuntimeMetricsForDiagnostics();
    require(metrics && metrics->sampleRate == 96000 && metrics->playbackRate.deviceRate == 96000
        && audio.getMagnitude(1, 0, 128) > .001f, "revalidated device rate prepares the actual shared SRC and renders physical samples");
    const auto saved = juce::JSON::parse(settings.getUserSettings()->getValue(AsioSettingsSession::bindingKey));
    require(static_cast<double>(saved["sampleRate"]) == 96000 && saved["stableId"].toString() == "asio:managed-fixture"
        && session->selectedBackend() == "Windows Audio", "maintenance saves the active confirmed owner without overwriting the unrelated draft");
    engine.pause();

    // The actual coordinator observes a user-held panel for more than its
    // ordinary driver-call timeout; no artificial clock override is involved.
    controls.duringMaintenance = [&](bool panel, std::atomic<bool>& waiting)
    {
        require(panel, "explicit control panel reaches the active managed owner");
        waiting.store(true); controls.entered.signal(); controls.release.wait(8000); waiting.store(false);
    };
    session->openControlPanel(); require(controls.entered.wait(2000), "panel reached worker");
    const auto panelDeadline = juce::Time::getMillisecondCounterHiRes() + 5200;
    while (juce::Time::getMillisecondCounterHiRes() < panelDeadline)
    { session->poll(); juce::Thread::sleep(5); }
    require(session->busy() && !session->driverUnresolved() && session->status().contains("Control Panel is open"),
        "user panel dwell is not treated as a hung driver or cancelled by timeout");
    engine.seekToSeconds(.003); controls.release.signal(); drain(); controls.duringMaintenance = {};
    require(engine.isSelectedOutputReady() && std::abs(engine.getCurrentPositionSeconds() - .003) < .0001, "panel return keeps latest paused seek");

    session->retryOutput(); drain(); const auto callsBeforeStorm = controls.maintenanceCalls.load();
    for (int reset = 0; reset < 3; ++reset) { view->notify(AsioDriverEvents::reset); session->poll(); drain(); }
    require(controls.maintenanceCalls == callsBeforeStorm + 2 && !engine.isSelectedOutputReady()
        && session->status().contains("automatic recovery stopped") && !engine.isDeviceLeasedForProbe(),
        "fourth transaction within 30 seconds closes the exact output without another automatic reopen");
    const auto stoppedCalls = controls.maintenanceCalls.load();
    for (int poll = 0; poll < 20; ++poll) session->poll();
    require(controls.maintenanceCalls == stoppedCalls && document.getState() == original, "open circuit does not spin or affect editing state");
    session->retryOutput(); drain(); require(engine.isSelectedOutputReady(), "explicit Retry releases only this driver's circuit");
    controls.maintenanceResult = DeviceOutput::Maintenance::unavailable;
    view->notify(AsioDriverEvents::reset); session->poll(); drain();
    require(engine.isSelectedOutputReady(), "failed revalidation restores the last known good exact output");
    controls.maintenanceResult = DeviceOutput::Maintenance::ready;
    controls.duringMaintenance = [&](bool, std::atomic<bool>&) { controls.entered.signal(); controls.release.wait(3000); };
    session->retryOutput(); require(controls.entered.wait(2000), "cancellable maintenance holds driver seam");
    engine.seekToSeconds(.004); session->cancel(); controls.release.signal(); drain(); controls.duringMaintenance = {};
    require(engine.isSelectedOutputReady() && std::abs(engine.getCurrentPositionSeconds() - .004) < .0001,
        "cancel drains and returns the exact original output with latest seek");
    controls.maintenanceResult = DeviceOutput::Maintenance::rebindRequired;
    session->retryOutput(); drain();
    require(!engine.isSelectedOutputReady() && session->status().contains("Validate and Ping"), "physical capability change refuses rollback and requires explicit rebind");
    session->retryOutput(); require(!session->busy() && !engine.isSelectedOutputReady(), "Retry cannot bypass physical rebind requirement");
    controls.maintenanceResult = DeviceOutput::Maintenance::ready;
    session->openControlPanel(); drain();
    require(!engine.isSelectedOutputReady(), "Control Panel remains accessible after topology invalidation but cannot bypass rebind");
    {
        AsioSettingsComponent panel(session); panel.setSize(820, 600);
        session->poll(); session->sendSynchronousChangeMessage();
        auto file = artifacts.getChildFile("asio-managed-rebind-minimum.png").createOutputStream();
        require(file && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()), *file), "retain native recovery controls and rebind state");
    }
    require(document.getState() == original, "all managed output paths retain the original document state");
    target.stableId = "asio:another-managed-fixture";
    auto another = std::make_unique<FixtureOutput>(otherControls, target); auto* anotherView = another.get();
    std::unique_ptr<DeviceOutput> anotherCandidate = std::move(another);
    auto anotherLease = engine.takeDeviceLeaseForSwitch();
    require(anotherLease && anotherLease->closeForSwitch() && AudioEngineDeviceTestAccess::commit(engine, anotherLease, anotherCandidate),
        "explicit device change installs another exact owner after a rebind failure");
    anotherLease.reset(); anotherView->notify(AsioDriverEvents::reset); session->poll(); drain();
    require(otherControls.maintenanceCalls == 1 && engine.isSelectedOutputReady(), "per-device circuit and rebind state do not poison another driver");
    session->shutdown();
    std::cout << "Managed ASIO session passed: exact owner, active/draft separation, actual-rate SRC, panel dwell, latency, fuse, retry, cancel and rebind\n";
}
void runSelectedAsioPanelTests(const juce::File& artifacts)
{
    using Switch = DeviceSwitchCoordinator;
    Controls oldControls, selectedControls; oldControls.managed = true; selectedControls.managed = true;
    SingleTrackDocument document; AudioEngine engine(document);
    require(engine.reserveExplicitStartupOutput() && engine.loadFile(artifacts.getChildFile("physical-route-source.wav")), "selected-panel test has a real editable document");
    DeviceOutput::Snapshot oldTarget {"ASIO", "asio:prior-panel-owner", "Previous ASIO", 48000, 128, 16,
        {1, 3}, {"1", "2", "3", "4"}, 0, 96};
    auto oldOutput = std::make_unique<FixtureOutput>(oldControls, oldTarget); auto* oldView = oldOutput.get();
    std::unique_ptr<DeviceOutput> originalOutput = std::move(oldOutput);
    auto lease = engine.takeDeviceLeaseForSwitch();
    require(lease && lease->closeForSwitch() && AudioEngineDeviceTestAccess::commit(engine, lease, originalOutput), "selected-panel test installs previous exact output");
    lease.reset(); const auto originalDocument = document.getState();
    juce::ApplicationProperties properties; juce::PropertiesFile::Options options;
    options.applicationName = "SelectedPanelTest"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("selected-panel-private").getFullPathName(); properties.setStorageParameters(options);
    const std::string selectedId = "asio:selected-panel-fixture";
    std::atomic<int> panelCalls {0}; std::atomic<bool> holdPanel {true};
    std::atomic<FixtureOutput*> latestOutput {nullptr};
    const auto read = [&](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access,
        const std::atomic<bool>& cancelled) -> Switch::Inspected
    {
        require(access.isValid() && oldControls.replacementReleases > 0, "selected driver inspection follows actual SDK-release stage");
        if (cancelled.load()) return {{}, Switch::Error::cancelled};
        auto facts = std::make_shared<PreparedDeviceCapabilities>(); facts->stableId = choice.stableId;
        facts->capabilities.outputNames = {"Rebound 1", "Rebound 2", "Rebound 3", "Rebound 4"};
        facts->capabilities.sampleRates = {48000, 96000}; facts->capabilities.bufferSizes = {128, 256};
        facts->capabilities.reportedSampleRate = 96000; facts->capabilities.preferredBufferSize = 256;
        facts->capabilities.hasControlPanel = true;
        return {facts, Switch::Error::none};
    };
    auto session = AsioSettingsTestAccess::make(engine, properties,
        [&] { return JuceAsioBackendAdapter::Catalog{JuceAsioBackendAdapter::Status::available, 1, {{"Selected ASIO", selectedId}}}; }, read,
        [&](const Switch::Selection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Prepared
        {
            require(access.isValid() && oldControls.replacementReleases > 0, "explicit selected activation enters after old SDK release");
            if (cancelled.load()) return {{}, Switch::Error::cancelled};
            auto output = std::make_unique<FixtureOutput>(selectedControls, choice.expected);
            latestOutput.store(output.get()); return {std::move(output), Switch::Error::none};
        },
        [&](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled,
            std::atomic<bool>& waiting) -> Switch::Inspected
        {
            require(choice.stableId == selectedId, "explicit panel targets selected driver rather than previous active driver");
            ++panelCalls; waiting.store(true); selectedControls.entered.signal();
            if (holdPanel.load()) selectedControls.release.wait(4000);
            waiting.store(false); return read(choice, access, cancelled);
        });
    const auto drain = [&]
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(2); }
        require(!session->busy(), "selected-panel transaction drains");
    };
    session->refreshDrivers(); drain(); session->selectDriver(selectedId);
    require(session->canOpenControlPanel() && !session->capabilities(), "unactivated selected driver can open its panel before a route exists");
    {
        AsioSettingsComponent panel(session);
        auto* button = dynamic_cast<juce::TextButton*>(panel.findChildWithID("asio.control-panel"));
        require(button && button->isEnabled(), "real native selected-panel button is enabled"); button->onClick();
        require(selectedControls.entered.wait(2000), "selected panel reaches exclusive worker");
        session->poll(); require(session->status().contains("Control Panel is open"), "selected panel exposes user-wait state");
        engine.seekToSeconds(.006); session->cancel(); selectedControls.release.signal(); drain();
        require(!session->capabilities() && engine.isSelectedOutputReady() && engine.activeOutputSnapshot()->stableId == oldTarget.stableId
            && std::abs(engine.getCurrentPositionSeconds() - .006) < .0001, "cancelled selected panel publishes no capabilities and returns exact prior output/latest seek");
        holdPanel.store(false); button->onClick(); drain(); session->sendSynchronousChangeMessage();
        require(session->capabilities() && session->sampleRate() == 96000 && session->bufferSize() == 256
            && engine.activeOutputSnapshot()->sampleRate == 48000 && engine.activeOutputSnapshot()->stableId == oldTarget.stableId,
            "panel capabilities describe the selected driver without silently activating it or changing old stream format");
        panel.setSize(820, 600); auto file = artifacts.getChildFile("asio-selected-panel-review.png").createOutputStream();
        require(file && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()), *file), "retain actual selected-panel review controls");
    }
    // A physical change leaves a retained closed owner. Reading replacement
    // capabilities must preserve this absence rather than try to reopen it.
    oldControls.maintenanceResult = DeviceOutput::Maintenance::rebindRequired;
    oldView->notify(AsioDriverEvents::reset); session->poll(); drain();
    require(!engine.isSelectedOutputReady(), "old route becomes unavailable after physical invalidation");
    const auto oldOpens = oldControls.opens.load();
    session->inspect(); drain();
    require(session->capabilities() && !engine.isSelectedOutputReady() && oldControls.opens == oldOpens
        && session->status().contains("Output remains unavailable"), "inspection returns new facts while preserving the old closed state");
    const auto validateStereo = [&]
    {
        const auto& profiles = session->profiles();
        const auto stereo = std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) { return profile.bus.speakers.size() == 2; });
        require(stereo != profiles.end(), "standard stereo is available for explicit rebind");
        session->selectProfile(static_cast<int>(std::distance(profiles.begin(), stereo)));
        session->setConnection(0, 1, true); session->setConnection(1, 3, true); session->validate();
        require(session->review() && session->review()->isValid(), "new physical capabilities get a new validated explicit matrix");
    };
    validateStereo(); session->ping(1); drain();
    require(!engine.isSelectedOutputReady() && selectedControls.pingFrames > 20000 && selectedControls.aliveDevices == 0,
        "96k physical Ping drains its temporary output and keeps the invalid old route closed");
    session->activate(false); drain();
    require(engine.isSelectedOutputReady() && engine.activeOutputSnapshot()->stableId == selectedId
        && engine.activeOutputSnapshot()->sampleRate == 96000 && engine.activeOutputSnapshot()->bufferSamples == 256
        && oldControls.aliveDevices == 0 && selectedControls.aliveDevices == 1,
        "explicit rebind activates the new exact owner and releases the old wrapper");
    session->inspect(); drain();
    require(session->capabilities() && selectedControls.replacementReleases == 1 && engine.isSelectedOutputReady(),
        "same-driver inspection releases the old SDK before prepare then returns its exact output");
    validateStereo(); session->activate(false); drain();
    require(selectedControls.replacementReleases == 2 && engine.isSelectedOutputReady() && selectedControls.aliveDevices == 1,
        "same-driver activation uses SDK release and leaves one final owner");
    engine.setMonitorGainLinear(1); engine.play(); latestOutput.load()->render();
    require(latestOutput.load()->render().getMagnitude(1, 0, 128) > .001f && document.getState() == originalDocument,
        "rebound output plays actual document samples and keeps document history state");
    engine.pause(); session->shutdown();
    std::cout << "Selected ASIO panel/rebind passed: explicit target, native button, cancel/owner return, unavailable inspection, 96k Ping, same-driver replacement and real playback\n";
}
void runDeviceSwitchCoordinatorTests(const juce::File& artifacts, bool lifecycleOnly = false)
{
    using Switch = DeviceSwitchCoordinator;
    const auto selection = []
    {
        Switch::Selection selected;
        selected.generation = 19;
        selected.expected = {"Fixture Direct", "fixture:direct-output", "Private Fixture Speakers", 48000, 128, 16,
            {1, 3}, {"1", "2", "3", "4"}, 0, 96};
        selected.bus = AudioEngineDeviceTestAccess::bus();
        auto draft = DeviceRoutePlan::propose(selected.bus, selected.expected); draft.outputsBySpeaker = {{3}, {}, {1}};
        selected.plan = DeviceRoutePlan::validate(draft, selected.expected, selected.bus).approved();
        return selected;
    };
    for (int scenario = 0; scenario < 10; ++scenario)
    {
        if (lifecycleOnly && scenario != 0 && scenario != 2) continue;
        // 0 parallel success; 1 exclusive success; 2 late cancellation;
        // 3 stale selection; 4 prepare + restore failure; 5 teardown;
        // 6 unresolved driver; 7 candidate callback failure; 8 restore callback failure;
        // 9 cancellation followed by an actual restore failure.
        Controls oldControls, newControls, subsequentControls;
        if (scenario == 7 || scenario == 8) newControls.failAttach.store(true);
        SingleTrackDocument document;
        auto engine = std::make_unique<AudioEngine>(document);
        AudioEngineDeviceTestAccess::install(*engine, std::make_unique<FixtureManager>(oldControls));
        auto* manager = engine->getDeviceManagerForConfiguration();
        require(engine->loadFile(artifacts.getChildFile("physical-route-source.wav")), "switch coordinator retains a real editable Document");
        engine->setMonitorGainLinear(1.0f); engine->seekToSeconds(0.005);
        if (scenario == 8)
        {
            auto oldLease = engine->takeDeviceLeaseForSwitch();
            require(oldLease && oldLease->closeForSwitch(), "prepare direct last-known-good fixture");
            std::unique_ptr<DeviceOutput> oldOutput = std::make_unique<FixtureOutput>(oldControls);
            require(AudioEngineDeviceTestAccess::commit(*engine, oldLease, oldOutput), "install the actual direct old owner");
            oldLease.reset();
        }
        auto selected = selection();
        juce::WaitableEvent entered, release;
        std::atomic<int> prepared {0};
        std::atomic<FixtureOutput*> view {nullptr};
        DeviceSwitchAccess access;
        const auto ownerThread = std::this_thread::get_id();
        std::atomic<bool> workerThread {false}, restoreAfterDisposal {true};
        oldControls.duringOpen = [&] { if (newControls.aliveDevices.load() != 0) restoreAfterDisposal.store(false); };
        std::shared_ptr<Switch> operation;
        struct Drain
        {
            std::function<void()> action;
            ~Drain() { action(); }
        } drain { [&]
        {
            release.signal();
            if (!operation) return;
            operation->cancel();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8000;
            while (!operation->snapshot().resourcesReleased && juce::Time::getMillisecondCounterHiRes() < deadline)
            { if (engine) operation->poll(*engine, selected); else operation->abandon(); juce::Thread::sleep(1); }
        } };
        if (scenario == 0) engine->play();
        const auto opensBefore = oldControls.opens.load();
        operation = Switch::start(*engine, selected, scenario == 0 ? Switch::Mode::parallel : Switch::Mode::exclusive,
            [&](const auto&, const DeviceSwitchAccess& grant, const std::atomic<bool>&) -> Switch::Prepared
            {
                ++prepared; workerThread.store(std::this_thread::get_id() != ownerThread);
                access = grant; entered.signal(); release.wait(12000);
                if (scenario == 4) return {{}, Switch::Error::prepareFailed};
                auto output = std::make_unique<FixtureOutput>(newControls); view.store(output.get());
                return {std::move(output), Switch::Error::none};
            });
        require(entered.wait(2000) && workerThread.load() && prepared == 1, "driver candidate preparation runs once off the owner thread");
        const auto pendingRequest = engine->getDeviceDiagnostics().latestRequest;
        require(pendingRequest && pendingRequest->direct && pendingRequest->kind == DeviceRequestKind::activateOutput
            && pendingRequest->outcome == DeviceRequestOutcome::pending && pendingRequest->requestedRate == 48000
            && pendingRequest->requestedBuffer == 128 && pendingRequest->requestedOutputChannels == 2
            && !pendingRequest->resourcesReleased, "live direct request exists while the candidate driver is still pending");
        const auto pendingFields = snapshot(*engine, document)["fields"];
        require(pendingFields["negotiationRequestKind"]["value"].toString() == "direct-activate"
            && pendingFields["negotiationResources"]["value"].toString() == "pending"
            && pendingFields["negotiationOutcome"]["value"].toString() == "pending",
            "pending request serializes even during the real exclusive device lease");
        require(access.isValid() == (scenario != 0), "only the actual closed exclusive lease issues a driver window");
        auto invalidSelection = selected; invalidSelection.plan.reset();
        const auto rejectPrepare = [&](const auto&, const auto&, const auto&) -> Switch::Prepared { ++prepared; return {}; };
        const auto invalid = Switch::start(*engine, invalidSelection, Switch::Mode::parallel, rejectPrepare);
        const auto busy = Switch::start(*engine, selected, Switch::Mode::parallel, rejectPrepare);
        require(invalid->snapshot().error == Switch::Error::invalidSelection && invalid->snapshot().resourcesReleased
            && busy->snapshot().error == Switch::Error::engineBusy && busy->snapshot().resourcesReleased && prepared == 1,
            "static invalid selection and a concurrent operation fail before invoking another backend or replacing the active operation");
        const auto rejectedRequest = engine->getDeviceDiagnostics().latestRequest;
        require(rejectedRequest && rejectedRequest->operationId != pendingRequest->operationId
            && rejectedRequest->switchError == Switch::Error::engineBusy && rejectedRequest->resourcesReleased,
            "a rejected newer intent has its own request ID without changing the in-flight driver operation");
        if (scenario == 0)
        {
            auto* device = dynamic_cast<FixtureDevice*>(manager->getCurrentAudioDevice());
            require(engine->isPlaying() && device->renderOutputs().getMagnitude(0, 128) > 0.05f && oldControls.opens == opensBefore,
                "parallel candidate preparation leaves the old actual output playing without reopening it");
            std::jthread callbacks([&](std::stop_token stop) { while (!stop.stop_requested()) { device->renderOutputs(); juce::Thread::sleep(1); } });
            engine->pause(); callbacks.request_stop(); callbacks.join();
        }
        else
        {
            require(engine->isDeviceLeasedForProbe() && !engine->getRuntimeMetricsForDiagnostics(), "exclusive preparation honestly suspends output");
            engine->play(); engine->seekToSeconds(0.012); // latest intent must survive prepare
        }
        if (scenario == 2 || scenario == 9) operation->cancel();
        if (scenario == 3) { ++selected.generation; operation->poll(*engine, selected); }
        if (scenario == 4 || scenario == 9) oldControls.failNext.store(true);
        if (scenario == 8) oldControls.failAttach.store(true);
        if (scenario == 5) { engine.reset(); require(!access.isValid(), "engine teardown cancels its operation and revokes copied switch credentials"); }
        if (scenario == 6)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 6500;
            while (!operation->snapshot().driverUnresolved && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(5);
            operation->poll(*engine, selected);
            require(operation->snapshot().driverUnresolved && operation->snapshot().error == Switch::Error::driverUnresolved
                && !operation->snapshot().resourcesReleased, "unreturned driver is exposed as unresolved, never falsely reported released or restored");
        }
        release.signal();
        if (scenario == 0)
        {
            const auto readyDeadline = juce::Time::getMillisecondCounterHiRes() + 2000;
            while (operation->snapshot().phase != Switch::Phase::readyToSwitch && juce::Time::getMillisecondCounterHiRes() < readyDeadline) juce::Thread::sleep(1);
            SingleTrackDocument otherDocument; AudioEngine otherEngine(otherDocument);
            operation->poll(otherEngine, selected);
            require(operation->snapshot().phase == Switch::Phase::readyToSwitch && !otherEngine.isDeviceLeasedForProbe()
                && !otherEngine.getRuntimeMetricsForDiagnostics(), "a ready parallel candidate cannot be admitted or committed by another AudioEngine");
        }
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8000;
        while (!operation->snapshot().resourcesReleased && juce::Time::getMillisecondCounterHiRes() < deadline)
        { if (engine) operation->poll(*engine, selected); juce::Thread::sleep(1); }
        const auto result = operation->snapshot();
        require(result.resourcesReleased && !access.isValid() && restoreAfterDisposal.load(), "worker drains rejected candidates before restore and revokes every completed window");
        if (engine)
        {
            operation->poll(*engine, selected);
            const auto retained = engine->getDeviceDiagnostics();
            require(retained.latestRequest && retained.latestRequest->operationId == rejectedRequest->operationId,
                "late completion of an older operation cannot replace the latest requested intent");
            const auto failedOlderOperation = scenario == 4 || scenario >= 6;
            require(retained.latestFailure && retained.latestFailure->operationId
                == (failedOlderOperation ? pendingRequest->operationId : rejectedRequest->operationId),
                "a genuine later failure is retained independently from the latest request");
            if (failedOlderOperation)
                require(retained.latestFailure->switchError == result.error && retained.latestFailure->resourcesReleased
                    && retained.latestFailure->restored == result.restored
                    && retained.latestFailure->failurePhase != Switch::Phase::failed,
                    "failure retains its originating phase and actual final restoration after worker release");
            const auto captured = snapshot(*engine, document, "audio", true);
            require(captured["captureStatus"].toString() != "capture-failed"
                && captured["fields"]["lastNegotiationFailureSource"]["value"].toString() == "device-switch-coordinator",
                "direct outcome and native metadata remain within the shared snapshot field budget");
            if (scenario == 9)
                require(captured["fields"]["lastNegotiationFailure"]["value"].toString() == "restore-failed"
                    && captured["fields"]["lastNegotiationFailureStage"]["value"].toString() == "restoring"
                    && captured["fields"]["lastNegotiationFailureRestore"]["value"].toString() == "failed",
                    "an actual failed restore after cancellation is reported as a failure, not a successful abort");
            require(artifacts.getChildFile("direct-request-" + juce::String(scenario) + ".json").replaceWithText(juce::JSON::toString(captured, true)),
                "retain serialized real pending/failure/restoration evidence");
            std::weak_ptr<Switch> released = operation; operation.reset();
            require(released.expired() && engine->getDeviceDiagnostics().latestRequest->operationId == rejectedRequest->operationId,
                "retained request values never retain the finished coordinator or driver worker");
        }
        if (scenario < 2)
        {
            require(result.phase == Switch::Phase::active && result.error == Switch::Error::none
                && engine->isPlaying() == (scenario == 1) && !engine->isDeviceLeasedForProbe(), "successful transaction preserves latest play or pause intent");
            if (scenario == 1) { view.load()->render(); require(view.load()->render().getMagnitude(1, 0, 128) > 0.1f, "committed asynchronous output actually renders the Document"); }
            if (scenario == 1)
            {
                const auto failureId = engine->getDeviceDiagnostics().latestFailure->operationId;
                selected.expected.sampleRate = 96000; selected.expected.bufferSamples = 256;
                auto nextDraft = DeviceRoutePlan::propose(selected.bus, selected.expected);
                nextDraft.outputsBySpeaker = {{3}, {}, {1}};
                selected.plan = DeviceRoutePlan::validate(nextDraft, selected.expected, selected.bus).approved();
                entered.reset(); release.reset();
                operation = Switch::start(*engine, selected, Switch::Mode::parallel,
                    [&](const auto& choice, const auto&, const auto&) -> Switch::Prepared
                    {
                        entered.signal(); release.wait(12000);
                        return {std::make_unique<FixtureOutput>(subsequentControls, choice.expected), Switch::Error::none};
                    });
                require(entered.wait(2000), "next request is pending while the previous direct owner remains active");
                const auto preparing = snapshot(*engine, document)["fields"];
                require(static_cast<double>(preparing["requestedRate"]["value"]) == 96000
                    && static_cast<double>(preparing["activeRate"]["value"]) == 48000,
                    "requested clock remains distinct from the actual still-playing device clock");
                release.signal();
                const auto finish = juce::Time::getMillisecondCounterHiRes() + 8000;
                while (!operation->snapshot().resourcesReleased && juce::Time::getMillisecondCounterHiRes() < finish)
                { operation->poll(*engine, selected); juce::Thread::sleep(1); }
                require(operation->snapshot().resourcesReleased && operation->snapshot().phase == Switch::Phase::active,
                    "new direct request actually activates and releases its operation resources");
                operation->poll(*engine, selected);
                std::weak_ptr<Switch> released = operation; operation.reset();
                const auto completed = snapshot(*engine, document)["fields"];
                require(released.expired() && completed["negotiationOutcome"]["value"].toString() == "succeeded"
                    && completed["negotiationResources"]["value"].toString() == "released"
                    && static_cast<double>(completed["activeRate"]["value"]) == 96000
                    && engine->getDeviceDiagnostics().latestFailure->operationId == failureId,
                    "successful later activation retains exact request/actual facts and the independent earlier failure");
            }
        }
        else if (scenario == 4 || scenario == 8 || scenario == 9)
        {
            require(result.phase == Switch::Phase::failed && result.restorationAttempted && !result.restored
                && !engine->getRuntimeMetricsForDiagnostics() && !engine->isPlaying(), "failed driver reopen or callback reattachment reports an unavailable restoration");
            const auto opens = oldControls.opens.load(); engine->play(); engine->warmUpAudioDeviceAsync();
            require(oldControls.opens == opens && document.applySelectionGain({0.0, 0.02}, 0.5f) && document.undo(),
                "no implicit fallback; editing and Undo remain available without output");
        }
        else if (scenario == 5)
            require(newControls.aliveDevices == 0 && oldControls.aliveDevices == 0 && oldControls.opens == opensBefore,
                "abandoned operation disposes late output and old owner on its worker without reopening either");
        else
            require(result.phase == Switch::Phase::failed && result.restored && engine->isPlaying()
                && std::abs(engine->getCurrentPositionSeconds() - 0.012) < 0.00001 && newControls.aliveDevices == 0,
                "cancelled, stale and late driver results restore the exact old output and latest transport intent");
    }
    if (lifecycleOnly)
        std::cout << "DeviceSwitchCoordinator lifecycle subset passed: existing parallel success and late cancellation flows; controlled local IODevice only\n";
    else
        std::cout << "DeviceSwitchCoordinator passed: asynchronous parallel/exclusive, stale/cancel/restore failure, teardown, unresolved-driver truth and real playback\n";
}

void runCapabilityInspectionTests(const juce::File& artifacts)
{
    using Switch = DeviceSwitchCoordinator;
    using Adapter = JuceAsioBackendAdapter;
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        // Success, late cancel, stale choice, failed restoration, teardown,
        // and restoring the original absence of a device.
        Controls oldControls, temporaryControls;
        SingleTrackDocument document;
        auto engine = std::make_unique<AudioEngine>(document);
        if (scenario != 5)
        {
            AudioEngineDeviceTestAccess::install(*engine, std::make_unique<FixtureManager>(oldControls));
            engine->getDeviceManagerForConfiguration();
        }
        require(engine->loadFile(artifacts.getChildFile("physical-route-source.wav")), "inspection retains real Document");
        const auto original = document.getState();
        Switch::InspectionSelection selected { 73, "ASIO", "asio:{00000000-0000-0000-0000-000000000001}" };
        juce::WaitableEvent entered, release;
        std::atomic<bool> ordered {true}, offOwner {false};
        const auto ownerThread = std::this_thread::get_id();
        DeviceSwitchAccess copiedAccess;
        oldControls.duringOpen = [&] { if (temporaryControls.aliveDevices.load() != 0) ordered.store(false); };
        std::shared_ptr<Switch> operation;
        struct Drain
        {
            std::function<void()> action;
            ~Drain() { action(); }
        } drain { [&]
        {
            release.signal();
            if (!operation) return;
            operation->cancel();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
            while (!operation->snapshot().resourcesReleased && juce::Time::getMillisecondCounterHiRes() < deadline)
            { if (engine) operation->pollInspection(*engine, selected); else operation->abandon(); juce::Thread::sleep(1); }
        } };
        operation = Switch::startInspection(*engine, selected,
            [&](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Inspected
            {
                copiedAccess = access;
                offOwner.store(std::this_thread::get_id() != ownerThread);
                const std::vector<AsioDriverRegistration> registry {
                    {"Private Fixture Speakers", "{00000000-0000-0000-0000-000000000001}", "Selected", "Selected.dll"}
                };
                auto adapter = JuceAsioBackendTestAccess::make(
                    [&] { return std::make_unique<FixtureType>(temporaryControls, "ASIO"); },
                    [&] { return std::optional{registry}; },
                    [&](juce::AudioIODeviceType*, const AsioDriverRegistration&)
                    {
                        auto device = std::make_unique<FixtureDevice>(temporaryControls);
                        entered.signal(); release.wait(8000);
                        return device;
                    });
                const auto& catalog = adapter->scanDevices(Adapter::Activation::userSelection);
                auto result = adapter->inspectCapabilities(catalog.generation, choice.stableId, access, cancelled);
                if (temporaryControls.aliveDevices.load() != 0) ordered.store(false);
                if (!result.capabilities) return {{}, cancelled.load() ? Switch::Error::cancelled : Switch::Error::prepareFailed};
                return { std::make_shared<const Switch::InspectedCapabilities>(std::move(*result.capabilities)), Switch::Error::none };
            });
        require(entered.wait(2000) && copiedAccess.isValid() && offOwner.load() && engine->isDeviceLeasedForProbe(),
            "real engine lease grants inspection only after old output closure, on a worker");
        require(!operation->inspection(selected) && temporaryControls.aliveDevices == 1,
            "capabilities stay unpublished while driver work remains live");
        if (scenario != 5) { engine->play(); engine->seekToSeconds(0.012); }
        if (scenario == 1) operation->cancel();
        if (scenario == 2) { ++selected.generation; operation->pollInspection(*engine, selected); }
        if (scenario == 3) oldControls.failNext.store(true);
        if (scenario == 4) engine.reset();
        release.signal();
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        while (!operation->snapshot().resourcesReleased && juce::Time::getMillisecondCounterHiRes() < deadline)
        { if (engine) operation->pollInspection(*engine, selected); juce::Thread::sleep(1); }
        const auto result = operation->snapshot();
        require(result.resourcesReleased && !copiedAccess.isValid() && temporaryControls.aliveDevices == 0 && ordered.load()
            && temporaryControls.opens == 0 && document.getState() == original,
            "inspection closes before restoration, never starts candidate playback, revokes window and preserves Document");
        if (scenario == 0 || scenario == 5)
        {
            const auto facts = operation->inspection(selected);
            auto stale = selected; ++stale.generation;
            require(result.phase == Switch::Phase::readyForReview && result.restored && !result.outputSuspended
                && facts && facts->stableId == selected.stableId && facts->capabilities.outputNames.size() == 4
                && !operation->inspection(stale) && !engine->isDeviceLeasedForProbe(),
                "review becomes available only after exact old-owner return and matches current selection");
            if (scenario == 0)
                require(engine->isPlaying() && std::abs(engine->getCurrentPositionSeconds() - 0.012) < 1.0e-5,
                    "inspection returns latest play/seek intent before user review");
            else require(!engine->getRuntimeMetricsForDiagnostics() && oldControls.opens == 0,
                "cold inspection restores original no-output state without selecting a fallback");
        }
        else
        {
            require(!operation->inspection(selected) && result.phase == Switch::Phase::failed,
                "cancelled, stale, failed-restore or abandoned inspection exposes no review result");
            if (scenario == 1 || scenario == 2)
                require(result.restored && engine->isPlaying(), "cancel and stale selection still restore latest transport intent");
            if (scenario == 3) require(!result.restored && !engine->getRuntimeMetricsForDiagnostics(), "restore failure stays unavailable");
        }
    }
    std::cout << "Capability inspection passed: real lease plus adapter, close-before-restore, publication fence, cancel/stale/restore failure/teardown/cold state\n";
}

juce::var snapshot(AudioEngine& engine, SingleTrackDocument& document, const char* section, bool nativeMetadata)
{
    auto captured = diagnostics::captureLiveDiagnosticProviders(engine, document, nullptr);
    if (nativeMetadata)
    {
        std::atomic<bool> cancelled { false };
        diagnostics::enrichNativeDeviceMetadata(captured, cancelled);
    }
    std::vector<diagnostics::LoadedProviderRef> providers;
    for (const auto& provider : captured) providers.push_back({ provider.kind, &provider });
    const diagnostics::BundleIdentity identity { "0.3.0", "device-fixture", juce::String::repeatedString("a", 64),
        "Release", "test", "x64", "x64", "test", false };
    const auto prepared = diagnostics::DiagnosticSnapshot::capture(identity, "test-fixture", providers);
    require(static_cast<bool>(prepared.status()), "live device evidence is accepted by the product snapshot schema");
    for (const auto& entry : prepared.content().entries)
        if (entry.id == juce::String("providers/") + section + ".json")
        {
            const auto text = entry.bytes.toString();
            require(!text.contains("Private Fixture Speakers") && !text.contains("fixture-user") && !text.contains("Private driver details"),
                "default captured device evidence excludes names and private raw driver messages");
            return juce::JSON::parse(text);
        }
    throw std::runtime_error("missing requested loaded provider");
}
juce::ComboBox* findBox(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id) return dynamic_cast<juce::ComboBox*>(&component);
    for (auto* child : component.getChildren()) if (auto* found = findBox(*child, id)) return found;
    return nullptr;
}
void selectItem(juce::ComboBox& box, const juce::String& prefix)
{
    for (int i = 0; i < box.getNumItems(); ++i)
        if (box.getItemText(i).startsWith(prefix)) { box.setSelectedItemIndex(i, juce::sendNotificationSync); return; }
    throw std::runtime_error("required controlled device option absent from real Preferences component");
}
juce::Button* findButton(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id) return dynamic_cast<juce::Button*>(&component);
    for (auto* child : component.getChildren()) if (auto* found = findButton(*child, id)) return found;
    return nullptr;
}
void runWindowsOutputSettingsTests(const juce::File& artifacts)
{
    using Session = AsioSettingsSession;
    using Switch = DeviceSwitchCoordinator;
    using Adapter = JuceAsioBackendAdapter;
   #if JUCE_WINDOWS && SINGLE_EDITOR_WASAPI_DIAGNOSTIC_BINDING
    for (const auto* backend : {"Windows Audio", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)"})
    {
        WindowsOutputBackend native(backend); const auto catalog = native.discover();
        require(catalog.status == WindowsOutputBackend::Status::ready, "production WASAPI catalog bridge reads this machine's exact endpoint identities");
        size_t defaults = 0;
        for (const auto& output : catalog.outputs)
        {
            require(output.stableId.isNotEmpty() && output.stableId.length() <= 2048, "actual WASAPI catalog provides bounded endpoint identities");
            if (output.isDefault) ++defaults;
        }
        require(defaults <= 1, "actual WASAPI mode exposes at most one default output");
        std::cout << "Windows catalog bridge: " << backend << " endpoints=" << catalog.outputs.size() << " defaults=" << defaults << " (no stream opened)\n";
    }
   #endif
    Controls oldControls, asioControls, windowsControls;
    SingleTrackDocument document; AudioEngine engine(document);
    AudioEngineDeviceTestAccess::install(engine, std::make_unique<FixtureManager>(oldControls));
    engine.getDeviceManagerForConfiguration();
    require(engine.loadFile(artifacts.getChildFile("physical-route-source.wav"))
        && document.applySelectionGain({0, .04}, .5f) && document.undo() && document.canRedo(), "Windows switch retains real editable audio and history");
    engine.setMonitorGainLinear(1); engine.play(); engine.seekToSeconds(.01);
    const auto original = document.getState();
    juce::ApplicationProperties settings;
    juce::PropertiesFile::Options options;
    options.applicationName = "WindowsOutputSettingsTest"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("windows-output-private").getFullPathName();
    settings.setStorageParameters(options);
    require(settings.getUserSettings()->getFile().isAChildOf(artifacts), "Windows switch settings are isolated");
    const std::string asioId = "asio:controlled-output", endpoint = "windows:explicit-default";
    std::atomic<bool> wrongIdentity {false}, failAsio {false};
    std::atomic<int> discoveries {0};
    FixtureDevice* windowsDevice = nullptr;
    const auto makeWindows = [&](const std::string& backend)
    {
        return WindowsOutputBackendTestAccess::make(
            [&, backend] { return std::make_unique<FixtureType>(windowsControls, juce::String(backend)); },
            [&](const juce::AudioIODeviceType*) -> std::optional<std::vector<WindowsOutputEndpoint>>
            { return std::vector<WindowsOutputEndpoint>{{"Identical display name", "windows:not-selected", false}, {"Identical display name", juce::String(endpoint), true}}; },
            [&, backend](juce::AudioIODeviceType*, const juce::String& id) -> std::unique_ptr<juce::AudioIODevice>
            {
                require(id == juce::String(endpoint), "Windows construction receives exact selected endpoint despite duplicate names");
                auto device = std::make_unique<FixtureDevice>(windowsControls, juce::String(backend)); windowsDevice = device.get(); return device;
            },
            [&](const juce::AudioIODevice*) { return wrongIdentity.load() ? juce::String("windows:different") : juce::String(endpoint); });
    };
    {
        auto unleased = makeWindows("Windows Audio"); std::atomic<bool> cancelled{false};
        require(unleased->inspect(endpoint, {}, cancelled).status == WindowsOutputBackend::Status::accessNotGranted
            && windowsControls.scans == 0 && windowsControls.aliveDevices == 0, "Windows inspection without the old-owner grant does not enter native discovery or construct a device");
    }
    Switch::Prepare prepare = [&](const Switch::Selection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Prepared
    {
        require(access.isValid(), "selected device preparation holds actual old-owner exclusive access");
        if (choice.expected.backend == "ASIO")
            return failAsio.exchange(false) ? Switch::Prepared{{}, Switch::Error::prepareFailed}
                : Switch::Prepared{std::make_unique<FixtureOutput>(asioControls, choice.expected), Switch::Error::none};
        auto backend = makeWindows(choice.expected.backend);
        auto result = backend->prepare(choice.expected, access, cancelled);
        return {std::move(result.output), result.status == WindowsOutputBackend::Status::ready ? Switch::Error::none : Switch::Error::prepareFailed};
    };
    auto session = AsioSettingsTestAccess::makeForBackends(engine, settings,
        [&](const std::string& backend) -> Session::Catalog
        {
            ++discoveries;
            if (backend == "ASIO") return Adapter::Catalog{Adapter::Status::available, 1, {{"Controlled ASIO", asioId}}};
            auto adapter = makeWindows(backend); const auto found = adapter->discover();
            require(found.status == WindowsOutputBackend::Status::ready, "Windows catalog copied without a device");
            Session::Catalog result; result.status = Adapter::Status::available;
            for (const auto& output : found.outputs)
            {
                result.drivers.push_back({output.displayName.toStdString(), output.stableId.toStdString()});
                if (output.isDefault) result.defaultOutputId = output.stableId.toStdString();
            }
            return result;
        },
        [&](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Inspected
        {
            require(access.isValid(), "Windows capability inspection holds real exclusive grant");
            if (isWindowsOutputBackend(choice.backend))
            {
                auto backend = makeWindows(choice.backend); auto result = backend->inspect(choice.stableId, access, cancelled);
                require(windowsControls.aliveDevices <= 1, "temporary Windows inspection releases its device before returning values");
                return result.capabilities ? Switch::Inspected{std::make_shared<const PreparedDeviceCapabilities>(std::move(*result.capabilities)), Switch::Error::none}
                    : Switch::Inspected{{}, Switch::Error::prepareFailed};
            }
            auto facts = std::make_shared<PreparedDeviceCapabilities>(); facts->stableId = choice.stableId; facts->catalogGeneration = 1;
            facts->capabilities.outputNames = {"1", "2", "3", "4"}; facts->capabilities.sampleRates = {48000}; facts->capabilities.bufferSizes = {128};
            facts->capabilities.reportedSampleRate = 48000; facts->capabilities.reportedBufferSize = 128;
            return {facts, Switch::Error::none};
        }, prepare);
    auto drain = [&]
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(1); }
        session->sendSynchronousChangeMessage();
        require(!session->busy() && !engine.isDeviceLeasedForProbe(), "Windows operation returns its exact lease before fixture deadline");
    };
    struct DrainOnExit
    {
        std::shared_ptr<Session> session; Controls& controls;
        ~DrainOnExit()
        {
            controls.release.signal(); session->cancel();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
            while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(1); }
            session->shutdown();
        }
    } cleanup{session, windowsControls};
    AsioSettingsComponent panel(session); panel.addToDesktop(juce::ComponentPeer::windowHasTitleBar); panel.setVisible(true);
    const auto click = [&](const char* id)
    {
        auto* button = findButton(panel, id); require(button && button->isEnabled() && button->onClick, "Windows output action enabled");
        button->onClick(); session->sendSynchronousChangeMessage();
    };
    const auto route = [&]
    {
        click("asio.inspect"); drain(); require(session->capabilities(), "selected backend returned live capabilities");
        int stereo = -1;
        for (size_t i = 0; i < session->profiles().size(); ++i)
            if (const auto& bus = session->profiles()[i].bus; bus.speakers.size() == 2 && bus.speakers[0].role == SpeakerRole::L && bus.speakers[1].role == SpeakerRole::R) stereo = static_cast<int>(i);
        findBox(panel, "asio.profile")->setSelectedId(stereo + 1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
        require(session->draft().outputsBySpeaker == std::vector<std::vector<int>>{{}, {}}, "Windows output names do not guess physical speaker roles");
        for (const auto* id : {"asio.matrix.0.1", "asio.matrix.1.3"})
        {
            auto* cell = findButton(panel, id); require(cell && cell->onClick, "shared matrix exposes selected physical cells");
            cell->setToggleState(true, juce::dontSendNotification); cell->onClick(); session->sendSynchronousChangeMessage();
        }
        click("asio.validate"); require(session->review() && session->review()->isValid(), "shared matrix validates Windows route");
    };
    require(discoveries == 0 && windowsControls.scans == 0 && windowsControls.aliveDevices == 0, "backend UI construction stays lazy");
    click("asio.discover"); drain(); findBox(panel, "asio.driver")->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    route(); click("asio.activate"); drain();
    const auto asioRoute = AudioEngineDeviceTestAccess::route(engine);
    const auto asioSaved = settings.getUserSettings()->getValue(Session::bindingKey);
    require(asioRoute && asioRoute->device().backend == "ASIO", "controlled ASIO route active before Windows selection");
    click("audio-output.windows-default"); drain();
    require(session->selectedBackend() == "Windows Audio" && session->selectedDriver() == endpoint
        && windowsControls.opens == 0 && windowsControls.aliveDevices == 0 && AudioEngineDeviceTestAccess::route(engine) == asioRoute,
        "explicit Windows default discovers exact endpoint without constructing a stream or changing active ASIO");
    route();
    for (int failure = 0; failure < 4; ++failure)
    {
        if (failure == 0) windowsControls.failNext = true;
        if (failure == 1) windowsControls.coerceNext = true;
        if (failure == 2) wrongIdentity = true;
        if (failure == 3) windowsControls.failAttach = true;
        click("asio.activate"); drain(); wrongIdentity = false; windowsControls.failAttach = false;
        require(AudioEngineDeviceTestAccess::route(engine) == asioRoute && engine.isPlaying() && windowsControls.aliveDevices == 0
            && settings.getUserSettings()->getValue(Session::activeBackendKey) == "ASIO"
            && !settings.getUserSettings()->containsKey(Session::windowsBindingKey), "failed Windows open/mask/identity/attach restores ASIO and preserves saved selection");
    }
    windowsControls.blockNext = true; click("asio.activate");
    require(windowsControls.entered.wait(2000), "Windows open is held on worker");
    click("asio.cancel"); engine.seekToSeconds(.025); windowsControls.release.signal(); drain();
    require(AudioEngineDeviceTestAccess::route(engine) == asioRoute && engine.isPlaying()
        && std::abs(engine.getCurrentPositionSeconds() - .025) < 1.e-5 && windowsControls.aliveDevices == 0, "cancelled Windows activation returns original ASIO and latest seek");
    click("asio.activate"); drain();
    auto windowsRoute = AudioEngineDeviceTestAccess::route(engine);
    require(windowsRoute && windowsRoute->device().backend == "Windows Audio" && windowsRoute->device().stableId == endpoint
        && engine.isSelectedOutputReady() && windowsControls.aliveDevices == 1 && document.getState() == original && document.canRedo(), "actual Windows adapter commits without changing Document history");
    engine.seekToSeconds(.01); windowsDevice->renderOutputs(); const auto samples = windowsDevice->renderOutputs();
    require(samples.getMagnitude(0, 0, 128) == 0 && samples.getMagnitude(2, 0, 128) == 0
        && samples.getMagnitude(1, 0, 128) > .1f && samples.getMagnitude(3, 0, 128) > .2f, "actual Windows callback plays canonical stereo on selected physical outputs 2/4");
    const auto windowsSaved = settings.getUserSettings()->getValue(Session::windowsBindingKey);
    require(settings.getUserSettings()->getValue(Session::bindingKey) == asioSaved
        && settings.getUserSettings()->getValue(Session::activeBackendKey) == "Windows Audio"
        && juce::JSON::parse(settings.getUserSettings()->getValue(Session::deviceBindingsKey)).size() == 2, "successful Windows commit retains ASIO and separate exact-device records");
    const auto picture = panel.createComponentSnapshot(panel.getLocalBounds());
    auto imageFile = artifacts.getChildFile("windows-output-settings.png").createOutputStream(); juce::PNGImageFormat png;
    require(imageFile && png.writeImageToStream(picture, *imageFile), "capture actual Windows routing controls"); imageFile.reset();
    // Selecting ASIO cannot overwrite the persisted active Windows route. A
    // failed return exercises WindowsDeviceOutput::restore and callback attach.
    findBox(panel, "audio-output.backend")->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    click("asio.discover"); drain(); findBox(panel, "asio.driver")->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    route(); failAsio = true; click("asio.activate"); drain();
    require(AudioEngineDeviceTestAccess::route(engine) == windowsRoute && engine.isSelectedOutputReady() && engine.isPlaying()
        && settings.getUserSettings()->getValue(Session::activeBackendKey) == "Windows Audio", "failed ASIO return reopens exact Windows owner and keeps last successful preference");
    click("asio.retry-saved"); drain();
    require(session->selectedBackend() == "Windows Audio" && session->drivers().drivers.empty() && !session->capabilities()
        && engine.isSelectedOutputReady() && settings.getUserSettings()->getValue(Session::windowsBindingKey) == windowsSaved,
        "retry saved Windows while browsing ASIO retires the other backend's catalog without fabricating live facts");
    findBox(panel, "audio-output.backend")->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    click("asio.discover"); drain(); findBox(panel, "asio.driver")->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    route();
    click("asio.activate"); drain();
    require(AudioEngineDeviceTestAccess::route(engine)->device().backend == "ASIO" && windowsControls.aliveDevices == 0
        && settings.getUserSettings()->getValue(Session::windowsBindingKey) == windowsSaved, "explicit ASIO return releases Windows output and retains its saved binding");
    for (const auto* backend : {"Windows Audio", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)"})
    {
        SingleTrackDocument coldDocument; AudioEngine coldEngine(coldDocument);
        require(coldEngine.reserveExplicitStartupOutput(), "saved Windows startup reserves explicit output");
        juce::ApplicationProperties coldSettings; auto coldOptions = options; coldOptions.applicationName = "WindowsStartup" + juce::String::toHexString(juce::String(backend).hashCode()); coldSettings.setStorageParameters(coldOptions);
        auto record = juce::JSON::parse(windowsSaved); record.getDynamicObject()->setProperty("backend", backend);
        coldSettings.getUserSettings()->setValue(Session::windowsBindingKey, juce::JSON::toString(record, true));
        coldSettings.getUserSettings()->setValue(Session::activeBackendKey, backend);
        auto cold = AsioSettingsTestAccess::make(coldEngine, coldSettings, [] { return Adapter::Catalog{}; }, {}, prepare);
        cold->restoreSavedSelection();
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        while (cold->needsPoll() && juce::Time::getMillisecondCounterHiRes() < deadline) { cold->poll(); juce::Thread::sleep(1); }
        require(!cold->needsPoll() && coldEngine.isSelectedOutputReady() && AudioEngineDeviceTestAccess::route(coldEngine)->device().backend == backend,
            "saved Windows mode reconstructs exact endpoint through real adapter and engine");
    }
    require(windowsControls.aliveDevices == 0 && document.getState() == original && document.canRedo(), "Windows startup/switch cleanup preserves original edit history");
    std::cout << "Windows output settings passed: explicit default, exact endpoint despite duplicate names, real adapter inspection/activation/callback, failed open/mask/identity/attach rollback, cancelled latest seek, ASIO return, per-device persistence and three saved modes\n";
}
void runAsioSettingsTests(const juce::File& artifacts)
{
    using Adapter = JuceAsioBackendAdapter;
    using Switch = DeviceSwitchCoordinator;
    Controls oldControls, candidateControls;
    SingleTrackDocument document; AudioEngine engine(document);
    AudioEngineDeviceTestAccess::install(engine, std::make_unique<FixtureManager>(oldControls));
    engine.getDeviceManagerForConfiguration();
    require(engine.loadFile(artifacts.getChildFile("physical-route-source.wav")), "settings use real loaded Document");
    const auto original = document.getState();
    juce::ApplicationProperties settings;
    juce::PropertiesFile::Options options;
    options.applicationName = "AsioSettingsTest"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("asio-settings-private").getFullPathName();
    settings.setStorageParameters(options);
    require(settings.getUserSettings()->getFile().isAChildOf(artifacts), "settings test owns its persistence file");
    const std::string id = "asio:{00000000-0000-0000-0000-000000000001}";
    std::atomic<int> discoveries {0}, inspections {0}, preparations {0};
    std::atomic<bool> holdInspection {false}, coerce {false};
    juce::WaitableEvent inspectionEntered, releaseInspection;
    auto session = AsioSettingsTestAccess::make(engine, settings,
        [&] { ++discoveries; return Adapter::Catalog {Adapter::Status::available, 1, {{"Controlled ASIO (test fixture)", id}}}; },
        [&](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Inspected
        {
            ++inspections;
            if (!access.isValid()) return {{}, Switch::Error::prepareFailed};
            if (holdInspection.exchange(false)) { inspectionEntered.signal(); releaseInspection.wait(6000); }
            if (cancelled.load()) return {{}, Switch::Error::cancelled};
            auto facts = std::make_shared<Adapter::PreparedCapabilities>();
            facts->stableId = choice.stableId; facts->catalogGeneration = 1;
            facts->capabilities.outputNames = {"Analog 1", "Analog 2", "Analog 3", "Analog 4"};
            facts->capabilities.sampleRates = {48000, 96000}; facts->capabilities.bufferSizes = {128, 256};
            facts->capabilities.reportedSampleRate = 48000; facts->capabilities.reportedBufferSize = 128;
            return {facts, Switch::Error::none};
        },
        [&](const Switch::Selection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Prepared
        {
            ++preparations;
            if (!access.isValid() || cancelled.load()) return {{}, Switch::Error::cancelled};
            auto requested = choice.expected;
            if (coerce.exchange(false)) requested.outputNames[0] = "Changed endpoint";
            return {std::make_unique<FixtureOutput>(candidateControls, std::move(requested)), Switch::Error::none};
        });
    auto drain = [&]
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        // A visible output UI continues polling rate/latency after the device
        // operation has returned its lease; needsPoll is not a lifetime test.
        while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(1); }
        session->sendSynchronousChangeMessage();
        require(!session->busy() && !engine.isDeviceLeasedForProbe(), "settings operation releases resources within fixture deadline");
    };
    struct DrainOnExit
    {
        std::shared_ptr<AsioSettingsSession> session; juce::WaitableEvent& release;
        ~DrainOnExit()
        {
            release.signal(); session->cancel();
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
            while (session->busy() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(1); }
            session->shutdown();
        }
    } cleanup {session, releaseInspection};
    auto panel = std::make_unique<AsioSettingsComponent>(session);
    panel->addToDesktop(juce::ComponentPeer::windowHasTitleBar); panel->setVisible(true);
    int openedSettings = 0;
    {
        PreferencesComponent preferences(engine, settings, [&] { ++openedSettings; });
        auto* entry = findButton(preferences, "audio-device.asio-settings");
        require(entry && entry->isVisible() && entry->onClick, "Preferences exposes explicit ASIO settings entry");
        entry->onClick(); require(openedSettings == 1, "Preferences delegates settings ownership to application");
    }
    require(discoveries == 0 && inspections == 0 && preparations == 0, "settings construction and passive reads do not enumerate drivers");
    auto click = [&](const char* componentId)
    {
        auto* button = findButton(*panel, componentId);
        require(button && button->isEnabled() && button->onClick, "required settings action is enabled");
        button->onClick(); session->sendSynchronousChangeMessage();
    };
    click("asio.discover"); drain();
    auto* drivers = findBox(*panel, "asio.driver"); require(drivers && drivers->getSelectedId() == 0, "no default ASIO selection");
    drivers->setSelectedId(1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    engine.play(); engine.seekToSeconds(.012);
    click("asio.inspect"); drain();
    require(session->capabilities() && engine.isPlaying() && std::abs(engine.getCurrentPositionSeconds() - .012) < 1.e-5
        && !engine.isDeviceLeasedForProbe() && !settings.getUserSettings()->containsKey(AsioSettingsSession::bindingKey),
        "capability UI returns owner/transport and does not persist uncommitted driver choice");
    int stereo = -1;
    for (size_t index = 0; index < session->profiles().size(); ++index)
        if (const auto& bus = session->profiles()[index].bus; bus.speakers.size() == 2 && bus.speakers[0].role == SpeakerRole::L && bus.speakers[1].role == SpeakerRole::R) stereo = static_cast<int>(index);
    require(stereo >= 0, "canonical stereo profile exists");
    findBox(*panel, "asio.profile")->setSelectedId(stereo + 1, juce::sendNotificationSync); session->sendSynchronousChangeMessage();
    require(session->draft().outputsBySpeaker == std::vector<std::vector<int>>{{}, {}}, "unknown physical semantics create no guessed matrix");
    click("asio.validate"); require(session->review() && !session->review()->isValid(), "empty output matrix cannot activate");
    const auto setCell = [&](int row, int physical, bool on)
    {
        auto* button = findButton(*panel, "asio.matrix." + juce::String(row) + "." + juce::String(physical));
        require(button && button->isEnabled(), "actual table cell is available");
        button->setToggleState(on, juce::dontSendNotification); button->onClick(); session->sendSynchronousChangeMessage();
    };
    setCell(0, 1, true); setCell(1, 3, true); click("asio.validate");
    require(session->review()->isValid() && !session->review()->needsConfirmation(), "explicit noncontiguous stereo matrix validates");
    findBox(*panel, "asio.ping-output")->setSelectedId(4, juce::sendNotificationSync);
    click("asio.ping"); drain();
    require(candidateControls.pingFrames >= 12000 && candidateControls.pingPeak > .009f && candidateControls.pingPeak <= .010001f
        && !engine.isDeviceLeasedForProbe() && engine.isPlaying() && !AudioEngineDeviceTestAccess::route(engine),
        "actual bounded ping callback targets selected compact output and returns old owner without activating draft");
    const auto writePicture = [&](const char* name)
    {
        auto output = artifacts.getChildFile(name).createOutputStream(); juce::PNGImageFormat png;
        require(output && png.writeImageToStream(panel->createComponentSnapshot(panel->getLocalBounds()), *output), "save native ASIO settings evidence");
    };
    writePicture("asio-settings-stereo.png");
    // Convergence has two explicit warnings. Editing invalidates confirmation.
    setCell(1, 1, true); click("asio.validate");
    require(session->review()->needsConfirmation() && !findButton(*panel, "asio.activate")->isEnabled(), "warnings require explicit per-review confirmation");
    auto* confirm = findButton(*panel, "asio.confirm"); confirm->setToggleState(true, juce::dontSendNotification); confirm->onClick();
    require(findButton(*panel, "asio.activate")->isEnabled(), "user can explicitly approve reviewed warnings");
    writePicture("asio-settings-warnings.png");
    {
        const auto originalTheme = RendererVisualStyle::modernTheme();
        struct RestoreTheme { ModernTheme theme; ~RestoreTheme() { RendererVisualStyle::setModernTheme(theme); } } restoreTheme {originalTheme};
        RendererVisualStyle::setModernTheme(ModernTheme::night); session->sendSynchronousChangeMessage();
        writePicture("asio-settings-night.png");
    }
    session->sendSynchronousChangeMessage();
    panel->setSize(820, 600); session->sendSynchronousChangeMessage(); writePicture("asio-settings-minimum.png"); panel->setSize(1040, 720);
    int surround = -1;
    for (size_t index = 0; index < session->profiles().size(); ++index)
        if (session->profiles()[index].bus.speakers.size() == 12) surround = static_cast<int>(index);
    require(surround >= 0, "canonical 7.1.4 profile exists");
    findBox(*panel, "asio.profile")->setSelectedId(surround + 1, juce::sendNotificationSync);
    panel->setSize(1040, 900); session->sendSynchronousChangeMessage();
    setCell(0, 0, true); setCell(11, 3, true); click("asio.validate");
    require(session->draft().bus.speakers.size() == 12 && session->review()->needsConfirmation(), "7.1.4 matrix retains all canonical rows and warns about missing physical connections");
    writePicture("asio-settings-7.1.4.png");
    findBox(*panel, "asio.profile")->setSelectedId(stereo + 1, juce::sendNotificationSync);
    panel->setSize(1040, 720); session->sendSynchronousChangeMessage();
    setCell(0, 1, true); setCell(1, 3, true);
    setCell(1, 1, false); require(!session->review() && !confirm->getToggleState(), "matrix edit retires old review and warning confirmation");
    click("asio.validate");
    coerce.store(true); click("asio.activate"); drain();
    require(!AudioEngineDeviceTestAccess::route(engine) && !settings.getUserSettings()->containsKey(AsioSettingsSession::bindingKey)
        && !engine.isDeviceLeasedForProbe(), "changed actual endpoint rejects activation and persistence, returns old owner");
    click("asio.activate"); drain();
    const auto active = AudioEngineDeviceTestAccess::route(engine);
    const auto saved = settings.getUserSettings()->getValue(AsioSettingsSession::bindingKey);
    require(active && active->device().physicalOutputs == std::vector<int>{1, 3} && active->connected(0, 0) && active->connected(1, 1)
        && juce::JSON::parse(saved)["stableId"].toString() == juce::String(id), "actual UI activation commits and persists exact successful route");
    require(document.getState() == original && engine.isPlaying(), "settings flow retains actual Document and latest transport intent");
    // The dialog is disposable while a live operation remains application-owned.
    holdInspection.store(true); click("asio.inspect");
    const auto enteredDeadline = juce::Time::getMillisecondCounterHiRes() + 2000;
    bool entered = false;
    while (!(entered = inspectionEntered.wait(1)) && juce::Time::getMillisecondCounterHiRes() < enteredDeadline) session->poll();
    require(entered && engine.isDeviceLeasedForProbe(), "pending settings inspection holds exact device lease");
    panel.reset(); engine.seekToSeconds(.024); releaseInspection.signal(); drain();
    require(!session->capabilities() && !engine.isDeviceLeasedForProbe() && engine.isPlaying()
        && std::abs(engine.getCurrentPositionSeconds() - .024) < 1.e-5 && AudioEngineDeviceTestAccess::route(engine) == active
        && settings.getUserSettings()->getValue(AsioSettingsSession::bindingKey) == saved,
        "closing native settings cancels; application drains and returns committed route/latest seek without replacing saved binding");
    for (int scenario = 0; scenario < 7; ++scenario)
    {
        // Real startup owner: success, changed actual rate, renamed outputs,
        // cancel, stale geometry, invalid settings, missing warning approval.
        Controls startupControls, savedOutputControls;
        SingleTrackDocument startupDocument; AudioEngine startupEngine(startupDocument);
        AudioEngineDeviceTestAccess::install(startupEngine, std::make_unique<FixtureManager>(startupControls));
        require(startupEngine.reserveExplicitStartupOutput(), "reserve selected backend before ordinary warmup");
        startupEngine.warmUpAudioDeviceAsync(0);
        require(startupEngine.getDeviceManagerForConfiguration() != nullptr && startupControls.scans == 0 && startupControls.opens == 0,
            "reserved startup does not initialize even an existing cold system manager");
        require(startupEngine.loadFile(artifacts.getChildFile("physical-route-source.wav")), "document remains loadable while selected output is absent");
        startupEngine.play();
        require(startupEngine.canPlayAtSeconds(0) && !startupEngine.isPlaying() && !startupEngine.isSelectedOutputReady(),
            "real readable source is distinct from pending selected output and cannot fake playback");
        juce::ApplicationProperties restoredSettings;
        auto restoredOptions = options;
        restoredOptions.applicationName = "AsioStartup" + juce::String(scenario);
        restoredSettings.setStorageParameters(restoredOptions);
        auto binding = juce::JSON::parse(saved);
        if (scenario == 4) binding.getDynamicObject()->setProperty("revision", 777);
        if (scenario == 6)
        {
            auto* speakers = binding["speakers"].getArray();
            (*speakers)[1].getDynamicObject()->setProperty("physicalOutputs", juce::Array<juce::var>{1, 3});
            binding.getDynamicObject()->setProperty("warningsConfirmed", false);
        }
        const auto storedText = scenario == 5 ? juce::String("invalid saved setting") : juce::JSON::toString(binding, true);
        restoredSettings.getUserSettings()->setValue(AsioSettingsSession::bindingKey, storedText);
        restoredSettings.getUserSettings()->setValue("autoPlayEnabled", false);
        restoredSettings.getUserSettings()->setValue("restorePlaylistSession", false);
        juce::WaitableEvent preparedEntered, releasePrepared;
        std::atomic<int> startupDiscoveries {0}, startupPreparations {0};
        auto restoredSession = AsioSettingsTestAccess::make(startupEngine, restoredSettings,
            [&] { ++startupDiscoveries; return Adapter::Catalog{}; }, {},
            [&](const Switch::Selection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Prepared
            {
                ++startupPreparations;
                if (!access.isValid()) return {{}, Switch::Error::prepareFailed};
                if (scenario == 0 || scenario == 3) { preparedEntered.signal(); releasePrepared.wait(7000); }
                if (cancelled.load()) return {{}, Switch::Error::cancelled};
                auto actual = choice.expected;
                if (scenario == 1) actual.sampleRate = 96000;
                if (scenario == 2) actual.outputNames[0] = "Changed physical output";
                return {std::make_unique<FixtureOutput>(savedOutputControls, std::move(actual)), Switch::Error::none};
            });
        WaveformModel startupWaveform(startupDocument); BnkDocument startupBank; PlaylistModel startupPlaylist;
        juce::ResizableWindow startupHost("Selected ASIO startup fixture", false);
        std::unique_ptr<MainComponent> startupMain;
        bool painted = false;
        int coreNotifications = 0;
        if (scenario == 0)
        {
            startupMain = std::make_unique<MainComponent>(startupHost, restoredSettings, startupDocument, startupBank,
                startupPlaylist, startupEngine, startupWaveform, true, false);
            AsioSettingsTestAccess::install(*startupMain, restoredSession);
            AsioSettingsTestAccess::observeWaveform(*startupMain, painted);
            startupMain->setStartupCoreReadyCallback([&] { ++coreNotifications; });
            startupMain->setSize(1200, 820); startupHost.setContentNonOwned(startupMain.get(), true);
            startupHost.addToDesktop(); startupHost.setVisible(true);
        }
        struct ReturnBeforeFixtureDies
        {
            std::shared_ptr<AsioSettingsSession> session; juce::WaitableEvent& release;
            ~ReturnBeforeFixtureDies()
            {
                release.signal(); session->cancel();
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8000;
                while (session->needsPoll() && juce::Time::getMillisecondCounterHiRes() < deadline) { session->poll(); juce::Thread::sleep(1); }
            }
        } returnBeforeFixtureDies {restoredSession, releasePrepared};
        if (startupMain)
        {
            const auto preparedState = std::make_shared<SingleTrackDocument::State>(*startupDocument.getState());
            auto peaks = WaveformModel::buildCompletePeakCacheForState(preparedState);
            require(peaks && peaks->binCount > 0, "startup fixture builds real source waveform data");
            startupMain->openPreparedInitialAudio(preparedState->info.file, preparedState->info, peaks, preparedState);
            startupMain->restoreSavedAudioOutputAfterStartupHandoff();
        }
        else restoredSession->restoreSavedSelection();
        if (scenario == 0 || scenario == 3)
        {
            require(preparedEntered.wait(2000), "only the saved candidate is preparing asynchronously");
            if (startupMain)
            {
                const auto picture = startupMain->createComponentSnapshot(startupMain->getLocalBounds());
                auto output = artifacts.getChildFile("asio-startup-waveform-before-output.png").createOutputStream();
                juce::PNGImageFormat png;
                require(output && png.writeImageToStream(picture, *output) && painted
                    && !AsioSettingsTestAccess::coreReady(*startupMain) && coreNotifications == 0,
                    "real source waveform paints while selected driver is held; CoreReady/playable remains pending");
                require(!AsioSettingsTestAccess::spatialEnabled(*startupMain)
                    && !startupEngine.getSpatialEnergyDiagnostics().workerRunning, "Spatial Energy remains dormant until actual waveform AND playable");
            }
            if (scenario == 3) restoredSession->cancel();
            releasePrepared.signal();
        }
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000;
        while (restoredSession->needsPoll() && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            if (startupMain) AsioSettingsTestAccess::poll(*startupMain); else restoredSession->poll();
            juce::Thread::sleep(1);
        }
        if (startupMain) AsioSettingsTestAccess::poll(*startupMain);
        require(!restoredSession->needsPoll() && startupDiscoveries == 0 && startupControls.scans == 0 && startupControls.opens == 0
            && restoredSettings.getUserSettings()->getValue(AsioSettingsSession::bindingKey) == storedText,
            "saved restore never discovers alternate devices, opens Windows fallback, or rewrites saved intent");
        if (scenario == 0)
        {
            const auto restoredRoute = AudioEngineDeviceTestAccess::route(startupEngine);
            require(restoredRoute && restoredRoute->device().physicalOutputs == std::vector<int>{1, 3}
                && startupEngine.isSelectedOutputReady() && AsioSettingsTestAccess::coreReady(*startupMain) && coreNotifications == 1,
                "CoreReady becomes true only after the exact saved output commits following real waveform");
            auto* host=AsioSettingsTestAccess::workspace(*startupMain);
            require(host && AsioSettingsTestAccess::spatialEnabled(*startupMain)
                && !startupEngine.getSpatialEnergyDiagnostics().workerRunning, "post-Core registers closed Spatial Energy without creating a consumer");
            const auto state=startupDocument.getState();
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.module.spatial-energy.toggle")
                && startupEngine.getSpatialEnergyDiagnostics().consumers==1, "real command palette route opens a spatial view lease");
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.module.spatial-energy.float")
                && host->isModuleFloating(WorkspaceModuleId::spatialEnergy), "Spatial Energy floats through the real workspace command");
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.module.spatial-energy.redock")
                && !host->isModuleFloating(WorkspaceModuleId::spatialEnergy), "Spatial Energy redocks");
            require(host->dockModule(WorkspaceModuleId::spatialEnergy,WorkspaceModuleId::outputMeter,WorkspaceDockZone::centre)==WorkspaceGridMutationResult::committed,
                "spatial view joins real workspace tab stack");
            (void)host->activateTab(WorkspaceModuleId::outputMeter);
            require(!startupEngine.getSpatialEnergyDiagnostics().workerRunning,"Tab sleep releases spatial analysis and visual worker");
            (void)host->activateTab(WorkspaceModuleId::spatialEnergy);
            require(startupEngine.getSpatialEnergyDiagnostics().consumers==1,"Tab wake reacquires one spatial consumer");
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.view.simple")
                && !startupEngine.getSpatialEnergyDiagnostics().workerRunning,"Simple hides and releases spatial view resources");
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.scene.general")
                && startupEngine.getSpatialEnergyDiagnostics().consumers==1,"General restores the selected spatial view");
            require(AsioSettingsTestAccess::workspaceCommand(*startupMain,"workspace.module.spatial-energy.remove")
                && !startupEngine.getSpatialEnergyDiagnostics().workerRunning && startupDocument.getState()==state,
                "Remove frees resources without changing the document or Undo history");
        }
        else
        {
            startupEngine.warmUpAudioDeviceAsync(0); startupEngine.play();
            require(!startupEngine.isSelectedOutputReady() && !startupEngine.isPlaying() && startupControls.opens == 0
                && !AudioEngineDeviceTestAccess::route(startupEngine) && startupPreparations == (scenario <= 3 ? 1 : 0),
                "changed/invalid/cancelled saved configuration remains unavailable without fallback or guessed binding");
        }
        startupHost.clearContentComponent();
    }
    std::cout << "Saved ASIO startup passed: explicit reservation, exact single candidate, readable source/waveform before output, honest CoreReady, changed configuration/cancel/stale geometry rejection, no fallback\n";
    std::cout << "ASIO settings passed: native controls, lazy discovery, empty matrix, physical ping, per-review warnings, exact activation/persistence, rejected capabilities, close cancellation and owner return\n";
}
}

void runSpatialEnergyNativeTests(const juce::File& artifacts)
{
    runSpatialEnergyEngineTests(artifacts);
    runSpatialEnergyMultichannelTests(artifacts);
}

// The existing Native loop, rather than a nested modal pump, owns delivery of
// the actual delayed warmup. This session contains only the original fixture.
class DeviceBackendLifecycleTestSession final
{
public:
    explicit DeviceBackendLifecycleTestSession(juce::File output) : artifacts(std::move(output)),
        engine(std::make_unique<AudioEngine>(document))
    {
        AudioEngineDeviceTestAccess::install(*engine, std::make_unique<FixtureManager>(controls));
        require(!engine->getDeviceDiagnostics().latestRequest && controls.opens == 0,
            "warmup starts from a cold independent fixture with no manufactured UUID");
        engine->warmUpAudioDeviceAsync(0);
    }
    void finish()
    {
        try
        {
            require(engine != nullptr, "warmup lifecycle finishes once for its actual engine");
            require(!AudioEngineDeviceTestAccess::warmupPending(*engine),
                "actual warmup completed during the existing bounded Native message loop");
            const auto completed = engine->getDeviceDiagnostics();
            require(completed.active && completed.latestRequest && !completed.latestRequest->operationId.isNull()
                && completed.latestRequest->kind == DeviceRequestKind::initialise
                && completed.latestRequest->outcome == DeviceRequestOutcome::succeeded
                && completed.latestRequest->requestedOutputChannels == 12 && completed.active->activeOutputChannels == 4
                && controls.scans > 0 && controls.opens > 0,
                "real warmup binds its actual successful request UUID and keeps requested versus available outputs distinct");
            const auto captured = snapshot(*engine, document);
            require(artifacts.getChildFile("warmup-request.json").replaceWithText(juce::JSON::toString(captured, true)),
                "retain the real warmup diagnostic request independently from observer events");
            engine.reset(); // the existing cancelAndWait joins the actual joinable worker
            require(controls.aliveDevices == 0, "real engine destruction joins warmup and destroys its private manager/device");
            std::cout << "Device backend lifecycle passed: two existing coordinator flows plus actual warmup UUID, natural timer delivery, worker join and device disposal; no hardware acceptance\n";
        }
        catch (...)
        {
            engine.reset(); // preserve the failure while following the same real cleanup path
            throw;
        }
    }
private:
    juce::File artifacts;
    Controls controls;
    SingleTrackDocument document;
    std::unique_ptr<AudioEngine> engine; // dies before its real fixture dependencies
};

// One bounded observer invocation reuses the two existing ownership flows.
// The original diagnostics entry below still executes its complete suite.
std::shared_ptr<DeviceBackendLifecycleTestSession> runDeviceBackendLifecycleTests(const juce::File& artifacts)
{
    const auto source = artifacts.getChildFile("physical-route-source.wav");
    require(!source.existsAsFile(), "backend lifecycle owns a fresh local PCM fixture");
    juce::MemoryOutputStream wave;
    wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 4); wave.write("WAVEfmt ", 8);
    wave.writeInt(16); wave.writeShort(1); wave.writeShort(2); wave.writeInt(48000);
    wave.writeInt(192000); wave.writeShort(4); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 4);
    for (int frame = 0; frame < 4096; ++frame) { wave.writeShort(4096); wave.writeShort(8192); }
    require(source.replaceWithData(wave.getData(), wave.getDataSize()), "write existing coordinator fixture samples");
    runDeviceSwitchCoordinatorTests(artifacts, true);
    return std::make_shared<DeviceBackendLifecycleTestSession>(artifacts);
}

void finishDeviceBackendLifecycleTests(DeviceBackendLifecycleTestSession& session) { session.finish(); }

void runAudioDeviceDiagnosticsTests(juce::ApplicationProperties& properties, const juce::File& artifacts)
{
    using Kind = AudioEngine::DeviceRequestKind;
    using Outcome = AudioEngine::DeviceRequestOutcome;
    using Availability = AudioEngine::DeviceDiagnostics::Availability;
    Controls controls;
    SingleTrackDocument document;
    AudioEngine engine(document);
    const auto cold = engine.getDeviceDiagnostics();
    require(cold.availability == Availability::cold && !cold.latestRequest && !cold.active, "cold diagnostics constructs no operation or manager");
    require(snapshot(engine, document)["fields"]["deviceState"]["value"].toString() == "uninitialised", "cold is explicit rather than confused with open failure");
    require(snapshot(engine, document)["fields"]["channelMapping"]["unavailable"].toString() == "not-applicable",
        "cold capture cannot invent a physical output mapping");
    AudioEngineDeviceTestAccess::install(engine, std::make_unique<FixtureManager>(controls));
    auto* initialManager = engine.getDeviceManagerForConfiguration(); // actual production initialization path
    require(initialManager != nullptr, "unleased configuration has a manager");
    auto& manager = *initialManager;
    const auto initial = engine.getDeviceDiagnostics();
    require(initial.active && initial.active->activeOutputChannels == 4 && initial.latestRequest
        && initial.latestRequest->kind == Kind::initialise && initial.latestRequest->requestedOutputChannels == 12
        && initial.latestRequest->outcome == Outcome::succeeded, "initial desired maximum and actual available outputs remain distinct");
    require(initial.outputEndpoint.isEmpty(), "unrelated IODevice cannot be name-matched to a native endpoint");
    {
        auto providers = diagnostics::captureLiveDiagnosticProviders(engine, document, nullptr);
        std::atomic<bool> cancelled { false };
        const auto opens = controls.opens.load(), scans = controls.scans.load();
        diagnostics::enrichNativeDeviceMetadata(providers, cancelled);
        require(controls.opens == opens && controls.scans == scans, "unsupported native metadata does not scan or open a device");
        const auto& fields = providers.front().evidence.fields;
        require(std::any_of(fields.begin(), fields.end(), [](const auto& item)
            { return item.field == diagnostics::Field::deviceMetadataState && item.value.toString() == "not-supported"; }),
            "unsupported native binding has an explicit product outcome");
    }
    require(snapshot(engine, document)["fields"]["requestedRate"]["unavailable"].toString() == "not-applicable",
        "default rate choice never invents a numeric request");
    auto setup = manager.getAudioDeviceSetup();
    setup.sampleRate = 96000.0; setup.bufferSize = 256;
    setup.useDefaultOutputChannels = false; setup.outputChannels.clear(); setup.outputChannels.setRange(0, 2, true);
    require(engine.applyAudioDeviceSetup(setup).isEmpty(), "real manager accepts controlled explicit setup");
    const auto negotiated = snapshot(engine, document);
    require(static_cast<double>(negotiated["fields"]["requestedRate"]["value"]) == 96000.0
        && static_cast<double>(negotiated["fields"]["activeRate"]["value"]) == 48000.0
        && static_cast<int>(negotiated["fields"]["requestedBuffer"]["value"]) == 256
        && static_cast<int>(negotiated["fields"]["activeBuffer"]["value"]) == 128, "negotiated driver values do not overwrite original user request");
    controls.failNext = true; setup.bufferSize = 512;
    require(engine.applyAudioDeviceSetup(setup).contains("Private driver details"), "existing local warning retains the actual JUCE error");
    const auto failedState = engine.getDeviceDiagnostics();
    require(!failedState.active && failedState.availability == Availability::available && failedState.latestFailure
        && failedState.latestFailure->outcome == Outcome::rejected, "failed initialized owner is distinct from a cold device");
    const auto failed = snapshot(engine, document);
    const auto failureId = failed["latestFailure"]["operationId"].toString();
    require(failed["fields"]["deviceState"]["value"].toString() == "no-open-device"
        && failed["fields"]["negotiationOutcome"]["value"].toString() == "request-rejected"
        && static_cast<int>(failed["latestFailure"]["counters"][1]) == 512, "real failure's stage and requested buffer reach the same provider");
    setup.sampleRate = 48000.0; setup.bufferSize = 128;
    require(engine.applyAudioDeviceSetup(setup).isEmpty(), "normal explicit reopen succeeds after failure");
    const auto recovered = snapshot(engine, document);
    require(recovered["fields"]["deviceState"]["value"].toString() == "open"
        && recovered["fields"]["negotiationOutcome"]["value"].toString() == "succeeded"
        && recovered["latestFailure"]["operationId"].toString() == failureId
        && recovered["operationIds"].size() == 2 && recovered["operationIds"][0].toString() != failureId,
        "reopened device retains original failure identity separately from successful request");
    require(recovered["fields"]["lastNegotiationFailureStage"]["value"].toString() == "apply-setup",
        "successful current state does not obscure the retained failure's actual negotiation step");
    const auto opensBefore = controls.opens.load(), scansBefore = controls.scans.load();
    for (int i = 0; i < 3; ++i) snapshot(engine, document);
    require(controls.opens == opensBefore && controls.scans == scansBefore, "passive capture does not reopen or enumerate a loaded device");
    engine.selectAudioDeviceType("Not Installed Fixture Backend");
    const auto declined = engine.getDeviceDiagnostics();
    require(declined.active && declined.latestFailure->operationId != failedState.latestFailure->operationId
        && declined.latestRequest->kind == Kind::selectBackend && declined.latestRequest->outcome == Outcome::rejected,
        "void backend rejection preserves active device and replaces only the latest failure capsule");
    {
        PreferencesComponent preferences(engine, properties);
        auto* rate = findBox(preferences, "audio-device.rate");
        auto* buffer = findBox(preferences, "audio-device.buffer");
        require(rate && buffer, "actual Preferences exposes its device controls");
        selectItem(*rate, "96");
        const auto fromUi = engine.getDeviceDiagnostics();
        require(fromUi.latestRequest->kind == Kind::applySetup && fromUi.latestRequest->requestedRate == 96000.0,
            "actual Preferences rate callback uses the recorded Engine operation");
        controls.failNext = true;
        selectItem(*buffer, "512");
        require(engine.getDeviceDiagnostics().latestFailure->requestedBuffer == 512, "actual Preferences failure enters the provider-owned capsule");
        const auto image = preferences.createComponentSnapshot(preferences.getLocalBounds(), true, 1.0f);
        const auto stream = artifacts.getChildFile("device-preferences-fixture.png").createOutputStream();
        require(stream && juce::PNGImageFormat().writeImageToStream(image, *stream), "retain actual JUCE Preferences render with controlled device outcome");
    }
    controls.blockNext = true;
    juce::String workerError;
    std::jthread worker([&] { workerError = engine.applyAudioDeviceSetup(setup); });
    require(controls.entered.wait(3000), "controlled driver is inside actual manager open");
    const auto before = juce::Time::getMillisecondCounterHiRes();
    const auto busy = engine.getDeviceDiagnostics();
    require(busy.availability == Availability::busy && juce::Time::getMillisecondCounterHiRes() - before < 250.0,
        "passive capture reports busy without waiting for device negotiation");
    require(snapshot(engine, document)["fields"]["negotiationStage"]["unavailable"].toString() == "provider-busy",
        "busy reason reaches the public typed provider rather than a fabricated cold snapshot");
    controls.release.signal(); worker.join();
    require(workerError.isEmpty(), "bounded controlled driver resumes normal negotiation after observation");
    {
        using Probe = diagnostics::GuiDeviceProbe;
        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SingleEditorCLI.exe");
        require(executable.existsAsFile(), "coordinator test needs the actual product CLI companion");
        const auto original = engine.getDeviceProbeConfiguration();
        require(original.has_value(), "idle current device has a passive consent target");
        diagnostics::DeviceProbeRequest request;
        request.backend = original->backend; request.outputDevice = original->setup.outputDeviceName;
        request.outputChannels = original->setup.outputChannels.countNumberOfSetBits();
        auto stale = *original; stale.setup.bufferSize += 1;
        require(!Probe::start(engine, stale, request, executable) && !engine.isDeviceLeasedForProbe(),
            "changed consent target refuses transfer and abandons the prepared worker");
        const auto finish = [&](const std::shared_ptr<Probe>& probe)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 7000.0;
            while (!probe->result() && juce::Time::getMillisecondCounterHiRes() < deadline)
            { probe->pollReturn(engine); juce::Thread::sleep(2); }
            require(probe->result() != nullptr, "optional coordinator returns the actual manager after the child settles");
        };
        auto probe = Probe::start(engine, *original, request, executable);
        require(probe && engine.isDeviceLeasedForProbe(), "coordinator owns actual engine lease before returning to the UI");
        finish(probe);
        const auto* result = probe->result();
        require(!result->succeeded && result->evidence.latestFailure && engine.getDeviceProbeConfiguration() == original,
            "real child rejects fixture-only backend while coordinator still restores exact original device");
        require(std::any_of(result->evidence.fields.begin(), result->evidence.fields.end(), [](const auto& field)
            { return field.field == diagnostics::Field::deviceProbeRestore && field.value == juce::var("exact-device-restored"); }),
            "failed probe and successful GUI restoration remain separate reported outcomes");
        probe = Probe::start(engine, *original, request, executable); require(probe != nullptr, "next explicit operation starts after return");
        probe->cancel(); finish(probe);
        require(engine.getDeviceProbeConfiguration() == original && !engine.isPlaying(), "cancel restores without restarting transport");
        controls.alwaysFail = true;
        probe = Probe::start(engine, *original, request, executable); require(probe != nullptr, "restore failure operation starts");
        finish(probe); controls.alwaysFail = false;
        require(!probe->result()->succeeded && probe->result()->evidence.latestFailure->stage == diagnostics::FailureStage::cleanup
            && !engine.getDeviceProbeConfiguration(), "coordinator surfaces exact restore failure as distinct cleanup capsule");
        require(engine.applyAudioDeviceSetup(original->setup).isEmpty(), "explicit setup repairs reported coordinator restore failure");
        Controls abandonedControls; SingleTrackDocument abandonedDocument;
        auto abandonedEngine = std::make_unique<AudioEngine>(abandonedDocument);
        AudioEngineDeviceTestAccess::install(*abandonedEngine, std::make_unique<FixtureManager>(abandonedControls));
        abandonedEngine->getDeviceManagerForConfiguration();
        const auto abandonedTarget = abandonedEngine->getDeviceProbeConfiguration();
        auto abandonedProbe = Probe::start(*abandonedEngine, *abandonedTarget, request, executable);
        require(abandonedProbe != nullptr, "independent owner starts its own device operation");
        abandonedProbe->abandon(); abandonedEngine.reset();
        const auto releaseDeadline = juce::Time::getMillisecondCounterHiRes() + 7000.0;
        while (!abandonedProbe->resourcesReleased() && juce::Time::getMillisecondCounterHiRes() < releaseDeadline) juce::Thread::sleep(2);
        require(abandonedProbe->resourcesReleased() && abandonedControls.aliveDevices == 0,
            "worker releases actual detached device and finishes after owning engine disappears");
    }
    {
        auto exactSetup = manager.getAudioDeviceSetup();
        exactSetup.useDefaultOutputChannels = false;
        exactSetup.outputChannels.clear(); exactSetup.outputChannels.setBit(1); exactSetup.outputChannels.setBit(3);
        require(manager.setAudioDeviceSetup(exactSetup, true).isEmpty(), "controlled manager selects noncontiguous physical outputs");
        exactSetup = manager.getAudioDeviceSetup();
        const auto xml = manager.createStateXml()->toString();
        const auto state = document.getState();
        auto lease = engine.takeDeviceLeaseForProbe();
        require(lease && lease->originallyOpen() && lease->originalSetup() == exactSetup,
            "lease freezes actual setup and noncontiguous channels without another negotiation");
        const auto opens = controls.opens.load(), scans = controls.scans.load();
        require(!engine.returnDeviceLease(lease), "unrestored lease cannot return early");
        require(!engine.takeDeviceLeaseForProbe() && !engine.isIdleForDiagnosticProbe()
            && engine.getDeviceManagerForConfiguration() == nullptr, "exclusive lease refuses competing ownership/configuration");
        engine.play(); engine.pingSpeakerRole(SpeakerRole::L); engine.warmUpAudioDeviceAsync();
        engine.selectAudioDeviceType("Fixture Audio");
        require(!engine.applyAudioDeviceSetup(exactSetup).isEmpty() && engine.preferMaximumOutputChannelsForCurrentDevice() == 0,
            "settings and channel expansion refuse a leased manager");
        require(engine.getDeviceDiagnostics().availability == Availability::busy && !engine.getRuntimeMetricsForDiagnostics(),
            "passive reads report busy without inventing a cold or replacement device");
        require(snapshot(engine, document)["fields"]["channelMapping"]["unavailable"].toString() == "provider-busy",
            "leased device cannot publish a stale physical output mapping");
        {
            PreferencesComponent busyPreferences(engine, properties);
            auto* rate = findBox(busyPreferences, "audio-device.rate");
            require(rate && !rate->isEnabled(), "real Preferences is explicitly disabled during ownership transfer");
        }
        require(controls.opens == opens && controls.scans == scans && !engine.isPlaying(),
            "competing UI/playback/ping/warmup perform no device work or false playback");
        bool closed = false, restored = false;
        std::jthread restoration([&] { closed = lease->closeForProbe(); restored = lease->restore(); });
        restoration.join();
        require(closed && restored, "worker closes and restores the actual manager");
        Controls otherControls; SingleTrackDocument otherDocument; AudioEngine otherEngine(otherDocument);
        AudioEngineDeviceTestAccess::install(otherEngine, std::make_unique<FixtureManager>(otherControls));
        otherEngine.getDeviceManagerForConfiguration();
        auto otherLease = otherEngine.takeDeviceLeaseForProbe();
        require(otherLease && !otherEngine.returnDeviceLease(lease) && lease,
            "a different live engine cannot accept an otherwise successfully restored lease");
        require(otherLease->closeForProbe() && otherLease->restore() && otherEngine.returnDeviceLease(otherLease),
            "independent owner's lease remains usable after a stale return");
        require(engine.returnDeviceLease(lease) && !lease && engine.getDeviceManagerForConfiguration() == &manager,
            "only the original live owner regains the same manager instance");
        require(manager.getAudioDeviceSetup() == exactSetup && manager.createStateXml()->toString() == xml
            && document.getState() == state && engine.isIdleForDiagnosticProbe(),
            "exact device setup/explicit XML/Document identity survive transfer without replaying a suppressed ping");
        for (const bool coerce : { false, true })
        {
            lease = engine.takeDeviceLeaseForProbe();
            require(lease && lease->closeForProbe(), "failure case transfers and closes the manager");
            controls.failNext = !coerce; controls.coerceNext = coerce;
            require(!lease->restore() && engine.returnDeviceLease(lease), "open failure or changed physical channels cannot be labelled restored");
            const auto failedOpens = controls.opens.load();
            engine.warmUpAudioDeviceAsync(); engine.play(); engine.pingSpeakerRole(SpeakerRole::L);
            require(!engine.isPlaying() && !engine.getRuntimeMetricsForDiagnostics() && controls.opens == failedOpens,
                "failed restore stays unavailable until explicit setup, with no fallback or queued audio");
            require(engine.applyAudioDeviceSetup(exactSetup).isEmpty() && engine.getRuntimeMetricsForDiagnostics(),
                "explicit setup recovers the same owner after a reported restore failure");
        }
        std::unique_ptr<AudioEngine::DeviceLease> orphan;
        Controls orphanControls; SingleTrackDocument orphanDocument;
        {
            AudioEngine owner(orphanDocument);
            AudioEngineDeviceTestAccess::install(owner, std::make_unique<FixtureManager>(orphanControls));
            owner.getDeviceManagerForConfiguration();
            orphan = owner.takeDeviceLeaseForProbe();
            require(orphan && orphan->ownerAlive(), "transferred ownership has an independent liveness token");
        }
        const auto orphanOpens = orphanControls.opens.load();
        std::jthread orphanCleanup([&] {
            require(!orphan->ownerAlive() && orphan->closeForProbe() && !orphan->restore(),
                "destroyed engine is never called or reopened by a stale worker");
            orphan.reset();
        });
        orphanCleanup.join();
        require(orphanControls.opens == orphanOpens, "owner death only releases device resources");
    }
    require(engine.isIdleForDiagnosticProbe(), "stopped manager permits an explicit probe without another open");
    engine.pingSpeakerRole(SpeakerRole::L);
    require(!engine.isIdleForDiagnosticProbe(), "an actual pending speaker test on an open controlled device refuses a probe");
    {
        Controls unavailable; unavailable.alwaysFail = true;
        SingleTrackDocument unavailableDocument;
        AudioEngine unavailableEngine(unavailableDocument);
        AudioEngineDeviceTestAccess::install(unavailableEngine, std::make_unique<FixtureManager>(unavailable));
        unavailableEngine.getDeviceManagerForConfiguration();
        const auto unavailableState = unavailableEngine.getDeviceDiagnostics();
        require(!unavailableState.active && unavailableState.latestFailure && unavailableState.latestFailure->kind == Kind::initialise
            && unavailableState.latestFailure->outcome == Outcome::rejected, "actual initial manager failure is retained rather than labelled cold");
        require(snapshot(unavailableEngine, unavailableDocument)["fields"]["negotiationStage"]["value"].toString() == "initialise",
            "initialization failure stage reaches the same live provider");
        unavailableEngine.pingSpeakerRole(SpeakerRole::L);
        require(unavailableEngine.isIdleForDiagnosticProbe(), "failed device initialization cannot turn an inaudible pending ping into a permanent codec-probe refusal");
        auto closedLease = unavailableEngine.takeDeviceLeaseForProbe();
        const auto opens = unavailable.opens.load();
        require(closedLease && !closedLease->originallyOpen() && closedLease->closeForProbe()
            && closedLease->restore() && unavailableEngine.returnDeviceLease(closedLease) && unavailable.opens == opens,
            "a originally closed device remains closed instead of opening a fallback on restoration");
    }
    {
        // Real stable device clock across source changes: only the playback
        // consumer adapts, without reopening hardware or publishing edits.
        Controls rateControls;
        SingleTrackDocument rateDocument;
        AudioEngine rateEngine(rateDocument);
        AudioEngineDeviceTestAccess::install(rateEngine, std::make_unique<FixtureManager>(rateControls));
        auto* rateManager = rateEngine.getDeviceManagerForConfiguration();
        auto selected = rateManager->getAudioDeviceSetup();
        selected.useDefaultOutputChannels = false; selected.outputChannels.clear();
        selected.outputChannels.setBit(1); selected.outputChannels.setBit(3);
        require(rateManager->setAudioDeviceSetup(selected, true).isEmpty(), "prepare source-rate playlist output");
        const auto opens = rateControls.opens.load();
        int item = 0;
        for (int sourceRate : {44100, 48000, 96000, 48000})
        {
            const auto file = artifacts.getChildFile("rate-playlist-" + juce::String(++item) + ".wav");
            juce::MemoryOutputStream wave;
            wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 4); wave.write("WAVEfmt ", 8);
            wave.writeInt(16); wave.writeShort(1); wave.writeShort(2); wave.writeInt(sourceRate);
            wave.writeInt(sourceRate * 4); wave.writeShort(4); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 4);
            for (int frame = 0; frame < 4096; ++frame) { wave.writeShort(4096); wave.writeShort(-4096); }
            require(file.replaceWithData(wave.getData(), wave.getDataSize()) && rateEngine.loadFile(file), "load playlist source");
            const auto state = rateDocument.getState();
            // loadFile preserves/clamps the existing engine position; the
            // playlist starts each item explicitly, as its UI caller does.
            rateEngine.seekToSeconds(0.0);
            rateEngine.setMonitorGainLinear(1.0f); rateEngine.play();
            auto* device = dynamic_cast<FixtureDevice*>(rateManager->getCurrentAudioDevice());
            require(device != nullptr, "playlist retains controlled device");
            device->renderOutputs(); device->renderOutputs();
            const auto output = device->renderOutputs();
            const auto facts = snapshot(rateEngine, rateDocument)["fields"];
            std::cout << "Playlist source=" << sourceRate << " position=" << rateEngine.getCurrentPositionSeconds()
                      << " mode=" << facts["playbackRateMode"]["value"].toString() << '\n';
            require(std::abs(output.getSample(1, 127) - 0.125f) < 0.00001f
                && std::abs(output.getSample(3, 127) + 0.125f) < 0.00001f
                && output.getMagnitude(0, 0, 128) == 0 && output.getMagnitude(2, 0, 128) == 0,
                "rate adaptation preserves stereo phase on exact noncontiguous physical outputs");
            require(std::abs(rateEngine.getCurrentPositionSeconds() - 384.0 / 48000.0) < 1.0 / 48000.0,
                "playhead counts produced device frames, not converter input lookahead");
            require(facts["playbackRateMode"]["value"].toString() == (sourceRate == 48000 ? "bypassed" : "converting")
                && static_cast<double>(facts["playbackSourceRate"]["value"]) == sourceRate
                && static_cast<double>(facts["activeRate"]["value"]) == 48000.0
                && (sourceRate == 48000 || facts["playbackRateAlgorithm"]["value"].toString() == PlaybackRateSource::Snapshot::algorithm),
                "live diagnostics reports actual source/device rates and pinned converter or bypass");
            require(rateControls.opens == opens && rateDocument.getState() == state && !rateDocument.hasUnsavedChanges(),
                "source-rate changes neither reopen device nor change the Document");
            require(static_cast<int>(facts["playbackAggregateLatencySamples"]["value"]) == 96
                && static_cast<double>(facts["playbackAggregateLatencyMs"]["value"]) == 2.0
                && static_cast<int>(facts["playbackProcessingLatencySamples"]["value"]) == 0,
                "live diagnostic export reports aggregate output latency without summing SRC read-ahead or buffer twice");
            rateEngine.pause();
        }
        std::cout << "Source-rate playlist passed: 44.1/48/96/48, fixed 48k device, live SRC facts, no reopen or Document mutation\n";
    }
    {
        Controls routedControls;
        SingleTrackDocument routedDocument;
        AudioEngine routedEngine(routedDocument);
        AudioEngineDeviceTestAccess::install(routedEngine, std::make_unique<FixtureManager>(routedControls));
        auto* routedManager = routedEngine.getDeviceManagerForConfiguration();
        require(routedManager != nullptr, "route fixture uses the real configured manager");
        auto selected = routedManager->getAudioDeviceSetup();
        selected.useDefaultOutputChannels = false; selected.outputChannels.clear();
        selected.outputChannels.setBit(1); selected.outputChannels.setBit(3);
        // The Preferences wrapper intentionally expands to all available
        // outputs. Select the exact manager state, as the existing lease test
        // does, so this fixture actually exercises a noncontiguous device.
        require(routedManager->setAudioDeviceSetup(selected, true).isEmpty()
            && routedManager->getCurrentAudioDevice()->getActiveOutputChannels() == selected.outputChannels,
            "actual manager selects only second and fourth physical outputs");
        const auto source = artifacts.getChildFile("physical-route-source.wav");
        juce::MemoryOutputStream wave;
        wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 4); wave.write("WAVEfmt ", 8);
        wave.writeInt(16); wave.writeShort(1); wave.writeShort(2); wave.writeInt(48000);
        wave.writeInt(192000); wave.writeShort(4); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 4);
        for (int frame = 0; frame < 4096; ++frame) { wave.writeShort(4096); wave.writeShort(8192); }
        require(source.replaceWithData(wave.getData(), wave.getDataSize()) && routedEngine.loadFile(source),
            "load known stereo samples through the actual document reader");
        require(snapshot(routedEngine, routedDocument, "spatial")["fields"]["routingObservation"]["value"].toString() == "not-rendered",
            "loaded configuration does not masquerade as an observed render matrix");
        routedEngine.setMonitorGainLinear(1.0f); routedEngine.play();
        auto* device = dynamic_cast<FixtureDevice*>(routedManager->getCurrentAudioDevice());
        require(device != nullptr, "current manager owns the controlled output device");
        device->renderOutputs(); device->renderOutputs();
        const auto rendered = device->renderOutputs();
        std::cout << "Physical route samples: " << rendered.getSample(0, 127) << "," << rendered.getSample(1, 127)
                  << "," << rendered.getSample(2, 127) << "," << rendered.getSample(3, 127)
                  << " active=" << device->getActiveOutputChannels().toString(16)
                  << " playing=" << routedEngine.isPlaying() << " position=" << routedEngine.getCurrentPositionSeconds() << '\n';
        require(std::abs(rendered.getSample(1, 127) - 0.125f) < 0.00001f
            && std::abs(rendered.getSample(3, 127) - 0.25f) < 0.00001f
            && rendered.getMagnitude(0, 0, 128) == 0 && rendered.getMagnitude(2, 0, 128) == 0,
            "actual manager/source-player render sends stereo to physical outputs two and four");
        const auto observation = snapshot(routedEngine, routedDocument);
        const auto mapping = observation["fields"]["channelMapping"]["value"];
        require(mapping.isArray() && mapping.size() == 2 && static_cast<int>(mapping[0]) == 1 && static_cast<int>(mapping[1]) == 3,
            "public diagnostics reports the same zero-based physical outputs that actually carried the samples");
        require(artifacts.getChildFile("physical-route-observation.json").replaceWithText(juce::JSON::toString(observation)),
            "retain the actual rendered-route diagnostic snapshot");
        auto spatial = snapshot(routedEngine, routedDocument, "spatial");
        auto matrix = spatial["fields"]["outputMatrix"]["value"];
        require(matrix.isArray() && matrix.size() == 4 && static_cast<double>(matrix[0]) == 1.0
            && static_cast<double>(matrix[1]) == 0.0 && static_cast<double>(matrix[2]) == 0.0 && static_cast<double>(matrix[3]) == 1.0
            && static_cast<int>(spatial["fields"]["outputMatrixSourceChannels"]["value"]) == 2
            && static_cast<int>(spatial["fields"]["outputMatrixOutputChannels"]["value"]) == 2
            && spatial["fields"]["decoderState"]["value"].toString() == "channel-router",
            "reported output-major identity matrix matches the actual stereo render");
        ChannelOutputMapping swapped;
        swapped.setOutputChannel(SpeakerRole::L, 1); swapped.setOutputChannel(SpeakerRole::R, 0);
        routedEngine.setOutputMapping(swapped);
        const auto swappedOutput = device->renderOutputs();
        spatial = snapshot(routedEngine, routedDocument, "spatial"); matrix = spatial["fields"]["outputMatrix"]["value"];
        require(matrix.size() == 4 && static_cast<double>(matrix[0]) == 0.0 && static_cast<double>(matrix[1]) == 1.0
            && static_cast<double>(matrix[2]) == 1.0 && static_cast<double>(matrix[3]) == 0.0
            && std::abs(swappedOutput.getSample(1, 127) - 0.25f) < 0.00001f
            && std::abs(swappedOutput.getSample(3, 127) - 0.125f) < 0.00001f,
            "swapped role mapping changes both the actual physical samples and exported matrix");
        routedEngine.setSourceChannelMuted(1, true);
        // D107 exact-rate playback has no resampler buffer retaining samples
        // from the preceding block. The first muted callback is already silent.
        const auto muteTransition = device->renderOutputs();
        const auto mutedOutput = device->renderOutputs();
        spatial = snapshot(routedEngine, routedDocument, "spatial"); matrix = spatial["fields"]["outputMatrix"]["value"];
        std::cout << "Mute transition: first=" << muteTransition.getSample(1, 0)
                  << " last=" << muteTransition.getSample(1, 127)
                  << " steady=" << mutedOutput.getMagnitude(1, 0, 128) << '\n';
        require(matrix.size() == 4 && static_cast<double>(matrix[1]) == 0.0
            && muteTransition.getMagnitude(1, 0, 128) == 0 && mutedOutput.getMagnitude(1, 0, 128) == 0
            && std::abs(mutedOutput.getSample(3, 127) - 0.125f) < 0.00001f,
            "source mute is reflected in the matrix and real output samples");
        const auto playbackFacts = snapshot(routedEngine, routedDocument)["fields"];
        require(playbackFacts["playbackRateMode"]["value"].toString() == "bypassed"
            && static_cast<double>(playbackFacts["playbackSourceRate"]["value"]) == 48000.0
            && static_cast<int>(playbackFacts["playbackRateLookahead"]["value"]) == 0,
            "real rate path reports bypass and zero lookahead through the live provider");
        require(artifacts.getChildFile("rendered-matrix-observation.json").replaceWithText(juce::JSON::toString(spatial)),
            "retain the actual rendered matrix observation");
        juce::WaitableEvent callbackHeld, releaseCallback;
        std::jthread callbackOwner([&] {
            const juce::ScopedLock callbackLock(routedManager->getAudioCallbackLock());
            callbackHeld.signal(); releaseCallback.wait(2000);
        });
        const auto held = callbackHeld.wait(2000);
        const auto busyMatrix = snapshot(routedEngine, routedDocument, "spatial");
        releaseCallback.signal(); callbackOwner.join();
        require(held && busyMatrix["fields"]["outputMatrix"]["unavailable"].toString() == "provider-busy",
            "busy audio callback returns an unavailable observation without waiting or racing its matrix");
        routedEngine.clearSourceChannelSoloMute(); routedEngine.clearOutputMapping();
        for (int block = 0; block < 40; ++block) device->renderOutputs();
        const auto readerObservation = snapshot(routedEngine, routedDocument, "codec");
        require(readerObservation["fields"]["selectedReader"]["value"].toString() == "JUCE-WAV-memory-mapped"
            && readerObservation["fields"]["container"]["value"].toString() == "WAV"
            && readerObservation["fields"]["dependencyIdentity"]["value"].toString().startsWith("JUCE-")
            && snapshot(routedEngine, routedDocument, "spatial")["fields"]["channelOrder"]["value"].toString() == "L R",
            "final snapshot reports the live native reader and document channel order");
        require(routedDocument.applySelectionGain({ 0.0, 0.04 }, 0.5f), "real audio edit materialises the document backing");
        require(snapshot(routedEngine, routedDocument, "spatial")["fields"]["routingObservation"]["value"].toString() == "not-rendered",
            "a new document state cannot inherit an older state's observed matrix");
        require(snapshot(routedEngine, routedDocument, "codec")["fields"]["selectedReader"]["value"].toString() == "document-pcm-buffer",
            "editing reports the current PCM buffer rather than a historical file reader");
        require(routedDocument.undo()
            && snapshot(routedEngine, routedDocument, "codec")["fields"]["selectedReader"]["value"].toString() == "JUCE-WAV-memory-mapped"
            && routedDocument.canRedo(), "Undo restores the actual reader evidence without discarding redo");
        require(artifacts.getChildFile("document-reader-observation.json").replaceWithText(juce::JSON::toString(readerObservation)),
            "retain the current document reader snapshot");
    }
    {
        SingleTrackDocument spatialDocument;
        AudioEngine spatialEngine(spatialDocument);
        const auto source = artifacts.getChildFile("ambisonic-route-source.wav");
        juce::MemoryOutputStream wave;
        wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 8); wave.write("WAVEfmt ", 8);
        wave.writeInt(16); wave.writeShort(1); wave.writeShort(4); wave.writeInt(48000);
        wave.writeInt(384000); wave.writeShort(8); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 8);
        for (int frame = 0; frame < 4096; ++frame)
        { wave.writeShort(8192); wave.writeShort(0); wave.writeShort(0); wave.writeShort(0); }
        require(source.replaceWithData(wave.getData(), wave.getDataSize()) && spatialEngine.loadFile(source)
            && spatialDocument.setChannelLayoutPreset("foa-acn-sn3d"), "load explicit real FOA speaker projection");
        spatialEngine.setMonitorGainLinear(1.0f);
        juce::AudioBuffer<float> output(2, 1024);
        require(spatialEngine.renderDocumentBlockForDiagnostics(0, output), "render the actual Ambisonic speaker graph without a device");
        const auto speaker = snapshot(spatialEngine, spatialDocument, "spatial");
        const auto matrix = speaker["fields"]["outputMatrix"]["value"];
        require(speaker["fields"]["decoderState"]["value"].toString() == "ambisonic-speaker" && matrix.size() == 8
            && output.getMagnitude(0, 0, 1024) > 0.001f
            && std::abs(output.getSample(0, 1023) - static_cast<double>(matrix[0]) * 0.25) < 0.00001
            && std::abs(output.getSample(1, 1023) - static_cast<double>(matrix[4]) * 0.25) < 0.00001,
            "reported speaker decode coefficients explain the actual known W-channel samples");
        require(spatialDocument.setAmbisonicMonitorMode(AmbisonicMonitorMode::binauralHrtf)
            && spatialEngine.renderDocumentBlockForDiagnostics(0, output), "explicitly load and render the actual bundled HRTF");
        const auto binaural = snapshot(spatialEngine, spatialDocument, "spatial");
        require(binaural["fields"]["decoderState"]["value"].toString() == "binaural-hrtf"
            && binaural["fields"]["outputMatrix"]["unavailable"].toString() == "not-applicable"
            && output.getMagnitude(0, 0, 1024) > 0.00001f && !spatialEngine.getRuntimeMetricsForDiagnostics(),
            "real binaural convolution reports its decoder without fabricating a scalar matrix or opening a device");
        require(artifacts.getChildFile("ambisonic-speaker-observation.json").replaceWithText(juce::JSON::toString(speaker))
            && artifacts.getChildFile("binaural-render-observation.json").replaceWithText(juce::JSON::toString(binaural)),
            "retain actual speaker and binaural snapshots");
    }
    {
        // Real engine/transport/manager ownership, with controlled callbacks.
        // No ASIO driver or physical hardware claim is made by these cases.
        SingleTrackDocument coldSwitchDocument;
        auto coldSwitchEngine = std::make_unique<AudioEngine>(coldSwitchDocument);
        require(coldSwitchEngine->loadFile(artifacts.getChildFile("physical-route-source.wav")),
            "cold switch has a document without requesting any output device");
        auto coldLease = coldSwitchEngine->takeDeviceLeaseForSwitch();
        require(coldLease && !coldLease->originallyOpen() && !coldLease->hasExclusiveSwitchWindow()
            && coldLease->closeForSwitch() && coldLease->hasExclusiveSwitchWindow() && coldLease->restore()
            && coldSwitchEngine->returnDeviceLease(coldLease),
            "cold owner transfers and restores its exact absence of output without constructing a manager");
        coldSwitchEngine->play(); coldSwitchEngine->warmUpAudioDeviceAsync();
        require(!coldSwitchEngine->isPlaying() && !coldSwitchEngine->getDeviceManagerForConfiguration()
            && !coldSwitchEngine->getDeviceDiagnostics().latestRequest,
            "cold rollback cannot implicitly open Windows default or manufacture an active device");
        coldLease = coldSwitchEngine->takeDeviceLeaseForSwitch();
        require(coldLease && coldLease->closeForSwitch() && coldLease->hasExclusiveSwitchWindow(),
            "an unavailable cold owner still permits the next explicit selection");
        coldSwitchEngine.reset();
        require(!coldLease->hasExclusiveSwitchWindow() && !coldLease->restore(),
            "owner death revokes the exact switch window and forbids reopening");
        coldLease.reset();
        Controls switchControls;
        SingleTrackDocument switchDocument;
        AudioEngine switchEngine(switchDocument);
        AudioEngineDeviceTestAccess::install(switchEngine, std::make_unique<FixtureManager>(switchControls));
        auto* switchManager = switchEngine.getDeviceManagerForConfiguration();
        auto switchSetup = switchManager->getAudioDeviceSetup();
        switchSetup.useDefaultOutputChannels = false; switchSetup.outputChannels.clear();
        switchSetup.outputChannels.setBit(1); switchSetup.outputChannels.setBit(3);
        require(switchManager->setAudioDeviceSetup(switchSetup, true).isEmpty(), "switch source has exact noncontiguous outputs");
        switchSetup = switchManager->getAudioDeviceSetup();
        require(switchEngine.loadFile(artifacts.getChildFile("physical-route-source.wav")), "switch exercises the real document reader");
        switchEngine.setMonitorGainLinear(1.0f);
        switchEngine.setLoopEnabled(true);
        switchEngine.seekToSeconds(0.005);
        switchEngine.play();
        const auto originalStart = switchEngine.getPlayStartSeconds();

        const auto takePlaying = [&]()
        {
            auto* currentDevice = dynamic_cast<FixtureDevice*>(switchManager->getCurrentAudioDevice());
            require(currentDevice != nullptr && switchEngine.isPlaying(), "switch starts with an actual active transport");
            std::jthread callbacks([&](std::stop_token stop)
            {
                while (!stop.stop_requested()) { currentDevice->renderOutputs(); juce::Thread::sleep(1); }
            });
            auto lease = switchEngine.takeDeviceLeaseForSwitch();
            callbacks.request_stop(); callbacks.join();
            require(lease && !switchEngine.isPlaying() && !lease->hasExclusiveSwitchWindow()
                && !lease->closeForProbe() && !switchEngine.takeDeviceLeaseForSwitch(),
                "playing transfer pauses and admits one switch, without granting access before close");
            return lease;
        };
        const auto restore = [&](std::unique_ptr<AudioEngine::DeviceLease>& lease, bool expected)
        {
            bool closed = false, granted = false, restored = false;
            std::jthread worker([&]
            {
                closed = lease->closeForSwitch(); granted = lease->hasExclusiveSwitchWindow();
                restored = lease->restore();
            });
            worker.join();
            require(closed && granted && restored == expected && !lease->hasExclusiveSwitchWindow(),
                "exact live switch grant exists only between old-device close and restoration");
            require(switchEngine.returnDeviceLease(lease) && !lease, "real owner accepts completed switch rollback");
        };

        auto lease = takePlaying();
        const auto frozen = switchEngine.getCurrentPositionSeconds();
        const auto state = switchDocument.getState();
        juce::Thread::sleep(10);
        require(switchEngine.getCurrentPositionSeconds() == frozen && switchDocument.getState() == state,
            "detached device cannot advance the real transport or mutate the document");
        restore(lease, true);
        require(switchEngine.isPlaying() && switchEngine.getCurrentPositionSeconds() == frozen
            && switchEngine.getPlayStartSeconds() == originalStart && switchEngine.isLoopEnabled()
            && switchManager->getAudioDeviceSetup() == switchSetup,
            "successful rollback restores exact configuration, playhead, play-start and loop intent");
        auto* resumedDevice = dynamic_cast<FixtureDevice*>(switchManager->getCurrentAudioDevice());
        resumedDevice->renderOutputs();
        const auto resumedAudio = resumedDevice->renderOutputs();
        require(resumedAudio.getMagnitude(1, 0, 128) > 0.1f && resumedAudio.getMagnitude(3, 0, 128) > 0.2f
            && resumedAudio.getMagnitude(0, 0, 128) == 0 && resumedAudio.getMagnitude(2, 0, 128) == 0,
            "restored source callback actually plays on the same physical outputs");

        lease = takePlaying();
        switchEngine.pause();
        restore(lease, true);
        require(switchEngine.getPlaybackState() == AudioEngine::PlaybackState::paused,
            "pause during the switch supersedes its original resume intent");
        switchEngine.play(); lease = takePlaying();
        switchEngine.seekToSeconds(0.025);
        restore(lease, true);
        require(switchEngine.isPlaying() && std::abs(switchEngine.getCurrentPositionSeconds() - 0.025) < 0.00001
            && std::abs(switchEngine.getPlayStartSeconds() - 0.025) < 0.00001,
            "seek during switch resumes from the new position and updates playing seek origin");
        lease = takePlaying(); switchEngine.stop();
        const auto stoppedPosition = switchEngine.getCurrentPositionSeconds();
        restore(lease, true);
        require(switchEngine.getPlaybackState() == AudioEngine::PlaybackState::stopped
            && switchEngine.getCurrentPositionSeconds() == stoppedPosition,
            "stop during switch is not overwritten by an older result");

        lease = switchEngine.takeDeviceLeaseForSwitch();
        require(lease != nullptr, "a stopped transport can enter an explicit switch");
        const auto opens = switchControls.opens.load();
        switchEngine.play();
        require(!switchEngine.isPlaying() && switchControls.opens == opens,
            "play during transfer records intent without reopening an unowned device");
        require(switchDocument.applySelectionGain({0.0, 0.04}, 0.5f) && switchDocument.undo() && switchDocument.canRedo(),
            "real edit and Undo/Redo history remain usable with no output device");
        const auto editedState = switchDocument.getState();
        restore(lease, true);
        require(switchEngine.isPlaying() && switchDocument.getState() == editedState && switchDocument.canRedo(),
            "queued play resumes while retaining the edited document and full redo history");

        lease = takePlaying(); switchControls.failNext = true;
        restore(lease, false);
        const auto failedOpens = switchControls.opens.load();
        switchEngine.play(); switchEngine.warmUpAudioDeviceAsync();
        require(!switchEngine.isPlaying() && !switchEngine.getRuntimeMetricsForDiagnostics()
            && switchControls.opens == failedOpens && switchDocument.canRedo(),
            "failed rollback remains unavailable without fallback or discarded document history");
        require(switchEngine.applyAudioDeviceSetup(switchSetup).isEmpty(), "explicit configuration can recover output after failed rollback");
        switchEngine.play(); lease = takePlaying();
        const auto oldIdentity = switchDocument.getMediaIdentity().document;
        require(switchEngine.loadFile(artifacts.getChildFile("physical-route-source.wav"))
            && !switchDocument.getMediaIdentity().document.sameDocument(oldIdentity),
            "reopening the same path creates a distinct document during the switch");
        restore(lease, true);
        require(!switchEngine.isPlaying(), "old document's switch result cannot start a replacement document");
        std::cout << "Device switch ownership passed: actual transport pause/resume, exact physical outputs, latest user intent, unavailable restore and Undo/Redo\n";
    }
    {
        Controls oldControls, newControls;
        SingleTrackDocument commitDocument;
        auto commitEngine = std::make_unique<AudioEngine>(commitDocument);
        AudioEngineDeviceTestAccess::install(*commitEngine, std::make_unique<FixtureManager>(oldControls));
        auto* oldManager = commitEngine->getDeviceManagerForConfiguration();
        require(commitEngine->loadFile(artifacts.getChildFile("physical-route-source.wav")), "commit retains the real document source");
        commitEngine->setLoopEnabled(true); commitEngine->setMonitorGainLinear(1.0f); commitEngine->play();
        const auto takeOld = [&]
        {
            auto* device = dynamic_cast<FixtureDevice*>(oldManager->getCurrentAudioDevice());
            std::jthread callbacks([&](std::stop_token stop) { while (!stop.stop_requested()) { device->renderOutputs(); juce::Thread::sleep(1); } });
            auto lease = commitEngine->takeDeviceLeaseForSwitch();
            callbacks.request_stop(); callbacks.join();
            require(lease != nullptr, "actual playing manager enters commit window"); return lease;
        };
        auto lease = takeOld();
        std::unique_ptr<DeviceOutput> candidate = std::make_unique<FixtureOutput>(newControls);
        require(!AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate),
            "engine rejects commit before its exact old owner has closed");
          std::jthread closeOld([&] { require(lease->closeForSwitch(), "worker closes old device for commit"); }); closeOld.join();
          const auto currentBus = AudioEngineDeviceTestAccess::bus();
          auto draft = DeviceRoutePlan::propose(currentBus, candidate->snapshot());
          draft.outputsBySpeaker = {{3}, {}, {1}};
          const auto plan = DeviceRoutePlan::validate(draft, candidate->snapshot(), currentBus).approved();
          auto staleBus = currentBus; --staleBus.revision;
          auto otherDevice = candidate->snapshot(); otherDevice.stableId += ":other";
          auto otherDraft = DeviceRoutePlan::propose(currentBus, otherDevice); otherDraft.outputsBySpeaker = draft.outputsBySpeaker;
          require(!AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate, {}, currentBus)
              && !AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate, plan, staleBus)
              && !AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate,
                  DeviceRoutePlan::validate(otherDraft, otherDevice, currentBus).approved(), currentBus)
              && !AudioEngineDeviceTestAccess::route(*commitEngine),
              "actual commit rejects missing approval, changed canonical revision and another exact device identity");
        newControls.failAttach = true;
          require(!AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate) && lease->hasExclusiveSwitchWindow()
              && candidate && !commitEngine->isPlaying() && !AudioEngineDeviceTestAccess::route(*commitEngine)
              && commitEngine->getTransportChannelCapacityForDiagnostics() == 2,
              "failed callback attachment restores the old route and transport capacity before controlled rollback");
        candidate.reset(); newControls.failAttach = false;
        std::jthread reopenOld([&] { require(lease->restore(), "failed commit restores exact old manager"); }); reopenOld.join();
        require(commitEngine->returnDeviceLease(lease) && commitEngine->isPlaying(), "failed commit returns actual old playback");

        lease = takeOld();
        std::jthread closeAgain([&] { require(lease->closeForSwitch(), "worker closes old owner before successful commit"); }); closeAgain.join();
        auto direct = std::make_unique<FixtureOutput>(newControls);
        auto* directView = direct.get(); candidate = std::move(direct);
        const auto beforeCommit = commitDocument.getState();
        const auto position = commitEngine->getCurrentPositionSeconds();
        require(AudioEngineDeviceTestAccess::commit(*commitEngine, lease, candidate) && !candidate
            && !lease->hasExclusiveSwitchWindow() && !lease->restore() && commitEngine->isPlaying()
            && commitEngine->getCurrentPositionSeconds() == position && commitDocument.getState() == beforeCommit,
            "successful internal commit transfers one output and preserves the real document/transport");
        std::jthread retireOld([&] { lease.reset(); }); retireOld.join();
        const auto oldOpens = oldControls.opens.load();
        directView->render(); const auto committedAudio = directView->render();
        require(committedAudio.getMagnitude(1, 0, 128) > 0.1f && committedAudio.getMagnitude(3, 0, 128) > 0.2f
            && committedAudio.getMagnitude(0, 0, 128) == 0 && committedAudio.getMagnitude(2, 0, 128) == 0,
            "actual source-player callback delivers document samples to the committed direct outputs");
        const auto metrics = commitEngine->getRuntimeMetricsForDiagnostics();
        const auto active = commitEngine->getDeviceDiagnostics();
        require(metrics && metrics->backend == "Fixture Direct" && metrics->callbackLoadFraction < 0 && metrics->totalXruns < 0
            && active.outputChannelMapping && (*active.outputChannelMapping)[0] == 1 && (*active.outputChannelMapping)[1] == 3
            && !commitEngine->getDeviceManagerForConfiguration() && oldControls.opens == oldOpens && oldControls.aliveDevices == 0,
            "committed output reports actual mapping, unknown counters and no hidden replacement manager");
        std::jthread directCallbacks([&](std::stop_token stop)
            { while (!stop.stop_requested()) { directView->render(); juce::Thread::sleep(1); } });
        lease = commitEngine->takeDeviceLeaseForSwitch();
        directCallbacks.request_stop(); directCallbacks.join();
        require(lease && lease->originalBackend() == "Fixture Direct", "next switch takes the committed direct owner itself");
        require(lease->closeForSwitch(), "direct owner closes before another candidate is attached");
        const auto previousPlan = AudioEngineDeviceTestAccess::route(*commitEngine);
        Controls rejectedControls; rejectedControls.failAttach = true;
        std::unique_ptr<DeviceOutput> rejectedOutput = std::make_unique<FixtureOutput>(rejectedControls);
        auto replacementBus = currentBus; ++replacementBus.revision;
        replacementBus.speakers.push_back({"centre", SpeakerRole::C, false});
        auto replacementDraft = DeviceRoutePlan::propose(replacementBus, rejectedOutput->snapshot());
        replacementDraft.outputsBySpeaker = {{1}, {}, {3}, {}};
        require(!AudioEngineDeviceTestAccess::commit(*commitEngine, lease, rejectedOutput,
            DeviceRoutePlan::validate(replacementDraft, rejectedOutput->snapshot(), replacementBus).approved(), replacementBus)
            && AudioEngineDeviceTestAccess::route(*commitEngine) == previousPlan
            && commitEngine->getTransportChannelCapacityForDiagnostics() == 3,
            "failed direct-to-direct commit restores the exact previous plan and canonical capacity");
        rejectedOutput.reset();
        std::jthread restoreDirect([&] { require(lease->restore(), "worker reopens the exact previous direct output"); }); restoreDirect.join();
        require(commitEngine->returnDeviceLease(lease) && commitEngine->isPlaying(), "direct-owner rollback reattaches actual source callback");
        directView->render(); require(directView->render().getMagnitude(1, 0, 128) > 0.1f, "direct-owner rollback really resumes audio");
        std::jthread failedResumeCallbacks([&](std::stop_token stop)
            { while (!stop.stop_requested()) { directView->render(); juce::Thread::sleep(1); } });
        lease = commitEngine->takeDeviceLeaseForSwitch();
        failedResumeCallbacks.request_stop(); failedResumeCallbacks.join();
        require(lease && lease->closeForSwitch() && lease->restore(), "direct device can reopen before a renderer attachment failure");
        newControls.failAttach = true;
        require(commitEngine->returnDeviceLease(lease) && !commitEngine->isPlaying() && !commitEngine->getRuntimeMetricsForDiagnostics(),
            "reattachment failure is unavailable even when worker device reopening succeeded");
        const auto finalOpens = newControls.opens.load();
        commitEngine->play(); commitEngine->warmUpAudioDeviceAsync();
        require(newControls.opens == finalOpens && !commitEngine->getDeviceManagerForConfiguration(),
            "failed direct restore never reopens implicitly or falls back to a system manager");
        commitEngine.reset();
        require(newControls.aliveDevices == 0 && oldControls.aliveDevices == 0, "engine teardown drains both old and committed device owners");
          std::cout << "Device output commit passed: real engine callback, failed attachment rollback, direct-owner retransfer and destruction\n";
      }
      for (int routeCase = 0; routeCase < 3; ++routeCase)
      {
          Controls routeControls;
          SingleTrackDocument routeDocument;
          AudioEngine routeEngine(routeDocument);
          require(routeEngine.loadFile(artifacts.getChildFile("physical-route-source.wav")), "route matrix uses the actual Document reader");
          routeEngine.setMonitorGainLinear(1.0f);
          ChannelOutputMapping legacy;
          legacy.setOutputChannel(SpeakerRole::L, 1); legacy.setOutputChannel(SpeakerRole::R, 0);
          routeEngine.setOutputMapping(legacy);
          auto lease = routeEngine.takeDeviceLeaseForSwitch();
          require(lease && lease->closeForSwitch(), "cold explicit switch obtains an empty owner window");
          auto direct = std::make_unique<FixtureOutput>(routeControls); auto* view = direct.get();
          std::unique_ptr<DeviceOutput> output = std::move(direct);
          const auto bus = AudioEngineDeviceTestAccess::bus();
          auto draft = DeviceRoutePlan::propose(bus, output->snapshot());
          draft.outputsBySpeaker = routeCase == 0 ? std::vector<std::vector<int>>{{1}, {}, {3}}
              : routeCase == 1 ? std::vector<std::vector<int>>{{}, {}, {1, 3}}
                               : std::vector<std::vector<int>>{{1}, {3}, {1}};
          const auto review = DeviceRoutePlan::validate(draft, output->snapshot(), bus);
          require(routeCase == 0 || !review.approved(), "unmapped and convergent render drafts require explicit review confirmation");
          const auto plan = review.approved(true);
          require(AudioEngineDeviceTestAccess::commit(routeEngine, lease, output, plan, bus)
              && routeEngine.getTransportChannelCapacityForDiagnostics() == 3,
              "explicit three-row canonical bus prepares independently from two physical outputs and stereo source");
          auto meterLease = routeEngine.acquireOutputMeterAnalysisLease();
          lease.reset(); routeEngine.play(); view->render(); const auto rendered = view->render();
          const auto expectedLeft = routeCase == 0 ? 0.25f : routeCase == 1 ? 0.125f : 0.375f;
          const auto expectedRight = routeCase == 2 ? 0.0f : 0.125f;
          require(std::abs(rendered.getSample(1, 100) - expectedLeft) < 1.0e-5f
              && std::abs(rendered.getSample(3, 100) - expectedRight) < 1.0e-5f
              && rendered.getMagnitude(0, 0, 128) == 0 && rendered.getMagnitude(2, 0, 128) == 0,
              "actual renderer preserves unity permutation/copy/sum and independent silent LFE on noncontiguous physical outputs");
          const auto meter = routeEngine.getOutputMeterSnapshot();
          require(meter.channelCount == 2 && meter.roles[0] == (routeCase == 0 ? SpeakerRole::R : routeCase == 1 ? SpeakerRole::L : SpeakerRole::unknown)
              && meter.roles[1] == (routeCase == 2 ? SpeakerRole::LFE : SpeakerRole::L),
              "post-route meter roles reflect actual contributors and do not label a merged output as one speaker");
          std::jthread callbacks([&](std::stop_token stop) { while (!stop.stop_requested()) { view->render(); juce::Thread::sleep(1); } });
          routeEngine.pause(); callbacks.request_stop(); callbacks.join();
          routeEngine.pingSpeakerRole(SpeakerRole::L); const auto ping = view->render();
          require((ping.getMagnitude(1, 0, 128) > 0) == (routeCase != 0)
              && (ping.getMagnitude(3, 0, 128) > 0) == (routeCase != 2),
              "post-route speaker ping follows the approved connections, not old role-slot guesses");
      }
      std::cout << "Device route integration passed: canonical bus width/order, real unity permutation/copy/sum, LFE silence and post-route ping\n";
      {
          std::array<juce::AudioBuffer<float>, 3> ears;
          const auto source = artifacts.getChildFile("directional-binaural-route.wav");
          juce::MemoryOutputStream wave;
          wave.write("RIFF", 4); wave.writeInt(36 + 4096 * 8); wave.write("WAVEfmt ", 8);
          wave.writeInt(16); wave.writeShort(1); wave.writeShort(4); wave.writeInt(48000);
          wave.writeInt(384000); wave.writeShort(8); wave.writeShort(16); wave.write("data", 4); wave.writeInt(4096 * 8);
          for (int frame = 0; frame < 4096; ++frame)
          {
              const auto signal = static_cast<short>(4096 * std::sin(frame * 0.037));
              wave.writeShort(signal); wave.writeShort(signal); wave.writeShort(0); wave.writeShort(0);
          }
          require(source.replaceWithData(wave.getData(), wave.getDataSize()), "retain directional FOA fixture");
          for (int earCase = 0; earCase < 3; ++earCase)
          {
              Controls earControls;
              SingleTrackDocument earDocument; AudioEngine earEngine(earDocument);
              require(earEngine.loadFile(source) && earDocument.setChannelLayoutPreset("foa-acn-sn3d")
                  && earDocument.setAmbisonicMonitorMode(AmbisonicMonitorMode::binauralHrtf), "actual route renders the bundled HRTF");
              earEngine.setMonitorGainLinear(1.0f);
              auto lease = earEngine.takeDeviceLeaseForSwitch(); require(lease && lease->closeForSwitch(), "prepare explicit binaural output");
              auto direct = std::make_unique<FixtureOutput>(earControls); auto* view = direct.get();
              std::unique_ptr<DeviceOutput> output = std::move(direct);
              auto bus = AudioEngineDeviceTestAccess::bus();
              if (earCase == 0) bus.speakers = {{"left", SpeakerRole::L}, {"right", SpeakerRole::R}};
              auto draft = DeviceRoutePlan::propose(bus, output->snapshot());
              draft.outputsBySpeaker = earCase == 0 ? std::vector<std::vector<int>>{{1}, {3}}
                  : earCase == 1 ? std::vector<std::vector<int>>{{1}, {}, {3}}
                                 : std::vector<std::vector<int>>{{}, {1}, {3}};
              require(AudioEngineDeviceTestAccess::commit(earEngine, lease, output,
                  DeviceRoutePlan::validate(draft, output->snapshot(), bus).approved(true), bus), "commit exact binaural canonical order");
              lease.reset(); earEngine.play();
              for (int block = 0; block < 6; ++block) ears[static_cast<std::size_t>(earCase)] = view->render();
          }
          float earDifference = 0.0f;
          for (int sample = 0; sample < 128; ++sample)
          {
              earDifference += std::abs(ears[0].getSample(1, sample) - ears[0].getSample(3, sample));
              require(std::abs(ears[1].getSample(1, sample) - ears[0].getSample(3, sample)) < 1.0e-6f
                  && std::abs(ears[1].getSample(3, sample) - ears[0].getSample(1, sample)) < 1.0e-6f
                  && ears[2].getSample(1, sample) == 0
                  && std::abs(ears[2].getSample(3, sample) - ears[0].getSample(1, sample)) < 1.0e-6f,
                  "canonical binaural reordering preserves distinct ears and never writes directional content to LFE");
          }
          require(earDifference > 0.001f, "directional source distinguishes the two real HRTF outputs");
          std::cout << "Binaural device route passed: actual distinct HRTF ears, canonical reorder and independent LFE silence\n";
      }
      {
          Controls geometryControls;
          SingleTrackDocument geometryDocument; AudioEngine geometryEngine(geometryDocument);
          require(geometryEngine.loadFile(artifacts.getChildFile("ambisonic-route-source.wav"))
              && geometryDocument.setChannelLayoutPreset("foa-acn-sn3d"), "custom geometry uses a real FOA Document");
          SpeakerGeometryProfile profile;
          profile.profileId = "fixture-custom-route"; profile.revision = 7;
          const std::array customRoles {SpeakerRole::Rs, SpeakerRole::R, SpeakerRole::L, SpeakerRole::Ls};
          for (int index = 0; index < 4; ++index)
          {
              SpeakerGeometrySpeaker speaker;
              speaker.stableId = "custom-" + juce::String(index); speaker.label = speaker.stableId;
              speaker.role = customRoles[static_cast<std::size_t>(index)];
              speaker.azimuthDegrees = -135.0f + static_cast<float>(index) * 90.0f;
              profile.speakers.push_back(speaker); // explicit non-default order and stable IDs
          }
          const auto generated = AmbisonicSpeakerGeometryGenerator::generate({1, profile.revision, 1, profile});
          require(generated.succeeded() && geometryEngine.activateAmbisonicDecodeMatrixArtifact(generated.artifact),
              "generate and activate the real custom canonical decode artifact");
          geometryEngine.setMonitorGainLinear(1.0f);
          auto lease = geometryEngine.takeDeviceLeaseForSwitch(); require(lease && lease->closeForSwitch(), "custom bus prepares explicit output");
          auto direct = std::make_unique<FixtureOutput>(geometryControls); auto* view = direct.get();
          std::unique_ptr<DeviceOutput> output = std::move(direct);
          DeviceRoutePlan::Bus bus {profile.profileId.toStdString(), profile.revision, {}};
          for (const auto& speaker : profile.speakers) bus.speakers.push_back({speaker.stableId.toStdString(), speaker.role});
          auto draft = DeviceRoutePlan::propose(bus, output->snapshot());
          draft.outputsBySpeaker = {{3}, {1}, {}, {}};
          require(AudioEngineDeviceTestAccess::commit(geometryEngine, lease, output,
              DeviceRoutePlan::validate(draft, output->snapshot(), bus).approved(true), bus), "commit exact custom stable-ID bus");
          lease.reset(); geometryEngine.play(); view->render(); const auto audio = view->render();
          require(audio.getMagnitude(1, 0, 128) > 0.001f
              && std::abs(audio.getSample(1, 100) - generated.artifact->decodeMatrix.gain(1, 0) * 0.25f) < 1.0e-5f
              && std::abs(audio.getSample(3, 100) - generated.artifact->decodeMatrix.gain(0, 0) * 0.25f) < 1.0e-5f,
              "actual custom decoder rows reach their explicit physical outputs without physical-count layout inference");
          ++profile.revision; profile.speakers[0].azimuthDegrees += 1.0f;
          const auto replacement = AmbisonicSpeakerGeometryGenerator::generate({2, profile.revision, 1, profile});
          require(replacement.succeeded() && geometryEngine.activateAmbisonicDecodeMatrixArtifact(replacement.artifact), "publish another real geometry revision");
          const auto invalidated = view->render();
          require(invalidated.getMagnitude(0, 128) == 0 && !geometryEngine.getRuntimeMetricsForDiagnostics(),
              "changed geometry silences stale binding and reports unavailable instead of guessing a compatible row count");
          std::cout << "Custom geometry device route passed: stable-ID canonical samples and live revision invalidation\n";
      }
      runDeviceSwitchCoordinatorTests(artifacts);
      runAsioIdentityDiagnosticsTests(artifacts);
      runCapabilityInspectionTests(artifacts);
      runAsioSettingsTests(artifacts);
      runWindowsOutputSettingsTests(artifacts);
      runManagedOutputSessionTests(artifacts);
      runFollowSourceTests(artifacts);
      runSelectedAsioPanelTests(artifacts);
      runSpatialEnergyEngineTests(artifacts);
      runSpatialEnergyMultichannelTests(artifacts);
      juce::DynamicObject::Ptr outcomes = new juce::DynamicObject;
    outcomes->setProperty("negotiated", negotiated); outcomes->setProperty("failed", failed); outcomes->setProperty("recovered", recovered);
    require(artifacts.getChildFile("device-outcomes.json").replaceWithText(juce::JSON::toString(juce::var(outcomes.get()), false)), "retain real manager/provider outcome evidence");
    std::cout << "AudioDeviceDiagnosticsTests passed (real manager initialization, requested versus active values, failure/reopen IDs, Preferences callbacks, passive and busy capture; controlled IODevice, not hardware acceptance)\n";
}
}
