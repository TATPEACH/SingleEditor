#include "UI/Workspace/WorkspaceModuleLayout.h"
#include "UI/Workspace/WorkspacePresetCatalog.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "WorkspaceGridTests failed: " << message << '\n';
    std::exit(1);
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

WorkspaceGridPlacement placementOf(const WorkspaceModuleLayout& layout, WorkspaceModuleId id)
{
    const auto placement = layout.getGridPlacement(id);
    require(placement.has_value(), "requested placement exists");
    return *placement;
}

void requireNoOverlap(const WorkspaceModuleLayout& layout)
{
    const auto items = layout.gridItems();
    for (size_t left = 0; left < items.size(); ++left)
        for (size_t right = left + 1; right < items.size(); ++right)
            require(!items[left].placement.overlaps(items[right].placement),
                    "committed grid items never overlap");
}

void requireSameGrid(const WorkspaceModuleLayout& left, const WorkspaceModuleLayout& right)
{
    const auto leftItems = left.gridItems();
    const auto rightItems = right.gridItems();
    require(leftItems.size() == rightItems.size(), "deterministic grids contain the same item count");
    for (size_t index = 0; index < leftItems.size(); ++index)
    {
        require(leftItems[index].id == rightItems[index].id
                    && leftItems[index].privilegedCore == rightItems[index].privilegedCore
                    && leftItems[index].placement == rightItems[index].placement,
                "repeated operation sequence produces the same placement");
    }
}

void runPrivilegedCoreAndDefaultPackingTest()
{
    WorkspaceModuleLayout empty;
    const auto emptyItems = empty.gridItems();
    require(empty.activeModules().empty(), "default workspace has no optional active module");
    require(emptyItems.size() == 1
                && emptyItems.front().id == WorkspaceModuleId::coreWaveform
                && emptyItems.front().privilegedCore
                && emptyItems.front().placement == WorkspaceGridPlacement { 0, 0, { 3, 2 } },
            "truthful waveform is the always-present privileged grid surface");
    require(empty.setModulePlaced(WorkspaceModuleId::coreWaveform, false)
                == WorkspaceGridMutationResult::protectedCore,
            "workspace Remove cannot remove the core waveform");

    auto layout = makeLayout();
    require(placementOf(layout, WorkspaceModuleId::ambisonicEnergy)
                == WorkspaceGridPlacement { 3, 0, { 1, 1 } }
                && placementOf(layout, WorkspaceModuleId::outputMeter)
                == WorkspaceGridPlacement { 3, 1, { 1, 1 } },
            "optional modules auto-pack beside the protected waveform in stable reading order");
    requireNoOverlap(layout);
}

void runDeterministicMoveUndoAndRedoTest()
{
    auto layout = makeLayout();
    const auto originalMeter = placementOf(layout, WorkspaceModuleId::outputMeter);
    const auto originalEnergy = placementOf(layout, WorkspaceModuleId::ambisonicEnergy);

    require(layout.moveModule(WorkspaceModuleId::outputMeter,
                              { 3, 0, { 1, 1 } },
                              WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "drop target wins and locally pushes the collided neighbour");
    require(placementOf(layout, WorkspaceModuleId::outputMeter).row == 0
                && placementOf(layout, WorkspaceModuleId::ambisonicEnergy).row == 1,
            "collision resolution uses a deterministic local swap");
    require(layout.gridUndoDepth() == 1, "one committed gesture produces one workspace undo step");
    require(layout.moveModule(WorkspaceModuleId::outputMeter,
                              { 3, 0, { 1, 1 } },
                              WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::noChange
                && layout.gridUndoDepth() == 1,
            "repeating an already committed move is idempotent");
    require(layout.undoGridMutation(), "workspace move can be undone");
    require(placementOf(layout, WorkspaceModuleId::outputMeter) == originalMeter
                && placementOf(layout, WorkspaceModuleId::ambisonicEnergy) == originalEnergy,
            "undo restores the complete placement transaction");
    require(layout.redoGridMutation(), "workspace move can be redone");
    requireNoOverlap(layout);
}

void runResizeRemoveAndDockBoundaryTest()
{
    auto layout = makeLayout();
    require(layout.moveModule(WorkspaceModuleId::outputMeter,
                              { 3, 0, { 1, 1 } },
                              WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "resize fixture first places the meter above its neighbour");
    require(layout.resizeModule(WorkspaceModuleId::outputMeter,
                                { 1, 2 },
                                WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "supported span resize commits through the same solver");
    const auto resizedMeter = placementOf(layout, WorkspaceModuleId::outputMeter);
    const auto displacedEnergy = placementOf(layout, WorkspaceModuleId::ambisonicEnergy);
    if (!(resizedMeter.span == WorkspaceGridSpan { 1, 2 } && displacedEnergy.row >= 2))
        std::cerr << "Resize diagnostic: meter=" << resizedMeter.column << ',' << resizedMeter.row
                  << ' ' << resizedMeter.span.columns << 'x' << resizedMeter.span.rows
                  << " energy=" << displacedEnergy.column << ',' << displacedEnergy.row << '\n';
    require(resizedMeter.span == WorkspaceGridSpan { 1, 2 }
                && displacedEnergy.row >= 2,
            "resize pushes only the collided neighbour below the target");
    requireNoOverlap(layout);

    require(layout.moveModule(WorkspaceModuleId::outputMeter,
                              { 0, 0, { 1, 2 } },
                              WorkspaceGridDirection::left)
                == WorkspaceGridMutationResult::protectedCore,
            "an optional Mag cannot displace or cover the waveform");
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::protectedCore,
            "the privileged waveform rejects centre stacking while retaining edge split support");

    require(layout.resizeModule(WorkspaceModuleId::outputMeter,
                                { 1, 1 },
                                WorkspaceGridDirection::up)
                == WorkspaceGridMutationResult::committed,
            "shrinking back to the compact span is undoable");
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::bottom)
                == WorkspaceGridMutationResult::committed,
            "edge docking uses the same renderer-neutral placement model");
    require(placementOf(layout, WorkspaceModuleId::outputMeter).row == 2,
            "bottom docking places the Mag directly after the protected core span");

    const auto beforeRemove = placementOf(layout, WorkspaceModuleId::ambisonicEnergy);
    require(layout.setModulePlaced(WorkspaceModuleId::ambisonicEnergy, false)
                == WorkspaceGridMutationResult::committed,
            "Remove is a workspace transaction");
    require(!layout.isActive(WorkspaceModuleId::ambisonicEnergy),
            "Remove clears placed intent while leaving capability registration intact");
    require(layout.undoGridMutation() && layout.isActive(WorkspaceModuleId::ambisonicEnergy)
                && placementOf(layout, WorkspaceModuleId::ambisonicEnergy) == beforeRemove,
            "Remove restores placement through workspace undo");
    requireNoOverlap(layout);
}

void runAtomicOpenCloseStateTest()
{
    WorkspaceModuleLayout source;
    source.registerModule({ WorkspaceModuleId::parametricEq,
                            2,
                            920,
                            180,
                            { 1, 1 },
                            { 2, 1 },
                            { 4, 3 } });
    source.setAvailable(WorkspaceModuleId::parametricEq, true);
    require(source.setModuleOpen(WorkspaceModuleId::parametricEq, false)
                == WorkspaceGridMutationResult::committed,
            "closing EQ atomically changes its user-facing open state");
    require(!source.isEnabled(WorkspaceModuleId::parametricEq)
                && !source.isPlaced(WorkspaceModuleId::parametricEq),
            "closed EQ is neither capability-enabled nor placed");

    WorkspaceModuleLayout restored;
    restored.registerModule({ WorkspaceModuleId::parametricEq,
                              2,
                              920,
                              180,
                              { 1, 1 },
                              { 2, 1 },
                              { 4, 3 } });
    require(restored.restoreUserState(source.serialiseUserState()).applied(),
            "disabled EQ state round-trips through the workspace preset");
    restored.setAvailable(WorkspaceModuleId::parametricEq, true);
    require(restored.setModuleOpen(WorkspaceModuleId::parametricEq, true)
                == WorkspaceGridMutationResult::committed
                && restored.isEnabled(WorkspaceModuleId::parametricEq)
                && restored.isPlaced(WorkspaceModuleId::parametricEq)
                && restored.isActive(WorkspaceModuleId::parametricEq),
            "one toolbar request opens a restored EQ instead of toggling placement alone");
    require(restored.undoGridMutation()
                && !restored.isEnabled(WorkspaceModuleId::parametricEq)
                && !restored.isPlaced(WorkspaceModuleId::parametricEq)
                && restored.redoGridMutation()
                && restored.isActive(WorkspaceModuleId::parametricEq),
            "open and close remain one coherent Undo and Redo transaction");
}

void runDockCloseReopenGeometryTest()
{
    auto layout = makeLayout();
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform, WorkspaceDockZone::left)
                == WorkspaceGridMutationResult::committed,
            "reopen fixture customizes the Output rail");
    const auto original = layout.serialiseUserState();
    require(layout.setModuleOpen(WorkspaceModuleId::outputMeter, false)
                == WorkspaceGridMutationResult::committed,
            "closing a docked Mag commits once");
    const auto closed = layout.serialiseUserState();
    const auto closeDepth = layout.gridUndoDepth();
    require(layout.setModuleOpen(WorkspaceModuleId::outputMeter, true)
                == WorkspaceGridMutationResult::committed
                && layout.serialiseUserState() == original
                && layout.gridUndoDepth() == closeDepth + 1,
            "reopen restores exact geometry as a new command rather than consuming Undo");
    require(layout.undoGridMutation() && layout.serialiseUserState() == closed
                && layout.redoGridMutation() && layout.serialiseUserState() == original,
            "Undo and Redo replay the close and recovered layout without drift");
    requireNoOverlap(layout);

    require(layout.setModuleOpen(WorkspaceModuleId::outputMeter, false)
                == WorkspaceGridMutationResult::committed
                && layout.dockModule(WorkspaceModuleId::ambisonicEnergy,
                                      WorkspaceModuleId::coreWaveform, WorkspaceDockZone::bottom)
                       == WorkspaceGridMutationResult::committed,
            "a later split changes the layout while Output is closed");
    const auto changedCore = placementOf(layout, WorkspaceModuleId::coreWaveform);
    const auto changedEnergy = placementOf(layout, WorkspaceModuleId::ambisonicEnergy);
    require(layout.setModuleOpen(WorkspaceModuleId::outputMeter, true)
                == WorkspaceGridMutationResult::committed
                && placementOf(layout, WorkspaceModuleId::coreWaveform) == changedCore
                && placementOf(layout, WorkspaceModuleId::ambisonicEnergy) == changedEnergy,
            "reopening never rolls back intervening layout edits");
    requireNoOverlap(layout);

    auto tabs = makeLayout();
    require(tabs.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::ambisonicEnergy, WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "reopen fixture creates a shared Tab tile");
    const auto originalTabs = tabs.serialiseUserState();
    require(tabs.setModuleOpen(WorkspaceModuleId::outputMeter, false)
                == WorkspaceGridMutationResult::committed
                && tabs.setModuleOpen(WorkspaceModuleId::outputMeter, true)
                       == WorkspaceGridMutationResult::committed
                && tabs.serialiseUserState() == originalTabs,
            "an unchanged Tab tile recovers its membership and active Tab");
    requireNoOverlap(tabs);
}

void runRepeatedSequenceDeterminismTest()
{
    auto left = makeLayout();
    auto right = makeLayout();
    for (auto* layout : { &left, &right })
    {
        require(layout->moveModule(WorkspaceModuleId::outputMeter,
                                   { 3, 0, { 1, 1 } },
                                   WorkspaceGridDirection::down)
                    == WorkspaceGridMutationResult::committed,
                "determinism fixture move commits");
        require(layout->resizeModule(WorkspaceModuleId::ambisonicEnergy,
                                     { 1, 2 },
                                     WorkspaceGridDirection::down)
                    == WorkspaceGridMutationResult::committed,
                "determinism fixture resize commits");
    }
    requireSameGrid(left, right);
    requireNoOverlap(left);
}

void runFiveZoneCoreDockingTest()
{
    WorkspaceModuleLayout layout;
    layout.registerModule({ WorkspaceModuleId::outputMeter,
                            0,
                            1000,
                            120,
                            { 1, 1 },
                            { 1, 1 },
                            { 2, 3 },
                            -1,
                            0 });
    layout.setAvailable(WorkspaceModuleId::outputMeter, true);

    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::left)
                == WorkspaceGridMutationResult::committed,
            "left edge docking atomically shifts rather than replaces the privileged core");
    require(placementOf(layout, WorkspaceModuleId::outputMeter).column == 0
                && placementOf(layout, WorkspaceModuleId::coreWaveform).column == 1,
            "left split preserves both Mag and waveform placement");
    requireNoOverlap(layout);
    require(layout.undoGridMutation(), "left split is one undoable workspace transaction");

    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::top)
                == WorkspaceGridMutationResult::committed,
            "top edge docking atomically shifts rather than covers the privileged core");
    require(placementOf(layout, WorkspaceModuleId::outputMeter).row == 0
                && placementOf(layout, WorkspaceModuleId::coreWaveform).row
                       == placementOf(layout, WorkspaceModuleId::outputMeter).span.rows,
            "top split preserves the core below the requested Mag");
    requireNoOverlap(layout);
    require(layout.undoGridMutation(), "top split is one undoable workspace transaction");

    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::right)
                == WorkspaceGridMutationResult::noChange,
            "right edge docking is idempotent when the default Mag already occupies that edge");
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::coreWaveform,
                              WorkspaceDockZone::bottom)
                == WorkspaceGridMutationResult::committed,
            "bottom edge docking uses the same grid transaction");
    requireNoOverlap(layout);
}

void runTabStackIdentityAndTileMutationTest()
{
    auto layout = makeLayout();
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::ambisonicEnergy,
                              WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "centre docking between general Mags creates a real Tab Stack");
    const auto stacks = layout.tabStacks();
    require(stacks.size() == 1 && stacks.front().id == "stack-1"
                && stacks.front().tabs.size() == 2
                && stacks.front().activeTab == WorkspaceModuleId::outputMeter,
            "the stack owns stable identity, ordered membership, and the dropped active tab");
    require(layout.gridItems().size() == 2
                && layout.isModuleVisible(WorkspaceModuleId::outputMeter)
                && !layout.isModuleVisible(WorkspaceModuleId::ambisonicEnergy),
            "one stack occupies one grid tile and exposes exactly one visible tab");

    const auto beforeResize = placementOf(layout, WorkspaceModuleId::outputMeter);
    const WorkspaceGridSpan expandedStackSpan { beforeResize.span.columns,
                                                beforeResize.span.rows + 1 };
    const auto stackResizeResult = layout.resizeModule(WorkspaceModuleId::outputMeter,
                                                       expandedStackSpan,
                                                       WorkspaceGridDirection::down);
    const auto meterAfterStackResize = placementOf(layout, WorkspaceModuleId::outputMeter);
    const auto energyAfterStackResize = placementOf(layout, WorkspaceModuleId::ambisonicEnergy);
    if (!(stackResizeResult == WorkspaceGridMutationResult::committed
          && meterAfterStackResize == energyAfterStackResize
          && meterAfterStackResize.span.rows == beforeResize.span.rows + 1))
        std::cerr << "Stack resize diagnostic: result=" << static_cast<int>(stackResizeResult)
                  << " before=" << beforeResize.column << ',' << beforeResize.row << ' '
                  << beforeResize.span.columns << 'x' << beforeResize.span.rows
                  << " meter=" << meterAfterStackResize.column << ',' << meterAfterStackResize.row << ' '
                  << meterAfterStackResize.span.columns << 'x' << meterAfterStackResize.span.rows
                  << " energy=" << energyAfterStackResize.column << ',' << energyAfterStackResize.row << ' '
                  << energyAfterStackResize.span.columns << 'x' << energyAfterStackResize.span.rows << '\n';
    require(stackResizeResult == WorkspaceGridMutationResult::committed
                && meterAfterStackResize == energyAfterStackResize
                && meterAfterStackResize.span.rows == beforeResize.span.rows + 1,
            "resize mutates the Tab Stack tile instead of one hidden child");
    require(layout.activateTab(WorkspaceModuleId::ambisonicEnergy)
                == WorkspaceGridMutationResult::committed
                && layout.isModuleVisible(WorkspaceModuleId::ambisonicEnergy)
                && !layout.isModuleVisible(WorkspaceModuleId::outputMeter),
            "tab activation changes only visible identity through one workspace transaction");
    layout.setEnabled(WorkspaceModuleId::ambisonicEnergy, false);
    require(layout.isModuleVisible(WorkspaceModuleId::outputMeter)
                && !layout.isModuleVisible(WorkspaceModuleId::ambisonicEnergy),
            "disabling the active tab deterministically reveals the next usable stack member");
    layout.setEnabled(WorkspaceModuleId::ambisonicEnergy, true);
    require(layout.activateTab(WorkspaceModuleId::ambisonicEnergy)
                == WorkspaceGridMutationResult::committed,
            "a re-enabled tab remains addressable without losing stable stack identity");

    require(layout.setModulePlaced(WorkspaceModuleId::ambisonicEnergy, false)
                == WorkspaceGridMutationResult::committed
                && layout.tabStacks().empty()
                && layout.isModuleVisible(WorkspaceModuleId::outputMeter),
            "removing one of two tabs dissolves the one-member container without losing the survivor");
}

void runVersionedPresetMigrationAndOpaquePreservationTest()
{
    auto layout = makeLayout();
    require(layout.dockModule(WorkspaceModuleId::outputMeter,
                              WorkspaceModuleId::ambisonicEnergy,
                              WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "preset fixture creates a stack");
    require(layout.setModuleViewState(WorkspaceModuleId::outputMeter, 1, R"({"mode":"rms"})"),
            "bounded typed module view state is accepted");
    const auto saved = layout.serialiseUserState();
    require(saved.find("single-editor.workspace-preset") != std::string::npos
                && saved.find("\"version\": 2") != std::string::npos,
            "workspace save emits the versioned structural schema");

    WorkspaceModuleLayout restored;
    restored.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    restored.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    const auto restoreResult = restored.restoreUserState(saved);
    require(restoreResult.status == WorkspaceRestoreStatus::applied
                && restored.activeModules().empty(),
            "restore stages intent without treating unavailable modules as active");
    restored.setAvailable(WorkspaceModuleId::outputMeter, true);
    restored.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    require(restored.tabStacks().size() == 1
                && restored.getModuleViewState(WorkspaceModuleId::outputMeter).find("rms") != std::string::npos
                && restored.serialiseUserState() == saved,
            "save-load-save preserves layout, Tab identity, active tab, and local view state stably");

    WorkspaceModuleLayout upgraded;
    upgraded.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    upgraded.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    upgraded.registerModule({ WorkspaceModuleId::parametricEq, 2, 920, 180 });
    upgraded.registerModule({ WorkspaceModuleId::truePeakLimiter, 3, 900, 180 });
    const auto upgradedResult = upgraded.restoreUserState(saved);
    require(upgradedResult.status == WorkspaceRestoreStatus::applied
                && !upgraded.isEnabled(WorkspaceModuleId::parametricEq)
                && !upgraded.isPlaced(WorkspaceModuleId::parametricEq)
                && !upgraded.isEnabled(WorkspaceModuleId::truePeakLimiter)
                && !upgraded.isPlaced(WorkspaceModuleId::truePeakLimiter)
                && upgraded.inertModules().empty(),
            "an older valid preset remains readable when later builds register new known modules; "
            "missing modules retain the safe disabled and unplaced state");

    WorkspaceModuleLayout migrated;
    migrated.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    migrated.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    const auto migratedResult = migrated.restoreUserState(
        "v1|future-analyzer:1:4:900|output-meter:1:1:1000|ambisonic-energy:0:0:780");
    require(migratedResult.status == WorkspaceRestoreStatus::migratedLegacyV1
                && migrated.inertModules().size() == 1
                && migrated.serialiseUserState().find("future-analyzer") != std::string::npos,
            "legacy v1 migrates deterministically and preserves an unknown module as inert data");
    WorkspaceModuleLayout migratedRoundTrip;
    migratedRoundTrip.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    migratedRoundTrip.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(migratedRoundTrip.restoreUserState(migrated.serialiseUserState()).status
                == WorkspaceRestoreStatus::applied,
            "legacy migration emits an immediately valid non-overlapping V2 workspace before availability attaches");
    WorkspaceModuleLayout milestoneFiveMigration;
    milestoneFiveMigration.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    milestoneFiveMigration.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(milestoneFiveMigration.restoreUserState(
                "v1|ambisonic-energy:1:0:780|output-meter:1:1:1000").status
                == WorkspaceRestoreStatus::migratedLegacyV1,
            "the exact M3-05 two-module state enters the deterministic migration path");
    WorkspaceModuleLayout milestoneFiveReload;
    milestoneFiveReload.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    milestoneFiveReload.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(milestoneFiveReload.restoreUserState(milestoneFiveMigration.serialiseUserState()).status
                == WorkspaceRestoreStatus::applied,
            "the exact M3-05 state cannot migrate into a self-overlapping corrupt V2 preset");

    const auto legacyOverlapPreset = R"({"schema":"single-editor.workspace-preset","version":2,"presetId":"migrated-v1","core":{"instanceId":"core-waveform","placement":{"column":0,"row":0,"span":{"columns":3,"rows":2}}},"modules":[{"instanceId":"ambisonic-energy","moduleId":"ambisonic-energy","enabled":true,"placed":true,"order":0,"weight":780,"placement":{"column":3,"row":0,"span":{"columns":1,"rows":1}},"viewState":{"version":1,"payload":{}}},{"instanceId":"output-meter","moduleId":"output-meter","enabled":true,"placed":true,"order":1,"weight":1000,"placement":{"column":3,"row":0,"span":{"columns":1,"rows":2}},"viewState":{"version":1,"payload":{}}}],"stacks":[]})";
    WorkspaceModuleLayout repairedOverlap;
    repairedOverlap.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    repairedOverlap.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    const auto repairedResult = repairedOverlap.restoreUserState(legacyOverlapPreset);
    repairedOverlap.setAvailable(WorkspaceModuleId::outputMeter, true);
    repairedOverlap.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    require(repairedResult.status == WorkspaceRestoreStatus::repairedLegacyOverlap
                && repairedResult.repairedTileCount == 1
                && !placementOf(repairedOverlap, WorkspaceModuleId::outputMeter)
                       .overlaps(placementOf(repairedOverlap, WorkspaceModuleId::ambisonicEnergy)),
            "an early V2 right-rail overlap is repaired into the nearest filled non-overlapping rail");
    WorkspaceModuleLayout repairedReload;
    repairedReload.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    repairedReload.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(repairedReload.restoreUserState(repairedOverlap.serialiseUserState()).status
                == WorkspaceRestoreStatus::applied,
            "a repaired overlap serialises as a stable ordinary V2 preset on the next launch");

    WorkspaceModuleLayout dormantReservation;
    dormantReservation.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    dormantReservation.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    dormantReservation.setAvailable(WorkspaceModuleId::outputMeter, true);
    dormantReservation.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    const auto dormantSlot = placementOf(dormantReservation, WorkspaceModuleId::ambisonicEnergy);
    dormantReservation.setAvailable(WorkspaceModuleId::ambisonicEnergy, false);
    const auto reclaimResult = dormantReservation.moveModule(WorkspaceModuleId::outputMeter,
                                                              dormantSlot,
                                                              WorkspaceGridDirection::none);
    require((reclaimResult == WorkspaceGridMutationResult::committed
             || reclaimResult == WorkspaceGridMutationResult::noChange)
                && placementOf(dormantReservation, WorkspaceModuleId::outputMeter)
                       .overlaps(dormantSlot),
            "an active Mag may reclaim the remembered slot of a dormant lazy module");
    WorkspaceModuleLayout dormantReload;
    dormantReload.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    dormantReload.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    const auto dormantRestore = dormantReload.restoreUserState(dormantReservation.serialiseUserState());
    dormantReload.setAvailable(WorkspaceModuleId::outputMeter, true);
    dormantReload.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    require(dormantRestore.status == WorkspaceRestoreStatus::applied
                && !placementOf(dormantReload, WorkspaceModuleId::outputMeter)
                       .overlaps(placementOf(dormantReload, WorkspaceModuleId::ambisonicEnergy)),
            "persistence reserves a free dormant Mag slot without reopening the repair path");

    const auto opaquePreset = R"({"schema":"single-editor.workspace-preset","version":2,"presetId":"opaque-test","core":{"instanceId":"core-waveform","placement":{"column":0,"row":0,"span":{"columns":3,"rows":2}}},"modules":[{"instanceId":"output-meter","moduleId":"output-meter","enabled":true,"placed":true,"order":1,"weight":1000,"placement":{"column":3,"row":0,"span":{"columns":1,"rows":1}},"viewState":{"version":1,"payload":{}}},{"instanceId":"future-spectrum","moduleId":"vendor.future-spectrum","enabled":true,"placed":true,"placement":{"column":3,"row":1,"span":{"columns":1,"rows":1}},"futureField":{"token":"keep"}}],"stacks":[],"futureTop":{"revision":7}})";
    WorkspaceModuleLayout opaque;
    opaque.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    opaque.registerModule({ WorkspaceModuleId::ambisonicEnergy, 0, 780, 180 });
    require(opaque.restoreUserState(opaquePreset).status == WorkspaceRestoreStatus::applied
                && opaque.inertModules().size() == 1,
            "unknown current-schema modules restore only as inert placeholders");
    const auto opaqueRoundTrip = opaque.serialiseUserState();
    require(opaqueRoundTrip.find("future-spectrum") != std::string::npos
                && opaqueRoundTrip.find("futureField") != std::string::npos
                && opaqueRoundTrip.find("futureTop") != std::string::npos,
            "unknown module payload and top-level fields survive opaque round-trip");

    auto quarantinedPreset = std::string(opaquePreset);
    const auto payloadPosition = quarantinedPreset.find("\"payload\":{}");
    require(payloadPosition != std::string::npos, "quarantine fixture finds the known envelope");
    quarantinedPreset.replace(payloadPosition, std::string("\"payload\":{}").size(), "\"payload\":[]");
    WorkspaceModuleLayout quarantined;
    quarantined.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    const auto quarantineResult = quarantined.restoreUserState(quarantinedPreset);
    require(quarantineResult.status == WorkspaceRestoreStatus::applied
                && quarantineResult.quarantinedModuleCount == 1
                && quarantined.inertModules().size() == 2,
            "one invalid known envelope quarantines only its module while preserving other unknown state");
    WorkspaceModuleLayout quarantineRoundTrip;
    quarantineRoundTrip.registerModule({ WorkspaceModuleId::outputMeter, 1, 1000, 160 });
    require(quarantineRoundTrip.restoreUserState(quarantined.serialiseUserState()).status
                == WorkspaceRestoreStatus::applied,
            "quarantined known record round-trips once without emitting a duplicate live record");

    const auto stable = opaque.serialiseUserState();
    require(opaque.restoreUserState(R"({"schema":"single-editor.workspace-preset","version":99,"presetId":"future","modules":[],"stacks":[]})").status
                == WorkspaceRestoreStatus::futureSchema
                && opaque.serialiseUserState() == stable,
            "future schema fails closed without mutating or overwriting the active workspace");
    require(opaque.restoreUserState("{broken-json").status == WorkspaceRestoreStatus::corrupt
                && opaque.serialiseUserState() == stable,
            "corrupt preset fails transactionally and leaves the usable workspace unchanged");
    require(!opaque.setModuleViewState(WorkspaceModuleId::outputMeter,
                                       1,
                                       "{\"blob\":\"" + std::string(9000, 'x') + "\"}"),
            "module envelope byte budget rejects oversized state locally");
}

void runSidebarFloatingModelTest()
{
    WorkspaceModuleLayout layout;
    layout.registerModule({ WorkspaceModuleId::playlist, 1 });
    layout.registerModule({ WorkspaceModuleId::metadata, 2 });
    layout.setAvailable(WorkspaceModuleId::playlist, true);
    layout.setAvailable(WorkspaceModuleId::metadata, true);
    require(layout.dockModule(WorkspaceModuleId::metadata, WorkspaceModuleId::playlist,
                               WorkspaceDockZone::centre) == WorkspaceGridMutationResult::committed,
            "sidebar floating fixture starts with a shared tab stack");
    require(layout.floatModule(WorkspaceModuleId::metadata, {}) == WorkspaceGridMutationResult::committed
                && layout.isFloating(WorkspaceModuleId::playlist),
            "floating a sidebar tab moves its shared tile as one unit");
    const auto preset = layout.serialiseUserState();
    require(layout.redockModule(WorkspaceModuleId::playlist) == WorkspaceGridMutationResult::committed
                && layout.restoreUserState(preset).applied()
                && layout.isFloating(WorkspaceModuleId::metadata),
            "sidebar floating placement and active tab survive a preset roundtrip");
    require(layout.removeFloatingTile(WorkspaceModuleId::metadata) == WorkspaceGridMutationResult::committed
                && !layout.isActive(WorkspaceModuleId::metadata)
                && !layout.isActive(WorkspaceModuleId::playlist)
                && layout.undoGridMutation()
                && layout.isFloating(WorkspaceModuleId::metadata),
            "closing and undoing a floating sidebar stack preserves the shared layout identity");
}

void runTallWorkspaceRestoreTest()
{
    // Preserved from a real workspace that the split layout saved successfully,
    // but the next launch rejected because live modules exceeded eight rows.
    const std::string tallPreset = R"({"schema":"single-editor.workspace-preset","version":2,"presetId":"migrated-v1",
"core":{"instanceId":"core-waveform","placement":{"column":1,"row":0,"span":{"columns":3,"rows":21}}},
"modules":[
{"instanceId":"ambisonic-energy","moduleId":"ambisonic-energy","enabled":true,"placed":true,"order":0,"weight":780,"placement":{"column":3,"row":21,"span":{"columns":1,"rows":1}},"viewState":{"version":1,"payload":{}}},
{"instanceId":"output-meter","moduleId":"output-meter","enabled":true,"placed":true,"order":1,"weight":1000,"placement":{"column":4,"row":1,"span":{"columns":4,"rows":20}},"viewState":{"version":1,"payload":{}}},
{"instanceId":"parametric-eq","moduleId":"parametric-eq","enabled":false,"placed":false,"order":2,"weight":920,"placement":{"column":2,"row":2,"span":{"columns":2,"rows":1}},"viewState":{"version":1,"payload":{}}},
{"instanceId":"playlist","moduleId":"playlist","enabled":true,"placed":true,"order":3,"weight":1000,"placement":{"column":0,"row":0,"span":{"columns":1,"rows":21}},"viewState":{"version":1,"payload":{}}},
{"instanceId":"true-peak-limiter","moduleId":"true-peak-limiter","enabled":false,"placed":false,"order":3,"weight":760,"placement":{"column":2,"row":2,"span":{"columns":2,"rows":1}},"viewState":{"version":1,"payload":{}}},
{"instanceId":"metadata","moduleId":"metadata","enabled":true,"placed":true,"order":4,"weight":1000,"placement":{"column":4,"row":0,"span":{"columns":4,"rows":1}},"viewState":{"version":1,"payload":{}}}],"stacks":[],"floating":[]})";
    const auto makeRegisteredLayout = []
    {
        auto layout = makeLayout();
        layout.registerModule({ WorkspaceModuleId::playlist, 3, 1000, 120 });
        layout.registerModule({ WorkspaceModuleId::metadata, 4, 1000, 120 });
        layout.registerModule({ WorkspaceModuleId::parametricEq, 2, 920, 180 });
        layout.registerModule({ WorkspaceModuleId::truePeakLimiter, 3, 760, 180 });
        layout.setAvailable(WorkspaceModuleId::playlist, true);
        layout.setAvailable(WorkspaceModuleId::metadata, true);
        return layout;
    };
    auto layout = makeRegisteredLayout();
    require(layout.restoreUserState(tallPreset).status == WorkspaceRestoreStatus::applied,
            "a saved tall split workspace restores without falsely reporting corruption");
    require(placementOf(layout, WorkspaceModuleId::outputMeter) == WorkspaceGridPlacement { 4, 1, { 4, 20 } }
                && placementOf(layout, WorkspaceModuleId::playlist) == WorkspaceGridPlacement { 0, 0, { 1, 21 } }
                && placementOf(layout, WorkspaceModuleId::coreWaveform) == WorkspaceGridPlacement { 1, 0, { 3, 21 } }
                && !layout.isEnabled(WorkspaceModuleId::parametricEq)
                && !layout.isEnabled(WorkspaceModuleId::truePeakLimiter),
            "tall workspace recovery preserves geometry and explicitly hidden DSP modules");
    requireNoOverlap(layout);
    const auto saved = layout.serialiseUserState();
    auto restarted = makeRegisteredLayout();
    require(restarted.restoreUserState(saved).status == WorkspaceRestoreStatus::applied,
            "recovered tall workspace saves and restarts as an ordinary V2 preset");
    requireSameGrid(layout, restarted);
    require(restarted.serialiseUserState() == saved,
            "tall workspace persistence is stable across restart");
    require(layout.resizeModule(WorkspaceModuleId::outputMeter, { 4, 22 }, WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "a recovered tall workspace remains editable");
    requireNoOverlap(layout);
    auto resizedRestart = makeRegisteredLayout();
    require(resizedRestart.restoreUserState(layout.serialiseUserState()).status == WorkspaceRestoreStatus::applied,
            "a newly resized tall workspace also survives restart");
    requireSameGrid(layout, resizedRestart);
    auto oversized = tallPreset;
    oversized.replace(oversized.find("\"rows\":20"), std::string("\"rows\":20").size(), "\"rows\":257");
    const auto beforeInvalid = restarted.serialiseUserState();
    require(restarted.restoreUserState(oversized).status == WorkspaceRestoreStatus::corrupt
                && restarted.serialiseUserState() == beforeInvalid,
            "unsupported tall spans still fail transactionally without replacing a valid workspace");
}

void runContentAwarePresetCatalogTest()
{
    auto generalLayout = makeLayout();
    require(generalLayout.setModulePlaced(WorkspaceModuleId::ambisonicEnergy, false)
                == WorkspaceGridMutationResult::committed,
            "catalog fixture creates a distinct general workspace");
    auto spatialLayout = makeLayout();
    require(spatialLayout.dockModule(WorkspaceModuleId::outputMeter,
                                     WorkspaceModuleId::ambisonicEnergy,
                                     WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "catalog fixture creates a distinct spatial workspace");

    WorkspacePresetCatalog catalog;
    require(catalog.savePreset("general-clean", "General Clean", generalLayout.serialiseUserState())
                && catalog.savePreset("spatial-tabs", "Spatial Tabs", spatialLayout.serialiseUserState())
                && catalog.bindCategory(WorkspaceContentCategory::general, "general-clean")
                && catalog.bindCategory(WorkspaceContentCategory::ambisonics, "spatial-tabs"),
            "multiple explicit structural presets bind independently to stable content categories");
    require(WorkspacePresetCatalog::classify(false, false, false, 2)
                == WorkspaceContentCategory::general
                && WorkspacePresetCatalog::classify(false, false, false, 6)
                       == WorkspaceContentCategory::discreteMultichannel
                && WorkspacePresetCatalog::classify(false, false, true, 4)
                       == WorkspaceContentCategory::ambisonics
                && WorkspacePresetCatalog::classify(false, true, false, 2)
                       == WorkspaceContentCategory::videoAudio
                && WorkspacePresetCatalog::classify(true, false, false, 2)
                       == WorkspaceContentCategory::bnk,
            "content category uses already-known document semantics with deterministic precedence");

    const auto savedCatalog = catalog.serialise();
    WorkspacePresetCatalog restored;
    require(restored.restore(savedCatalog) == WorkspacePresetCatalogRestoreStatus::applied
                && restored.serialise() == savedCatalog
                && restored.resolve(WorkspaceContentCategory::general)->id == "general-clean"
                && restored.resolve(WorkspaceContentCategory::ambisonics)->id == "spatial-tabs",
            "catalog save-load-save and category bindings round-trip stably across restart");
    auto catalogWithUnknownPresetField = savedCatalog;
    const auto knownField = catalogWithUnknownPresetField.find("\"name\": \"General Clean\"");
    require(knownField != std::string::npos,
            "catalog fixture locates a stable preset record insertion point");
    catalogWithUnknownPresetField.insert(knownField + std::string("\"name\": \"General Clean\"").size(),
                                         ", \"futurePresetField\": { \"token\": \"keep\" }");
    WorkspacePresetCatalog unknownFieldCatalog;
    require(unknownFieldCatalog.restore(catalogWithUnknownPresetField)
                == WorkspacePresetCatalogRestoreStatus::applied,
            "catalog with an additive preset-record field remains readable");
    const auto unknownFieldRoundTrip = unknownFieldCatalog.serialise();
    WorkspacePresetCatalog unknownFieldReloaded;
    require(unknownFieldRoundTrip.find("futurePresetField") != std::string::npos
                && unknownFieldRoundTrip.find("keep") != std::string::npos
                && unknownFieldReloaded.restore(unknownFieldRoundTrip)
                       == WorkspacePresetCatalogRestoreStatus::applied
                && unknownFieldReloaded.serialise() == unknownFieldRoundTrip,
            "unknown preset-record fields survive semantic opaque round-trip");
    const auto stable = restored.serialise();
    require(restored.restore("{broken-catalog") == WorkspacePresetCatalogRestoreStatus::corrupt
                && restored.serialise() == stable,
            "corrupt catalog fails transactionally without replacing usable presets");
    require(restored.restore(R"({"schema":"single-editor.workspace-preset-catalog","version":9,"presets":[],"bindings":{}})")
                == WorkspacePresetCatalogRestoreStatus::futureSchema
                && restored.serialise() == stable,
            "future catalog remains read-only and cannot be silently downgraded");
}
}

int main()
{
    runPrivilegedCoreAndDefaultPackingTest();
    runDeterministicMoveUndoAndRedoTest();
    runResizeRemoveAndDockBoundaryTest();
    runAtomicOpenCloseStateTest();
    runDockCloseReopenGeometryTest();
    runRepeatedSequenceDeterminismTest();
    runFiveZoneCoreDockingTest();
    runTabStackIdentityAndTileMutationTest();
    runVersionedPresetMigrationAndOpaquePreservationTest();
    runTallWorkspaceRestoreTest();
    runContentAwarePresetCatalogTest();
    runSidebarFloatingModelTest();
    std::cout << "WorkspaceGridTests passed\n";
    return 0;
}
