#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace singleeditor
{
class ModernIconButton final : public juce::Button
{
public:
    explicit ModernIconButton(const juce::String& componentName = {});

    void setIconName(const juce::String& newIconName);
    void setIconDrawable(std::unique_ptr<juce::Drawable> newDrawable);
    void setIconPadding(int newPadding);
    void setNominalIconSize(int newSize);
    void setCornerRadius(float newCornerRadius);
    void setTintColours(juce::Colour normalColour, juce::Colour disabledColour);

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void enablementChanged() override;
    void lookAndFeelChanged() override;

private:
    void refreshIcons();
    void prepareIconsIfNeeded();

    juce::String iconName;
    int iconPadding = 6;
    int nominalIconSize = 18;
    float cornerRadius = 8.0f;
    juce::Colour normalTint;
    juce::Colour disabledTint;
    bool customTint = false;
    bool iconsNeedPreparation = false;
    std::unique_ptr<juce::Drawable> sourceDrawable;
    std::unique_ptr<juce::Drawable> normalIcon;
    std::unique_ptr<juce::Drawable> disabledIcon;
    // Optional skins draw a primary transport key on a filled accent.
    std::unique_ptr<juce::Drawable> onAccentIcon;
};
} // namespace singleeditor
