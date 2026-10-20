#pragma once

#include <memory>
#include <unordered_map>

#include <juce_gui_extra/juce_gui_extra.h>

#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
class ModernAppleLookAndFeel final : public WindowChromeLookAndFeel
{
public:
    enum class SkinMode
    {
        startup = 0,
        full
    };

    ModernAppleLookAndFeel();

    enum class LabelRole { primary = 1, secondary, accent };
    static void setLabelRole(juce::Label& label, LabelRole role)
    {
        label.getProperties().set("modernLabelRole", static_cast<int>(role));
        label.setColour(juce::Label::textColourId,
            role == LabelRole::secondary ? RendererVisualStyle::secondaryText()
            : role == LabelRole::accent ? (SkinStyle::isOriginal() ? RendererVisualStyle::accentBlue() : SkinStyle::signal())
            : RendererVisualStyle::primaryText());
    }
    // Rebind an existing component tree after changing the selected theme.
    // Does not rebuild modules, reload audio, or move/focus native windows.
    static void refreshTheme(juce::Component& root);
    void refreshColours();
    int getAlertWindowButtonHeight() override { return 32; }
    juce::Font getAlertWindowTitleFont() override { return juce::Font(juce::FontOptions(16.0f, juce::Font::bold)); }
    juce::Font getAlertWindowMessageFont() override { return juce::Font(juce::FontOptions(13.0f)); }
    juce::Font getAlertWindowFont() override { return juce::Font(juce::FontOptions(13.0f)); }
    void drawAlertBox(juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>&,
                      juce::TextLayout&) override;

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent(juce::Component&) override;

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics& g,
                        juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;
    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float position, float minimum, float maximum,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    void drawComboBox(juce::Graphics& g,
                      int width,
                      int height,
                      bool isButtonDown,
                      int buttonX,
                      int buttonY,
                      int buttonW,
                      int buttonH,
                      juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu(juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;
    int getMenuWindowFlags() override;
    void getIdealPopupMenuItemSize(const juce::String&, bool isSeparator, int standardHeight,
                                  int& idealWidth, int& idealHeight) override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                          bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                          bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                          const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader(juce::Graphics&, const juce::Rectangle<int>&,
                                    const juce::String&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override;
    void drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override;

    static void setSkinMode(SkinMode newMode) noexcept;
    static SkinMode getSkinMode() noexcept;
    static bool isStartupSkin() noexcept;
    static juce::Colour windowBackground();
    static juce::Colour panelBackground();
    static juce::Colour elevatedPanelBackground();
    static juce::Colour workspaceSurface();
    static juce::Colour mutedSurface();
    static juce::Colour titleBarBackground();
    static juce::Colour borderColour();
    static juce::Colour primaryText();
    static juce::Colour secondaryText();
    static juce::Colour disabledText();
    static juce::Colour accentOrange();
    static juce::Colour accentBlue();
    static juce::Colour accentPurple();
    static juce::Colour waveformBlue();
    static juce::Colour waveformCyan();
    static juce::Colour ghostHoverFill();
    static juce::Colour ghostPressedFill();
    static float cardRadius() noexcept;
    static float componentRadius() noexcept;
    static int outerPadding() noexcept;
    static int interPanelGap() noexcept;
    static int topBarHeight() noexcept;
    static int bottomBarHeight() noexcept;

private:
    juce::Drawable* getCachedButtonIcon(ModernNorthStarAssetId assetId) const;
    juce::String preferredFontName;
    mutable juce::CriticalSection iconCacheLock;
    mutable std::unordered_map<std::string, std::unique_ptr<juce::Drawable>> iconCache;
};
} // namespace singleeditor
