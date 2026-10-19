#include "UI/Components/WaveformViewComponent.h"
#include "UI/Commands/NativeFocusTrace.h"
#include "UI/Components/WaveformLaneControls.h"
#include "UI/Components/WaveformPlotGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernWaveformPainter.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
namespace
{
class WaveformAccessibilityValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit WaveformAccessibilityValue(const WaveformViewComponent& ownerToUse)
        : owner(ownerToUse)
    {
    }

    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override
    {
        return owner.getAccessibilityValueSummary();
    }
    void setValueAsString(const juce::String&) override {}

private:
    const WaveformViewComponent& owner;
};

bool hasEditableAudioState(const SingleTrackDocument::StatePtr& state) noexcept
{
    return state != nullptr
        && state->info.isValid()
        && !state->isPreviewOnly()
        && state->hasPlaybackSource();
}

juce::Colour laneColourForIndex(int index)
{
    return RendererVisualStyle::waveformLaneColour(index % 6);
}

bool usesModernWaveform() noexcept
{
    return !RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin();
}

juce::Rectangle<float> waveformContent(juce::Rectangle<float> plot) noexcept
{
    return WaveformPlotGeometry::content(plot, usesModernWaveform());
}

juce::String formatRulerLabel(double seconds, double stepSeconds)
{
    return WaveformPlotGeometry::formatTime(seconds, stepSeconds);
}

double chooseRulerStep(double durationSeconds, float width)
{
    if (durationSeconds <= 0.0 || width <= 0.0f)
        return 1.0;

    constexpr std::array<double, 18> steps {
        0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0,
        30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0, 7200.0
    };

    const auto targetTickCount = juce::jlimit(4.0, 12.0, static_cast<double>(width / 120.0f));
    const auto desiredStep = durationSeconds / targetTickCount;

    for (const auto step : steps)
    {
        if (step >= desiredStep)
            return step;
    }

    return steps.back();
}

float dbFromLinear(float linear)
{
    if (linear <= 0.000001f)
        return -120.0f;

    return juce::Decibels::gainToDecibels(linear);
}

void setViewButtonIcon(juce::TextButton& button, RendererAssetId assetId)
{
    const auto iconFile = juce::String(RendererVisualStyle::assetToken(assetId).fileName);
    button.getProperties().set("iconName", iconFile);
    button.getProperties().set("iconOnly", true);
    if (button.getButtonText().isNotEmpty())
        button.setTooltip(button.getButtonText());
}
} // namespace

WaveformViewComponent::WaveformViewComponent(AudioEngine& engineToUse,
                                             SingleTrackDocument& documentToUse,
                                             WaveformModel& waveformModelToUse)
    : engine(engineToUse),
      document(documentToUse),
      waveformModel(waveformModelToUse)
{
    waveformModel.addChangeListener(this);
    addChildComponent(waveformTitle);
    waveformTitle.setText("Waveform", juce::dontSendNotification);
    waveformTitle.setFont(ModernTypography::font(13.0f, juce::Font::bold));
    waveformTitle.setInterceptsMouseClicks(false, false);
    addChildComponent(selectionSummary);
    selectionSummary.setInterceptsMouseClicks(false, false);
    selectionSummary.setComponentID("core.waveform.selection-summary");
    selectionSummary.setTitle("Waveform selection range and length");
    selectionSummary.setText("Selection: --", juce::dontSendNotification);
    selectionSummary.setFont(ModernTypography::font(11.0f));
    selectionSummary.setMinimumHorizontalScale(1.0f);
    addChildComponent(viewOptionsButton);
    addChildComponent(returnToGeneralButton);
    returnToGeneralButton.setComponentID("workspace.view.back");
    returnToGeneralButton.setTitle("Return to General workspace");
    returnToGeneralButton.setTooltip("Restore your full Mag layout");
    returnToGeneralButton.onClick = [this] { if (onReturnToGeneral) onReturnToGeneral(); };
    viewOptionsButton.setTitle("Waveform view options");
    viewOptionsButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Waveform", waveformTab.isEnabled(), viewMode == ViewMode::waveform);
        menu.addItem(2, "Spectral", spectralTab.isEnabled(), viewMode == ViewMode::spectral);
        menu.addSeparator();
        menu.addItem(3, "Stacked channels", stackedViewButton.isEnabled(), displayMode == DisplayMode::stacked);
        menu.addItem(4, "Mixed channels", mixedViewButton.isEnabled(), displayMode == DisplayMode::mixed);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&viewOptionsButton),
            [safe = juce::Component::SafePointer<WaveformViewComponent>(this)] (int result)
            {
                if (safe == nullptr) return;
                juce::Button* button = result == 1 ? &safe->waveformTab
                    : result == 2 ? &safe->spectralTab : result == 3 ? &safe->stackedViewButton
                    : result == 4 ? &safe->mixedViewButton : nullptr;
                if (button != nullptr && button->isEnabled()) button->triggerClick();
            });
    };
    startTimerHz(30);
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    setWantsKeyboardFocus(true);
    setHasFocusOutline(!RendererVisualStyle::isClassic());

    addAndMakeVisible(waveformTab);
    waveformTab.setButtonText("Waveform");
    waveformTab.setComponentID("core.waveform.view");
    waveformTab.setTitle("Waveform view");
    waveformTab.setClickingTogglesState(true);
    waveformTab.setRadioGroupId(1001);
    waveformTab.setToggleState(true, juce::dontSendNotification);
    waveformTab.onClick = [this]
    {
        if (onViewModeRequested)
            onViewModeRequested(ViewMode::waveform);
        else
            setViewMode(ViewMode::waveform);
    };
    setViewButtonIcon(waveformTab, ModernNorthStarAssetId::waveform);

    addAndMakeVisible(spectralTab);
    spectralTab.setButtonText("Spectral");
    spectralTab.setComponentID("core.waveform.spectral");
    spectralTab.setTitle("Spectral view");
    spectralTab.setClickingTogglesState(true);
    spectralTab.setRadioGroupId(1001);
    spectralTab.onClick = [this]
    {
        if (onViewModeRequested)
            onViewModeRequested(ViewMode::spectral);
        else
            setViewMode(ViewMode::spectral);
    };
    setViewButtonIcon(spectralTab, ModernNorthStarAssetId::spectrum);

    addAndMakeVisible(stackedViewButton);
    stackedViewButton.setButtonText("Stacked");
    stackedViewButton.setComponentID("core.waveform.channels.stacked");
    stackedViewButton.setTitle("Stacked channels");
    stackedViewButton.setClickingTogglesState(true);
    stackedViewButton.setRadioGroupId(1002);
    stackedViewButton.setToggleState(true, juce::dontSendNotification);
    stackedViewButton.onClick = [this]
    {
        if (onDisplayModeRequested)
            onDisplayModeRequested(DisplayMode::stacked);
        else
            setDisplayMode(DisplayMode::stacked);
    };

    addAndMakeVisible(mixedViewButton);
    mixedViewButton.setButtonText("Mixed");
    mixedViewButton.setComponentID("core.waveform.channels.mixed");
    mixedViewButton.setTitle("Mixed channels");
    mixedViewButton.setClickingTogglesState(true);
    mixedViewButton.setRadioGroupId(1002);
    mixedViewButton.onClick = [this]
    {
        if (onDisplayModeRequested)
            onDisplayModeRequested(DisplayMode::mixed);
        else
            setDisplayMode(DisplayMode::mixed);
    };
}

WaveformViewComponent::~WaveformViewComponent()
{
    waveformModel.removeChangeListener(this);
}

void WaveformViewComponent::repaintAndNotify()
{
    refreshOverview();
    repaint();
    if (onVisualContentChanged) onVisualContentChanged();
}

void WaveformViewComponent::repaintAndNotify(juce::Rectangle<int> bounds)
{
    refreshOverview();
    repaint(bounds);
    if (onVisualContentChanged) onVisualContentChanged();
}

void WaveformViewComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    // Optional skins paint the whole tile themselves, possibly translucent.
    const bool skinTile = !SkinStyle::isOriginal() && !ModernAppleLookAndFeel::isStartupSkin();
    if (!skinTile)
    {
        g.setColour(ModernAppleLookAndFeel::panelBackground());
        g.fillRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::cardRadius());
    }

    if (!RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin())
    {
        juce::Graphics::ScopedSaveState state(g);
        juce::Path card;
        card.addRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::cardRadius());
        g.reduceClipRegion(card);
        ModernPanelPainter::paint(g, getLocalBounds(), {}, ModernPanelPainter::Icon::waveform);
    }
    if (skinTile)
        SkinStyle::paintPanelOutline(g, bounds.reduced(0.5f), false);

    auto rulerBounds = getRulerBounds();
    auto plotBounds = getPlotBounds();

    if (const auto state = waveformModel.getState(); state->info.isValid())
    {
        const auto duration = state->info.durationSeconds;
        const auto visibleRange = getVisibleTimeRange(duration);
        drawTimeRuler(g, rulerBounds, waveformContent(plotBounds), duration);
        if (viewMode == ViewMode::spectral && corePresentationReady)
            drawSpectrogram(g, plotBounds, *state);
        else
        {
            drawWaveforms(g, plotBounds, *state);
            notifyWaveformPresentationIfNeeded();
            drawEnvelope(g, plotBounds, *state);
        }

        const auto waveformBounds = waveformContent(plotBounds);
        const auto position = juce::jlimit(0.0, duration, engine.getCurrentPositionSeconds());
        const auto cursorX = secondsToX(position, waveformBounds, visibleRange);
        const bool modern = usesModernWaveform();
        if (modern && selectionRange.has_value())
        {
            g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.8f));
            for (const auto time : { selectionRange->getStart(), selectionRange->getEnd() })
                if (time >= visibleRange.getStart() && time <= visibleRange.getEnd())
                {
                    const auto x = secondsToX(time, waveformBounds, visibleRange);
                    g.drawLine(x, rulerBounds.getY(), x, waveformBounds.getBottom(), 1);
                }
        }

        if (position >= visibleRange.getStart() && position <= visibleRange.getEnd())
        {
            g.setColour(modern ? ModernAppleLookAndFeel::accentBlue() : ModernAppleLookAndFeel::accentOrange());
            g.drawVerticalLine(static_cast<int>(cursorX), rulerBounds.getY(), waveformBounds.getBottom());
            if (modern && waveformBounds.getWidth() >= 100)
            {
                const auto text = formatRulerLabel(position, 0.001);
                const auto font = ModernTypography::font(10.0f);
                const auto width = juce::jmin(116.0f, juce::GlyphArrangement::getStringWidth(font, text) + 10);
                const auto left = juce::jlimit(waveformBounds.getX(), waveformBounds.getRight() - width, cursorX - width * 0.5f);
                const juce::Rectangle<float> tag(left, rulerBounds.getY() + 1, width, 15);
                g.fillRoundedRectangle(tag, 3);
                g.setFont(font);
                g.setColour(juce::Colours::white);
                g.drawText(text, tag, juce::Justification::centred, false);
            }
        }

    }
    else
    {
        drawEmptyState(g, plotBounds);
    }
}

void WaveformViewComponent::setSimplePresentation(bool enabled)
{
    simplePresentation = enabled;
    resized();
    repaint();
}

juce::Component::WindowControlKind WaveformViewComponent::findControlAtPoint(juce::Point<float> point) const
{
    if (simplePresentation && getHeaderBounds().contains(point)
        && !viewOptionsButton.getBounds().toFloat().contains(point)
        && !returnToGeneralButton.getBounds().toFloat().contains(point))
        return WindowControlKind::caption;
    return WindowControlKind::client;
}

void WaveformViewComponent::resized()
{
    const bool modern = !RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin();
    waveformTitle.setVisible(modern);
    selectionSummary.setVisible(modern);
    viewOptionsButton.setVisible(modern);
    returnToGeneralButton.setVisible(modern && simplePresentation);
    for (auto* button : { &waveformTab, &spectralTab, &stackedViewButton, &mixedViewButton })
        button->setVisible(!modern);
    if (modern)
    {
        getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
        auto header = getHeaderBounds().toNearestInt();
        header.removeFromLeft(30);
        if (simplePresentation)
        {
            returnToGeneralButton.setBounds(header.removeFromLeft(48).reduced(0, 4));
            header.removeFromLeft(8);
        }
        header.removeFromRight(30);
        viewOptionsButton.setBounds(header.removeFromRight(54).reduced(0, 4));
        header.removeFromRight(6);
        waveformTitle.setBounds(header.removeFromLeft(juce::jmin(header.getWidth(), getWidth() < 500 ? 44 : 76)));
        waveformTitle.setText(getWidth() < 500 ? "Wave" : "Waveform", juce::dontSendNotification);
        selectionSummary.setBounds(header);
        selectionSummary.setVisible(header.getWidth() >= 80);
        for (auto* button : { &waveformTab, &spectralTab, &stackedViewButton, &mixedViewButton })
            button->setBounds({});
        if (!overview)
        {
            overview = std::make_unique<WaveformOverviewComponent>();
            addChildComponent(*overview);
            overview->onPanRequested = [this] (double start)
            {
                panVisibleRangeBy(start - visibleRangeStartSeconds);
            };
            overview->onActionRequested = [this] (WaveformOverviewComponent::Action action)
            {
                if (onNavigationRequested) onNavigationRequested(action);
                else if (action == WaveformOverviewComponent::Action::reset) resetView();
                else zoomHorizontal(action == WaveformOverviewComponent::Action::zoomIn ? 1.35 : 1.0 / 1.35);
            };
        }
        overview->setBounds(getOverviewBounds());
        overview->setVisible(!overview->getBounds().isEmpty());
        overview->setNavigationEnabled(navigationEnabled);
        refreshOverview();
        return;
    }
    overview.reset();
    auto header = getHeaderBounds().toNearestInt();
    header.removeFromLeft(72);
    header.removeFromRight(72);
    auto left = header.removeFromLeft(260);
    auto right = header.removeFromRight(234);

    constexpr auto iconTabWidth = 46;
    waveformTab.setBounds(left.removeFromLeft(iconTabWidth));
    left.removeFromLeft(8);
    spectralTab.setBounds(left.removeFromLeft(iconTabWidth));

    mixedViewButton.setBounds(right.removeFromRight(98));
    right.removeFromRight(8);
    stackedViewButton.setBounds(right.removeFromRight(110));
}

void WaveformViewComponent::mouseDown(const juce::MouseEvent& event)
{
    traceNativeFocus("wave.mouseDown", *this);
    if (!waveformModel.hasWaveform() || !getInteractiveBounds().contains(event.position))
        return;

    if (!hasEditableAudioState(waveformModel.getState()))
        return;

    if (event.mods.isLeftButtonDown())
    {
        const auto channelButton = hitTestChannelButton(event.position);
        if (channelButton.type != ChannelButtonHit::Type::none)
        {
            if (channelButton.type == ChannelButtonHit::Type::solo)
                engine.setSourceChannelSoloed(channelButton.channel, !engine.isSourceChannelSoloed(channelButton.channel));
            else if (channelButton.type == ChannelButtonHit::Type::mute)
                engine.setSourceChannelMuted(channelButton.channel, !engine.isSourceChannelMuted(channelButton.channel));

            repaintAndNotify(getInteractiveBounds().toNearestInt());
            return;
        }

        if (const auto sourceChannel = hitTestChannelLaneLabel(event.position);
            sourceChannel >= 0)
        {
            if (onSourceChannelSelectionGesture)
                onSourceChannelSelectionGesture(sourceChannel, event.mods.isCtrlDown());
            selectionRange = juce::Range<double>(0.0, waveformModel.getLengthInSeconds());
            previewSelectionGainLinear = 1.0f;
            selectionDragActive = false;
            notifySelectionChanged();
            repaintAndNotify();
            return;
        }
    }

    if (event.mods.isMiddleButtonDown() || (event.mods.isLeftButtonDown() && event.mods.isAltDown()))
    {
        panDragActive = true;
        selectionDragActive = false;
        draggingEnvelopePoint = -1;
        panDragAnchorX = event.position.x;
        panDragStartVisibleStart = visibleRangeStartSeconds;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    if (viewMode == ViewMode::waveform && event.mods.isRightButtonDown())
    {
        const auto hitPoint = findEnvelopePointAt(event.position);
        if (hitPoint > 0)
            document.removeEnvelopePoint(hitPoint);
        else if (getPlotBounds().contains(event.position))
            document.insertEnvelopePoint(xToSeconds(event.position.x,
                                                    waveformContent(getPlotBounds()),
                                                    getVisibleTimeRange(waveformModel.getLengthInSeconds())));
        return;
    }

    mouseDownPosition = event.position;
    pointerMovedDuringSelection = false;

    draggingEnvelopePoint = viewMode == ViewMode::waveform ? findEnvelopePointAt(event.position) : -1;
    if (draggingEnvelopePoint >= 0)
    {
        document.beginEnvelopeGesture();
        selectionDragActive = false;
        return;
    }

    selectionDragActive = true;
    selectionAnchorSeconds = xToSeconds(event.position.x,
                                        waveformContent(getPlotBounds()),
                                        getVisibleTimeRange(waveformModel.getLengthInSeconds()));
}

void WaveformViewComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (panDragActive)
    {
        const auto duration = waveformModel.getLengthInSeconds();
        const auto visibleRange = getVisibleTimeRange(duration);
        const auto plotBounds = waveformContent(getPlotBounds());
        if (plotBounds.getWidth() > 0.0f && duration > 0.0)
        {
            const auto secondsPerPixel = visibleRange.getLength() / plotBounds.getWidth();
            visibleRangeStartSeconds = panDragStartVisibleStart - ((event.position.x - panDragAnchorX) * secondsPerPixel);
            panVisibleRangeBy(0.0);
        }
        return;
    }

    if (draggingEnvelopePoint >= 0)
    {
        updateEnvelopePointFromMouse(event);
        return;
    }

    if (!selectionDragActive || !waveformModel.hasWaveform())
        return;

    if (!pointerMovedDuringSelection && mouseDownPosition.getDistanceFrom(event.position) > 4.0f)
        pointerMovedDuringSelection = true;

    if (!pointerMovedDuringSelection)
        return;

    updateSelectionFromPosition(event.position.x);
}

void WaveformViewComponent::mouseUp(const juce::MouseEvent& event)
{
    if (panDragActive)
    {
        panDragActive = false;
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }

    if (draggingEnvelopePoint >= 0)
    {
        updateEnvelopePointFromMouse(event);
        document.endEnvelopeGesture();
        draggingEnvelopePoint = -1;
        return;
    }

    if (!selectionDragActive || !waveformModel.hasWaveform())
        return;

    selectionDragActive = false;

    if (!pointerMovedDuringSelection)
    {
        clearSelection();
        auto target = xToSeconds(event.position.x,
                                 waveformContent(getPlotBounds()),
                                 getVisibleTimeRange(waveformModel.getLengthInSeconds()));
        seekToTime(target);
        return;
    }

    updateSelectionFromPosition(event.position.x);

    if (selectionRange.has_value() && selectionRange->getLength() < 0.01)
        clearSelection();
}

void WaveformViewComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (!waveformModel.hasWaveform() || !getInteractiveBounds().contains(event.position))
        return;

    const auto duration = waveformModel.getLengthInSeconds();
    if (duration <= 0.0)
        return;

    if (event.mods.isShiftDown())
    {
        const auto factor = wheel.deltaY > 0.0f ? 1.12f : (wheel.deltaY < 0.0f ? 1.0f / 1.12f : 1.0f);
        if (std::abs(factor - 1.0f) > 0.0001f)
            zoomVertical(factor);
        return;
    }

    const auto plotBounds = waveformContent(getPlotBounds());
    const auto visibleRange = getVisibleTimeRange(duration);
    const auto focusSeconds = xToSeconds(event.position.x, plotBounds, visibleRange);

    if (std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
    {
        panVisibleRangeBy(-(visibleRange.getLength() * wheel.deltaX * 0.35));
        return;
    }

    const auto factor = wheel.deltaY > 0.0f ? 1.18 : (wheel.deltaY < 0.0f ? (1.0 / 1.18) : 1.0);
    if (std::abs(factor - 1.0) > 0.0001)
        zoomHorizontal(factor, focusSeconds);
}

void WaveformViewComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (viewMode != ViewMode::waveform || !waveformModel.hasWaveform() || !hasEditableAudioState(waveformModel.getState()))
        return;

    if (!waveformContent(getPlotBounds()).contains(event.position))
        return;

    const auto time = xToSeconds(event.position.x,
                                 waveformContent(getPlotBounds()),
                                 getVisibleTimeRange(waveformModel.getLengthInSeconds()));
    document.insertEnvelopePoint(time);
}

bool WaveformViewComponent::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto commandOrCtrl = modifiers.isCommandDown() || modifiers.isCtrlDown();
    if (commandOrCtrl
        && !modifiers.isAltDown()
        && key.getKeyCode() == 'A')
    {
        if (onWholeFileSelectionGesture)
            onWholeFileSelectionGesture();
        selectWholeFile();
        return true;
    }
    return false;
}

void WaveformViewComponent::refreshNow()
{
    lastCursorSeconds = engine.getCurrentPositionSeconds();
    if (waveformModel.hasWaveform())
        ensureTimeVisible(lastCursorSeconds, waveformModel.getLengthInSeconds());
    repaintAndNotify(getInteractiveBounds().toNearestInt());
}

void WaveformViewComponent::clearSelection()
{
    if (!selectionRange.has_value())
        return;

    selectionRange.reset();
    previewSelectionGainLinear = 1.0f;
    notifySelectionChanged();
    repaintAndNotify();
}

void WaveformViewComponent::restoreSelection(std::optional<juce::Range<double>> selection)
{
    selectionRange.reset();
    if (selection && waveformModel.getLengthInSeconds() > 0.0)
    {
        const auto bounded = selection->getIntersectionWith({ 0.0, waveformModel.getLengthInSeconds() });
        if (!bounded.isEmpty()) selectionRange = bounded;
    }
    selectionDragActive = false;
    previewSelectionGainLinear = 1.0f;
    notifySelectionChanged();
    repaintAndNotify();
}

void WaveformViewComponent::selectWholeFile()
{
    if (!waveformModel.hasWaveform() || waveformModel.getLengthInSeconds() <= 0.0)
        return;

    selectedSourceChannels.clear();
    selectionRange = juce::Range<double>(0.0, waveformModel.getLengthInSeconds());
    previewSelectionGainLinear = 1.0f;
    notifySelectionChanged();
    repaintAndNotify();
}

void WaveformViewComponent::setSelectedSourceChannels(std::vector<int> sourceChannels)
{
    std::sort(sourceChannels.begin(), sourceChannels.end());
    sourceChannels.erase(std::unique(sourceChannels.begin(), sourceChannels.end()),
                         sourceChannels.end());
    if (selectedSourceChannels == sourceChannels)
        return;

    selectedSourceChannels = std::move(sourceChannels);
    repaintAndNotify(getInteractiveBounds().toNearestInt());
}

std::optional<juce::Range<double>> WaveformViewComponent::getSelectionRange() const noexcept
{
    return selectionRange;
}

std::optional<juce::Rectangle<float>> WaveformViewComponent::getSelectionBoundsInLocal() const noexcept
{
    if (!selectionRange.has_value() || !waveformModel.hasWaveform())
        return std::nullopt;

    const auto waveformBounds = waveformContent(getPlotBounds());
    const auto visibleRange = getVisibleTimeRange(waveformModel.getLengthInSeconds());
    const auto clipped = selectionRange->getIntersectionWith(visibleRange);
    if (clipped.isEmpty())
        return std::nullopt;

    const auto left = secondsToX(clipped.getStart(), waveformBounds, visibleRange);
    const auto right = secondsToX(clipped.getEnd(), waveformBounds, visibleRange);
    return juce::Rectangle<float>(left, waveformBounds.getY(), right - left, waveformBounds.getHeight());
}

WaveformViewComponent::ViewMode WaveformViewComponent::getViewMode() const noexcept
{
    return viewMode;
}

void WaveformViewComponent::setViewMode(ViewMode mode)
{
    viewMode = mode;
    waveformTab.setToggleState(viewMode == ViewMode::waveform, juce::dontSendNotification);
    spectralTab.setToggleState(viewMode == ViewMode::spectral, juce::dontSendNotification);

    prepareRequestedSpectrogram(true);

    repaintAndNotify();
}

WaveformViewComponent::DisplayMode WaveformViewComponent::getDisplayMode() const noexcept
{
    return displayMode;
}

void WaveformViewComponent::setDisplayMode(DisplayMode mode)
{
    displayMode = mode;
    stackedViewButton.setToggleState(displayMode == DisplayMode::stacked, juce::dontSendNotification);
    mixedViewButton.setToggleState(displayMode == DisplayMode::mixed, juce::dontSendNotification);
    repaintAndNotify();
}

void WaveformViewComponent::setSelectionPreviewGain(float linearGain) noexcept
{
    previewSelectionGainLinear = linearGain;
    repaintAndNotify(getPlotBounds().toNearestInt());
}

void WaveformViewComponent::clearSelectionPreviewGain() noexcept
{
    previewSelectionGainLinear = 1.0f;
    repaintAndNotify(getPlotBounds().toNearestInt());
}

void WaveformViewComponent::setLoadingHint(juce::String primaryLine, juce::String secondaryLine)
{
    if (!openFailureHint && loadingPrimaryText == primaryLine && loadingSecondaryText == secondaryLine)
        return;

    openFailureHint = false;
    loadingPrimaryText = std::move(primaryLine);
    loadingSecondaryText = std::move(secondaryLine);
    repaintAndNotify(getPlotBounds().toNearestInt());
}

void WaveformViewComponent::setOpenFailureHint(juce::String fileName, juce::String reason)
{
    if (openFailureHint && loadingPrimaryText == fileName && loadingSecondaryText == reason)
        return;
    openFailureHint = true;
    loadingPrimaryText = std::move(fileName);
    loadingSecondaryText = std::move(reason);
    repaintAndNotify(getPlotBounds().toNearestInt());
}

void WaveformViewComponent::setCommandAvailability(bool viewModeEnabled,
                                                   bool displayModeEnabled)
{
    navigationEnabled = viewModeEnabled;
    if (overview) overview->setNavigationEnabled(navigationEnabled);
    waveformTab.setEnabled(viewModeEnabled);
    spectralTab.setEnabled(viewModeEnabled);
    stackedViewButton.setEnabled(displayModeEnabled);
    mixedViewButton.setEnabled(displayModeEnabled);
}

juce::String WaveformViewComponent::getAccessibilityValueSummary() const
{
    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid())
    {
        if (openFailureHint)
            return "Could not open " + loadingPrimaryText + ". " + loadingSecondaryText;
        return "No audio document";
    }

    const auto duration = juce::jmax(state->info.durationSeconds,
                                     engine.getDurationSeconds());
    const auto position = juce::jlimit(0.0, duration,
                                       engine.getCurrentPositionSeconds());
    const auto playable = engine.getPlayableTimeRange();
    auto summary = "Position " + juce::String(position, 3)
                 + " seconds of " + juce::String(duration, 3) + " seconds";
    if (viewMode == ViewMode::spectral)
    {
        const auto status = waveformModel.getSpectrogramStatus();
        if (status == WaveformModel::SpectrogramStatus::failed)
            summary += "; spectrum unavailable; choose View, Spectral to retry; waveform remains visible";
        else if (status == WaveformModel::SpectrogramStatus::preparing)
            summary += "; preparing spectrum; waveform remains visible";
    }
    if (!playable.isEmpty())
    {
        summary += "; playable range " + juce::String(playable.getStart(), 3)
                 + " to " + juce::String(playable.getEnd(), 3) + " seconds";
    }
    if (selectionRange.has_value())
    {
        summary += "; selection " + juce::String(selectionRange->getStart(), 3)
                 + " to " + juce::String(selectionRange->getEnd(), 3) + " seconds";
    }
    else
    {
        summary += "; no selection";
    }
    const auto visible = getVisibleTimeRange(duration);
    summary += "; visible range " + juce::String(visible.getStart(), 3)
        + " to " + juce::String(visible.getEnd(), 3) + " seconds";
    if (selectedSourceChannels.empty())
    {
        summary += "; all source channels";
    }
    else
    {
        summary += "; source channels";
        for (const auto channel : selectedSourceChannels)
            summary += " " + juce::String(channel + 1);
    }
    return summary;
}

std::unique_ptr<juce::AccessibilityHandler> WaveformViewComponent::createAccessibilityHandler()
{
    std::unique_ptr<juce::AccessibilityValueInterface> value =
        std::make_unique<WaveformAccessibilityValue>(*this);
    juce::AccessibilityActions actions;
    actions.addAction(juce::AccessibilityActionType::press,
                      [this]
                      {
                          if (onWholeFileSelectionGesture)
                              onWholeFileSelectionGesture();
                          selectWholeFile();
                      });
    return std::make_unique<juce::AccessibilityHandler>(
        *this,
        juce::AccessibilityRole::group,
        std::move(actions),
        juce::AccessibilityHandler::Interfaces(std::move(value)));
}

void WaveformViewComponent::zoomHorizontal(double factor, std::optional<double> focusSeconds)
{
    const auto duration = waveformModel.getLengthInSeconds();
    if (duration <= 0.0)
        return;

    const auto targetZoom = juce::jlimit(1.0, 512.0, horizontalZoom * factor);
    if (std::abs(targetZoom - horizontalZoom) < 0.001)
        return;

    const auto previousRange = getVisibleTimeRange(duration);
    const auto focus = juce::jlimit(previousRange.getStart(),
                                    previousRange.getEnd(),
                                    focusSeconds.value_or(engine.getCurrentPositionSeconds()));
    const auto ratio = previousRange.getLength() > 0.0
                     ? (focus - previousRange.getStart()) / previousRange.getLength()
                     : 0.5;

    horizontalZoom = targetZoom;
    const auto newRangeLength = duration / horizontalZoom;
    visibleRangeStartSeconds = focus - (newRangeLength * ratio);
    ensureTimeVisible(focus, duration);
    repaintAndNotify(getInteractiveBounds().toNearestInt());
}

void WaveformViewComponent::zoomVertical(float factor) noexcept
{
    verticalZoom = juce::jlimit(0.5f, 12.0f, verticalZoom * factor);
    repaintAndNotify(getPlotBounds().toNearestInt());
}

void WaveformViewComponent::seekToTime(double seconds)
{
    if (!std::isfinite(seconds) || !engine.hasLoadedFile()) return;
    auto target = juce::jlimit(0.0, engine.getDurationSeconds(), seconds);
    if (const auto loadedPreviewRange = getLoadedPreviewTimeRange(); loadedPreviewRange.has_value())
        target = juce::jlimit(loadedPreviewRange->getStart(), loadedPreviewRange->getEnd(), target);
    engine.seekToSeconds(target);
    ensureTimeVisible(target, engine.getDurationSeconds());
    refreshNow();
}

void WaveformViewComponent::resetView() noexcept
{
    horizontalZoom = 1.0;
    verticalZoom = 1.0f;
    visibleRangeStartSeconds = 0.0;
    lastAutoFocusedPreviewRevision = -1;
    repaintAndNotify();
}

std::optional<juce::Range<double>> WaveformViewComponent::getLoadedPreviewTimeRange() const noexcept
{
    const auto peakCache = waveformModel.getPeakCache();
    if (peakCache == nullptr
        || !peakCache->isPreview
        || peakCache->sampleRate <= 0.0
        || peakCache->loadedSampleRange.getLength() <= 0
        || peakCache->loadedSampleRange.getEnd() >= peakCache->totalSamples)
    {
        return std::nullopt;
    }

    return juce::Range<double>(static_cast<double>(peakCache->loadedSampleRange.getStart()) / peakCache->sampleRate,
                               static_cast<double>(peakCache->loadedSampleRange.getEnd()) / peakCache->sampleRate);
}

juce::Range<double> WaveformViewComponent::getVisibleTimeRange(double durationSeconds) const noexcept
{
    if (durationSeconds <= 0.0)
        return { 0.0, 0.0 };

    const auto visibleLength = juce::jlimit(0.0001, durationSeconds, durationSeconds / horizontalZoom);
    const auto maxStart = juce::jmax(0.0, durationSeconds - visibleLength);
    const auto start = juce::jlimit(0.0, maxStart, visibleRangeStartSeconds);
    return { start, start + visibleLength };
}

void WaveformViewComponent::ensureTimeVisible(double seconds, double durationSeconds) noexcept
{
    if (durationSeconds <= 0.0)
    {
        visibleRangeStartSeconds = 0.0;
        return;
    }

    const auto visibleLength = juce::jlimit(0.0001, durationSeconds, durationSeconds / horizontalZoom);
    auto start = visibleRangeStartSeconds;
    auto end = start + visibleLength;

    if (seconds < start)
    {
        start = seconds - visibleLength * 0.1;
        end = start + visibleLength;
    }
    else if (seconds > end)
    {
        start = seconds - visibleLength * 0.9;
        end = start + visibleLength;
    }

    const auto maxStart = juce::jmax(0.0, durationSeconds - visibleLength);
    visibleRangeStartSeconds = juce::jlimit(0.0, maxStart, start);
}

juce::Rectangle<float> WaveformViewComponent::getHeaderBounds() const
{
    if (!RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin())
        return getLocalBounds().toFloat().withHeight(32);
    return getLocalBounds().toFloat().reduced(18.0f, 14.0f).removeFromTop(30.0f);
}

juce::Rectangle<float> WaveformViewComponent::getRulerBounds() const
{
    if (!RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin())
        return getLocalBounds().toFloat().withTrimmedTop(32).withHeight(24);
    auto bounds = getLocalBounds().toFloat().reduced(18.0f, 14.0f);
    bounds.removeFromTop(38.0f);
    return bounds.removeFromTop(30.0f);
}

juce::Rectangle<float> WaveformViewComponent::getPlotBounds() const
{
    if (!RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin())
        return getLocalBounds().toFloat().withTrimmedTop(56)
            .withTrimmedBottom(getOverviewBounds().isEmpty() ? 0.0f : 34.0f);
    auto bounds = getLocalBounds().toFloat().reduced(18.0f, 14.0f);
    bounds.removeFromTop(38.0f);
    bounds.removeFromTop(30.0f);
    return bounds;
}

juce::Rectangle<int> WaveformViewComponent::getOverviewBounds() const
{
    if (RendererVisualStyle::isClassic() || ModernAppleLookAndFeel::isStartupSkin()
        || getWidth() < 180 || getHeight() < 145) return {};
    return getLocalBounds().reduced(6, 4).removeFromBottom(26);
}

void WaveformViewComponent::refreshOverview()
{
    if (!overview || !overview->isVisible()) return;
    const auto state = waveformModel.getState();
    const auto duration = waveformModel.getLengthInSeconds();
    overview->setState(waveformModel.getPeakCache(), state != nullptr ? state->audioRevision : -1,
                        duration, getVisibleTimeRange(duration), engine.getCurrentPositionSeconds());
}

juce::Rectangle<float> WaveformViewComponent::getInteractiveBounds() const
{
    auto result = getRulerBounds().getUnion(getPlotBounds());
    if (usesModernWaveform())
    {
        const auto content = waveformContent(getPlotBounds());
        result.setX(content.getX());
        result.setWidth(content.getWidth());
    }
    return result;
}

float WaveformViewComponent::secondsToX(double seconds, juce::Rectangle<float> bounds, juce::Range<double> visibleRange) const
{
    return WaveformPlotGeometry::timeToX(seconds, bounds, visibleRange);
}

double WaveformViewComponent::xToSeconds(float x, juce::Rectangle<float> bounds, juce::Range<double> visibleRange) const
{
    return WaveformPlotGeometry::xToTime(x, bounds, visibleRange);
}

float WaveformViewComponent::gainToY(float gain, juce::Rectangle<float> plotBounds, float zoomFactor) const
{
    const auto centerY = plotBounds.getCentreY();
    const auto maxOffset = plotBounds.getHeight() * 0.38f * zoomFactor;
    const auto clampedGain = juce::jlimit(0.0f, 2.0f, gain);
    return centerY - ((clampedGain - 1.0f) * maxOffset);
}

float WaveformViewComponent::yToGain(float y, juce::Rectangle<float> plotBounds, float zoomFactor) const
{
    const auto centerY = plotBounds.getCentreY();
    const auto maxOffset = juce::jmax(1.0f, plotBounds.getHeight() * 0.38f * zoomFactor);
    return juce::jlimit(0.0f, 2.0f, 1.0f + ((centerY - y) / maxOffset));
}

int WaveformViewComponent::findEnvelopePointAt(juce::Point<float> position) const
{
    const auto state = waveformModel.getState();
    if (!hasEditableAudioState(state))
        return -1;

    const auto plotBounds = waveformContent(getPlotBounds());
    const auto visibleRange = getVisibleTimeRange(state->info.durationSeconds);
    for (int i = 0; i < static_cast<int>(state->envelopePoints.size()); ++i)
    {
        const auto& point = state->envelopePoints[static_cast<std::size_t>(i)];
        if (point.timeSeconds < visibleRange.getStart() || point.timeSeconds > visibleRange.getEnd())
            continue;

        const auto pointX = secondsToX(point.timeSeconds, plotBounds, visibleRange);
        const auto pointY = gainToY(point.gain, plotBounds, verticalZoom);
        if (juce::Point<float>(pointX, pointY).getDistanceFrom(position) <= 9.0f)
            return i;
    }

    return -1;
}

WaveformViewComponent::ChannelButtonHit WaveformViewComponent::hitTestChannelButton(juce::Point<float> position) const
{
    ChannelButtonHit result;
    if (displayMode != DisplayMode::stacked)
        return result;

    const auto state = waveformModel.getState();
    if (state == nullptr || !state->info.isValid() || state->info.numChannels <= 1)
        return result;

    const auto channelCount = juce::jmax(1, state->info.numChannels);
    const auto waveformBounds = waveformContent(getPlotBounds());
    if (!waveformBounds.contains(position))
        return result;

    const auto gap = juce::jmin(12.0f, waveformBounds.getHeight() * 0.025f);
    const auto laneHeight = (waveformBounds.getHeight() - gap * static_cast<float>(channelCount - 1)) / static_cast<float>(channelCount);
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto laneY = waveformBounds.getY() + channel * (laneHeight + gap);
        auto laneBounds = juce::Rectangle<float>(waveformBounds.getX(), laneY, waveformBounds.getWidth(), laneHeight);
        if (!laneBounds.contains(position))
            continue;

        const auto controls = WaveformLaneControls::calculate(laneBounds,
            !RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin());

        if (controls.solo.contains(position))
            return { channel, ChannelButtonHit::Type::solo };
        if (controls.mute.contains(position))
            return { channel, ChannelButtonHit::Type::mute };

        return result;
    }

    return result;
}

int WaveformViewComponent::hitTestChannelLaneLabel(juce::Point<float> position) const
{
    if (displayMode != DisplayMode::stacked)
        return -1;

    const auto state = waveformModel.getState();
    if (state == nullptr || !state->info.isValid() || state->info.numChannels <= 1)
        return -1;

    const auto channelCount = state->info.numChannels;
    const auto waveformBounds = waveformContent(getPlotBounds());
    const auto gap = juce::jmin(12.0f, waveformBounds.getHeight() * 0.025f);
    const auto laneHeight = (waveformBounds.getHeight()
                           - gap * static_cast<float>(channelCount - 1))
                          / static_cast<float>(channelCount);
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto laneY = waveformBounds.getY() + channel * (laneHeight + gap);
        const auto laneBounds = juce::Rectangle<float>(waveformBounds.getX(), laneY,
                                                        waveformBounds.getWidth(), laneHeight);
        const auto controls = WaveformLaneControls::calculate(laneBounds,
            !RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin());
        if (controls.label.contains(position))
            return channel;
    }
    return -1;
}

void WaveformViewComponent::drawTimeRuler(juce::Graphics& g,
                                          juce::Rectangle<float> rulerBounds,
                                          juce::Rectangle<float> waveformBounds,
                                          double durationSeconds) const
{
    if (usesModernWaveform())
    {
        const auto visible = getVisibleTimeRange(durationSeconds);
        if (visible.isEmpty() || waveformBounds.isEmpty()) return;
        if (selectionRange.has_value())
        {
            const auto selected = selectionRange->getIntersectionWith(visible);
            if (!selected.isEmpty())
            {
                const auto left = secondsToX(selected.getStart(), waveformBounds, visible);
                const auto right = secondsToX(selected.getEnd(), waveformBounds, visible);
                g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.12f));
                g.fillRect(left, rulerBounds.getY(), right - left, rulerBounds.getHeight());
            }
        }
        const auto majorStep = WaveformPlotGeometry::rulerStep(visible.getLength(), waveformBounds.getWidth());
        const int divisions = majorStep >= 15.0 ? 6 : 5;
        const auto minorStep = majorStep / divisions;
        const auto first = std::ceil(visible.getStart() / minorStep);
        const auto count = juce::jmin(1024, static_cast<int>(std::ceil(visible.getLength() / minorStep)) + 1);
        const auto rulerFont = ModernTypography::font(11.0f);
        g.setFont(rulerFont);
        for (int i = 0; i < count; ++i)
        {
            const auto t = (first + i) * minorStep;
            if (t > visible.getEnd() + 0.0000001) break;
            const auto x = secondsToX(t, waveformBounds, visible);
            const bool major = std::abs(t / majorStep - std::round(t / majorStep)) < 0.0001;
            g.setColour(ModernAppleLookAndFeel::borderColour().withAlpha(major ? 0.85f : 0.50f));
            g.drawLine(x, rulerBounds.getBottom() - (major ? 7.0f : 3.0f), x, rulerBounds.getBottom(), 1);
            if (major)
            {
                g.setColour(ModernAppleLookAndFeel::secondaryText());
                const auto text = formatRulerLabel(t, majorStep);
                const auto textBounds = WaveformPlotGeometry::rulerLabelBounds(x, rulerBounds, waveformBounds,
                    juce::GlyphArrangement::getStringWidth(rulerFont, text));
                if (!textBounds.isEmpty())
                    g.drawText(text, textBounds, juce::Justification::centredLeft, false);
            }
        }
        return;
    }
    g.setColour(ModernAppleLookAndFeel::workspaceSurface().withAlpha(0.82f));
    g.fillRoundedRectangle(rulerBounds.reduced(0.0f, 1.5f), 14.0f);
    const auto visibleRange = getVisibleTimeRange(durationSeconds);

    if (selectionRange.has_value())
    {
        const auto clipped = selectionRange->getIntersectionWith(visibleRange);
        if (!clipped.isEmpty())
        {
            const auto selectionX = secondsToX(clipped.getStart(), waveformBounds, visibleRange);
            const auto selectionRight = secondsToX(clipped.getEnd(), waveformBounds, visibleRange);
            const auto selectionWidth = selectionRight - selectionX;
            g.setColour(juce::Colours::black.withAlpha(0.05f));
            g.fillRect(juce::Rectangle<float>(waveformBounds.getX(),
                                              rulerBounds.getY() + 3.0f,
                                              juce::jmax(0.0f, selectionX - waveformBounds.getX()),
                                              rulerBounds.getHeight() - 6.0f));
            g.fillRect(juce::Rectangle<float>(selectionRight,
                                              rulerBounds.getY() + 3.0f,
                                              juce::jmax(0.0f, waveformBounds.getRight() - selectionRight),
                                              rulerBounds.getHeight() - 6.0f));
            g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.18f));
            g.fillRoundedRectangle({ selectionX, rulerBounds.getY() + 3.0f, selectionWidth, rulerBounds.getHeight() - 6.0f }, 10.0f);
        }
    }

    const auto stepSeconds = chooseRulerStep(visibleRange.getLength(), waveformBounds.getWidth());
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(12.5f)));

    const auto firstTick = std::floor(visibleRange.getStart() / stepSeconds) * stepSeconds;
    for (double t = firstTick; t <= visibleRange.getEnd() + stepSeconds * 0.5; t += stepSeconds)
    {
        if (t < visibleRange.getStart() - 0.0001)
            continue;

        const auto x = secondsToX(t, waveformBounds, visibleRange);
        if (x <= waveformBounds.getX() + 2.0f || x >= waveformBounds.getRight() - 2.0f)
            continue;
        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.72f));
        g.drawFittedText(formatRulerLabel(t, stepSeconds),
                         juce::Rectangle<int>(static_cast<int>(x) + 4, static_cast<int>(rulerBounds.getY()) + 6, 92, static_cast<int>(rulerBounds.getHeight()) - 8),
                         juce::Justification::topLeft,
                         1);
    }
}

void WaveformViewComponent::drawBackgroundGrid(juce::Graphics& g, juce::Rectangle<float> plotBounds) const
{
    juce::ignoreUnused(g, plotBounds);
}

void WaveformViewComponent::drawWaveforms(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const
{
    auto waveformBounds = waveformContent(plotBounds);
    const auto durationSeconds = state.info.durationSeconds;
    const auto visibleRange = getVisibleTimeRange(durationSeconds);
    const auto channelCount = juce::jmax(1, state.info.numChannels);
    const auto widthPixels = juce::jmax(1, static_cast<int>(waveformBounds.getWidth()));
    const bool modern = usesModernWaveform();
    const bool showAmplitudeScale = modern && plotBounds.getWidth() >= 180;
    const auto previewOn = selectionRange.has_value() && std::abs(previewSelectionGainLinear - 1.0f) > 0.001f;
    const auto unityEnvelope = SingleTrackDocument::isEnvelopeFlatUnity(state.envelopePoints);

    const auto peakCache = waveformModel.getPeakCache();
    if (peakCache == nullptr || peakCache->audioRevision != state.audioRevision || peakCache->binCount <= 0)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.82f));
        g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        g.drawFittedText("Preparing waveform peaks...",
                         waveformBounds.toNearestInt(),
                         juce::Justification::centred,
                         1);
        return;
    }

    if (peakCache->isPreview)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.68f));
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        const auto previewText = getLoadedPreviewTimeRange().has_value()
            ? "Opening from start, loading waveform tail in background..."
            : "Waveform preview ready, refining in background...";
        g.drawFittedText(previewText,
                         waveformBounds.removeFromTop(22.0f).toNearestInt(),
                         juce::Justification::centredRight,
                         1);
    }

    const auto loadedPreviewRange = getLoadedPreviewTimeRange();
    const auto peakLevel = WaveformModel::choosePeakLevel(*peakCache, visibleRange, widthPixels);
    if (peakLevel.binCount <= 0 || peakLevel.channelBins == nullptr || peakLevel.mixedBins == nullptr)
        return;

    const auto scalePeakBin = [&](const WaveformModel::PeakBin& bin,
                                  int sourceChannelIndex)
    {
        float gainMin = 1.0f;
        float gainMax = 1.0f;

        if (!unityEnvelope)
        {
            const auto midSeconds = 0.5 * (bin.startSeconds + bin.endSeconds);
            const auto startGain = SingleTrackDocument::evaluateEnvelope(state.envelopePoints, bin.startSeconds);
            const auto midGain = SingleTrackDocument::evaluateEnvelope(state.envelopePoints, midSeconds);
            const auto endGain = SingleTrackDocument::evaluateEnvelope(state.envelopePoints, bin.endSeconds);
            gainMin = juce::jmin(startGain, juce::jmin(midGain, endGain));
            gainMax = juce::jmax(startGain, juce::jmax(midGain, endGain));
        }

        const auto previewTargetsLane = selectedSourceChannels.empty()
            || (sourceChannelIndex >= 0
                && std::find(selectedSourceChannels.begin(),
                             selectedSourceChannels.end(),
                             sourceChannelIndex) != selectedSourceChannels.end());
        if (previewOn
            && previewTargetsLane
            && selectionRange.has_value()
            && juce::Range<double>(bin.startSeconds, bin.endSeconds).intersects(*selectionRange))
        {
            gainMin *= previewSelectionGainLinear;
            gainMax *= previewSelectionGainLinear;
        }

        const auto candidateA = bin.minValue * gainMin;
        const auto candidateB = bin.minValue * gainMax;
        const auto candidateC = bin.maxValue * gainMin;
        const auto candidateD = bin.maxValue * gainMax;
        const auto scaledMin = juce::jmin(juce::jmin(candidateA, candidateB), juce::jmin(candidateC, candidateD));
        const auto scaledMax = juce::jmax(juce::jmax(candidateA, candidateB), juce::jmax(candidateC, candidateD));
        return std::pair<float, float>(scaledMin, scaledMax);
    };

    const auto drawLane = [&](juce::Rectangle<float> laneBounds,
                              const std::vector<WaveformModel::PeakBin>& bins,
                              juce::Colour colour,
                              int sourceChannelIndex)
    {
        if (bins.empty())
            return;

        const auto binTimeStart = bins.empty() ? 0.0 : bins.front().startSeconds;
        const auto binTimeEnd = bins.empty() ? durationSeconds : bins.back().endSeconds;
        const auto binTimeLength = juce::jmax(0.000001, binTimeEnd - binTimeStart);
        const auto timeToBinIndex = [&](double seconds) noexcept
        {
            return juce::jlimit(0,
                                static_cast<int>(bins.size()) - 1,
                                static_cast<int>(((seconds - binTimeStart) / binTimeLength) * bins.size()));
        };

        juce::Graphics::ScopedSaveState scope(g);
        g.reduceClipRegion(laneBounds.withWidth(laneBounds.getWidth() + (showAmplitudeScale ? 26.0f : 0.0f)).toNearestInt());
        if (!RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin())
            ModernWaveformPainter::prepareLane(g, laneBounds, colour);
        else
        {
            g.setColour(ModernAppleLookAndFeel::workspaceSurface().withAlpha(0.92f));
            g.fillRoundedRectangle(laneBounds, 14.0f);
            g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.03f));
            g.drawHorizontalLine(static_cast<int>(laneBounds.getCentreY()), laneBounds.getX(), laneBounds.getRight());
            g.setColour(colour);
        }

        for (int x = 0; x < widthPixels; ++x)
        {
            const auto sliceStart = visibleRange.getStart() + (visibleRange.getLength() * static_cast<double>(x) / widthPixels);
            const auto sliceEnd = visibleRange.getStart() + (visibleRange.getLength() * static_cast<double>(x + 1) / widthPixels);
            const auto binStart = timeToBinIndex(sliceStart);
            const auto binEnd = juce::jlimit(binStart + 1,
                                             static_cast<int>(bins.size()),
                                             static_cast<int>(std::ceil(((sliceEnd - binTimeStart) / binTimeLength) * bins.size())));

            float minValue = 1.0f;
            float maxValue = -1.0f;
            auto sawLoadedPeak = false;
            for (int binIndex = binStart; binIndex < binEnd; ++binIndex)
            {
                const auto& bin = bins[static_cast<std::size_t>(binIndex)];
                if (loadedPreviewRange.has_value()
                    && (bin.endSeconds <= loadedPreviewRange->getStart()
                        || bin.startSeconds >= loadedPreviewRange->getEnd()))
                {
                    continue;
                }

                const auto [scaledMin, scaledMax] = scalePeakBin(bin,
                                                                 sourceChannelIndex);
                minValue = juce::jmin(minValue, scaledMin);
                maxValue = juce::jmax(maxValue, scaledMax);
                sawLoadedPeak = true;
            }

            if (!sawLoadedPeak)
                continue;

            const auto lineX = laneBounds.getX() + x;
            const auto top = WaveformPlotGeometry::sampleToY(modern ? maxValue : juce::jlimit(-1.0f, 1.0f, maxValue), laneBounds, verticalZoom);
            const auto bottom = WaveformPlotGeometry::sampleToY(modern ? minValue : juce::jlimit(-1.0f, 1.0f, minValue), laneBounds, verticalZoom);
            g.drawVerticalLine(static_cast<int>(lineX), top, bottom);
        }

        if (showAmplitudeScale)
        {
            const auto axis = laneBounds.withX(laneBounds.getRight()).withWidth(26);
            if (g.clipRegionIntersects(axis.toNearestInt()))
            {
                g.setFont(ModernTypography::font(10.0f));
                for (const auto& tick : WaveformPlotGeometry::amplitudeTicks(laneBounds, verticalZoom))
                {
                    g.setColour(ModernAppleLookAndFeel::borderColour().withAlpha(0.68f));
                    g.drawLine(laneBounds.getRight(), tick.y, laneBounds.getRight() + 3, tick.y, 1);
                    g.setColour(ModernAppleLookAndFeel::secondaryText());
                    const auto label = tick.zero ? juce::String::fromUTF8("−∞")
                        : (tick.db > 0 ? "+" : "") + juce::String(juce::roundToInt(tick.db));
                    g.drawText(label, tick.label, juce::Justification::centredLeft, false);
                }
            }
        }

        if (loadedPreviewRange.has_value())
        {
            const auto unloadedStartSeconds = juce::jmax(loadedPreviewRange->getEnd(), visibleRange.getStart());
            if (unloadedStartSeconds < visibleRange.getEnd())
            {
                const auto unloadedStartX = secondsToX(unloadedStartSeconds, laneBounds, visibleRange);
                if (unloadedStartX < laneBounds.getRight())
                {
                    const auto unloadedBounds = juce::Rectangle<float>(unloadedStartX,
                                                                       laneBounds.getY(),
                                                                       laneBounds.getRight() - unloadedStartX,
                                                                       laneBounds.getHeight());
                    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.22f));
                    g.fillRect(unloadedBounds);
                    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.22f));
                    g.drawVerticalLine(static_cast<int>(std::round(unloadedStartX)),
                                       laneBounds.getY(),
                                       laneBounds.getBottom());

                    if (unloadedBounds.getWidth() >= 140.0f)
                    {
                        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.72f));
                        g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
                        g.drawFittedText("Loading tail...",
                                         unloadedBounds.reduced(10.0f, 8.0f).toNearestInt(),
                                         juce::Justification::centred,
                                         1);
                    }
                }
            }
        }
    };

    if (displayMode == DisplayMode::mixed || channelCount == 1)
    {
        drawLane(waveformBounds,
                 *peakLevel.mixedBins,
                 ModernAppleLookAndFeel::waveformBlue(),
                 -1);
        if (selectionRange.has_value())
        {
            const auto clipped = selectionRange->getIntersectionWith(visibleRange);
            if (!clipped.isEmpty())
            {
                const auto selectionX = secondsToX(clipped.getStart(), waveformBounds, visibleRange);
                const auto selectionRight = secondsToX(clipped.getEnd(), waveformBounds, visibleRange);
                const auto selectionWidth = selectionRight - selectionX;
                g.setColour(juce::Colours::black.withAlpha(0.05f));
                g.fillRect(juce::Rectangle<float>(waveformBounds.getX(),
                                                  waveformBounds.getY(),
                                                  juce::jmax(0.0f, selectionX - waveformBounds.getX()),
                                                  waveformBounds.getHeight()));
                g.fillRect(juce::Rectangle<float>(selectionRight,
                                                  waveformBounds.getY(),
                                                  juce::jmax(0.0f, waveformBounds.getRight() - selectionRight),
                                                  waveformBounds.getHeight()));
                g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.2f));
                g.fillRoundedRectangle({ selectionX, waveformBounds.getY(), selectionWidth, waveformBounds.getHeight() },
                                       RendererVisualStyle::cardRadius());
            }
        }
        return;
    }

    const auto gap = juce::jmin(12.0f, waveformBounds.getHeight() * 0.025f);
    const auto laneHeight = (waveformBounds.getHeight() - gap * static_cast<float>(channelCount - 1)) / static_cast<float>(channelCount);
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto laneY = waveformBounds.getY() + channel * (laneHeight + gap);
        auto laneBounds = juce::Rectangle<float>(waveformBounds.getX(), laneY, waveformBounds.getWidth(), laneHeight);
        const auto laneColour = laneColourForIndex(channel);
        if (channel < static_cast<int>(peakLevel.channelBins->size()))
            drawLane(laneBounds,
                     (*peakLevel.channelBins)[static_cast<std::size_t>(channel)],
                     laneColour,
                     channel);

        if (std::find(selectedSourceChannels.begin(),
                      selectedSourceChannels.end(),
                      channel) != selectedSourceChannels.end())
        {
            g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.88f));
            g.drawRoundedRectangle(laneBounds.reduced(1.0f),
                                   RendererVisualStyle::cardRadius(),
                                   2.0f);
        }

        g.setFont(modern ? ModernTypography::font(11.0f) : juce::Font(juce::FontOptions(11.5f)));
        auto laneLabel = "Ch " + juce::String(channel + 1);
        if (channel < static_cast<int>(state.info.channelLayout.selectedRoles.size()))
        {
            const auto roleLabel = ChannelLayoutCatalog::roleToAbbreviation(
                state.info.channelLayout.selectedRoles[static_cast<std::size_t>(channel)]);
            if (roleLabel.isNotEmpty() && roleLabel != "?")
                laneLabel << " " << roleLabel;
        }
        const auto controls = WaveformLaneControls::calculate(laneBounds,
            !RendererVisualStyle::isClassic() && !ModernAppleLookAndFeel::isStartupSkin());
        if (modern)
        {
            g.setColour(ModernAppleLookAndFeel::elevatedPanelBackground().withAlpha(0.80f));
            g.fillRoundedRectangle(controls.label.reduced(0, juce::jmin(2.0f, controls.label.getHeight() * 0.1f)), 4.0f);
        }
        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.9f));
        g.drawText(laneLabel, (modern ? controls.label.reduced(3, 0) : controls.label).toNearestInt(),
                   juce::Justification::centredLeft, modern);

        const auto drawChannelButton = [&](juce::StringRef label, juce::Rectangle<float> bounds, bool active)
        {
            g.setColour(active ? ModernAppleLookAndFeel::accentBlue().withAlpha(0.88f)
                               : ModernAppleLookAndFeel::elevatedPanelBackground().withAlpha(0.72f));
            g.fillRoundedRectangle(bounds, RendererVisualStyle::componentRadius());
            g.setColour(active ? juce::Colours::white
                               : ModernAppleLookAndFeel::secondaryText().withAlpha(0.92f));
            g.setFont(modern ? ModernTypography::font(10.0f, juce::Font::bold)
                             : juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
            g.drawText(label, bounds.toNearestInt(), juce::Justification::centred, false);
        };

        drawChannelButton("S", controls.solo, engine.isSourceChannelSoloed(channel));
        drawChannelButton("M", controls.mute, engine.isSourceChannelMuted(channel));

        if (channel + 1 < channelCount)
        {
            const auto separatorY = laneY + laneHeight + gap * 0.5f;
            g.setColour(juce::Colour(0x04000000).withAlpha(0.10f));
            g.drawHorizontalLine(static_cast<int>(std::round(separatorY)),
                                 waveformBounds.getX(),
                                 waveformBounds.getRight());
        }
    }

    if (selectionRange.has_value())
    {
        const auto clipped = selectionRange->getIntersectionWith(visibleRange);
        if (!clipped.isEmpty())
        {
            const auto selectionX = secondsToX(clipped.getStart(), waveformBounds, visibleRange);
            const auto selectionRight = secondsToX(clipped.getEnd(), waveformBounds, visibleRange);
            const auto selectionWidth = selectionRight - selectionX;
            g.setColour(juce::Colours::black.withAlpha(0.05f));
            g.fillRect(juce::Rectangle<float>(waveformBounds.getX(),
                                              waveformBounds.getY(),
                                              juce::jmax(0.0f, selectionX - waveformBounds.getX()),
                                              waveformBounds.getHeight()));
            g.fillRect(juce::Rectangle<float>(selectionRight,
                                              waveformBounds.getY(),
                                              juce::jmax(0.0f, waveformBounds.getRight() - selectionRight),
                                              waveformBounds.getHeight()));
            g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.18f));
            g.fillRoundedRectangle({ selectionX, waveformBounds.getY(), selectionWidth, waveformBounds.getHeight() },
                                   RendererVisualStyle::cardRadius());
        }
    }
}

void WaveformViewComponent::drawSpectrogram(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const
{
    auto waveformBounds = waveformContent(plotBounds);
    const auto durationSeconds = state.info.durationSeconds;
    const auto visibleRange = getVisibleTimeRange(durationSeconds);

    g.setColour(RendererVisualStyle::waveformSurface());
    g.fillRoundedRectangle(waveformBounds, RendererVisualStyle::cardRadius());

    const auto spectrogram = waveformModel.getSpectrogramCache();
    if (spectrogram == nullptr || spectrogram->audioRevision != state.audioRevision || !spectrogram->image.isValid())
    {
        drawWaveforms(g, plotBounds, state);
        const auto failed = waveformModel.getSpectrogramStatus() == WaveformModel::SpectrogramStatus::failed;

        if (usesModernWaveform())
        {
            ModernWaveformPainter::paintSpectralStatus(g, waveformBounds, failed);
            return;
        }

        auto badgeBounds = waveformBounds.removeFromTop(34.0f).removeFromRight(220.0f);
        g.setColour(ModernAppleLookAndFeel::panelBackground().withAlpha(0.96f));
        g.fillRoundedRectangle(badgeBounds, 12.0f);
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawFittedText(failed ? "Spectrum unavailable" : "Preparing spectral view...",
                         badgeBounds.reduced(12.0f, 6.0f).toNearestInt(),
                         juce::Justification::centred,
                         1);
        return;
    }

    const auto sourceWidth = static_cast<float>(spectrogram->image.getWidth());
    const auto startRatio = durationSeconds > 0.0 ? static_cast<float>(visibleRange.getStart() / durationSeconds) : 0.0f;
    const auto endRatio = durationSeconds > 0.0 ? static_cast<float>(visibleRange.getEnd() / durationSeconds) : 1.0f;
    const auto sourceX = juce::jlimit(0.0f, sourceWidth - 1.0f, sourceWidth * startRatio);
    const auto sourceRight = juce::jlimit(sourceX + 1.0f, sourceWidth, sourceWidth * endRatio);

    g.drawImage(spectrogram->image,
                static_cast<int>(std::round(waveformBounds.getX())),
                static_cast<int>(std::round(waveformBounds.getY())),
                static_cast<int>(std::round(waveformBounds.getWidth())),
                static_cast<int>(std::round(waveformBounds.getHeight())),
                static_cast<int>(std::floor(sourceX)),
                0,
                static_cast<int>(std::ceil(sourceRight - sourceX)),
                spectrogram->image.getHeight());

    if (selectionRange.has_value())
    {
        const auto clipped = selectionRange->getIntersectionWith(visibleRange);
        if (!clipped.isEmpty())
        {
            const auto selectionX = secondsToX(clipped.getStart(), waveformBounds, visibleRange);
            const auto selectionRight = secondsToX(clipped.getEnd(), waveformBounds, visibleRange);
            const auto selectionWidth = selectionRight - selectionX;
            g.setColour(juce::Colours::black.withAlpha(0.1f));
            g.fillRect(juce::Rectangle<float>(waveformBounds.getX(),
                                              waveformBounds.getY(),
                                              juce::jmax(0.0f, selectionX - waveformBounds.getX()),
                                              waveformBounds.getHeight()));
            g.fillRect(juce::Rectangle<float>(selectionRight,
                                              waveformBounds.getY(),
                                              juce::jmax(0.0f, waveformBounds.getRight() - selectionRight),
                                              waveformBounds.getHeight()));
            g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.16f));
            g.fillRoundedRectangle({ selectionX, waveformBounds.getY(), selectionWidth, waveformBounds.getHeight() }, 12.0f);
        }
    }

    if (usesModernWaveform())
    {
        ModernWaveformPainter::paintSpectralAxis(g, plotBounds, spectrogram->sampleRate);
        return;
    }

    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.88f));
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawFittedText("Spectral",
                     waveformBounds.removeFromBottom(24.0f).reduced(8.0f, 0.0f).toNearestInt(),
                     juce::Justification::centredRight,
                     1);
}

void WaveformViewComponent::drawEnvelope(juce::Graphics& g, juce::Rectangle<float> plotBounds, const SingleTrackDocument::State& state) const
{
    auto waveformBounds = waveformContent(plotBounds);
    const auto visibleRange = getVisibleTimeRange(state.info.durationSeconds);
    juce::Path envelopePath;
    const bool modern = usesModernWaveform();
    const bool quietUnity = modern && SingleTrackDocument::isEnvelopeFlatUnity(state.envelopePoints)
        && draggingEnvelopePoint < 0;
    bool started = modern;
    if (modern)
        envelopePath.startNewSubPath(waveformBounds.getX(),
            gainToY(SingleTrackDocument::evaluateEnvelope(state.envelopePoints, visibleRange.getStart()), waveformBounds, verticalZoom));
    for (const auto& point : state.envelopePoints)
    {
        if (point.timeSeconds < visibleRange.getStart() || point.timeSeconds > visibleRange.getEnd())
            continue;

        const auto x = secondsToX(point.timeSeconds, waveformBounds, visibleRange);
        const auto y = gainToY(point.gain, waveformBounds, verticalZoom);
        if (!started)
        {
            envelopePath.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            envelopePath.lineTo(x, y);
        }
    }

    if (modern)
        envelopePath.lineTo(waveformBounds.getRight(),
            gainToY(SingleTrackDocument::evaluateEnvelope(state.envelopePoints, visibleRange.getEnd()), waveformBounds, verticalZoom));
    g.setColour(quietUnity ? ModernAppleLookAndFeel::secondaryText().withAlpha(0.28f)
        : ModernAppleLookAndFeel::accentOrange().withAlpha(0.92f));
    g.strokePath(envelopePath, juce::PathStrokeType(quietUnity ? 1.0f : 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    for (int i = 0; i < static_cast<int>(state.envelopePoints.size()); ++i)
    {
        const auto& point = state.envelopePoints[static_cast<std::size_t>(i)];
        if (point.timeSeconds < visibleRange.getStart() || point.timeSeconds > visibleRange.getEnd())
            continue;

        const auto x = secondsToX(point.timeSeconds, waveformBounds, visibleRange);
        const auto y = gainToY(point.gain, waveformBounds, verticalZoom);
        const auto radius = i == draggingEnvelopePoint ? 6.5f : quietUnity ? 3.0f : 5.0f;

        g.setColour(ModernAppleLookAndFeel::panelBackground());
        g.fillEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f);
        g.setColour(quietUnity ? ModernAppleLookAndFeel::secondaryText().withAlpha(0.5f) : ModernAppleLookAndFeel::accentOrange());
        g.drawEllipse(x - radius, y - radius, radius * 2.0f, radius * 2.0f, 1.7f);
    }

    if (quietUnity && std::abs(previewSelectionGainLinear - 1.0f) < 0.001f) return;
    auto labelBounds = waveformBounds.removeFromBottom(28.0f);
    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.82f));
    g.drawFittedText("Envelope" + (selectionRange.has_value() ? " | Sel gain preview: " + juce::String(dbFromLinear(previewSelectionGainLinear), 1) + " dB" : ""),
                     labelBounds.toNearestInt(),
                     juce::Justification::centredRight,
                     1);
}

void WaveformViewComponent::drawEmptyState(juce::Graphics& g, juce::Rectangle<float> plotBounds) const
{
    const auto hasLoadingHint = loadingPrimaryText.isNotEmpty() || loadingSecondaryText.isNotEmpty();
    if (!RendererVisualStyle::isClassic())
    {
        const auto peaks = waveformModel.getPeakCache();
        const auto hasRealPreview = peaks != nullptr && peaks->binCount > 0
            && !peaks->mixedBins.empty() && peaks->durationSeconds > 0.0;
        if (openFailureHint || !hasLoadingHint || !hasRealPreview)
        {
            const auto state = openFailureHint ? ModernWaveformPainter::PlaceholderState::failed
                : hasLoadingHint ? ModernWaveformPainter::PlaceholderState::loading : ModernWaveformPainter::PlaceholderState::empty;
            ModernWaveformPainter::paintPlaceholder(g, waveformContent(plotBounds), state,
                                                     loadingPrimaryText, loadingSecondaryText);
            return;
        }
    }
    if (hasLoadingHint)
    {
        auto skeletonBounds = waveformContent(plotBounds);
        g.setColour(ModernAppleLookAndFeel::workspaceSurface().withAlpha(0.94f));
        g.fillRoundedRectangle(skeletonBounds, 18.0f);

        auto textBounds = skeletonBounds.reduced(26.0f, 22.0f);
        auto titleBounds = textBounds.removeFromTop(34.0f);
        auto detailBounds = textBounds.removeFromTop(22.0f);
        g.setColour(ModernAppleLookAndFeel::primaryText());
        g.setFont(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)));
        g.drawFittedText(loadingPrimaryText,
                         titleBounds.toNearestInt(),
                         juce::Justification::centred,
                         1);

        if (loadingSecondaryText.isNotEmpty())
        {
            g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.9f));
            g.setFont(juce::Font(juce::FontOptions(12.5f)));
            g.drawFittedText(loadingSecondaryText,
                             detailBounds.toNearestInt(),
                             juce::Justification::centred,
                             2);
        }

        auto previewBounds = textBounds.reduced(6.0f, 18.0f);
        if (const auto peakCache = waveformModel.getPeakCache();
            peakCache != nullptr
            && peakCache->binCount > 0
            && !peakCache->mixedBins.empty()
            && peakCache->durationSeconds > 0.0)
        {
            g.setColour(ModernAppleLookAndFeel::windowBackground().withAlpha(0.92f));
            g.fillRoundedRectangle(previewBounds, 16.0f);

            auto waveformBounds = previewBounds.reduced(18.0f, 14.0f);
            const auto widthPixels = juce::jmax(1, static_cast<int>(waveformBounds.getWidth()));
            const auto centerY = waveformBounds.getCentreY();
            const auto halfHeight = waveformBounds.getHeight() * 0.38f;
            const auto loadedEndSeconds = (peakCache->isPreview && peakCache->sampleRate > 0.0)
                ? static_cast<double>(peakCache->loadedSampleRange.getEnd()) / peakCache->sampleRate
                : peakCache->durationSeconds;

            for (int x = 0; x < widthPixels; ++x)
            {
                const auto binStart = juce::jlimit(0,
                                                   static_cast<int>(peakCache->mixedBins.size()) - 1,
                                                   static_cast<int>((static_cast<double>(x) / widthPixels) * peakCache->mixedBins.size()));
                const auto binEnd = juce::jlimit(binStart + 1,
                                                 static_cast<int>(peakCache->mixedBins.size()),
                                                 static_cast<int>(std::ceil((static_cast<double>(x + 1) / widthPixels) * peakCache->mixedBins.size())));

                auto minValue = 1.0f;
                auto maxValue = -1.0f;
                auto hasLoadedPeak = false;
                for (int binIndex = binStart; binIndex < binEnd; ++binIndex)
                {
                    const auto& bin = peakCache->mixedBins[static_cast<std::size_t>(binIndex)];
                    if (peakCache->isPreview && bin.startSeconds >= loadedEndSeconds)
                        break;

                    minValue = juce::jmin(minValue, bin.minValue);
                    maxValue = juce::jmax(maxValue, bin.maxValue);
                    hasLoadedPeak = true;
                }

                const auto lineX = waveformBounds.getX() + static_cast<float>(x);
                if (hasLoadedPeak)
                {
                    g.setColour(ModernAppleLookAndFeel::waveformBlue().withAlpha(0.92f));
                    const auto top = juce::jlimit(waveformBounds.getY(),
                                                  waveformBounds.getBottom(),
                                                  centerY - juce::jlimit(-1.0f, 1.0f, maxValue) * halfHeight);
                    const auto bottom = juce::jlimit(waveformBounds.getY(),
                                                     waveformBounds.getBottom(),
                                                     centerY - juce::jlimit(-1.0f, 1.0f, minValue) * halfHeight);
                    g.drawVerticalLine(static_cast<int>(lineX), top, bottom);
                }
                else
                {
                    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.12f));
                    g.drawVerticalLine(static_cast<int>(lineX),
                                       centerY - halfHeight * 0.32f,
                                       centerY + halfHeight * 0.32f);
                }
            }

            if (peakCache->isPreview && loadedEndSeconds < peakCache->durationSeconds)
            {
                const auto unloadedStartX = waveformBounds.getX()
                                          + waveformBounds.getWidth() * static_cast<float>(loadedEndSeconds / peakCache->durationSeconds);
                if (unloadedStartX < waveformBounds.getRight())
                {
                    auto unloadedBounds = juce::Rectangle<float>(unloadedStartX,
                                                                 waveformBounds.getY(),
                                                                 waveformBounds.getRight() - unloadedStartX,
                                                                 waveformBounds.getHeight());
                    g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.12f));
                    g.fillRoundedRectangle(unloadedBounds, 12.0f);
                    if (unloadedBounds.getWidth() > 120.0f)
                    {
                        g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.76f));
                        g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
                        g.drawFittedText("Loading tail...",
                                         unloadedBounds.reduced(10.0f, 8.0f).toNearestInt(),
                                         juce::Justification::centred,
                                         1);
                    }
                }
            }
        }
        else
        {
            constexpr int barCount = 28;
            const auto barGap = 6.0f;
            const auto barWidth = juce::jmax(6.0f, (previewBounds.getWidth() - barGap * (barCount - 1)) / barCount);
            const auto totalWidth = barCount * barWidth + (barCount - 1) * barGap;
            const auto startX = previewBounds.getCentreX() - totalWidth * 0.5f;
            const auto barBaseY = previewBounds.getCentreY() + 10.0f;
            const auto barColour = ModernAppleLookAndFeel::accentBlue().withAlpha(0.18f);
            const auto tailColour = ModernAppleLookAndFeel::secondaryText().withAlpha(0.10f);

            for (int i = 0; i < barCount; ++i)
            {
                const auto norm = std::sin((static_cast<float>(i) / static_cast<float>(barCount - 1)) * juce::MathConstants<float>::twoPi);
                const auto height = juce::jmap(std::abs(norm), 0.0f, 1.0f, 22.0f, previewBounds.getHeight() * 0.33f);
                const auto x = startX + i * (barWidth + barGap);
                const auto colour = i < barCount / 3 ? barColour : tailColour;
                g.setColour(colour);
                g.fillRoundedRectangle({ x, barBaseY - height * 0.5f, barWidth, height }, 8.0f);
            }
        }

        return;
    }

    const auto center = plotBounds.getCentre();
    const auto baseY = center.y - 18.0f;
    const float barWidth = 18.0f;
    const float barGap = 10.0f;
    const int barCount = 15;
    const float totalWidth = barCount * barWidth + (barCount - 1) * barGap;
    const float startX = center.x - totalWidth * 0.5f;

    for (int i = 0; i < barCount; ++i)
    {
        const auto distance = std::abs(i - (barCount / 2));
        const auto height = juce::jmax(56.0f, 270.0f - distance * 30.0f);
        const auto alpha = juce::jlimit(0.12f, 0.9f, 0.78f - distance * 0.07f);
        const auto x = startX + i * (barWidth + barGap);
        auto colour = ModernAppleLookAndFeel::accentBlue().withAlpha(alpha);

        if (i > barCount / 2)
            colour = ModernAppleLookAndFeel::accentPurple().withAlpha(alpha * 0.78f);

        g.setColour(colour);
        g.fillRoundedRectangle({x, baseY - height * 0.5f, barWidth, height}, 12.0f);
    }

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    g.drawFittedText("Drop audio here or open a file",
                     plotBounds.removeFromBottom(120.0f).removeFromTop(40.0f).toNearestInt(),
                     juce::Justification::centred,
                     1);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawFittedText("WAV, FLAC, WEM and BNK supported",
                     plotBounds.removeFromBottom(86.0f).removeFromTop(18.0f).toNearestInt(),
                     juce::Justification::centred,
                     1);
}

void WaveformViewComponent::updateSelectionFromPosition(float x)
{
    const auto duration = waveformModel.getLengthInSeconds();
    if (duration <= 0.0)
        return;

    const auto current = xToSeconds(x,
                                    waveformContent(getPlotBounds()),
                                    getVisibleTimeRange(duration));
    const auto start = juce::jmin(selectionAnchorSeconds, current);
    const auto end = juce::jmax(selectionAnchorSeconds, current);
    selectionRange = juce::Range<double>(start, end);
    notifySelectionChanged();
    repaintAndNotify();
}

void WaveformViewComponent::notifySelectionChanged()
{
    const auto summary = selectionRange.has_value()
        ? formatRulerLabel(selectionRange->getStart(), 0.001) + " - "
            + formatRulerLabel(selectionRange->getEnd(), 0.001) + "   Length "
            + formatRulerLabel(selectionRange->getLength(), 0.001)
        : juce::String("Selection: --");
    selectionSummary.setText(summary, juce::dontSendNotification);
    selectionSummary.setTooltip(summary);
    if (onSelectionChanged)
        onSelectionChanged(selectionRange);
}

void WaveformViewComponent::updateEnvelopePointFromMouse(const juce::MouseEvent& event)
{
    const auto state = waveformModel.getState();
    if (!hasEditableAudioState(state) || draggingEnvelopePoint < 0)
        return;

    const auto plotBounds = waveformContent(getPlotBounds());
    const auto time = xToSeconds(event.position.x, plotBounds, getVisibleTimeRange(state->info.durationSeconds));
    const auto gain = yToGain(event.position.y, plotBounds, verticalZoom);
    document.moveEnvelopePoint(draggingEnvelopePoint, time, gain);
}

void WaveformViewComponent::panVisibleRangeBy(double deltaSeconds) noexcept
{
    const auto duration = waveformModel.getLengthInSeconds();
    if (duration <= 0.0)
        return;

    const auto visibleLength = juce::jlimit(0.0001, duration, duration / horizontalZoom);
    const auto maxStart = juce::jmax(0.0, duration - visibleLength);
    visibleRangeStartSeconds = juce::jlimit(0.0, maxStart, visibleRangeStartSeconds + deltaSeconds);
    repaintAndNotify(getInteractiveBounds().toNearestInt());
}

void WaveformViewComponent::beginPresentationGeneration() noexcept
{
    // Reopening retained audio may keep its revision and peak cache. The new
    // CoreReady generation still needs a real presentation notification.
    lastPresentedWaveformRevision = -1;
    lastPresentedWaveformWasFull = false;
    corePresentationReady = false;
    openFailureHint = false;
    loadingPrimaryText.clear();
    loadingSecondaryText.clear();
}

void WaveformViewComponent::notifyCoreReady()
{
    corePresentationReady = true;
    prepareRequestedSpectrogram();
    repaintAndNotify();
}

void WaveformViewComponent::prepareRequestedSpectrogram(bool retryAfterFailure)
{
    if (corePresentationReady && viewMode == ViewMode::spectral)
        waveformModel.ensureSpectrogramReady(retryAfterFailure);
}

void WaveformViewComponent::notifyWaveformPresentationIfNeeded() noexcept
{
    if (!onWaveformPresentation)
        return;

    const auto state = waveformModel.getState();
    const auto peakCache = waveformModel.getPeakCache();
    if (state == nullptr
        || !state->info.isValid()
        || peakCache == nullptr
        || peakCache->audioRevision != state->audioRevision
        || peakCache->binCount <= 0)
    {
        return;
    }

    const auto hasFullWaveform = !peakCache->isPreview;
    if (lastPresentedWaveformRevision == peakCache->audioRevision
        && lastPresentedWaveformWasFull == hasFullWaveform)
    {
        return;
    }

    lastPresentedWaveformRevision = peakCache->audioRevision;
    lastPresentedWaveformWasFull = hasFullWaveform;
    onWaveformPresentation(hasFullWaveform);
}

void WaveformViewComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (const auto peakCache = waveformModel.getPeakCache(); peakCache != nullptr)
    {
        if (peakCache->isPreview
            && peakCache->audioRevision != lastAutoFocusedPreviewRevision
            && peakCache->sampleRate > 0.0
            && peakCache->loadedSampleRange.getLength() > 0
            && peakCache->loadedSampleRange.getEnd() < peakCache->totalSamples)
        {
            lastAutoFocusedPreviewRevision = peakCache->audioRevision;
        }
        else if (!peakCache->isPreview || peakCache->loadedSampleRange.getEnd() >= peakCache->totalSamples)
        {
            lastAutoFocusedPreviewRevision = -1;
        }

    }
    else
    {
        lastPresentedWaveformRevision = -1;
        lastPresentedWaveformWasFull = false;
    }

    prepareRequestedSpectrogram();

    // A completed cache is not a presentation. Only paint(), after drawing the
    // current source waveform, may publish its first/full waveform milestone.
    repaintAndNotify();
}

void WaveformViewComponent::timerCallback()
{
    const auto current = engine.getCurrentPositionSeconds();
    if (std::abs(current - lastCursorSeconds) > 0.01)
    {
        const auto previousRange = getVisibleTimeRange(waveformModel.getLengthInSeconds());
        if (waveformModel.hasWaveform())
            ensureTimeVisible(current, waveformModel.getLengthInSeconds());

        auto dirtyBounds = getInteractiveBounds().toNearestInt();
        if (waveformModel.hasWaveform()
            && previousRange == getVisibleTimeRange(waveformModel.getLengthInSeconds()))
        {
            const auto visibleRange = getVisibleTimeRange(waveformModel.getLengthInSeconds());
            const auto waveformBounds = waveformContent(getPlotBounds());
            const auto oldX = secondsToX(lastCursorSeconds, waveformBounds, visibleRange);
            const auto newX = secondsToX(current, waveformBounds, visibleRange);
            // Include the old and new timestamp tags, including their clamped
            // edge placement, so narrow cursor repaints do not leave old text.
            const auto margin = usesModernWaveform() ? 120 : 6;
            const auto left = static_cast<int>(std::floor(juce::jmin(oldX, newX))) - margin;
            const auto right = static_cast<int>(std::ceil(juce::jmax(oldX, newX))) + margin;
            dirtyBounds = juce::Rectangle<int>(left,
                                               static_cast<int>(getRulerBounds().getY()),
                                               juce::jmax(12, right - left),
                                               static_cast<int>(waveformBounds.getBottom() - getRulerBounds().getY()));
        }

        lastCursorSeconds = current;
        repaintAndNotify(dirtyBounds);
    }
}
} // namespace singleeditor
