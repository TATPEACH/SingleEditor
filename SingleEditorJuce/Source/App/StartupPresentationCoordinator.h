#pragma once

#include <cstdint>

namespace singleeditor
{
enum class StartupPresentationResult
{
    applied,
    unchanged,
    staleGeneration,
    rejectedBeforeCoreReady,
    rejectedInvalidTransition
};

enum class StartupRendererAttachState
{
    dormant,
    attaching,
    attached,
    failed
};

enum class StartupHostMutation
{
    resize,
    activate,
    maximize,
    dpiChange
};

class StartupPresentationCoordinator final
{
public:
    using Generation = std::uint64_t;

    struct Snapshot
    {
        Generation generation = 0;
        bool fileNameVisible = false;
        bool truthfulWaveformVisible = false;
        bool playable = false;
        bool coreReady = false;
        bool coreSurfaceVisible = false;
        bool rendererSurfaceVisible = false;
        StartupRendererAttachState rendererState = StartupRendererAttachState::dormant;
        std::uint64_t hostMutationRevision = 0;

        [[nodiscard]] bool hasVisibleSurface() const noexcept;
        [[nodiscard]] bool preservesCoreTruth() const noexcept;
    };

    Generation beginGeneration() noexcept;
    StartupPresentationResult markFileNameVisible(Generation generation) noexcept;
    StartupPresentationResult markTruthfulWaveformVisible(Generation generation) noexcept;
    StartupPresentationResult markPlayable(Generation generation) noexcept;
    StartupPresentationResult beginRendererAttach(Generation generation) noexcept;
    StartupPresentationResult completeRendererAttach(Generation generation) noexcept;
    StartupPresentationResult failRendererAttach(Generation generation) noexcept;
    StartupPresentationResult noteHostMutation(Generation generation,
                                               StartupHostMutation mutation) noexcept;

    [[nodiscard]] bool isCurrent(Generation generation) const noexcept;
    [[nodiscard]] const Snapshot& snapshot() const noexcept;

private:
    StartupPresentationResult mark(bool& milestone, Generation generation) noexcept;
    void refreshCoreReady() noexcept;

    Snapshot state;
};
} // namespace singleeditor
