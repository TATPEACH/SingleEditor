#include "UI/Renderer/RendererVisualStyle.h"

#include <array>
#include <atomic>

#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"
#include "UI/LookAndFeel/ModernGlassTokens.h"

namespace singleeditor
{
namespace
{
constexpr std::uint32_t classicWindow = 0xffd4d0c8;
constexpr std::uint32_t classicPanel = 0xffece9e2;
constexpr std::uint32_t classicSurface = 0xfff7f5ef;
constexpr std::uint32_t classicMutedSurface = 0xffdfdcd4;
constexpr std::uint32_t classicBorder = 0xff646464;
constexpr std::uint32_t classicText = 0xff101010;
constexpr std::uint32_t classicMutedText = 0xff4a4a4a;
constexpr std::uint32_t classicAccent = 0xff0a246a;
constexpr std::uint32_t classicAccentSoft = 0xffc7d5f2;
constexpr std::uint32_t classicAccentInk = 0xffffffff;
constexpr std::uint32_t classicDanger = 0xffa31515;
constexpr std::uint32_t classicWaveform = 0xff006b6b;
constexpr std::array<std::uint32_t, 6> classicWaveformLanes {
    0xff006b6b, 0xff008080, 0xff2b5f5f, 0xff317d7d, 0xff005757, 0xff4a8e8e
};
std::atomic_bool reducedMotion { false };
std::atomic<ModernTheme> selectedTheme { ModernTheme::light };
std::atomic<ModernGlassMaterial> selectedGlass { ModernGlassMaterial::frosted };

RendererKind activeKind() noexcept
{
    return RendererSelection::selected();
}

void noteTokens() noexcept
{
    RendererSelection::noteTokenSliceAccess(activeKind());
}

juce::Colour colour(std::uint32_t classicArgb, std::uint32_t modernArgb)
{
    noteTokens();
    return juce::Colour(activeKind() == RendererKind::classic ? classicArgb : modernArgb);
}
}

void RendererMotionController::start(float from, float to, bool enabled, bool reduceMotion,
                                     float durationMsToUse) noexcept
{
    startValue = std::clamp(from, 0.0f, 1.0f);
    currentValue = startValue;
    targetValue = std::clamp(to, 0.0f, 1.0f);
    elapsedMs = 0.0f;
    durationMs = std::max(1.0f, durationMsToUse);
    active = enabled && !reduceMotion && startValue != targetValue;
    if (!active)
        currentValue = targetValue;
}

void RendererMotionController::interrupt(float to, bool enabled, bool reduceMotion,
                                         float durationMsToUse) noexcept
{
    start(currentValue, to, enabled, reduceMotion, durationMsToUse);
}

void RendererMotionController::advance(float deltaMs) noexcept
{
    if (!active)
        return;
    elapsedMs = std::max(0.0f, elapsedMs + deltaMs);
    const auto linear = std::clamp(elapsedMs / durationMs, 0.0f, 1.0f);
    const auto eased = linear * linear * (3.0f - (2.0f * linear));
    currentValue = startValue + ((targetValue - startValue) * eased);
    if (linear >= 1.0f)
    {
        currentValue = targetValue;
        active = false;
    }
}

void RendererMotionController::snapTo(float value) noexcept
{
    currentValue = startValue = targetValue = std::clamp(value, 0.0f, 1.0f);
    elapsedMs = 0.0f;
    active = false;
}

bool RendererVisualStyle::isClassic() noexcept { return activeKind() == RendererKind::classic; }
void RendererVisualStyle::setModernTheme(ModernTheme theme) noexcept { selectedTheme.store(theme, std::memory_order_relaxed); }
ModernTheme RendererVisualStyle::modernTheme() noexcept { return selectedTheme.load(std::memory_order_relaxed); }
bool RendererVisualStyle::isNight() noexcept { return !isClassic() && modernTheme() == ModernTheme::night; }
void RendererVisualStyle::setGlassMaterial(ModernGlassMaterial material) noexcept { selectedGlass.store(material, std::memory_order_relaxed); }
ModernGlassMaterial RendererVisualStyle::glassMaterial() noexcept
{
    return isClassic() ? ModernGlassMaterial::solid : selectedGlass.load(std::memory_order_relaxed);
}
const ModernGlassTokens::Palette& RendererVisualStyle::modernPalette() noexcept
{
    return isNight() ? ModernGlassTokens::night : ModernGlassTokens::light;
}
juce::Colour RendererVisualStyle::windowBackground() { return colour(classicWindow, modernPalette().background); }
juce::Colour RendererVisualStyle::panelBackground() { return colour(classicPanel, modernPalette().surface); }
juce::Colour RendererVisualStyle::elevatedPanelBackground() { return colour(classicSurface, modernPalette().surface); }
juce::Colour RendererVisualStyle::workspaceSurface() { return colour(classicSurface, modernPalette().surface); }
juce::Colour RendererVisualStyle::mutedSurface() { return colour(classicMutedSurface, modernPalette().control); }
juce::Colour RendererVisualStyle::titleBarBackground() { return colour(classicWindow, modernPalette().background); }
juce::Colour RendererVisualStyle::borderColour() { return colour(classicBorder, modernPalette().border); }
juce::Colour RendererVisualStyle::primaryText() { return colour(classicText, modernPalette().text); }
juce::Colour RendererVisualStyle::secondaryText() { return colour(classicMutedText, modernPalette().secondaryText); }
juce::Colour RendererVisualStyle::disabledText() { return secondaryText().withAlpha(0.52f); }
juce::Colour RendererVisualStyle::accentOrange() { return colour(0xff8b5a00, ModernNorthStarVisualContract::warningArgb); }
juce::Colour RendererVisualStyle::accentBlue() { return colour(classicAccent, modernPalette().accent); }
juce::Colour RendererVisualStyle::accentPurple() { return colour(0xff5a2d82, modernPalette().selectionText); }
juce::Colour RendererVisualStyle::waveformBlue() { return colour(classicWaveform, modernPalette().waveform[0]); }
juce::Colour RendererVisualStyle::waveformCyan() { return colour(0xff80bcbc, modernPalette().selection); }
juce::Colour RendererVisualStyle::ghostHoverFill() { return isClassic() ? juce::Colour(0x140a246a) : juce::Colour(isNight() ? 0x12ffffff : 0x0d000000); }
juce::Colour RendererVisualStyle::ghostPressedFill() { return isClassic() ? juce::Colour(0x240a246a) : juce::Colour(isNight() ? 0x20ffffff : 0x1a000000); }

juce::Colour RendererVisualStyle::waveformLaneColour(int index)
{
    noteTokens();
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, 5));
    return juce::Colour(isClassic() ? classicWaveformLanes[i]
                                    : modernPalette().waveform[i]);
}

juce::Colour RendererVisualStyle::waveformSurface() { return colour(classicSurface, modernPalette().surface); }
juce::Colour RendererVisualStyle::meterFill() { return colour(classicWaveform, modernPalette().accent); }
juce::Colour RendererVisualStyle::danger() { return colour(classicDanger, ModernNorthStarVisualContract::dangerArgb); }
juce::Colour RendererVisualStyle::accentSoft() { return colour(classicAccentSoft, modernPalette().selection); }
juce::Colour RendererVisualStyle::accentInk() { return colour(classicAccentInk, modernPalette().selectionText); }

float RendererVisualStyle::cardRadius() noexcept { noteTokens(); return isClassic() ? 0.0f : ModernGlassTokens::cardRadius; }
float RendererVisualStyle::componentRadius() noexcept { noteTokens(); return isClassic() ? 0.0f : ModernGlassTokens::controlRadius; }
float RendererVisualStyle::focusRadius() noexcept { noteTokens(); return isClassic() ? 0.0f : ModernGlassTokens::controlRadius; }
float RendererVisualStyle::focusRingWidth() noexcept { noteTokens(); return isClassic() ? 1.0f : ModernNorthStarVisualContract::Elevation::focusRingWidth; }
float RendererVisualStyle::magBorderWidth() noexcept { noteTokens(); return isClassic() ? 1.0f : ModernNorthStarVisualContract::Elevation::magBorderWidth; }
int RendererVisualStyle::outerPadding() noexcept { noteTokens(); return isClassic() ? 6 : ModernNorthStarVisualContract::Spacing::outerPadding; }
int RendererVisualStyle::interPanelGap() noexcept { noteTokens(); return isClassic() ? 4 : ModernNorthStarVisualContract::Spacing::standardGap; }
int RendererVisualStyle::topBarHeight() noexcept { noteTokens(); return isClassic() ? 40 : ModernNorthStarVisualContract::Spacing::topBarHeight; }
int RendererVisualStyle::bottomBarHeight() noexcept { noteTokens(); return isClassic() ? 20 : ModernNorthStarVisualContract::Spacing::bottomBarHeight; }
int RendererVisualStyle::titleStripHeight() noexcept { noteTokens(); return isClassic() ? 24 : ModernNorthStarVisualContract::Spacing::titleStripHeight; }
int RendererVisualStyle::resizeHandleExtent() noexcept { noteTokens(); return isClassic() ? 14 : ModernNorthStarVisualContract::Spacing::resizeHandleExtent; }

int RendererVisualStyle::gapForDensity(RendererDensity density) noexcept
{
    noteTokens();
    if (isClassic())
        return density == RendererDensity::compact ? 2 : (density == RendererDensity::expanded ? 6 : 4);
    return ModernNorthStarVisualContract::gapForDensity(density);
}

float RendererVisualStyle::titleTextSize() noexcept { noteTokens(); return isClassic() ? 13.0f : ModernNorthStarVisualContract::Typography::titleSize; }
float RendererVisualStyle::bodyTextSize() noexcept { noteTokens(); return isClassic() ? 12.0f : ModernNorthStarVisualContract::Typography::bodySize; }
float RendererVisualStyle::captionTextSize() noexcept { noteTokens(); return isClassic() ? 10.0f : ModernNorthStarVisualContract::Typography::captionSize; }
float RendererVisualStyle::monoTextSize() noexcept { noteTokens(); return isClassic() ? 11.0f : ModernNorthStarVisualContract::Typography::monoSize; }

RendererInformationTier RendererVisualStyle::resolvedInformationTier(
    RendererDensity density, RendererMagSize size, RendererContentRect content) noexcept
{
    noteTokens();
    if (isClassic())
    {
        auto requested = RendererInformationTier::standard;
        if (density == RendererDensity::compact || size == RendererMagSize::small)
            requested = RendererInformationTier::summary;
        else if (density == RendererDensity::expanded || size == RendererMagSize::large)
            requested = RendererInformationTier::detail;

        const auto capacity = content.width >= 280 && content.height >= 170
            ? RendererInformationTier::detail
            : (content.width >= 150 && content.height >= 90
                ? RendererInformationTier::standard
                : RendererInformationTier::summary);
        return static_cast<int>(requested) <= static_cast<int>(capacity) ? requested : capacity;
    }
    return ModernNorthStarVisualContract::resolvedInformationTier(density, size, content);
}

bool RendererVisualStyle::sweetSpotSatisfied(RendererMagSize size, RendererContentRect content) noexcept
{
    noteTokens();
    if (isClassic())
    {
        if (size == RendererMagSize::small)
            return content.width >= 100 && content.height >= 64;
        if (size == RendererMagSize::large)
            return content.width >= 260 && content.height >= 150;
        return content.width >= 140 && content.height >= 82;
    }
    return ModernNorthStarVisualContract::sweetSpotSatisfied(size, content);
}

RendererAssetToken RendererVisualStyle::assetToken(RendererAssetId id) noexcept
{
    RendererSelection::noteAssetLookup(activeKind());
    if (isClassic())
        return { id, "", RendererAssetOwner::core };
    const auto token = ModernNorthStarVisualContract::assetToken(id);
    return { token.id, token.fileName, token.owner };
}

RendererAssetId RendererVisualStyle::assetIdForFileName(std::string_view fileName) noexcept
{
    RendererSelection::noteAssetLookup(activeKind());
    if (isClassic())
        return RendererAssetId::unknown;
    return ModernNorthStarVisualContract::assetIdForFileName(fileName);
}

void RendererVisualStyle::setReduceMotion(bool reduced) noexcept
{
    reducedMotion.store(reduced, std::memory_order_relaxed);
    if (!isClassic())
        ModernNorthStarMotionPolicy::setReduceMotion(reduced);
}

bool RendererVisualStyle::isReduceMotion() noexcept
{
    return isClassic() || reducedMotion.load(std::memory_order_relaxed);
}

bool RendererVisualStyle::isMotionEnabled() noexcept
{
    return !isClassic() && ModernNorthStarMotionPolicy::isEnabled();
}

float RendererVisualStyle::motionDurationMs() noexcept { return isClassic() ? 0.0f : ModernNorthStarVisualContract::Motion::postCoreDurationMs; }
float RendererVisualStyle::enteringOpacityFloor() noexcept { return isClassic() ? 1.0f : ModernNorthStarVisualContract::Motion::enteringOpacityFloor; }
int RendererVisualStyle::motionTimerIntervalMs() noexcept { return isClassic() ? 0 : ModernNorthStarVisualContract::Motion::timerIntervalMs; }
void RendererVisualStyle::noteMotionStart() noexcept { RendererSelection::noteMotionStart(activeKind()); }
} // namespace singleeditor
