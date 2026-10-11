#include "App/StartupPresentationCoordinator.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "StartupPresentationTests failed: " << message << '\n';
        std::exit(1);
    }
}

void testTruthfulCoreGatesRendererAttach()
{
    StartupPresentationCoordinator coordinator;
    const auto generation = coordinator.beginGeneration();

    require(coordinator.snapshot().hasVisibleSurface(), "minimal core surface starts visible");
    require(coordinator.markFileNameVisible(generation) == StartupPresentationResult::applied,
            "file name is an independent truthful milestone");
    require(coordinator.beginRendererAttach(generation)
                == StartupPresentationResult::rejectedBeforeCoreReady,
            "metadata cannot masquerade as CoreReady");

    require(coordinator.markTruthfulWaveformVisible(generation) == StartupPresentationResult::applied,
            "truthful waveform can be presented before playback");
    require(!coordinator.snapshot().coreReady, "waveform alone is not CoreReady");
    require(coordinator.beginRendererAttach(generation)
                == StartupPresentationResult::rejectedBeforeCoreReady,
            "renderer remains dormant until playback is truthful");

    require(coordinator.markPlayable(generation) == StartupPresentationResult::applied,
            "playable milestone completes the core pair");
    require(coordinator.snapshot().coreReady, "waveform plus playable reaches CoreReady");
    require(coordinator.snapshot().preservesCoreTruth(), "truthful core remains visible at CoreReady");
}

void testAttachNeverCreatesABlankFrame()
{
    StartupPresentationCoordinator coordinator;
    const auto generation = coordinator.beginGeneration();
    coordinator.markFileNameVisible(generation);
    coordinator.markTruthfulWaveformVisible(generation);
    coordinator.markPlayable(generation);

    require(coordinator.beginRendererAttach(generation) == StartupPresentationResult::applied,
            "renderer attach begins after CoreReady");
    require(coordinator.snapshot().coreSurfaceVisible, "core surface stays visible during attach");
    require(!coordinator.snapshot().rendererSurfaceVisible, "renderer is not exposed before attach completes");
    require(coordinator.snapshot().preservesCoreTruth(), "attach preserves truthful transport surface");

    for (const auto mutation : { StartupHostMutation::resize,
                                 StartupHostMutation::activate,
                                 StartupHostMutation::maximize,
                                 StartupHostMutation::dpiChange })
    {
        require(coordinator.noteHostMutation(generation, mutation) == StartupPresentationResult::applied,
                "host mutation is accepted during attach");
        require(coordinator.snapshot().preservesCoreTruth(), "host mutation cannot blank or reset CoreReady");
    }
    require(coordinator.snapshot().hostMutationRevision == 4, "all host mutations are observed");

    require(coordinator.completeRendererAttach(generation) == StartupPresentationResult::applied,
            "renderer becomes visible only after a completed attach");
    require(!coordinator.snapshot().coreSurfaceVisible, "core surface retires after renderer is visible");
    require(coordinator.snapshot().rendererSurfaceVisible, "completed renderer owns the visible surface");
    require(coordinator.snapshot().preservesCoreTruth(), "renderer handoff preserves core truth");
}

void testFailureRetryAndStaleGenerationSafety()
{
    StartupPresentationCoordinator coordinator;
    const auto first = coordinator.beginGeneration();
    coordinator.markFileNameVisible(first);
    coordinator.markTruthfulWaveformVisible(first);
    coordinator.markPlayable(first);
    coordinator.beginRendererAttach(first);

    require(coordinator.failRendererAttach(first) == StartupPresentationResult::applied,
            "renderer failure is an explicit transition");
    require(coordinator.snapshot().coreSurfaceVisible, "renderer failure keeps the core surface visible");
    require(coordinator.snapshot().preservesCoreTruth(), "renderer failure cannot lose transport truth");
    require(coordinator.failRendererAttach(first) == StartupPresentationResult::unchanged,
            "duplicate failure is repeat-safe");
    require(coordinator.beginRendererAttach(first) == StartupPresentationResult::applied,
            "failed renderer attach can be retried");

    const auto second = coordinator.beginGeneration();
    require(coordinator.markPlayable(first) == StartupPresentationResult::staleGeneration,
            "stale playback callbacks cannot mutate the new document");
    require(coordinator.completeRendererAttach(first) == StartupPresentationResult::staleGeneration,
            "stale renderer callbacks cannot reveal an old surface");
    require(coordinator.snapshot().generation == second, "new generation remains authoritative");
    require(!coordinator.snapshot().coreReady, "new generation starts with no inherited readiness");
    require(coordinator.snapshot().coreSurfaceVisible, "new generation still has a non-blank core host");
}
} // namespace

int main()
{
    testTruthfulCoreGatesRendererAttach();
    testAttachNeverCreatesABlankFrame();
    testFailureRetryAndStaleGenerationSafety();
    std::cout << "StartupPresentationTests passed\n";
    return 0;
}
