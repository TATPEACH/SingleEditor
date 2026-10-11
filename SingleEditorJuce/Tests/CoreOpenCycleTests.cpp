#include "App/CoreOpenCycleProbe.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace singleeditor;
namespace
{
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Mapping
{
    std::wstring name = L"Local\\SingleEditorCycleTest-" + std::to_wstring(GetCurrentProcessId());
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CoreOpenCycleSharedState), name.c_str());
    CoreOpenCycleSharedState* data = handle ? static_cast<CoreOpenCycleSharedState*>(
        MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CoreOpenCycleSharedState))) : nullptr;
    ~Mapping()
    {
        CoreOpenCycleProbe::detach();
        SetEnvironmentVariableW(L"SINGLE_EDITOR_OPENCYCLE_MAPPING", nullptr);
        if (data) UnmapViewOfFile(data);
        if (handle) CloseHandle(handle);
    }
};
}
int main()
{
    try
    {
        require(CoreOpenCycleProbe::captureStart() == 0 && !CoreOpenProbe::isActive(), "both channels are inert without a harness");
        Mapping mapping;
        require(mapping.data != nullptr, "create an explicit external cycle mapping");
        auto& shared = *mapping.data;
        LARGE_INTEGER frequency {}; QueryPerformanceFrequency(&frequency);
        shared.magic = CoreOpenCycleSharedState::expectedMagic;
        shared.version = CoreOpenCycleSharedState::expectedVersion;
        shared.byteSize = sizeof(shared); shared.capacity = CoreOpenCycleSharedState::maximumCycles;
        shared.qpcFrequency = frequency.QuadPart;
        SetEnvironmentVariableW(L"SINGLE_EDITOR_OPENCYCLE_MAPPING", mapping.name.c_str());
        CoreOpenCycleProbe::attachFromEnvironment();
        require(shared.processId == GetCurrentProcessId() && !CoreOpenProbe::isActive(), "cycle channel does not attach or reset COP1");
        const auto started = CoreOpenCycleProbe::captureStart();
        CoreOpenCycleProbe::begin(7, started);
        CoreOpenCycleProbe::mark(7, CoreOpenMilestone::playable);
        const auto firstPlayable = shared.cycles[0].playable;
        CoreOpenCycleProbe::mark(7, CoreOpenMilestone::playable);
        CoreOpenCycleProbe::begin(7, CoreOpenCycleProbe::captureStart());
        require(shared.publishedCount == 1 && firstPlayable >= started && shared.cycles[0].playable == firstPlayable,
            "each accepted milestone is one-shot within its exact generation");
        CoreOpenCycleProbe::mark(7, CoreOpenMilestone::firstWaveform);
        require(shared.cycles[0].firstWaveform >= started, "actual waveform and playable timestamps stay independent");
        CoreOpenCycleProbe::begin(3, CoreOpenCycleProbe::captureStart());
        CoreOpenCycleProbe::mark(7, CoreOpenMilestone::firstWaveform);
        require(shared.publishedCount == 2 && shared.cycles[1].firstWaveform == 0,
            "identity equality rejects stale events without sorting generation numbers");
        CoreOpenCycleProbe::mark(3, CoreOpenMilestone::firstWaveform);
        CoreOpenCycleProbe::mark(3, CoreOpenMilestone::playable);
        for (std::uint64_t i = 2; i < CoreOpenCycleSharedState::maximumCycles; ++i)
            CoreOpenCycleProbe::begin(100 + i, CoreOpenCycleProbe::captureStart());
        CoreOpenCycleProbe::begin(1000, CoreOpenCycleProbe::captureStart());
        require(shared.publishedCount == CoreOpenCycleSharedState::maximumCycles && shared.overflow == 1
            && shared.cycles[0].playable == firstPlayable, "bounded append-only cycles expose overflow and never overwrite earlier samples");
        CoreOpenCycleProbe::detach();
        require(CoreOpenCycleProbe::captureStart() == 0, "detach releases observation without affecting retained external samples");
        std::cout << "CoreOpenCycleTests passed: inactive, independent COP1, exact generations, one-shot milestones, bounded append-only and detach\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "CoreOpenCycleTests FAILED: " << error.what() << '\n'; return 1; }
}
