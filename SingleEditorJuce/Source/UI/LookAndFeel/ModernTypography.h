#pragma once

#include <juce_graphics/juce_graphics.h>

#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"

namespace singleeditor
{
// Round 8 uses CSS pixel sizes (the em square). JUCE's ordinary height scales
// ascent + descent instead and made the same numeric size visibly too small.
// In this JUCE version point height is the em square in logical pixels, not a
// 72-to-96 DPI conversion. Device scaling remains the native peer's job.
struct ModernTypography final
{
    static juce::Font font(float emPixels = 13.0f, int style = juce::Font::plain)
    {
        return juce::Font(juce::FontOptions(emPixels, style).withPointHeight(emPixels));
    }

    // The family that unnamed (default sans-serif) fonts resolve to. The startup
    // chrome and the full Modern LookAndFeel must agree: JUCE keeps the glyph
    // layouts made before CoreReady. Each candidate is one DirectWrite family
    // lookup (a missing family comes back under another name), so this stays
    // cheap enough for the first frame, unlike Font::findAllTypefaceNames().
    static const juce::String& sansSerifFamily()
    {
        static const juce::String family = []
        {
            using Contract = ModernNorthStarVisualContract::Typography;
            // Round 8 body family order; display faces should not be used for every
            // compact label. Keep installed-font fallback and native Unicode shaping.
            for (const auto* candidate : { "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI",
                                           Contract::displayPrimary, Contract::bodyPrimary,
                                           Contract::displayFallback, "Microsoft YaHei", "PingFang SC",
                                           "PingFang", "Helvetica Neue", "Helvetica", "Arial" })
            {
                const auto face = juce::Typeface::createSystemTypefaceFor(
                    juce::Font(juce::FontOptions(candidate, 13.0f, juce::Font::plain)));
                if (face != nullptr && face->getName() == candidate)
                    return juce::String(candidate);
            }
            return juce::String();
        }();
        return family;
    }
};
}
