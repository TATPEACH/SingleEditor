#include "WorkspaceFloatingWindowModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
int intersectionArea(WorkspaceLogicalBounds left, WorkspaceLogicalBounds right) noexcept
{
    const auto width = std::max(0, std::min(left.right(), right.right()) - std::max(left.x, right.x));
    const auto height = std::max(0, std::min(left.bottom(), right.bottom()) - std::max(left.y, right.y));
    return width * height;
}

WorkspaceLogicalBounds clampToWorkArea(WorkspaceLogicalBounds bounds,
                                       WorkspaceLogicalBounds workArea,
                                       int minimumWidth,
                                       int minimumHeight) noexcept
{
    const auto maximumWidth = std::max(1, workArea.width);
    const auto maximumHeight = std::max(1, workArea.height);
    bounds.width = std::clamp(bounds.width, std::min(minimumWidth, maximumWidth), maximumWidth);
    bounds.height = std::clamp(bounds.height, std::min(minimumHeight, maximumHeight), maximumHeight);
    bounds.x = std::clamp(bounds.x, workArea.x, workArea.right() - bounds.width);
    bounds.y = std::clamp(bounds.y, workArea.y, workArea.bottom() - bounds.height);
    return bounds;
}

const WorkspaceDisplaySnapshot* fallbackDisplay(
    const std::vector<WorkspaceDisplaySnapshot>& displays,
    WorkspaceLogicalBounds mainWindowBounds) noexcept
{
    const WorkspaceDisplaySnapshot* best = nullptr;
    auto bestArea = -1;
    for (const auto& display : displays)
    {
        if (!display.workArea.isValid())
            continue;
        const auto area = intersectionArea(mainWindowBounds, display.workArea);
        if (area > bestArea)
        {
            best = &display;
            bestArea = area;
        }
    }
    if (best != nullptr && bestArea > 0)
        return best;
    const auto primary = std::find_if(displays.begin(), displays.end(), [] (const auto& display)
    {
        return display.primary && display.workArea.isValid();
    });
    if (primary != displays.end())
        return &*primary;
    const auto first = std::find_if(displays.begin(), displays.end(), [] (const auto& display)
    {
        return display.workArea.isValid();
    });
    return first != displays.end() ? &*first : nullptr;
}

WorkspaceLogicalBounds mapBetweenWorkAreas(WorkspaceLogicalBounds bounds,
                                           WorkspaceLogicalBounds source,
                                           WorkspaceLogicalBounds target) noexcept
{
    if (!source.isValid() || !target.isValid())
        return bounds;
    const auto relativeX = static_cast<double>(bounds.x - source.x) / source.width;
    const auto relativeY = static_cast<double>(bounds.y - source.y) / source.height;
    const auto relativeWidth = static_cast<double>(bounds.width) / source.width;
    const auto relativeHeight = static_cast<double>(bounds.height) / source.height;
    return {
        target.x + static_cast<int>(std::lround(relativeX * target.width)),
        target.y + static_cast<int>(std::lround(relativeY * target.height)),
        static_cast<int>(std::lround(relativeWidth * target.width)),
        static_cast<int>(std::lround(relativeHeight * target.height))
    };
}
}

WorkspaceFloatingPlacement WorkspaceFloatingWindowModel::capture(
    WorkspaceLogicalBounds bounds,
    const WorkspaceDisplaySnapshot& display)
{
    WorkspaceFloatingPlacement result;
    result.bounds = clampToWorkArea(bounds, display.workArea, 1, 1);
    result.sourceWorkArea = display.workArea;
    result.monitorAffinity = display.affinity;
    result.scalePercent = std::clamp(display.scalePercent, 25, 800);
    return result;
}

WorkspaceResolvedFloatingPlacement WorkspaceFloatingWindowModel::resolve(
    const WorkspaceFloatingPlacement& saved,
    const std::vector<WorkspaceDisplaySnapshot>& displays,
    WorkspaceLogicalBounds mainWindowBounds,
    int minimumWidth,
    int minimumHeight)
{
    minimumWidth = std::max(1, minimumWidth);
    minimumHeight = std::max(1, minimumHeight);
    const WorkspaceDisplaySnapshot* target = nullptr;
    if (!saved.monitorAffinity.empty())
    {
        const auto match = std::find_if(displays.begin(), displays.end(), [&saved] (const auto& display)
        {
            return display.affinity == saved.monitorAffinity && display.workArea.isValid();
        });
        if (match != displays.end())
            target = &*match;
    }

    auto recovery = WorkspaceFloatingRecovery::exact;
    if (target == nullptr)
    {
        target = fallbackDisplay(displays, mainWindowBounds);
        recovery = WorkspaceFloatingRecovery::monitorFallback;
    }
    if (target == nullptr)
    {
        const auto fallback = mainWindowBounds.isValid()
            ? mainWindowBounds
            : WorkspaceLogicalBounds { 0, 0, std::max(640, minimumWidth), std::max(480, minimumHeight) };
        WorkspaceResolvedFloatingPlacement result;
        result.displayAvailable = false;
        result.recovery = WorkspaceFloatingRecovery::noDisplayFallback;
        result.placement = saved;
        result.placement.bounds = clampToWorkArea(saved.bounds.isValid() ? saved.bounds : fallback,
                                                  fallback,
                                                  minimumWidth,
                                                  minimumHeight);
        result.placement.sourceWorkArea = fallback;
        result.placement.monitorAffinity.clear();
        result.placement.scalePercent = 100;
        return result;
    }

    auto bounds = saved.bounds;
    const auto hadSafeVisibleArea = saved.bounds.isValid()
                                 && isSafelyVisible(saved.bounds, displays);
    if (!hadSafeVisibleArea)
    {
        bounds = {
            target->workArea.x + (target->workArea.width - std::max(minimumWidth, 480)) / 2,
            target->workArea.y + (target->workArea.height - std::max(minimumHeight, 320)) / 2,
            std::max(minimumWidth, saved.bounds.isValid() ? saved.bounds.width : 480),
            std::max(minimumHeight, saved.bounds.isValid() ? saved.bounds.height : 320)
        };
        recovery = WorkspaceFloatingRecovery::offscreenFallback;
    }
    else if (!(saved.sourceWorkArea == target->workArea))
    {
        bounds = mapBetweenWorkAreas(saved.bounds, saved.sourceWorkArea, target->workArea);
        if (recovery == WorkspaceFloatingRecovery::exact)
            recovery = WorkspaceFloatingRecovery::workAreaAdjusted;
    }

    const auto clamped = clampToWorkArea(bounds, target->workArea, minimumWidth, minimumHeight);
    if (!(clamped == bounds) && recovery == WorkspaceFloatingRecovery::exact)
        recovery = WorkspaceFloatingRecovery::workAreaAdjusted;
    if (saved.scalePercent != target->scalePercent
        && recovery == WorkspaceFloatingRecovery::exact)
    {
        recovery = WorkspaceFloatingRecovery::dpiAdjusted;
    }

    WorkspaceResolvedFloatingPlacement result;
    result.recovery = recovery;
    result.placement = capture(clamped, *target);
    return result;
}

bool WorkspaceFloatingWindowModel::isSafelyVisible(
    WorkspaceLogicalBounds bounds,
    const std::vector<WorkspaceDisplaySnapshot>& displays,
    int minimumVisibleTitleWidth,
    int titleBarHeight)
{
    if (!bounds.isValid())
        return false;
    const auto title = WorkspaceLogicalBounds {
        bounds.x,
        bounds.y,
        bounds.width,
        std::min(std::max(1, titleBarHeight), bounds.height)
    };
    for (const auto& display : displays)
    {
        if (!display.workArea.isValid())
            continue;
        const auto visibleWidth = std::max(
            0,
            std::min(title.right(), display.workArea.right()) - std::max(title.x, display.workArea.x));
        const auto visibleHeight = std::max(
            0,
            std::min(title.bottom(), display.workArea.bottom()) - std::max(title.y, display.workArea.y));
        if (visibleWidth >= std::max(1, minimumVisibleTitleWidth) && visibleHeight > 0)
            return true;
    }
    return false;
}
