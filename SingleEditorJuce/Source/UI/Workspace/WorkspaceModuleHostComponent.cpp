#include "WorkspaceModuleHostComponent.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>

#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"
#include "UI/Workspace/WorkspaceVisualContractTarget.h"
#include "App/WindowsWindowShell.h"

namespace singleeditor
{
namespace
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
OptionalLifecycleOwner lifecycleOwner(WorkspaceModuleId id) noexcept
{
    switch (id)
    {
        case WorkspaceModuleId::coreWaveform: return OptionalLifecycleOwner::coreFile;
        case WorkspaceModuleId::outputMeter: return OptionalLifecycleOwner::outputMeter;
        case WorkspaceModuleId::ambisonicEnergy: return OptionalLifecycleOwner::ambisonics;
        case WorkspaceModuleId::parametricEq: return OptionalLifecycleOwner::parametricEq;
        case WorkspaceModuleId::truePeakLimiter: return OptionalLifecycleOwner::truePeakLimiter;
        case WorkspaceModuleId::playlist: return OptionalLifecycleOwner::workspace;
        case WorkspaceModuleId::metadata: return OptionalLifecycleOwner::metadata;
        case WorkspaceModuleId::markers: return OptionalLifecycleOwner::markers;
        case WorkspaceModuleId::spatialEnergy: return OptionalLifecycleOwner::spatialEnergy;
    }
    return OptionalLifecycleOwner::workspace;
}
#endif
const char* accessibleModuleTitle(WorkspaceModuleId id) noexcept
{
    switch (id)
    {
        case WorkspaceModuleId::parametricEq: return "Parametric EQ";
        case WorkspaceModuleId::truePeakLimiter: return "True-Peak Limiter";
        case WorkspaceModuleId::outputMeter: return "Output Meter";
        case WorkspaceModuleId::ambisonicEnergy: return "Ambisonic Energy";
        case WorkspaceModuleId::spatialEnergy: return "Rendered Output Spatial Energy";
        case WorkspaceModuleId::coreWaveform: return "Audio Waveform";
        case WorkspaceModuleId::playlist: return "Playlist";
        case WorkspaceModuleId::metadata: return "Metadata inspector";
        case WorkspaceModuleId::markers: return "Markers";
    }
    return "Workspace Module";
}

const char* accessibleModuleKey(WorkspaceModuleId id) noexcept
{
    switch (id)
    {
        case WorkspaceModuleId::parametricEq: return "parametric-eq";
        case WorkspaceModuleId::truePeakLimiter: return "true-peak-limiter";
        case WorkspaceModuleId::outputMeter: return "output-meter";
        case WorkspaceModuleId::ambisonicEnergy: return "ambisonic-energy";
        case WorkspaceModuleId::spatialEnergy: return "spatial-energy";
        case WorkspaceModuleId::coreWaveform: return "core-waveform";
        case WorkspaceModuleId::playlist: return "playlist";
        case WorkspaceModuleId::metadata: return "metadata";
        case WorkspaceModuleId::markers: return "markers";
    }
    return "unknown";
}

class InertWorkspaceModuleComponent final : public juce::Component
{
public:
    explicit InertWorkspaceModuleComponent(const WorkspaceInertModuleInfo& state)
        : title(juce::String::fromUTF8(state.moduleTypeId.c_str())), quarantined(state.quarantined)
    {
        setInterceptsMouseClicks(true, false);
        setWantsKeyboardFocus(true);
        setComponentID("workspace.module.inert." + title);
        setHasFocusOutline(!RendererVisualStyle::isClassic());
        setTitle(quarantined ? "Quarantined workspace module" : "Unavailable workspace module");
        setDescription(title);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(RendererVisualStyle::panelBackground());
        g.fillRoundedRectangle(bounds, RendererVisualStyle::cardRadius());
        g.setColour((quarantined ? RendererVisualStyle::danger()
                                 : RendererVisualStyle::secondaryText())
                        .withAlpha(0.72f));
        g.drawRoundedRectangle(bounds,
                               RendererVisualStyle::cardRadius(),
                               RendererVisualStyle::magBorderWidth());
        g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::bodyTextSize(),
                                               juce::Font::bold)));
        g.drawText(quarantined ? "Module state quarantined" : "Module unavailable",
                   getLocalBounds().reduced(14).removeFromTop(22),
                   juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::monoTextSize())));
        g.drawText(title,
                   getLocalBounds().reduced(14).withTrimmedTop(26),
                   juce::Justification::centredLeft,
                   true);
    }

private:
    juce::String title;
    bool quarantined = false;
};
}

class WorkspaceModuleHostComponent::FloatingWindow final : public juce::DocumentWindow
{
public:
    class Content final : public juce::Component
    {
    public:
        Content()
        {
            setComponentID("workspace.floating.content");
            setTitle("Floating workspace tile");
            setWantsKeyboardFocus(true);
            redockButton.setButtonText("Redock");
            redockButton.setTitle("Redock this tile");
            redockButton.onClick = [this]
            {
                if (onRedock)
                    onRedock();
            };
            addAndMakeVisible(redockButton);
        }

        void setHostedComponent(juce::Component* next)
        {
            if (hosted == next)
                return;
            if (hosted != nullptr)
                removeChildComponent(hosted);
            hosted = next;
            if (hosted != nullptr)
                addAndMakeVisible(*hosted);
            resized();
        }

        juce::Component* detachHostedComponent()
        {
            auto* result = hosted;
            if (hosted != nullptr)
                removeChildComponent(hosted);
            hosted = nullptr;
            return result;
        }

        void setTabs(const std::vector<WorkspaceModuleId>& members,
                     WorkspaceModuleId active,
                     std::function<void(WorkspaceModuleId)> activate)
        {
            tabButtons.clear();
            if (members.size() < 2)
            {
                resized();
                return;
            }
            for (const auto member : members)
            {
                auto button = std::make_unique<juce::TextButton>(accessibleModuleTitle(member));
                button->setComponentID("workspace.floating.tab."
                                       + juce::String(accessibleModuleKey(member)));
                button->setToggleState(member == active, juce::dontSendNotification);
                button->setClickingTogglesState(false);
                button->onClick = [activate, member]
                {
                    if (activate)
                        activate(member);
                };
                addAndMakeVisible(*button);
                tabButtons.push_back(std::move(button));
            }
            resized();
        }

        void resized() override
        {
            auto area = getLocalBounds();
            auto header = area.removeFromTop(34).reduced(6, 4);
            redockButton.setBounds(header.removeFromRight(78));
            if (!tabButtons.empty())
            {
                header.removeFromRight(8);
                const auto width = std::max(1, header.getWidth() / static_cast<int>(tabButtons.size()));
                for (size_t index = 0; index < tabButtons.size(); ++index)
                    tabButtons[index]->setBounds(header.removeFromLeft(
                        index + 1 == tabButtons.size() ? header.getWidth() : width));
            }
            if (hosted != nullptr)
                hosted->setBounds(area.reduced(6, 2).withTrimmedBottom(4));
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(RendererVisualStyle::panelBackground());
            g.setColour(RendererVisualStyle::borderColour());
            g.drawHorizontalLine(33, 0.0f, static_cast<float>(getWidth()));
        }

        std::function<void()> onRedock;

    private:
        juce::TextButton redockButton;
        std::vector<std::unique_ptr<juce::TextButton>> tabButtons;
        juce::Component* hosted = nullptr;
    };

    FloatingWindow(juce::String title, bool privileged)
        : juce::DocumentWindow(std::move(title),
                               RendererVisualStyle::panelBackground(),
                               juce::DocumentWindow::allButtons,
                               false),
          privilegedCore(privileged),
          content(std::make_unique<Content>())
    {
        WindowsWindowShell().configureWindow(*this);
        setResizable(true, false);
        setResizeLimits(240, 160, 4096, 4096);
        setWantsKeyboardFocus(true);
        setContentNonOwned(content.get(), false);
        addToDesktop(getDesktopWindowStyleFlags());
        scaleNotifier = std::make_unique<juce::NativeScaleFactorNotifier>(
            this,
            [this] (float)
            {
                if (onScaleFactorChanged)
                    onScaleFactorChanged();
            });
        content->onRedock = [this]
        {
            if (onRedock)
                onRedock();
        };
    }

    ~FloatingWindow() override
    {
        scaleNotifier.reset();
        clearContentComponent();
    }

    void setHostedComponent(juce::Component* component,
                            const std::vector<WorkspaceModuleId>& members,
                            WorkspaceModuleId active)
    {
        content->setHostedComponent(component);
        content->setTabs(members, active, onActivateTab);
    }

    juce::Component* detachHostedComponent()
    {
        return content->detachHostedComponent();
    }

    void setBoundsWithoutFeedback(juce::Rectangle<int> bounds)
    {
        const juce::ScopedValueSetter<bool> guard(suppressBoundsFeedback, true);
        setBounds(bounds);
    }

    WindowControlKind findControlAtPoint(juce::Point<float> point) const override
    {
        return WindowsWindowShell::findControlAtPoint(*this, point);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose(privilegedCore);
    }

    void moved() override
    {
        juce::DocumentWindow::moved();
        sendBoundsFeedback();
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        sendBoundsFeedback();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey && onTogglePlayback)
        {
            onTogglePlayback();
            return true;
        }
        return juce::DocumentWindow::keyPressed(key);
    }

    std::function<void(bool)> onClose;
    std::function<void()> onRedock;
    std::function<void()> onTogglePlayback;
    std::function<void(WorkspaceModuleId)> onActivateTab;
    std::function<void(juce::Rectangle<int>)> onBoundsChanged;
    std::function<void()> onScaleFactorChanged;

private:
    void sendBoundsFeedback()
    {
        if (!suppressBoundsFeedback && onBoundsChanged)
            onBoundsChanged(getBounds());
    }

    bool privilegedCore = false;
    bool suppressBoundsFeedback = false;
    std::unique_ptr<Content> content;
    std::unique_ptr<juce::NativeScaleFactorNotifier> scaleNotifier;
};

class WorkspaceModuleHostComponent::DockedTabStrip final : public juce::Component
{
public:
    explicit DockedTabStrip(std::string stackIdToUse)
        : stackId(std::move(stackIdToUse))
    {
        setComponentID("workspace.docked.tab-strip." + juce::String::fromUTF8(stackId.c_str()));
        setTitle("Workspace Tab Stack");
        setDescription("Switches the visible Mag in this shared tile.");
        setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    }

    [[nodiscard]] const std::string& getStackId() const noexcept { return stackId; }

    void update(const std::vector<std::string>& nextIds,
                const std::string& activeId,
                std::function<juce::String(const std::string&)> labelForId,
                std::function<void(const std::string&)> activate)
    {
        if (!onActivate)
            onActivate = std::move(activate);
        if (nextIds != ids)
        {
            buttons.clear();
            ids = nextIds;
            for (const auto& id : ids)
            {
                auto button = std::make_unique<juce::TextButton>(labelForId(id));
                button->setComponentID("workspace.docked.tab." + juce::String::fromUTF8(id.c_str()));
                button->setTitle("Show " + labelForId(id));
                button->setClickingTogglesState(false);
                // Activate on press so an inactive tab becomes the concrete
                // drag source before Button::mouseUp can compete with the
                // Host's tear-off transaction. A click still switches tabs;
                // a drag can now detach either the active or inactive tab.
                button->setTriggeredOnMouseDown(true);
                button->setWantsKeyboardFocus(true);
                button->onClick = [this, id]
                {
                    if (onActivate)
                        onActivate(id);
                };
                addAndMakeVisible(*button);
                buttons.push_back(std::move(button));
            }
        }

        for (size_t index = 0; index < buttons.size(); ++index)
            buttons[index]->setToggleState(ids[index] == activeId, juce::dontSendNotification);
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        if (buttons.empty())
            return;
        const auto baseWidth = std::max(1, area.getWidth() / static_cast<int>(buttons.size()));
        for (size_t index = 0; index < buttons.size(); ++index)
            buttons[index]->setBounds(area.removeFromLeft(
                index + 1 == buttons.size() ? area.getWidth() : baseWidth));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(RendererVisualStyle::mutedSurface());
        g.setColour(RendererVisualStyle::borderColour());
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

private:
    std::string stackId;
    std::vector<std::string> ids;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    std::function<void(const std::string&)> onActivate;
};

class WorkspaceModuleHostComponent::MagCloseButton final : public juce::Button
{
public:
    explicit MagCloseButton(WorkspaceModuleId moduleToUse)
        : juce::Button("Close " + juce::String(accessibleModuleTitle(moduleToUse))),
          module(moduleToUse)
    {
        setComponentID("workspace.close."
            + juce::String(accessibleModuleKey(moduleToUse)));
        setTitle("Close " + juce::String(accessibleModuleTitle(moduleToUse)));
        setTooltip("Close this Mag. It can be reopened from the toolbar.");
        setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    [[nodiscard]] WorkspaceModuleId getModule() const noexcept { return module; }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto area = getLocalBounds().toFloat().reduced(2.0f);
        if (highlighted || hasKeyboardFocus(true))
        {
            g.setColour(RendererVisualStyle::mutedSurface().withAlpha(down ? 0.96f : 0.82f));
            g.fillEllipse(area);
        }
        g.setColour((highlighted ? RendererVisualStyle::primaryText()
                                 : RendererVisualStyle::secondaryText())
                        .withAlpha(highlighted ? 0.92f : 0.38f));
        const auto inset = highlighted ? 6.0f : 7.0f;
        const auto mark = getLocalBounds().toFloat().reduced(inset);
        g.drawLine(mark.getX(), mark.getY(), mark.getRight(), mark.getBottom(), 1.35f);
        g.drawLine(mark.getRight(), mark.getY(), mark.getX(), mark.getBottom(), 1.35f);
    }

private:
    WorkspaceModuleId module;
};

WorkspaceModuleHostComponent::WorkspaceModuleHostComponent()
{
    moduleGap = RendererVisualStyle::gapForDensity(visualDensity);
    setInterceptsMouseClicks(false, true);
    setWantsKeyboardFocus(true);
    setComponentID("workspace.modules");
    setTitle("Workspace modules");
    setDescription("Visible optional audio tools and Tab Stacks.");
    setHelpText("Control+Tab changes the active Tab. F6 moves between major regions.");
}

WorkspaceModuleHostComponent::~WorkspaceModuleHostComponent()
{
    stopTimer();
    destroyFloatingWindows();
    for (auto& registration : registrations)
        releaseView(registration);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    lifecycleToken.release();
#endif
}

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
void WorkspaceModuleHostComponent::setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
{
    if (!registrations.empty())
    {
        if (context.observer) context.observer->fail(OptionalLifecycleFailure::invalidContext);
        if (optionalLifecycleContext.observer) optionalLifecycleContext.observer->fail(OptionalLifecycleFailure::invalidContext);
        return;
    }
    optionalLifecycleContext = context;
}
#endif

void WorkspaceModuleHostComponent::setVisualDensity(RendererDensity density)
{
    if (visualDensity == density)
        return;

    visualDensity = density;
    moduleGap = RendererVisualStyle::gapForDensity(density);
    for (auto& registration : registrations)
        applyVisualContractToComponent(registration);
    resized();
    if (onLayoutChanged)
        onLayoutChanged();
    beginVisualTransition();
    repaint();
}

void WorkspaceModuleHostComponent::setModuleVisualSize(WorkspaceModuleId id,
                                                       RendererMagSize size)
{
    auto* registration = findRegistration(id);
    if (registration == nullptr || registration->visualSize == size)
        return;

    registration->visualSize = size;
    resized();
    if (onLayoutChanged)
        onLayoutChanged();
    beginVisualTransition();
    repaint();
}

RendererMagSize WorkspaceModuleHostComponent::getModuleVisualSize(WorkspaceModuleId id) const noexcept
{
    if (const auto* registration = findRegistration(id))
        return registration->visualSize;
    return RendererMagSize::automatic;
}

void WorkspaceModuleHostComponent::setPresentationReady(bool ready)
{
    if (presentationReady == ready)
        return;

    presentationReady = ready;
    if (ready)
    {
        synchroniseComponents();
        beginVisualTransition();
    }
    else
    {
        destroyFloatingWindows();
        visualMotion.snapTo(1.0f);
        applyVisualMotionFrame();
        refreshTimerPolicy();
    }
}

WorkspaceModuleHostComponent::SplitHandle::SplitHandle(SplitAxis axisToUse,
                                                       int leadingTrackToUse,
                                                       int trailingTrackToUse)
    : axis(axisToUse),
      leadingTrack(leadingTrackToUse),
      trailingTrack(trailingTrackToUse)
{
    setMouseCursor(axis == SplitAxis::vertical
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::UpDownResizeCursor);
    setAlwaysOnTop(true);
    setWantsKeyboardFocus(false);
    setTitle("Resize adjacent workspace regions");
}

void WorkspaceModuleHostComponent::SplitHandle::paint(juce::Graphics& g)
{
    if (!hovered)
        return;

    g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.56f));
    const auto bounds = getLocalBounds().toFloat();
    if (axis == SplitAxis::vertical)
        g.drawVerticalLine(juce::roundToInt(bounds.getCentreX()),
                           bounds.getY() + 2.0f, bounds.getBottom() - 2.0f);
    else
        g.drawHorizontalLine(juce::roundToInt(bounds.getCentreY()),
                             bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
}

void WorkspaceModuleHostComponent::SplitHandle::mouseEnter(const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}

void WorkspaceModuleHostComponent::SplitHandle::mouseExit(const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

void WorkspaceModuleHostComponent::SplitHandle::mouseDown(const juce::MouseEvent& event)
{
    dragging = event.mods.isLeftButtonDown();
    if (!dragging)
        return;
    dragStartScreenX = event.getScreenX();
    dragStartScreenY = event.getScreenY();
    if (onDragStarted)
        onDragStarted();
}

void WorkspaceModuleHostComponent::SplitHandle::mouseDrag(const juce::MouseEvent& event)
{
    if (dragging && event.mods.isLeftButtonDown() && onDragged)
        onDragged(event.getScreenX() - dragStartScreenX,
                  event.getScreenY() - dragStartScreenY);
}

void WorkspaceModuleHostComponent::SplitHandle::mouseUp(const juce::MouseEvent&)
{
    if (!dragging)
        return;
    dragging = false;
    if (onDragEnded)
        onDragEnded();
}

void WorkspaceModuleHostComponent::registerModule(WorkspaceModuleDescriptor descriptor,
                                                   int preferredWidth,
                                                   Factory factory)
{
    if (findRegistration(descriptor.id) != nullptr)
        return;

    layout.registerModule(descriptor);
    Registration registration { descriptor.id };
    registration.preferredWidth = std::max(1, preferredWidth);
    registration.factory = std::move(factory);
    registrations.push_back(std::move(registration));
}

void WorkspaceModuleHostComponent::registerBorrowedModule(WorkspaceModuleDescriptor descriptor,
                                                          int preferredWidth,
                                                          juce::Component& view,
                                                          bool initiallyPlaced)
{
    if (findRegistration(descriptor.id) != nullptr)
        return;
    // Borrowed content remains caller-owned even while inactive. Do not count
    // it as a cache whose memory the host can reclaim by deleting the view.
    descriptor.allowInactiveViewResidency = false;
    descriptor.estimatedViewBytes = 0;
    descriptor.restoreDefaultWhenAbsent = true;
    descriptor.defaultPlaced = initiallyPlaced;
    const auto id = descriptor.id;
    registerModule(std::move(descriptor), preferredWidth, {});
    auto* registration = findRegistration(id);
    registration->borrowedView = &view;
    registration->borrowedHomeParent = view.getParentComponent();
}

void WorkspaceModuleHostComponent::setModuleAvailable(WorkspaceModuleId id, bool available)
{
    if (layout.isAvailable(id) == available)
        return;

    layout.setAvailable(id, available);
    synchroniseComponents();
}

void WorkspaceModuleHostComponent::setModuleEnabled(WorkspaceModuleId id, bool enabled)
{
    if (layout.isEnabled(id) == enabled && layout.isPlaced(id) == enabled)
        return;

    const auto result = layout.setModuleOpen(id, enabled);
    if (result != WorkspaceGridMutationResult::committed)
        return;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (findRegistration(id) != nullptr && optionalLifecycleContext.observer)
        optionalLifecycleContext.observer->command(optionalLifecycleContext, lifecycleOwner(id),
            enabled ? OptionalLifecycleAction::moduleEnableRequested : OptionalLifecycleAction::moduleOffRequested);
#endif
    synchroniseComponents();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
}

bool WorkspaceModuleHostComponent::isModuleAvailable(WorkspaceModuleId id) const
{
    return layout.isAvailable(id);
}

bool WorkspaceModuleHostComponent::isModuleEnabled(WorkspaceModuleId id) const
{
    return layout.isEnabled(id) && layout.isPlaced(id);
}

bool WorkspaceModuleHostComponent::isModuleActive(WorkspaceModuleId id) const
{
    return visibleLayout().isActive(id);
}

bool WorkspaceModuleHostComponent::isModuleActiveInUserLayout(WorkspaceModuleId id) const
{
    return layout.isActive(id);
}

bool WorkspaceModuleHostComponent::isModuleVisible(WorkspaceModuleId id) const
{
    return visibleLayout().isModuleVisible(id);
}

bool WorkspaceModuleHostComponent::hasActiveModules() const
{
    const auto active = visibleLayout().activeModules();
    return simpleView || layout.isFloating(WorkspaceModuleId::coreWaveform)
        || std::any_of(active.begin(), active.end(), [this] (WorkspaceModuleId id)
        {
            return !layout.isFloating(id);
        });
}

int WorkspaceModuleHostComponent::getPreferredWidth() const
{
    int preferredWidth = 0;
    for (const auto id : visibleLayout().activeModules())
        if (const auto* registration = findRegistration(id))
            preferredWidth = std::max(preferredWidth, registration->preferredWidth);
    return preferredWidth;
}

juce::Component* WorkspaceModuleHostComponent::getModuleComponent(WorkspaceModuleId id) const
{
    if (const auto* registration = findRegistration(id))
        return registration->component.getComponent();
    return nullptr;
}

juce::Rectangle<int> WorkspaceModuleHostComponent::getCoreBounds() const noexcept
{
    return coreBounds;
}

juce::Rectangle<int> WorkspaceModuleHostComponent::getModuleTileBounds(
    WorkspaceModuleId id) const noexcept
{
    return projectedBoundsForModule(id);
}

int WorkspaceModuleHostComponent::splitHandleCountForTesting() const noexcept
{
    return static_cast<int>(splitHandles.size());
}

bool WorkspaceModuleHostComponent::resizeSplitBoundaryForTesting(bool vertical,
                                                                 int leadingTrack,
                                                                 int deltaPixels)
{
    const auto found = std::find_if(projectedSplits.begin(), projectedSplits.end(),
                                    [vertical, leadingTrack] (const auto& split)
    {
        return split.axis == (vertical ? SplitAxis::vertical : SplitAxis::horizontal)
            && split.leadingTrack == leadingTrack;
    });
    if (found == projectedSplits.end())
        return false;
    const auto before = serialiseUserState();
    beginSplitResize(found->axis, found->leadingTrack, found->trailingTrack);
    updateSplitResize(vertical ? deltaPixels : 0, vertical ? 0 : deltaPixels);
    finishSplitResize();
    return serialiseUserState() != before;
}

std::optional<juce::Rectangle<int>>
WorkspaceModuleHostComponent::getDragPreviewBoundsForTesting() const noexcept
{
    if (dragDropProposal.has_value() && !dragDropProposal->destinationBounds.isEmpty())
        return dragDropProposal->destinationBounds;
    return std::nullopt;
}

juce::String WorkspaceModuleHostComponent::getDragPreviewLabelForTesting() const
{
    return dragDropProposal.has_value() ? dragDropProposal->label : juce::String {};
}

void WorkspaceModuleHostComponent::setPrivilegedCoreComponent(juce::Component* component)
{
    if (privilegedCoreComponent == component)
        return;
    if (layout.isFloating(WorkspaceModuleId::coreWaveform))
        destroyFloatingWindows();
    privilegedCoreComponent = component;
    privilegedCoreHomeParent = component != nullptr ? component->getParentComponent() : nullptr;
    synchroniseFloatingWindows();
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::floatModule(WorkspaceModuleId id)
{
    if (!presentationReady
        || (id == WorkspaceModuleId::coreWaveform && privilegedCoreComponent == nullptr))
    {
        return WorkspaceGridMutationResult::moduleUnavailable;
    }

    const auto displays = currentDisplaySnapshots();
    if (displays.empty())
        return WorkspaceGridMutationResult::invalidPlacement;
    const auto mainBounds = mainWindowLogicalBounds();
    const auto centre = WorkspaceLogicalBounds {
        mainBounds.x + std::max(24, (mainBounds.width - 640) / 2),
        mainBounds.y + std::max(24, (mainBounds.height - 420) / 2),
        std::min(640, std::max(240, mainBounds.width - 48)),
        std::min(420, std::max(160, mainBounds.height - 48))
    };
    const auto mainCentreX = mainBounds.x + mainBounds.width / 2;
    const auto mainCentreY = mainBounds.y + mainBounds.height / 2;
    const auto selected = std::find_if(displays.begin(), displays.end(), [&] (const WorkspaceDisplaySnapshot& item)
    {
        return mainCentreX >= item.workArea.x && mainCentreX < item.workArea.right()
            && mainCentreY >= item.workArea.y && mainCentreY < item.workArea.bottom();
    });
    const auto& target = selected != displays.end() ? *selected : displays.front();
    const auto result = layout.floatModule(id,
        WorkspaceFloatingWindowModel::capture(centre, target));
    if (result == WorkspaceGridMutationResult::committed)
    {
        synchroniseComponents();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::redockModule(WorkspaceModuleId id)
{
    const auto result = layout.redockModule(id);
    if (result == WorkspaceGridMutationResult::committed)
    {
        synchroniseComponents();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::removeFloatingTile(WorkspaceModuleId id)
{
    const auto result = layout.removeFloatingTile(id);
    if (result == WorkspaceGridMutationResult::committed)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (findRegistration(id) != nullptr && optionalLifecycleContext.observer)
            optionalLifecycleContext.observer->command(optionalLifecycleContext, lifecycleOwner(id),
                OptionalLifecycleAction::moduleRemoveRequested);
#endif
        synchroniseComponents();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::closeModule(WorkspaceModuleId id)
{
    const auto result = layout.setModuleOpen(id, false);
    if (result == WorkspaceGridMutationResult::committed)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (findRegistration(id) != nullptr && optionalLifecycleContext.observer)
            optionalLifecycleContext.observer->command(optionalLifecycleContext, lifecycleOwner(id),
                OptionalLifecycleAction::moduleRemoveRequested);
#endif
        synchroniseComponents();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

bool WorkspaceModuleHostComponent::isModuleFloating(WorkspaceModuleId id) const
{
    return layout.isFloating(id);
}

void WorkspaceModuleHostComponent::revalidateFloatingWindows()
{
    synchroniseFloatingWindows();
}

int WorkspaceModuleHostComponent::floatingWindowCountForTesting() const noexcept
{
    return static_cast<int>(floatingProjections.size());
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::moveModule(
    WorkspaceModuleId id,
    WorkspaceGridPlacement placement,
    WorkspaceGridDirection direction)
{
    const auto result = layout.moveModule(id, placement, direction);
    if (result == WorkspaceGridMutationResult::committed)
    {
        resized();
        if (onLayoutChanged)
            onLayoutChanged();
        repaint();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::resizeModule(
    WorkspaceModuleId id,
    WorkspaceGridSpan span,
    WorkspaceGridDirection direction)
{
    const auto result = layout.resizeModule(id, span, direction);
    if (result == WorkspaceGridMutationResult::committed)
    {
        resized();
        if (onLayoutChanged)
            onLayoutChanged();
        repaint();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::dockModule(
    WorkspaceModuleId id,
    WorkspaceModuleId target,
    WorkspaceDockZone zone)
{
    const auto result = layout.dockModule(id, target, zone);
    if (result == WorkspaceGridMutationResult::committed)
    {
        synchroniseComponents();
        repaint();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::dockModuleToGroup(
    WorkspaceModuleId id,
    std::vector<WorkspaceModuleId> targets,
    WorkspaceDockZone zone)
{
    const auto result = layout.dockModuleToGroup(id, std::move(targets), zone);
    if (result == WorkspaceGridMutationResult::committed)
    {
        synchroniseComponents();
        repaint();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

bool WorkspaceModuleHostComponent::canUndoUserLayout() const noexcept
{
    return layout.canUndoGridMutation();
}

bool WorkspaceModuleHostComponent::undoUserLayout()
{
    if (!layout.undoGridMutation())
        return false;
    synchroniseComponents();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
    return true;
}

bool WorkspaceModuleHostComponent::redoUserLayout()
{
    if (!layout.redoGridMutation())
        return false;
    synchroniseComponents();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
    return true;
}

WorkspaceGridMutationResult WorkspaceModuleHostComponent::activateTab(WorkspaceModuleId id)
{
    const auto result = layout.activateTab(id);
    if (result == WorkspaceGridMutationResult::committed)
    {
        synchroniseComponents();
        if (onUserLayoutCommitted)
            onUserLayoutCommitted();
    }
    return result;
}

bool WorkspaceModuleHostComponent::activateAdjacentTab(bool reverse)
{
    const auto stacks = layout.tabStacks();
    const WorkspaceTabStackInfo* selected = nullptr;
    for (const auto& stack : stacks)
    {
        const auto activeModule = layout.moduleForInstanceId(stack.activeInstanceId);
        const auto* registration = activeModule.has_value() ? findRegistration(*activeModule) : nullptr;
        if (registration != nullptr && registration->component != nullptr
            && registration->component->hasKeyboardFocus(true))
        {
            selected = &stack;
            break;
        }
    }
    if (selected == nullptr && !stacks.empty())
        selected = &stacks.front();
    if (selected == nullptr || selected->tabInstanceIds.size() < 2)
        return false;

    const auto active = std::find(selected->tabInstanceIds.begin(),
                                  selected->tabInstanceIds.end(),
                                  selected->activeInstanceId);
    const auto index = active == selected->tabInstanceIds.end()
        ? std::size_t { 0 }
        : static_cast<std::size_t>(std::distance(selected->tabInstanceIds.begin(), active));
    const auto next = reverse
        ? (index + selected->tabInstanceIds.size() - 1) % selected->tabInstanceIds.size()
        : (index + 1) % selected->tabInstanceIds.size();
    if (layout.activateTabInstanceId(selected->tabInstanceIds[next]) != WorkspaceGridMutationResult::committed)
        return false;
    synchroniseComponents();
    focusActiveModule();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
    return true;
}

bool WorkspaceModuleHostComponent::hasFocusableVisibleModule() const noexcept
{
    if (layout.isFloating(WorkspaceModuleId::coreWaveform)
        && privilegedCoreComponent != nullptr && privilegedCoreComponent->isShowing())
    {
        return true;
    }
    const auto live = std::any_of(registrations.begin(), registrations.end(), [] (const Registration& item)
    {
        return item.component != nullptr && item.visible && item.component->isShowing();
    });
    if (live)
        return true;
    return std::any_of(inertViews.begin(), inertViews.end(), [] (const InertView& item)
    {
        return item.component != nullptr && item.component->isShowing();
    });
}

bool WorkspaceModuleHostComponent::focusActiveModule()
{
    if (layout.isFloating(WorkspaceModuleId::coreWaveform)
        && privilegedCoreComponent != nullptr && privilegedCoreComponent->isShowing())
    {
        privilegedCoreComponent->grabKeyboardFocus();
        return true;
    }
    const auto live = std::find_if(registrations.begin(), registrations.end(), [] (const Registration& item)
    {
        return item.component != nullptr && item.visible && item.component->isShowing();
    });
    if (live != registrations.end())
    {
        live->component->grabKeyboardFocus();
        return true;
    }
    const auto inert = std::find_if(inertViews.begin(), inertViews.end(), [] (const InertView& item)
    {
        return item.component != nullptr && item.component->isShowing();
    });
    if (inert != inertViews.end())
    {
        inert->component->grabKeyboardFocus();
        return true;
    }
    return false;
}

bool WorkspaceModuleHostComponent::focusFloatingModule(WorkspaceModuleId id)
{
    if (!presentationReady)
        return false;
    const auto tiles = layout.floatingTiles();
    const auto tile = std::find_if(tiles.begin(), tiles.end(), [id] (const WorkspaceFloatingTileInfo& item)
    {
        return std::find(item.members.begin(), item.members.end(), id) != item.members.end();
    });
    if (tile == tiles.end())
        return false;
    const auto projection = std::find_if(floatingProjections.begin(), floatingProjections.end(),
        [&] (const FloatingProjection& item) { return item.tileId == tile->id; });
    auto* hosted = visibleComponentForFloatingTile(*tile);
    if (projection == floatingProjections.end() || projection->window == nullptr
        || hosted == nullptr || !hosted->isShowing() || !hosted->isEnabled())
    {
        return false;
    }
    projection->window->toFront(true);
    hosted->grabKeyboardFocus();
    return true;
}

std::optional<WorkspaceFocusIdentity> WorkspaceModuleHostComponent::currentFocusIdentity() const
{
    if (layout.isFloating(WorkspaceModuleId::coreWaveform)
        && privilegedCoreComponent != nullptr
        && privilegedCoreComponent->hasKeyboardFocus(true))
    {
        return WorkspaceFocusIdentity { "floating:core-waveform", {}, "core-waveform" };
    }
    auto selected = std::find_if(registrations.begin(), registrations.end(), [] (const Registration& item)
    {
        return item.component != nullptr && item.visible && item.component->hasKeyboardFocus(true);
    });
    if (selected == registrations.end())
    {
        selected = std::find_if(registrations.begin(), registrations.end(), [] (const Registration& item)
        {
            return item.component != nullptr && item.visible && item.component->isVisible();
        });
    }
    if (selected == registrations.end())
        return std::nullopt;

    WorkspaceFocusIdentity identity;
    if (const auto placement = layout.getGridPlacement(selected->id))
    {
        identity.placementId = std::to_string(placement->column) + ":"
                             + std::to_string(placement->row) + ":"
                             + std::to_string(placement->span.columns) + ":"
                             + std::to_string(placement->span.rows);
    }
    if (const auto stack = layout.tabStackForModule(selected->id))
    {
        identity.tabStackId = stack->id;
        identity.tabInstanceId = stack->activeInstanceId;
    }
    else
    {
        identity.tabInstanceId = accessibleModuleKey(selected->id);
    }
    return identity;
}

juce::String WorkspaceModuleHostComponent::serialiseUserState() const
{
    return layout.serialiseUserState();
}

WorkspaceRestoreResult WorkspaceModuleHostComponent::restoreUserState(const juce::String& state)
{
    const auto result = layout.restoreUserState(state.toStdString());
    if (result.applied())
        synchroniseComponents();
    return result;
}

void WorkspaceModuleHostComponent::resetUserLayout()
{
    layout.resetUserState();
    synchroniseComponents();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
}

void WorkspaceModuleHostComponent::releaseInactiveViewsForMemoryPressure()
{
    trimInactiveResidency(true);
}

int WorkspaceModuleHostComponent::inactiveResidentViewCount() const noexcept
{
    return static_cast<int>(std::count_if(registrations.begin(), registrations.end(), [] (const Registration& item)
    {
        return item.inactiveResident && item.component != nullptr;
    }));
}

void WorkspaceModuleHostComponent::resized()
{
    refreshSceneProjection();
    rebuildMosaicProjection();
    coreBounds = projectedBoundsForModule(WorkspaceModuleId::coreWaveform);

    for (const auto id : visibleLayout().activeModules())
    {
        auto* registration = findRegistration(id);
        if (registration == nullptr || registration->component == nullptr
            || layout.isFloating(id))
            continue;
        const auto tileBounds = projectedBoundsForModule(id);
        auto contentBounds = tileBounds;
        if (const auto stack = layout.tabStackForModule(id);
            stack.has_value() && stack->tabInstanceIds.size() > 1)
        {
            contentBounds.removeFromTop(std::min(contentBounds.getHeight(),
                                                 RendererVisualStyle::titleStripHeight()));
        }
        registration->component->setBounds(contentBounds);
        applyVisualContractToComponent(*registration);
    }
    for (auto& inert : inertViews)
        if (inert.component != nullptr)
        {
            auto bounds = boundsForPlacement(inert.state.placement);
            if (!inert.state.tabStackId.empty())
                bounds.removeFromTop(std::min(bounds.getHeight(),
                                              RendererVisualStyle::titleStripHeight()));
            inert.component->setBounds(bounds);
        }

    for (auto& strip : dockedTabStrips)
    {
        const auto found = std::find_if(projectedTiles.begin(), projectedTiles.end(), [&strip] (const ProjectedTile& tile)
        {
            return tile.tabStackId == strip->getStackId();
        });
        if (found == projectedTiles.end())
        {
            strip->setBounds({});
            continue;
        }
        auto stripBounds = found->bounds;
        strip->setBounds(stripBounds.removeFromTop(
            std::min(stripBounds.getHeight(), RendererVisualStyle::titleStripHeight())));
        strip->toFront(false);
    }
    synchroniseSplitHandles();
    synchroniseMagCloseButtons();
}

void WorkspaceModuleHostComponent::paintOverChildren(juce::Graphics& g)
{
    if (!simpleView && layout.isFloating(WorkspaceModuleId::coreWaveform) && !coreBounds.isEmpty())
    {
        const auto anchor = coreBounds.toFloat().reduced(0.5f);
        g.setColour(RendererVisualStyle::mutedSurface());
        g.fillRoundedRectangle(anchor, RendererVisualStyle::cardRadius());
        g.setColour(RendererVisualStyle::borderColour());
        g.drawRoundedRectangle(anchor,
                               RendererVisualStyle::cardRadius(),
                               RendererVisualStyle::magBorderWidth());
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        g.drawText("Audio Waveform is floating",
                   coreBounds.reduced(18).removeFromTop(28),
                   juce::Justification::centredLeft);
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText("Use Redock in the waveform window, or close it to return here.",
                   coreBounds.reduced(18).withTrimmedTop(34),
                   juce::Justification::centredLeft,
                   true);
    }

    for (const auto id : visibleLayout().visibleModules())
    {
        const auto* registration = findRegistration(id);
        if (registration == nullptr || registration->component == nullptr || layout.isFloating(id))
            continue;

        const auto bounds = projectedBoundsForModule(id).toFloat().reduced(0.5f);
        g.setColour(RendererVisualStyle::borderColour());
        g.drawRoundedRectangle(bounds,
                               RendererVisualStyle::cardRadius(),
                               RendererVisualStyle::magBorderWidth());
        // Modern focus follows the exact owner via JUCE's FocusOutline. Drawing
        // a second ring here would also highlight the Mag when a child has focus.
        if (RendererVisualStyle::isClassic() && registration->component->hasKeyboardFocus(true))
        {
            g.setColour(RendererVisualStyle::accentBlue());
            g.drawRoundedRectangle(bounds.reduced(1.5f),
                                   RendererVisualStyle::focusRadius(),
                                   RendererVisualStyle::focusRingWidth());
        }
    }

    if (draggingModule.has_value())
    {
        if (const auto* registration = findRegistration(*draggingModule);
            registration != nullptr && registration->component != nullptr)
        {
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.72f));
            g.drawRoundedRectangle(registration->component->getBounds().toFloat().reduced(1.0f),
                                   RendererVisualStyle::cardRadius(),
                                   RendererVisualStyle::focusRingWidth());
        }
    }

    auto paintExactPreview = [&g, this] (const std::vector<ProjectedTile>&,
                                        juce::Rectangle<int> targetGroup,
                                        juce::Rectangle<int> destination,
                                        const juce::String& label,
                                        bool stacking)
    {
        const auto accent = stacking ? RendererVisualStyle::accentPurple()
                                     : RendererVisualStyle::accentBlue();
        if (destination.isEmpty())
            return;
        if (!targetGroup.isEmpty())
        {
            const auto groupOutline = targetGroup.toFloat().reduced(1.5f);
            g.setColour(accent.withAlpha(0.62f));
            g.drawRoundedRectangle(groupOutline,
                                   RendererVisualStyle::cardRadius(),
                                   1.5f);
        }
        auto bounds = destination.toFloat().reduced(3.0f);
        g.setColour(accent.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds, RendererVisualStyle::cardRadius());
        g.setColour(accent.withAlpha(0.98f));
        g.drawRoundedRectangle(bounds, RendererVisualStyle::cardRadius(), 3.0f);

        const auto font = juce::Font(juce::FontOptions(12.0f, juce::Font::bold));
        const auto availableWidth = std::max(0, destination.getWidth() - 16);
        juce::GlyphArrangement labelGlyphs;
        labelGlyphs.addLineOfText(font, label, 0.0f, 0.0f);
        const auto requestedWidth = juce::roundToInt(
            std::ceil(labelGlyphs.getBoundingBox(0, -1, true).getWidth())) + 24;
        const auto labelWidth = std::clamp(requestedWidth, std::min(96, availableWidth), availableWidth);
        if (labelWidth <= 0 || destination.getHeight() < 24)
            return;
        auto labelBounds = destination.withSizeKeepingCentre(
            labelWidth, std::min(30, destination.getHeight() - 8));
        g.setColour(RendererVisualStyle::workspaceSurface().withAlpha(0.94f));
        g.fillRoundedRectangle(labelBounds.toFloat(), 3.0f);
        g.setColour(accent);
        g.setFont(font);
        g.drawText(label,
                   labelBounds.reduced(9, 2),
                   juce::Justification::centred,
                   true);
    };

    if (dragDropProposal.has_value())
    {
        paintExactPreview(dragDropProposal->projectedTiles,
                          dragDropProposal->targetGroupBounds,
                          dragDropProposal->destinationBounds,
                          dragDropProposal->label,
                          dragDropProposal->zone == WorkspaceDockZone::centre);
    }
}

WorkspaceModuleHostComponent::Registration* WorkspaceModuleHostComponent::findRegistration(WorkspaceModuleId id)
{
    const auto found = std::find_if(registrations.begin(), registrations.end(), [id] (const Registration& item)
    {
        return item.id == id;
    });
    return found != registrations.end() ? &*found : nullptr;
}

const WorkspaceModuleHostComponent::Registration* WorkspaceModuleHostComponent::findRegistration(WorkspaceModuleId id) const
{
    const auto found = std::find_if(registrations.begin(), registrations.end(), [id] (const Registration& item)
    {
        return item.id == id;
    });
    return found != registrations.end() ? &*found : nullptr;
}

std::optional<WorkspaceModuleId> WorkspaceModuleHostComponent::moduleAt(juce::Point<int> point) const
{
    for (const auto id : layout.activeModules())
        if (!layout.isFloating(id))
            if (const auto* registration = findRegistration(id);
                registration != nullptr && registration->component != nullptr
                    && registration->component->isVisible()
                    && registration->component->getBounds().contains(point))
            {
                return id;
            }
    return std::nullopt;
}

std::optional<WorkspaceModuleId> WorkspaceModuleHostComponent::tabModuleFromEvent(
    const juce::MouseEvent& event) const
{
    static constexpr auto prefix = "workspace.docked.tab.";
    for (auto* child = event.originalComponent;
         child != nullptr && child != this;
         child = child->getParentComponent())
    {
        const auto componentId = child->getComponentID();
        if (!componentId.startsWith(prefix))
            continue;
        const auto instanceId = componentId.substring(static_cast<int>(std::char_traits<char>::length(prefix)));
        return layout.moduleForInstanceId(instanceId.toStdString());
    }
    return std::nullopt;
}

const WorkspaceModuleHostComponent::ProjectedTile*
WorkspaceModuleHostComponent::projectedTileAt(juce::Point<int> point) const noexcept
{
    const auto found = std::find_if(projectedTiles.begin(), projectedTiles.end(), [point] (const auto& tile)
    {
        return tile.bounds.contains(point);
    });
    return found != projectedTiles.end() ? &*found : nullptr;
}

std::optional<WorkspaceModuleHostComponent::MosaicEdge>
WorkspaceModuleHostComponent::projectedEdgeForTile(const ProjectedTile& tile) const noexcept
{
    if (tile.id == WorkspaceModuleId::coreWaveform || coreBounds.isEmpty())
        return std::nullopt;
    if (tile.bounds.getRight() <= coreBounds.getX())
        return MosaicEdge::west;
    if (tile.bounds.getX() >= coreBounds.getRight())
        return MosaicEdge::east;
    if (tile.bounds.getBottom() <= coreBounds.getY())
        return MosaicEdge::north;
    if (tile.bounds.getY() >= coreBounds.getBottom())
        return MosaicEdge::south;
    return std::nullopt;
}

WorkspaceDockZone WorkspaceModuleHostComponent::nearestCoreEdge(juce::Point<int> point) const noexcept
{
    if (coreBounds.isEmpty())
        return WorkspaceDockZone::right;
    const auto halfWidth = std::max(1.0, static_cast<double>(coreBounds.getWidth()) * 0.5);
    const auto halfHeight = std::max(1.0, static_cast<double>(coreBounds.getHeight()) * 0.5);
    const auto dx = (static_cast<double>(point.x) - coreBounds.getCentreX()) / halfWidth;
    const auto dy = (static_cast<double>(point.y) - coreBounds.getCentreY()) / halfHeight;
    if (std::abs(dx) >= std::abs(dy))
        return dx < 0.0 ? WorkspaceDockZone::left : WorkspaceDockZone::right;
    return dy < 0.0 ? WorkspaceDockZone::top : WorkspaceDockZone::bottom;
}

std::optional<WorkspaceModuleHostComponent::DropProposal>
WorkspaceModuleHostComponent::dropProposalAt(WorkspaceModuleId moving,
                                              juce::Point<int> point) const
{
    if (moving == WorkspaceModuleId::coreWaveform || !getLocalBounds().contains(point))
        return std::nullopt;

    auto target = WorkspaceModuleId::coreWaveform;
    auto zone = nearestCoreEdge(point);
    const auto* tile = projectedTileAt(point);
    if (tile != nullptr)
    {
        if (tile->id == WorkspaceModuleId::coreWaveform)
        {
            zone = dockZoneAt(tile->bounds, point);
            if (zone == WorkspaceDockZone::centre)
                return std::nullopt;
        }
        else
        {
            target = tile->id;
            if (target == moving)
            {
                const auto sibling = std::find_if(tile->tabMembers.begin(), tile->tabMembers.end(),
                                                  [moving] (WorkspaceModuleId id)
                {
                    return id != moving;
                });
                if (sibling == tile->tabMembers.end())
                    return std::nullopt;
                target = *sibling;
            }

            // Every Mag is a route-neutral split target. Its current position
            // relative to the waveform must not suppress two of the four drop
            // directions; that made it impossible to create another column
            // beside an east rail or another row inside a north/south group.
            zone = dockZoneAt(tile->bounds, point);
        }
    }

    const auto makeProposal = [this, moving, zone]
        (std::vector<WorkspaceModuleId> targets) -> std::optional<DropProposal>
    {
        if (targets.empty())
            return std::nullopt;
        auto previewLayout = layout;
        const auto result = zone == WorkspaceDockZone::centre
            ? previewLayout.dockModule(moving, targets.front(), zone)
            : previewLayout.dockModuleToGroup(moving, targets, zone);
        if (result != WorkspaceGridMutationResult::committed)
            return std::nullopt;

        DropProposal proposal;
        proposal.moving = moving;
        proposal.target = targets.front();
        proposal.targets = std::move(targets);
        proposal.zone = zone;
        proposal.projectedTiles = buildMosaicProjection(previewLayout).tiles;
        const auto destination = std::find_if(proposal.projectedTiles.begin(),
                                              proposal.projectedTiles.end(),
                                              [moving] (const auto& projected)
        {
            return projected.id == moving
                || std::find(projected.tabMembers.begin(), projected.tabMembers.end(), moving)
                       != projected.tabMembers.end();
        });
        if (destination == proposal.projectedTiles.end())
            return std::nullopt;
        proposal.destinationBounds = destination->bounds;

        juce::String groupTitle;
        for (size_t index = 0; index < proposal.targets.size(); ++index)
        {
            if (index != 0)
                groupTitle += " + ";
            groupTitle += accessibleModuleTitle(proposal.targets[index]);
        }
        if (zone == WorkspaceDockZone::centre)
        {
            proposal.label = "Stack with " + groupTitle;
        }
        else
        {
            switch (zone)
            {
                case WorkspaceDockZone::left: proposal.label = "Left of " + groupTitle; break;
                case WorkspaceDockZone::right: proposal.label = "Right of " + groupTitle; break;
                case WorkspaceDockZone::top: proposal.label = "Above " + groupTitle; break;
                case WorkspaceDockZone::bottom: proposal.label = "Below " + groupTitle; break;
                case WorkspaceDockZone::centre: break;
            }
        }
        return proposal;
    };

    if (zone == WorkspaceDockZone::centre)
    {
        auto proposal = makeProposal({ target });
        if (proposal.has_value() && tile != nullptr)
            proposal->targetGroupBounds = tile->bounds;
        return proposal;
    }
    if (tile == nullptr)
    {
        auto proposal = makeProposal({ WorkspaceModuleId::coreWaveform });
        if (proposal.has_value())
            proposal->targetGroupBounds = coreBounds;
        return proposal;
    }

    struct EligibleTile
    {
        const ProjectedTile* tile = nullptr;
        WorkspaceModuleId target = WorkspaceModuleId::coreWaveform;
    };
    std::vector<EligibleTile> eligible;
    eligible.reserve(projectedTiles.size());
    for (const auto& projected : projectedTiles)
    {
        auto representative = projected.id;
        if (representative == moving)
        {
            const auto sibling = std::find_if(projected.tabMembers.begin(), projected.tabMembers.end(),
                                              [moving] (WorkspaceModuleId id)
            {
                return id != moving;
            });
            if (sibling == projected.tabMembers.end())
                continue;
            representative = *sibling;
        }
        eligible.push_back({ &projected, representative });
    }
    const auto hovered = std::find_if(eligible.begin(), eligible.end(), [tile] (const auto& item)
    {
        return item.tile == tile;
    });
    if (hovered == eligible.end() || eligible.size() >= 63)
    {
        auto proposal = makeProposal({ target });
        if (proposal.has_value())
            proposal->targetGroupBounds = tile->bounds;
        return proposal;
    }

    struct ScopedProposal
    {
        DropProposal proposal;
        juce::Rectangle<int> groupBounds;
    };
    std::vector<ScopedProposal> candidates;
    const auto hoveredIndex = static_cast<std::size_t>(hovered - eligible.begin());
    const auto combinationCount = std::uint64_t { 1 } << eligible.size();
    for (std::uint64_t mask = 1; mask < combinationCount; ++mask)
    {
        if ((mask & (std::uint64_t { 1 } << hoveredIndex)) == 0)
            continue;
        std::vector<WorkspaceModuleId> targets;
        juce::Rectangle<int> groupBounds;
        for (size_t index = 0; index < eligible.size(); ++index)
        {
            if ((mask & (std::uint64_t { 1 } << index)) == 0)
                continue;
            targets.push_back(eligible[index].target);
            groupBounds = groupBounds.isEmpty() ? eligible[index].tile->bounds
                                                : groupBounds.getUnion(eligible[index].tile->bounds);
        }

        const auto touchesRequestedEdge = zone == WorkspaceDockZone::left
            ? tile->bounds.getX() == groupBounds.getX()
            : zone == WorkspaceDockZone::right
                ? tile->bounds.getRight() == groupBounds.getRight()
                : zone == WorkspaceDockZone::top
                    ? tile->bounds.getY() == groupBounds.getY()
                    : tile->bounds.getBottom() == groupBounds.getBottom();
        if (!touchesRequestedEdge)
            continue;
        auto proposal = makeProposal(std::move(targets));
        if (proposal.has_value())
        {
            proposal->targetGroupBounds = groupBounds;
            candidates.push_back({ std::move(*proposal), groupBounds });
        }
    }
    if (candidates.empty())
        return std::nullopt;

    std::sort(candidates.begin(), candidates.end(), [] (const auto& left, const auto& right)
    {
        const auto leftArea = static_cast<long long>(left.groupBounds.getWidth())
                            * left.groupBounds.getHeight();
        const auto rightArea = static_cast<long long>(right.groupBounds.getWidth())
                             * right.groupBounds.getHeight();
        if (leftArea != rightArea)
            return leftArea < rightArea;
        return left.proposal.targets.size() < right.proposal.targets.size();
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [] (const auto& left,
                                                                            const auto& right)
    {
        return left.proposal.destinationBounds == right.proposal.destinationBounds
            && left.proposal.targets == right.proposal.targets;
    }), candidates.end());

    const auto edgeDepth = zone == WorkspaceDockZone::left
        ? point.x - tile->bounds.getX()
        : zone == WorkspaceDockZone::right
            ? tile->bounds.getRight() - point.x
            : zone == WorkspaceDockZone::top
                ? point.y - tile->bounds.getY()
                : tile->bounds.getBottom() - point.y;
    const auto edgeBand = zone == WorkspaceDockZone::left || zone == WorkspaceDockZone::right
        ? std::max(1, (tile->bounds.getWidth() * 28) / 100)
        : std::max(1, (tile->bounds.getHeight() * 28) / 100);
    const auto depth = std::clamp(static_cast<double>(edgeDepth) / edgeBand, 0.0, 1.0);
    std::vector<std::size_t> scopeSizes;
    for (const auto& candidate : candidates)
        if (std::find(scopeSizes.begin(), scopeSizes.end(), candidate.proposal.targets.size())
            == scopeSizes.end())
            scopeSizes.push_back(candidate.proposal.targets.size());
    std::sort(scopeSizes.begin(), scopeSizes.end());
    const auto scopeIndex = static_cast<std::size_t>(std::lround(
        (1.0 - depth) * static_cast<double>(scopeSizes.size() - 1)));
    const auto scopeSize = scopeSizes[scopeIndex];
    const auto perpendicular = zone == WorkspaceDockZone::left || zone == WorkspaceDockZone::right
        ? point.y : point.x;
    const auto selected = std::min_element(candidates.begin(), candidates.end(),
                                           [scopeSize, perpendicular, zone]
                                           (const auto& left, const auto& right)
    {
        const auto score = [scopeSize, perpendicular, zone] (const auto& candidate)
        {
            if (candidate.proposal.targets.size() != scopeSize)
                return std::numeric_limits<long long>::max();
            const auto centre = zone == WorkspaceDockZone::left || zone == WorkspaceDockZone::right
                ? candidate.groupBounds.getCentreY() : candidate.groupBounds.getCentreX();
            return static_cast<long long>(std::abs(centre - perpendicular));
        };
        return score(left) < score(right);
    });
    return selected != candidates.end() ? std::optional<DropProposal>(selected->proposal)
                                        : std::nullopt;
}

WorkspaceDockZone WorkspaceModuleHostComponent::dockZoneAt(juce::Rectangle<int> bounds,
                                                            juce::Point<int> point) const
{
    if (bounds.isEmpty())
        return WorkspaceDockZone::centre;
    const auto centre = bounds.reduced((bounds.getWidth() * 28) / 100,
                                       (bounds.getHeight() * 28) / 100);
    if (centre.contains(point))
        return WorkspaceDockZone::centre;
    const auto halfWidth = std::max(1.0, static_cast<double>(bounds.getWidth()) * 0.5);
    const auto halfHeight = std::max(1.0, static_cast<double>(bounds.getHeight()) * 0.5);
    const auto dx = (static_cast<double>(point.x) - bounds.getCentreX()) / halfWidth;
    const auto dy = (static_cast<double>(point.y) - bounds.getCentreY()) / halfHeight;
    if (std::abs(dx) >= std::abs(dy))
        return dx < 0.0 ? WorkspaceDockZone::left : WorkspaceDockZone::right;
    return dy < 0.0 ? WorkspaceDockZone::top : WorkspaceDockZone::bottom;
}

juce::Rectangle<int> WorkspaceModuleHostComponent::boundsForPlacement(
    const WorkspaceGridPlacement& placement) const
{
    const auto projected = std::find_if(projectedTiles.begin(), projectedTiles.end(), [&placement] (const ProjectedTile& tile)
    {
        return tile.placement == placement;
    });
    if (projected != projectedTiles.end())
        return projected->bounds;

    // Drag previews may describe a placement that is not part of the committed
    // mosaic yet. Keep a bounded logical-grid fallback for that transient only.
    const auto columns = layout.gridColumnCount();
    const auto rows = std::max(1, layout.requiredGridRows());
    const auto availableWidth = std::max(0, getWidth() - moduleGap * (columns - 1));
    const auto availableHeight = std::max(0, getHeight() - moduleGap * (rows - 1));
    const auto left = (availableWidth * placement.column) / columns + moduleGap * placement.column;
    const auto right = (availableWidth * placement.right()) / columns
                     + moduleGap * (placement.right() - 1);
    const auto top = (availableHeight * placement.row) / rows + moduleGap * placement.row;
    const auto bottom = (availableHeight * placement.bottom()) / rows
                      + moduleGap * (placement.bottom() - 1);
    return { left, top, std::max(0, right - left), std::max(0, bottom - top) };
}

juce::Rectangle<int> WorkspaceModuleHostComponent::projectedBoundsForModule(
    WorkspaceModuleId id) const noexcept
{
    const auto found = std::find_if(projectedTiles.begin(), projectedTiles.end(), [id] (const ProjectedTile& tile)
    {
        return tile.id == id
            || std::find(tile.tabMembers.begin(), tile.tabMembers.end(), id) != tile.tabMembers.end();
    });
    return found != projectedTiles.end() ? found->bounds : juce::Rectangle<int> {};
}

int WorkspaceModuleHostComponent::preferredWidthForTile(const WorkspaceGridItem& item) const noexcept
{
    auto preferred = 154;
    auto consider = [this, &preferred] (WorkspaceModuleId id)
    {
        if (const auto* registration = findRegistration(id))
        {
            auto scaled = registration->preferredWidth;
            if (registration->visualSize == RendererMagSize::small)
                scaled = (scaled * 4) / 5;
            else if (registration->visualSize == RendererMagSize::large)
                scaled = (scaled * 5) / 4;
            preferred = std::max(preferred, scaled);
        }
    };
    consider(item.id);
    for (const auto member : item.tabMembers)
        consider(member);
    return preferred;
}

RendererMagSize WorkspaceModuleHostComponent::visualSizeForTile(
    const WorkspaceGridItem& item) const noexcept
{
    if (const auto* registration = findRegistration(item.id))
        return registration->visualSize;
    return RendererMagSize::automatic;
}

int WorkspaceModuleHostComponent::mainAxisWeightForTile(
    const WorkspaceGridItem& item) const noexcept
{
    return ModernNorthStarVisualContract::mainAxisWeight(visualSizeForTile(item));
}

int WorkspaceModuleHostComponent::crossAxisExtentForTile(
    const WorkspaceGridItem& item,
    MosaicEdge edge) const noexcept
{
    const auto size = visualSizeForTile(item);
    const auto densityDelta = visualDensity == RendererDensity::compact ? -10
                            : visualDensity == RendererDensity::expanded ? 14
                                                                         : 0;
    if (edge == MosaicEdge::north || edge == MosaicEdge::south)
    {
        auto extent = size == RendererMagSize::small ? 96
                    : size == RendererMagSize::large ? 224
                                                     : 148;
        extent = std::max(extent, 96 + (std::max(1, item.placement.span.rows) - 1) * 54);
        return std::max(84, extent + densityDelta);
    }

    const auto preferred = preferredWidthForTile(item);
    const auto viewportShare = size == RendererMagSize::small ? (getWidth() * 18) / 100
                           : size == RendererMagSize::large ? (getWidth() * 25) / 100
                                                            : (getWidth() * 22) / 100;
    auto extent = size == RendererMagSize::small ? std::max({ 160, preferred, viewportShare })
                : size == RendererMagSize::large ? std::max({ 240, preferred, viewportShare })
                                                 : std::max({ 220, preferred, viewportShare });
    return std::max(150, extent + densityDelta);
}

void WorkspaceModuleHostComponent::rebuildMosaicProjection()
{
    auto projection = buildMosaicProjection(visibleLayout());
    projectedTiles = std::move(projection.tiles);
    projectedSplits = std::move(projection.splits);
    projectedColumnTracks = std::move(projection.columnTracks);
    projectedRowTracks = std::move(projection.rowTracks);
    projectedColumnExtents = std::move(projection.columnExtents);
    projectedRowExtents = std::move(projection.rowExtents);
}

WorkspaceModuleHostComponent::MosaicProjection
WorkspaceModuleHostComponent::buildMosaicProjection(const WorkspaceModuleLayout& canonicalLayout) const
{
    const auto& sourceLayout = canonicalLayout;
    MosaicProjection projection;
    auto items = sourceLayout.gridItems();
    const auto core = std::find_if(items.begin(), items.end(), [] (const WorkspaceGridItem& item)
    {
        return item.privilegedCore || item.id == WorkspaceModuleId::coreWaveform;
    });
    if (core == items.end() || getWidth() <= 0 || getHeight() <= 0)
        return projection;

    std::vector<int> columnBoundaries;
    std::vector<int> rowBoundaries;
    columnBoundaries.reserve(items.size() * 2);
    rowBoundaries.reserve(items.size() * 2);
    for (const auto& item : items)
    {
        columnBoundaries.push_back(item.placement.column);
        columnBoundaries.push_back(item.placement.right());
        rowBoundaries.push_back(item.placement.row);
        rowBoundaries.push_back(item.placement.bottom());
    }
    std::sort(columnBoundaries.begin(), columnBoundaries.end());
    columnBoundaries.erase(std::unique(columnBoundaries.begin(), columnBoundaries.end()),
                           columnBoundaries.end());
    std::sort(rowBoundaries.begin(), rowBoundaries.end());
    rowBoundaries.erase(std::unique(rowBoundaries.begin(), rowBoundaries.end()),
                        rowBoundaries.end());
    for (size_t index = 0; index + 1 < columnBoundaries.size(); ++index)
        if (columnBoundaries[index + 1] > columnBoundaries[index])
            projection.columnTracks.push_back(columnBoundaries[index]);
    for (size_t index = 0; index + 1 < rowBoundaries.size(); ++index)
        if (rowBoundaries[index + 1] > rowBoundaries[index])
            projection.rowTracks.push_back(rowBoundaries[index]);
    if (projection.columnTracks.empty() || projection.rowTracks.empty())
        return projection;

    std::vector<int> desiredColumns(projection.columnTracks.size(), 180);
    std::vector<int> desiredRows(projection.rowTracks.size(), 120);
    for (const auto& item : items)
    {
        const auto projectedColumns = std::count_if(projection.columnTracks.begin(),
                                                    projection.columnTracks.end(),
                                                    [&item] (int track)
        {
            return track >= item.placement.column && track < item.placement.right();
        });
        if (item.id != WorkspaceModuleId::coreWaveform && projectedColumns == 1)
        {
            const auto found = std::find(projection.columnTracks.begin(),
                                         projection.columnTracks.end(),
                                         item.placement.column);
            if (found != projection.columnTracks.end())
                desiredColumns[static_cast<std::size_t>(found - projection.columnTracks.begin())]
                    = std::max(preferredWidthForTile(item), 150);
        }
        const auto projectedRows = std::count_if(projection.rowTracks.begin(),
                                                 projection.rowTracks.end(),
                                                 [&item] (int track)
        {
            return track >= item.placement.row && track < item.placement.bottom();
        });
        if (item.id != WorkspaceModuleId::coreWaveform && projectedRows == 1)
        {
            const auto found = std::find(projection.rowTracks.begin(),
                                         projection.rowTracks.end(),
                                         item.placement.row);
            if (found != projection.rowTracks.end())
                desiredRows[static_cast<std::size_t>(found - projection.rowTracks.begin())]
                    = std::max(72, crossAxisExtentForTile(item, MosaicEdge::north));
        }
    }

    // The waveform is the privileged workspace surface. Give its complete
    // logical region one stable weight even when T-junctions subdivide it into
    // several projection tracks; optional Mags then remain freely resizable
    // without winning half the window merely because they span one grid cell.
    const auto coreColumnSegments = std::max(1, static_cast<int>(std::count_if(
        projection.columnTracks.begin(), projection.columnTracks.end(), [&core] (int track)
    {
        return track >= core->placement.column && track < core->placement.right();
    })));
    const auto coreRowSegments = std::max(1, static_cast<int>(std::count_if(
        projection.rowTracks.begin(), projection.rowTracks.end(), [&core] (int track)
    {
        return track >= core->placement.row && track < core->placement.bottom();
    })));
    for (size_t index = 0; index < projection.columnTracks.size(); ++index)
        if (projection.columnTracks[index] >= core->placement.column
            && projection.columnTracks[index] < core->placement.right())
            desiredColumns[index] = std::max(desiredColumns[index], 840 / coreColumnSegments);
    for (size_t index = 0; index < projection.rowTracks.size(); ++index)
        if (projection.rowTracks[index] >= core->placement.row
            && projection.rowTracks[index] < core->placement.bottom())
            desiredRows[index] = std::max(desiredRows[index], 520 / coreRowSegments);

    const auto allocate = [this] (int total,
                                  const std::vector<int>& desired,
                                  const std::vector<int>& stored,
                                  int minimum)
    {
        std::vector<int> result(desired.size(), 0);
        if (desired.empty())
            return result;
        const auto available = std::max(0, total - moduleGap * static_cast<int>(desired.size() - 1));
        if (available <= minimum * static_cast<int>(desired.size()))
        {
            auto remaining = available;
            for (size_t index = 0; index < result.size(); ++index)
            {
                result[index] = index + 1 == result.size()
                    ? remaining
                    : available / static_cast<int>(result.size());
                remaining -= result[index];
            }
            return result;
        }
        const auto flexible = available - minimum * static_cast<int>(desired.size());
        auto storedFlexible = std::vector<int> {};
        if (stored.size() == desired.size())
        {
            // Split gestures persist full pixel extents, while this allocator
            // distributes only the space above each track's physical floor.
            // Subtracting that floor preserves a handle dragged fully to the
            // minimum instead of adding the minimum a second time on rebuild.
            storedFlexible.reserve(stored.size());
            for (const auto extent : stored)
                storedFlexible.push_back(std::max(0, extent - minimum));
        }
        const auto storedFlexibleTotal = std::accumulate(storedFlexible.begin(),
                                                         storedFlexible.end(), 0LL);
        const auto& weights = storedFlexibleTotal > 0 ? storedFlexible : desired;
        const auto totalWeight = std::accumulate(weights.begin(), weights.end(), 0LL);
        auto assigned = 0;
        for (size_t index = 0; index < result.size(); ++index)
        {
            const auto extra = index + 1 == result.size()
                ? flexible - assigned
                : static_cast<int>((flexible * static_cast<long long>(weights[index]))
                                   / std::max(1LL, totalWeight));
            result[index] = minimum + extra;
            assigned += extra;
        }
        return result;
    };
    const auto useLiveSplitPreview = (&canonicalLayout == &layout || &canonicalLayout == &visibleLayout())
        && resizingSplitAxis.has_value();
    const auto& columnWeights = useLiveSplitPreview
        && splitResizePreviewColumnExtents.size() == projection.columnTracks.size()
            ? splitResizePreviewColumnExtents
            : sourceLayout.getColumnTrackWeights();
    const auto& rowWeights = useLiveSplitPreview
        && splitResizePreviewRowExtents.size() == projection.rowTracks.size()
            ? splitResizePreviewRowExtents
            : sourceLayout.getRowTrackWeights();
    projection.columnExtents = allocate(getWidth(), desiredColumns, columnWeights, 72);
    projection.rowExtents = allocate(getHeight(), desiredRows, rowWeights, 48);

    std::vector<int> columnStarts(projection.columnExtents.size(), 0);
    std::vector<int> rowStarts(projection.rowExtents.size(), 0);
    for (size_t index = 1; index < columnStarts.size(); ++index)
        columnStarts[index] = columnStarts[index - 1] + projection.columnExtents[index - 1] + moduleGap;
    for (size_t index = 1; index < rowStarts.size(); ++index)
        rowStarts[index] = rowStarts[index - 1] + projection.rowExtents[index - 1] + moduleGap;

    const auto boundsFor = [&] (const WorkspaceGridPlacement& placement)
    {
        auto firstColumn = projection.columnTracks.size();
        auto lastColumn = projection.columnTracks.size();
        for (size_t index = 0; index < projection.columnTracks.size(); ++index)
            if (projection.columnTracks[index] >= placement.column
                && projection.columnTracks[index] < placement.right())
            {
                if (firstColumn == projection.columnTracks.size())
                    firstColumn = index;
                lastColumn = index;
            }
        auto firstRow = projection.rowTracks.size();
        auto lastRow = projection.rowTracks.size();
        for (size_t index = 0; index < projection.rowTracks.size(); ++index)
            if (projection.rowTracks[index] >= placement.row
                && projection.rowTracks[index] < placement.bottom())
            {
                if (firstRow == projection.rowTracks.size())
                    firstRow = index;
                lastRow = index;
            }
        if (firstColumn == projection.columnTracks.size()
            || firstRow == projection.rowTracks.size())
            return juce::Rectangle<int> {};
        const auto left = columnStarts[firstColumn];
        const auto right = columnStarts[lastColumn] + projection.columnExtents[lastColumn];
        const auto top = rowStarts[firstRow];
        const auto bottom = rowStarts[lastRow] + projection.rowExtents[lastRow];
        return juce::Rectangle<int>::leftTopRightBottom(left, top, right, bottom);
    };
    for (const auto& item : items)
        projection.tiles.push_back({ item.id, item.placement, item.tabStackId,
                                     item.tabMembers, boundsFor(item.placement) });

    const auto appendVerticalSegments = [&]
    {
        for (size_t boundary = 0; boundary + 1 < projection.columnTracks.size(); ++boundary)
        {
            const auto x = columnStarts[boundary] + projection.columnExtents[boundary];
            std::vector<int> cuts { 0, getHeight() };
            for (const auto& tile : projection.tiles)
            {
                cuts.push_back(tile.bounds.getY());
                cuts.push_back(tile.bounds.getBottom());
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            for (size_t segment = 0; segment + 1 < cuts.size(); ++segment)
            {
                const auto y0 = cuts[segment];
                const auto y1 = cuts[segment + 1];
                if (y1 <= y0)
                    continue;
                const auto midY = (y0 + y1) / 2;
                const auto spanning = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getY() <= midY && tile.bounds.getBottom() > midY
                        && tile.bounds.getX() < x && tile.bounds.getRight() > x + moduleGap;
                });
                const auto left = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getY() <= midY && tile.bounds.getBottom() > midY
                        && tile.bounds.getRight() == x;
                });
                const auto right = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getY() <= midY && tile.bounds.getBottom() > midY
                        && tile.bounds.getX() == x + moduleGap;
                });
                if (!spanning && left && right)
                    projection.splits.push_back({ SplitAxis::vertical,
                                                  projection.columnTracks[boundary],
                                                  projection.columnTracks[boundary + 1],
                                                  { x, y0, moduleGap, y1 - y0 } });
            }
        }
    };
    const auto appendHorizontalSegments = [&]
    {
        for (size_t boundary = 0; boundary + 1 < projection.rowTracks.size(); ++boundary)
        {
            const auto y = rowStarts[boundary] + projection.rowExtents[boundary];
            std::vector<int> cuts { 0, getWidth() };
            for (const auto& tile : projection.tiles)
            {
                cuts.push_back(tile.bounds.getX());
                cuts.push_back(tile.bounds.getRight());
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            for (size_t segment = 0; segment + 1 < cuts.size(); ++segment)
            {
                const auto x0 = cuts[segment];
                const auto x1 = cuts[segment + 1];
                if (x1 <= x0)
                    continue;
                const auto midX = (x0 + x1) / 2;
                const auto spanning = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getX() <= midX && tile.bounds.getRight() > midX
                        && tile.bounds.getY() < y && tile.bounds.getBottom() > y + moduleGap;
                });
                const auto top = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getX() <= midX && tile.bounds.getRight() > midX
                        && tile.bounds.getBottom() == y;
                });
                const auto bottom = std::any_of(projection.tiles.begin(), projection.tiles.end(), [&] (const auto& tile)
                {
                    return tile.bounds.getX() <= midX && tile.bounds.getRight() > midX
                        && tile.bounds.getY() == y + moduleGap;
                });
                if (!spanning && top && bottom)
                    projection.splits.push_back({ SplitAxis::horizontal,
                                                  projection.rowTracks[boundary],
                                                  projection.rowTracks[boundary + 1],
                                                  { x0, y, x1 - x0, moduleGap } });
            }
        }
    };
    appendVerticalSegments();
    appendHorizontalSegments();
    return projection;
}

void WorkspaceModuleHostComponent::applyVisualContractToComponent(Registration& registration)
{
    if (registration.component == nullptr)
        return;

    const auto content = RendererContentRect {
        registration.component->getWidth(),
        registration.component->getHeight()
    };
    const auto tier = RendererVisualStyle::resolvedInformationTier(
        visualDensity,
        registration.visualSize,
        content);
    registration.component->getProperties().set("northStarDensity", static_cast<int>(visualDensity));
    registration.component->getProperties().set("northStarMagSize", static_cast<int>(registration.visualSize));
    registration.component->getProperties().set("northStarInformationTier", static_cast<int>(tier));
    registration.component->getProperties().set(
        "northStarSweetSpotSatisfied",
        RendererVisualStyle::sweetSpotSatisfied(registration.visualSize, content));
    registration.component->getProperties().set("northStarContentWidth", content.width);
    registration.component->getProperties().set("northStarContentHeight", content.height);
    if (auto* target = dynamic_cast<WorkspaceVisualContractTarget*>(registration.component.getComponent()))
        target->applyWorkspaceVisualContract(visualDensity,
                                             registration.visualSize,
                                             tier,
                                             RendererVisualStyle::sweetSpotSatisfied(
                                                 registration.visualSize,
                                                 content));
    registration.component->repaint();
}

void WorkspaceModuleHostComponent::beginVisualTransition()
{
    const auto hasVisibleComponent = std::any_of(registrations.begin(), registrations.end(), [] (const Registration& item)
    {
        return item.visible && item.component != nullptr;
    });
    const auto enabled = presentationReady
                      && hasVisibleComponent
                      && RendererVisualStyle::isMotionEnabled();
    const auto reduced = RendererVisualStyle::isReduceMotion();
    const auto from = visualMotion.isActive() ? visualMotion.value() : 0.0f;
    visualMotion.start(from, 1.0f, enabled, reduced, RendererVisualStyle::motionDurationMs());
    if (visualMotion.isActive())
        RendererVisualStyle::noteMotionStart();
    lastMotionTickMs = juce::Time::getMillisecondCounterHiRes();
    applyVisualMotionFrame();
    refreshTimerPolicy();
}

void WorkspaceModuleHostComponent::applyVisualMotionFrame()
{
    const auto progress = visualMotion.value();
    const auto opacityFloor = RendererVisualStyle::enteringOpacityFloor();
    const auto alpha = opacityFloor + ((1.0f - opacityFloor) * progress);
    for (auto& registration : registrations)
        if (registration.component != nullptr)
            registration.component->setAlpha(registration.visible ? alpha : 1.0f);
    repaint();
}

void WorkspaceModuleHostComponent::refreshTimerPolicy()
{
    if (visualMotion.isActive())
        startTimer(RendererVisualStyle::motionTimerIntervalMs());
    else if (residencyTimerNeeded)
        startTimer(1000);
    else
        stopTimer();
}

void WorkspaceModuleHostComponent::releaseView(Registration& registration)
{
    if (registration.component != nullptr)
    {
        registration.component->removeMouseListener(this);
        if (registration.borrowedView != nullptr)
        {
            registration.component->setVisible(false);
            registration.component->setAlpha(1.0f);
            if (auto* parent = registration.component->getParentComponent())
                parent->removeChildComponent(registration.component.getComponent());
            if (registration.borrowedHomeParent != nullptr)
                registration.borrowedHomeParent->addChildComponent(registration.component.getComponent());
        }
    }
    registration.component = nullptr;
    registration.ownedComponent.reset();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Owned destruction (or borrowed detachment) has actually completed.
    // Inactive residency and Simple hiding do not pass through this terminal.
    if (registration.lifecycleToken.isOpen())
    {
        registration.lifecycleToken.quiesce();
        if (registration.lifecycleToken.resourceKind() == OptionalLifecycleResourceKind::borrowedView)
            registration.lifecycleToken.detachBorrowed();
        else
            registration.lifecycleToken.release();
    }
#endif
    registration.inactiveResident = false;
    registration.inactiveSinceMs = 0.0;
}

void WorkspaceModuleHostComponent::setSimpleView(bool enabled)
{
    if (simpleView == enabled) return;
    cancelPointerGesture();
    simpleView = enabled;
    synchroniseComponents();
}

void WorkspaceModuleHostComponent::refreshSceneProjection()
{
    simpleProjection.reset();
    if (simpleView) simpleProjection = layout.withCoreOnlyPresentation();
}

void WorkspaceModuleHostComponent::synchroniseComponents()
{
    refreshSceneProjection();
    const auto transferFocus = std::any_of(registrations.begin(), registrations.end(), [this] (const Registration& item)
    {
        return item.component != nullptr && item.component->hasKeyboardFocus(true)
            && !visibleLayout().isModuleVisible(item.id);
    });

    synchroniseFloatingWindows();

    for (auto& registration : registrations)
    {
        const auto shouldRemainPlaced = layout.isActive(registration.id);
        const auto shouldBeVisible = visibleLayout().isModuleVisible(registration.id);
        if (shouldBeVisible && registration.component == nullptr
            && (registration.factory || registration.borrowedView != nullptr))
        {
            if (onBeforeModuleViewCreated && !onBeforeModuleViewCreated(registration.id))
                continue;
            if (registration.borrowedView != nullptr)
                registration.component = registration.borrowedView;
            else
            {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                registration.lifecycleToken = OptionalLifecycleToken::request(optionalLifecycleContext,
                    lifecycleOwner(registration.id), OptionalLifecycleResourceKind::ownedView);
                const ScopedOptionalLifecycleContext construction(optionalLifecycleContext);
                try
                {
#endif
                registration.ownedComponent = registration.factory();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                }
                catch (...)
                {
                    registration.lifecycleToken.failed();
                    throw;
                }
                if (registration.ownedComponent) registration.lifecycleToken.created();
                else registration.lifecycleToken.failed();
#endif
                registration.component = registration.ownedComponent.get();
            }
            if (registration.component != nullptr)
            {
                // Every optional View occupies a real Mag tile.  Even passive
                // visualisers (notably the Output Meter) must own their header
                // hit target so the Host can start a drag instead of letting
                // the pointer fall through the workspace.
                registration.component->setInterceptsMouseClicks(true, true);
                registration.component->setWantsKeyboardFocus(true);
                registration.component->setHasFocusOutline(!RendererVisualStyle::isClassic());
                registration.component->setComponentID(
                    "workspace.module." + juce::String(accessibleModuleKey(registration.id)));
                registration.component->setTitle(accessibleModuleTitle(registration.id));
                applyVisualContractToComponent(registration);
                addAndMakeVisible(*registration.component);
                registration.component->addMouseListener(this, true);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                if (registration.borrowedView != nullptr)
                    registration.lifecycleToken = OptionalLifecycleToken::attachBorrowed(
                        optionalLifecycleContext, lifecycleOwner(registration.id));
#endif

            }
        }

        if (registration.component != nullptr && shouldBeVisible)
        {
            registration.component->setVisible(true);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            registration.lifecycleToken.activate();
#endif
            registration.inactiveResident = false;
            registration.inactiveSinceMs = 0.0;
            registration.lastUsedSequence = nextUseSequence++;
        }
        else if (registration.component != nullptr && shouldRemainPlaced)
        {
            if (registration.component->hasKeyboardFocus(true))
                registration.component->giveAwayKeyboardFocus();
            registration.component->setVisible(false);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            registration.lifecycleToken.quiesce();
#endif
            registration.component->invalidateAccessibilityHandler();
            if (simpleView)
            {
                // Temporarily hidden active views keep their existing state.
                // Simple is not a Tab eviction or a module Close operation.
                registration.inactiveResident = false;
                registration.inactiveSinceMs = 0.0;
            }
            else if (!registration.inactiveResident)
            {
                registration.inactiveResident = true;
                registration.inactiveSinceMs = juce::Time::getMillisecondCounterHiRes();
                registration.lastUsedSequence = nextUseSequence++;
            }
        }
        else if (registration.component != nullptr)
        {
            releaseView(registration);
        }

        const auto actualVisible = shouldBeVisible && registration.component != nullptr;
        if (registration.visible != actualVisible)
        {
            registration.visible = actualVisible;
            if (onModuleVisibilityChanged)
                onModuleVisibilityChanged(registration.id, actualVisible);
        }
    }

    synchroniseInertViews();
    synchroniseFloatingWindows();
    synchroniseDockedTabStrips();
    for (auto& registration : registrations)
    {
        if (registration.component != nullptr
            && visibleLayout().isModuleVisible(registration.id)
            && !layout.isFloating(registration.id)
            && registration.component->getParentComponent() != this)
        {
            addAndMakeVisible(*registration.component);
        }
    }
    if (transferFocus)
    {
        const auto visible = std::find_if(registrations.begin(), registrations.end(), [] (const Registration& item)
        {
            return item.component != nullptr && item.visible;
        });
        if (visible != registrations.end())
            visible->component->grabKeyboardFocus();
        else
        {
            const auto inert = std::find_if(inertViews.begin(), inertViews.end(), [] (const InertView& item)
            {
                return item.component != nullptr && item.component->isVisible();
            });
            if (inert != inertViews.end())
                inert->component->grabKeyboardFocus();
        }
    }
    trimInactiveResidency(false);
    resized();
    beginVisualTransition();
    if (onLayoutChanged)
        onLayoutChanged();
}

void WorkspaceModuleHostComponent::synchroniseSplitHandles()
{
    if (resizingSplitAxis.has_value() && splitHandles.size() == projectedSplits.size())
    {
        for (size_t index = 0; index < splitHandles.size(); ++index)
        {
            splitHandles[index]->setBounds(projectedSplits[index].bounds);
            splitHandles[index]->toFront(false);
        }
        return;
    }

    splitHandles.clear();
    splitHandles.reserve(projectedSplits.size());
    for (size_t index = 0; index < projectedSplits.size(); ++index)
    {
        const auto split = projectedSplits[index];
        auto handle = std::make_unique<SplitHandle>(split.axis,
                                                   split.leadingTrack,
                                                   split.trailingTrack);
        handle->setComponentID("workspace.split."
            + juce::String(split.axis == SplitAxis::vertical ? "vertical." : "horizontal.")
            + juce::String(split.leadingTrack) + "." + juce::String(split.trailingTrack)
            + "." + juce::String(static_cast<int>(index)));
        handle->onDragStarted = [this, split]
        {
            beginSplitResize(split.axis, split.leadingTrack, split.trailingTrack);
        };
        handle->onDragged = [this] (int deltaX, int deltaY)
        {
            updateSplitResize(deltaX, deltaY);
        };
        handle->onDragEnded = [this] { finishSplitResize(); };
        handle->setBounds(split.bounds);
        addAndMakeVisible(*handle);
        handle->toFront(false);
        splitHandles.push_back(std::move(handle));
    }
}

void WorkspaceModuleHostComponent::synchroniseDockedTabStrips()
{
    auto stacks = visibleLayout().tabStacks();
    auto isDockedStack = [this, &stacks] (const std::string& stackId)
    {
        const auto found = std::find_if(stacks.begin(), stacks.end(), [&stackId] (const WorkspaceTabStackInfo& stack)
        {
            return stack.id == stackId && stack.tabInstanceIds.size() > 1;
        });
        if (found == stacks.end())
            return false;
        if (const auto active = layout.moduleForInstanceId(found->activeInstanceId))
            return !layout.isFloating(*active);
        return true;
    };
    dockedTabStrips.erase(std::remove_if(dockedTabStrips.begin(), dockedTabStrips.end(), [&isDockedStack] (const auto& strip)
    {
        return !isDockedStack(strip->getStackId());
    }), dockedTabStrips.end());

    auto labelForId = [this] (const std::string& instanceId)
    {
        if (const auto module = layout.moduleForInstanceId(instanceId))
            return juce::String(accessibleModuleTitle(*module));
        return juce::String::fromUTF8(instanceId.c_str());
    };
    for (const auto& stack : stacks)
    {
        if (stack.tabInstanceIds.size() < 2 || !isDockedStack(stack.id))
            continue;
        auto found = std::find_if(dockedTabStrips.begin(), dockedTabStrips.end(), [&stack] (const auto& strip)
        {
            return strip->getStackId() == stack.id;
        });
        if (found == dockedTabStrips.end())
        {
            auto strip = std::make_unique<DockedTabStrip>(stack.id);
            strip->addMouseListener(this, true);
            addAndMakeVisible(*strip);
            dockedTabStrips.push_back(std::move(strip));
            found = std::prev(dockedTabStrips.end());
        }
        (*found)->update(stack.tabInstanceIds,
                         stack.activeInstanceId,
                         labelForId,
                         [this] (const std::string& instanceId)
                         {
                             handleDockedTab(instanceId);
                         });
    }
}

void WorkspaceModuleHostComponent::synchroniseMagCloseButtons()
{
    const auto isDockedVisible = [this] (WorkspaceModuleId id)
    {
        return id != WorkspaceModuleId::coreWaveform
            && visibleLayout().isModuleVisible(id)
            && !layout.isFloating(id)
            && !projectedBoundsForModule(id).isEmpty();
    };
    magCloseButtons.erase(std::remove_if(magCloseButtons.begin(), magCloseButtons.end(),
                                         [&isDockedVisible] (const auto& button)
    {
        return !isDockedVisible(button->getModule());
    }), magCloseButtons.end());

    for (const auto& tile : projectedTiles)
    {
        if (!isDockedVisible(tile.id))
            continue;
        auto found = std::find_if(magCloseButtons.begin(), magCloseButtons.end(),
                                  [&tile] (const auto& button)
        {
            return button->getModule() == tile.id;
        });
        if (found == magCloseButtons.end())
        {
            auto button = std::make_unique<MagCloseButton>(tile.id);
            button->onClick = [this, id = tile.id]
            {
                if (onModuleCloseRequested)
                    onModuleCloseRequested(id);
                else
                    (void) closeModule(id);
            };
            addAndMakeVisible(*button);
            magCloseButtons.push_back(std::move(button));
            found = std::prev(magCloseButtons.end());
        }
        constexpr auto buttonExtent = 22;
        constexpr auto inset = 5;
        (*found)->setBounds(tile.bounds.getRight() - buttonExtent - inset,
                           tile.bounds.getY() + inset,
                           buttonExtent,
                           buttonExtent);
        (*found)->toFront(false);
    }
}

void WorkspaceModuleHostComponent::handleDockedTab(const std::string& instanceId)
{
    auto result = WorkspaceGridMutationResult::noChange;
    if (const auto known = layout.moduleForInstanceId(instanceId))
        result = layout.activateTab(*known);
    else
        result = layout.activateTabInstanceId(instanceId);
    if (result != WorkspaceGridMutationResult::committed)
        return;
    synchroniseComponents();
    if (onUserLayoutCommitted)
        onUserLayoutCommitted();
}

void WorkspaceModuleHostComponent::synchroniseInertViews()
{
    const auto states = layout.inertModules();
    auto shouldBeVisible = [this] (const WorkspaceInertModuleInfo& state)
    {
        if (state.tabStackId.empty())
            return true;
        const auto stacks = layout.tabStacks();
        const auto found = std::find_if(stacks.begin(), stacks.end(), [&state] (const WorkspaceTabStackInfo& stack)
        {
            return stack.id == state.tabStackId;
        });
        return found != stacks.end() && found->activeInstanceId == state.instanceId;
    };

    inertViews.erase(std::remove_if(inertViews.begin(), inertViews.end(), [&states, &shouldBeVisible] (InertView& view)
    {
        const auto found = std::find_if(states.begin(), states.end(), [&view] (const WorkspaceInertModuleInfo& state)
        {
            return state.instanceId == view.state.instanceId;
        });
        return found == states.end() || !shouldBeVisible(*found);
    }), inertViews.end());

    for (const auto& state : states)
    {
        if (!shouldBeVisible(state))
            continue;
        const auto found = std::find_if(inertViews.begin(), inertViews.end(), [&state] (const InertView& view)
        {
            return view.state.instanceId == state.instanceId;
        });
        if (found != inertViews.end())
        {
            found->state = state;
            continue;
        }
        InertView view;
        view.state = state;
        view.component = std::make_unique<InertWorkspaceModuleComponent>(state);
        addAndMakeVisible(*view.component);
        view.component->addMouseListener(this, true);
        inertViews.push_back(std::move(view));
    }
}

void WorkspaceModuleHostComponent::synchroniseFloatingWindows()
{
    if (!presentationReady || simpleView)
    {
        destroyFloatingWindows();
        return;
    }

    const auto tiles = layout.floatingTiles();
    for (auto item = floatingProjections.begin(); item != floatingProjections.end();)
    {
        const auto desired = std::any_of(tiles.begin(), tiles.end(), [&] (const WorkspaceFloatingTileInfo& tile)
        {
            return tile.id == item->tileId && visibleComponentForFloatingTile(tile) != nullptr;
        });
        if (desired)
        {
            ++item;
            continue;
        }
        detachFloatingContent(*item);
        item = floatingProjections.erase(item);
    }

    const auto displays = currentDisplaySnapshots();
    const auto mainBounds = mainWindowLogicalBounds();
    for (const auto& tile : tiles)
    {
        auto* hosted = visibleComponentForFloatingTile(tile);
        if (hosted == nullptr)
            continue;

        auto active = tile.privilegedCore ? WorkspaceModuleId::coreWaveform : tile.members.front();
        for (const auto member : tile.members)
            if (visibleLayout().isModuleVisible(member))
                active = member;

        auto found = std::find_if(floatingProjections.begin(), floatingProjections.end(), [&] (const FloatingProjection& item)
        {
            return item.tileId == tile.id;
        });
        auto created = false;
        if (found == floatingProjections.end())
        {
            FloatingProjection projection;
            projection.tileId = tile.id;
            projection.commandTarget = active;
            projection.window = std::make_unique<FloatingWindow>(
                juce::String(accessibleModuleTitle(active)) + " - Single Editor",
                tile.privilegedCore);
            auto* window = projection.window.get();
            window->onClose = [this, active] (bool privileged)
            {
                handleFloatingClose(active, privileged);
            };
            window->onRedock = [this, active] { handleFloatingRedock(active); };
            window->onTogglePlayback = [this, active]
            {
                if (onFloatingWindowCommandRequested)
                    onFloatingWindowCommandRequested(active,
                                                       WorkspaceFloatingWindowCommand::togglePlayback);
            };
            window->onActivateTab = [this] (WorkspaceModuleId id) { handleFloatingTab(id); };
            window->onBoundsChanged = [this, id = tile.id] (juce::Rectangle<int> bounds)
            {
                updateFloatingPlacementFromWindow(id, bounds);
            };
            window->onScaleFactorChanged = [this]
            {
                juce::Component::SafePointer<WorkspaceModuleHostComponent> safeThis(this);
                juce::MessageManager::callAsync([safeThis]
                {
                    if (safeThis != nullptr)
                        safeThis->revalidateFloatingWindows();
                });
            };
            floatingProjections.push_back(std::move(projection));
            found = std::prev(floatingProjections.end());
            created = true;
        }

        found->commandTarget = active;
        found->window->setName(juce::String(accessibleModuleTitle(active)) + " - Single Editor");
        found->window->setHostedComponent(hosted, tile.members, active);
        if (!displays.empty())
        {
            const auto resolved = WorkspaceFloatingWindowModel::resolve(tile.placement,
                                                                        displays,
                                                                        mainBounds);
            found->window->setBoundsWithoutFeedback({ resolved.placement.bounds.x,
                                                      resolved.placement.bounds.y,
                                                      resolved.placement.bounds.width,
                                                      resolved.placement.bounds.height });
            if (!(resolved.placement == tile.placement))
                (void) layout.updateFloatingPlacement(active, resolved.placement);
        }
        found->window->setVisible(true);
        if (created)
            found->window->toFront(false);
    }
}

void WorkspaceModuleHostComponent::destroyFloatingWindows()
{
    for (auto& projection : floatingProjections)
    {
        if (projection.window != nullptr)
            projection.window->setVisible(false);
        detachFloatingContent(projection);
    }
    floatingProjections.clear();
}

void WorkspaceModuleHostComponent::detachFloatingContent(FloatingProjection& projection)
{
    if (projection.window == nullptr)
        return;
    auto* component = projection.window->detachHostedComponent();
    if (component == nullptr)
        return;

    if (component == privilegedCoreComponent)
    {
        if (privilegedCoreHomeParent != nullptr)
            privilegedCoreHomeParent->addAndMakeVisible(*component);
        else
            component->setVisible(false);
        return;
    }

    addAndMakeVisible(*component);
    component->setVisible(visibleLayout().isModuleVisible(projection.commandTarget)
                          && !layout.isFloating(projection.commandTarget));
}

void WorkspaceModuleHostComponent::updateFloatingPlacementFromWindow(
    const std::string& tileId,
    juce::Rectangle<int> bounds)
{
    const auto tiles = layout.floatingTiles();
    const auto tile = std::find_if(tiles.begin(), tiles.end(), [&] (const WorkspaceFloatingTileInfo& item)
    {
        return item.id == tileId;
    });
    const auto displays = currentDisplaySnapshots();
    if (tile == tiles.end() || tile->members.empty() || displays.empty())
        return;

    const auto centre = bounds.getCentre();
    auto target = std::find_if(displays.begin(), displays.end(), [&] (const WorkspaceDisplaySnapshot& item)
    {
        return centre.x >= item.workArea.x && centre.x < item.workArea.right()
            && centre.y >= item.workArea.y && centre.y < item.workArea.bottom();
    });
    if (target == displays.end())
        target = std::find_if(displays.begin(), displays.end(), [] (const WorkspaceDisplaySnapshot& item)
        {
            return item.primary;
        });
    if (target == displays.end())
        target = displays.begin();

    const auto placement = WorkspaceFloatingWindowModel::capture(
        { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() },
        *target);
    if (layout.updateFloatingPlacement(tile->members.front(), placement)
        && onUserLayoutCommitted)
    {
        onUserLayoutCommitted();
    }
}

std::vector<WorkspaceDisplaySnapshot> WorkspaceModuleHostComponent::currentDisplaySnapshots() const
{
    std::vector<WorkspaceDisplaySnapshot> result;
    for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
    {
        const auto affinity = display.isMain
            ? std::string("primary")
            : std::string("display:") + std::to_string(display.totalArea.getX()) + ":"
                + std::to_string(display.totalArea.getY()) + ":"
                + std::to_string(display.totalArea.getWidth()) + ":"
                + std::to_string(display.totalArea.getHeight());
        result.push_back({ affinity,
                           { display.userArea.getX(),
                             display.userArea.getY(),
                             display.userArea.getWidth(),
                             display.userArea.getHeight() },
                           juce::roundToInt(display.scale * 100.0),
                           display.isMain });
    }
    return result;
}

WorkspaceLogicalBounds WorkspaceModuleHostComponent::mainWindowLogicalBounds() const
{
    const auto* topLevel = getTopLevelComponent();
    const auto bounds = topLevel != nullptr ? topLevel->getScreenBounds() : getScreenBounds();
    return { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() };
}

juce::Component* WorkspaceModuleHostComponent::visibleComponentForFloatingTile(
    const WorkspaceFloatingTileInfo& tile) const
{
    if (tile.privilegedCore)
        return privilegedCoreComponent;
    for (const auto member : tile.members)
        if (visibleLayout().isModuleVisible(member))
            if (const auto* registration = findRegistration(member))
                return registration->component.getComponent();
    return nullptr;
}

void WorkspaceModuleHostComponent::handleFloatingClose(WorkspaceModuleId id,
                                                        bool privilegedCore)
{
    juce::Component::SafePointer<WorkspaceModuleHostComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, id, privilegedCore]
    {
        if (safeThis != nullptr && safeThis->onFloatingWindowCommandRequested)
            safeThis->onFloatingWindowCommandRequested(
                id,
                privilegedCore ? WorkspaceFloatingWindowCommand::redock
                               : WorkspaceFloatingWindowCommand::remove);
    });
}

void WorkspaceModuleHostComponent::handleFloatingRedock(WorkspaceModuleId id)
{
    juce::Component::SafePointer<WorkspaceModuleHostComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, id]
    {
        if (safeThis != nullptr && safeThis->onFloatingWindowCommandRequested)
            safeThis->onFloatingWindowCommandRequested(id,
                                                        WorkspaceFloatingWindowCommand::redock);
    });
}

void WorkspaceModuleHostComponent::handleFloatingTab(WorkspaceModuleId id)
{
    juce::Component::SafePointer<WorkspaceModuleHostComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, id]
    {
        if (safeThis != nullptr)
            (void) safeThis->activateTab(id);
    });
}

void WorkspaceModuleHostComponent::trimInactiveResidency(bool memoryPressure)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    auto inactiveCount = 0;
    std::size_t inactiveBytes = 0;
    for (const auto& registration : registrations)
    {
        if (!registration.inactiveResident || registration.component == nullptr)
            continue;
        ++inactiveCount;
        inactiveBytes += layout.estimatedViewBytes(registration.id);
    }

    while (inactiveCount > 0)
    {
        auto candidate = registrations.end();
        for (auto item = registrations.begin(); item != registrations.end(); ++item)
        {
            if (!item->inactiveResident || item->component == nullptr)
                continue;
            const auto expired = now - item->inactiveSinceMs >= maximumInactiveResidentAgeMs;
            const auto mustRelease = memoryPressure
                                  || !layout.allowsInactiveViewResidency(item->id)
                                  || expired
                                  || inactiveCount > maximumInactiveResidentViews
                                  || inactiveBytes > maximumInactiveResidentBytes;
            if (!mustRelease)
                continue;
            if (candidate == registrations.end()
                || item->lastUsedSequence < candidate->lastUsedSequence)
            {
                candidate = item;
            }
        }
        if (candidate == registrations.end())
            break;

        inactiveBytes -= std::min(inactiveBytes, layout.estimatedViewBytes(candidate->id));
        --inactiveCount;
        releaseView(*candidate);
    }

    residencyTimerNeeded = inactiveCount > 0;
    refreshTimerPolicy();
}

void WorkspaceModuleHostComponent::timerCallback()
{
    if (visualMotion.isActive())
    {
        if (!presentationReady
            || !RendererVisualStyle::isMotionEnabled()
            || RendererVisualStyle::isReduceMotion())
        {
            visualMotion.snapTo(1.0f);
        }
        else
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            visualMotion.advance(static_cast<float>(std::max(0.0, now - lastMotionTickMs)));
            lastMotionTickMs = now;
        }
        applyVisualMotionFrame();
        refreshTimerPolicy();
        return;
    }

    trimInactiveResidency(false);
}

void WorkspaceModuleHostComponent::beginSplitResize(SplitAxis axis,
                                                    int leadingTrack,
                                                    int trailingTrack)
{
    const auto& tracks = axis == SplitAxis::vertical ? projectedColumnTracks
                                                     : projectedRowTracks;
    const auto leading = std::find(tracks.begin(), tracks.end(), leadingTrack);
    const auto trailing = std::find(tracks.begin(), tracks.end(), trailingTrack);
    if (leading == tracks.end() || trailing == tracks.end() || trailing != leading + 1)
        return;
    resizingSplitAxis = axis;
    resizingLeadingTrack = leadingTrack;
    resizingTrailingTrack = trailingTrack;
    splitResizeStartColumnExtents = projectedColumnExtents;
    splitResizeStartRowExtents = projectedRowExtents;
    splitResizePreviewColumnExtents = splitResizeStartColumnExtents;
    splitResizePreviewRowExtents = splitResizeStartRowExtents;
}

void WorkspaceModuleHostComponent::updateSplitResize(int screenDeltaX, int screenDeltaY)
{
    if (!resizingSplitAxis.has_value())
        return;
    const auto vertical = *resizingSplitAxis == SplitAxis::vertical;
    const auto& tracks = vertical ? projectedColumnTracks : projectedRowTracks;
    const auto leading = std::find(tracks.begin(), tracks.end(), resizingLeadingTrack);
    if (leading == tracks.end() || leading + 1 == tracks.end()
        || *(leading + 1) != resizingTrailingTrack)
        return;
    const auto index = static_cast<std::size_t>(leading - tracks.begin());
    auto& preview = vertical ? splitResizePreviewColumnExtents : splitResizePreviewRowExtents;
    const auto& start = vertical ? splitResizeStartColumnExtents : splitResizeStartRowExtents;
    if (index + 1 >= preview.size() || index + 1 >= start.size())
        return;
    const auto minimum = vertical ? 72 : 48;
    const auto pairTotal = start[index] + start[index + 1];
    if (pairTotal <= minimum * 2)
        return;
    const auto delta = vertical ? screenDeltaX : screenDeltaY;
    preview = start;
    preview[index] = std::clamp(start[index] + delta, minimum, pairTotal - minimum);
    preview[index + 1] = pairTotal - preview[index];
    resized();
    if (onLayoutChanged)
        onLayoutChanged();
}

void WorkspaceModuleHostComponent::finishSplitResize()
{
    if (!resizingSplitAxis.has_value())
        return;
    const auto columns = splitResizePreviewColumnExtents;
    const auto rows = splitResizePreviewRowExtents;
    resizingSplitAxis.reset();
    resizingLeadingTrack = -1;
    resizingTrailingTrack = -1;
    splitResizeStartColumnExtents.clear();
    splitResizeStartRowExtents.clear();
    splitResizePreviewColumnExtents.clear();
    splitResizePreviewRowExtents.clear();
    const auto result = layout.setGridTrackWeights(columns, rows);
    resized();
    if (onLayoutChanged)
        onLayoutChanged();
    if (result == WorkspaceGridMutationResult::committed && onUserLayoutCommitted)
        onUserLayoutCommitted();
}

void WorkspaceModuleHostComponent::mouseDown(const juce::MouseEvent& event)
{
    if (simpleView) return;
    if (!event.mods.isLeftButtonDown())
        return;
    const auto point = event.getEventRelativeTo(this).getPosition();
    const auto tabModule = tabModuleFromEvent(event);
    const auto module = tabModule.has_value() ? tabModule : moduleAt(point);
    if (!module.has_value())
        return;

    const auto* registration = findRegistration(*module);
    if (registration == nullptr || registration->component == nullptr)
        return;

    if (!tabModule.has_value()
        && point.y - registration->component->getY() >= static_cast<int>(registration->component->getProperties()
                .getWithDefault("workspace.headerHeight", titleDragHeight + 1)))
        return;

    // The host listens recursively, so a header button/selector also reaches
    // this callback. Its gesture belongs to the control, never to the tile.
    if (!tabModule.has_value())
    {
        for (auto* child = event.originalComponent;
             child != nullptr && child != registration->component.getComponent();
             child = child->getParentComponent())
        {
            if (dynamic_cast<juce::Button*>(child) != nullptr
                || dynamic_cast<juce::Slider*>(child) != nullptr
                || dynamic_cast<juce::ComboBox*>(child) != nullptr
                || dynamic_cast<juce::TextEditor*>(child) != nullptr)
                return;
        }
    }

    draggingModule = module;
    dragDropProposal.reset();
    repaint();
}

bool WorkspaceModuleHostComponent::cancelPointerGesture()
{
    if (!draggingModule.has_value() && !resizingSplitAxis.has_value())
        return false;
    draggingModule.reset();
    dragDropProposal.reset();
    resizingSplitAxis.reset();
    resizingLeadingTrack = -1;
    resizingTrailingTrack = -1;
    splitResizeStartColumnExtents.clear();
    splitResizeStartRowExtents.clear();
    splitResizePreviewColumnExtents.clear();
    splitResizePreviewRowExtents.clear();
    resized();
    repaint();
    return true;
}

bool WorkspaceModuleHostComponent::keyPressed(const juce::KeyPress& key)
{
    return key == juce::KeyPress::escapeKey && cancelPointerGesture();
}

void WorkspaceModuleHostComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (!draggingModule.has_value())
        return;

    const auto point = event.getEventRelativeTo(this).getPosition();
    dragDropProposal = dropProposalAt(*draggingModule, point);
    repaint();
}

void WorkspaceModuleHostComponent::mouseUp(const juce::MouseEvent& event)
{
    if (!draggingModule.has_value())
        return;
    if (!getLocalBounds().contains(event.getEventRelativeTo(this).getPosition()))
    {
        cancelPointerGesture();
        return;
    }

    const auto moving = *draggingModule;
    const auto proposal = dragDropProposal;
    draggingModule.reset();
    dragDropProposal.reset();
    repaint();

    if (proposal.has_value())
    {
        if (proposal->zone == WorkspaceDockZone::centre)
            (void) dockModule(moving, proposal->target, proposal->zone);
        else
            (void) dockModuleToGroup(moving, proposal->targets, proposal->zone);
    }
}
} // namespace singleeditor
