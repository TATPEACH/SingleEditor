#pragma once
#include "CoreOpenProbe.h"
#include <cstddef>
#include <cstdint>

namespace singleeditor
{
// Supplemental repeat-open handshake. COP1 launch timing is independent and
// remains one-shot. This channel only opens a harness-created memory mapping.
struct alignas(8) CoreOpenCycleSharedState
{
    static constexpr std::uint32_t expectedMagic = 0x3159434fu; // OCY1
    static constexpr std::uint32_t expectedVersion = 1;
    static constexpr std::uint32_t maximumCycles = 64;
    struct alignas(8) Cycle
    {
        std::uint64_t generation = 0;
        std::int64_t started = 0;
        volatile std::int64_t firstWaveform = 0, playable = 0;
    };
    std::uint32_t magic = 0, version = 0, byteSize = 0, capacity = 0;
    std::int64_t qpcFrequency = 0;
    volatile std::int64_t processId = 0, publishedCount = 0, overflow = 0;
    Cycle cycles[maximumCycles] {};
};
static_assert(offsetof(CoreOpenCycleSharedState, cycles) == 48);
static_assert(sizeof(CoreOpenCycleSharedState::Cycle) == 32);
static_assert(sizeof(CoreOpenCycleSharedState) == 2096);

class CoreOpenCycleProbe final
{
public:
    static void attachFromEnvironment() noexcept;
    static void detach() noexcept;
    static std::int64_t captureStart() noexcept;
    // Message-thread generation lifecycle only. No numeric version ordering.
    static void begin(std::uint64_t generation, std::int64_t started) noexcept;
    static void mark(std::uint64_t generation, CoreOpenMilestone) noexcept;
};
}
