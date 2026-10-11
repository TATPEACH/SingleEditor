#pragma once
#include "Audio/DeviceBackendCapabilities.h"
#include "Audio/AsioDriverEvents.h"
#include "Audio/DeviceOutput.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace juce { class AudioIODevice; class AudioIODeviceType; }
namespace singleeditor
{
// Internal adapter bridge, not a document/device preference schema. Registry
// provenance is retained to reject a changed registration before driver access.
struct AsioDriverRegistration
{
    std::string displayName, classId, registryKey, serverPath;
    bool operator==(const AsioDriverRegistration&) const = default;
};
using AsioClockSource = DeviceClockSource;
using AsioClockState = DeviceClockState;
struct AsioDriverActualState
{
    double sampleRate = 0;
    int minimumBuffer = 0, maximumBuffer = 0, preferredBuffer = 0;
    std::optional<int> inputLatency, outputLatency;
    std::vector<std::string> inputNames, outputNames;
    std::optional<AsioClockState> clock;
};
std::optional<AsioClockState> copyAsioClockState(const juce::AudioIODevice*);
// Passive initialized-instance cache. Never invokes an SDK getter.
std::optional<DeviceOutput::DriverIdentity> copyAsioDriverIdentity(const juce::AudioIODevice*);
// Opt-in native event bridge. The application must own the exact device and
// service its revalidation before arming; ordinary construction never arms it.
bool armAsioDriverEvents(juce::AudioIODevice*, std::uint64_t generation);
AsioDriverEvents::Snapshot readAsioDriverEvents(juce::AudioIODevice*, std::uint64_t generation, bool consume, bool includeCounters = true);
bool showManagedAsioControlPanel(juce::AudioIODevice*, std::uint64_t generation);
bool acknowledgeAsioRevalidation(juce::AudioIODevice*, std::uint64_t generation);
std::optional<AsioDriverActualState> readAsioDriverActualState(const juce::AudioIODevice*);
bool requestAsioDriverReset(juce::AudioIODevice*, std::uint64_t generation);
bool releaseClosedAsioDriver(juce::AudioIODevice*);
bool prepareClosedAsioDriver(juce::AudioIODevice*);
bool refreshAsioDriverCapabilities(juce::AudioIODevice*);
std::optional<std::pair<int, int>> readAsioDriverLatencies(const juce::AudioIODevice*);
std::optional<std::vector<AsioDriverRegistration>> readAsioDriverRegistrations();
std::unique_ptr<juce::AudioIODevice> createAsioDeviceByClassId(
    juce::AudioIODeviceType*, const AsioDriverRegistration&);
}
