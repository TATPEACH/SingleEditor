#include "Core/CoreLifecycleCoordinator.h"

namespace singleeditor
{
CoreLifecycleCoordinator::Generation CoreLifecycleCoordinator::beginGeneration() noexcept
{
    generation.value = nextGeneration++;
    if (nextGeneration == 0)
        nextGeneration = 1;

    firstWaveform = false;
    playable = false;
    coreReady = false;
    for (auto& module : modules)
    {
        module = {};
        module.generation = generation;
        module.leaseEpoch = nextLeaseEpoch++;
        if (nextLeaseEpoch == 0)
            nextLeaseEpoch = 1;
    }
    return generation;
}

bool CoreLifecycleCoordinator::isCurrent(Generation candidate) const noexcept
{
    return candidate && candidate == generation;
}

CoreMilestoneResult CoreLifecycleCoordinator::markFirstWaveform(Generation candidate) noexcept
{
    return markCoreMilestone(candidate, firstWaveform);
}

CoreMilestoneResult CoreLifecycleCoordinator::markPlayable(Generation candidate) noexcept
{
    return markCoreMilestone(candidate, playable);
}

bool CoreLifecycleCoordinator::isCoreReady(Generation candidate) const noexcept
{
    return isCurrent(candidate) && coreReady;
}

LifecycleTransitionResult CoreLifecycleCoordinator::setAvailability(Generation candidate,
                                                                     OptionalModuleId module,
                                                                     ModuleAvailability availability) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (state->availability == availability)
        return LifecycleTransitionResult::unchanged;

    state->availability = availability;
    if (availability == ModuleAvailability::unavailable)
    {
        state->intent = ModuleIntent::none;
        state->placement = ModulePlacement::absent;
        state->viewResidency = ModuleViewResidency::absent;
        state->engineActivity = ModuleEngineActivity::released;
        invalidateLeases(*state);
    }
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::trigger(Generation candidate,
                                                             OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (state->availability != ModuleAvailability::available)
        return LifecycleTransitionResult::rejectedUnavailable;
    if (state->health == ModuleHealth::failed)
        return LifecycleTransitionResult::rejectedFailed;
    if (state->intent == ModuleIntent::requested)
        return LifecycleTransitionResult::unchanged;

    state->intent = ModuleIntent::requested;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::describePlacement(Generation candidate,
                                                                       OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (state->placement != ModulePlacement::absent)
        return LifecycleTransitionResult::unchanged;

    state->placement = ModulePlacement::described;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::prepare(Generation candidate,
                                                             OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (!coreReady)
        return LifecycleTransitionResult::rejectedBeforeCoreReady;
    if (state->availability != ModuleAvailability::available)
        return LifecycleTransitionResult::rejectedUnavailable;
    if (state->intent != ModuleIntent::requested)
        return LifecycleTransitionResult::rejectedNotRequested;
    if (state->health == ModuleHealth::failed)
        return LifecycleTransitionResult::rejectedFailed;
    if (state->engineActivity != ModuleEngineActivity::released)
        return LifecycleTransitionResult::unchanged;

    state->engineActivity = ModuleEngineActivity::preparing;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::activate(Generation candidate,
                                                              OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (!coreReady)
        return LifecycleTransitionResult::rejectedBeforeCoreReady;
    if (state->health == ModuleHealth::failed)
        return LifecycleTransitionResult::rejectedFailed;
    if (state->intent != ModuleIntent::requested)
        return LifecycleTransitionResult::rejectedNotRequested;
    if (state->engineActivity == ModuleEngineActivity::active)
        return LifecycleTransitionResult::unchanged;
    if (state->engineActivity != ModuleEngineActivity::preparing
        && state->engineActivity != ModuleEngineActivity::suspended)
    {
        return LifecycleTransitionResult::rejectedInvalidState;
    }

    state->engineActivity = ModuleEngineActivity::active;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::attachView(Generation candidate,
                                                                OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (!coreReady)
        return LifecycleTransitionResult::rejectedBeforeCoreReady;
    if (state->health == ModuleHealth::failed)
        return LifecycleTransitionResult::rejectedFailed;
    if (state->intent != ModuleIntent::requested)
        return LifecycleTransitionResult::rejectedNotRequested;
    if (state->engineActivity == ModuleEngineActivity::released)
        return LifecycleTransitionResult::rejectedInvalidState;
    if (state->viewResidency == ModuleViewResidency::resident
        && state->placement == ModulePlacement::placed)
    {
        return LifecycleTransitionResult::unchanged;
    }

    state->placement = ModulePlacement::placed;
    state->viewResidency = ModuleViewResidency::resident;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::suspend(Generation candidate,
                                                             OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (state->engineActivity == ModuleEngineActivity::suspended
        && state->viewResidency != ModuleViewResidency::resident)
    {
        return LifecycleTransitionResult::unchanged;
    }
    if (state->engineActivity != ModuleEngineActivity::active
        && state->engineActivity != ModuleEngineActivity::preparing)
    {
        return LifecycleTransitionResult::rejectedInvalidState;
    }

    state->engineActivity = ModuleEngineActivity::suspended;
    if (state->viewResidency == ModuleViewResidency::resident)
        state->viewResidency = ModuleViewResidency::suspended;
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::release(Generation candidate,
                                                             OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    const auto alreadyReleased = state->intent == ModuleIntent::none
                              && state->placement == ModulePlacement::absent
                              && state->viewResidency == ModuleViewResidency::absent
                              && state->engineActivity == ModuleEngineActivity::released
                              && state->health == ModuleHealth::healthy;
    if (alreadyReleased)
        return LifecycleTransitionResult::unchanged;

    state->intent = ModuleIntent::none;
    state->placement = ModulePlacement::absent;
    state->viewResidency = ModuleViewResidency::absent;
    state->engineActivity = ModuleEngineActivity::released;
    state->health = ModuleHealth::healthy;
    invalidateLeases(*state);
    return LifecycleTransitionResult::applied;
}

LifecycleTransitionResult CoreLifecycleCoordinator::fail(Generation candidate,
                                                          OptionalModuleId module) noexcept
{
    auto* state = mutableSnapshot(candidate, module);
    if (state == nullptr)
        return LifecycleTransitionResult::rejectedStaleGeneration;
    if (state->health == ModuleHealth::failed
        && state->engineActivity == ModuleEngineActivity::released
        && state->viewResidency == ModuleViewResidency::absent)
    {
        return LifecycleTransitionResult::unchanged;
    }

    state->health = ModuleHealth::failed;
    state->engineActivity = ModuleEngineActivity::released;
    state->viewResidency = ModuleViewResidency::absent;
    invalidateLeases(*state);
    return LifecycleTransitionResult::applied;
}

std::optional<CoreLifecycleCoordinator::Lease> CoreLifecycleCoordinator::acquireLease(
    Generation candidate,
    OptionalModuleId module) const noexcept
{
    const auto* state = findSnapshot(candidate, module);
    if (state == nullptr
        || state->health != ModuleHealth::healthy
        || state->intent != ModuleIntent::requested
        || state->engineActivity == ModuleEngineActivity::released)
    {
        return std::nullopt;
    }
    return Lease { candidate, module, state->leaseEpoch };
}

bool CoreLifecycleCoordinator::isLeaseCurrent(const Lease& lease) const noexcept
{
    const auto* state = findSnapshot(lease.generation, lease.module);
    return state != nullptr
        && state->leaseEpoch == lease.epoch
        && state->health == ModuleHealth::healthy
        && state->engineActivity != ModuleEngineActivity::released;
}

std::optional<CoreLifecycleCoordinator::ModuleSnapshot> CoreLifecycleCoordinator::snapshot(
    Generation candidate,
    OptionalModuleId module) const noexcept
{
    if (const auto* state = findSnapshot(candidate, module))
        return *state;
    return std::nullopt;
}

std::optional<std::size_t> CoreLifecycleCoordinator::moduleIndex(OptionalModuleId module) noexcept
{
    const auto index = static_cast<std::size_t>(module);
    if (index >= moduleCount)
        return std::nullopt;
    return index;
}

CoreLifecycleCoordinator::ModuleSnapshot* CoreLifecycleCoordinator::mutableSnapshot(
    Generation candidate,
    OptionalModuleId module) noexcept
{
    if (!isCurrent(candidate))
        return nullptr;
    if (const auto index = moduleIndex(module))
        return &modules[*index];
    return nullptr;
}

const CoreLifecycleCoordinator::ModuleSnapshot* CoreLifecycleCoordinator::findSnapshot(
    Generation candidate,
    OptionalModuleId module) const noexcept
{
    if (!isCurrent(candidate))
        return nullptr;
    if (const auto index = moduleIndex(module))
        return &modules[*index];
    return nullptr;
}

CoreMilestoneResult CoreLifecycleCoordinator::markCoreMilestone(Generation candidate,
                                                                bool& milestone) noexcept
{
    if (!isCurrent(candidate))
        return CoreMilestoneResult::rejectedStaleGeneration;
    if (milestone)
        return CoreMilestoneResult::unchanged;

    milestone = true;
    if (!coreReady && firstWaveform && playable)
    {
        coreReady = true;
        return CoreMilestoneResult::coreReadyReached;
    }
    return CoreMilestoneResult::applied;
}

void CoreLifecycleCoordinator::invalidateLeases(ModuleSnapshot& state) noexcept
{
    state.leaseEpoch = nextLeaseEpoch++;
    if (nextLeaseEpoch == 0)
        nextLeaseEpoch = 1;
}
} // namespace singleeditor
