#include "UI/Workspace/WorkspaceModuleHostComponent.h"
#include "UI/Workspace/WorkspaceModuleRuntimePolicy.h"
#include "UI/Workspace/WorkspaceCanvasAuthority.h"
#include "UI/Workspace/ModernWorkspaceScene.h"
#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "WorkspaceModuleHostTests failed: " << message << '\n';
    std::exit(1);
}

class CountedComponent final : public juce::Component
{
public:
    explicit CountedComponent(int& destructionCountToUse)
        : destructionCount(destructionCountToUse)
    {
    }

    ~CountedComponent() override
    {
        ++destructionCount;
    }

private:
    int& destructionCount;
};

WorkspaceModuleDescriptor descriptor(WorkspaceModuleId id, int order, int row)
{
    return { id,
             order,
             1000,
             120,
             { 1, 1 },
             { 1, 1 },
             { 2, 3 },
             -1,
             row };
}

void requireGapFreeMosaic(const singleeditor::WorkspaceModuleHostComponent& host,
                          std::vector<WorkspaceModuleId> modules,
                          const std::string& message)
{
    std::vector<juce::Rectangle<int>> tiles { host.getCoreBounds() };
    for (const auto id : modules)
    {
        const auto bounds = host.getModuleTileBounds(id);
        if (bounds.isEmpty()
            || std::find(tiles.begin(), tiles.end(), bounds) != tiles.end())
        {
            continue;
        }
        tiles.push_back(bounds);
    }
    for (size_t left = 0; left < tiles.size(); ++left)
        for (size_t right = left + 1; right < tiles.size(); ++right)
            require(!tiles[left].intersects(tiles[right]), message + " (tiles overlap)");
    long long covered = 0;
    for (const auto bounds : tiles)
        covered += static_cast<long long>(bounds.getWidth()) * bounds.getHeight();
    const auto total = static_cast<long long>(host.getWidth()) * host.getHeight();
    // Intentional 10 px shared split gutters are not unassigned canvas.  A
    // dense multi-column layout can legitimately spend just over three per
    // cent of the host on those hit targets while every grid cell is owned.
    if (!(total > 0 && covered * 100 >= total * 95))
    {
        std::cerr << "Mosaic coverage diagnostic: covered=" << covered
                  << " total=" << total << '\n';
        for (const auto bounds : tiles)
            std::cerr << "  tile " << bounds.toString() << '\n';
    }
    require(total > 0 && covered * 100 >= total * 95, message + " (large blank area remains)");
}

juce::Component* findDescendantWithId(juce::Component& parent, const juce::String& id)
{
    for (auto* child : parent.getChildren())
    {
        if (child->getComponentID() == id)
            return child;
        if (auto* nested = findDescendantWithId(*child, id))
            return nested;
    }
    return nullptr;
}

void runBorrowedViewLifecycleTest()
{
    juce::Component owner;
    int destructions = 0;
    CountedComponent playlist(destructions);
    owner.addChildComponent(playlist);
    {
        singleeditor::WorkspaceModuleHostComponent host;
        host.setBounds(0, 0, 1000, 600);
        host.registerBorrowedModule(descriptor(WorkspaceModuleId::playlist, 1, 0), 218, playlist);
        require(playlist.getParentComponent() == &owner && !playlist.isVisible()
                    && host.getModuleComponent(WorkspaceModuleId::playlist) == nullptr,
                "registering a borrowed sidebar does not reveal or reparent it before availability");
        host.setModuleAvailable(WorkspaceModuleId::playlist, true);
        require(host.getModuleComponent(WorkspaceModuleId::playlist) == &playlist
                    && playlist.getParentComponent() == &host && playlist.isVisible(),
                "available Playlist uses the exact existing view as a Mag, without a wrapper or duplicate");
        require(!playlist.getBounds().intersects(host.getCoreBounds()),
                "borrowed Playlist and waveform have disjoint layout bounds");
        host.setModuleEnabled(WorkspaceModuleId::playlist, false);
        require(destructions == 0 && !playlist.isVisible()
                    && host.getModuleComponent(WorkspaceModuleId::playlist) == nullptr,
                "removing a borrowed Mag hides but does not delete its owner's view/model");
        require(host.undoUserLayout()
                    && host.getModuleComponent(WorkspaceModuleId::playlist) == &playlist,
                "workspace undo restores the same borrowed view and its existing state");
    }
    require(destructions == 0 && playlist.getParentComponent() == &owner && !playlist.isVisible(),
            "destroying the workspace restores borrowed parentage hidden and never deletes the sidebar");
}

void runBorrowedViewRestoreTest()
{
    singleeditor::WorkspaceModuleHostComponent oldHost;
    oldHost.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                           [] { return std::make_unique<juce::Component>(); });
    const auto oldPreset = oldHost.serialiseUserState();
    juce::Component owner;
    juce::Component playlist;
    juce::Component metadata;
    owner.addChildComponent(playlist);
    owner.addChildComponent(metadata);
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::playlist, 1, 0), 218, playlist);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::metadata, 2, 1), 232, metadata);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    require(host.restoreUserState(oldPreset).applied()
                && host.isModuleVisible(WorkspaceModuleId::playlist)
                && host.isModuleVisible(WorkspaceModuleId::metadata),
            "pre-sidebar presets migrate missing borrowed modules using their legacy visibility defaults");
    require(host.dockModule(WorkspaceModuleId::metadata, WorkspaceModuleId::playlist,
                            WorkspaceDockZone::centre) == WorkspaceGridMutationResult::committed,
            "Playlist and Metadata join the same real Tab Stack");
    const auto stackedPreset = host.serialiseUserState();
    require(host.activateTab(WorkspaceModuleId::playlist) == WorkspaceGridMutationResult::committed
                && host.restoreUserState(stackedPreset).applied()
                && host.getModuleComponent(WorkspaceModuleId::metadata) == &metadata
                && metadata.isVisible() && !playlist.isVisible(),
            "preset restore preserves active sidebar tab and reuses its exact view");
    host.setModuleEnabled(WorkspaceModuleId::metadata, false);
    const auto hiddenPreset = host.serialiseUserState();
    host.setModuleEnabled(WorkspaceModuleId::metadata, true);
    require(host.restoreUserState(hiddenPreset).applied()
                && !host.isModuleEnabled(WorkspaceModuleId::metadata),
            "an explicit saved hidden sidebar wins over the legacy default");
    host.setModuleEnabled(WorkspaceModuleId::playlist, false);
    require(host.restoreUserState("v1").status == WorkspaceRestoreStatus::migratedLegacyV1
                && host.isModuleVisible(WorkspaceModuleId::playlist)
                && host.isModuleVisible(WorkspaceModuleId::metadata)
                && !playlist.getBounds().intersects(metadata.getBounds()),
            "v1 migration uses sidebar defaults, not the previous tab/hidden state, and packs them apart");
}

void runBorrowedRestoreSafetyTest()
{
    juce::Component playlist;
    juce::Component metadata;
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::playlist, 1, 0), 218, playlist, false);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::metadata, 2, 1), 232, metadata);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    singleeditor::WorkspaceModuleHostComponent oldHost;
    require(host.restoreUserState(oldHost.serialiseUserState()).applied()
                && !host.isModuleEnabled(WorkspaceModuleId::playlist)
                && host.isModuleVisible(WorkspaceModuleId::metadata),
            "a formerly collapsed sidebar stays hidden when migrating an older preset");

    auto mismatched = juce::JSON::parse(host.serialiseUserState());
    for (auto& record : *mismatched.getDynamicObject()->getProperty("modules").getArray())
        if (record.getProperty("instanceId", {}).toString() == "metadata")
            record.getDynamicObject()->setProperty("moduleId", "external-metadata-v9");
    const auto restored = host.restoreUserState(juce::JSON::toString(mismatched));
    require(restored.applied() && restored.quarantinedModuleCount == 1
                && host.getModuleComponent(WorkspaceModuleId::metadata) == nullptr,
            "a quarantined sidebar record is never mistaken for an absent legacy default");
    const auto before = host.serialiseUserState();
    require(host.restoreUserState("{broken").status == WorkspaceRestoreStatus::corrupt
                && host.serialiseUserState() == before,
            "a corrupt preset is rejected without resetting or rewriting the current workspace");
}

void runGapFreeAdaptiveMosaicRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180, []
    {
        auto component = std::make_unique<juce::Component>();
        component->setInterceptsMouseClicks(false, false);
        return component;
    });
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 1, 1), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);

    for (const auto size : { juce::Point<int> { 800, 520 },
                             juce::Point<int> { 1000, 600 },
                             juce::Point<int> { 1600, 900 } })
    {
        host.setBounds(0, 0, size.x, size.y);
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::outputMeter,
                               WorkspaceModuleId::playlist,
                               WorkspaceModuleId::metadata },
                             "three right-edge Mags must leave no unowned lower canvas at any supported size");
        require(host.getCoreBounds().getHeight() == host.getHeight(),
                "right-edge Mags never shorten the privileged waveform");
    }

    host.setModuleEnabled(WorkspaceModuleId::playlist, false);
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter, WorkspaceModuleId::metadata },
                         "hiding Playlist immediately reflows the remaining mosaic");
    const auto meterWithPlaylistHidden = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    const auto metadataWithPlaylistHidden = host.getModuleTileBounds(WorkspaceModuleId::metadata);
    require(meterWithPlaylistHidden.getHeight() + metadataWithPlaylistHidden.getHeight()
                >= host.getHeight() - 3 * singleeditor::RendererVisualStyle::gapForDensity(
                    singleeditor::RendererDensity::standard),
            "the two surviving right-edge Mags consume the released Playlist space");

    host.setModuleEnabled(WorkspaceModuleId::playlist, true);
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata },
                         "showing Playlist restores a filled deterministic mosaic");
}

void runLateWideMagGapFillRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1166, 583);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 154,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 1, 1), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });

    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 3, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.defaultGridColumn = 2;
    host.registerModule(eqDescriptor, 360,
                        [] { return std::make_unique<juce::Component>(); });
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, false);

    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata },
                         "the initial right rail fills the canvas before a DSP Mag is opened");

    host.setModuleAvailable(WorkspaceModuleId::parametricEq, true);
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, true);
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq },
                         "opening a wider EQ after the rail exists cannot leave a blank lower-left island");
    const auto eq = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    require(!eq.isEmpty() && !eq.intersects(host.getCoreBounds()),
            "the late EQ remains a real non-overlapping tile after the blank is absorbed");
}

void runDockedTabStripInteractionRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 1, 1), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    require(host.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::metadata,
                            WorkspaceDockZone::centre) == WorkspaceGridMutationResult::committed,
            "Metadata and Output form a docked Tab Stack");

    auto* outputTab = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(host, "workspace.docked.tab.output-meter"));
    auto* metadataTab = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(host, "workspace.docked.tab.metadata"));
    require(outputTab != nullptr && metadataTab != nullptr
                && outputTab->getWantsKeyboardFocus() && metadataTab->getWantsKeyboardFocus(),
            "a docked stack exposes real focusable tab buttons instead of painted labels");

    auto* output = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    const auto outputTabBounds = host.getLocalArea(outputTab, outputTab->getLocalBounds());
    require(output != nullptr && output->isVisible()
                && output->getBounds().getY() >= outputTabBounds.getBottom(),
            "the real tab strip owns layout space and never paints over Output content");

    metadataTab->onClick();
    auto* metadata = host.getModuleComponent(WorkspaceModuleId::metadata);
    const auto metadataTabBounds = host.getLocalArea(metadataTab, metadataTab->getLocalBounds());
    require(metadata != nullptr && metadata->isVisible() && !output->isVisible()
                && metadata->getBounds().getY() >= metadataTabBounds.getBottom(),
            "clicking the Metadata tab switches the exact Mag and keeps content below the strip");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::playlist, WorkspaceModuleId::metadata },
                         "a Tab Stack counts as one tile and the remaining mosaic stays filled");
}

void runFourEdgeMosaicProjectionRegressionTest()
{
    for (const auto zone : { WorkspaceDockZone::left,
                             WorkspaceDockZone::top,
                             WorkspaceDockZone::right,
                             WorkspaceDockZone::bottom })
    {
        singleeditor::WorkspaceModuleHostComponent host;
        host.setBounds(0, 0, 1000, 600);
        host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                            [] { return std::make_unique<juce::Component>(); });
        host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
        const auto result = host.dockModule(WorkspaceModuleId::outputMeter,
                                            WorkspaceModuleId::coreWaveform,
                                            zone);
        require(result == WorkspaceGridMutationResult::committed
                    || (zone == WorkspaceDockZone::right
                        && result == WorkspaceGridMutationResult::noChange),
                "all four waveform edges accept the same Mag docking transaction");
        const auto core = host.getCoreBounds();
        const auto tile = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
        const auto onRequestedEdge = zone == WorkspaceDockZone::left ? tile.getRight() <= core.getX()
            : zone == WorkspaceDockZone::top ? tile.getBottom() <= core.getY()
            : zone == WorkspaceDockZone::right ? tile.getX() >= core.getRight()
            : tile.getY() >= core.getBottom();
        require(onRequestedEdge && !tile.intersects(core),
                "the adaptive projection preserves the requested physical edge without overlap");
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::outputMeter },
                             "each single-edge mosaic assigns all usable canvas to waveform or Mag");
    }
}

void runLazyProjectionAndReleaseTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);

    int meterFactoryCalls = 0;
    int energyFactoryCalls = 0;
    int meterDestructions = 0;
    int energyDestructions = 0;
    int committedLayouts = 0;
    host.onUserLayoutCommitted = [&committedLayouts] { ++committedLayouts; };

    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                        180,
                        [&]
                        {
                            ++meterFactoryCalls;
                            return std::make_unique<CountedComponent>(meterDestructions);
                        });
    host.registerModule(descriptor(WorkspaceModuleId::ambisonicEnergy, 0, 1),
                        280,
                        [&]
                        {
                            ++energyFactoryCalls;
                            return std::make_unique<CountedComponent>(energyDestructions);
                        });

    require(meterFactoryCalls == 0 && energyFactoryCalls == 0,
            "registering grid chrome never invokes an optional module factory");
    require(host.getCoreBounds().getX() == 0
                && host.getCoreBounds().getY() == 0
                && host.getCoreBounds().getWidth() > 700
                && host.getCoreBounds().getHeight() == 600,
            "the default four-column projection reserves a privileged three-column waveform");

    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    auto* meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    require(meterFactoryCalls == 1 && meter != nullptr,
            "an owning availability trigger constructs the requested View once");
    const auto focusIdentity = host.currentFocusIdentity();
    require(meter->getWantsKeyboardFocus()
                && meter->getComponentID() == "workspace.module.output-meter"
                && focusIdentity.has_value()
                && !focusIdentity->placementId.empty()
                && focusIdentity->tabStackId.empty()
                && focusIdentity->tabInstanceId == "output-meter",
            "visible module Views expose stable focus and placement identity without a second View");
    require(meter->getBounds().getHeight() == host.getHeight()
                && meter->getBounds().getX() > host.getCoreBounds().getRight(),
            "a single untouched Mag fills the avoidable side-column hole");

    host.setModuleAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    auto* energy = host.getModuleComponent(WorkspaceModuleId::ambisonicEnergy);
    meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    require(energyFactoryCalls == 1 && energy != nullptr && meter != nullptr,
            "a second explicit trigger adds only its own View");
    require(energy->getBounds().getBottom() < meter->getBounds().getBottom()
                && !energy->getBounds().intersects(meter->getBounds()),
            "two default Mags deterministically pack into separate right-column cells");

    require(host.moveModule(WorkspaceModuleId::outputMeter,
                            { 3, 0, { 1, 1 } },
                            WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "host move command projects the renderer-neutral solver result");
    require(host.getModuleComponent(WorkspaceModuleId::outputMeter)->getY()
                < host.getModuleComponent(WorkspaceModuleId::ambisonicEnergy)->getY(),
            "drop target priority is visible in JUCE component bounds");
    require(committedLayouts == 1, "one drop emits one persistence boundary");

    require(host.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::protectedCore
                && committedLayouts == 1,
            "centre docking cannot hide the privileged waveform inside a Tab Stack");

    host.setModuleEnabled(WorkspaceModuleId::ambisonicEnergy, false);
    require(host.getModuleComponent(WorkspaceModuleId::ambisonicEnergy) == nullptr
                && energyDestructions == 1,
            "Remove destroys the module View immediately");
    require(host.canUndoUserLayout() && host.undoUserLayout(),
            "the host exposes workspace-level undo for Remove");
    require(host.getModuleComponent(WorkspaceModuleId::ambisonicEnergy) != nullptr
                && energyFactoryCalls == 2,
            "undo recreates only the explicitly restored View");

    host.setModuleAvailable(WorkspaceModuleId::outputMeter, false);
    require(host.getModuleComponent(WorkspaceModuleId::outputMeter) == nullptr
                && meterDestructions == 1,
            "loss of runtime availability releases the corresponding View completely");
}

void runWaveformLayoutMutationRegressionTest()
{
    juce::Component owner;
    juce::Component waveform;
    singleeditor::WorkspaceModuleHostComponent host;
    owner.setBounds(0, 0, 1200, 760);
    owner.addAndMakeVisible(waveform);
    owner.addAndMakeVisible(host);
    host.setBounds(40, 80, 1000, 600);
    host.onLayoutChanged = [&]
    {
        waveform.setBounds(host.getCoreBounds().translated(host.getX(), host.getY()));
    };
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                        180, [] { return std::make_unique<juce::Component>(); });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    const auto initialWaveform = waveform.getBounds();

    require(host.moveModule(WorkspaceModuleId::outputMeter,
                            { 2, 2, { 1, 1 } }, WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "a tile can move below the waveform into a newly required grid row");
    const auto* meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    const auto meterInOwner = meter->getBounds().translated(host.getX(), host.getY());
    require(waveform.getBounds() != initialWaveform
                && !waveform.getBounds().intersects(meterInOwner)
                && waveform.getBounds() == host.getCoreBounds().translated(host.getX(), host.getY()),
            "moving a Mag must resize the actual sibling waveform before the mutation returns");
    require(host.resizeModule(WorkspaceModuleId::outputMeter,
                              { 1, 3 }, WorkspaceGridDirection::down)
                == WorkspaceGridMutationResult::committed,
            "resizing a Mag can extend the shared workspace grid");
    require(!waveform.getBounds().intersects(meter->getBounds().translated(host.getX(), host.getY()))
                && waveform.getBounds() == host.getCoreBounds().translated(host.getX(), host.getY()),
            "resizing a Mag must keep the actual waveform disjoint in the same transaction");
    require(host.undoUserLayout() && host.redoUserLayout()
                && !waveform.getBounds().intersects(meter->getBounds().translated(host.getX(), host.getY())),
            "workspace undo and redo preserve the visible waveform boundary");
    host.setVisualDensity(singleeditor::RendererDensity::compact);
    require(waveform.getBounds() == host.getCoreBounds().translated(host.getX(), host.getY()),
            "changing workspace density must update the actual waveform and tile gutters together");
}

juce::MouseEvent pointerEvent(juce::Component& target, juce::Point<float> point,
                              juce::Point<float> down, int modifiers, bool dragged)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), point,
             juce::ModifierKeys(modifiers), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &target, &target, juce::Time(100), down, juce::Time(0), 1, dragged };
}

void runPerMagSizeGeometryRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1200, 800);
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 2, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.maximumSpan = { 4, 3 };
    host.registerModule(eqDescriptor, 360, [] { return std::make_unique<juce::Component>(); });
    host.setModuleAvailable(WorkspaceModuleId::parametricEq, true);
    require(host.dockModule(WorkspaceModuleId::parametricEq,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::bottom) == WorkspaceGridMutationResult::committed,
            "EQ can be placed below the waveform before testing its visual sizes");

    host.setModuleVisualSize(WorkspaceModuleId::parametricEq, singleeditor::RendererMagSize::small);
    const auto small = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    const auto smallCore = host.getCoreBounds();
    host.setModuleVisualSize(WorkspaceModuleId::parametricEq, singleeditor::RendererMagSize::medium);
    const auto medium = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    const auto mediumCore = host.getCoreBounds();
    host.setModuleVisualSize(WorkspaceModuleId::parametricEq, singleeditor::RendererMagSize::large);
    const auto large = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    const auto largeCore = host.getCoreBounds();

    require(small.getHeight() < medium.getHeight() && medium.getHeight() < large.getHeight(),
            "EQ Compact, Standard, and Expanded must have three visibly distinct tile heights");
    require(smallCore.getHeight() > mediumCore.getHeight()
                && mediumCore.getHeight() > largeCore.getHeight(),
            "the privileged waveform yields layout space when an individual Mag grows");
    require(!small.intersects(smallCore)
                && !medium.intersects(mediumCore)
                && !large.intersects(largeCore),
            "every EQ visual size remains disjoint from the waveform instead of overlaying it");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::parametricEq },
                         "changing one Mag's size preserves a filled adaptive canvas");
}

void runPointerFourEdgePreviewRegressionTest()
{
    for (const auto zone : { WorkspaceDockZone::left,
                             WorkspaceDockZone::top,
                             WorkspaceDockZone::right,
                             WorkspaceDockZone::bottom })
    {
        singleeditor::WorkspaceModuleHostComponent host;
        host.setBounds(0, 0, 1200, 760);
        host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                            [] { return std::make_unique<juce::Component>(); });
        host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
        if (zone == WorkspaceDockZone::right)
            require(host.dockModule(WorkspaceModuleId::outputMeter,
                                    WorkspaceModuleId::coreWaveform,
                                    WorkspaceDockZone::bottom) == WorkspaceGridMutationResult::committed,
                    "the right-edge pointer test starts from a different physical edge");

        auto& tile = *host.getModuleComponent(WorkspaceModuleId::outputMeter);
        const auto core = host.getCoreBounds();
        const auto hostDrop = zone == WorkspaceDockZone::left
            ? juce::Point<int> { core.getX() + 2, core.getCentreY() }
            : zone == WorkspaceDockZone::top
                ? juce::Point<int> { core.getCentreX(), core.getY() + 2 }
                : zone == WorkspaceDockZone::right
                    ? juce::Point<int> { core.getRight() - 2, core.getCentreY() }
                    : juce::Point<int> { core.getCentreX(), core.getBottom() - 2 };
        const juce::Point<float> down { 12.0f, 12.0f };
        const auto drop = tile.getLocalPoint(&host, hostDrop.toFloat());
        auto& input = static_cast<juce::Component&>(host);
        input.mouseDown(pointerEvent(tile, down, down,
                                     juce::ModifierKeys::leftButtonModifier, false));
        input.mouseDrag(pointerEvent(tile, drop, down,
                                     juce::ModifierKeys::leftButtonModifier, true));
        const auto preview = host.getDragPreviewBoundsForTesting();
        const auto previewLabel = host.getDragPreviewLabelForTesting();
        require(preview.has_value() && !preview->isEmpty() && previewLabel.isNotEmpty(),
                "each waveform edge exposes a concrete readable drop proposal");
        input.mouseUp(pointerEvent(tile, drop, down,
                                   juce::ModifierKeys::leftButtonModifier, true));

        const auto committed = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
        const auto committedCore = host.getCoreBounds();
        require(committed == *preview,
                "the drop preview must equal the exact bounds committed on pointer release");
        const auto onRequestedEdge = zone == WorkspaceDockZone::left
            ? committed.getRight() <= committedCore.getX()
            : zone == WorkspaceDockZone::top
                ? committed.getBottom() <= committedCore.getY()
                : zone == WorkspaceDockZone::right
                    ? committed.getX() >= committedCore.getRight()
                    : committed.getY() >= committedCore.getBottom();
        require(onRequestedEdge && !committed.intersects(committedCore),
                "pointer docking lands on the selected physical waveform edge without overlap");
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::outputMeter },
                             "pointer docking to any edge preserves the gap-free mosaic");
    }
}

void runPointerFourZoneAroundMagRegressionTest()
{
    for (const auto zone : { WorkspaceDockZone::left,
                             WorkspaceDockZone::top,
                             WorkspaceDockZone::right,
                             WorkspaceDockZone::bottom })
    {
        singleeditor::WorkspaceModuleHostComponent host;
        host.setBounds(0, 0, 1300, 760);
        host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                            [] { return std::make_unique<juce::Component>(); });
        host.registerModule(descriptor(WorkspaceModuleId::metadata, 1, 1), 232,
                            [] { return std::make_unique<juce::Component>(); });
        host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
        host.setModuleAvailable(WorkspaceModuleId::metadata, true);
        const auto outputStart = host.dockModule(WorkspaceModuleId::outputMeter,
                                                 WorkspaceModuleId::coreWaveform,
                                                 WorkspaceDockZone::right);
        require(outputStart == WorkspaceGridMutationResult::committed
                    || outputStart == WorkspaceGridMutationResult::noChange,
                "the Mag-target pointer test starts with Output right of the waveform");
        require(host.dockModule(WorkspaceModuleId::metadata,
                                WorkspaceModuleId::coreWaveform,
                                WorkspaceDockZone::bottom) == WorkspaceGridMutationResult::committed,
                "the moving Metadata Mag starts away from the target");

        auto& moving = *host.getModuleComponent(WorkspaceModuleId::metadata);
        const auto targetBefore = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
        const auto hostDrop = zone == WorkspaceDockZone::left
            ? juce::Point<int> { targetBefore.getX() + 2, targetBefore.getCentreY() }
            : zone == WorkspaceDockZone::top
                ? juce::Point<int> { targetBefore.getCentreX(), targetBefore.getY() + 2 }
                : zone == WorkspaceDockZone::right
                    ? juce::Point<int> { targetBefore.getRight() - 2, targetBefore.getCentreY() }
                    : juce::Point<int> { targetBefore.getCentreX(), targetBefore.getBottom() - 2 };
        const juce::Point<float> down { 12.0f, 12.0f };
        const auto drop = moving.getLocalPoint(&host, hostDrop.toFloat());
        auto& input = static_cast<juce::Component&>(host);
        input.mouseDown(pointerEvent(moving, down, down,
                                     juce::ModifierKeys::leftButtonModifier, false));
        input.mouseDrag(pointerEvent(moving, drop, down,
                                     juce::ModifierKeys::leftButtonModifier, true));
        const auto preview = host.getDragPreviewBoundsForTesting();
        const auto previewLabel = host.getDragPreviewLabelForTesting();
        require(preview.has_value() && !preview->isEmpty()
                    && previewLabel.contains("Output Meter"),
                "every side of a non-core Mag exposes a concrete target-named preview");
        input.mouseUp(pointerEvent(moving, drop, down,
                                   juce::ModifierKeys::leftButtonModifier, true));

        const auto moved = host.getModuleTileBounds(WorkspaceModuleId::metadata);
        const auto target = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
        const auto onRequestedSide = zone == WorkspaceDockZone::left
            ? moved.getRight() <= target.getX()
            : zone == WorkspaceDockZone::top
                ? moved.getBottom() <= target.getY()
                : zone == WorkspaceDockZone::right
                    ? moved.getX() >= target.getRight()
                    : moved.getY() >= target.getBottom();
        require(moved == *preview && onRequestedSide && !moved.intersects(target),
                "a non-core Mag commits to the exact left/right/top/bottom preview");
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::outputMeter, WorkspaceModuleId::metadata },
                             "four-way Mag-to-Mag docking keeps the entire mosaic filled");
    }
}

void runTabTearOffRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1200, 760);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 1, 1), 232,
                        [] { return std::make_unique<juce::Component>(); });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    require(host.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::metadata,
                            WorkspaceDockZone::centre) == WorkspaceGridMutationResult::committed,
            "Output and Metadata form a real Tab Stack before tear-off");

    auto* outputTab = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(host, "workspace.docked.tab.output-meter"));
    require(outputTab != nullptr, "the Output tab exists as the tear-off pointer source");
    const auto core = host.getCoreBounds();
    const auto hostDrop = juce::Point<int> { core.getX() + 2, core.getCentreY() };
    const auto down = outputTab->getLocalBounds().getCentre().toFloat();
    const auto drop = outputTab->getLocalPoint(&host, hostDrop.toFloat());
    auto& input = static_cast<juce::Component&>(host);
    input.mouseDown(pointerEvent(*outputTab, down, down,
                                 juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(*outputTab, drop, down,
                                 juce::ModifierKeys::leftButtonModifier, true));
    const auto preview = host.getDragPreviewBoundsForTesting();
    require(preview.has_value() && host.getDragPreviewLabelForTesting().isNotEmpty(),
            "dragging a tab exposes the same exact dock preview as dragging a standalone Mag");
    input.mouseUp(pointerEvent(*outputTab, drop, down,
                               juce::ModifierKeys::leftButtonModifier, true));

    const auto outputBounds = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    const auto metadataBounds = host.getModuleTileBounds(WorkspaceModuleId::metadata);
    require(outputBounds == *preview
                && outputBounds.getRight() <= host.getCoreBounds().getX()
                && outputBounds != metadataBounds
                && !outputBounds.intersects(metadataBounds),
            "one tab tears into one standalone Mag without moving or covering its former sibling");
    require(findDescendantWithId(host, "workspace.docked.tab.output-meter") == nullptr,
            "a two-item stack dissolves after one tab tears off");
    require(host.undoUserLayout()
                && findDescendantWithId(host, "workspace.docked.tab.output-meter") != nullptr,
            "Undo reconstructs the exact docked Tab Stack");
    require(host.redoUserLayout()
                && findDescendantWithId(host, "workspace.docked.tab.output-meter") == nullptr,
            "Redo repeats the one-tab tear-off without collapsing the mosaic");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter, WorkspaceModuleId::metadata },
                         "tab tear-off and redo leave no blank region");

    require(host.undoUserLayout(),
            "Undo reconstructs the Tab Stack for the inactive-tab tear-off case");
    (void) host.activateTab(WorkspaceModuleId::outputMeter);
    auto* metadataTab = dynamic_cast<juce::TextButton*>(
        findDescendantWithId(host, "workspace.docked.tab.metadata"));
    require(metadataTab != nullptr && !metadataTab->getToggleState(),
            "Metadata is a real inactive tab before direct tear-off");
    const auto inactiveCore = host.getCoreBounds();
    const auto inactiveDrop = juce::Point<int> { inactiveCore.getX() + 2,
                                                  inactiveCore.getCentreY() };
    const auto inactiveDown = metadataTab->getLocalBounds().getCentre().toFloat();
    const auto inactiveHostDrop = metadataTab->getLocalPoint(&host, inactiveDrop.toFloat());
    input.mouseDown(pointerEvent(*metadataTab, inactiveDown, inactiveDown,
                                 juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(*metadataTab, inactiveHostDrop, inactiveDown,
                                 juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(*metadataTab, inactiveHostDrop, inactiveDown,
                               juce::ModifierKeys::leftButtonModifier, true));
    require(host.getModuleTileBounds(WorkspaceModuleId::metadata).getRight()
                    <= host.getCoreBounds().getX()
                && host.getModuleTileBounds(WorkspaceModuleId::metadata)
                    != host.getModuleTileBounds(WorkspaceModuleId::outputMeter),
            "an inactive tab tears directly into one standalone Mag");
}

void runDenseCompositionPointerRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1200, 760);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 1, 1), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 3, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.maximumSpan = { 4, 3 };
    eqDescriptor.defaultGridColumn = 2;
    host.registerModule(eqDescriptor, 360, [] { return std::make_unique<juce::Component>(); });
    for (const auto id : { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq })
    {
        host.setModuleAvailable(id, true);
    }
    (void) host.dockModule(WorkspaceModuleId::parametricEq,
                           WorkspaceModuleId::coreWaveform,
                           WorkspaceDockZone::bottom);

    auto& eq = *host.getModuleComponent(WorkspaceModuleId::parametricEq);
    const auto core = host.getCoreBounds();
    const juce::Point<float> down { 14.0f, 14.0f };
    const auto rightDrop = eq.getLocalPoint(
        &host,
        juce::Point<float>(static_cast<float>(core.getRight() - 2),
                           static_cast<float>(core.getCentreY())));
    auto& input = static_cast<juce::Component&>(host);
    input.mouseDown(pointerEvent(eq, down, down,
                                 juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(eq, rightDrop, down,
                                 juce::ModifierKeys::leftButtonModifier, true));
    const auto preview = host.getDragPreviewBoundsForTesting();
    require(preview.has_value(),
            "a dense Output/Playlist/Metadata composition still accepts EQ on the waveform right edge");
    input.mouseUp(pointerEvent(eq, rightDrop, down,
                               juce::ModifierKeys::leftButtonModifier, true));
    const auto eqBounds = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    require(eqBounds == *preview && eqBounds.getX() >= host.getCoreBounds().getRight(),
            "dense pointer routing commits EQ to the same right-edge tile that was previewed");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq },
                         "dense right-edge docking preserves a filled non-overlapping mosaic");

    for (const auto zone : { WorkspaceDockZone::top,
                             WorkspaceDockZone::bottom,
                             WorkspaceDockZone::left })
    {
        const auto currentCore = host.getCoreBounds();
        const auto hostDrop = zone == WorkspaceDockZone::top
            ? juce::Point<int> { currentCore.getCentreX(), currentCore.getY() + 2 }
            : zone == WorkspaceDockZone::bottom
                ? juce::Point<int> { currentCore.getCentreX(), currentCore.getBottom() - 2 }
                : juce::Point<int> { currentCore.getX() + 2, currentCore.getCentreY() };
        const auto localDrop = eq.getLocalPoint(&host, hostDrop.toFloat());
        input.mouseDown(pointerEvent(eq, down, down,
                                     juce::ModifierKeys::leftButtonModifier, false));
        input.mouseDrag(pointerEvent(eq, localDrop, down,
                                     juce::ModifierKeys::leftButtonModifier, true));
        const auto densePreview = host.getDragPreviewBoundsForTesting();
        require(densePreview.has_value(),
                "dense compositions expose every remaining waveform edge as a drop target");
        input.mouseUp(pointerEvent(eq, localDrop, down,
                                   juce::ModifierKeys::leftButtonModifier, true));
        require(host.getModuleTileBounds(WorkspaceModuleId::parametricEq) == *densePreview,
                "dense edge changes commit the exact previewed EQ tile");
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::outputMeter,
                               WorkspaceModuleId::playlist,
                               WorkspaceModuleId::metadata,
                               WorkspaceModuleId::parametricEq },
                             "dense four-edge routing remains filled and non-overlapping");
    }
}

void runCrossBandMigrationRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1200, 760);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0), 180, []
    {
        auto component = std::make_unique<juce::Component>();
        component->setInterceptsMouseClicks(false, false);
        return component;
    });
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 1, 1), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 3, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.maximumSpan = { 4, 3 };
    eqDescriptor.defaultGridColumn = 2;
    host.registerModule(eqDescriptor, 360, [] { return std::make_unique<juce::Component>(); });
    for (const auto id : { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq })
    {
        host.setModuleAvailable(id, true);
    }

    require(host.dockModule(WorkspaceModuleId::parametricEq,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::top)
                == WorkspaceGridMutationResult::committed,
            "EQ can occupy the north band before a cross-band migration");
    require(host.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::left)
                == WorkspaceGridMutationResult::committed,
            "Output can occupy the west band before a cross-band migration");
    const auto playlistEast = host.dockModule(WorkspaceModuleId::playlist,
                                              WorkspaceModuleId::coreWaveform,
                                              WorkspaceDockZone::right);
    require(playlistEast == WorkspaceGridMutationResult::committed
                || playlistEast == WorkspaceGridMutationResult::noChange,
            "Playlist can occupy the east band before a cross-band migration");
    const auto metadataEast = host.dockModule(WorkspaceModuleId::metadata,
                                              WorkspaceModuleId::coreWaveform,
                                              WorkspaceDockZone::right);
    require(metadataEast == WorkspaceGridMutationResult::committed
                || metadataEast == WorkspaceGridMutationResult::noChange,
            "Metadata can share the populated east band before a cross-band migration");

    auto& output = *host.getModuleComponent(WorkspaceModuleId::outputMeter);
    auto& input = static_cast<juce::Component&>(host);
    const juce::Point<float> down { 14.0f, 14.0f };
    require(output.hitTest(static_cast<int>(down.x), static_cast<int>(down.y)),
            "a passive meter still owns its Mag header hit target");
    const auto core = host.getCoreBounds();
    const auto rightDrop = output.getLocalPoint(
        &host,
        juce::Point<float>(static_cast<float>(core.getRight() - 2),
                           static_cast<float>(core.getCentreY())));
    input.mouseDown(pointerEvent(output, down, down,
                                 juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(output, rightDrop, down,
                                 juce::ModifierKeys::leftButtonModifier, true));
    const auto preview = host.getDragPreviewBoundsForTesting();
    require(preview.has_value(),
            "a west-band Output still previews migration into a populated east band");
    input.mouseUp(pointerEvent(output, rightDrop, down,
                               juce::ModifierKeys::leftButtonModifier, true));
    require(host.getModuleTileBounds(WorkspaceModuleId::outputMeter) == *preview
                && host.getModuleTileBounds(WorkspaceModuleId::outputMeter).getX()
                       >= host.getCoreBounds().getRight(),
            "cross-band Output migration commits the exact east-side preview");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::playlist,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq },
                         "cross-band migration keeps the dense composition filled");
}

void runNestedColumnsAndSharedSplitRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1600, 800);
    host.registerModule(descriptor(WorkspaceModuleId::parametricEq, 0, 0), 360,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 1), 210,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    for (const auto id : { WorkspaceModuleId::parametricEq,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata })
        host.setModuleAvailable(id, true);

    const auto eqEast = host.dockModule(WorkspaceModuleId::parametricEq,
                                        WorkspaceModuleId::coreWaveform,
                                        WorkspaceDockZone::right);
    require(eqEast == WorkspaceGridMutationResult::committed
                || eqEast == WorkspaceGridMutationResult::noChange,
            "EQ creates one real column beside the waveform");
    require(host.dockModule(WorkspaceModuleId::outputMeter,
                            WorkspaceModuleId::parametricEq,
                            WorkspaceDockZone::bottom) == WorkspaceGridMutationResult::committed,
            "Output creates a row below EQ within that column");
    require(host.dockModule(WorkspaceModuleId::metadata,
                            WorkspaceModuleId::parametricEq,
                            WorkspaceDockZone::right) == WorkspaceGridMutationResult::committed,
            "Metadata creates a second independent column to the right of the EQ/Output column");

    const auto eq = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    const auto output = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    const auto metadata = host.getModuleTileBounds(WorkspaceModuleId::metadata);
    if (!(eq.getX() == output.getX() && eq.getWidth() == output.getWidth()
          && output.getY() >= eq.getBottom() && metadata.getX() >= eq.getRight()))
    {
        std::cerr << "Nested split diagnostic: EQ=" << eq.toString()
                  << " Output=" << output.toString()
                  << " Metadata=" << metadata.toString() << '\n';
    }
    require(eq.getX() == output.getX() && eq.getWidth() == output.getWidth()
                && output.getY() >= eq.getBottom()
                && metadata.getX() >= eq.getRight()
                && metadata.getY() <= std::min(eq.getY(), output.getY())
                && metadata.getBottom() >= std::max(eq.getBottom(), output.getBottom())
                && !metadata.intersects(eq) && !metadata.intersects(output),
            "nested grid preserves the requested T-junction instead of flattening every Mag into one rail");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::parametricEq,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata },
                         "nested columns still assign every non-gutter cell to waveform or Mag");
    require(host.splitHandleCountForTesting() >= 3,
            "every visible T-junction exposes shared row/column split hit targets");

    const auto before = host.serialiseUserState();
    auto resized = false;
    for (auto leadingTrack = 0; leadingTrack < 8 && !resized; ++leadingTrack)
        resized = host.resizeSplitBoundaryForTesting(true, leadingTrack, 70);
    require(resized && host.serialiseUserState() != before
                && host.serialiseUserState().contains("gridTracks"),
            "dragging one shared vertical boundary persists track ratios rather than resizing one Mag corner");
    require(host.undoUserLayout() && host.serialiseUserState() == before,
            "one Undo restores the complete shared-boundary resize transaction");
}

void runHierarchicalGroupDockAndCloseRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1800, 820);
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 0, 0), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 0), 232,
                        [] { return std::make_unique<juce::Component>(); });
    for (const auto id : { WorkspaceModuleId::playlist,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata })
        host.setModuleAvailable(id, true);

    const auto playlistLeft = host.dockModule(WorkspaceModuleId::playlist,
                                              WorkspaceModuleId::coreWaveform,
                                              WorkspaceDockZone::left);
    const auto outputRight = host.dockModule(WorkspaceModuleId::outputMeter,
                                             WorkspaceModuleId::coreWaveform,
                                             WorkspaceDockZone::right);
    require((playlistLeft == WorkspaceGridMutationResult::committed
                || playlistLeft == WorkspaceGridMutationResult::noChange)
                && (outputRight == WorkspaceGridMutationResult::committed
                    || outputRight == WorkspaceGridMutationResult::noChange),
            "hierarchical fixture creates left and right siblings around the waveform");
    const auto metadataTop = host.dockModule(WorkspaceModuleId::metadata,
                                             WorkspaceModuleId::coreWaveform,
                                             WorkspaceDockZone::top);
    require(metadataTop == WorkspaceGridMutationResult::committed
                || metadataTop == WorkspaceGridMutationResult::noChange,
            "moving Mag starts away from every bottom-group target");

    auto& moving = *host.getModuleComponent(WorkspaceModuleId::metadata);
    auto& input = static_cast<juce::Component&>(host);
    const juce::Point<float> down { 12.0f, 12.0f };
    const auto previewAt = [&] (juce::Point<int> hostPoint)
    {
        const auto drop = moving.getLocalPoint(&host, hostPoint.toFloat());
        input.mouseDown(pointerEvent(moving, down, down,
                                     juce::ModifierKeys::leftButtonModifier, false));
        input.mouseDrag(pointerEvent(moving, drop, down,
                                     juce::ModifierKeys::leftButtonModifier, true));
        const auto label = host.getDragPreviewLabelForTesting();
        require(host.cancelPointerGesture(), "scope preview remains a cancellable pointer gesture");
        return label;
    };
    const auto core = host.getCoreBounds();
    const auto outer = previewAt({ core.getCentreX(), core.getBottom() - 2 });
    require(outer.contains("Playlist") && outer.contains("Audio Waveform")
                && outer.contains("Output Meter"),
            "the outer bottom lane previews a split below the complete root group");
    const auto middleDepth = std::max(3, (core.getHeight() * 14) / 100);
    const auto leftPair = previewAt({ core.getX() + core.getWidth() / 4,
                                      core.getBottom() - middleDepth });
    const auto rightPair = previewAt({ core.getX() + (core.getWidth() * 3) / 4,
                                       core.getBottom() - middleDepth });
    require(leftPair.contains("Playlist") && leftPair.contains("Audio Waveform")
                && !leftPair.contains("Output Meter")
                && rightPair.contains("Audio Waveform") && rightPair.contains("Output Meter")
                && !rightPair.contains("Playlist"),
            "the middle bottom lane chooses the left or right sibling group from pointer position");
    const auto innerDepth = std::max(3, (core.getHeight() * 27) / 100);
    const auto local = previewAt({ core.getCentreX(), core.getBottom() - innerDepth });
    require(local.contains("Audio Waveform") && !local.contains("Playlist")
                && !local.contains("Output Meter"),
            "the inner bottom lane targets only the waveform tile");

    const auto assertAroundGroup = [&host] (std::vector<WorkspaceModuleId> targets,
                                            WorkspaceDockZone zone,
                                            const std::string& label)
    {
        require(host.dockModuleToGroup(WorkspaceModuleId::metadata,
                                       targets,
                                       zone)
                    == WorkspaceGridMutationResult::committed,
                label + " accepts one hierarchical split transaction");
        juce::Rectangle<int> group;
        for (const auto id : targets)
        {
            const auto bounds = id == WorkspaceModuleId::coreWaveform
                ? host.getCoreBounds() : host.getModuleTileBounds(id);
            group = group.isEmpty() ? bounds : group.getUnion(bounds);
        }
        const auto moved = host.getModuleTileBounds(WorkspaceModuleId::metadata);
        const auto aligned = zone == WorkspaceDockZone::left
            ? moved.getRight() <= group.getX()
                && moved.getY() == group.getY() && moved.getBottom() == group.getBottom()
            : zone == WorkspaceDockZone::right
                ? moved.getX() >= group.getRight()
                    && moved.getY() == group.getY() && moved.getBottom() == group.getBottom()
                : zone == WorkspaceDockZone::top
                    ? moved.getBottom() <= group.getY()
                        && moved.getX() == group.getX() && moved.getRight() == group.getRight()
                    : moved.getY() >= group.getBottom()
                        && moved.getX() == group.getX() && moved.getRight() == group.getRight();
        require(aligned && !moved.intersects(group),
                label + " gives Metadata the exact span beside the selected group");
        requireGapFreeMosaic(host,
                             { WorkspaceModuleId::playlist,
                               WorkspaceModuleId::outputMeter,
                               WorkspaceModuleId::metadata },
                             label + " preserves the no-blank workspace invariant");
        require(host.undoUserLayout(), label + " is one undoable gesture");
    };
    assertAroundGroup({ WorkspaceModuleId::playlist, WorkspaceModuleId::coreWaveform },
                      WorkspaceDockZone::bottom,
                      "Playlist plus waveform group");
    assertAroundGroup({ WorkspaceModuleId::coreWaveform, WorkspaceModuleId::outputMeter },
                      WorkspaceDockZone::bottom,
                      "waveform plus Output group");
    const std::vector rootGroup { WorkspaceModuleId::playlist,
                                  WorkspaceModuleId::coreWaveform,
                                  WorkspaceModuleId::outputMeter };
    assertAroundGroup(rootGroup, WorkspaceDockZone::bottom, "complete root bottom group");
    assertAroundGroup(rootGroup, WorkspaceDockZone::top, "complete root top group");
    assertAroundGroup(rootGroup, WorkspaceDockZone::left, "complete root left group");
    assertAroundGroup(rootGroup, WorkspaceDockZone::right, "complete root right group");

    const auto beforeClose = host.serialiseUserState();
    const auto metadataBeforeClose = host.getModuleTileBounds(WorkspaceModuleId::metadata);
    auto* closeComponent = findDescendantWithId(host, "workspace.close.metadata");
    auto* closeButton = dynamic_cast<juce::Button*>(closeComponent);
    require(closeButton != nullptr,
            "every docked optional Mag exposes a subtle host-owned Close control");
    std::optional<WorkspaceModuleId> requestedClose;
    host.onModuleCloseRequested = [&requestedClose] (WorkspaceModuleId id)
    {
        requestedClose = id;
    };
    closeButton->onClick();
    require(requestedClose == WorkspaceModuleId::metadata
                && host.getModuleComponent(WorkspaceModuleId::metadata) != nullptr,
            "the Close control delegates through the owner lifecycle when one is installed");
    host.onModuleCloseRequested = {};
    closeButton->onClick();
    require(host.getModuleComponent(WorkspaceModuleId::metadata) == nullptr
                && !host.isModuleActive(WorkspaceModuleId::metadata)
                && findDescendantWithId(host, "workspace.close.metadata") == nullptr,
            "Close hides and releases Metadata without leaving inert chrome behind");
    require(host.closeModule(WorkspaceModuleId::coreWaveform)
                == WorkspaceGridMutationResult::protectedCore,
            "the privileged waveform cannot be closed through Mag chrome");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::playlist, WorkspaceModuleId::outputMeter },
                         "closing a Mag lets neighbouring regions reclaim every cell");
    host.setModuleEnabled(WorkspaceModuleId::metadata, true);
    require(host.getModuleComponent(WorkspaceModuleId::metadata) != nullptr,
            "a closed Mag reopens from the existing toolbar command");
    require(host.serialiseUserState() == beforeClose
                && host.getModuleTileBounds(WorkspaceModuleId::metadata) == metadataBeforeClose,
            "closing and reopening without intervening edits restores the same Mag position and split ratios");
}

void runTopologyEditPreservesIndependentSplitRatiosRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1600, 800);
    host.registerModule(descriptor(WorkspaceModuleId::parametricEq, 0, 0), 360,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 1), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 2), 232,
                        [] { return std::make_unique<juce::Component>(); });
    for (const auto id : { WorkspaceModuleId::parametricEq,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata })
        host.setModuleAvailable(id, true);

    require(host.dockModule(WorkspaceModuleId::parametricEq,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::right) != WorkspaceGridMutationResult::noSpace
                && host.dockModule(WorkspaceModuleId::outputMeter,
                                   WorkspaceModuleId::coreWaveform,
                                   WorkspaceDockZone::bottom)
                       == WorkspaceGridMutationResult::committed
                && host.dockModule(WorkspaceModuleId::metadata,
                                   WorkspaceModuleId::outputMeter,
                                   WorkspaceDockZone::bottom)
                       == WorkspaceGridMutationResult::committed,
            "ratio-preservation fixture builds a right EQ beside a left vertical group");

    const auto initialEqWidth = host.getModuleTileBounds(
        WorkspaceModuleId::parametricEq).getWidth();
    auto narrowed = false;
    for (auto leadingTrack = 0; leadingTrack < 8 && !narrowed; ++leadingTrack)
    {
        const auto before = host.getModuleTileBounds(
            WorkspaceModuleId::parametricEq).getWidth();
        if (!host.resizeSplitBoundaryForTesting(true, leadingTrack, 260))
            continue;
        narrowed = host.getModuleTileBounds(
            WorkspaceModuleId::parametricEq).getWidth() < before;
        if (!narrowed)
            require(host.undoUserLayout(),
                    "an unrelated split probe remains one reversible transaction");
    }
    const auto narrowEqWidth = host.getModuleTileBounds(
        WorkspaceModuleId::parametricEq).getWidth();
    require(narrowed && narrowEqWidth < initialEqWidth,
            "shared boundary can deliberately make the EQ a narrow vertical Mag");

    require(host.dockModule(WorkspaceModuleId::metadata,
                            WorkspaceModuleId::outputMeter,
                            WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "Metadata can stack with Output after the independent EQ resize");
    const auto stackedEqWidth = host.getModuleTileBounds(
        WorkspaceModuleId::parametricEq).getWidth();
    require(std::abs(stackedEqWidth - narrowEqWidth) <= 2,
            "stacking one row preserves the unrelated vertical split ratio");

    require(host.dockModule(WorkspaceModuleId::metadata,
                            WorkspaceModuleId::outputMeter,
                            WorkspaceDockZone::right)
                == WorkspaceGridMutationResult::committed,
            "an inactive Metadata tab can tear out beside its former Output stack");
    const auto tornOutEqWidth = host.getModuleTileBounds(
        WorkspaceModuleId::parametricEq).getWidth();
    require(std::abs(tornOutEqWidth - narrowEqWidth) <= 8,
            "tearing a tab into a new column splits only its target group and preserves the narrow EQ");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::parametricEq,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata },
                         "ratio preservation does not weaken the gap-free invariant");
}

void runCustomOpenAndCloseLocalityRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1800, 820);
    host.registerModule(descriptor(WorkspaceModuleId::playlist, 0, 0), 218,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0), 180,
                        [] { return std::make_unique<juce::Component>(); });
    host.registerModule(descriptor(WorkspaceModuleId::metadata, 2, 0), 232,
                        [] { return std::make_unique<juce::Component>(); });
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 3, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.maximumSpan = { 8, 3 };
    host.registerModule(eqDescriptor, 360,
                        [] { return std::make_unique<juce::Component>(); });
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, false);
    for (const auto id : { WorkspaceModuleId::playlist,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq })
        host.setModuleAvailable(id, true);

    require(host.dockModule(WorkspaceModuleId::playlist,
                            WorkspaceModuleId::coreWaveform,
                            WorkspaceDockZone::left) != WorkspaceGridMutationResult::noSpace
                && host.dockModule(WorkspaceModuleId::outputMeter,
                                   WorkspaceModuleId::coreWaveform,
                                   WorkspaceDockZone::right) != WorkspaceGridMutationResult::noSpace
                && host.dockModuleToGroup(WorkspaceModuleId::metadata,
                                          { WorkspaceModuleId::playlist,
                                            WorkspaceModuleId::coreWaveform,
                                            WorkspaceModuleId::outputMeter },
                                          WorkspaceDockZone::bottom)
                       == WorkspaceGridMutationResult::committed,
            "custom open fixture creates a three-tile top group with Metadata below it");

    const auto playlistBefore = host.getModuleTileBounds(WorkspaceModuleId::playlist);
    const auto coreBefore = host.getCoreBounds();
    const auto outputBefore = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    require(playlistBefore.getRight() <= coreBefore.getX()
                && outputBefore.getX() >= coreBefore.getRight(),
            "custom fixture has stable left/core/right topology before opening EQ");

    host.setModuleEnabled(WorkspaceModuleId::parametricEq, true);
    const auto playlistOpen = host.getModuleTileBounds(WorkspaceModuleId::playlist);
    const auto coreOpen = host.getCoreBounds();
    const auto outputOpen = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    const auto metadataOpen = host.getModuleTileBounds(WorkspaceModuleId::metadata);
    const auto eqOpen = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
    require(playlistOpen.getRight() <= coreOpen.getX()
                && outputOpen.getX() >= coreOpen.getRight()
                && eqOpen.getY() >= metadataOpen.getBottom()
                && eqOpen.getX() == 0 && eqOpen.getRight() == host.getWidth(),
            "opening a required Mag appends one root-width row without flattening the custom topology");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::playlist,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata,
                           WorkspaceModuleId::parametricEq },
                         "opening a Mag keeps the custom workspace filled");

    require(host.closeModule(WorkspaceModuleId::parametricEq)
                == WorkspaceGridMutationResult::committed,
            "the appended EQ closes as one workspace transaction");
    const auto playlistClosed = host.getModuleTileBounds(WorkspaceModuleId::playlist);
    const auto coreClosed = host.getCoreBounds();
    const auto outputClosed = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    require(playlistClosed.getRight() <= coreClosed.getX()
                && outputClosed.getX() >= coreClosed.getRight()
                && host.getModuleTileBounds(WorkspaceModuleId::parametricEq).isEmpty(),
            "closing the appended Mag preserves sibling order and reclaims only its row");
    requireGapFreeMosaic(host,
                         { WorkspaceModuleId::playlist,
                           WorkspaceModuleId::outputMeter,
                           WorkspaceModuleId::metadata },
                         "closing a Mag keeps the surviving custom topology filled");
}

void runWideMagDefaultLayoutRegressionTest()
{
    juce::Component waveform;
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.onLayoutChanged = [&] { waveform.setBounds(host.getCoreBounds()); };
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                        180, [] { return std::make_unique<juce::Component>(); });
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 2, 2);
    eqDescriptor.preferredSpan = { 2, 1 };
    eqDescriptor.maximumSpan = { 4, 3 };
    eqDescriptor.defaultGridColumn = 2;
    host.registerModule(eqDescriptor, 360, [] { return std::make_unique<juce::Component>(); });
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, false);
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    host.setModuleAvailable(WorkspaceModuleId::parametricEq, true);
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, true);
    const auto* eq = host.getModuleComponent(WorkspaceModuleId::parametricEq);
    const auto* meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    require(eq != nullptr && meter != nullptr
                && !eq->getBounds().intersects(waveform.getBounds())
                && !eq->getBounds().intersects(meter->getBounds()),
            "opening a wide EQ must not let default right-column packing overwrite the waveform");
}

void runFreelyResizableMagBoundaryRegressionTest()
{
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1200, 800);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                        420, [] { return std::make_unique<juce::Component>(); });
    host.setModuleVisualSize(WorkspaceModuleId::outputMeter,
                             singleeditor::RendererMagSize::large);
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);

    const auto wide = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    auto narrowed = false;
    for (auto leadingTrack = 0; leadingTrack < 16 && !narrowed; ++leadingTrack)
        narrowed = host.resizeSplitBoundaryForTesting(true, leadingTrack, 4000);
    const auto narrow = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    if (!(narrowed && narrow.getWidth() < wide.getWidth() && narrow.getWidth() <= 96))
        std::cerr << "Output narrow diagnostic: before=" << wide.toString()
                  << " after=" << narrow.toString() << " resized=" << narrowed << '\n';
    require(narrowed && narrow.getWidth() < wide.getWidth() && narrow.getWidth() <= 96,
            "a large Output Mag can be narrowed to a compact column through its shared boundary");

    const auto dockedBottom = host.dockModule(WorkspaceModuleId::outputMeter,
                                               WorkspaceModuleId::coreWaveform,
                                               WorkspaceDockZone::bottom);
    require(dockedBottom == WorkspaceGridMutationResult::committed
                || dockedBottom == WorkspaceGridMutationResult::noChange,
            "Output can become a bottom row before testing the horizontal boundary");
    const auto tall = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    auto shortened = false;
    for (auto leadingTrack = 0; leadingTrack < 16 && !shortened; ++leadingTrack)
        shortened = host.resizeSplitBoundaryForTesting(false, leadingTrack, 4000);
    const auto shortRow = host.getModuleTileBounds(WorkspaceModuleId::outputMeter);
    require(shortened && shortRow.getHeight() < tall.getHeight() && shortRow.getHeight() <= 72,
            "the same large Output Mag can be shortened to a compact row through its shared boundary");

    const auto saved = host.serialiseUserState();
    singleeditor::WorkspaceModuleHostComponent restored;
    restored.setBounds(0, 0, 1200, 800);
    restored.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                            420, [] { return std::make_unique<juce::Component>(); });
    restored.setModuleVisualSize(WorkspaceModuleId::outputMeter,
                                 singleeditor::RendererMagSize::large);
    restored.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    require(restored.restoreUserState(saved).applied()
                && restored.getModuleTileBounds(WorkspaceModuleId::outputMeter).getHeight()
                       == shortRow.getHeight(),
            "persisted split extents restore the compact physical boundary without adding its floor twice");
}

void runHeaderControlDragRegressionTest()
{
    juce::TextButton headerButton("Action");
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0), 180, [&]
    {
        auto content = std::make_unique<juce::Component>();
        content->addAndMakeVisible(headerButton);
        headerButton.setBounds(5, 5, 80, 28);
        return content;
    });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    const auto before = host.serialiseUserState();
    const juce::Point<float> down { 15.0f, 15.0f };
    const auto end = headerButton.getLocalPoint(&host, juce::Point<float>(100.0f, 595.0f));
    auto& input = static_cast<juce::Component&>(host);
    input.mouseDown(pointerEvent(headerButton, down, down, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(headerButton, end, down, juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(headerButton, end, down, juce::ModifierKeys::leftButtonModifier, true));
    require(host.serialiseUserState() == before,
            "dragging a header control must not also move or dock the surrounding Mag");

    auto& tile = *host.getModuleComponent(WorkspaceModuleId::outputMeter);
    const auto tileEnd = tile.getLocalPoint(&host, juce::Point<float>(100.0f, 595.0f));
    tile.getProperties().set("workspace.headerHeight", 32);
    const juce::Point<float> firstRow { 15.0f, 38.0f };
    input.mouseDown(pointerEvent(tile, firstRow, firstRow, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(tile, tileEnd, firstRow, juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(tile, tileEnd, firstRow, juce::ModifierKeys::leftButtonModifier, true));
    require(host.serialiseUserState() == before,
            "a dense panel's first content row is outside its declared title drag area");
    tile.getProperties().remove("workspace.headerHeight");
    input.mouseDown(pointerEvent(tile, down, down, juce::ModifierKeys::rightButtonModifier, false));
    input.mouseDrag(pointerEvent(tile, tileEnd, down, juce::ModifierKeys::rightButtonModifier, true));
    input.mouseUp(pointerEvent(tile, tileEnd, down, juce::ModifierKeys::rightButtonModifier, true));
    require(host.serialiseUserState() == before,
            "a secondary-button gesture must not rearrange the workspace");

    input.mouseDown(pointerEvent(tile, down, down, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(tile, tileEnd, down, juce::ModifierKeys::leftButtonModifier, true));
    require(input.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)),
            "Escape consumes an active Mag drag without changing the audio selection");
    input.mouseUp(pointerEvent(tile, tileEnd, down, juce::ModifierKeys::leftButtonModifier, true));
    require(host.serialiseUserState() == before,
            "releasing the pointer after Escape must not commit the canceled drop");
    const auto outside = tile.getLocalPoint(&host, juce::Point<float>(100.0f, 1200.0f));
    input.mouseDown(pointerEvent(tile, down, down, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(tile, outside, down, juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(tile, outside, down, juce::ModifierKeys::leftButtonModifier, true));
    require(host.serialiseUserState() == before,
            "releasing a tile outside the workspace must not create an off-screen grid placement");
    host.setVisible(true); // Component-tree hit testing only; no native peer/window.
    const auto splitPoint = juce::Point<int> { host.getCoreBounds().getRight() + 2,
                                               host.getCoreBounds().getCentreY() };
    auto* grip = host.getComponentAt(splitPoint.x, splitPoint.y);
    require(grip != nullptr && grip != &tile && grip != &host,
            "the shared boundary between waveform and Mag has its own hit target");
    const juce::Point<float> gripDown { 2.0f, 2.0f };
    const juce::Point<float> gripEnd { 2.0f, 402.0f };
    grip->mouseDown(pointerEvent(*grip, gripDown, gripDown, juce::ModifierKeys::rightButtonModifier, false));
    grip->mouseDrag(pointerEvent(*grip, gripEnd, gripDown, juce::ModifierKeys::rightButtonModifier, true));
    grip->mouseUp(pointerEvent(*grip, gripEnd, gripDown, juce::ModifierKeys::rightButtonModifier, true));
    require(host.serialiseUserState() == before,
            "a secondary-button gesture on the shared split must not resize either region");
}

void runFocusableTitleDragRegressionTest()
{
    juce::Label focusableTitle;
    juce::Component playlist;
    playlist.addAndMakeVisible(focusableTitle);
    focusableTitle.setBounds(4, 4, 120, 24);
    focusableTitle.setWantsKeyboardFocus(true);

    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::playlist, 1, 0), 218, playlist);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);

    const auto before = playlist.getBounds();
    const auto core = host.getCoreBounds();
    const juce::Point<float> down { 8.0f, 8.0f };
    const auto drop = focusableTitle.getLocalPoint(
        &host,
        juce::Point<float>(static_cast<float>(core.getCentreX()),
                           static_cast<float>(core.getBottom() - 2)));
    auto& input = static_cast<juce::Component&>(host);
    input.mouseDown(pointerEvent(focusableTitle,
                                 down,
                                 down,
                                 juce::ModifierKeys::leftButtonModifier,
                                 false));
    input.mouseDrag(pointerEvent(focusableTitle,
                                 drop,
                                 down,
                                 juce::ModifierKeys::leftButtonModifier,
                                 true));
    input.mouseUp(pointerEvent(focusableTitle,
                               drop,
                               down,
                               juce::ModifierKeys::leftButtonModifier,
                               true));

    require(playlist.getBounds() != before
                && playlist.getY() >= host.getCoreBounds().getBottom(),
            "a focusable Playlist/Metadata title remains a draggable Mag surface");
}

void runWorkspaceCanvasAuthorityRegressionTest()
{
    require(singleeditor::workspaceOwnsCanvas({ true, true, false }),
            "a transiently hidden Host remains the sole canvas authority after construction");
    require(!singleeditor::workspaceOwnsCanvas({ false, true, true })
                && !singleeditor::workspaceOwnsCanvas({ true, false, true }),
            "the workspace owns the canvas only after the Host and an active Mag both exist");
}

void runBorrowedPointerAndBoundsTest()
{
    juce::Component playlist;
    juce::Component metadata;
    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::playlist, 1, 0), 218, playlist);
    host.registerBorrowedModule(descriptor(WorkspaceModuleId::metadata, 2, 1), 232, metadata);
    host.setModuleAvailable(WorkspaceModuleId::playlist, true);
    host.setModuleAvailable(WorkspaceModuleId::metadata, true);
    const juce::Point<float> down { 15.0f, 15.0f };
    auto core = host.getCoreBounds();
    auto& input = static_cast<juce::Component&>(host);
    const auto leftDrop = metadata.getLocalPoint(&host,
        juce::Point<float>(static_cast<float>(core.getX() + 2), static_cast<float>(core.getCentreY())));
    input.mouseDown(pointerEvent(metadata, down, down, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(metadata, leftDrop, down, juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(metadata, leftDrop, down, juce::ModifierKeys::leftButtonModifier, true));
    require(host.getModuleComponent(WorkspaceModuleId::metadata) == &metadata
                && metadata.getRight() <= host.getCoreBounds().getX(),
            "dragging Metadata's own title docks the same view left of the protected waveform");
    core = host.getCoreBounds();
    const auto drop = playlist.getLocalPoint(&host,
        juce::Point<float>(static_cast<float>(core.getCentreX()), static_cast<float>(core.getBottom() - 2)));
    input.mouseDown(pointerEvent(playlist, down, down, juce::ModifierKeys::leftButtonModifier, false));
    input.mouseDrag(pointerEvent(playlist, drop, down, juce::ModifierKeys::leftButtonModifier, true));
    input.mouseUp(pointerEvent(playlist, drop, down, juce::ModifierKeys::leftButtonModifier, true));
    require(host.getModuleComponent(WorkspaceModuleId::playlist) == &playlist
                && playlist.getY() >= host.getCoreBounds().getBottom(),
            "dragging a borrowed Playlist header really docks the same view below the waveform");
    for (const auto size : { juce::Point<int>(800, 520), juce::Point<int>(1600, 900),
                             juce::Point<int>(1000, 600) })
    {
        host.setSize(size.x, size.y);
        require(host.getLocalBounds().contains(playlist.getBounds())
                    && host.getLocalBounds().contains(metadata.getBounds())
                    && host.getLocalBounds().contains(host.getCoreBounds())
                    && !playlist.getBounds().intersects(host.getCoreBounds())
                    && !metadata.getBounds().intersects(host.getCoreBounds())
                    && !playlist.getBounds().intersects(metadata.getBounds()),
                "all borrowed tiles and waveform remain inside the resized host without intersecting");
    }
}

void runReenablePlacedModuleRegressionTest()
{
    auto eqDescriptor = descriptor(WorkspaceModuleId::parametricEq, 2, 2);
    WorkspaceModuleLayout disabledState;
    disabledState.registerModule(eqDescriptor);
    disabledState.setAvailable(WorkspaceModuleId::parametricEq, true);
    require(disabledState.setModulePlaced(WorkspaceModuleId::parametricEq, false)
                == WorkspaceGridMutationResult::committed,
            "disabled restore fixture removes EQ placement");
    disabledState.setEnabled(WorkspaceModuleId::parametricEq, false);
    const auto disabledPreset = disabledState.serialiseUserState();

    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    auto factoryCalls = 0;
    auto destructions = 0;
    host.registerModule(eqDescriptor,
                        360,
                        [&]
                        {
                            ++factoryCalls;
                            return std::make_unique<CountedComponent>(destructions);
                        });

    require(host.restoreUserState(juce::String::fromUTF8(disabledPreset.c_str())).applied(),
            "host restores a preset whose EQ capability and placement are both false");
    host.setModuleAvailable(WorkspaceModuleId::parametricEq, true);
    require(factoryCalls == 0 && !host.isModuleEnabled(WorkspaceModuleId::parametricEq),
            "an available post-Core Mag can restore disabled and unplaced until requested");

    host.setModuleEnabled(WorkspaceModuleId::parametricEq, true);
    require(factoryCalls == 1
                && host.isModuleEnabled(WorkspaceModuleId::parametricEq)
                && host.getModuleComponent(WorkspaceModuleId::parametricEq) != nullptr,
            "one EQ request atomically restores capability and placement after a disabled preset");

    host.registerModule(descriptor(WorkspaceModuleId::truePeakLimiter, 3, 3),
                        340,
                        [&]
                        {
                            ++factoryCalls;
                            return std::make_unique<CountedComponent>(destructions);
                        });
    host.setModuleEnabled(WorkspaceModuleId::truePeakLimiter, false);
    host.setModuleAvailable(WorkspaceModuleId::truePeakLimiter, true);
    require(factoryCalls == 1
                && !host.isModuleEnabled(WorkspaceModuleId::truePeakLimiter),
            "available Limiter Mag remains processor-free and View-free until requested");
    host.setModuleEnabled(WorkspaceModuleId::truePeakLimiter, true);
    require(factoryCalls == 2
                && host.getModuleComponent(WorkspaceModuleId::truePeakLimiter) != nullptr,
            "explicit Limiter request creates its View through the shared workspace host");
}

void runTabLazyRestoreAndResidencyTest()
{
    WorkspaceModuleLayout state;
    state.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0));
    state.registerModule(descriptor(WorkspaceModuleId::ambisonicEnergy, 0, 1));
    state.setAvailable(WorkspaceModuleId::outputMeter, true);
    state.setAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    require(state.dockModule(WorkspaceModuleId::outputMeter,
                             WorkspaceModuleId::ambisonicEnergy,
                             WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed,
            "residency fixture saves a real Tab Stack");
    const auto preset = state.serialiseUserState();

    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    int meterFactoryCalls = 0;
    int energyFactoryCalls = 0;
    int meterDestructions = 0;
    int energyDestructions = 0;
    int lifecyclePrepareCalls = 0;
    std::vector<std::pair<WorkspaceModuleId, bool>> visibilityTransitions;
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 1, 0),
                        180,
                        [&]
                        {
                            ++meterFactoryCalls;
                            return std::make_unique<CountedComponent>(meterDestructions);
                        });
    host.registerModule(descriptor(WorkspaceModuleId::ambisonicEnergy, 0, 1),
                        280,
                        [&]
                        {
                            ++energyFactoryCalls;
                            return std::make_unique<CountedComponent>(energyDestructions);
                        });
    host.onBeforeModuleViewCreated = [&] (WorkspaceModuleId)
    {
        ++lifecyclePrepareCalls;
        return true;
    };
    host.onModuleVisibilityChanged = [&] (WorkspaceModuleId id, bool visible)
    {
        visibilityTransitions.emplace_back(id, visible);
    };

    const auto restored = host.restoreUserState(preset);
    require(restored.status == WorkspaceRestoreStatus::applied
                && meterFactoryCalls == 0 && energyFactoryCalls == 0
                && lifecyclePrepareCalls == 0 && visibilityTransitions.empty()
                && !host.isInactiveResidencyMaintenanceRunning(),
            "post-Core preset staging does not construct any unavailable tab View");
    host.setModuleAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    require(energyFactoryCalls == 0,
            "making an inactive restored tab available still does not construct its View");
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    require(meterFactoryCalls == 1 && energyFactoryCalls == 0
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) != nullptr
                && lifecyclePrepareCalls == 1
                && visibilityTransitions.size() == 1
                && visibilityTransitions.back()
                       == std::pair { WorkspaceModuleId::outputMeter, true },
            "only the active restored tab invokes its factory");

    require(host.activateTab(WorkspaceModuleId::ambisonicEnergy)
                == WorkspaceGridMutationResult::committed
                && energyFactoryCalls == 1
                && host.inactiveResidentViewCount() == 1
                && host.isInactiveResidencyMaintenanceRunning()
                && !host.getModuleComponent(WorkspaceModuleId::outputMeter)->isVisible()
                && host.getModuleComponent(WorkspaceModuleId::ambisonicEnergy)->isVisible()
                && lifecyclePrepareCalls == 2
                && visibilityTransitions.size() == 3
                && visibilityTransitions[1]
                       == std::pair { WorkspaceModuleId::outputMeter, false }
                && visibilityTransitions[2]
                       == std::pair { WorkspaceModuleId::ambisonicEnergy, true },
            "switching tabs sleeps the old View and lazily creates only the new active View");
    require(host.activateTab(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && meterFactoryCalls == 1
                && energyFactoryCalls == 1
                && host.inactiveResidentViewCount() == 1
                && lifecyclePrepareCalls == 2,
            "a legal hot resident View is reused without another factory call");

    host.releaseInactiveViewsForMemoryPressure();
    require(host.inactiveResidentViewCount() == 0 && energyDestructions == 1
                && !host.isInactiveResidencyMaintenanceRunning(),
            "memory pressure synchronously clears the bounded inactive LRU pool");
    require(host.activateTab(WorkspaceModuleId::ambisonicEnergy)
                == WorkspaceGridMutationResult::committed
                && energyFactoryCalls == 2
                && lifecyclePrepareCalls == 3,
            "an evicted tab rebuilds only its View from retained state when reactivated");
    const auto tabFocusIdentity = host.currentFocusIdentity();
    require(tabFocusIdentity.has_value()
                && !tabFocusIdentity->placementId.empty()
                && !tabFocusIdentity->tabStackId.empty()
                && !tabFocusIdentity->tabInstanceId.empty(),
            "the active Tab exposes a stable runtime focus token without waking its hidden sibling");
}

void runModernNorthStarNativeSliceTest()
{
    using Contract = singleeditor::ModernNorthStarVisualContract;
    using Density = singleeditor::ModernNorthStarDensity;
    using Edge = singleeditor::ModernNorthStarEdge;
    using Size = singleeditor::ModernNorthStarMagSize;
    using Tier = singleeditor::ModernNorthStarInformationTier;

    require(Contract::informationTier(Density::compact, Size::large) == Tier::summary,
            "Compact suppresses detail even when a Mag keeps large geometry");
    require(Contract::informationTier(Density::standard, Size::small) == Tier::summary
                && Contract::informationTier(Density::standard, Size::medium) == Tier::standard
                && Contract::informationTier(Density::standard, Size::large) == Tier::detail,
            "Standard resolves S/M/L to summary/standard/detail");
    require(Contract::informationTier(Density::expanded, Size::small) == Tier::standard
                && Contract::informationTier(Density::expanded, Size::automatic) == Tier::detail,
            "Expanded preserves the small standard floor and expands other Mags to detail");
    require(Contract::mainAxisWeight(Size::small) < Contract::mainAxisWeight(Size::medium)
                && Contract::mainAxisWeight(Size::medium) < Contract::mainAxisWeight(Size::large),
            "per-Mag S/M/L weights are independent and monotonic");
    require(Contract::usesHorizontalMainAxis(Edge::north)
                && Contract::usesHorizontalMainAxis(Edge::south)
                && !Contract::usesHorizontalMainAxis(Edge::east)
                && !Contract::usesHorizontalMainAxis(Edge::west),
            "north/south size width while east/west size height");

    singleeditor::WorkspaceModuleHostComponent host;
    host.setBounds(0, 0, 1000, 600);
    int meterFactoryCalls = 0;
    int meterDestructions = 0;
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0),
                        180,
                        [&]
                        {
                            ++meterFactoryCalls;
                            return std::make_unique<CountedComponent>(meterDestructions);
                        });
    host.setModuleVisualSize(WorkspaceModuleId::outputMeter, Size::large);

    host.setVisualDensity(Density::compact);
    require(meterFactoryCalls == 0 && !host.isVisualMotionActive(),
            "size/density changes before availability and presentation readiness wake no View or motion timer");
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    auto* meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    require(meter != nullptr
                && static_cast<int>(meter->getProperties()["northStarInformationTier"])
                       == static_cast<int>(Tier::summary),
            "Compact projects the summary tier into the native Mag without a second View");
    const auto compactCore = host.getCoreBounds();

    host.setVisualDensity(Density::expanded);
    require(static_cast<int>(meter->getProperties()["northStarInformationTier"])
                == static_cast<int>(Tier::standard)
                && !static_cast<bool>(meter->getProperties()["northStarSweetSpotSatisfied"])
                && host.getCoreBounds() != compactCore,
            "Expanded cannot force detail into a content rectangle below the large Mag sweet spot");

    host.setBounds(0, 0, 1280, 600);
    require(static_cast<int>(meter->getProperties()["northStarInformationTier"])
                == static_cast<int>(Tier::detail)
                && static_cast<bool>(meter->getProperties()["northStarSweetSpotSatisfied"])
                && host.getModuleVisualSize(WorkspaceModuleId::outputMeter) == Size::large,
            "the same large Mag reaches detail only when its real content rectangle reaches the sweet spot");

    singleeditor::RendererVisualStyle::setReduceMotion(false);
    host.setPresentationReady(true);
    require(host.isVisualMotionActive(),
            "post-Core presentation readiness permits a bounded visual transition for an existing Mag");
    singleeditor::RendererVisualStyle::setReduceMotion(true);
    host.setVisualDensity(Density::standard);
    require(!host.isVisualMotionActive() && meter->getAlpha() == 1.0f,
            "Reduce Motion interrupts an active transition and applies the new state directly");
    singleeditor::RendererVisualStyle::setReduceMotion(false);

    const auto snapshot = host.createComponentSnapshot(host.getLocalBounds(), true, 1.5f);
    require(snapshot.isValid() && snapshot.getWidth() == 1920 && snapshot.getHeight() == 900,
            "representative Mag chrome paints at 150 percent DPI without overflow");
    require(meter->getComponentID() == "workspace.module.output-meter"
                && meter->getTitle() == "Output Meter"
                && meter->getWantsKeyboardFocus(),
            "native slice retains stable accessibility identity and focus semantics");
}

void runFloatingNativeProjectionTest()
{
    juce::Component owner;
    owner.setBounds(80, 80, 1200, 760);
    singleeditor::WorkspaceModuleHostComponent host;
    juce::Component waveform;
    waveform.setWantsKeyboardFocus(true);
    owner.addAndMakeVisible(host);
    owner.addAndMakeVisible(waveform);
    host.setBounds(0, 0, 1200, 760);
    waveform.setBounds(host.getCoreBounds());
    host.setPrivilegedCoreComponent(&waveform);

    int meterFactoryCalls = 0;
    int meterDestructions = 0;
    host.registerModule(descriptor(WorkspaceModuleId::outputMeter, 0, 0),
                        180,
                        [&]
                        {
                            ++meterFactoryCalls;
                            return std::make_unique<CountedComponent>(meterDestructions);
                        });
    host.setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    auto* meter = host.getModuleComponent(WorkspaceModuleId::outputMeter);
    require(host.floatModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::moduleUnavailable
                && host.floatingWindowCountForTesting() == 0
                && !host.focusFloatingModule(WorkspaceModuleId::outputMeter),
            "no native floating window can exist before presentation readiness");

    host.setPresentationReady(true);
    require(host.floatModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && host.floatingWindowCountForTesting() == 1
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) == meter
                && meter->getParentComponent() != &host,
            "floating reparents the same Mag View into one native window");
    juce::TopLevelWindow* meterWindow = nullptr;
    for (int index = 0; index < juce::TopLevelWindow::getNumTopLevelWindows(); ++index)
    {
        auto* candidate = juce::TopLevelWindow::getTopLevelWindow(index);
        if (candidate != nullptr && candidate->getName().contains("Output Meter - Single Editor"))
            meterWindow = candidate;
    }
    require(meterWindow != nullptr && meterWindow->isVisible(),
            "the projected Mag owns a visible renderer-native top-level window");
    const auto floatingSnapshot = meterWindow->createComponentSnapshot(
        meterWindow->getLocalBounds(), true, 1.0f);
    auto hasVisualContrast = false;
    if (floatingSnapshot.isValid())
    {
        const auto first = floatingSnapshot.getPixelAt(2, 2);
        for (int y = 0; y < floatingSnapshot.getHeight() && !hasVisualContrast; y += 12)
            for (int x = 0; x < floatingSnapshot.getWidth(); x += 12)
                if (floatingSnapshot.getPixelAt(x, y) != first)
                    hasVisualContrast = true;
    }
    require(floatingSnapshot.isValid()
                && floatingSnapshot.getWidth() >= 240
                && floatingSnapshot.getHeight() >= 160
                && hasVisualContrast,
            "native floating chrome paints non-empty content without a white-frame placeholder");
    require(host.serialiseUserState().contains("\"floating\"")
                && host.redockModule(WorkspaceModuleId::outputMeter)
                       == WorkspaceGridMutationResult::committed
                && host.floatingWindowCountForTesting() == 0
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) == meter
                && meter->getParentComponent() == &host,
            "Redock returns the same View and closes only its native projection");

    for (int cycle = 0; cycle < 12; ++cycle)
    {
        require(host.floatModule(WorkspaceModuleId::outputMeter)
                    == WorkspaceGridMutationResult::committed
                    && host.redockModule(WorkspaceModuleId::outputMeter)
                           == WorkspaceGridMutationResult::committed
                    && host.getModuleComponent(WorkspaceModuleId::outputMeter) == meter
                    && host.floatingWindowCountForTesting() == 0,
                "repeated float and Redock cycles retain exactly one stable Mag View");
    }

    require(host.floatModule(WorkspaceModuleId::coreWaveform)
                == WorkspaceGridMutationResult::committed
                && host.floatingWindowCountForTesting() == 1
                && waveform.getParentComponent() != &owner,
            "the privileged waveform can float without creating a second waveform component");
    require(host.floatModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && host.floatingWindowCountForTesting() == 2,
            "independent floating Mags coexist for targeted focus handoff");
    require(host.focusFloatingModule(WorkspaceModuleId::outputMeter) && meter->hasKeyboardFocus(true),
            "floating Output hands focus to that Mag rather than the already-floating waveform");
    require(host.focusFloatingModule(WorkspaceModuleId::coreWaveform) && waveform.hasKeyboardFocus(true),
            "explicit floating focus also returns to the same privileged waveform");
    const auto floatingBeforeSimple = host.serialiseUserState();
    host.setSimpleView(true);
    require(host.floatingWindowCountForTesting() == 0 && waveform.getParentComponent() == &owner
                && host.serialiseUserState() == floatingBeforeSimple
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) == meter,
            "Simple rehomes the single waveform, closes native float projections and preserves their model");
    host.setSimpleView(false);
    require(host.floatingWindowCountForTesting() == 2 && waveform.getParentComponent() != &owner
                && host.serialiseUserState() == floatingBeforeSimple
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) == meter,
            "Back restores both float windows with their original view identities and placement");
    host.focusFloatingModule(WorkspaceModuleId::coreWaveform);
    require(host.redockModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed,
            "targeted floating focus does not alter Output Redock");
    require(!host.focusFloatingModule(WorkspaceModuleId::outputMeter)
                && waveform.hasKeyboardFocus(true),
            "a stale deferred Float handoff cannot refocus a Mag after Redock");
    require(host.redockModule(WorkspaceModuleId::coreWaveform)
                == WorkspaceGridMutationResult::committed
                && waveform.getParentComponent() == &owner
                && host.floatingWindowCountForTesting() == 0,
            "waveform Redock restores its original parent and stable identity");

    require(host.floatModule(WorkspaceModuleId::outputMeter)
                == WorkspaceGridMutationResult::committed
                && host.removeFloatingTile(WorkspaceModuleId::outputMeter)
                       == WorkspaceGridMutationResult::committed
                && host.floatingWindowCountForTesting() == 0
                && host.getModuleComponent(WorkspaceModuleId::outputMeter) == nullptr
                && meterDestructions == 1,
            "floating-window Remove closes the projection before releasing its owned View");
}
}

void runModernSceneProjectionTest()
{
    using namespace singleeditor;
    WorkspaceModuleHostComponent host;
    int dspViewsCreated = 0;
    for (const auto id : { WorkspaceModuleId::playlist, WorkspaceModuleId::metadata,
             WorkspaceModuleId::markers, WorkspaceModuleId::outputMeter,
             WorkspaceModuleId::parametricEq, WorkspaceModuleId::truePeakLimiter })
    {
        auto spec = descriptor(id, 0, 0);
        if (id == WorkspaceModuleId::markers) spec.defaultPlaced = false;
        spec.maximumSpan = { 4, 4 };
        host.registerModule(spec, 230, [&, id]
        {
            if (id == WorkspaceModuleId::parametricEq || id == WorkspaceModuleId::truePeakLimiter)
                ++dspViewsCreated;
            return std::make_unique<juce::Component>();
        });
        host.setModuleAvailable(id, true);
    }
    const auto restoredScene = host.restoreUserState(modernWorkspaceDefaultLayout);
    require(restoredScene.applied() && restoredScene.quarantinedModuleCount == 0,
            "OD native preset restores without quarantine");
    const auto sharedState = host.serialiseUserState();
    require(dspViewsCreated > 0 && host.isModuleVisible(WorkspaceModuleId::parametricEq),
            "General shows explicitly enabled DSP Mags without a separate DSP scene");
    auto* eqView = host.getModuleComponent(WorkspaceModuleId::parametricEq);
    int simpleDspHideNotifications = 0;
    host.onModuleVisibilityChanged = [&] (WorkspaceModuleId id, bool visible)
    {
        if (!visible && host.isSimpleView()
            && (id == WorkspaceModuleId::parametricEq || id == WorkspaceModuleId::truePeakLimiter))
        {
            ++simpleDspHideNotifications;
            require(!host.isModuleActive(id) && host.isModuleActiveInUserLayout(id),
                    "Simple visibility callback distinguishes hidden DSP from disabled DSP");
        }
    };
    for (const int width : { 640, 800, 1000, 1560, 2400 })
    {
        host.setSize(width, 800);
        const auto fullCore = host.getCoreBounds();
        const auto metadata = host.getModuleTileBounds(WorkspaceModuleId::metadata);
        const auto eq = host.getModuleTileBounds(WorkspaceModuleId::parametricEq);
        require(eq.getY() >= fullCore.getBottom() && eq.getX() == fullCore.getX()
                    && eq.getWidth() == fullCore.getWidth(), "EQ occupies its canonical slot below waveform");
        requireGapFreeMosaic(host, { WorkspaceModuleId::playlist, WorkspaceModuleId::metadata,
                                    WorkspaceModuleId::markers, WorkspaceModuleId::outputMeter,
                                    WorkspaceModuleId::parametricEq, WorkspaceModuleId::truePeakLimiter },
                            "General fills the shared Mag layout");
        host.setSimpleView(true);
        require(host.hasActiveModules() && host.getCoreBounds() == host.getLocalBounds()
                    && !host.isModuleVisible(WorkspaceModuleId::parametricEq)
                    && !host.isModuleVisible(WorkspaceModuleId::metadata),
                "Simple gives the entire host to waveform and hides optional Mags");
        host.releaseInactiveViewsForMemoryPressure();
        require(host.getModuleComponent(WorkspaceModuleId::parametricEq) == eqView
                    && host.serialiseUserState() == sharedState && !host.canUndoUserLayout(),
                "Simple retains active DSP view identity and canonical layout/history");
        host.setSimpleView(false);
        require(host.getCoreBounds() == fullCore
                    && host.getModuleTileBounds(WorkspaceModuleId::metadata) == metadata
                    && host.getModuleTileBounds(WorkspaceModuleId::parametricEq) == eq
                    && host.getModuleComponent(WorkspaceModuleId::parametricEq) == eqView,
                "Back restores exact Mag geometry and DSP view identity");
    }
    require(simpleDspHideNotifications == 10,
            "both active DSP Mags report retained canonical state during every Simple transition");
    host.onModuleVisibilityChanged = {};
    host.setSize(800, 400);
    require(host.getModuleTileBounds(WorkspaceModuleId::playlist).getHeight() >= 100
                && host.getModuleTileBounds(WorkspaceModuleId::metadata).getHeight() >= 150
                && host.getModuleTileBounds(WorkspaceModuleId::markers).getHeight() >= 100,
            "compact General preserves intrinsic file, playlist and marker minima");
    host.setSize(2400, 800);
    require(host.dockModule(WorkspaceModuleId::parametricEq, WorkspaceModuleId::metadata, WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed, "existing Mag Tab interaction remains available");
    const auto custom = host.serialiseUserState();
    host.setSimpleView(true);
    host.setSimpleView(false);
    require(host.isModuleVisible(WorkspaceModuleId::parametricEq) && host.serialiseUserState() == custom
                && host.undoUserLayout(), "Simple restores active Tab and retains layout undo");
    auto legacyScene = juce::JSON::parse(modernWorkspaceDefaultLayout);
    auto* legacyModules = legacyScene.getProperty("modules", {}).getArray();
    require(legacyModules != nullptr, "read prior-preset fixture");
    for (int i = legacyModules->size(); --i >= 0;)
        if ((*legacyModules)[i].getProperty("moduleId", {}).toString() == "markers") legacyModules->remove(i);
    require(host.restoreUserState(juce::JSON::toString(legacyScene)).applied()
                && !host.isModuleActive(WorkspaceModuleId::markers)
                && host.isModuleActive(WorkspaceModuleId::metadata),
            "an existing preset without markers keeps its explicit module choices");
    require(host.restoreUserState(modernWorkspaceDefaultLayout).applied(), "restore current visual scene after legacy check");
    require(host.dockModule(WorkspaceModuleId::markers, WorkspaceModuleId::metadata, WorkspaceDockZone::centre)
                == WorkspaceGridMutationResult::committed && host.isModuleVisible(WorkspaceModuleId::markers),
            "new marker Mag reuses real Tab docking and activation");
    const auto markerStack = host.serialiseUserState();
    require(host.restoreUserState(markerStack).applied() && host.isModuleVisible(WorkspaceModuleId::markers),
            "marker identity and active Tab survive schema roundtrip");
    require(host.closeModule(WorkspaceModuleId::markers) == WorkspaceGridMutationResult::committed
                && host.isModuleVisible(WorkspaceModuleId::metadata) && host.undoUserLayout()
                && host.isModuleVisible(WorkspaceModuleId::markers), "closing markers restores sibling and layout undo restores markers");
    host.setSimpleView(true);
    host.setModuleEnabled(WorkspaceModuleId::parametricEq, false);
    host.setModuleAvailable(WorkspaceModuleId::truePeakLimiter, false);
    require(!host.isModuleActiveInUserLayout(WorkspaceModuleId::parametricEq)
                && !host.isModuleActiveInUserLayout(WorkspaceModuleId::truePeakLimiter),
            "Simple does not retain explicitly disabled or unavailable DSP in the user layout");
    std::cout << "ModernSceneProjectionTests passed (shared layout, retained DSP callbacks, 5 widths, marker Tab/restore/close, undo)\n";
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    singleeditor::RendererSelection::resetForTests(singleeditor::RendererKind::modern);
    runModernSceneProjectionTest();
    require(!singleeditor::workspaceViewRuntime(WorkspaceModuleId::playlist).has_value()
                && !singleeditor::workspaceViewRuntime(WorkspaceModuleId::markers).has_value()
                && !singleeditor::workspaceViewRuntime(WorkspaceModuleId::metadata).has_value()
                && !singleeditor::workspaceViewRuntime(WorkspaceModuleId::parametricEq).has_value()
                && !singleeditor::workspaceViewRuntime(WorkspaceModuleId::truePeakLimiter).has_value()
                && singleeditor::workspaceViewRuntime(WorkspaceModuleId::outputMeter)
                       == singleeditor::OptionalModuleId::meter
                && singleeditor::workspaceViewRuntime(WorkspaceModuleId::ambisonicEnergy)
                       == singleeditor::OptionalModuleId::ambisonics,
            "sidebar/DSP view creation never requests a meter or spatial runtime");
    runWaveformLayoutMutationRegressionTest();
    runPerMagSizeGeometryRegressionTest();
    runPointerFourEdgePreviewRegressionTest();
    runPointerFourZoneAroundMagRegressionTest();
    runTabTearOffRegressionTest();
    runDenseCompositionPointerRegressionTest();
    runCrossBandMigrationRegressionTest();
    runNestedColumnsAndSharedSplitRegressionTest();
    runHierarchicalGroupDockAndCloseRegressionTest();
    runTopologyEditPreservesIndependentSplitRatiosRegressionTest();
    runCustomOpenAndCloseLocalityRegressionTest();
    runWideMagDefaultLayoutRegressionTest();
    runFreelyResizableMagBoundaryRegressionTest();
    runHeaderControlDragRegressionTest();
    runFocusableTitleDragRegressionTest();
    runWorkspaceCanvasAuthorityRegressionTest();
    runGapFreeAdaptiveMosaicRegressionTest();
    runLateWideMagGapFillRegressionTest();
    runDockedTabStripInteractionRegressionTest();
    runFourEdgeMosaicProjectionRegressionTest();
    runBorrowedViewLifecycleTest();
    runBorrowedViewRestoreTest();
    runBorrowedRestoreSafetyTest();
    runBorrowedPointerAndBoundsTest();
    if (argc == 2 && std::string(argv[1]) == "--layout-regression-only")
    {
        std::cout << "WorkspaceModuleHostTests layout regression passed\n";
        return 0;
    }
    runLazyProjectionAndReleaseTest();
    runReenablePlacedModuleRegressionTest();
    runTabLazyRestoreAndResidencyTest();
    runModernNorthStarNativeSliceTest();
    if (argc != 2 || std::string(argv[1]) != "--no-native")
        runFloatingNativeProjectionTest();
    std::cout << "WorkspaceModuleHostTests passed\n";
    return 0;
}
