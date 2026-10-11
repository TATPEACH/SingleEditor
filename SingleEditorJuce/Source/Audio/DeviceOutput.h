#pragma once
#include "Audio/AsioDriverEvents.h"

#include <optional>
#include <string>
#include <vector>

namespace juce { class AudioIODeviceCallback; }
namespace singleeditor
{
// An already validated, exclusively owned output. No driver enumeration or
// device-policy decisions belong to the renderer. All methods are control-side;
// the callback is the existing JUCE realtime audio seam.
class DeviceOutput
{
public:
    // Facts returned by the initialized IASIO interface, not registry labels.
    // The version is the raw signed SDK integer; its encoding is driver-owned.
    struct DriverIdentity
    {
        std::string product;
        std::optional<std::int32_t> rawAsioVersion;
        bool versionReadFailed = false;
    };
    struct Snapshot
    {
        std::string backend, stableId, displayName;
        double sampleRate = 0;
        int bufferSamples = 0, bitDepth = 0;
        std::vector<int> physicalOutputs;
        std::vector<std::string> outputNames;
        std::optional<int> inputLatencySamples, outputLatencySamples;
        std::optional<DriverIdentity> driverIdentity;
    };
    virtual ~DeviceOutput() = default;
    virtual const Snapshot& snapshot() const noexcept = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual bool attach(juce::AudioIODeviceCallback&) = 0;
    virtual void detach() noexcept = 0;
    virtual bool closeForSwitch() noexcept = 0;
    // Worker-only after closure, before another instance may enter the same
    // backend. Retain exact configuration for rollback without holding its SDK.
    virtual bool releaseForReplacement() noexcept { return !isOpen(); }
    // Reopen exactly the last validated configuration, with no fallback.
    virtual bool restore() noexcept = 0;
    virtual bool hasManagedEvents() const noexcept { return false; }
    virtual bool outputSuspended() const noexcept { return false; }
    virtual AsioDriverEvents::Snapshot driverEvents(bool, bool) noexcept { return {}; }
    virtual bool refreshDriverLatency() { return false; }
    enum class Maintenance { ready, unavailable, rebindRequired, cancelled };
    // Worker-owned, closed stream only. Keep a rollback image until the engine
    // has accepted the revalidated output and its renderer callback.
    virtual Maintenance maintain(bool, const std::atomic<bool>&, std::atomic<bool>&) { return Maintenance::unavailable; }
    virtual void acceptMaintenance() noexcept {}
    virtual void rollbackMaintenance() noexcept {}
};
}
