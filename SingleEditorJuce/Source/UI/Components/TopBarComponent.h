#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace singleeditor
{
class TopBarComponent final : public juce::Component
{
public:
    explicit TopBarComponent(juce::ResizableWindow& hostWindowToDrag);

    void setTitle(const juce::String& newTitle);
    void setSubtitle(const juce::String& newSubtitle);
    void resized() override;
    void paint(juce::Graphics& g) override;

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;

private:
    class WindowControlButton final : public juce::Button
    {
    public:
        enum class Type { minimise, maximise, close };

        explicit WindowControlButton(Type typeToUse);
        void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    private:
        Type type;
    };

    juce::ResizableWindow& hostWindow;
    juce::ComponentDragger dragger;
    WindowControlButton minimiseButton;
    WindowControlButton maximiseButton;
    WindowControlButton closeButton;
    juce::Label fileMenuLabel;
    juce::Label editMenuLabel;
    juce::Label viewMenuLabel;
    juce::Label windowMenuLabel;
    juce::Label helpMenuLabel;
    juce::String currentTitle = "Single Editor";
};
} // namespace singleeditor
