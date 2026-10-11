#include "App/StartupPresentationCoordinator.h"

namespace singleeditor
{
bool StartupPresentationCoordinator::Snapshot::hasVisibleSurface() const noexcept
{
    return coreSurfaceVisible || rendererSurfaceVisible;
}

bool StartupPresentationCoordinator::Snapshot::preservesCoreTruth() const noexcept
{
    return coreReady && fileNameVisible && truthfulWaveformVisible && playable && hasVisibleSurface();
}

StartupPresentationCoordinator::Generation StartupPresentationCoordinator::beginGeneration() noexcept
{
    const auto nextGeneration = state.generation + 1;
    state = {};
    state.generation = nextGeneration;
    state.coreSurfaceVisible = true;
    return nextGeneration;
}

StartupPresentationResult StartupPresentationCoordinator::markFileNameVisible(Generation generation) noexcept
{
    return mark(state.fileNameVisible, generation);
}

StartupPresentationResult StartupPresentationCoordinator::markTruthfulWaveformVisible(Generation generation) noexcept
{
    const auto result = mark(state.truthfulWaveformVisible, generation);
    refreshCoreReady();
    return result;
}

StartupPresentationResult StartupPresentationCoordinator::markPlayable(Generation generation) noexcept
{
    const auto result = mark(state.playable, generation);
    refreshCoreReady();
    return result;
}

StartupPresentationResult StartupPresentationCoordinator::beginRendererAttach(Generation generation) noexcept
{
    if (!isCurrent(generation))
        return StartupPresentationResult::staleGeneration;
    if (!state.coreReady)
        return StartupPresentationResult::rejectedBeforeCoreReady;
    if (state.rendererState == StartupRendererAttachState::attaching
        || state.rendererState == StartupRendererAttachState::attached)
    {
        return StartupPresentationResult::unchanged;
    }

    state.rendererState = StartupRendererAttachState::attaching;
    state.coreSurfaceVisible = true;
    state.rendererSurfaceVisible = false;
    return StartupPresentationResult::applied;
}

StartupPresentationResult StartupPresentationCoordinator::completeRendererAttach(Generation generation) noexcept
{
    if (!isCurrent(generation))
        return StartupPresentationResult::staleGeneration;
    if (state.rendererState == StartupRendererAttachState::attached)
        return StartupPresentationResult::unchanged;
    if (state.rendererState != StartupRendererAttachState::attaching)
        return StartupPresentationResult::rejectedInvalidTransition;

    state.rendererState = StartupRendererAttachState::attached;
    state.rendererSurfaceVisible = true;
    state.coreSurfaceVisible = false;
    return StartupPresentationResult::applied;
}

StartupPresentationResult StartupPresentationCoordinator::failRendererAttach(Generation generation) noexcept
{
    if (!isCurrent(generation))
        return StartupPresentationResult::staleGeneration;
    if (state.rendererState == StartupRendererAttachState::failed)
        return StartupPresentationResult::unchanged;
    if (state.rendererState != StartupRendererAttachState::attaching)
        return StartupPresentationResult::rejectedInvalidTransition;

    state.rendererState = StartupRendererAttachState::failed;
    state.coreSurfaceVisible = true;
    state.rendererSurfaceVisible = false;
    return StartupPresentationResult::applied;
}

StartupPresentationResult StartupPresentationCoordinator::noteHostMutation(Generation generation,
                                                                            StartupHostMutation mutation) noexcept
{
    if (!isCurrent(generation))
        return StartupPresentationResult::staleGeneration;

    static_cast<void>(mutation);
    ++state.hostMutationRevision;
    return StartupPresentationResult::applied;
}

bool StartupPresentationCoordinator::isCurrent(Generation generation) const noexcept
{
    return generation != 0 && generation == state.generation;
}

const StartupPresentationCoordinator::Snapshot& StartupPresentationCoordinator::snapshot() const noexcept
{
    return state;
}

StartupPresentationResult StartupPresentationCoordinator::mark(bool& milestone,
                                                                Generation generation) noexcept
{
    if (!isCurrent(generation))
        return StartupPresentationResult::staleGeneration;
    if (milestone)
        return StartupPresentationResult::unchanged;

    milestone = true;
    return StartupPresentationResult::applied;
}

void StartupPresentationCoordinator::refreshCoreReady() noexcept
{
    state.coreReady = state.truthfulWaveformVisible && state.playable;
}
} // namespace singleeditor
