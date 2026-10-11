#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace singleeditor
{
class CustomTitleBarComponent final : public juce::Component
{
public:
    enum class MenuKind { file, edit, view, window, help };

    explicit CustomTitleBarComponent(juce::ResizableWindow& hostWindowToDrag);

    void setTitle(const juce::String& newTitle);
    void setSubtitle(const juce::String& newSubtitle);
    void resized() override;
    void paint(juce::Graphics& g) override;

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;

    std::function<void(MenuKind, juce::Rectangle<int>)> onMenuRequested;
    std::function<void()> onMinimiseRequested;
    std::function<void()> onMaximiseRequested;
    std::function<void()> onCloseRequested;

private:
    class MenuButton final : public juce::Button
    {
    public:
        MenuButton(const juce::String& textToUse, MenuKind kindToUse);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        MenuKind kind;
    };

    class WindowControlButton final : public juce::Button
    {
    public:
        enum class Type { minimise, maximise, close };

        explicit WindowControlButton(Type typeToUse);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        Type type;
    };

    static void configureMenuButton(MenuButton& button, const juce::String& text);

    juce::ResizableWindow& hostWindow;
    juce::ComponentDragger dragger;
    juce::String subtitle;
    MenuButton fileMenuButton;
    MenuButton editMenuButton;
    MenuButton viewMenuButton;
    MenuButton windowMenuButton;
    MenuButton helpMenuButton;
    WindowControlButton minimiseButton;
    WindowControlButton maximiseButton;
    WindowControlButton closeButton;
    juce::String currentTitle;
};
} // namespace singleeditor
