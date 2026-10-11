#include <juce_gui_basics/juce_gui_basics.h>
#include "App/WindowsWindowShell.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <iostream>
#include <set>

namespace
{
class MenuProbeLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool separator, bool active, bool highlighted, bool ticked, bool subMenu,
                           const juce::String& text, const juce::String& shortcut,
                           const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        if (paintedItems.insert(text).second)
            std::cout << "menu painted: " << text << std::endl;
        LookAndFeel_V4::drawPopupMenuItem(g, area, separator, active, highlighted, ticked,
                                        subMenu, text, shortcut, icon, textColour);
    }
    std::set<juce::String> paintedItems;
};

class InputSurface final : public juce::Component, private juce::Timer
{
public:
    InputSurface()
    {
        setWantsKeyboardFocus(true);
        addAndMakeVisible(instructions);
        instructions.setText("Native JUCE input probe. Click here, then press a key or drag.\n"
                             "This diagnostic does not open audio or change SingleEditor settings.",
                             juce::dontSendNotification);
        instructions.setInterceptsMouseClicks(false, false);
        instructions.setColour(juce::Label::textColourId, juce::Colours::black);
        addAndMakeVisible(events);
        events.setInterceptsMouseClicks(false, false);
        events.setColour(juce::Label::textColourId, juce::Colours::black);
        events.setJustificationType(juce::Justification::topLeft);
        events.setText("Waiting for input", juce::dontSendNotification);
        addAndMakeVisible(menuButton);
        menuButton.setButtonText("Open standard JUCE submenu probe");
        addAndMakeVisible(modernMenuButton);
        modernMenuButton.setButtonText("Open product-look submenu probe");
        addAndMakeVisible(titleButton);
        titleButton.setButtonText("Toggle modified caption");
        titleButton.onClick = [this]
        {
            auto* window = getTopLevelComponent();
            const auto name = window->getName();
            window->setName(name.startsWith("* ") ? name.substring(2) : "* " + name);
            record("requested caption=" + window->getName());
        };
        auto showMenu = [this] (bool modern)
        {
            juce::PopupMenu menu, child;
            child.addItem(1, "First child action");
            child.addItem(2, "Second child action");
            menu.addSubMenu("View probe", child);
            menu.addItem(3, "Direct action");
            menu.setLookAndFeel(modern ? static_cast<juce::LookAndFeel*>(&modernMenuLookAndFeel) : &menuLookAndFeel);
            menuLookAndFeel.paintedItems.clear();
            pointerSamples = 0;
            startTimer(250);
            record(modern ? "product-look menu opening" : "standard menu opening");
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(modern ? &modernMenuButton : &menuButton),
                [safe = juce::Component::SafePointer<InputSurface>(this)] (int result)
                {
                    if (safe != nullptr) safe->record("menu result=" + juce::String(result));
                });
        };
        menuButton.onClick = [showMenu] { showMenu(false); };
        modernMenuButton.onClick = [showMenu] { showMenu(true); };
        juce::Desktop::getInstance().addGlobalMouseListener(this);
    }

    ~InputSurface() override
    {
        stopTimer();
        juce::Desktop::getInstance().removeGlobalMouseListener(this);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::whitesmoke); }
    void resized() override
    {
        auto area = getLocalBounds().reduced(20);
        instructions.setBounds(area.removeFromTop(70));
        auto menuRow = area.removeFromTop(32);
        menuButton.setBounds(menuRow.removeFromLeft(320));
        menuRow.removeFromLeft(12);
        modernMenuButton.setBounds(menuRow.removeFromLeft(320));
        area.removeFromTop(12);
        titleButton.setBounds(area.removeFromTop(32).removeFromLeft(320));
        area.removeFromTop(12);
        events.setBounds(area);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto modifiers = key.getModifiers();
        record("key=" + juce::String(key.getKeyCode())
               + " text=" + juce::String(static_cast<int>(key.getTextCharacter()))
               + " flags=" + juce::String(modifiers.getRawFlags())
               + " ctrl=" + juce::String(modifiers.isCtrlDown() ? 1 : 0)
               + " shift=" + juce::String(modifiers.isShiftDown() ? 1 : 0)
               + " alt=" + juce::String(modifiers.isAltDown() ? 1 : 0)
               + " (" + key.getTextDescription() + ")");
        return true;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.eventComponent == this) grabKeyboardFocus();
        record("mouseDown event=" + event.getScreenPosition().toString()
               + " source=" + event.source.getScreenPosition().toString()
               + " desktop=" + juce::Desktop::getMousePosition().toString());
    }
    void mouseUp(const juce::MouseEvent& event) override
    {
        record("mouseUp delta=" + event.getOffsetFromDragStart().toString());
    }

    void record(const juce::String& entry)
    {
        const auto line = juce::String(++sequence) + ": " + entry;
        history.add(line);
        if (history.size() > 12)
            history.remove(0);
        events.setText(history.joinIntoString("\n"), juce::dontSendNotification);
        std::cout << line << std::endl;
    }

private:
    void timerCallback() override
    {
        record("pointer poll=" + juce::String(++pointerSamples)
               + " source=" + juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().toString()
               + " desktop=" + juce::Desktop::getMousePosition().toString());
        if (pointerSamples >= 8) stopTimer();
    }
    MenuProbeLookAndFeel menuLookAndFeel;
    singleeditor::ModernAppleLookAndFeel modernMenuLookAndFeel;
    juce::TextButton menuButton, modernMenuButton, titleButton;
    juce::Label instructions, events;
    juce::StringArray history;
    int sequence = 0;
    int pointerSamples = 0;
};

class InputWindow final : public juce::DocumentWindow
{
public:
    InputWindow() : DocumentWindow("SingleEditor native input probe", juce::Colours::whitesmoke,
                                   juce::DocumentWindow::allButtons, false)
    {
        singleeditor::WindowsWindowShell().configureWindow(*this);
        setContentOwned(new InputSurface(), false);
        setResizable(true, false);
        centreWithSize(802, 500);
        const auto requestedBounds = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_INPUT_PROBE_BOUNDS", {});
        if (requestedBounds.isNotEmpty())
        {
            const auto bounds = juce::Rectangle<int>::fromString(requestedBounds);
            if (bounds.getWidth() >= 400 && bounds.getHeight() >= 300)
                setBounds(bounds);
        }
        addToDesktop(getDesktopWindowStyleFlags());
        setVisible(true);
    }

    void closeButtonPressed() override { juce::MessageManager::getInstance()->stopDispatchLoop(); }
    void paint(juce::Graphics& g) override
    {
        if (lastPaintedName != getName())
        {
            lastPaintedName = getName();
            std::cout << "paint caption=" << lastPaintedName
                      << " clip=" << g.getClipBounds().toString() << std::endl;
        }
        DocumentWindow::paint(g);
    }
    void moved() override
    {
        DocumentWindow::moved();
        if (auto* content = dynamic_cast<InputSurface*>(getContentComponent()))
            content->record("window bounds=" + getBounds().toString());
    }
private:
    juce::String lastPaintedName;
};
}

void runNativeInputProbe()
{
    singleeditor::RendererSelection::resetForTests(singleeditor::RendererKind::modern);
    singleeditor::RendererVisualStyle::setModernTheme(singleeditor::ModernTheme::light);
    singleeditor::ModernAppleLookAndFeel::setSkinMode(singleeditor::ModernAppleLookAndFeel::SkinMode::full);
    singleeditor::WindowChromeLookAndFeel chrome(true);
    juce::LookAndFeel::setDefaultLookAndFeel(&chrome);
    InputWindow window;
    juce::MessageManager::getInstance()->runDispatchLoop();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}
