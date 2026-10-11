#include "ModernGlassSurface.h"

namespace singleeditor
{
ModernGlassSurface::ModernGlassSurface(float cornerRadius)
    : CardSurface(cornerRadius), radius(cornerRadius)
{
    hover.snapTo(0.0f);
    addMouseListener(this, true);
}

ModernGlassSurface::~ModernGlassSurface()
{
    stopTimer();
    setInteractionHost(nullptr);
    setBackdropOverlay(nullptr);
    setBackdropSource(nullptr);
    removeMouseListener(this);
}

void ModernGlassSurface::setInteractionHost(juce::Component* host)
{
    jassert(host == nullptr || host == this || host->isParentOf(this));
    auto* parent = host != nullptr && host->isParentOf(this) ? host : nullptr;
    if (parent == interactionHost.getComponent()) return;
    if (interactionHost != nullptr) interactionHost->removeMouseListener(this);
    interactionHost = parent;
    if (interactionHost != nullptr) interactionHost->addMouseListener(this, true);
    releaseBackdrop();
    invalidateBackdrop();
}

void ModernGlassSurface::setBackdropSource(juce::Component* source)
{
    if (source == backdropSource.getComponent()) return;
    if (backdropSource != nullptr)
    {
        backdropSource->removeMouseListener(this);
        backdropSource->removeComponentListener(this);
    }
    // Sampling a parent containing this surface would recurse through paint.
    jassert(source == nullptr || (source != this && !source->isParentOf(this)));
    backdropSource = source != this && (source == nullptr || !source->isParentOf(this)) ? source : nullptr;
    releaseBackdrop();
    if (backdropSource != nullptr)
    {
        backdropSource->addMouseListener(this, true);
        backdropSource->addComponentListener(this);
        invalidateBackdrop();
    }
}

void ModernGlassSurface::setBackdropOverlay(juce::Component* overlay)
{
    if (overlay == backdropOverlay.getComponent()) return;
    if (backdropOverlay != nullptr)
    {
        backdropOverlay->removeMouseListener(this);
        backdropOverlay->removeComponentListener(this);
    }
    jassert(overlay == nullptr || (overlay != this && !overlay->isParentOf(this)));
    backdropOverlay = overlay != this && (overlay == nullptr || !overlay->isParentOf(this)) ? overlay : nullptr;
    if (backdropOverlay != nullptr)
    {
        backdropOverlay->addMouseListener(this, true);
        backdropOverlay->addComponentListener(this);
    }
    invalidateBackdrop();
}

bool ModernGlassSurface::canSample() const
{
    return effectsEnabled && !RendererVisualStyle::isClassic()
        && RendererVisualStyle::glassMaterial() != ModernGlassMaterial::solid
        && isVisible() && getWidth() > 0 && getHeight() > 0
        && backdropSource != nullptr && backdropSource->isVisible()
        && backdropSource->getTopLevelComponent() == getTopLevelComponent()
        && !backdropSource->isParentOf(this)
        && (getPeer() == nullptr || isShowing());
}

void ModernGlassSurface::releaseBackdrop()
{
    stopTimer();
    blurred = {}; rendered = {}; sampleBody = {};
    dirty = false;
    hover.snapTo(0.0f);
    hovered = false;
    lightPosition = { 0.25f, 0.0f };
}

void ModernGlassSurface::setEffectsEnabled(bool enabled)
{
    if (effectsEnabled == enabled) return;
    effectsEnabled = enabled;
    invalidateBackdrop();
}

void ModernGlassSurface::invalidateBackdrop()
{
    if (!canSample()) { releaseBackdrop(); repaint(); return; }
    dirty = true;
    if (!isTimerRunning())
    {
        lastTickAt = juce::Time::getMillisecondCounterHiRes();
        startTimer(20);
    }
}

void ModernGlassSurface::refreshBackdropNow()
{
    if (!canSample()) { releaseBackdrop(); return; }
    constexpr int margin = 24;
    auto captureArea = backdropSource->getLocalArea(this, getLocalBounds()).expanded(margin);
    sampleScale = juce::jmin(0.5f, 1024.0f / static_cast<float>(captureArea.getWidth()),
                                   512.0f / static_cast<float>(captureArea.getHeight()));
    auto captured = backdropSource->createComponentSnapshot(captureArea, false, sampleScale, juce::SoftwareImageType());
    if (backdropOverlay != nullptr && backdropOverlay->isVisible()
        && backdropOverlay->getTopLevelComponent() == getTopLevelComponent()
        && (getPeer() == nullptr || backdropOverlay->isShowing())
        && backdropOverlay != backdropSource && !backdropSource->isParentOf(backdropOverlay))
    {
        const auto overlayArea = backdropOverlay->getLocalArea(backdropSource, captureArea)
                                     .getIntersection(backdropOverlay->getLocalBounds());
        if (!overlayArea.isEmpty())
        {
            const auto overlayImage = backdropOverlay->createComponentSnapshot(overlayArea, false, sampleScale, juce::SoftwareImageType());
            const auto target = backdropSource->getLocalArea(backdropOverlay, overlayArea)
                                    .translated(-captureArea.getX(), -captureArea.getY()).toFloat() * sampleScale;
            juce::Graphics composite(captured);
            composite.drawImage(overlayImage, target);
        }
    }
    ++captureCount;
    const auto material = RendererVisualStyle::glassMaterial();
    // The liquid boundary still refracts the prepared image; soften small
    // background glyphs enough that they cannot read as foreground labels.
    blurred = ModernGlassBackdrop::blur(captured, material == ModernGlassMaterial::frosted ? 6.0f : 3.0f,
                                       RendererVisualStyle::panelBackground());
    sampleBody = blurred.getBounds().reduced(juce::roundToInt(margin * sampleScale));
    renderPreparedBackdrop();
    dirty = false;
    nextCaptureAt = juce::Time::getMillisecondCounterHiRes() + 80.0;
    repaint();
}

void ModernGlassSurface::renderPreparedBackdrop()
{
    rendered = ModernGlassBackdrop::sample(blurred, sampleBody, radius * sampleScale,
        RendererVisualStyle::glassMaterial(), 1.0f + hover.value() * 0.2f);
}

void ModernGlassSurface::timerCallback()
{
    if (!canSample()) { releaseBackdrop(); return; }
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto previous = hover.value();
    if (RendererVisualStyle::glassMaterial() != ModernGlassMaterial::liquid)
    {
        hover.snapTo(0.0f);
        hovered = false;
    }
    else if (!RendererVisualStyle::isMotionEnabled() || RendererVisualStyle::isReduceMotion())
        hover.snapTo(hovered ? 1.0f : 0.0f);
    else
        hover.advance(static_cast<float>(now - lastTickAt));
    lastTickAt = now;
    if (dirty && now >= nextCaptureAt) refreshBackdropNow();
    else if (previous != hover.value() && RendererVisualStyle::glassMaterial() == ModernGlassMaterial::liquid)
        renderPreparedBackdrop();
    if (previous != hover.value()) repaint();
    if (!dirty && !hover.isActive()) stopTimer();
}

void ModernGlassSurface::paint(juce::Graphics& g)
{
    const bool samplingAllowed = canSample();
    if (!samplingAllowed || !rendered.isValid())
    {
        // A pending timer may release the cache while the native window is
        // minimised. Restore can repaint without resizing this child or moving
        // the pointer. Recover through the existing coalesced timer, never by
        // taking a source snapshot from inside paint.
        if (samplingAllowed) invalidateBackdrop();
        CardSurface::paint(g);
        return;
    }
    const auto body = getLocalBounds().toFloat().reduced(1.0f).withTrimmedBottom(1.0f);
    const bool night = RendererVisualStyle::isNight();
    const bool liquid = RendererVisualStyle::glassMaterial() == ModernGlassMaterial::liquid;
    g.setColour(juce::Colours::black.withAlpha(night ? 0.18f : 0.07f));
    g.fillRoundedRectangle(body.translated(0, 1.5f), radius);
    {
        juce::Graphics::ScopedSaveState save(g);
        juce::Path clip;
        clip.addRoundedRectangle(body, radius);
        g.reduceClipRegion(clip);
        // The shadow brush has a low alpha. The prepared backdrop must replace
        // the sharp content below before applying the glass tint.
        g.setOpacity(1.0f);
        g.drawImage(rendered, getLocalBounds().toFloat());
        // Keep the refracted backdrop visible without letting the light-theme
        // workspace labels compete with the foreground command/selection text.
        g.setColour(RendererVisualStyle::panelBackground().withAlpha(liquid ? (night ? 0.70f : 0.56f) : (night ? 0.85f : 0.76f)));
        g.fillRect(body);
        if (liquid)
        {
            const auto centre = juce::Point<float>(body.getX() + body.getWidth() * lightPosition.x,
                                                   body.getY() + body.getHeight() * lightPosition.y);
            const auto shine = juce::Colours::white.withAlpha((night ? 0.04f : 0.12f) + hover.value() * 0.06f);
            g.setGradientFill(juce::ColourGradient(shine, centre.x, centre.y,
                juce::Colours::transparentWhite, centre.x + body.getHeight() * 2, centre.y + body.getHeight(), true));
            g.fillRect(body);
        }
    }
    g.setColour(RendererVisualStyle::borderColour().withAlpha(night ? 0.9f : 0.8f));
    g.drawRoundedRectangle(body, radius, 0.8f);
    const auto inner = body.reduced(0.8f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(night ? 0.24f : 0.94f),
        inner.getCentreX(), inner.getY(), juce::Colours::white.withAlpha(night ? 0.02f : 0.10f),
        inner.getCentreX(), inner.getBottom(), false));
    g.drawRoundedRectangle(inner, radius - 0.8f, liquid ? 1.1f : 0.8f);
}

void ModernGlassSurface::moved() { invalidateBackdrop(); }
void ModernGlassSurface::resized() { invalidateBackdrop(); }
void ModernGlassSurface::visibilityChanged() { invalidateBackdrop(); }
void ModernGlassSurface::parentHierarchyChanged() { invalidateBackdrop(); }
void ModernGlassSurface::lookAndFeelChanged() { invalidateBackdrop(); }
void ModernGlassSurface::componentMovedOrResized(juce::Component&, bool, bool) { invalidateBackdrop(); }
void ModernGlassSurface::componentParentHierarchyChanged(juce::Component&) { invalidateBackdrop(); }
void ModernGlassSurface::componentVisibilityChanged(juce::Component&) { invalidateBackdrop(); }
void ModernGlassSurface::componentBeingDeleted(juce::Component& component)
{
    if (&component == backdropSource.getComponent()) backdropSource = nullptr;
    if (&component == backdropOverlay.getComponent()) backdropOverlay = nullptr;
    releaseBackdrop();
}

void ModernGlassSurface::updateHover(const juce::MouseEvent& event, bool over)
{
    const auto* host = interactionHost != nullptr ? interactionHost.getComponent() : this;
    if (event.eventComponent != host && !host->isParentOf(event.eventComponent)) return;
    const auto local = event.getEventRelativeTo(this).position;
    over = over && getLocalBounds().toFloat().contains(local);
    lightPosition = { juce::jlimit(0.0f, 1.0f, local.x / static_cast<float>(juce::jmax(1, getWidth()))),
                      juce::jlimit(0.0f, 1.0f, local.y / static_cast<float>(juce::jmax(1, getHeight()))) };
    if (RendererVisualStyle::glassMaterial() != ModernGlassMaterial::liquid || !canSample()) return;
    if (hovered == over) { repaint(); return; }
    hovered = over;
    hover.interrupt(over ? 1.0f : 0.0f, RendererVisualStyle::isMotionEnabled(),
                    RendererVisualStyle::isReduceMotion(), 140.0f);
    if (!hover.isActive()) renderPreparedBackdrop();
    if ((dirty || hover.isActive()) && !isTimerRunning())
    {
        lastTickAt = juce::Time::getMillisecondCounterHiRes();
        startTimer(20);
    }
    else if (!dirty && !hover.isActive())
        stopTimer();
    repaint();
}

void ModernGlassSurface::mouseMove(const juce::MouseEvent& e) { updateHover(e, true); }
void ModernGlassSurface::mouseEnter(const juce::MouseEvent& e) { updateHover(e, true); }
void ModernGlassSurface::mouseExit(const juce::MouseEvent& e)
{
    const auto* host = interactionHost != nullptr ? interactionHost.getComponent() : this;
    updateHover(e, host->isMouseOver(true));
}
void ModernGlassSurface::mouseDrag(const juce::MouseEvent& e) { updateHover(e, true); invalidateBackdrop(); }
void ModernGlassSurface::mouseDown(const juce::MouseEvent&) { invalidateBackdrop(); }
void ModernGlassSurface::mouseUp(const juce::MouseEvent&) { invalidateBackdrop(); }
void ModernGlassSurface::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) { invalidateBackdrop(); }
} // namespace singleeditor
