#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace singleeditor
{
enum class OptionalModuleId : std::uint8_t
{
    workspace,
    meter,
    dsp,
    ambisonics,
    bnkWem,
    diagnostics,
    recovery,
    cli,
    count
};

enum class ModuleAvailability : std::uint8_t { unavailable, available };
enum class ModuleIntent : std::uint8_t { none, requested };
enum class ModulePlacement : std::uint8_t { absent, described, placed };
enum class ModuleViewResidency : std::uint8_t { absent, resident, suspended };
enum class ModuleEngineActivity : std::uint8_t { released, preparing, active, suspended };
enum class ModuleHealth : std::uint8_t { healthy, failed };

enum class LifecycleTransitionResult : std::uint8_t
{
    applied,
    unchanged,
    rejectedStaleGeneration,
    rejectedBeforeCoreReady,
    rejectedUnavailable,
    rejectedNotRequested,
    rejectedFailed,
    rejectedInvalidState
};

enum class CoreMilestoneResult : std::uint8_t
{
    applied,
    unchanged,
    coreReadyReached,
    rejectedStaleGeneration
};

class CoreLifecycleCoordinator final
{
public:
    struct Generation final
    {
        std::uint64_t value = 0;
        explicit operator bool() const noexcept { return value != 0; }
        friend bool operator==(Generation, Generation) = default;
    };

    struct Lease final
    {
        Generation generation;
        OptionalModuleId module = OptionalModuleId::workspace;
        std::uint64_t epoch = 0;
    };

    struct ModuleSnapshot final
    {
        Generation generation;
        ModuleAvailability availability = ModuleAvailability::available;
        ModuleIntent intent = ModuleIntent::none;
        ModulePlacement placement = ModulePlacement::absent;
        ModuleViewResidency viewResidency = ModuleViewResidency::absent;
        ModuleEngineActivity engineActivity = ModuleEngineActivity::released;
        ModuleHealth health = ModuleHealth::healthy;
        std::uint64_t leaseEpoch = 0;
    };

    Generation beginGeneration() noexcept;
    Generation currentGeneration() const noexcept { return generation; }
    bool isCurrent(Generation candidate) const noexcept;

    CoreMilestoneResult markFirstWaveform(Generation candidate) noexcept;
    CoreMilestoneResult markPlayable(Generation candidate) noexcept;
    bool isCoreReady(Generation candidate) const noexcept;

    LifecycleTransitionResult setAvailability(Generation candidate,
                                              OptionalModuleId module,
                                              ModuleAvailability availability) noexcept;
    LifecycleTransitionResult trigger(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult describePlacement(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult prepare(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult activate(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult attachView(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult suspend(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult release(Generation candidate, OptionalModuleId module) noexcept;
    LifecycleTransitionResult fail(Generation candidate, OptionalModuleId module) noexcept;

    std::optional<Lease> acquireLease(Generation candidate, OptionalModuleId module) const noexcept;
    bool isLeaseCurrent(const Lease& lease) const noexcept;
    std::optional<ModuleSnapshot> snapshot(Generation candidate, OptionalModuleId module) const noexcept;

private:
    static constexpr std::size_t moduleCount = static_cast<std::size_t>(OptionalModuleId::count);
    static std::optional<std::size_t> moduleIndex(OptionalModuleId module) noexcept;
    ModuleSnapshot* mutableSnapshot(Generation candidate, OptionalModuleId module) noexcept;
    const ModuleSnapshot* findSnapshot(Generation candidate, OptionalModuleId module) const noexcept;
    CoreMilestoneResult markCoreMilestone(Generation candidate, bool& milestone) noexcept;
    void invalidateLeases(ModuleSnapshot& state) noexcept;

    Generation generation;
    std::uint64_t nextGeneration = 1;
    std::uint64_t nextLeaseEpoch = 1;
    bool firstWaveform = false;
    bool playable = false;
    bool coreReady = false;
    std::array<ModuleSnapshot, moduleCount> modules {};
};
} // namespace singleeditor
