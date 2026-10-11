#pragma once

#include <juce_graphics/juce_graphics.h>

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
};
}
