#include "Audio/WindowsOutputBackend.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <algorithm>
#include <cmath>

namespace singleeditor
{
namespace
{
DeviceOutput::Snapshot readOutput(juce::AudioIODevice& device, const juce::String& identity)
{
    DeviceOutput::Snapshot result;
    result.backend = device.getTypeName().toStdString(); result.stableId = identity.toStdString(); result.displayName = device.getName().toStdString();
    result.sampleRate = device.getCurrentSampleRate(); result.bufferSamples = device.getCurrentBufferSizeSamples(); result.bitDepth = device.getCurrentBitDepth();
    const auto active = device.getActiveOutputChannels();
    if (active.getHighestBit() >= 512) return result;
    for (int index = 0; index <= active.getHighestBit(); ++index) if (active[index]) result.physicalOutputs.push_back(index);
    for (const auto& name : device.getOutputChannelNames()) result.outputNames.push_back(name.toStdString());
    const auto inputLatency = device.getInputLatencyInSamples(), outputLatency = device.getOutputLatencyInSamples();
    if (inputLatency >= 0) result.inputLatencySamples = inputLatency;
    if (outputLatency >= 0) result.outputLatencySamples = outputLatency;
    return result;
}
bool matches(juce::AudioIODevice& device, const DeviceOutput::Snapshot& actual, const DeviceOutput::Snapshot& expected)
{
    return device.isOpen() && device.getActiveInputChannels().isZero()
        && actual.backend == expected.backend && actual.stableId == expected.stableId
        && actual.outputNames == expected.outputNames && actual.physicalOutputs == expected.physicalOutputs
        && actual.sampleRate == expected.sampleRate && actual.bufferSamples == expected.bufferSamples && actual.bitDepth > 0;
}
class WindowsDeviceOutput final : public DeviceOutput
{
public:
    using IdentityReader = std::function<juce::String(const juce::AudioIODevice*)>;
    WindowsDeviceOutput(std::unique_ptr<juce::AudioIODeviceType> owner, std::unique_ptr<juce::AudioIODevice> selected,
        Snapshot actual, IdentityReader reader) : type(std::move(owner)), device(std::move(selected)), state(std::move(actual)), identityReader(std::move(reader)) {}
    ~WindowsDeviceOutput() override { closeForSwitch(); }
    const Snapshot& snapshot() const noexcept override { return state; }
    bool isOpen() const noexcept override { try { return device && device->isOpen(); } catch (...) { return false; } }
    bool attach(juce::AudioIODeviceCallback& callback) override
    {
        if (attached || !isOpen()) return false;
        try
        {
            const auto actual = readOutput(*device, identityReader(device.get()));
            if (!matches(*device, actual, state)) return false;
            state = actual;
            device->start(&callback);
            if (device->isPlaying()) { attached = true; return true; }
        }
        catch (...) {}
        detach(); return false;
    }
    void detach() noexcept override { try { if (device) device->stop(); } catch (...) {} attached = false; }
    bool closeForSwitch() noexcept override
    { detach(); try { device->close(); return !device->isOpen(); } catch (...) { return false; } }
    bool restore() noexcept override
    {
        if (attached || isOpen()) return false;
        try
        {
            if (identityReader(device.get()).toStdString() != state.stableId) return false;
            juce::BigInteger outputs; for (int index : state.physicalOutputs) outputs.setBit(index);
            const auto error = device->open({}, outputs, state.sampleRate, state.bufferSamples);
            const auto actual = readOutput(*device, identityReader(device.get()));
            if (error.isEmpty() && matches(*device, actual, state)) { state = actual; return true; }
        }
        catch (...) {}
        closeForSwitch(); return false;
    }
private:
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    Snapshot state;
    IdentityReader identityReader;
    bool attached = false;
};
}
WindowsOutputBackend::WindowsOutputBackend(const std::string& backend)
{
   #if JUCE_WINDOWS && SINGLE_EDITOR_WASAPI_DIAGNOSTIC_BINDING
    if (!isWindowsOutputBackend(backend)) return;
    const auto mode = backend == "Windows Audio (Exclusive Mode)" ? juce::WASAPIDeviceMode::exclusive
        : backend == "Windows Audio (Low Latency Mode)" ? juce::WASAPIDeviceMode::sharedLowLatency : juce::WASAPIDeviceMode::shared;
    factory = [mode] { return std::unique_ptr<juce::AudioIODeviceType>(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(mode)); };
    catalogReader = copyWasapiOutputCatalog;
    creator = createWasapiOutputByEndpoint;
    identityReader = copyWasapiOutputEndpoint;
   #else
    juce::ignoreUnused(backend);
   #endif
}
WindowsOutputBackend::WindowsOutputBackend(Factory f, CatalogReader c, Creator make, IdentityReader identity)
    : factory(std::move(f)), catalogReader(std::move(c)), creator(std::move(make)), identityReader(std::move(identity)) {}
WindowsOutputBackend::~WindowsOutputBackend() = default;
WindowsOutputBackend::Catalog WindowsOutputBackend::discover()
{
    if (device || !factory || !catalogReader) return {};
    type = factory();
    if (!type) return {};
    type->scanForDevices();
    const auto values = catalogReader(type.get());
    return values ? Catalog {Status::ready, *values} : Catalog{};
}
WindowsOutputBackend::Status WindowsOutputBackend::createSelected(const std::string& stableId,
    const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled)
{
    if (cancelled.load()) return Status::cancelled;
    if (!access.isValid()) return Status::accessNotGranted;
    if (stableId.empty() || !creator || !identityReader) return Status::identityUnavailable;
    const auto choices = discover();
    if (choices.status != Status::ready) return choices.status;
    if (std::none_of(choices.outputs.begin(), choices.outputs.end(), [&](const auto& value) { return value.stableId.toStdString() == stableId; })) return Status::identityUnavailable;
    if (cancelled.load() || !access.isValid()) return Status::cancelled;
    device = creator(type.get(), juce::String(stableId));
    if (!device || identityReader(device.get()).toStdString() != stableId) { device.reset(); return Status::identityUnavailable; }
    if (cancelled.load() || !access.isValid()) { device.reset(); return Status::cancelled; }
    return Status::ready;
}
std::optional<PreparedDeviceCapabilities> WindowsOutputBackend::copyCapabilities(const std::string& id) const
{
    if (!device) return {};
    PreparedDeviceCapabilities result {id, 1, {}};
    auto& capabilities = result.capabilities;
    for (const auto& name : device->getOutputChannelNames()) capabilities.outputNames.push_back(name.toStdString());
    for (const auto& name : device->getInputChannelNames()) capabilities.inputNames.push_back(name.toStdString());
    for (double rate : device->getAvailableSampleRates()) if (std::isfinite(rate) && rate > 0) capabilities.sampleRates.push_back(rate);
    for (int buffer : device->getAvailableBufferSizes()) if (buffer > 0) capabilities.bufferSizes.push_back(buffer);
    if (capabilities.outputNames.empty() || capabilities.outputNames.size() > 512 || capabilities.inputNames.size() > 512
        || capabilities.sampleRates.empty() || capabilities.bufferSizes.empty()) return {};
    const auto rate = device->getCurrentSampleRate();
    if (std::isfinite(rate) && rate > 0) capabilities.reportedSampleRate = rate;
    const auto buffer = device->getCurrentBufferSizeSamples(), preferred = device->getDefaultBufferSize();
    if (buffer > 0) capabilities.reportedBufferSize = buffer;
    if (preferred > 0) capabilities.preferredBufferSize = preferred;
    return result;
}
WindowsOutputBackend::Inspection WindowsOutputBackend::inspect(const std::string& id,
    const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled)
{
    const auto status = createSelected(id, access, cancelled);
    if (status != Status::ready) return {status, {}};
    auto values = copyCapabilities(id);
    device->stop(); device->close(); device.reset(); type.reset();
    if (cancelled.load() || !access.isValid()) return {Status::cancelled, {}};
    return values ? Inspection{Status::ready, std::move(values)} : Inspection{Status::unsupportedCapabilities, {}};
}
WindowsOutputBackend::Candidate WindowsOutputBackend::prepare(const DeviceOutput::Snapshot& requested,
    const DeviceSwitchAccess& access, const std::atomic<bool>& cancelled)
{
    const auto status = createSelected(requested.stableId, access, cancelled);
    if (status != Status::ready) return {status, {}};
    const auto values = copyCapabilities(requested.stableId);
    if (!values) return {Status::unsupportedCapabilities, {}};
    const auto& capabilities = values->capabilities;
    if (!isWindowsOutputBackend(requested.backend) || requested.backend != device->getTypeName().toStdString()
        || requested.outputNames != capabilities.outputNames) return {Status::configurationChanged, {}};
    if (std::find(capabilities.sampleRates.begin(), capabilities.sampleRates.end(), requested.sampleRate) == capabilities.sampleRates.end()
        || std::find(capabilities.bufferSizes.begin(), capabilities.bufferSizes.end(), requested.bufferSamples) == capabilities.bufferSizes.end()
        || requested.physicalOutputs.empty() || requested.physicalOutputs.size() > 32
        || !std::is_sorted(requested.physicalOutputs.begin(), requested.physicalOutputs.end())
        || std::adjacent_find(requested.physicalOutputs.begin(), requested.physicalOutputs.end()) != requested.physicalOutputs.end()
        || requested.physicalOutputs.front() < 0 || requested.physicalOutputs.back() >= static_cast<int>(capabilities.outputNames.size())) return {Status::unsupportedRequest, {}};
    if (cancelled.load() || !access.isValid()) return {Status::cancelled, {}};
    juce::BigInteger outputs; for (int index : requested.physicalOutputs) outputs.setBit(index);
    const auto error = device->open({}, outputs, requested.sampleRate, requested.bufferSamples);
    if (cancelled.load() || !access.isValid()) { device->close(); return {Status::cancelled, {}}; }
    if (error.isNotEmpty() || !device->isOpen()) { device->close(); return {Status::openFailed, {}}; }
    auto actual = readOutput(*device, identityReader(device.get()));
    if (!matches(*device, actual, requested)) { device->close(); return {Status::configurationChanged, {}}; }
    return {Status::ready, std::make_unique<WindowsDeviceOutput>(std::move(type), std::move(device), std::move(actual), identityReader)};
}
}
