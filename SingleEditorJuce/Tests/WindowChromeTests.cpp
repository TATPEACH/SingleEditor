#include "App/WindowsWindowShell.h"
#include "App/WindowPlacementPolicy.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "WindowChromeTests failed: " << message << '\n';
    std::exit(1);
}

class TestWindow final : public juce::DocumentWindow
{
public:
    TestWindow() : DocumentWindow("Single Editor - a long audio file name.wav",
        RendererVisualStyle::windowBackground(), allButtons, false)
    {
        setResizable(true, false);
        WindowsWindowShell().configureWindow(*this);
        setContentNonOwned(&content, false);
    }
    ~TestWindow() override { clearContentComponent(); setLookAndFeel(nullptr); }
    void closeButtonPressed() override { ++closes; }
    void minimiseButtonPressed() override { ++minimises; }
    void maximiseButtonPressed() override { ++maximises; }
    WindowControlKind findControlAtPoint(juce::Point<float> point) const override
    {
        return WindowsWindowShell::findControlAtPoint(*this, point);
    }
    bool hitTest(int x, int y) override
    {
        ++ancestorHitTests;
        return juce::DocumentWindow::hitTest(x, y);
    }
    juce::Component content;
    int ancestorHitTests = 0;
    int closes = 0, minimises = 0, maximises = 0;
};
}

void runWindowChromeTests()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::modern);
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        const auto initial = WindowChromeLookAndFeel::palette(false);
        check(initial.background == RendererVisualStyle::windowBackground()
                  && initial.text == RendererVisualStyle::primaryText(),
              "the lightweight first peer must match the saved Modern appearance before full skin attachment");
    }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    {
        const juce::Rectangle<int> primary(0, 0, 1920, 1040);
        const juce::Rectangle<int> secondary(-2560, 0, 2560, 1400);
        const juce::Rectangle<int> saved(-2119, 285, 1164, 721);
        check(windowPlacement::hasReachableCaption(saved, secondary)
                  && !windowPlacement::hasReachableCaption(saved, primary),
              "negative monitor coordinates remain valid only while that monitor is connected");
        const auto recovered = windowPlacement::fitInWorkArea(saved, primary);
        check(recovered == juce::Rectangle<int>(0, 285, 1164, 721)
                  && windowPlacement::hasReachableCaption(recovered, primary),
              "a removed monitor recovers the title and preserves the fitting window size");
        check(windowPlacement::hasReachableCaption({ -500, 20, 1164, 721 }, primary)
                  && windowPlacement::hasReachableCaption({ 300, 700, 1164, 721 }, primary),
              "deliberate partial off-screen positions with reachable titles need no correction");
        check(!windowPlacement::hasReachableCaption({ 1900, 20, 1164, 721 }, primary)
                  && !windowPlacement::hasReachableCaption({ 20, -10, 1164, 721 }, primary)
                  && !windowPlacement::hasReachableCaption({ 20, 1030, 1164, 721 }, primary),
              "a sliver or vertically clipped caption is not a usable recovery grip");
        check(windowPlacement::fitInWorkArea({ 5000, 5000, 2560, 1440 }, primary) == primary,
              "recovery fits an oversized rectangle to the current work area");
        check(windowPlacement::fitInWorkArea({ 5000, 5000, 800, 600 }, secondary)
                  == juce::Rectangle<int>(-800, 800, 800, 600),
              "recovery respects negative work-area origins");
        const juce::Rectangle<int> above(0, -1200, 1600, 1160);
        check(windowPlacement::fitInWorkArea({ 50, -2000, 1000, 700 }, above)
                  == juce::Rectangle<int>(50, -1200, 1000, 700),
              "a monitor above primary retains its negative vertical coordinate");
        check(!windowPlacement::hasReachableCaption(saved, {})
                  && windowPlacement::fitInWorkArea(saved, {}) == saved,
              "an unavailable display list does not invent screen coordinates");
    }
    WindowChromeLookAndFeel startup;
    ModernAppleLookAndFeel modern;
    {
        juce::TopLevelWindow basePhase("Opaque startup root", false);
        check(startup.createDropShadowerForComponent(basePhase) == nullptr
                  && modern.createDropShadowerForComponent(basePhase) == nullptr,
              "base construction must not allocate a software shadow / virtual-desktop watcher");
    }
    const auto output = juce::File::getCurrentWorkingDirectory()
        .getChildFile("out/modern-glass-window-chrome");
    check(output.createDirectory().wasOk(), "create screenshot output");
    {
        TestWindow window;
        check(!window.isUsingNativeTitleBar() && window.getTitleBarHeight() == 32,
              "Modern uses the same custom caption from the first peer");
        check(window.isDropShadowEnabled()
                  && (window.getDesktopWindowStyleFlags() & juce::ComponentPeer::windowHasDropShadow) != 0
                  && startup.createDropShadowerForComponent(window) == nullptr
                  && modern.createDropShadowerForComponent(window) == nullptr,
              "retain DWM window style without software shadow helper peers");
        for (int phase = 0; phase != 4; ++phase)
        {
            RendererVisualStyle::setModernTheme(phase % 2 == 1 ? ModernTheme::night : ModernTheme::light);
            window.setColour(juce::ResizableWindow::backgroundColourId, RendererVisualStyle::windowBackground());
            window.setLookAndFeel(phase < 2 ? &startup : &modern);
            window.sendLookAndFeelChange();
            for (int width : { 240, 800, 1280 })
            {
                window.setSize(width, 160);
                bool foundResizeBorder = false;
                for (auto* child : window.getChildren())
                    if (auto* border = dynamic_cast<juce::ResizableBorderComponent*>(child))
                    {
                        foundResizeBorder = true;
                        check(!border->getBorderThickness().isEmpty() && child->hitTest(0, 0),
                              "invisible resize frame retains its edge hit area");
                        const auto frame = border->createComponentSnapshot(border->getLocalBounds());
                        for (int y = 0; y < frame.getHeight(); ++y)
                            for (int x = 0; x < frame.getWidth(); ++x)
                                check(frame.getPixelAt(x, y).getAlpha() == 0,
                                      "Modern resize child must not add JUCE's default black outline");
                    }
                check(foundResizeBorder, "exercise the actual window resize child");
                auto* min = window.getMinimiseButton();
                auto* max = window.getMaximiseButton();
                auto* close = window.getCloseButton();
                check(min && max && close, "all native window controls remain available");
                for (auto* button : { min, max, close })
                {
                    check(button->isVisible() && button->getWidth() == 36
                              && window.getTitleBarArea().contains(button->getBounds()),
                          "caption controls remain usable and inside the title at every size");
                    check(!button->getBounds().intersects(window.content.getBounds()),
                          "caption controls never overlap audio content");
                }
                check(min->getRight() <= max->getX() && max->getRight() <= close->getX(),
                      "window controls maintain their familiar right-side order");
                check(min->getX() > 100, "small floating windows retain a draggable title span");
                check(window.findControlAtPoint({ 80.0f, 18.0f }) == juce::Component::WindowControlKind::caption,
                      "blank title area is a native caption hit target");
                for (const auto& [button, kind] : {
                         std::pair { min, juce::Component::WindowControlKind::minimise },
                         std::pair { max, juce::Component::WindowControlKind::maximise },
                         std::pair { close, juce::Component::WindowControlKind::close } })
                {
                    window.ancestorHitTests = 0;
                    check(window.findControlAtPoint(button->getBounds().getCentre().toFloat()) == kind,
                          "custom title button geometry returns the native command kind");
                    check(window.ancestorHitTests == 0,
                          "nonclient title hit testing must not query ancestors and re-enter the native peer");
                }
                for (const auto point : { juce::Point<float> { 1.0f, 1.0f },
                                          juce::Point<float> { width / 2.0f, 1.0f },
                                          juce::Point<float> { width - 1.0f, 1.0f },
                                          juce::Point<float> { 1.0f, 80.0f },
                                          juce::Point<float> { width - 1.0f, 159.0f },
                                          juce::Point<float> { 80.0f, 80.0f } })
                    check(window.findControlAtPoint(point)
                              == window.juce::DocumentWindow::findControlAtPoint(point),
                          "caption repair preserves the existing resize corners, edges and content hit routes");
                for (float scale : { 1.0f, 1.5f })
                {
                    const auto image = window.createComponentSnapshot(window.getLocalBounds(), true, scale);
                    check(image.isValid() && image.getWidth() == juce::roundToInt(width * scale),
                          "custom frame renders at both DPI scales");
                    auto stream = output.getChildFile(juce::String(phase) + "-" + juce::String(width)
                        + "-" + juce::String(scale, 1) + ".png").createOutputStream();
                    check(stream && stream->setPosition(0) && juce::PNGImageFormat().writeImageToStream(image, *stream)
                              && stream->truncate().wasOk(), "save titlebar rendering evidence");
                }
            }
            const auto contentBeforeSimple = window.content.getBounds();
            auto* peerBeforeSimple = window.getPeer();
            auto* closeBeforeSimple = window.getCloseButton();
            WindowsWindowShell::setSimplePresentation(window, true);
            check(window.getTitleBarHeight() == 0 && !window.getCloseButton()->isVisible()
                      && window.content.getHeight() > contentBeforeSimple.getHeight()
                      && window.getPeer() == peerBeforeSimple && window.getCloseButton() == closeBeforeSimple,
                  "Simple expands content without recreating the window peer or caption controls");
            WindowsWindowShell::setSimplePresentation(window, false);
            check(window.content.getBounds() == contentBeforeSimple && window.getCloseButton()->isVisible()
                      && window.getPeer() == peerBeforeSimple && window.getCloseButton() == closeBeforeSimple,
                  "Back restores chrome geometry while retaining peer identity");
            // These are JUCE's real native non-client control routes, not direct
            // calls to the overrides. The on-screen check exercises actual hits.
            window.windowControlClickedMinimise();
            window.windowControlClickedMaximise();
            window.windowControlClickedClose();
            check(window.minimises == phase + 1 && window.maximises == phase + 1
                      && window.closes == phase + 1, "native control routes reach existing window policy");
        }
    }
    RendererSelection::resetForTests(RendererKind::classic);
    check(WindowChromeLookAndFeel::palette(false).background == juce::Colour(0xff0d1117),
          "Classic keeps its independent neutral startup appearance");
    { TestWindow window; check(window.isUsingNativeTitleBar(), "Classic retains its native frame"); }
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "WindowChromeTests passed (24 renders, startup/full Light/Night, native command routes)\n";
}
