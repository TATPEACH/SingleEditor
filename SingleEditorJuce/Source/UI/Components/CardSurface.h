#pragma once

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernGlassPainter.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
class CardSurface : public juce::Component
{
public:
    explicit CardSurface(float cornerRadiusToUse = ModernAppleLookAndFeel::cardRadius()) : cornerRadius(cornerRadiusToUse) {}

    void paint(juce::Graphics& g) override
    {
        if (static_cast<bool>(getProperties().getWithDefault("glassChrome", false)))
        {
            if (!SkinStyle::isOriginal())
            {
                SkinStyle::paintToolbar(g, *this);
                return;
            }
            ModernGlassPainter::surface(g, getLocalBounds().toFloat(),
                                         ModernAppleLookAndFeel::panelBackground(), 12.0f);
            return;
        }
        if (!SkinStyle::isOriginal() && !ModernAppleLookAndFeel::isStartupSkin()
            && getProperties()["skinRole"].toString() == "statusBar")
        {
            SkinStyle::paintStatusBar(g, getLocalBounds().toFloat());
            return;
        }
        auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        auto fill = ModernAppleLookAndFeel::panelBackground();

        if (auto colour = getProperties()["fillColour"]; colour.isInt() || colour.isInt64())
            fill = juce::Colour(static_cast<juce::uint32>(static_cast<int64_t>(colour)));

        const auto fillAlpha = static_cast<float>(getProperties().getWithDefault("fillAlpha", 1.0));
        const auto shadowAlpha = static_cast<float>(getProperties().getWithDefault("shadowAlpha", 0.008));

        fill = fill.withAlpha(fillAlpha);

        if (shadowAlpha > 0.0f)
        {
            auto shadowBounds = bounds.translated(0.0f, 4.0f);
            g.setColour(juce::Colours::black.withAlpha(shadowAlpha));
            g.fillRoundedRectangle(shadowBounds, cornerRadius + 1.0f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(bounds, cornerRadius);
    }

private:
    float cornerRadius;
};
} // namespace singleeditor
