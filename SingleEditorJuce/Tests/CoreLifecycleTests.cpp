#include "Core/CoreLifecycleCoordinator.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace singleeditor;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "CoreLifecycleTests failed: " << message << '\n';
        std::exit(1);
    }
}

void reachCoreReady(CoreLifecycleCoordinator& coordinator,
                    CoreLifecycleCoordinator::Generation generation)
{
    require(coordinator.markFirstWaveform(generation) == CoreMilestoneResult::applied,
            "first truthful waveform alone does not claim CoreReady");
    require(!coordinator.isCoreReady(generation), "first waveform alone is insufficient");
    require(coordinator.markPlayable(generation) == CoreMilestoneResult::coreReadyReached,
            "playable plus first waveform reaches CoreReady exactly once");
    require(coordinator.isCoreReady(generation), "CoreReady latch stays observable");
}

void testGenerationLatchAndStaleCallbacks()
{
    CoreLifecycleCoordinator coordinator;
    const auto first = coordinator.beginGeneration();
    reachCoreReady(coordinator, first);
    require(coordinator.markPlayable(first) == CoreMilestoneResult::unchanged,
            "repeated milestone is idempotent");

    require(coordinator.trigger(first, OptionalModuleId::dsp) == LifecycleTransitionResult::applied,
            "explicit DSP trigger is accepted post-Core");
    require(coordinator.prepare(first, OptionalModuleId::dsp) == LifecycleTransitionResult::applied,
            "triggered DSP prepares post-Core");
    const auto lease = coordinator.acquireLease(first, OptionalModuleId::dsp);
    require(lease.has_value() && coordinator.isLeaseCurrent(*lease), "active generation lease is current");

    const auto second = coordinator.beginGeneration();
    require(!coordinator.isCoreReady(second), "new generation resets only its own CoreReady latch");
    require(coordinator.markPlayable(first) == CoreMilestoneResult::rejectedStaleGeneration,
            "old callback cannot mark the replacement generation");
    require(!coordinator.isLeaseCurrent(*lease), "generation replacement invalidates the old lease");
    require(!coordinator.snapshot(first, OptionalModuleId::dsp).has_value(),
            "stale generation state is not exposed");
}

void testOrthogonalRepeatSafeTransitions()
{
    CoreLifecycleCoordinator coordinator;
    const auto generation = coordinator.beginGeneration();
    require(coordinator.describePlacement(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "restore may describe placement before CoreReady");
    require(coordinator.trigger(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "trigger records intent without constructing a module");
    require(coordinator.prepare(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::rejectedBeforeCoreReady,
            "prepare is fenced before CoreReady");

    reachCoreReady(coordinator, generation);
    require(coordinator.prepare(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "prepare is admitted after CoreReady");
    require(coordinator.prepare(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::unchanged,
            "repeated prepare does not duplicate work");
    require(coordinator.activate(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "prepared engine activates");
    require(coordinator.attachView(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "View residency is independent and explicit");
    require(coordinator.suspend(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "active module suspends");
    require(coordinator.suspend(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::unchanged,
            "repeated suspend is safe");
    require(coordinator.activate(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "suspended engine resumes without requiring a View");
    require(coordinator.release(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "owner release reaches zero state");
    require(coordinator.release(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::unchanged,
            "repeated release is safe");
}

void testOrdinaryZeroInstanceAndCapabilityFailure()
{
    CoreLifecycleCoordinator coordinator;
    const auto generation = coordinator.beginGeneration();
    constexpr std::array optionalModules {
        OptionalModuleId::dsp,
        OptionalModuleId::ambisonics,
        OptionalModuleId::bnkWem,
        OptionalModuleId::diagnostics,
        OptionalModuleId::recovery,
        OptionalModuleId::cli
    };
    std::array<int, optionalModules.size()> factoryRequests {};
    std::array<int, optionalModules.size()> instances {};

    reachCoreReady(coordinator, generation);
    for (std::size_t index = 0; index < optionalModules.size(); ++index)
    {
        const auto state = coordinator.snapshot(generation, optionalModules[index]);
        require(state.has_value(), "ordinary module snapshot exists without constructing a module");
        require(state->intent == ModuleIntent::none
                    && state->engineActivity == ModuleEngineActivity::released,
                "ordinary CoreReady leaves optional module unrequested and released");
        require(factoryRequests[index] == 0 && instances[index] == 0,
                "ordinary open makes zero optional factory requests and instances");
    }

    constexpr auto ambisonicsIndex = 1u;
    ++factoryRequests[ambisonicsIndex];
    require(coordinator.trigger(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "explicit Ambisonics trigger is required");
    if (coordinator.prepare(generation, OptionalModuleId::ambisonics)
        == LifecycleTransitionResult::applied)
    {
        ++instances[ambisonicsIndex];
    }
    require(instances[ambisonicsIndex] == 1, "explicit trigger creates one owning instance");
    require(coordinator.activate(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "explicit Ambisonics instance activates");

    require(coordinator.trigger(generation, OptionalModuleId::dsp)
                == LifecycleTransitionResult::applied,
            "independent DSP trigger succeeds");
    require(coordinator.prepare(generation, OptionalModuleId::dsp)
                == LifecycleTransitionResult::applied,
            "independent DSP prepares");
    require(coordinator.activate(generation, OptionalModuleId::dsp)
                == LifecycleTransitionResult::applied,
            "independent DSP activates");
    require(coordinator.fail(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::applied,
            "synthetic Ambisonics failure is capability scoped");
    require(coordinator.fail(generation, OptionalModuleId::ambisonics)
                == LifecycleTransitionResult::unchanged,
            "repeated failure is safe");
    require(coordinator.isCoreReady(generation), "module failure cannot reset CoreReady");
    const auto dspState = coordinator.snapshot(generation, OptionalModuleId::dsp);
    const auto ambisonicsState = coordinator.snapshot(generation, OptionalModuleId::ambisonics);
    require(dspState->engineActivity == ModuleEngineActivity::active
                && dspState->health == ModuleHealth::healthy,
            "failure does not degrade a sibling capability");
    require(ambisonicsState->engineActivity == ModuleEngineActivity::released
                && ambisonicsState->health == ModuleHealth::failed,
            "failed capability releases its engine and reports typed health");
}

void testDependencyFailureCannotDelayCore()
{
    CoreLifecycleCoordinator coordinator;
    const auto generation = coordinator.beginGeneration();
    require(coordinator.setAvailability(generation,
                                        OptionalModuleId::diagnostics,
                                        ModuleAvailability::unavailable)
                == LifecycleTransitionResult::applied,
            "synthetic missing dependency is recorded on its owning capability");
    require(coordinator.trigger(generation, OptionalModuleId::diagnostics)
                == LifecycleTransitionResult::rejectedUnavailable,
            "missing optional dependency fails closed before any factory request");

    reachCoreReady(coordinator, generation);
    require(coordinator.prepare(generation, OptionalModuleId::diagnostics)
                == LifecycleTransitionResult::rejectedUnavailable,
            "missing dependency remains capability scoped after CoreReady");
    require(coordinator.isCoreReady(generation),
            "optional dependency failure cannot reset waveform and transport readiness");

    require(coordinator.trigger(generation, OptionalModuleId::dsp)
                == LifecycleTransitionResult::applied
                && coordinator.prepare(generation, OptionalModuleId::dsp)
                       == LifecycleTransitionResult::applied
                && coordinator.activate(generation, OptionalModuleId::dsp)
                       == LifecycleTransitionResult::applied,
            "an unrelated optional capability remains usable");
}
} // namespace

int main()
{
    testGenerationLatchAndStaleCallbacks();
    testOrthogonalRepeatSafeTransitions();
    testOrdinaryZeroInstanceAndCapabilityFailure();
    testDependencyFailureCannotDelayCore();
    std::cout << "CoreLifecycleTests passed\n";
    return 0;
}
