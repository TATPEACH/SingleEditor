#pragma once
#include "Audio/DeviceBackendCapabilities.h"
#include "Audio/DeviceOutput.h"
#include "Audio/DeviceSwitchAccess.h"
#include "Audio/NativeAudioDeviceBinding.h"
#include <functional>

namespace singleeditor
{
// Explicit Windows output adapter. It never enumerates ASIO or selects
// a default endpoint implicitly. Construction and passive reads are inert.
class WindowsOutputBackend final
{
public:
    enum class Status { ready, unavailable, cancelled, accessNotGranted, identityUnavailable,
        unsupportedCapabilities, unsupportedRequest, openFailed, configurationChanged };
    struct Catalog { Status status = Status::unavailable; std::vector<WindowsOutputEndpoint> outputs; };
    struct Inspection { Status status = Status::unavailable; std::optional<PreparedDeviceCapabilities> capabilities; };
    struct Candidate { Status status = Status::unavailable; std::unique_ptr<DeviceOutput> output; };
    explicit WindowsOutputBackend(const std::string& backend = "Windows Audio");
    ~WindowsOutputBackend();
    Catalog discover();
    Inspection inspect(const std::string& stableId, const DeviceSwitchAccess&, const std::atomic<bool>&);
    Candidate prepare(const DeviceOutput::Snapshot& requested, const DeviceSwitchAccess&, const std::atomic<bool>&);
private:
    friend struct WindowsOutputBackendTestAccess;
    using Factory = std::function<std::unique_ptr<juce::AudioIODeviceType>()>;
    using CatalogReader = std::function<std::optional<std::vector<WindowsOutputEndpoint>>(const juce::AudioIODeviceType*)>;
    using Creator = std::function<std::unique_ptr<juce::AudioIODevice>(juce::AudioIODeviceType*, const juce::String&)>;
    using IdentityReader = std::function<juce::String(const juce::AudioIODevice*)>;
    WindowsOutputBackend(Factory, CatalogReader, Creator, IdentityReader);
    Status createSelected(const std::string&, const DeviceSwitchAccess&, const std::atomic<bool>&);
    std::optional<PreparedDeviceCapabilities> copyCapabilities(const std::string&) const;
    Factory factory;
    CatalogReader catalogReader;
    Creator creator;
    IdentityReader identityReader;
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
};
}
