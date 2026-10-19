#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/Renderer/RendererVisualStyle.h"
#include "WorkspaceModuleLayout.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
struct WorkspaceFocusIdentity
{
    std::string placementId;
    std::string tabStackId;
    std::string tabInstanceId;
};

enum class WorkspaceFloatingWindowCommand
{
    redock,
    remove,
    togglePlayback
};

class WorkspaceModuleHostComponent final : public juce::Component,
                                           private juce::Timer
{
public:
    using Factory = std::function<std::unique_ptr<juce::Component>()>;

    WorkspaceModuleHostComponent();
    ~WorkspaceModuleHostComponent() override;

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Install before registration. Existing resources retain their creation
    // generation even when Main begins a new document before destroying us.
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept;
    void observeLifetime(OptionalLifecycleToken token) noexcept
    {
        lifecycleToken = std::move(token);
    }
#endif

    void registerModule(WorkspaceModuleDescriptor descriptor,
                        int preferredWidth,
                        Factory factory);
    // Adopts only layout/parentage; the caller retains the view and its model.
    // Registration alone must not reparent or show it before availability.
    void registerBorrowedModule(WorkspaceModuleDescriptor descriptor,
                                int preferredWidth,
                                juce::Component& view,
                                bool initiallyPlaced = true);
    void setModuleAvailable(WorkspaceModuleId id, bool available);
    void setModuleEnabled(WorkspaceModuleId id, bool enabled);
    [[nodiscard]] bool isModuleAvailable(WorkspaceModuleId id) const;
    [[nodiscard]] bool isModuleEnabled(WorkspaceModuleId id) const;
    [[nodiscard]] bool isModuleActive(WorkspaceModuleId id) const;
    // Canonical enablement/availability/placement, independent of Simple's view filter.
    [[nodiscard]] bool isModuleActiveInUserLayout(WorkspaceModuleId id) const;
    [[nodiscard]] bool isModuleVisible(WorkspaceModuleId id) const;
    [[nodiscard]] bool hasActiveModules() const;
    [[nodiscard]] int getPreferredWidth() const;
    [[nodiscard]] juce::Component* getModuleComponent(WorkspaceModuleId id) const;
    [[nodiscard]] juce::Rectangle<int> getCoreBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getModuleTileBounds(WorkspaceModuleId id) const noexcept;
    [[nodiscard]] int splitHandleCountForTesting() const noexcept;
    [[nodiscard]] bool resizeSplitBoundaryForTesting(bool vertical,
                                                     int leadingTrack,
                                                     int deltaPixels);
    [[nodiscard]] std::optional<juce::Rectangle<int>> getDragPreviewBoundsForTesting() const noexcept;
    [[nodiscard]] juce::String getDragPreviewLabelForTesting() const;
    void setPrivilegedCoreComponent(juce::Component* component);
    [[nodiscard]] WorkspaceGridMutationResult floatModule(WorkspaceModuleId id);
    [[nodiscard]] WorkspaceGridMutationResult redockModule(WorkspaceModuleId id);
    [[nodiscard]] WorkspaceGridMutationResult removeFloatingTile(WorkspaceModuleId id);
    [[nodiscard]] WorkspaceGridMutationResult closeModule(WorkspaceModuleId id);
    [[nodiscard]] bool isModuleFloating(WorkspaceModuleId id) const;
    void revalidateFloatingWindows();
    [[nodiscard]] int floatingWindowCountForTesting() const noexcept;
    void setVisualDensity(RendererDensity density);
    [[nodiscard]] RendererDensity getVisualDensity() const noexcept { return visualDensity; }
    void setModuleVisualSize(WorkspaceModuleId id, RendererMagSize size);
    [[nodiscard]] RendererMagSize getModuleVisualSize(WorkspaceModuleId id) const noexcept;
    void setPresentationReady(bool ready);
    // A view filter, not module enablement or another persisted layout.
    void setSimpleView(bool enabled);
    [[nodiscard]] bool isSimpleView() const noexcept { return simpleView; }
    [[nodiscard]] bool isVisualMotionActive() const noexcept { return visualMotion.isActive(); }
    [[nodiscard]] WorkspaceGridMutationResult moveModule(WorkspaceModuleId id,
                                                         WorkspaceGridPlacement placement,
                                                         WorkspaceGridDirection direction);
    [[nodiscard]] WorkspaceGridMutationResult resizeModule(WorkspaceModuleId id,
                                                           WorkspaceGridSpan span,
                                                           WorkspaceGridDirection direction);
    [[nodiscard]] WorkspaceGridMutationResult dockModule(WorkspaceModuleId id,
                                                         WorkspaceModuleId target,
                                                         WorkspaceDockZone zone);
    [[nodiscard]] WorkspaceGridMutationResult dockModuleToGroup(
        WorkspaceModuleId id,
        std::vector<WorkspaceModuleId> targets,
        WorkspaceDockZone zone);
    [[nodiscard]] bool canUndoUserLayout() const noexcept;
    bool undoUserLayout();
    bool redoUserLayout();
    [[nodiscard]] WorkspaceGridMutationResult activateTab(WorkspaceModuleId id);
    bool activateAdjacentTab(bool reverse);
    [[nodiscard]] bool hasFocusableVisibleModule() const noexcept;
    bool focusActiveModule();
    // A completed Float command owns this specific projection's focus. The
    // generic reading-order fallback can select another Mag in the main window.
    bool focusFloatingModule(WorkspaceModuleId id);
    [[nodiscard]] std::optional<WorkspaceFocusIdentity> currentFocusIdentity() const;

    [[nodiscard]] juce::String serialiseUserState() const;
    WorkspaceRestoreResult restoreUserState(const juce::String& state);
    void resetUserLayout();
    void releaseInactiveViewsForMemoryPressure();
    [[nodiscard]] int inactiveResidentViewCount() const noexcept;
    [[nodiscard]] bool isInactiveResidencyMaintenanceRunning() const noexcept
    {
        return isTimerRunning();
    }

    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void lookAndFeelChanged() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool cancelPointerGesture();

    std::function<void()> onLayoutChanged;
    std::function<void()> onUserLayoutCommitted;
    std::function<bool(WorkspaceModuleId)> onBeforeModuleViewCreated;
    std::function<void(WorkspaceModuleId, bool)> onModuleVisibilityChanged;
    std::function<void(WorkspaceModuleId)> onModuleCloseRequested;
    std::function<void(WorkspaceModuleId, WorkspaceFloatingWindowCommand)>
        onFloatingWindowCommandRequested;

private:
    enum class SplitAxis { vertical, horizontal };

    class SplitHandle final : public juce::Component
    {
    public:
        SplitHandle(SplitAxis axisToUse, int leadingTrackToUse, int trailingTrackToUse);
        std::function<void()> onDragStarted;
        std::function<void(int, int)> onDragged;
        std::function<void()> onDragEnded;

        void paint(juce::Graphics& g) override;
        void mouseEnter(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent&) override;
        [[nodiscard]] SplitAxis getAxis() const noexcept { return axis; }
        [[nodiscard]] int getLeadingTrack() const noexcept { return leadingTrack; }
        [[nodiscard]] int getTrailingTrack() const noexcept { return trailingTrack; }

    private:
        SplitAxis axis;
        int leadingTrack = 0;
        int trailingTrack = 0;
        bool hovered = false;
        bool dragging = false;
        int dragStartScreenX = 0;
        int dragStartScreenY = 0;
    };

    struct Registration
    {
        WorkspaceModuleId id;
        int preferredWidth = 154;
        Factory factory;
        std::unique_ptr<juce::Component> ownedComponent;
        juce::Component::SafePointer<juce::Component> component;
        juce::Component::SafePointer<juce::Component> borrowedView;
        juce::Component::SafePointer<juce::Component> borrowedHomeParent;
        bool visible = false;
        bool inactiveResident = false;
        double inactiveSinceMs = 0.0;
        std::uint64_t lastUsedSequence = 0;
        RendererMagSize visualSize = RendererMagSize::automatic;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        OptionalLifecycleToken lifecycleToken;
#endif
    };

    struct InertView
    {
        WorkspaceInertModuleInfo state;
        std::unique_ptr<juce::Component> component;
    };

    class FloatingWindow;

    struct FloatingProjection
    {
        std::string tileId;
        WorkspaceModuleId commandTarget = WorkspaceModuleId::coreWaveform;
        std::unique_ptr<FloatingWindow> window;
    };

    struct ProjectedTile
    {
        WorkspaceModuleId id = WorkspaceModuleId::coreWaveform;
        WorkspaceGridPlacement placement;
        std::string tabStackId;
        std::vector<WorkspaceModuleId> tabMembers;
        juce::Rectangle<int> bounds;
    };

    struct ProjectedSplit
    {
        SplitAxis axis = SplitAxis::vertical;
        int leadingTrack = 0;
        int trailingTrack = 0;
        juce::Rectangle<int> bounds;
    };

    struct MosaicProjection
    {
        std::vector<ProjectedTile> tiles;
        std::vector<ProjectedSplit> splits;
        std::vector<int> columnTracks;
        std::vector<int> rowTracks;
        std::vector<int> columnExtents;
        std::vector<int> rowExtents;
    };

    enum class MosaicEdge { north, east, south, west };

    struct DropProposal
    {
        WorkspaceModuleId moving = WorkspaceModuleId::coreWaveform;
        WorkspaceModuleId target = WorkspaceModuleId::coreWaveform;
        std::vector<WorkspaceModuleId> targets;
        WorkspaceDockZone zone = WorkspaceDockZone::centre;
        std::vector<ProjectedTile> projectedTiles;
        juce::Rectangle<int> targetGroupBounds;
        juce::Rectangle<int> destinationBounds;
        juce::String label;
    };

    class DockedTabStrip;
    class MagCloseButton;

    Registration* findRegistration(WorkspaceModuleId id);
    const Registration* findRegistration(WorkspaceModuleId id) const;
    std::optional<WorkspaceModuleId> moduleAt(juce::Point<int> point) const;
    std::optional<WorkspaceModuleId> tabModuleFromEvent(const juce::MouseEvent& event) const;
    const ProjectedTile* projectedTileAt(juce::Point<int> point) const noexcept;
    std::optional<MosaicEdge> projectedEdgeForTile(const ProjectedTile& tile) const noexcept;
    WorkspaceDockZone nearestCoreEdge(juce::Point<int> point) const noexcept;
    std::optional<DropProposal> dropProposalAt(WorkspaceModuleId moving,
                                                juce::Point<int> point) const;
    WorkspaceDockZone dockZoneAt(juce::Rectangle<int> bounds, juce::Point<int> point) const;
    juce::Rectangle<int> boundsForPlacement(const WorkspaceGridPlacement& placement) const;
    void rebuildMosaicProjection();
    [[nodiscard]] MosaicProjection buildMosaicProjection(
        const WorkspaceModuleLayout& sourceLayout) const;
    [[nodiscard]] juce::Rectangle<int> projectedBoundsForModule(WorkspaceModuleId id) const noexcept;
    [[nodiscard]] int preferredWidthForTile(const WorkspaceGridItem& item) const noexcept;
    [[nodiscard]] RendererMagSize visualSizeForTile(const WorkspaceGridItem& item) const noexcept;
    [[nodiscard]] int mainAxisWeightForTile(const WorkspaceGridItem& item) const noexcept;
    [[nodiscard]] int crossAxisExtentForTile(const WorkspaceGridItem& item,
                                             MosaicEdge edge) const noexcept;
    void synchroniseDockedTabStrips();
    void synchroniseMagCloseButtons();
    void synchroniseSplitHandles();
    void handleDockedTab(const std::string& instanceId);
    void applyVisualContractToComponent(Registration& registration);
    void beginVisualTransition();
    void applyVisualMotionFrame();
    void refreshTimerPolicy();
    void synchroniseComponents();
    void releaseView(Registration& registration);
    void synchroniseInertViews();
    void synchroniseFloatingWindows();
    void destroyFloatingWindows();
    void detachFloatingContent(FloatingProjection& projection);
    void updateFloatingPlacementFromWindow(const std::string& tileId,
                                           juce::Rectangle<int> bounds);
    [[nodiscard]] std::vector<WorkspaceDisplaySnapshot> currentDisplaySnapshots() const;
    [[nodiscard]] WorkspaceLogicalBounds mainWindowLogicalBounds() const;
    [[nodiscard]] juce::Component* visibleComponentForFloatingTile(
        const WorkspaceFloatingTileInfo& tile) const;
    void handleFloatingClose(WorkspaceModuleId id, bool privilegedCore);
    void handleFloatingRedock(WorkspaceModuleId id);
    void handleFloatingTab(WorkspaceModuleId id);
    void trimInactiveResidency(bool memoryPressure);
    void timerCallback() override;
    void beginSplitResize(SplitAxis axis, int leadingTrack, int trailingTrack);
    void updateSplitResize(int screenDeltaX, int screenDeltaY);
    void finishSplitResize();
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

    static constexpr int titleDragHeight = 42;

    WorkspaceModuleLayout layout;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext;
    OptionalLifecycleToken lifecycleToken;
#endif
    bool simpleView = false;
    std::optional<WorkspaceModuleLayout> simpleProjection;
    [[nodiscard]] const WorkspaceModuleLayout& visibleLayout() const noexcept
    {
        return simpleProjection ? *simpleProjection : layout;
    }
    void refreshSceneProjection();
    std::vector<Registration> registrations;
    std::vector<InertView> inertViews;
    std::vector<ProjectedTile> projectedTiles;
    std::vector<ProjectedSplit> projectedSplits;
    std::vector<int> projectedColumnTracks;
    std::vector<int> projectedRowTracks;
    std::vector<int> projectedColumnExtents;
    std::vector<int> projectedRowExtents;
    std::vector<std::unique_ptr<SplitHandle>> splitHandles;
    std::vector<std::unique_ptr<DockedTabStrip>> dockedTabStrips;
    std::vector<std::unique_ptr<MagCloseButton>> magCloseButtons;
    juce::Component* privilegedCoreComponent = nullptr;
    juce::Component* privilegedCoreHomeParent = nullptr;
    std::vector<FloatingProjection> floatingProjections;
    juce::Rectangle<int> coreBounds;
    std::uint64_t nextUseSequence = 1;
    std::optional<WorkspaceModuleId> draggingModule;
    std::optional<DropProposal> dragDropProposal;
    std::optional<SplitAxis> resizingSplitAxis;
    int resizingLeadingTrack = -1;
    int resizingTrailingTrack = -1;
    std::vector<int> splitResizeStartColumnExtents;
    std::vector<int> splitResizeStartRowExtents;
    std::vector<int> splitResizePreviewColumnExtents;
    std::vector<int> splitResizePreviewRowExtents;
    RendererDensity visualDensity = RendererDensity::standard;
    int moduleGap = 10;
    RendererMotionController visualMotion;
    bool presentationReady = false;
    bool residencyTimerNeeded = false;
    double lastMotionTickMs = 0.0;

    static constexpr int maximumInactiveResidentViews = 2;
    static constexpr std::size_t maximumInactiveResidentBytes = 4 * 1024 * 1024;
    static constexpr double maximumInactiveResidentAgeMs = 15000.0;
};
} // namespace singleeditor
