#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include "UI/Renderer/RendererVisualTypes.h"

namespace singleeditor
{
using ModernNorthStarDensity = RendererDensity;
using ModernNorthStarMagSize = RendererMagSize;
using ModernNorthStarInformationTier = RendererInformationTier;
using ModernNorthStarEdge = RendererEdge;
using ModernNorthStarAssetOwner = RendererAssetOwner;
using ModernNorthStarAssetId = RendererAssetId;

struct ModernNorthStarFoundationTokens final
{
    static constexpr std::uint32_t backgroundArgb = 0xfff4f0e7;
    static constexpr std::uint32_t surfaceArgb = 0xfffcfaf6;
    static constexpr std::uint32_t foregroundArgb = 0xff191f23;
    static constexpr std::uint32_t mutedArgb = 0xff576069;
    static constexpr std::uint32_t borderArgb = 0xffbab5ac;
    static constexpr std::uint32_t accentArgb = 0xff008c94;
    static constexpr std::uint32_t surfaceSoftArgb = 0xffede9e2;
    static constexpr std::uint32_t accentSoftArgb = 0xffb8e4e7;
    static constexpr std::uint32_t accentInkArgb = 0xff00282d;
    static constexpr std::uint32_t dangerArgb = 0xffb3463a;
    static constexpr std::uint32_t successArgb = 0xff16845b;
    static constexpr std::uint32_t warningArgb = 0xffa66b18;
};

struct ModernNorthStarSemanticTokens final
{
    static constexpr auto workspaceBackgroundArgb = ModernNorthStarFoundationTokens::backgroundArgb;
    static constexpr auto occupiedSurfaceArgb = ModernNorthStarFoundationTokens::surfaceArgb;
    static constexpr auto controlSurfaceArgb = ModernNorthStarFoundationTokens::surfaceSoftArgb;
    static constexpr auto primaryTextArgb = ModernNorthStarFoundationTokens::foregroundArgb;
    static constexpr auto secondaryTextArgb = ModernNorthStarFoundationTokens::mutedArgb;
    static constexpr auto focusArgb = ModernNorthStarFoundationTokens::accentArgb;
    static constexpr auto selectionArgb = ModernNorthStarFoundationTokens::accentSoftArgb;
    static constexpr auto selectionTextArgb = ModernNorthStarFoundationTokens::accentInkArgb;
    static constexpr auto failureArgb = ModernNorthStarFoundationTokens::dangerArgb;
    static constexpr auto readyArgb = ModernNorthStarFoundationTokens::successArgb;
};

struct ModernNorthStarComponentTokens final
{
    static constexpr std::array<std::uint32_t, 6> waveformLaneArgb {
        0xff008c94, 0xff19a6ad, 0xff2f7f85, 0xff4aaeb4, 0xff006b72, 0xff6bbfc3
    };
    static constexpr std::uint32_t waveformSurfaceArgb = ModernNorthStarFoundationTokens::surfaceArgb;
    static constexpr std::uint32_t waveformSelectionArgb = ModernNorthStarFoundationTokens::accentSoftArgb;
    static constexpr std::uint32_t meterFillArgb = ModernNorthStarFoundationTokens::accentArgb;
};

struct ModernNorthStarTypographyTokens final
{
    static constexpr float displaySize = 22.0f;
    static constexpr float titleSize = 14.0f;
    static constexpr float bodySize = 12.0f;
    static constexpr float captionSize = 10.0f;
    static constexpr float monoSize = 11.0f;
    static constexpr const char* displayPrimary = "Aptos Display";
    static constexpr const char* bodyPrimary = "Aptos";
    static constexpr const char* displayFallback = "Segoe UI Variable Display";
    static constexpr const char* bodyFallback = "Segoe UI Variable Text";
    static constexpr const char* monoPrimary = "Cascadia Mono";
    static constexpr const char* monoFallback = "Consolas";
};

struct ModernNorthStarSpacingTokens final
{
    static constexpr int compactGap = 6;
    static constexpr int standardGap = 10;
    static constexpr int expandedGap = 14;
    static constexpr int outerPadding = 10;
    static constexpr int controlPadding = 10;
    static constexpr int titleStripHeight = 28;
    static constexpr int resizeHandleExtent = 18;
    static constexpr int topBarHeight = 48;
    static constexpr int bottomBarHeight = 24;
};

struct ModernNorthStarRadiusTokens final
{
    static constexpr float mag = 4.0f;
    static constexpr float control = 3.0f;
    static constexpr float focus = 3.0f;
};

struct ModernNorthStarElevationTokens final
{
    static constexpr float settledShadowAlpha = 0.0f;
    static constexpr float dragShadowAlpha = 0.10f;
    static constexpr float focusRingWidth = 2.0f;
    static constexpr float magBorderWidth = 1.0f;
};

struct ModernNorthStarMotionTokens final
{
    static constexpr float postCoreDurationMs = 140.0f;
    static constexpr int timerIntervalMs = 16;
    static constexpr float enteringOpacityFloor = 0.86f;
};

using ModernNorthStarContentRect = RendererContentRect;

struct ModernNorthStarAssetToken final
{
    ModernNorthStarAssetId id = ModernNorthStarAssetId::unknown;
    const char* fileName = "";
    ModernNorthStarAssetOwner owner = ModernNorthStarAssetOwner::core;
};

class ModernNorthStarMotionController final
{
public:
    void start(float from, float to, bool enabled, bool reduceMotion) noexcept
    {
        startValue = std::clamp(from, 0.0f, 1.0f);
        currentValue = startValue;
        targetValue = std::clamp(to, 0.0f, 1.0f);
        elapsedMs = 0.0f;
        active = enabled && !reduceMotion && startValue != targetValue;
        if (!active)
            currentValue = targetValue;
    }

    void interrupt(float to, bool enabled, bool reduceMotion) noexcept
    {
        start(currentValue, to, enabled, reduceMotion);
    }

    void advance(float deltaMs) noexcept
    {
        if (!active)
            return;
        elapsedMs = std::max(0.0f, elapsedMs + deltaMs);
        const auto linear = std::clamp(elapsedMs / ModernNorthStarMotionTokens::postCoreDurationMs,
                                       0.0f,
                                       1.0f);
        const auto eased = linear * linear * (3.0f - (2.0f * linear));
        currentValue = startValue + ((targetValue - startValue) * eased);
        if (linear >= 1.0f)
        {
            currentValue = targetValue;
            active = false;
        }
    }

    void snapTo(float value) noexcept
    {
        currentValue = startValue = targetValue = std::clamp(value, 0.0f, 1.0f);
        elapsedMs = 0.0f;
        active = false;
    }

    [[nodiscard]] float value() const noexcept { return currentValue; }
    [[nodiscard]] bool isActive() const noexcept { return active; }

private:
    float startValue = 1.0f;
    float currentValue = 1.0f;
    float targetValue = 1.0f;
    float elapsedMs = 0.0f;
    bool active = false;
};

struct ModernNorthStarMotionPolicy final
{
    static void setEnabled(bool enabled) noexcept { motionEnabled.store(enabled, std::memory_order_relaxed); }
    static void setReduceMotion(bool reduced) noexcept { reduceMotion.store(reduced, std::memory_order_relaxed); }
    [[nodiscard]] static bool isEnabled() noexcept { return motionEnabled.load(std::memory_order_relaxed); }
    [[nodiscard]] static bool isReduceMotion() noexcept { return reduceMotion.load(std::memory_order_relaxed); }

private:
    static inline std::atomic_bool motionEnabled { true };
    static inline std::atomic_bool reduceMotion { false };
};

struct ModernNorthStarVisualContract final
{
    using Foundation = ModernNorthStarFoundationTokens;
    using Semantic = ModernNorthStarSemanticTokens;
    using Component = ModernNorthStarComponentTokens;
    using Typography = ModernNorthStarTypographyTokens;
    using Spacing = ModernNorthStarSpacingTokens;
    using Radius = ModernNorthStarRadiusTokens;
    using Elevation = ModernNorthStarElevationTokens;
    using Motion = ModernNorthStarMotionTokens;

    static constexpr auto backgroundArgb = Foundation::backgroundArgb;
    static constexpr auto surfaceArgb = Foundation::surfaceArgb;
    static constexpr auto foregroundArgb = Foundation::foregroundArgb;
    static constexpr auto mutedArgb = Foundation::mutedArgb;
    static constexpr auto borderArgb = Foundation::borderArgb;
    static constexpr auto accentArgb = Foundation::accentArgb;
    static constexpr auto surfaceSoftArgb = Foundation::surfaceSoftArgb;
    static constexpr auto accentSoftArgb = Foundation::accentSoftArgb;
    static constexpr auto accentInkArgb = Foundation::accentInkArgb;
    static constexpr auto dangerArgb = Foundation::dangerArgb;
    static constexpr auto successArgb = Foundation::successArgb;
    static constexpr auto warningArgb = Foundation::warningArgb;

    static constexpr int gapForDensity(ModernNorthStarDensity density) noexcept
    {
        switch (density)
        {
            case ModernNorthStarDensity::compact: return Spacing::compactGap;
            case ModernNorthStarDensity::standard: return Spacing::standardGap;
            case ModernNorthStarDensity::expanded: return Spacing::expandedGap;
        }
        return Spacing::standardGap;
    }

    static constexpr ModernNorthStarInformationTier informationTier(
        ModernNorthStarDensity density, ModernNorthStarMagSize size) noexcept
    {
        if (density == ModernNorthStarDensity::compact)
            return ModernNorthStarInformationTier::summary;
        if (density == ModernNorthStarDensity::expanded)
            return size == ModernNorthStarMagSize::small
                ? ModernNorthStarInformationTier::standard
                : ModernNorthStarInformationTier::detail;
        if (size == ModernNorthStarMagSize::small)
            return ModernNorthStarInformationTier::summary;
        if (size == ModernNorthStarMagSize::large)
            return ModernNorthStarInformationTier::detail;
        return ModernNorthStarInformationTier::standard;
    }

    static constexpr ModernNorthStarInformationTier contentCapacity(
        ModernNorthStarContentRect content) noexcept
    {
        if (content.width >= 280 && content.height >= 170)
            return ModernNorthStarInformationTier::detail;
        if (content.width >= 150 && content.height >= 90)
            return ModernNorthStarInformationTier::standard;
        return ModernNorthStarInformationTier::summary;
    }

    static constexpr ModernNorthStarInformationTier resolvedInformationTier(
        ModernNorthStarDensity density,
        ModernNorthStarMagSize size,
        ModernNorthStarContentRect content) noexcept
    {
        const auto requested = informationTier(density, size);
        const auto capacity = contentCapacity(content);
        return static_cast<int>(requested) <= static_cast<int>(capacity) ? requested : capacity;
    }

    static constexpr bool sweetSpotSatisfied(ModernNorthStarMagSize size,
                                             ModernNorthStarContentRect content) noexcept
    {
        switch (size)
        {
            case ModernNorthStarMagSize::small: return content.width >= 110 && content.height >= 72;
            case ModernNorthStarMagSize::large: return content.width >= 280 && content.height >= 170;
            case ModernNorthStarMagSize::automatic:
            case ModernNorthStarMagSize::medium: return content.width >= 150 && content.height >= 90;
        }
        return false;
    }

    static constexpr int mainAxisWeight(ModernNorthStarMagSize size) noexcept
    {
        switch (size)
        {
            case ModernNorthStarMagSize::small: return 1;
            case ModernNorthStarMagSize::large: return 3;
            case ModernNorthStarMagSize::automatic:
            case ModernNorthStarMagSize::medium: return 2;
        }
        return 2;
    }

    static constexpr bool usesHorizontalMainAxis(ModernNorthStarEdge edge) noexcept
    {
        return edge == ModernNorthStarEdge::north || edge == ModernNorthStarEdge::south;
    }

    static constexpr ModernNorthStarAssetToken assetToken(ModernNorthStarAssetId id) noexcept
    {
        switch (id)
        {
            case ModernNorthStarAssetId::openFiles: return { id, "open-files.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::remove: return { id, "delete.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::preferences: return { id, "Settings.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::save: return { id, "save.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::skipBack: return { id, "skip-back.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::previousFrame: return { id, "PreviousFrame.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::nextFrame: return { id, "NextFrame.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::play: return { id, "play.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::pause: return { id, "pause.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::stop: return { id, "stop.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::toggle: return { id, "toggle.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::loop: return { id, "loop.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::fadeIn: return { id, "fade-in.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::fadeOut: return { id, "fade-out.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::undo: return { id, "undo.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::redo: return { id, "redo.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::reset: return { id, "reset.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::zoomIn: return { id, "zoom-in.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::zoomOut: return { id, "zoom-out.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::waveform: return { id, "waveform.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::spectrum: return { id, "spectrum.svg", ModernNorthStarAssetOwner::core };
            case ModernNorthStarAssetId::unknown: break;
        }
        return {};
    }

    static constexpr ModernNorthStarAssetId assetIdForFileName(std::string_view fileName) noexcept
    {
        constexpr std::array ids {
            ModernNorthStarAssetId::openFiles, ModernNorthStarAssetId::remove,
            ModernNorthStarAssetId::preferences,
            ModernNorthStarAssetId::save, ModernNorthStarAssetId::skipBack,
            ModernNorthStarAssetId::previousFrame, ModernNorthStarAssetId::nextFrame,
            ModernNorthStarAssetId::play, ModernNorthStarAssetId::pause,
            ModernNorthStarAssetId::stop, ModernNorthStarAssetId::toggle,
            ModernNorthStarAssetId::loop, ModernNorthStarAssetId::fadeIn,
            ModernNorthStarAssetId::fadeOut, ModernNorthStarAssetId::undo,
            ModernNorthStarAssetId::redo, ModernNorthStarAssetId::reset,
            ModernNorthStarAssetId::zoomIn, ModernNorthStarAssetId::zoomOut,
            ModernNorthStarAssetId::waveform, ModernNorthStarAssetId::spectrum
        };
        for (const auto id : ids)
            if (fileName == assetToken(id).fileName)
                return id;
        return ModernNorthStarAssetId::unknown;
    }
};
} // namespace singleeditor
