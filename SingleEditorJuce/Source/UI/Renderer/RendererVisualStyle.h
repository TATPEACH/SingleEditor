#pragma once

#include <algorithm>
#include <cstdint>

#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualTypes.h"
#include "UI/LookAndFeel/ModernGlassTokens.h"

namespace singleeditor
{
class RendererMotionController final
{
public:
    void start(float from, float to, bool enabled, bool reduceMotion, float durationMsToUse) noexcept;
    void interrupt(float to, bool enabled, bool reduceMotion, float durationMsToUse) noexcept;
    void advance(float deltaMs) noexcept;
    void snapTo(float value) noexcept;
    [[nodiscard]] float value() const noexcept { return currentValue; }
    [[nodiscard]] bool isActive() const noexcept { return active; }

private:
    float startValue = 1.0f;
    float currentValue = 1.0f;
    float targetValue = 1.0f;
    float elapsedMs = 0.0f;
    float durationMs = 1.0f;
    bool active = false;
};

class RendererVisualStyle final
{
public:
    [[nodiscard]] static bool isClassic() noexcept;
    static void setModernTheme(ModernTheme theme) noexcept;
    [[nodiscard]] static ModernTheme modernTheme() noexcept;
    [[nodiscard]] static bool isNight() noexcept;
    [[nodiscard]] static const ModernGlassTokens::Palette& modernPalette() noexcept;
    static void setGlassMaterial(ModernGlassMaterial material) noexcept;
    [[nodiscard]] static ModernGlassMaterial glassMaterial() noexcept;
    [[nodiscard]] static juce::Colour windowBackground();
    [[nodiscard]] static juce::Colour panelBackground();
    [[nodiscard]] static juce::Colour elevatedPanelBackground();
    [[nodiscard]] static juce::Colour workspaceSurface();
    [[nodiscard]] static juce::Colour mutedSurface();
    [[nodiscard]] static juce::Colour titleBarBackground();
    [[nodiscard]] static juce::Colour borderColour();
    [[nodiscard]] static juce::Colour primaryText();
    [[nodiscard]] static juce::Colour secondaryText();
    [[nodiscard]] static juce::Colour disabledText();
    [[nodiscard]] static juce::Colour accentOrange();
    [[nodiscard]] static juce::Colour accentBlue();
    [[nodiscard]] static juce::Colour accentPurple();
    [[nodiscard]] static juce::Colour waveformBlue();
    [[nodiscard]] static juce::Colour waveformCyan();
    [[nodiscard]] static juce::Colour ghostHoverFill();
    [[nodiscard]] static juce::Colour ghostPressedFill();
    [[nodiscard]] static juce::Colour waveformLaneColour(int index);
    [[nodiscard]] static juce::Colour waveformSurface();
    [[nodiscard]] static juce::Colour meterFill();
    [[nodiscard]] static juce::Colour danger();
    [[nodiscard]] static juce::Colour accentSoft();
    [[nodiscard]] static juce::Colour accentInk();

    [[nodiscard]] static float cardRadius() noexcept;
    [[nodiscard]] static float componentRadius() noexcept;
    [[nodiscard]] static float focusRadius() noexcept;
    [[nodiscard]] static float focusRingWidth() noexcept;
    [[nodiscard]] static float magBorderWidth() noexcept;
    [[nodiscard]] static int outerPadding() noexcept;
    [[nodiscard]] static int interPanelGap() noexcept;
    [[nodiscard]] static int topBarHeight() noexcept;
    [[nodiscard]] static int bottomBarHeight() noexcept;
    [[nodiscard]] static int titleStripHeight() noexcept;
    [[nodiscard]] static int resizeHandleExtent() noexcept;
    [[nodiscard]] static int gapForDensity(RendererDensity density) noexcept;
    [[nodiscard]] static float titleTextSize() noexcept;
    [[nodiscard]] static float bodyTextSize() noexcept;
    [[nodiscard]] static float captionTextSize() noexcept;
    [[nodiscard]] static float monoTextSize() noexcept;

    [[nodiscard]] static RendererInformationTier resolvedInformationTier(
        RendererDensity density, RendererMagSize size, RendererContentRect content) noexcept;
    [[nodiscard]] static bool sweetSpotSatisfied(RendererMagSize size,
                                                 RendererContentRect content) noexcept;
    [[nodiscard]] static RendererAssetToken assetToken(RendererAssetId id) noexcept;
    [[nodiscard]] static RendererAssetId assetIdForFileName(std::string_view fileName) noexcept;

    static void setReduceMotion(bool reduced) noexcept;
    [[nodiscard]] static bool isReduceMotion() noexcept;
    [[nodiscard]] static bool isMotionEnabled() noexcept;
    [[nodiscard]] static float motionDurationMs() noexcept;
    [[nodiscard]] static float enteringOpacityFloor() noexcept;
    [[nodiscard]] static int motionTimerIntervalMs() noexcept;
    static void noteMotionStart() noexcept;
};
} // namespace singleeditor
