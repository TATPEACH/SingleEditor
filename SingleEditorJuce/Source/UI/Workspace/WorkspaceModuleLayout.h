#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "WorkspaceFloatingWindowModel.h"

enum class WorkspaceModuleId
{
    coreWaveform,
    outputMeter,
    ambisonicEnergy,
    parametricEq,
    truePeakLimiter,
    playlist,
    metadata,
    markers,
    spatialEnergy
};

enum class WorkspaceGridDirection
{
    none,
    left,
    right,
    up,
    down
};

enum class WorkspaceDockZone
{
    left,
    top,
    centre,
    right,
    bottom
};

enum class WorkspaceGridMutationResult
{
    committed,
    noChange,
    unknownModule,
    moduleUnavailable,
    protectedCore,
    invalidPlacement,
    noSpace,
    tabStackRequired
};

struct WorkspaceGridSpan
{
    int columns = 1;
    int rows = 1;

    [[nodiscard]] bool operator==(const WorkspaceGridSpan& other) const noexcept
    {
        return columns == other.columns && rows == other.rows;
    }
};

struct WorkspaceGridPlacement
{
    int column = 0;
    int row = 0;
    WorkspaceGridSpan span;

    [[nodiscard]] int right() const noexcept { return column + span.columns; }
    [[nodiscard]] int bottom() const noexcept { return row + span.rows; }
    [[nodiscard]] bool overlaps(const WorkspaceGridPlacement& other) const noexcept;
    [[nodiscard]] bool operator==(const WorkspaceGridPlacement& other) const noexcept
    {
        return column == other.column && row == other.row && span == other.span;
    }
};

struct WorkspaceGridItem
{
    WorkspaceModuleId id = WorkspaceModuleId::coreWaveform;
    WorkspaceGridPlacement placement;
    bool privilegedCore = false;
    std::string tabStackId;
    std::vector<WorkspaceModuleId> tabMembers;
};

enum class WorkspaceRestoreStatus
{
    applied,
    migratedLegacyV1,
    repairedLegacyOverlap,
    noState,
    corrupt,
    futureSchema
};

struct WorkspaceRestoreResult
{
    WorkspaceRestoreStatus status = WorkspaceRestoreStatus::noState;
    int quarantinedModuleCount = 0;
    int repairedTileCount = 0;

    [[nodiscard]] bool applied() const noexcept
    {
        return status == WorkspaceRestoreStatus::applied
            || status == WorkspaceRestoreStatus::migratedLegacyV1
            || status == WorkspaceRestoreStatus::repairedLegacyOverlap;
    }
};

struct WorkspaceTabStackInfo
{
    std::string id;
    WorkspaceGridPlacement placement;
    std::vector<WorkspaceModuleId> tabs;
    WorkspaceModuleId activeTab = WorkspaceModuleId::outputMeter;
    std::vector<std::string> tabInstanceIds;
    std::string activeInstanceId;
};

struct WorkspaceInertModuleInfo
{
    std::string instanceId;
    std::string moduleTypeId;
    WorkspaceGridPlacement placement;
    std::string tabStackId;
    bool quarantined = false;
};

struct WorkspaceFloatingTileInfo
{
    std::string id;
    std::vector<WorkspaceModuleId> members;
    bool privilegedCore = false;
    WorkspaceFloatingPlacement placement;
};

struct WorkspaceModuleDescriptor
{
    WorkspaceModuleId id;
    int defaultOrder = 0;
    int defaultWeight = 1000;
    int minimumExtent = 120;
    WorkspaceGridSpan minimumSpan { 1, 1 };
    WorkspaceGridSpan preferredSpan { 1, 1 };
    WorkspaceGridSpan maximumSpan { 2, 2 };
    int defaultGridColumn = -1;
    int defaultGridRow = 0;
    std::string instanceId;
    std::string moduleTypeId;
    std::size_t estimatedViewBytes = 256 * 1024;
    bool allowInactiveViewResidency = true;
    bool restoreDefaultWhenAbsent = false;
    bool defaultPlaced = true;
};

class WorkspaceModuleLayout
{
public:
    void registerModule(WorkspaceModuleDescriptor descriptor);

    void setAvailable(WorkspaceModuleId id, bool available);
    void setEnabled(WorkspaceModuleId id, bool enabled);
    [[nodiscard]] bool isAvailable(WorkspaceModuleId id) const;
    [[nodiscard]] bool isEnabled(WorkspaceModuleId id) const;
    [[nodiscard]] bool isPlaced(WorkspaceModuleId id) const;
    [[nodiscard]] bool isActive(WorkspaceModuleId id) const;
    [[nodiscard]] bool isModuleVisible(WorkspaceModuleId id) const;
    [[nodiscard]] WorkspaceModuleLayout withCoreOnlyPresentation() const;

    [[nodiscard]] std::vector<WorkspaceModuleId> activeModules() const;
    [[nodiscard]] std::vector<WorkspaceModuleId> visibleModules() const;
    void moveModuleToActiveIndex(WorkspaceModuleId id, int activeIndex);

    [[nodiscard]] int gridColumnCount() const noexcept;
    [[nodiscard]] int requiredGridRows() const;
    [[nodiscard]] std::vector<WorkspaceGridItem> gridItems() const;
    [[nodiscard]] std::optional<WorkspaceGridPlacement> getGridPlacement(WorkspaceModuleId id) const;
    [[nodiscard]] WorkspaceGridMutationResult setModulePlaced(WorkspaceModuleId id, bool placed);
    // User-facing open/close is one atomic workspace transaction. Capability
    // enablement and placement remain distinct in the persisted schema, but a
    // toolbar request must never leave a module enabled without a visible tile.
    [[nodiscard]] WorkspaceGridMutationResult setModuleOpen(WorkspaceModuleId id, bool open);
    [[nodiscard]] WorkspaceGridMutationResult moveModule(WorkspaceModuleId id,
                                                         WorkspaceGridPlacement placement,
                                                         WorkspaceGridDirection direction);
    [[nodiscard]] WorkspaceGridMutationResult resizeModule(WorkspaceModuleId id,
                                                           WorkspaceGridSpan span,
                                                           WorkspaceGridDirection direction);
    [[nodiscard]] WorkspaceGridMutationResult dockModule(WorkspaceModuleId id,
                                                         WorkspaceModuleId target,
                                                         WorkspaceDockZone zone);
    // Splits one rectangular group of existing tiles. This is the renderer-
    // neutral seam used by hierarchical/Wwise-style docking: the group may be
    // one tile, a contiguous subset, or the complete workspace root.
    [[nodiscard]] WorkspaceGridMutationResult dockModuleToGroup(
        WorkspaceModuleId id,
        std::vector<WorkspaceModuleId> targets,
        WorkspaceDockZone zone);
    [[nodiscard]] WorkspaceGridMutationResult activateTab(WorkspaceModuleId id);
    [[nodiscard]] WorkspaceGridMutationResult activateTabInstanceId(const std::string& id);
    [[nodiscard]] std::vector<WorkspaceTabStackInfo> tabStacks() const;
    [[nodiscard]] std::optional<WorkspaceTabStackInfo> tabStackForModule(WorkspaceModuleId id) const;
    [[nodiscard]] std::optional<WorkspaceModuleId> moduleForInstanceId(const std::string& id) const;
    [[nodiscard]] std::vector<WorkspaceInertModuleInfo> inertModules() const;
    [[nodiscard]] WorkspaceGridMutationResult floatModule(
        WorkspaceModuleId id,
        WorkspaceFloatingPlacement placement);
    [[nodiscard]] WorkspaceGridMutationResult redockModule(WorkspaceModuleId id);
    [[nodiscard]] WorkspaceGridMutationResult removeFloatingTile(WorkspaceModuleId id);
    [[nodiscard]] bool updateFloatingPlacement(
        WorkspaceModuleId id,
        WorkspaceFloatingPlacement placement);
    [[nodiscard]] bool isFloating(WorkspaceModuleId id) const;
    [[nodiscard]] std::optional<WorkspaceFloatingTileInfo> floatingTileForModule(
        WorkspaceModuleId id) const;
    [[nodiscard]] std::vector<WorkspaceFloatingTileInfo> floatingTiles() const;
    [[nodiscard]] std::size_t estimatedViewBytes(WorkspaceModuleId id) const;
    [[nodiscard]] bool allowsInactiveViewResidency(WorkspaceModuleId id) const;
    bool setModuleViewState(WorkspaceModuleId id, int schemaVersion, std::string jsonPayload);
    [[nodiscard]] std::string getModuleViewState(WorkspaceModuleId id) const;
    [[nodiscard]] bool canUndoGridMutation() const noexcept;
    [[nodiscard]] bool canRedoGridMutation() const noexcept;
    bool undoGridMutation();
    bool redoGridMutation();
    [[nodiscard]] size_t gridUndoDepth() const noexcept;

    void setWeight(WorkspaceModuleId id, int weight);
    [[nodiscard]] int getWeight(WorkspaceModuleId id) const;
    [[nodiscard]] int getMinimumExtent(WorkspaceModuleId id) const;
    [[nodiscard]] std::vector<int> calculateActiveExtents(int totalExtent, int gap) const;
    void setActiveExtents(const std::vector<int>& extents);
    [[nodiscard]] const std::vector<int>& getColumnTrackWeights() const noexcept;
    [[nodiscard]] const std::vector<int>& getRowTrackWeights() const noexcept;
    [[nodiscard]] WorkspaceGridMutationResult setGridTrackWeights(
        std::vector<int> columnWeights,
        std::vector<int> rowWeights);

    [[nodiscard]] std::string serialiseUserState() const;
    WorkspaceRestoreResult restoreUserState(const std::string& state);
    void resetUserState();

private:
    struct Entry
    {
        WorkspaceModuleDescriptor descriptor;
        bool available = false;
        bool enabled = true;
        bool placed = true;
        int order = 0;
        int weight = 1000;
        WorkspaceGridPlacement placement;
        bool gridCustomized = false;
        std::string tabStackId;
        int viewStateVersion = 1;
        std::string viewStateJson = "{}";
        std::string opaqueRecordJson = "{}";
    };

    struct TabStack
    {
        std::string id;
        WorkspaceGridPlacement placement;
        std::vector<std::string> tabInstanceIds;
        std::string activeInstanceId;
        std::string opaqueRecordJson = "{}";
    };

    struct InertModule
    {
        std::string instanceId;
        std::string moduleTypeId;
        WorkspaceGridPlacement placement;
        std::string tabStackId;
        bool enabled = true;
        bool placed = true;
        bool quarantined = false;
        std::string opaqueRecordJson;
    };

    struct FloatingTile
    {
        std::string id;
        WorkspaceFloatingPlacement placement;
        bool privilegedCore = false;
        std::string opaqueRecordJson = "{}";
    };

    struct UserState
    {
        WorkspaceGridPlacement corePlacement;
        std::vector<Entry> entries;
        std::vector<TabStack> stacks;
        std::vector<InertModule> inert;
        std::vector<FloatingTile> floating;
        std::vector<std::string> opaqueFloatingRecords;
        std::vector<int> columnTrackWeights;
        std::vector<int> rowTrackWeights;
        int nextStackSequence = 1;
    };

    struct TrackWeightSnapshot
    {
        std::size_t columnTrackCount = 0;
        std::size_t rowTrackCount = 0;
        std::vector<int> columnWeights;
        std::vector<int> rowWeights;
        std::vector<int> columnTrackStarts;
        std::vector<int> rowTrackStarts;
    };

    struct ClosedDock
    {
        WorkspaceModuleId id;
        UserState beforeClose;
        UserState afterClose;
    };

    [[nodiscard]] Entry* findEntry(WorkspaceModuleId id);
    [[nodiscard]] const Entry* findEntry(WorkspaceModuleId id) const;
    [[nodiscard]] Entry* findEntryByInstanceId(const std::string& id);
    [[nodiscard]] const Entry* findEntryByInstanceId(const std::string& id) const;
    [[nodiscard]] std::vector<Entry*> entriesInOrder();
    [[nodiscard]] std::vector<const Entry*> entriesInOrder() const;
    [[nodiscard]] UserState captureUserState() const;
    [[nodiscard]] TrackWeightSnapshot captureTrackWeightSnapshot() const;
    void preserveUnchangedTrackWeights(const TrackWeightSnapshot& snapshot);
    void preserveTrackWeightsAfterSplit(const TrackWeightSnapshot& snapshot,
                                        WorkspaceGridPlacement groupRegion,
                                        WorkspaceDockZone zone);
    void restoreUserState(const UserState& state);
    [[nodiscard]] bool userStateEquals(const UserState& left, const UserState& right) const;
    [[nodiscard]] WorkspaceGridPlacement normalisePlacement(const Entry& entry,
                                                            WorkspaceGridPlacement placement) const;
    [[nodiscard]] bool placementIsFree(WorkspaceModuleId movingId,
                                       const WorkspaceGridPlacement& placement) const;
    [[nodiscard]] std::optional<WorkspaceGridPlacement> nearestFreePlacement(
        const Entry& entry,
        WorkspaceGridPlacement origin,
        WorkspaceGridDirection direction) const;
    [[nodiscard]] bool entriesShareTile(const Entry& left, const Entry& right) const;
    void setModulePlacedInternal(Entry& entry, bool placed);
    void placeNewModuleBelowRoot(Entry& entry);
    void setTilePlacement(WorkspaceModuleId id, WorkspaceGridPlacement placement);
    void setTileCustomized(WorkspaceModuleId id);
    void removeModuleFromTabStack(WorkspaceModuleId id);
    [[nodiscard]] TabStack* findTabStack(const std::string& id);
    [[nodiscard]] const TabStack* findTabStack(const std::string& id) const;
    [[nodiscard]] std::string tileIdForModule(WorkspaceModuleId id) const;
    [[nodiscard]] FloatingTile* findFloatingTileForModule(WorkspaceModuleId id);
    [[nodiscard]] const FloatingTile* findFloatingTileForModule(WorkspaceModuleId id) const;
    [[nodiscard]] bool entryIsFloating(const Entry& entry) const;
    void removeFloatingTileForModule(WorkspaceModuleId id);
    void replaceInactiveTabIfNeeded(const Entry& entry);
    [[nodiscard]] WorkspaceGridMutationResult createOrMergeTabStack(WorkspaceModuleId id,
                                                                    WorkspaceModuleId target);
    [[nodiscard]] bool resolveCollisions(WorkspaceModuleId target,
                                         WorkspaceGridDirection direction);
    void ensureActivePlacements();
    void reflowDefaultPlacements();
    void fillLocalHole(WorkspaceGridPlacement hole, WorkspaceModuleId protectedTarget);
    void compactGridTracks(std::optional<WorkspaceModuleId> excludedModule = std::nullopt);
    void fillVacatedTile(WorkspaceGridPlacement hole, WorkspaceModuleId excludedModule);
    void fillAllGridHoles();
    [[nodiscard]] WorkspaceGridMutationResult commitMutation(const UserState& before);

    static constexpr int gridColumns = 8;

    std::vector<Entry> entries;
    WorkspaceGridPlacement corePlacement { 0, 0, { 3, 2 } };
    std::vector<TabStack> stacks;
    std::vector<InertModule> inert;
    std::vector<FloatingTile> floating;
    std::vector<std::string> opaqueFloatingRecords;
    std::vector<int> columnTrackWeights;
    std::vector<int> rowTrackWeights;
    int nextStackSequence = 1;
    std::string presetId = "current";
    std::string opaqueTopLevelJson = "{}";
    std::vector<UserState> undoHistory;
    std::vector<UserState> redoHistory;
    // One in-session close can be reversed structurally without undoing the
    // user's command history. Other layout edits retain the safe root fallback.
    std::optional<ClosedDock> lastClosedDock;
};
