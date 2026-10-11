#pragma once

#include <cstdint>

namespace singleeditor
{
enum class CoreOpenMilestone : std::uint32_t
{
    window = 0,
    fileName,
    firstWaveform,
    playable,
    fullWaveform
};

// Release-safe benchmark handshake. It is inert unless an external harness
// creates the named shared-memory channel before launching the process.
class CoreOpenProbe final
{
public:
    static void attachFromEnvironment() noexcept;
    static void detach() noexcept;
    static bool isActive() noexcept;
    static void mark(CoreOpenMilestone milestone) noexcept;
};
} // namespace singleeditor
