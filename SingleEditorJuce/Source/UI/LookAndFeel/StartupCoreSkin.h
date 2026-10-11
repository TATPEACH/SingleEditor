#pragma once

#include <juce_graphics/juce_graphics.h>
#include "UI/LookAndFeel/ModernGlassTokens.h"

namespace singleeditor
{
// The caller supplies the already-read appearance preference. This lightweight
// value owns no LookAndFeel, assets, timers, threads or renderer state, so the
// first peer can match the eventual editor without attaching its full skin.
struct StartupCoreSkin final
{
    struct Palette
    {
        juce::Colour windowBackground, panelBackground, workspaceSurface;
        juce::Colour primaryText, secondaryText, waveform, ready, failure;
    };

    static Palette palette(bool modern, ModernTheme theme) noexcept
    {
        if (!modern)
            return { juce::Colour(0xff0d1117), juce::Colour(0xff161b22), juce::Colour(0xff0f1721),
                     juce::Colour(0xfff0f3f6), juce::Colour(0xff8b949e), juce::Colour(0xff58a6ff),
                     juce::Colour(0xff3fb950), juce::Colour(0xfff85149) };

        const auto night = theme == ModernTheme::night;
        const auto& colours = night ? ModernGlassTokens::night : ModernGlassTokens::light;
        return { juce::Colour(colours.background), juce::Colour(colours.surface), juce::Colour(colours.control),
                 juce::Colour(colours.text), juce::Colour(colours.secondaryText), juce::Colour(colours.waveform[0]),
                 juce::Colour(night ? 0xff3fb950 : 0xff218144),
                 juce::Colour(night ? 0xfff85149 : 0xffce3039) };
    }
};
} // namespace singleeditor
