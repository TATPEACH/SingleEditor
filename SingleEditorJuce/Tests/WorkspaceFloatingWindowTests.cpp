#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "UI/Workspace/WorkspaceFloatingWindowModel.h"
#include "UI/Workspace/WorkspaceModuleLayout.h"

namespace
{
void require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "WorkspaceFloatingWindowTests failed: " << message << '\n';
    std::exit(1);
}

WorkspaceDisplaySnapshot display(std::string affinity,
                                 WorkspaceLogicalBounds workArea,
                                 int scalePercent,
                                 bool primary = false)
{
    return { std::move(affinity), workArea, scalePercent, primary };
}

WorkspaceFloatingPlacement placement(int x = 120, int y = 100)
{
    return { { x, y, 640, 420 }, { 0, 0, 1920, 1040 }, "primary", 100 };
}

WorkspaceModuleLayout makeLayout()
{
    WorkspaceModuleLayout layout;
    layout.registerModule({ WorkspaceModuleId::outputMeter,
                            1,
                            1000,
                            160,
                            { 1, 1 },
                            { 1, 1 },
                            { 2, 3 } });
    layout.registerModule({ WorkspaceModuleId::ambisonicEnergy,
                            0,
                            780,
                            180,
                            { 1, 1 },
                            { 1, 1 },
                            { 2, 3 } });
    layout.setAvailable(WorkspaceModuleId::outputMeter, true);
    layout.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    return layout;
}

void runExactAndDpiRestoreTest()
{
    const auto savedDisplay = display("primary", { 0, 0, 1920, 1040 }, 100, true);
    const auto saved = WorkspaceFloatingWindowModel::capture({ 120, 100, 640, 420 }, savedDisplay);
    const auto exact = WorkspaceFloatingWindowModel::resolve(saved, { savedDisplay }, { 30, 30, 1200, 760 });
    require(exact.recovery == WorkspaceFloatingRecovery::exact
                && exact.placement == saved,
            "unchanged display geometry must restore exactly");

    const auto scaled = WorkspaceFloatingWindowModel::resolve(
        saved,
        { display("primary", { 0, 0, 1920, 1040 }, 150, true) },
        { 30, 30, 1200, 760 });
    require(scaled.recovery == WorkspaceFloatingRecovery::dpiAdjusted
                && scaled.placement.bounds == saved.bounds
                && scaled.placement.scalePercent == 150,
            "logical bounds survive a DPI-only transition while the new scale is recorded");
}

void runWorkAreaAndMonitorFallbackTest()
{
    const auto saved = WorkspaceFloatingWindowModel::capture(
        { 960, 520, 720, 440 },
        display("secondary", { 0, 0, 1920, 1040 }, 100));
    const auto adjusted = WorkspaceFloatingWindowModel::resolve(
        saved,
        { display("secondary", { 0, 0, 1280, 720 }, 100) },
        { 10, 10, 900, 600 });
    require(adjusted.recovery == WorkspaceFloatingRecovery::workAreaAdjusted
                && adjusted.placement.bounds.right() <= 1280
                && adjusted.placement.bounds.bottom() <= 720,
            "work-area changes remap and clamp the floating window");

    const std::vector<WorkspaceDisplaySnapshot> remaining {
        display("primary", { 0, 0, 1600, 860 }, 125, true)
    };
    const auto fallback = WorkspaceFloatingWindowModel::resolve(
        saved,
        remaining,
        { 100, 100, 1100, 700 });
    require((fallback.recovery == WorkspaceFloatingRecovery::monitorFallback
             || fallback.recovery == WorkspaceFloatingRecovery::offscreenFallback)
                && fallback.placement.monitorAffinity == "primary"
                && WorkspaceFloatingWindowModel::isSafelyVisible(fallback.placement.bounds, remaining),
            "a missing monitor recovers onto the visible main-window display");
}

void runOffscreenAndNoDisplayFallbackTest()
{
    WorkspaceFloatingPlacement offscreen;
    offscreen.bounds = { 50000, -40000, 520, 360 };
    offscreen.sourceWorkArea = { 1920, 0, 1920, 1040 };
    offscreen.monitorAffinity = "missing";
    const std::vector<WorkspaceDisplaySnapshot> displays {
        display("primary", { 0, 0, 1366, 728 }, 100, true)
    };
    const auto recovered = WorkspaceFloatingWindowModel::resolve(
        offscreen,
        displays,
        { 80, 80, 1000, 600 });
    require(recovered.recovery == WorkspaceFloatingRecovery::offscreenFallback
                && WorkspaceFloatingWindowModel::isSafelyVisible(recovered.placement.bounds, displays),
            "off-screen coordinates are centred and clamped to a usable display");

    const auto headless = WorkspaceFloatingWindowModel::resolve(
        offscreen,
        {},
        { 20, 20, 900, 620 });
    require(!headless.displayAvailable
                && headless.recovery == WorkspaceFloatingRecovery::noDisplayFallback
                && headless.placement.bounds.width <= 900
                && headless.placement.bounds.height <= 620,
            "headless recovery remains bounded without inventing a monitor");
}

void runModuleFloatRedockAndUndoTest()
{
    auto layout = makeLayout();
    const auto originalPlacement = layout.getGridPlacement(WorkspaceModuleId::outputMeter);
    require(originalPlacement.has_value(), "meter begins as a placed Mag");
    require(layout.floatModule(WorkspaceModuleId::outputMeter, placement())
                == WorkspaceGridMutationResult::committed,
            "an available Mag can leave the main grid as one transaction");
    require(layout.isFloating(WorkspaceModuleId::outputMeter)
                && layout.isActive(WorkspaceModuleId::outputMeter),
            "floating preserves stable module activation and state ownership");
    const auto grid = layout.gridItems();
    require(std::none_of(grid.begin(), grid.end(), [] (const WorkspaceGridItem& item)
            {
                return item.id == WorkspaceModuleId::outputMeter;
            }),
            "a floating Mag is not simultaneously painted in the main grid");
    require(layout.undoGridMutation() && !layout.isFloating(WorkspaceModuleId::outputMeter),
            "float is one undoable workspace mutation");
    require(layout.redoGridMutation() && layout.isFloating(WorkspaceModuleId::outputMeter),
            "redo restores the floating identity and placement");
    require(layout.redockModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && !layout.isFloating(WorkspaceModuleId::outputMeter),
            "explicit Redock returns the same Mag to the main grid");
}

void runTabStackRoundTripTest()
{
    auto layout = makeLayout();
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::ambisonicEnergy,
                              WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "fixture creates a tab stack");
    require(layout.activateTab(WorkspaceModuleId::ambisonicEnergy)
                != WorkspaceGridMutationResult::unknownModule,
            "energy tab can become active before floating");
    require(layout.floatModule(WorkspaceModuleId::outputMeter, placement(240, 160))
                == WorkspaceGridMutationResult::committed,
            "floating one tab moves the complete stable stack tile");
    const auto floating = layout.floatingTileForModule(WorkspaceModuleId::ambisonicEnergy);
    require(floating.has_value() && floating->members.size() == 2,
            "both tab identities remain members of the floating stack");

    const auto encoded = layout.serialiseUserState();
    auto restored = makeLayout();
    const auto result = restored.restoreUserState(encoded);
    const auto stack = restored.tabStackForModule(WorkspaceModuleId::outputMeter);
    require(result.status == WorkspaceRestoreStatus::applied
                && restored.isFloating(WorkspaceModuleId::outputMeter)
                && restored.isFloating(WorkspaceModuleId::ambisonicEnergy)
                && stack.has_value()
                && stack->activeTab == WorkspaceModuleId::ambisonicEnergy,
            "preset restore preserves the floating stack, members, and active tab");
    require(restored.removeFloatingTile(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && !restored.isActive(WorkspaceModuleId::outputMeter)
                && !restored.isActive(WorkspaceModuleId::ambisonicEnergy),
            "a floating Tab Stack close removes every tab as one workspace transaction");

    auto oneTabRemaining = makeLayout();
    require(oneTabRemaining.dockModule(WorkspaceModuleId::outputMeter,
                                       WorkspaceModuleId::ambisonicEnergy,
                                       WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed
                && oneTabRemaining.floatModule(WorkspaceModuleId::outputMeter, placement())
                       == WorkspaceGridMutationResult::committed
                && oneTabRemaining.setModulePlaced(WorkspaceModuleId::outputMeter, false)
                       == WorkspaceGridMutationResult::committed
                && oneTabRemaining.isFloating(WorkspaceModuleId::ambisonicEnergy)
                && !oneTabRemaining.tabStackForModule(WorkspaceModuleId::ambisonicEnergy).has_value(),
            "removing one tab migrates the surviving tile identity without forcing a redock");
}

void runPrivilegedCoreAndOpaqueRestoreTest()
{
    auto layout = makeLayout();
    require(layout.floatModule(WorkspaceModuleId::coreWaveform, placement(300, 180))
                == WorkspaceGridMutationResult::committed
                && layout.isFloating(WorkspaceModuleId::coreWaveform),
            "the core waveform may float only as a privileged tile");
    require(layout.setModulePlaced(WorkspaceModuleId::coreWaveform, false)
                == WorkspaceGridMutationResult::protectedCore,
            "floating never grants Remove semantics to the waveform");
    require(layout.redockModule(WorkspaceModuleId::coreWaveform)
                == WorkspaceGridMutationResult::committed,
            "the privileged waveform always has a usable Redock path");

    auto encoded = layout.serialiseUserState();
    const auto marker = std::string("\"floating\"");
    const auto position = encoded.find(marker);
    const auto arrayStart = position == std::string::npos
        ? std::string::npos
        : encoded.find('[', position + marker.size());
    require(arrayStart != std::string::npos, "schema v2 emits a floating array");
    encoded.insert(arrayStart + 1,
                   "{\"id\":\"future-tool\",\"futureField\":\"retained\"},");
    auto restored = makeLayout();
    require(restored.restoreUserState(encoded).status == WorkspaceRestoreStatus::applied
                && restored.serialiseUserState().find("futureField") != std::string::npos,
            "unknown future floating records round-trip inert without waking a View");

    auto corruptKnown = layout.serialiseUserState();
    const auto corruptPosition = corruptKnown.find(marker);
    const auto corruptArrayStart = corruptKnown.find('[', corruptPosition + marker.size());
    corruptKnown.insert(corruptArrayStart + 1,
                        "{\"id\":\"output-meter\",\"privilegedCore\":false},");
    auto quarantined = makeLayout();
    const auto quarantineResult = quarantined.restoreUserState(corruptKnown);
    require(quarantineResult.status == WorkspaceRestoreStatus::applied
                && quarantineResult.quarantinedModuleCount == 1
                && !quarantined.isFloating(WorkspaceModuleId::outputMeter),
            "invalid known floating geometry is quarantined without corrupting the preset");
}
}

int main()
{
    runExactAndDpiRestoreTest();
    runWorkAreaAndMonitorFallbackTest();
    runOffscreenAndNoDisplayFallbackTest();
    runModuleFloatRedockAndUndoTest();
    runTabStackRoundTripTest();
    runPrivilegedCoreAndOpaqueRestoreTest();
    std::cout << "WorkspaceFloatingWindowTests passed\n";
    return 0;
}
