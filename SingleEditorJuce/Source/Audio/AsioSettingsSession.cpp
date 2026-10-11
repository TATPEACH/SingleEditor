#include "Audio/AsioSettingsSession.h"
#include "Audio/WindowsOutputBackend.h"
#include "Audio/AmbisonicSpeakerGeometry.h"
#include "Diagnostics/DeviceDiagnosticProbe.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace singleeditor
{
namespace
{
DeviceRoutePlan::Bus busFor(const SpeakerGeometryProfile& profile)
{
    DeviceRoutePlan::Bus bus { profile.profileId.toStdString(), profile.revision, {} };
    for (const auto& speaker : profile.speakers)
        if (speaker.enabled && speaker.available)
            bus.speakers.push_back({ speaker.stableId.toStdString(), speaker.role, true });
    return bus;
}
// Same explicit 250 ms / -40 dBFS / 100 Hz physical ping as Local Diagnostics.
// One compact output, no transport or Document access, no callback allocation.
class PhysicalPing final : public juce::AudioIODeviceCallback
{
public:
    explicit PhysicalPing(double sampleRate) : rate(sampleRate), target(static_cast<int>(rate * diagnostics::DeviceDiagnosticProbe::durationMs / 1000.0)) {}
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
        int channels, int samples, const juce::AudioIODeviceCallbackContext&) override
    {
        auto frame = frames.load(std::memory_order_relaxed);
        for (int sample = 0; sample < samples; ++sample, ++frame)
        {
            const auto ramp = frame < target ? std::max(0.0, std::min({1.0, frame / (rate * .01), (target - 1 - frame) / (rate * .01)})) : 0.0;
            const auto value = static_cast<float>(.01 * ramp * std::sin(juce::MathConstants<double>::twoPi * diagnostics::DeviceDiagnosticProbe::toneHz * frame / rate));
            for (int channel = 0; channel < channels; ++channel)
                if (outputs[channel]) outputs[channel][sample] = channel == 0 ? value : 0.0f;
        }
        frames.store(frame, std::memory_order_release);
    }
    bool done() const { return frames.load(std::memory_order_acquire) >= target; }
private:
    double rate;
    int target;
    std::atomic<int> frames {0};
};
juce::String failureText(AsioSettingsSession::Switch::Error error)
{
    using Error = AsioSettingsSession::Switch::Error;
    switch (error)
    {
        case Error::cancelled: return "Cancelled; previous output returned.";
        case Error::superseded: return "Selection changed; previous output returned. Read capabilities again.";
        case Error::restoreFailed: return "Previous output could not be restored. Audio output is unavailable.";
        case Error::engineBusy: return "Another audio device operation is in progress. Try again when it finishes.";
        case Error::driverUnresolved: return "Driver did not finish in time. Waiting for it to release the device.";
        case Error::staleCandidate: return "Driver capabilities or configuration changed. Read capabilities again.";
        case Error::rebindRequired: return "Physical outputs changed. Output stopped; read capabilities, Validate and Ping, then rebind.";
        case Error::circuitOpen: return "Repeated driver resets: automatic recovery stopped. Retry output, open Control Panel, or choose another device. Editing remains available.";
        default: return "Device operation failed; no new route was activated. Check the previous output status.";
    }
}
}
struct AsioSettingsSession::Discovery
{
    std::atomic<bool> done {false}, cancelled {false};
    Catalog result;
};
AsioSettingsSession::Backend AsioSettingsSession::defaultBackend()
{
    return {
        [](const std::string& backendName) -> Catalog
        {
            if (backendName == "ASIO") { Adapter adapter; return adapter.scanDevices(Adapter::Activation::asioSettings); }
            WindowsOutputBackend adapter(backendName); const auto found = adapter.discover();
            Catalog result;
            result.status = found.status == WindowsOutputBackend::Status::ready ? Adapter::Status::available : Adapter::Status::unavailable;
            for (const auto& output : found.outputs)
            {
                result.drivers.push_back({output.displayName.toStdString(), output.stableId.toStdString()});
                if (output.isDefault) result.defaultOutputId = output.stableId.toStdString();
            }
            return result;
        },
        [](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Inspected
        {
            if (isWindowsOutputBackend(choice.backend))
            {
                WindowsOutputBackend adapter(choice.backend);
                auto result = adapter.inspect(choice.stableId, access, cancelled);
                if (!result.capabilities) return {{}, cancelled.load() ? Switch::Error::cancelled : Switch::Error::prepareFailed};
                return {std::make_shared<const PreparedDeviceCapabilities>(std::move(*result.capabilities)), Switch::Error::none};
            }
            auto result = Adapter::inspectSelectedCapabilities(Adapter::Activation::asioSettings, choice.stableId, access, cancelled);
            if (!result.capabilities) return {{}, cancelled.load() ? Switch::Error::cancelled : Switch::Error::prepareFailed};
            return { std::make_shared<const Adapter::PreparedCapabilities>(std::move(*result.capabilities)), Switch::Error::none };
        },
        [](const Switch::Selection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Prepared
        {
            if (isWindowsOutputBackend(choice.expected.backend))
            {
                WindowsOutputBackend adapter(choice.expected.backend);
                auto result = adapter.prepare(choice.expected, access, cancelled);
                return {std::move(result.output), cancelled.load() ? Switch::Error::cancelled
                    : result.status == WindowsOutputBackend::Status::configurationChanged ? Switch::Error::staleCandidate
                    : result.status == WindowsOutputBackend::Status::ready ? Switch::Error::none : Switch::Error::prepareFailed};
            }
            auto result = Adapter::prepareSelectedOutput(Adapter::Activation::userSelection, choice.expected.stableId,
                {choice.expected.sampleRate, choice.expected.bufferSamples, choice.expected.physicalOutputs}, access, cancelled);
            return { std::move(result.output), cancelled.load() ? Switch::Error::cancelled : Switch::Error::none };
        },
        [](const Switch::InspectionSelection& choice, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled,
            std::atomic<bool>& awaitingPanel) -> Switch::Inspected
        {
            if (choice.backend != "ASIO") return {{}, Switch::Error::invalidSelection};
            auto result = Adapter::inspectSelectedCapabilities(Adapter::Activation::asioSettings, choice.stableId, access, cancelled, &awaitingPanel);
            if (!result.capabilities) return {{}, cancelled.load() ? Switch::Error::cancelled : Switch::Error::prepareFailed};
            return {std::make_shared<const Adapter::PreparedCapabilities>(std::move(*result.capabilities)), Switch::Error::none};
        }
    };
}
AsioSettingsSession::AsioSettingsSession(AudioEngine& value, juce::ApplicationProperties& settings)
    : AsioSettingsSession(value, settings, defaultBackend()) {}
AsioSettingsSession::AsioSettingsSession(AudioEngine& value, juce::ApplicationProperties& settings, Backend implementation)
    : engine(&value), properties(settings), backend(std::move(implementation))
{
    refreshProfiles(); monitoringOutput = engine->readManagedOutputEvents(false).has_value();
    if (const auto* stored = properties.getUserSettings()) followSource = stored->getBoolValue(followSourceKey, false);
    engine->addListener(this);
    loadedFileChanged(engine->getLoadedFileInfo());
}
void AsioSettingsSession::refreshProfiles()
{
    const auto selectedProfileId = profileIndex >= 0 && profileIndex < static_cast<int>(availableProfiles.size())
        ? availableProfiles[static_cast<size_t>(profileIndex)].bus.profileId : std::string{};
    availableProfiles.clear(); profileIndex = -1;
    for (const auto& id : AmbisonicStandardMatrixCatalog::standardProfileIds())
        if (const auto profile = AmbisonicStandardMatrixCatalog::standardProfile(id))
            availableProfiles.push_back({profile->displayName, busFor(*profile)});
    if (engine->isCustomAmbisonicDecodeMatrixActiveForDiagnostics())
        if (const auto artifact = engine->getAmbisonicDecodeMatrixArtifactForDiagnostics())
            availableProfiles.push_back({"Current: " + artifact->geometry.displayName, busFor(artifact->geometry)});
    for (size_t index = 0; index < availableProfiles.size(); ++index)
        if (availableProfiles[index].bus.profileId == selectedProfileId) profileIndex = static_cast<int>(index);
}
AsioSettingsSession::~AsioSettingsSession() { shutdown(); }
void AsioSettingsSession::shutdown()
{
    if (engine) engine->removeListener(this);
    if (discovery) discovery->cancelled.store(true);
    if (operation) operation->abandon();
    discovery.reset(); operation.reset(); engine = nullptr;
}
void AsioSettingsSession::changed(const juce::String& text)
{ if (busy()) liveStatus.clear(); statusText = text; sendChangeMessage(); }
void AsioSettingsSession::invalidate() { ++generation; reviewed.reset(); approved.reset(); }
void AsioSettingsSession::selectBackend(const std::string& value)
{
    if (!engine || busy() || backendId == value || (value != "ASIO" && !isWindowsOutputBackend(value))) return;
    invalidate(); backendId = value; selectedId.clear(); catalog = {}; facts.reset(); rate = 0; buffer = 0;
    rebuildDraft(); changed("Backend selected. Refresh its device list, then choose an output.");
}
void AsioSettingsSession::refreshDrivers()
{
    if (!engine || busy()) return;
    invalidate(); facts.reset(); refreshProfiles(); rebuildDraft();
    auto pending = std::make_shared<Discovery>();
    discovery = pending; currentActivity = Activity::discovery;
    changed(backendId == "ASIO" ? "Looking for registered ASIO drivers..." : "Looking for Windows output endpoints...");
    if (!juce::Thread::launch([pending, discover = backend.discover, selectedBackend = backendId]
    {
        try { if (!pending->cancelled.load()) pending->result = discover(selectedBackend); }
        catch (...) { pending->result.status = Adapter::Status::unavailable; }
        pending->done.store(true, std::memory_order_release);
    }))
    { discovery.reset(); currentActivity = Activity::idle; changed("Could not start device discovery."); }
}
void AsioSettingsSession::chooseWindowsDefault()
{
    if (!engine || busy()) return;
    selectBackend("Windows Audio");
    selectingWindowsDefault = true;
    refreshDrivers();
    if (!discovery) selectingWindowsDefault = false;
}
void AsioSettingsSession::selectDriver(const std::string& id)
{
    if (!engine || busy() || selectedId == id) return;
    if (std::none_of(catalog.drivers.begin(), catalog.drivers.end(), [&](const auto& driver) { return !id.empty() && driver.stableId == id; })) return;
    cancel(); invalidate(); selectedId = id; facts.reset(); rate = 0; buffer = 0; rebuildDraft();
    changed("Output selected. Read capabilities before editing its route.");
}
AsioSettingsSession::Switch::InspectionSelection AsioSettingsSession::inspectionSelection() const
{ return {generation, backendId, selectedId}; }
void AsioSettingsSession::inspect()
{
    if (!engine || busy() || selectedId.empty()) return;
    invalidate(); facts.reset(); rebuildDraft(); currentActivity = Activity::inspection;
    operation = Switch::startInspection(*engine, inspectionSelection(), backend.inspect);
    lastPhase = Switch::Phase::inactive; changed("Reading capabilities; audio will resume after the driver is released.");
}
DeviceRoutePlan::Bus AsioSettingsSession::currentBus() const
{
    if (engine && engine->isCustomAmbisonicDecodeMatrixActiveForDiagnostics())
    {
        if (const auto artifact = engine->getAmbisonicDecodeMatrixArtifactForDiagnostics()) return busFor(artifact->geometry);
        return {}; // No stale geometry may authorise a route.
    }
    return profileIndex >= 0 && profileIndex < static_cast<int>(availableProfiles.size()) ? availableProfiles[static_cast<size_t>(profileIndex)].bus : DeviceRoutePlan::Bus{};
}
void AsioSettingsSession::rebuildDraft()
{
    routing = {backendId, selectedId, facts ? facts->capabilities.outputNames : std::vector<std::string>{}, {}, {}};
    if (profileIndex >= 0 && profileIndex < static_cast<int>(availableProfiles.size())) routing.bus = availableProfiles[static_cast<size_t>(profileIndex)].bus;
    routing.outputsBySpeaker.resize(routing.bus.speakers.size());
}
void AsioSettingsSession::selectProfile(int index)
{
    if (!engine || busy() || index < 0 || index >= static_cast<int>(availableProfiles.size()) || index == profileIndex) return;
    invalidate(); profileIndex = index; rebuildDraft(); changed("Assign each canonical speaker to physical outputs, then Validate.");
}
void AsioSettingsSession::setFormat(double value, int samples)
{
    if (!engine || busy() || (rate == value && buffer == samples)) return;
    invalidate(); rate = value; buffer = samples; changed("Format changed. Validate the draft again.");
}
void AsioSettingsSession::setConnection(int row, int physical, bool connected)
{
    if (!engine || busy() || row < 0 || row >= static_cast<int>(routing.outputsBySpeaker.size()) || physical < 0 || physical >= static_cast<int>(routing.outputNames.size())) return;
    auto& outputs = routing.outputsBySpeaker[static_cast<size_t>(row)];
    const auto found = std::find(outputs.begin(), outputs.end(), physical);
    if (connected == (found != outputs.end())) return;
    if (connected) { outputs.push_back(physical); std::sort(outputs.begin(), outputs.end()); }
    else outputs.erase(found);
    invalidate(); changed("Draft changed. Validate before activation.");
}
bool AsioSettingsSession::validFormat() const
{
    if (!facts || facts->stableId != selectedId) return false;
    const auto& c = facts->capabilities;
    return std::isfinite(rate) && rate > 0 && std::find(c.sampleRates.begin(), c.sampleRates.end(), rate) != c.sampleRates.end()
        && buffer > 0 && std::find(c.bufferSizes.begin(), c.bufferSizes.end(), buffer) != c.bufferSizes.end();
}
DeviceOutput::Snapshot AsioSettingsSession::expected() const
{
    DeviceOutput::Snapshot result;
    result.backend = backendId; result.stableId = selectedId; result.sampleRate = rate; result.bufferSamples = buffer;
    result.outputNames = facts ? facts->capabilities.outputNames : routing.outputNames;
    for (const auto& outputs : routing.outputsBySpeaker) result.physicalOutputs.insert(result.physicalOutputs.end(), outputs.begin(), outputs.end());
    std::sort(result.physicalOutputs.begin(), result.physicalOutputs.end());
    result.physicalOutputs.erase(std::unique(result.physicalOutputs.begin(), result.physicalOutputs.end()), result.physicalOutputs.end());
    return result;
}
AsioSettingsSession::Switch::Selection AsioSettingsSession::selection() const
{ return {generation, expected(), currentBus(), {}, approved}; }
void AsioSettingsSession::validate()
{
    if (!engine || busy()) return;
    reviewed.reset(); approved.reset();
    if (!validFormat()) { changed("Choose a supported sample rate and buffer size; no fallback is applied."); return; }
    reviewed = DeviceRoutePlan::validate(routing, expected(), currentBus());
    changed(!reviewed->isValid() ? "Route cannot be activated. Correct the listed errors and Validate again."
        : reviewed->needsConfirmation() ? "Review the routing warnings. Confirmation applies only to this draft."
        : "Route validated. Use physical Ping to check wiring, then Activate.");
}
void AsioSettingsSession::activate(bool confirm)
{
    if (!engine || busy() || !validFormat() || !reviewed) return;
    approved = reviewed->approved(confirm);
    if (!approved || !approved->matches(expected(), currentBus())) { approved.reset(); changed("Route is unconfirmed or has changed. Validate again."); return; }
    operation = Switch::start(*engine, selection(), Switch::Mode::exclusive, backend.prepare);
    restoringSavedSelection = false;
    currentActivity = Activity::activation; lastPhase = Switch::Phase::inactive;
    changed("Activating the exact reviewed device and route...");
}

bool AsioSettingsSession::hasSavedSelection(juce::ApplicationProperties& settings)
{
    const auto* stored = settings.getUserSettings();
    return stored && (stored->getValue(activeBackendKey).isNotEmpty() || stored->getValue(bindingKey).isNotEmpty());
}
juce::String AsioSettingsSession::savedBackend(juce::ApplicationProperties& settings)
{
    const auto* stored = settings.getUserSettings();
    return stored ? stored->getValue(activeBackendKey, "ASIO") : juce::String{};
}
void AsioSettingsSession::restoreSavedSelection()
{
    if (!engine || busy()) return;
    auto* settings = properties.getUserSettings();
    if (!settings || !hasSavedSelection(properties)) return;
    invalidate(); facts.reset(); refreshProfiles();
    const auto backendName = savedBackend(properties).toStdString();
    const auto fail = [this](const juce::String& reason)
    { restoringSavedSelection = false; changed(reason + " Open Output Setup to read capabilities and rebind; no fallback was selected."); };
    if (backendName != "ASIO" && !isWindowsOutputBackend(backendName)) { fail("Saved output backend is unsupported."); return; }
    backendId = backendName;
    // A retry can cross backends. A previous backend's catalog and draft are
    // not observations of the saved endpoint; keep only the saved expectations.
    catalog = {}; selectedId.clear(); rate = 0; buffer = 0; rebuildDraft();
    const auto text = settings->getValue(selectedBindingKey());
    if (text.length() > 131072) { fail("Saved output binding is not supported."); return; }
    const auto saved = juce::JSON::parse(text);
    const auto integer = [](const juce::var& value) { return value.isInt() || value.isInt64(); };
    if (!saved.isObject() || !integer(saved["schemaVersion"]) || static_cast<juce::int64>(saved["schemaVersion"]) != 1
        || saved["backend"].toString().toStdString() != backendId || !saved["stableId"].isString() || saved["stableId"].toString().isEmpty()
        || !integer(saved["bufferSamples"]) || !integer(saved["revision"]))
    { fail("Saved output binding is invalid."); return; }
    const auto* names = saved["outputNames"].getArray();
    const auto* rows = saved["speakers"].getArray();
    const auto requestedRate = static_cast<double>(saved["sampleRate"]);
    const auto storedBuffer = static_cast<juce::int64>(saved["bufferSamples"]);
    const auto requestedBuffer = static_cast<int>(saved["bufferSamples"]);
    if (!names || names->isEmpty() || names->size() > DeviceRoutePlan::maxDeviceChannels
        || !rows || rows->isEmpty() || rows->size() > DeviceRoutePlan::maxBusChannels
        || !(saved["sampleRate"].isDouble() || integer(saved["sampleRate"])) || !std::isfinite(requestedRate) || requestedRate <= 0
        || storedBuffer <= 0 || storedBuffer > std::numeric_limits<int>::max())
    { fail("Saved output format or channel capability is invalid."); return; }
    DeviceRoutePlan::Draft savedDraft;
    savedDraft.backend = backendId; savedDraft.stableId = saved["stableId"].toString().toStdString();
    savedDraft.bus.profileId = saved["profileId"].toString().toStdString();
    savedDraft.bus.revision = static_cast<juce::int64>(saved["revision"]);
    for (const auto& name : *names)
    {
        if (!name.isString()) { fail("Saved output channel names are invalid."); return; }
        savedDraft.outputNames.push_back(name.toString().toStdString());
    }
    for (const auto& row : *rows)
    {
        const auto* outputs = row["physicalOutputs"].getArray();
        const auto role = ChannelLayoutCatalog::roleFromAbbreviation(row["role"].toString());
        if (!row.isObject() || !outputs || outputs->size() > DeviceRoutePlan::maxDeviceChannels
            || !row["stableId"].isString() || row["stableId"].toString().isEmpty() || role == SpeakerRole::unknown
            || (row.hasProperty("required") && !row["required"].isBool()))
        { fail("Saved output speaker rows are invalid."); return; }
        savedDraft.bus.speakers.push_back({row["stableId"].toString().toStdString(), role, row.hasProperty("required") ? static_cast<bool>(row["required"]) : true});
        std::vector<int> connections;
        for (const auto& output : *outputs)
        {
            if (!integer(output) || static_cast<juce::int64>(output) < 0 || static_cast<juce::int64>(output) >= names->size())
            { fail("Saved output connection is invalid."); return; }
            connections.push_back(static_cast<int>(output));
        }
        savedDraft.outputsBySpeaker.push_back(std::move(connections));
    }
    const auto profile = std::find_if(availableProfiles.begin(), availableProfiles.end(), [&](const auto& value) { return value.bus == savedDraft.bus; });
    if (profile == availableProfiles.end()) { fail("Saved output geometry is not available at the recorded revision."); return; }
    profileIndex = static_cast<int>(std::distance(availableProfiles.begin(), profile));
    selectedId = savedDraft.stableId; rate = requestedRate; buffer = requestedBuffer; routing = std::move(savedDraft);
    reviewed = DeviceRoutePlan::validate(routing, expected(), currentBus());
    approved = reviewed->approved(saved["warningsConfirmed"].isBool() && static_cast<bool>(saved["warningsConfirmed"]));
    if (!approved) { fail("Saved output route needs a new validation or warning confirmation."); return; }
    // These are recorded expectations, not newly observed capabilities. The
    // worker opens only this explicit identity and the coordinator compares the
    // actual device against the entire frozen plan before committing it.
    restoringSavedSelection = true;
    operation = Switch::start(*engine, selection(), Switch::Mode::exclusive, backend.prepare);
    currentActivity = Activity::activation; lastPhase = Switch::Phase::inactive;
    changed("Preparing the saved " + juce::String(backendId) + " output and exact route; waveform and editing remain available...");
}
void AsioSettingsSession::ping(int physical)
{
    if (!engine || busy() || !validFormat() || !reviewed || !reviewed->isValid() || physical < 0 || physical >= static_cast<int>(routing.outputNames.size())) return;
    auto pingChoice = selection(); pingChoice.expected.physicalOutputs = {physical}; pingChoice.plan.reset();
    const auto capturedFacts = facts;
    operation = Switch::startInspection(*engine, inspectionSelection(),
        [prepare = backend.prepare, pingChoice, capturedFacts](const Switch::InspectionSelection&, const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled) -> Switch::Inspected
        {
            auto prepared = prepare(pingChoice, access, cancelled);
            if (!prepared.output || prepared.error != Switch::Error::none) return {{}, prepared.error == Switch::Error::none ? Switch::Error::prepareFailed : prepared.error};
            auto& output = *prepared.output;
            const auto& actual = output.snapshot(); const auto& wanted = pingChoice.expected;
            if (!output.isOpen() || actual.backend != wanted.backend || actual.stableId != wanted.stableId
                || actual.outputNames != wanted.outputNames || actual.physicalOutputs != wanted.physicalOutputs
                || actual.sampleRate != wanted.sampleRate || actual.bufferSamples != wanted.bufferSamples || cancelled.load())
                return {{}, cancelled.load() ? Switch::Error::cancelled : Switch::Error::staleCandidate};
            PhysicalPing tone(actual.sampleRate);
            struct DetachBeforeToneDies
            {
                DeviceOutput& output;
                ~DetachBeforeToneDies() { output.detach(); output.closeForSwitch(); }
            } cleanup {output};
            const auto attached = output.attach(tone);
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
            while (attached && !tone.done() && !cancelled.load() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
            output.detach(); output.closeForSwitch();
            if (cancelled.load()) return {{}, Switch::Error::cancelled};
            if (!attached || !tone.done()) return {{}, Switch::Error::prepareFailed};
            return {capturedFacts, Switch::Error::none};
        });
    currentActivity = Activity::ping; lastPhase = Switch::Phase::inactive;
    changed("Pinging physical output " + juce::String(physical + 1) + " at -40 dBFS for 250 ms...");
}
void AsioSettingsSession::cancel()
{
    if (currentActivity == Activity::followSource) followPending = false;
    if (discovery) discovery->cancelled.store(true);
    if (operation) operation->cancel();
    if (busy()) changed("Cancelling; waiting for driver release and previous output return...");
}
bool AsioSettingsSession::canMaintainOutput() const
{
    return engine && engine->readManagedOutputEvents(false).has_value() && engine->activeOutputPlan() != nullptr;
}
DeviceRoutePlan::Bus AsioSettingsSession::activeBus(const DeviceRoutePlan::Bus& recorded) const
{
    if (engine && engine->isCustomAmbisonicDecodeMatrixActiveForDiagnostics())
    {
        if (const auto artifact = engine->getAmbisonicDecodeMatrixArtifactForDiagnostics()) return busFor(artifact->geometry);
        return {};
    }
    return recorded;
}
void AsioSettingsSession::retryOutput()
{
    if (followSource && canFollowSourceSampleRate() && sourceRate > 0)
    {
        const auto output = engine->activeOutputSnapshot();
        if (output && output->sampleRate != sourceRate)
        {
            auto& history = resetHistory[output->stableId];
            if (!history.rebind) { history.blocked = false; history.attempts.clear(); }
            followPending = true; serviceFollowSource(); return;
        }
    }
    if (canMaintainOutput()) beginMaintenance(false, true);
    else restoreSavedSelection();
}
bool AsioSettingsSession::canOpenControlPanel() const
{
    if (!engine) return false;
    const auto active = engine->activeOutputSnapshot();
    if (backendId == "ASIO" && !selectedId.empty()
        && (!active || active->backend != "ASIO" || active->stableId != selectedId)) return static_cast<bool>(backend.panel);
    return canMaintainOutput();
}
void AsioSettingsSession::openControlPanel()
{
    if (!engine || busy() || !canOpenControlPanel()) return;
    const auto active = engine->activeOutputSnapshot();
    if (backendId != "ASIO" || selectedId.empty() || (active && active->backend == "ASIO" && active->stableId == selectedId))
    { beginMaintenance(true, true); return; }
    invalidate(); facts.reset(); refreshProfiles(); rebuildDraft();
    operation = Switch::startPanelInspection(*engine, inspectionSelection(), backend.panel);
    currentActivity = Activity::panel; lastPhase = Switch::Phase::inactive;
    changed("Opening the selected ASIO driver's Control Panel. Its capabilities will be read again before routing review.");
}
void AsioSettingsSession::beginMaintenance(bool panel, bool explicitRetry)
{
    if (!engine || busy() || !canMaintainOutput()) return;
    const auto output = engine->activeOutputSnapshot();
    const auto plan = engine->activeOutputPlan();
    if (!output || !plan || output->backend != "ASIO") return;
    // Active owner identity is independent of the editable settings draft.
    auto& history = resetHistory[output->stableId];
    if (history.rebind && !panel)
    {
        changed(failureText(Switch::Error::rebindRequired));
        return;
    }
    const auto now = juce::Time::getMillisecondCounterHiRes();
    while (!history.attempts.empty() && now - history.attempts.front() >= 30000.0) history.attempts.pop_front();
    if (explicitRetry) { history.attempts.clear(); history.blocked = false; }
    if (history.blocked) return;
    const auto stopOnly = history.attempts.size() >= 3;
    if (stopOnly) history.blocked = true;
    else history.attempts.push_back(now);
    maintenanceSelection = {generation, *output, activeBus(plan->bus()), {}, plan};
    operation = Switch::startMaintenance(*engine, maintenanceSelection, panel, stopOnly, history.rebind);
    currentActivity = Activity::maintenance; lastPhase = Switch::Phase::inactive;
    changed(stopOnly ? "Repeated driver resets; stopping automatic recovery..."
        : panel ? "Opening the active ASIO Control Panel. Output is suspended until revalidation finishes."
        : "Driver state changed; revalidating the active output and preserving the document and playhead...");
}
void AsioSettingsSession::serviceDriverEvents()
{
    if (!monitoringOutput || busy()) return;
    const auto events = engine->readManagedOutputEvents(true, false);
    if (!events) { monitoringOutput = false; return; }
    if ((events->pending & AsioDriverEvents::latency) != 0) engine->refreshManagedOutputLatency();
    if (events->outputSuspended || (events->pending & AsioDriverEvents::requiresRevalidation) != 0)
        beginMaintenance(false, false);
}
bool AsioSettingsSession::hasVisibleConsumer() const
{
    return std::any_of(visibleConsumers.begin(), visibleConsumers.end(), [](const auto& consumer) { return consumer.second(); });
}
void AsioSettingsSession::refreshLiveStatus()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (busy() || !hasVisibleConsumer() || now - lastLiveRead < 1000.0) return;
    lastLiveRead = now;
    juce::String text;
    const auto metrics = engine->hasExplicitOutputSelection() && !engine->isSelectedOutputReady()
        ? std::optional<AudioEngine::RuntimeMetrics>{} : engine->getRuntimeMetricsForDiagnostics();
    if (metrics)
    {
        const auto& src = metrics->playbackRate;
        text = "Active " + metrics->backend + " | " + (src.sourceRate > 0 ? juce::String(src.sourceRate, 0) + " Hz source" : juce::String("no prepared source")) + " -> "
            + juce::String(metrics->sampleRate, 0) + " Hz output / " + juce::String(metrics->bufferSamples) + " samples";
        text += "\nSRC: ";
        text += src.mode == PlaybackRateSource::Mode::bypassed ? "bypassed"
            : src.mode == PlaybackRateSource::Mode::converting ? PlaybackRateSource::Snapshot::algorithm : "unavailable";
        text += " | aggregate latency: " + (metrics->aggregateOutputLatencyMs
            ? juce::String(*metrics->aggregateOutputLatencyMs, 2) + " ms" : juce::String("unavailable"));
        if (metrics->driverResets) text += " | resets " + juce::String(static_cast<juce::uint64>(*metrics->driverResets));
        if (metrics->driverXruns >= 0) text += " / xruns " + juce::String(metrics->driverXruns);
    }
    else text = "Output unavailable or switching. Aggregate playback latency is unavailable.";
    if (liveStatus != text) { liveStatus = text; sendChangeMessage(); }
}
bool AsioSettingsSession::canFollowSourceSampleRate() const
{
    return engine && engine->isSelectedOutputReady() && engine->activeOutputPlan()
        && engine->activeOutputSnapshot().has_value();
}
void AsioSettingsSession::setFollowSourceSampleRate(bool enabled)
{
    if (!engine || followSource == enabled || (enabled && (busy() || !canFollowSourceSampleRate()))) return;
    followSource = enabled;
    ++sourceRateGeneration;
    followPending = enabled && sourceRate > 0;
    if (!enabled && operation && currentActivity == Activity::followSource) operation->cancel();
    bool saved = false;
    if (auto* settings = properties.getUserSettings())
    { settings->setValue(followSourceKey, enabled); saved = settings->saveIfNeeded(); }
    changed(enabled ? "Follow Source enabled. Source-rate changes request the active route at that rate; failures return the previous output."
        : "Follow Source disabled. The current confirmed device rate stays fixed; other source rates use SRC.");
    if (!saved) changed(statusText + " The preference could not be saved.");
}
void AsioSettingsSession::loadedFileChanged(const AudioEngine::LoadedFileInfo& info)
{
    const auto next = std::isfinite(info.sampleRate) && info.sampleRate > 0 ? info.sampleRate : 0;
    if (next == sourceRate) return;
    sourceRate = next; ++sourceRateGeneration;
    followPending = followSource && sourceRate > 0;
    if (operation && currentActivity == Activity::followSource) operation->cancel();
}
void AsioSettingsSession::serviceFollowSource()
{
    if (!followPending || !followSource || busy()) return;
    followPending = false;
    if (!canFollowSourceSampleRate() || sourceRate <= 0) return;
    const auto output = engine->activeOutputSnapshot();
    const auto plan = engine->activeOutputPlan();
    if (!output || !plan || output->sampleRate == sourceRate) return;
    const auto history = resetHistory.find(output->stableId);
    if (history != resetHistory.end() && (history->second.blocked || history->second.rebind))
    { changed("Follow Source is waiting for explicit output recovery or rebind."); return; }
    const auto bus = activeBus(plan->bus());
    const auto requested = plan->withRequestedSampleRate(sourceRate, bus);
    if (!requested) { changed(failureText(Switch::Error::rebindRequired)); return; }
    followSelection = {sourceRateGeneration, requested->device(), bus, {}, requested};
    operation = Switch::start(*engine, followSelection, Switch::Mode::exclusive, backend.prepare);
    currentActivity = Activity::followSource; lastPhase = Switch::Phase::inactive;
    changed("Follow Source: switching the active output to " + juce::String(sourceRate, 0)
        + " Hz with its confirmed buffer and routing...");
}
bool AsioSettingsSession::persistCommitted()
{
    auto* settings = properties.getUserSettings();
    if (!settings) return false;
    const auto active = engine->activeOutputSnapshot();
    const auto plan = engine->activeOutputPlan();
    if (!active || !plan || !plan->matches(*active, activeBus(plan->bus()))) return false;
    auto committed = DeviceRoutePlan::propose(plan->bus(), *active);
    for (int row = 0; row < static_cast<int>(committed.bus.speakers.size()); ++row)
    {
        committed.outputsBySpeaker[static_cast<size_t>(row)].clear();
        for (int slot = 0; slot < static_cast<int>(active->physicalOutputs.size()); ++slot)
            if (plan->connected(row, slot)) committed.outputsBySpeaker[static_cast<size_t>(row)].push_back(active->physicalOutputs[static_cast<size_t>(slot)]);
    }
    const auto committedReview = DeviceRoutePlan::validate(committed, *active, plan->bus());
    if (!committedReview.isValid()) return false;
    auto* binding = new juce::DynamicObject;
    binding->setProperty("schemaVersion", 1); binding->setProperty("backend", juce::String(active->backend));
    binding->setProperty("stableId", juce::String(active->stableId));
    binding->setProperty("sampleRate", active->sampleRate); binding->setProperty("bufferSamples", active->bufferSamples);
    binding->setProperty("profileId", juce::String(committed.bus.profileId)); binding->setProperty("revision", static_cast<juce::int64>(committed.bus.revision));
    binding->setProperty("warningsConfirmed", committedReview.needsConfirmation());
    juce::Array<juce::var> names, rows;
    for (const auto& name : committed.outputNames) names.add(juce::String(name));
    for (size_t i = 0; i < committed.bus.speakers.size(); ++i)
    {
        auto* row = new juce::DynamicObject;
        row->setProperty("stableId", juce::String(committed.bus.speakers[i].stableId));
        row->setProperty("role", ChannelLayoutCatalog::roleToAbbreviation(committed.bus.speakers[i].role));
        row->setProperty("required", committed.bus.speakers[i].required);
        juce::Array<juce::var> outputs; for (int index : committed.outputsBySpeaker[i]) outputs.add(index);
        row->setProperty("physicalOutputs", outputs); rows.add(juce::var(row));
    }
    binding->setProperty("outputNames", names); binding->setProperty("speakers", rows);
    const juce::var record(binding);
    const auto previous = juce::JSON::parse(settings->getValue(deviceBindingsKey));
    juce::Array<juce::var> records;
    if (const auto* existing = previous.getArray())
        for (const auto& item : *existing)
            if (item["backend"].toString().toStdString() != active->backend || item["stableId"].toString().toStdString() != active->stableId) records.add(item);
    records.add(record);
    settings->setValue(deviceBindingsKey, juce::JSON::toString(records, true));
    settings->setValue(active->backend == "ASIO" ? bindingKey : windowsBindingKey, juce::JSON::toString(record, true));
    settings->setValue(activeBackendKey, juce::String(active->backend));
    return settings->saveIfNeeded();
}
void AsioSettingsSession::poll()
{
    if (!engine) return;
    if (discovery && discovery->done.load(std::memory_order_acquire))
    {
        const bool cancelled = discovery->cancelled.load();
        if (!cancelled) catalog = std::move(discovery->result);
        discovery.reset(); currentActivity = Activity::idle;
        if (std::none_of(catalog.drivers.begin(), catalog.drivers.end(), [&](const auto& driver) { return driver.stableId == selectedId; })) selectedId.clear();
        changed(cancelled ? "Discovery cancelled." : catalog.drivers.empty() ? "No output devices are available for this backend." : "Choose an output, then read its capabilities.");
        if (selectingWindowsDefault)
        {
            selectingWindowsDefault = false;
            if (!cancelled)
            {
                selectedId.clear();
                selectDriver(catalog.defaultOutputId);
                if (selectedId.empty()) changed("Windows has no available default output. Choose an output explicitly.");
            }
        }
    }
    if (!operation)
    {
        serviceDriverEvents();
        if (!operation) serviceFollowSource();
        refreshLiveStatus(); return;
    }
    if (currentActivity == Activity::followSource)
    {
        auto current = followSelection;
        current.generation = sourceRateGeneration;
        current.bus = activeBus(current.bus);
        operation->poll(*engine, current);
    }
    else if (currentActivity == Activity::maintenance)
    {
        auto current = maintenanceSelection;
        current.bus = activeBus(current.bus);
        operation->poll(*engine, current);
    }
    else if (currentActivity == Activity::activation) operation->poll(*engine, selection());
    else operation->pollInspection(*engine, inspectionSelection());
    const auto state = operation->snapshot();
    if (!state.resourcesReleased)
    {
        if (state.awaitingPanel)
        {
            if (statusText != "ASIO Control Panel is open. Close it to revalidate output; editing remains available.")
                changed("ASIO Control Panel is open. Close it to revalidate output; editing remains available.");
        }
        else if (state.phase != lastPhase || state.driverUnresolved)
        {
            lastPhase = state.phase;
            if (state.driverUnresolved) changed("Driver is still busy. Cancellation requested; waiting for device release.");
            else if (state.phase == Switch::Phase::restoring) changed("Returning the previous output and transport...");
        }
        return;
    }
    if (currentActivity == Activity::followSource)
    {
        if (state.phase == Switch::Phase::active)
            changed(persistCommitted() ? "Follow Source: output is active at " + juce::String(followSelection.expected.sampleRate, 0)
                + " Hz. Routing, document and latest transport intent were preserved."
                : "Follow Source succeeded, but the confirmed output settings could not be saved.");
        else if (followPending)
            changed("Source rate changed; previous output returned. Preparing the latest source rate next.");
        else if (state.restored)
            changed("Follow Source did not activate the requested rate; previous output returned. Playback uses its confirmed rate and SRC. Retry output or disable Follow Source.");
        else changed("Follow Source failed and the previous output could not be restored. Output is unavailable; Retry output or choose a device.");
    }
    else if (currentActivity == Activity::maintenance)
    {
        auto& history = resetHistory[maintenanceSelection.expected.stableId];
        if (state.error == Switch::Error::rebindRequired)
        {
            history.rebind = true; history.blocked = true;
            if (backendId == "ASIO" && selectedId == maintenanceSelection.expected.stableId)
            { invalidate(); facts.reset(); rebuildDraft(); }
        }
        if (state.phase == Switch::Phase::active)
            changed(persistCommitted() ? "ASIO output revalidated and saved at the actual driver rate. The approved route and latest transport position were preserved."
                : "ASIO output revalidated, but its settings could not be saved. Check access to the settings location.");
        else
        {
            // A failed reopen or explicit Cancel waits for user intent. Do not
            // turn a closed owner into an endless timer-driven reopen loop.
            history.blocked = true;
            changed(state.error == Switch::Error::rebindRequired || state.error == Switch::Error::circuitOpen
                ? failureText(state.error) : !state.restored
                    ? "ASIO output is unavailable; recovery and previous configuration restore failed. Retry output, open Control Panel, or choose another device."
                    : failureText(state.error));
        }
    }
    else if (state.phase == Switch::Phase::active)
    { changed(restoringSavedSelection ? "Saved " + juce::String(backendId) + " route restored at its exact rate and buffer."
        : persistCommitted() ? juce::String(backendId) + " route activated and saved. Source audio follows the selected device rate through shared SRC."
        : juce::String(backendId) + " route activated, but its settings could not be saved. Check access to the settings location."); }
    else if (auto result = operation->inspection(inspectionSelection()))
    {
        if (currentActivity == Activity::inspection || currentActivity == Activity::panel)
        {
            facts = std::move(result); const auto& c = facts->capabilities;
            rate = c.reportedSampleRate.value_or(0); buffer = c.reportedBufferSize.value_or(c.preferredBufferSize.value_or(0));
            if (std::find(c.sampleRates.begin(), c.sampleRates.end(), rate) == c.sampleRates.end()) rate = 0;
            if (std::find(c.bufferSizes.begin(), c.bufferSizes.end(), buffer) == c.bufferSizes.end()) buffer = 0;
            rebuildDraft(); changed(engine->hasExplicitOutputSelection() && !engine->isSelectedOutputReady()
                ? "Capabilities read. Output remains unavailable until you Validate, Ping and Activate an explicit route."
                : "Capabilities read; previous output returned. Choose a canonical layout and assign physical outputs.");
        }
        else changed("Physical ping completed; previous output returned. Activate when the wiring is confirmed.");
    }
    else changed(failureText(state.error));
    if (state.phase == Switch::Phase::active && currentActivity == Activity::activation) resetHistory.erase(state.stableId);
    if (state.phase == Switch::Phase::active && currentActivity == Activity::activation)
        followPending = followSource && sourceRate > 0;
    operation.reset(); currentActivity = Activity::idle; restoringSavedSelection = false;
    monitoringOutput = engine->readManagedOutputEvents(false).has_value();
    if (!monitoringOutput) liveStatus.clear();
    sendChangeMessage();
}
}
