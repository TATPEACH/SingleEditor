#pragma once

namespace singleeditor
{
struct WorkspaceCanvasAuthorityInput
{
    bool hostConstructed = false;
    bool hasActiveModules = false;
    bool presentationVisible = false;
};

inline bool workspaceOwnsCanvas(const WorkspaceCanvasAuthorityInput input) noexcept
{
    // Visibility is presentation state, not ownership state. A transient hide
    // during post-Core promotion must never make MainComponent fall back to a
    // second layout that stretches the waveform under the Mag rectangles.
    return input.hostConstructed && input.hasActiveModules;
}
} // namespace singleeditor
