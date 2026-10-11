#include "App/CoreOpenProbe.h"
#include "App/CoreOpenCycleProbe.h"

#if defined(_WIN32)
#include <Windows.h>

#include <cstddef>
#include <iterator>
#endif

namespace singleeditor
{
#if defined(_WIN32)
namespace
{
constexpr wchar_t mappingEnvironmentVariable[] = L"SINGLE_EDITOR_COREOPEN_MAPPING";
constexpr std::uint32_t sharedMagic = 0x31504f43u; // "COP1"
constexpr std::uint32_t sharedVersion = 1u;
constexpr std::size_t milestoneCount = 5u;

struct alignas(8) CoreOpenSharedState
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t byteSize = 0;
    std::uint32_t reserved = 0;
    std::int64_t qpcFrequency = 0;
    std::int64_t launchQpc = 0;
    volatile LONG64 processId = 0;
    volatile LONG64 milestones[milestoneCount] {};
};

static_assert(offsetof(CoreOpenSharedState, milestones) == 40);
static_assert(sizeof(CoreOpenSharedState) == 80);

HANDLE mappingHandle = nullptr;
CoreOpenSharedState* sharedState = nullptr;

void closeMapping() noexcept
{
    if (sharedState != nullptr)
    {
        UnmapViewOfFile(sharedState);
        sharedState = nullptr;
    }

    if (mappingHandle != nullptr)
    {
        CloseHandle(mappingHandle);
        mappingHandle = nullptr;
    }
}
} // namespace
#endif

void CoreOpenProbe::attachFromEnvironment() noexcept
{
#if defined(_WIN32)
    if (sharedState != nullptr)
        return;

    wchar_t mappingName[192] {};
    const auto length = GetEnvironmentVariableW(mappingEnvironmentVariable,
                                                mappingName,
                                                static_cast<DWORD>(std::size(mappingName)));
    if (length == 0 || length >= std::size(mappingName))
        return;

    mappingHandle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
    if (mappingHandle == nullptr)
        return;

    sharedState = static_cast<CoreOpenSharedState*>(MapViewOfFile(mappingHandle,
                                                                 FILE_MAP_READ | FILE_MAP_WRITE,
                                                                 0,
                                                                 0,
                                                                 sizeof(CoreOpenSharedState)));
    if (sharedState == nullptr)
    {
        closeMapping();
        return;
    }

    LARGE_INTEGER frequency {};
    if (sharedState->magic != sharedMagic
        || sharedState->version != sharedVersion
        || sharedState->byteSize != sizeof(CoreOpenSharedState)
        || sharedState->launchQpc <= 0
        || QueryPerformanceFrequency(&frequency) == FALSE
        || sharedState->qpcFrequency != frequency.QuadPart)
    {
        closeMapping();
        return;
    }

    InterlockedExchange64(&sharedState->processId, static_cast<LONG64>(GetCurrentProcessId()));
#endif
}

void CoreOpenProbe::detach() noexcept
{
#if defined(_WIN32)
    closeMapping();
#endif
}

bool CoreOpenProbe::isActive() noexcept
{
#if defined(_WIN32)
    return sharedState != nullptr;
#else
    return false;
#endif
}

void CoreOpenProbe::mark(CoreOpenMilestone milestone) noexcept
{
#if defined(_WIN32)
    auto* const state = sharedState;
    const auto index = static_cast<std::size_t>(milestone);
    if (state == nullptr || index >= milestoneCount)
        return;

    LARGE_INTEGER now {};
    if (QueryPerformanceCounter(&now) == FALSE)
        return;

    InterlockedCompareExchange64(&state->milestones[index], now.QuadPart, 0);
#else
    (void) milestone;
#endif
}
namespace
{
#if defined(_WIN32)
HANDLE cycleMappingHandle = nullptr;
CoreOpenCycleSharedState* cycleState = nullptr;
CoreOpenCycleSharedState::Cycle* currentCycle = nullptr;
#endif
}

void CoreOpenCycleProbe::detach() noexcept
{
#if defined(_WIN32)
    currentCycle = nullptr;
    if (cycleState) UnmapViewOfFile(cycleState);
    if (cycleMappingHandle) CloseHandle(cycleMappingHandle);
    cycleState = nullptr; cycleMappingHandle = nullptr;
#endif
}

void CoreOpenCycleProbe::attachFromEnvironment() noexcept
{
#if defined(_WIN32)
    if (cycleState) return;
    wchar_t name[192] {};
    const auto length = GetEnvironmentVariableW(L"SINGLE_EDITOR_OPENCYCLE_MAPPING", name, static_cast<DWORD>(std::size(name)));
    if (length == 0 || length >= std::size(name)) return;
    cycleMappingHandle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!cycleMappingHandle) return;
    cycleState = static_cast<CoreOpenCycleSharedState*>(MapViewOfFile(cycleMappingHandle,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CoreOpenCycleSharedState)));
    LARGE_INTEGER frequency {};
    if (!cycleState || cycleState->magic != CoreOpenCycleSharedState::expectedMagic
        || cycleState->version != CoreOpenCycleSharedState::expectedVersion
        || cycleState->byteSize != sizeof(CoreOpenCycleSharedState)
        || cycleState->capacity != CoreOpenCycleSharedState::maximumCycles
        || cycleState->publishedCount != 0 || cycleState->overflow != 0
        || !QueryPerformanceFrequency(&frequency) || cycleState->qpcFrequency != frequency.QuadPart)
    { detach(); return; }
    if (InterlockedCompareExchange64(&cycleState->processId, static_cast<LONG64>(GetCurrentProcessId()), 0) != 0)
    { detach(); return; }
#endif
}

std::int64_t CoreOpenCycleProbe::captureStart() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER now {};
    if (cycleState && QueryPerformanceCounter(&now)) return now.QuadPart;
#endif
    return 0;
}

void CoreOpenCycleProbe::begin(std::uint64_t generation, std::int64_t started) noexcept
{
#if defined(_WIN32)
    if (!cycleState || generation == 0 || started <= 0) return;
    if (currentCycle && currentCycle->generation == generation) return;
    currentCycle = nullptr;
    const auto index = cycleState->publishedCount;
    if (index < 0 || index >= CoreOpenCycleSharedState::maximumCycles)
    { InterlockedExchange64(&cycleState->overflow, 1); return; }
    auto& cycle = cycleState->cycles[static_cast<std::size_t>(index)];
    cycle.generation = generation; cycle.started = started;
    cycle.firstWaveform = 0; cycle.playable = 0;
    currentCycle = &cycle;
    InterlockedExchange64(&cycleState->publishedCount, index + 1);
#else
    (void) generation; (void) started;
#endif
}

void CoreOpenCycleProbe::mark(std::uint64_t generation, CoreOpenMilestone milestone) noexcept
{
#if defined(_WIN32)
    if (!currentCycle || currentCycle->generation != generation) return;
    auto* field = milestone == CoreOpenMilestone::firstWaveform ? &currentCycle->firstWaveform
        : milestone == CoreOpenMilestone::playable ? &currentCycle->playable : nullptr;
    if (!field) return;
    LARGE_INTEGER now {};
    if (QueryPerformanceCounter(&now)) InterlockedCompareExchange64(field, now.QuadPart, 0);
#else
    (void) generation; (void) milestone;
#endif
}
} // namespace singleeditor
