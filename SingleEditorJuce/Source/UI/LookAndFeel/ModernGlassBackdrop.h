#pragma once

#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
struct ModernGlassBackdrop final
{
    // Pure image processing. Call only for an explicit, visible post-Core
    // transient surface, never from its paint callback or audio code.
    static juce::Image blur(const juce::Image& source, float radius, juce::Colour background);
    static juce::Point<float> refractedPoint(juce::Point<float> point,
        juce::Rectangle<float> body, float cornerRadius, float strength) noexcept;
    static juce::Image sample(const juce::Image& blurred, juce::Rectangle<int> body,
        float cornerRadius, ModernGlassMaterial material, float strength = 1.0f);
};
} // namespace singleeditor
