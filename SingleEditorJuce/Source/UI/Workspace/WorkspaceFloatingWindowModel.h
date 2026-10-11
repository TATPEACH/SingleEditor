#pragma once

#include <string>
#include <vector>

struct WorkspaceLogicalBounds
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }
    [[nodiscard]] bool isValid() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] bool operator==(const WorkspaceLogicalBounds& other) const noexcept
    {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

struct WorkspaceDisplaySnapshot
{
    std::string affinity;
    WorkspaceLogicalBounds workArea;
    int scalePercent = 100;
    bool primary = false;
};

struct WorkspaceFloatingPlacement
{
    WorkspaceLogicalBounds bounds { 80, 80, 480, 320 };
    WorkspaceLogicalBounds sourceWorkArea { 0, 0, 1920, 1040 };
    std::string monitorAffinity = "primary";
    int scalePercent = 100;

    [[nodiscard]] bool operator==(const WorkspaceFloatingPlacement& other) const noexcept
    {
        return bounds == other.bounds
            && sourceWorkArea == other.sourceWorkArea
            && monitorAffinity == other.monitorAffinity
            && scalePercent == other.scalePercent;
    }
};

enum class WorkspaceFloatingRecovery
{
    exact,
    workAreaAdjusted,
    dpiAdjusted,
    monitorFallback,
    offscreenFallback,
    noDisplayFallback
};

struct WorkspaceResolvedFloatingPlacement
{
    WorkspaceFloatingPlacement placement;
    WorkspaceFloatingRecovery recovery = WorkspaceFloatingRecovery::exact;
    bool displayAvailable = true;
};

class WorkspaceFloatingWindowModel
{
public:
    [[nodiscard]] static WorkspaceFloatingPlacement capture(
        WorkspaceLogicalBounds bounds,
        const WorkspaceDisplaySnapshot& display);

    [[nodiscard]] static WorkspaceResolvedFloatingPlacement resolve(
        const WorkspaceFloatingPlacement& saved,
        const std::vector<WorkspaceDisplaySnapshot>& displays,
        WorkspaceLogicalBounds mainWindowBounds,
        int minimumWidth = 240,
        int minimumHeight = 160);

    [[nodiscard]] static bool isSafelyVisible(
        WorkspaceLogicalBounds bounds,
        const std::vector<WorkspaceDisplaySnapshot>& displays,
        int minimumVisibleTitleWidth = 64,
        int titleBarHeight = 40);
};
