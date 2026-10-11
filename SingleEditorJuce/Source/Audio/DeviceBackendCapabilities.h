#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace singleeditor
{
inline bool isWindowsOutputBackend(std::string_view backend) noexcept
{
    return backend == "Windows Audio" || backend == "Windows Audio (Exclusive Mode)"
        || backend == "Windows Audio (Low Latency Mode)";
}
// Detached values shared by optional device backends. No driver/SDK owner or
// device access permission escapes a capability inspection.
struct DeviceClockSource
{
    int index = 0;
    std::string name;
    bool isCurrent = false;
};
struct DeviceClockState
{
    std::vector<DeviceClockSource> sources;
    std::optional<int> currentIndex;
};
struct DeviceBackendCapabilities
{
    std::vector<std::string> inputNames, outputNames;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    std::optional<double> reportedSampleRate;
    std::optional<int> reportedBufferSize, preferredBufferSize;
    bool hasControlPanel = false;
    std::optional<DeviceClockState> clockState;
};
struct PreparedDeviceCapabilities
{
    std::string stableId;
    std::uint64_t catalogGeneration = 0;
    DeviceBackendCapabilities capabilities;
};
}
