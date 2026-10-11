#include "WaveformOverviewComponent.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include <cmath>

namespace singleeditor
{
WaveformOverviewComponent::WaveformOverviewComponent()
{
    setComponentID("core.waveform.overview");
    setTitle("Source waveform overview; drag to pan, arrow keys to move the view");
    setWantsKeyboardFocus(true);
    for (auto* button : { &zoomOut, &zoomIn, &reset })
    {
        addAndMakeVisible(button);
        button->setMouseCursor(juce::MouseCursor::NormalCursor);
    }
    zoomOut.setComponentID("core.waveform.zoom-time-out");
    zoomIn.setComponentID("core.waveform.zoom-time-in");
    reset.setComponentID("core.waveform.reset-view");
    zoomOut.setTitle("Zoom time out"); zoomOut.setTooltip("Zoom time out");
    zoomIn.setTitle("Zoom time in"); zoomIn.setTooltip("Zoom time in");
    reset.setTitle("Reset waveform view"); reset.setTooltip("Reset time and amplitude zoom");
    const auto icon = [](juce::TextButton& button, RendererAssetId asset)
    {
        button.getProperties().set("iconOnly", true);
        button.getProperties().set("iconName", juce::String(RendererVisualStyle::assetToken(asset).fileName));
    };
    icon(zoomOut, RendererAssetId::zoomOut);
    icon(zoomIn, RendererAssetId::zoomIn);
    zoomOut.onClick = [this] { if (onActionRequested) onActionRequested(Action::zoomOut); };
    zoomIn.onClick = [this] { if (onActionRequested) onActionRequested(Action::zoomIn); };
    reset.onClick = [this] { if (onActionRequested) onActionRequested(Action::reset); };
}

void WaveformOverviewComponent::setState(std::shared_ptr<const WaveformModel::PeakCache> peaks,
                                        int64_t audioRevision, double seconds,
                                        juce::Range<double> visible, double position)
{
    if (peaks != nullptr && peaks->audioRevision != audioRevision) peaks.reset();
    const auto safeDuration = std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
    const bool changed = cache != peaks || duration != safeDuration;
    cache = std::move(peaks);
    duration = safeDuration;
    visibleRange = visible.getIntersectionWith({ 0.0, duration });
    cursor = juce::jlimit(0.0, duration, std::isfinite(position) ? position : 0.0);
    if (changed) rebuildPeaks();
    const auto zoom = visibleRange.getLength() > 0.0 ? duration / visibleRange.getLength() : 1.0;
    reset.setButtonText(juce::String(zoom, zoom < 10 ? 1 : 0) + "x");
    zoomOut.setEnabled(navigationEnabled && duration > 0 && zoom > 1.001);
    zoomIn.setEnabled(navigationEnabled && duration > 0 && zoom < 511.999);
    reset.setEnabled(navigationEnabled && duration > 0);
    repaint();
}

void WaveformOverviewComponent::setNavigationEnabled(bool enabled)
{
    navigationEnabled = enabled;
    if (!enabled) dragging = false;
    setState(cache, cache != nullptr ? cache->audioRevision : -1, duration, visibleRange, cursor);
}

void WaveformOverviewComponent::resized()
{
    auto bounds = getLocalBounds().reduced(2, 1);
    const auto height = juce::jmin(24, bounds.getHeight());
    bounds = bounds.withSizeKeepingCentre(bounds.getWidth(), height);
    reset.setBounds(bounds.removeFromRight(42)); bounds.removeFromRight(4);
    zoomIn.setBounds(bounds.removeFromRight(26)); bounds.removeFromRight(4);
    zoomOut.setBounds(bounds.removeFromRight(26)); bounds.removeFromRight(6);
    trackBounds = bounds.toFloat().reduced(0.0f, 2.0f);
    rebuildPeaks();
}

juce::Rectangle<float> WaveformOverviewComponent::getViewportBounds() const noexcept
{
    if (duration <= 0.0 || trackBounds.isEmpty()) return {};
    const auto x = trackBounds.getX() + static_cast<float>(visibleRange.getStart() / duration) * trackBounds.getWidth();
    const auto width = static_cast<float>(visibleRange.getLength() / duration) * trackBounds.getWidth();
    return { x, trackBounds.getY(), width, trackBounds.getHeight() };
}

void WaveformOverviewComponent::rebuildPeaks()
{
    peakPath.clear();
    if (cache == nullptr || duration <= 0 || trackBounds.isEmpty()) return;
    const auto level = WaveformModel::choosePeakLevel(*cache, { 0.0, duration }, juce::roundToInt(trackBounds.getWidth()));
    if (level.mixedBins == nullptr) return;
    const auto& bins = *level.mixedBins;
    size_t first = 0;
    const auto pixels = juce::jmax(1, juce::roundToInt(trackBounds.getWidth()));
    const auto amplitude = trackBounds.getHeight() * 0.42f;
    // Sweep real bins once. Unknown time ranges remain empty, including a
    // leading-only preview; never stretch the loaded prefix across the file.
    for (int x = 0; x < pixels; ++x)
    {
        const auto start = duration * x / pixels;
        const auto end = duration * (x + 1) / pixels;
        while (first < bins.size() && bins[first].endSeconds <= start) ++first;
        float low = 0, high = 0;
        bool known = false;
        for (auto i = first; i < bins.size() && bins[i].startSeconds < end; ++i)
        {
            low = juce::jmin(low, bins[i].minValue);
            high = juce::jmax(high, bins[i].maxValue);
            known = true;
        }
        if (!known) continue;
        const auto top = trackBounds.getCentreY() - juce::jlimit(-1.0f, 1.0f, high) * amplitude;
        const auto bottom = trackBounds.getCentreY() - juce::jlimit(-1.0f, 1.0f, low) * amplitude;
        peakPath.addRectangle(trackBounds.getX() + x, top, 1.0f, juce::jmax(0.5f, bottom - top));
    }
}

void WaveformOverviewComponent::paint(juce::Graphics& g)
{
    if (trackBounds.isEmpty()) return;
    g.setColour(RendererVisualStyle::mutedSurface());
    g.fillRoundedRectangle(trackBounds, 4);
    juce::Graphics::ScopedSaveState scope(g);
    g.reduceClipRegion(trackBounds.toNearestInt());
    g.setColour(RendererVisualStyle::waveformBlue().withAlpha(0.75f));
    g.fillPath(peakPath);
    if (cache != nullptr && cache->isPreview && duration > 0.0 && cache->sampleRate > 0.0)
    {
        const auto loadedStart = juce::jlimit(0.0, 1.0, cache->loadedSampleRange.getStart() / cache->sampleRate / duration);
        const auto loadedEnd = juce::jlimit(loadedStart, 1.0, cache->loadedSampleRange.getEnd() / cache->sampleRate / duration);
        g.setColour(RendererVisualStyle::secondaryText().withAlpha(0.18f));
        g.fillRect(trackBounds.withWidth(static_cast<float>(loadedStart) * trackBounds.getWidth()));
        g.fillRect(trackBounds.withTrimmedLeft(static_cast<float>(loadedEnd) * trackBounds.getWidth()));
    }
    auto viewport = getViewportBounds();
    if (!viewport.isEmpty())
    {
        g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.09f));
        g.fillRoundedRectangle(viewport, 3);
        g.setColour(RendererVisualStyle::accentBlue().withAlpha(hasKeyboardFocus(false) ? 0.95f : 0.55f));
        g.drawRoundedRectangle(viewport.reduced(0.5f), 3, 1);
        if (viewport.getWidth() >= 14 && visibleRange.getLength() < duration * 0.999)
            for (const auto x : { viewport.getX() + 3, viewport.getRight() - 3 })
                g.drawLine(x, viewport.getY() + 6, x, viewport.getBottom() - 6, 1);
    }
    if (duration > 0.0)
    {
        g.setColour(RendererVisualStyle::accentBlue());
        const auto x = trackBounds.getX() + static_cast<float>(cursor / duration) * trackBounds.getWidth();
        g.drawLine(x, trackBounds.getY() + 2, x, trackBounds.getBottom() - 2, 1);
    }
}

void WaveformOverviewComponent::requestPan(double start)
{
    if (navigationEnabled && duration > 0 && onPanRequested)
        onPanRequested(juce::jlimit(0.0, juce::jmax(0.0, duration - visibleRange.getLength()), start));
}

void WaveformOverviewComponent::mouseDown(const juce::MouseEvent& event)
{
    if (!event.mods.isLeftButtonDown() || !navigationEnabled || duration <= 0 || !trackBounds.contains(event.position)) return;
    grabKeyboardFocus();
    dragAnchorX = event.position.x;
    dragStart = visibleRange.getStart();
    if (!getViewportBounds().contains(event.position))
    {
        const auto time = (event.position.x - trackBounds.getX()) / trackBounds.getWidth() * duration;
        dragStart = juce::jlimit(0.0, juce::jmax(0.0, duration - visibleRange.getLength()), time - visibleRange.getLength() * 0.5);
        requestPan(dragStart);
    }
    dragging = true;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void WaveformOverviewComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (dragging && trackBounds.getWidth() > 0)
        requestPan(dragStart + (event.position.x - dragAnchorX) / trackBounds.getWidth() * duration);
}

void WaveformOverviewComponent::mouseUp(const juce::MouseEvent&)
{
    dragging = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

bool WaveformOverviewComponent::keyPressed(const juce::KeyPress& key)
{
    if (!navigationEnabled || duration <= 0 || key.getModifiers().isAnyModifierKeyDown()) return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        requestPan(visibleRange.getStart() + visibleRange.getLength() * (code == juce::KeyPress::leftKey ? -0.1 : 0.1));
    else if (code == juce::KeyPress::homeKey) requestPan(0);
    else if (code == juce::KeyPress::endKey) requestPan(duration);
    else return false;
    return true;
}
} // namespace singleeditor
