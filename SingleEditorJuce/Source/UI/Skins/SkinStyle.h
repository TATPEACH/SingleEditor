#pragma once

#include <cstdint>
#include <string_view>

#include <juce_gui_basics/juce_gui_basics.h>

#include "UI/LookAndFeel/ModernGlassTokens.h"

namespace singleeditor
{
// Optional Modern skins. `original` keeps every existing Modern Glass paint path;
// the others are chosen at runtime from More > Appearance and only take effect
// after CoreReady, except for their palette, which the startup frame also uses.
enum class ModernSkin { original = 0, soft, liquid, tiles };

// Roles the original palette does not name. Colours are ARGB.
struct SkinExtras final
{
    std::uint32_t sunken;       // lanes, graphs, meter tracks, clock display
    std::uint32_t hairline;     // translucent separators and tile outlines
    std::uint32_t signal;       // waveform, meters, EQ and limiter curves
    std::uint32_t live;         // processor preview / warning
    std::uint32_t danger;       // clip and failure
    std::uint32_t glassTint;    // translucent body over the window backdrop
    std::uint32_t glassRim;     // top rim light of glass and tactile controls
    std::uint32_t shadowLight;  // Soft: highlight shadow towards the light
    std::uint32_t shadowDark;   // contact / drop shadow
};

struct SkinGeometry final
{
    float tileRadius;
    float controlRadius;
    int gap;
    int outerPadding;
};

class SkinStyle final
{
public:
    // Classic always reports `original`.
    [[nodiscard]] static ModernSkin active() noexcept;
    [[nodiscard]] static bool isOriginal() noexcept { return active() == ModernSkin::original; }
    static void setActive(ModernSkin skin) noexcept;

    [[nodiscard]] static ModernSkin parse(std::string_view value,
                                          ModernSkin fallback = ModernSkin::original) noexcept;
    [[nodiscard]] static std::string_view id(ModernSkin skin) noexcept;
    [[nodiscard]] static const char* menuName(ModernSkin skin) noexcept;
    // Skins compiled into this build; `original` is always available.
    [[nodiscard]] static bool isAvailable(ModernSkin skin) noexcept;
    // Light-only skins (Soft) report false; the stored Night request is kept.
    [[nodiscard]] static bool supportsNight(ModernSkin skin) noexcept;

    // Valid for non-original skins; `night` is the effective theme.
    [[nodiscard]] static const ModernGlassTokens::Palette& palette(bool night) noexcept;
    [[nodiscard]] static const SkinExtras& extras(bool night) noexcept;
    [[nodiscard]] static SkinGeometry geometry() noexcept;

    // Data colour (waveform, meters, curves) for the active skin and theme.
    [[nodiscard]] static juce::Colour signal();

    // ---- Paint hooks. Callers invoke these only when !isOriginal(). ----
    // Window canvas; `origin` is the area's top-left in top-level window pixels,
    // so the title bar and the editor can paint one continuous backdrop.
    static void paintWindowBackground(juce::Graphics& g, juce::Rectangle<float> area,
                                      juce::Point<float> origin, juce::Rectangle<float> window);
    static void paintTitleBar(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> origin,
                              juce::Rectangle<float> window);
    static void paintToolbar(juce::Graphics& g, juce::Component& surface);
    static void paintStatusBar(juce::Graphics& g, juce::Rectangle<float> bounds);
    static void paintPanel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
                           int icon, int trailingControls);
    static void paintPanelShadow(juce::Graphics& g, juce::Rectangle<float> tile);
    static void paintPanelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live);
    static void paintDataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius);
    static void prepareWaveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour colour);
    static void paintClock(juce::Graphics& g, juce::Rectangle<float> bounds,
                           const juce::String& main, const juce::String& frames);

    // Controls. Return false to fall back to the original drawing.
    static void paintIconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                                bool& onAccent);
    static bool paintButtonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                                      juce::Colour& textColour);
    static void paintSwitch(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled, bool focused);
    static void paintComboBox(juce::Graphics& g, juce::ComboBox& box, bool down);
    static void paintLinearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position,
                                  juce::Slider& slider);
    static void paintPopupBackground(juce::Graphics& g, juce::Rectangle<float> bounds);
    static void paintPopupHighlight(juce::Graphics& g, juce::Rectangle<float> row);
    [[nodiscard]] static bool popupHighlightUsesAccent() noexcept;
    static void paintKnob(juce::Graphics& g, juce::Rectangle<float> dial, float angle,
                          juce::Colour indicator, bool enabled, bool focused);
    static void paintMeterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail,
                              bool loudness);
    static void paintMeterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held);
};
} // namespace singleeditor
