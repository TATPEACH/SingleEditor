#pragma once

#include <array>
#include <cstdint>

namespace singleeditor
{
enum class ModernTheme { light, night };
enum class ModernGlassMaterial { solid, frosted, liquid };

// Round 8, round8-toolbar-20260905: the approved prototype's solid
// native mapping. No runtime JSON, bitmap material or compositor dependency.
struct ModernGlassTokens final
{
    struct Palette
    {
        std::uint32_t background, surface, control, text, secondaryText, border;
        std::uint32_t accent, selection, selectionText;
        std::array<std::uint32_t, 6> waveform;
    };
    static constexpr std::uint32_t background = 0xfff3f6fc;
    static constexpr std::uint32_t surface = 0xfffcfdfe;
    static constexpr std::uint32_t control = 0xfff1f5fb;
    static constexpr std::uint32_t text = 0xff363a44;
    static constexpr std::uint32_t secondaryText = 0xff626c7c;
    static constexpr std::uint32_t border = 0xffdce3ed;
    static constexpr std::uint32_t accent = 0xff0e71fe;
    static constexpr std::uint32_t selection = 0xffd9e8ff;
    static constexpr std::uint32_t selectionText = 0xff174b91;
    static constexpr std::array<std::uint32_t, 6> waveform {
        0xff527cba, 0xff638dc8, 0xff426dab, 0xff7498ce, 0xff395f98, 0xff84a6d6
    };
    static constexpr Palette light {
        background, surface, control, text, secondaryText, border,
        accent, selection, selectionText, waveform
    };
    // OD's Night OKLCH foundation converted to sRGB; dividers and muted text
    // are lifted slightly for dense native controls and small desktop labels.
    static constexpr Palette night {
        0xff0d1a27, 0xff0d1925, 0xff162737, 0xffebeff3, 0xff96a4b3, 0xff304354,
        0xff2f92ff, 0xff1d3d60, 0xffaed3ff,
        { 0xff82acdf, 0xff91b7e6, 0xff739fd6, 0xff9cbfea, 0xff6995cd, 0xffa7c7ef }
    };
    static constexpr float cardRadius = 10.0f;
    static constexpr float controlRadius = 7.0f;
    static constexpr int minimumWindowWidth = 800;
    static constexpr int minimumWindowHeight = 600;
};
} // namespace singleeditor
