#include "UI/Components/TopBarComponent.h"

#include "App/MainWindow.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

namespace singleeditor
{
namespace
{
void configureMenuLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    ModernAppleLookAndFeel::setLabelRole(label, ModernAppleLookAndFeel::LabelRole::secondary);
    label.setFont(juce::Font(juce::FontOptions(13.0f)));
    label.setInterceptsMouseClicks(false, false);
    label.setJustificationType(juce::Justification::centredLeft);
}
}

TopBarComponent::WindowControlButton::WindowControlButton(Type typeToUse)
    : juce::Button({}), type(typeToUse)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);

    switch (type)
    {
        case Type::minimise: setTooltip("Minimise"); break;
        case Type::maximise: setTooltip("Toggle Full Screen"); break;
        case Type::close: setTooltip("Close"); break;
    }
}

void TopBarComponent::WindowControlButton::paintButton(juce::Graphics& g, bool hovered, bool pressed)
{
    auto bounds = getLocalBounds().toFloat();

    if (hovered || pressed)
    {
        auto fill = type == Type::close
                        ? juce::Colour(0x18f14444)
                        : (pressed ? ModernAppleLookAndFeel::ghostPressedFill()
                                   : ModernAppleLookAndFeel::ghostHoverFill());
        g.setColour(fill);
        g.fillRoundedRectangle(bounds.reduced(0.5f), 6.0f);
    }

    g.setColour(type == Type::close ? juce::Colour(0xfff14444) : ModernAppleLookAndFeel::secondaryText().withAlpha(isEnabled() ? 0.85f : 0.35f));

    juce::Path symbol;
    const auto reduced = bounds.reduced(10.0f, 10.0f);

    switch (type)
    {
        case Type::minimise:
            symbol.startNewSubPath(reduced.getX(), reduced.getCentreY());
            symbol.lineTo(reduced.getRight(), reduced.getCentreY());
            break;
        case Type::maximise:
            symbol.addRectangle(reduced.withSizeKeepingCentre(reduced.getWidth(), reduced.getHeight() - 1.5f));
            break;
        case Type::close:
            symbol.startNewSubPath(reduced.getX(), reduced.getY());
            symbol.lineTo(reduced.getRight(), reduced.getBottom());
            symbol.startNewSubPath(reduced.getRight(), reduced.getY());
            symbol.lineTo(reduced.getX(), reduced.getBottom());
            break;
    }

    g.strokePath(symbol, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

TopBarComponent::TopBarComponent(juce::ResizableWindow& hostWindowToDrag)
    : hostWindow(hostWindowToDrag),
      minimiseButton(WindowControlButton::Type::minimise),
      maximiseButton(WindowControlButton::Type::maximise),
      closeButton(WindowControlButton::Type::close)
{
    addAndMakeVisible(fileMenuLabel);
    addAndMakeVisible(editMenuLabel);
    addAndMakeVisible(viewMenuLabel);
    addAndMakeVisible(windowMenuLabel);
    addAndMakeVisible(helpMenuLabel);
    configureMenuLabel(fileMenuLabel, "File");
    configureMenuLabel(editMenuLabel, "Edit");
    configureMenuLabel(viewMenuLabel, "View");
    configureMenuLabel(windowMenuLabel, "Window");
    configureMenuLabel(helpMenuLabel, "Help");

    addAndMakeVisible(minimiseButton);
    addAndMakeVisible(maximiseButton);
    addAndMakeVisible(closeButton);

    minimiseButton.onClick = [this]() { hostWindow.setMinimised(true); };
    maximiseButton.onClick = [this]() { hostWindow.setFullScreen(!hostWindow.isFullScreen()); };
    closeButton.onClick = [this]()
    {
        if (auto* mainWindow = dynamic_cast<MainWindow*>(&hostWindow))
            mainWindow->requestUserCloseAction();
        else if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    };
}

void TopBarComponent::setSubtitle(const juce::String&)
{
}

void TopBarComponent::setTitle(const juce::String& newTitle)
{
    currentTitle = newTitle;
    repaint();
}

void TopBarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(ModernAppleLookAndFeel::outerPadding(), 6);

    juce::FlexBox row;
    row.flexDirection = juce::FlexBox::Direction::row;
    row.alignItems = juce::FlexBox::AlignItems::center;

    row.items.add(juce::FlexItem().withWidth(30.0f).withHeight(24.0f));
    row.items.add(juce::FlexItem(fileMenuLabel).withWidth(42.0f).withHeight(22.0f).withMargin({ 0.0f, 0.0f, 0.0f, 12.0f }));
    row.items.add(juce::FlexItem(editMenuLabel).withWidth(42.0f).withHeight(22.0f).withMargin({ 0.0f, 0.0f, 0.0f, 12.0f }));
    row.items.add(juce::FlexItem(viewMenuLabel).withWidth(46.0f).withHeight(22.0f).withMargin({ 0.0f, 0.0f, 0.0f, 12.0f }));
    row.items.add(juce::FlexItem(windowMenuLabel).withWidth(68.0f).withHeight(22.0f).withMargin({ 0.0f, 0.0f, 0.0f, 12.0f }));
    row.items.add(juce::FlexItem(helpMenuLabel).withWidth(40.0f).withHeight(22.0f).withMargin({ 0.0f, 0.0f, 0.0f, 12.0f }));
    row.items.add(juce::FlexItem().withFlex(1.0f));
    row.items.add(juce::FlexItem(minimiseButton).withWidth(30.0f).withHeight(24.0f));
    row.items.add(juce::FlexItem(maximiseButton).withWidth(30.0f).withHeight(24.0f));
    row.items.add(juce::FlexItem(closeButton).withWidth(30.0f).withHeight(24.0f));
    row.performLayout(bounds.toFloat());
}

void TopBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xfff5f7fa));
}

void TopBarComponent::mouseDown(const juce::MouseEvent& event)
{
    dragger.startDraggingComponent(&hostWindow, event);
}

void TopBarComponent::mouseDrag(const juce::MouseEvent& event)
{
    dragger.dragComponent(&hostWindow, event, hostWindow.getConstrainer());
}
} // namespace singleeditor
