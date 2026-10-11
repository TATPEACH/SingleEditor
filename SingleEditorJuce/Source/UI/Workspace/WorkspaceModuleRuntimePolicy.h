#pragma once

#include "Core/CoreLifecycleCoordinator.h"
#include "WorkspaceModuleLayout.h"

namespace singleeditor
{
// A view's presence is not permission to start an audio processor. DSP views
// use explicit Preview/Apply leases; document sidebars have no audio runtime.
[[nodiscard]] constexpr std::optional<OptionalModuleId> workspaceViewRuntime(
    WorkspaceModuleId id) noexcept
{
    switch (id)
    {
        case WorkspaceModuleId::outputMeter: return OptionalModuleId::meter;
        case WorkspaceModuleId::ambisonicEnergy: return OptionalModuleId::ambisonics;
        default: return std::nullopt;
    }
}
}
