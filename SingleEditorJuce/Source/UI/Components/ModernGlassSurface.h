#pragma once

#include "CardSurface.h"
#include "UI/LookAndFeel/ModernGlassBackdrop.h"

namespace singleeditor
{
class ModernGlassSurface final : public CardSurface,
                                 private juce::Timer,
                                 private juce::ComponentListener
{
public:
    explicit ModernGlassSurface(float radius = 14.0f);
    ~ModernGlassSurface() override;
    void setBackdropSource(juce::Component* source);
    // Some core content is a sibling above the Mag host, rather than its child.
    void setBackdropOverlay(juce::Component* overlay);
    // A decorative glass layer can sit behind sibling controls. Their common
    // parent supplies pointer events without becoming the sampled backdrop.
    void setInteractionHost(juce::Component* host);
    void setEffectsEnabled(bool enabled);
    void invalidateBackdrop();
    void refreshBackdropNow();
    void paint(juce::Graphics&) override;
    void moved() override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void lookAndFeelChanged() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    [[nodiscard]] bool hasBackdrop() const noexcept { return rendered.isValid(); }
    [[nodiscard]] int captureCountForTesting() const noexcept { return captureCount; }
    [[nodiscard]] float hoverAmountForTesting() const noexcept { return hover.value(); }
    [[nodiscard]] bool hasScheduledUpdateForTesting() const noexcept { return isTimerRunning(); }

private:
    bool canSample() const;
    void releaseBackdrop();
    void renderPreparedBackdrop();
    void updateHover(const juce::MouseEvent&, bool over);
    void timerCallback() override;
    void componentMovedOrResized(juce::Component&, bool, bool) override;
    void componentParentHierarchyChanged(juce::Component&) override;
    void componentVisibilityChanged(juce::Component&) override;
    void componentBeingDeleted(juce::Component&) override;
    juce::Component::SafePointer<juce::Component> backdropSource;
    juce::Component::SafePointer<juce::Component> backdropOverlay;
    juce::Component::SafePointer<juce::Component> interactionHost;
    juce::Image blurred, rendered;
    juce::Rectangle<int> sampleBody;
    juce::Point<float> lightPosition { 0.25f, 0.0f };
    RendererMotionController hover;
    double nextCaptureAt = 0.0, lastTickAt = 0.0;
    float radius = 14.0f, sampleScale = 0.5f;
    bool effectsEnabled = false, dirty = false, hovered = false;
    int captureCount = 0;
};
} // namespace singleeditor
