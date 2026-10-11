#include "UI/Components/CustomTitleBarComponent.h"

#include "App/MainWindow.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

namespace singleeditor
{
CustomTitleBarComponent::MenuButton::MenuButton(const juce::String& textToUse, MenuKind kindToUse)
    : juce::Button(textToUse), kind(kindToUse)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void CustomTitleBarComponent::MenuButton::paintButton(juce::Graphics& g, bool hovered, bool down)
{
    auto bounds = getLocalBounds().toFloat();

    if (hovered || down)
    {
        g.setColour(down ? ModernAppleLookAndFeel::ghostPressedFill()
                         : ModernAppleLookAndFeel::ghostHoverFill());
        g.fillRoundedRectangle(bounds, ModernAppleLookAndFeel::componentRadius());
    }

    g.setColour(ModernAppleLookAndFeel::primaryText().withAlpha(isEnabled() ? 0.90f : 0.45f));
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::plain)));
    g.drawFittedText(getButtonText(), getLocalBounds(), juce::Justification::centred, 1);
}

CustomTitleBarComponent::WindowControlButton::WindowControlButton(Type typeToUse)
    : juce::Button({}), type(typeToUse)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void CustomTitleBarComponent::WindowControlButton::paintButton(juce::Graphics& g, bool hovered, bool down)
{
    auto bounds = getLocalBounds().toFloat();
    const auto isClose = type == Type::close;

    if (hovered || down)
    {
        const auto fill = isClose
                              ? (down ? juce::Colour(0xffc42b1c) : juce::Colour(0xffe81123))
                              : (down ? juce::Colour(0x14000000) : juce::Colour(0x0d000000));
        g.setColour(fill);
        g.fillRect(bounds);
    }

    g.setColour(isClose && hovered ? juce::Colours::white
                                   : ModernAppleLookAndFeel::primaryText().withAlpha(isEnabled() ? 0.92f : 0.35f));

    juce::Path symbol;
    auto iconBounds = bounds.withSizeKeepingCentre(9.5f, 9.5f);

    switch (type)
    {
        case Type::minimise:
            symbol.startNewSubPath(iconBounds.getX(), iconBounds.getCentreY() + 1.8f);
            symbol.lineTo(iconBounds.getRight(), iconBounds.getCentreY() + 1.8f);
            break;
        case Type::maximise:
            symbol.addRectangle(iconBounds.reduced(0.9f));
            break;
        case Type::close:
            symbol.startNewSubPath(iconBounds.getX(), iconBounds.getY());
            symbol.lineTo(iconBounds.getRight(), iconBounds.getBottom());
            symbol.startNewSubPath(iconBounds.getRight(), iconBounds.getY());
            symbol.lineTo(iconBounds.getX(), iconBounds.getBottom());
            break;
    }

    g.strokePath(symbol, juce::PathStrokeType(0.95f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void CustomTitleBarComponent::configureMenuButton(MenuButton& button, const juce::String& text)
{
    button.setButtonText(text);
    button.setTooltip(text);
}

CustomTitleBarComponent::CustomTitleBarComponent(juce::ResizableWindow& hostWindowToDrag)
    : hostWindow(hostWindowToDrag),
      fileMenuButton("File", MenuKind::file),
      editMenuButton("Edit", MenuKind::edit),
      viewMenuButton("View", MenuKind::view),
      windowMenuButton("Window", MenuKind::window),
      helpMenuButton("Help", MenuKind::help),
      minimiseButton(WindowControlButton::Type::minimise),
      maximiseButton(WindowControlButton::Type::maximise),
      closeButton(WindowControlButton::Type::close)
{
    addAndMakeVisible(fileMenuButton);
    addAndMakeVisible(editMenuButton);
    addAndMakeVisible(viewMenuButton);
    addAndMakeVisible(windowMenuButton);
    addAndMakeVisible(helpMenuButton);

    configureMenuButton(fileMenuButton, "File");
    configureMenuButton(editMenuButton, "Edit");
    configureMenuButton(viewMenuButton, "View");
    configureMenuButton(windowMenuButton, "Window");
    configureMenuButton(helpMenuButton, "Help");
    fileMenuButton.setComponentID("title-menu.file");
    editMenuButton.setComponentID("title-menu.edit");
    viewMenuButton.setComponentID("title-menu.view");
    windowMenuButton.setComponentID("title-menu.window");
    helpMenuButton.setComponentID("title-menu.help");
    fileMenuButton.setTitle("File menu");
    editMenuButton.setTitle("Edit menu");
    viewMenuButton.setTitle("View menu");
    windowMenuButton.setTitle("Window menu");
    helpMenuButton.setTitle("Help menu");

    auto wireMenu = [this](MenuButton& button)
    {
        button.onClick = [this, &button]
        {
            if (onMenuRequested)
                onMenuRequested(button.kind, button.getScreenBounds());
        };
    };

    wireMenu(fileMenuButton);
    wireMenu(editMenuButton);
    wireMenu(viewMenuButton);
    wireMenu(windowMenuButton);
    wireMenu(helpMenuButton);

    addAndMakeVisible(minimiseButton);
    addAndMakeVisible(maximiseButton);
    addAndMakeVisible(closeButton);
    minimiseButton.setComponentID("application.window.minimize");
    maximiseButton.setComponentID("application.window.toggle-maximize");
    closeButton.setComponentID("application.close");
    minimiseButton.setTitle("Minimize window");
    maximiseButton.setTitle("Maximize or restore window");
    closeButton.setTitle("Close window");

    minimiseButton.onClick = [this]()
    {
        if (onMinimiseRequested)
            onMinimiseRequested();
        else
            hostWindow.setMinimised(true);
    };
    maximiseButton.onClick = [this]()
    {
        if (onMaximiseRequested)
            onMaximiseRequested();
        else
            hostWindow.setFullScreen(!hostWindow.isFullScreen());
    };
    closeButton.onClick = [this]()
    {
        if (onCloseRequested)
        {
            onCloseRequested();
            return;
        }
        if (auto* mainWindow = dynamic_cast<MainWindow*>(&hostWindow))
            mainWindow->requestUserCloseAction();
        else if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    };
}

void CustomTitleBarComponent::setTitle(const juce::String& newTitle)
{
    currentTitle = newTitle;
}

void CustomTitleBarComponent::setSubtitle(const juce::String& newSubtitle)
{
    subtitle = newSubtitle;
}

void CustomTitleBarComponent::resized()
{
    auto bounds = getLocalBounds();
    auto rightBounds = bounds.removeFromRight(132);
    auto leftBounds = bounds.reduced(14, 0);
    leftBounds.setRight(rightBounds.getX() - 8);

    const auto menuHeight = 20;
    const auto menuFont = juce::Font(juce::FontOptions(14.0f, juce::Font::plain));
    const auto menuWidth = [&](const juce::String& text)
    {
        return static_cast<float>(menuFont.getStringWidth(text) + 20);
    };

    juce::FlexBox menuRow;
    menuRow.flexDirection = juce::FlexBox::Direction::row;
    menuRow.alignItems = juce::FlexBox::AlignItems::center;
    menuRow.justifyContent = juce::FlexBox::JustifyContent::flexStart;
    menuRow.items.add(juce::FlexItem(fileMenuButton).withWidth(menuWidth("File")).withHeight(static_cast<float>(menuHeight)).withMargin({ 0.0f, 0.0f, 0.0f, 2.0f }));
    menuRow.items.add(juce::FlexItem(editMenuButton).withWidth(menuWidth("Edit")).withHeight(static_cast<float>(menuHeight)).withMargin({ 0.0f, 0.0f, 0.0f, 2.0f }));
    menuRow.items.add(juce::FlexItem(viewMenuButton).withWidth(menuWidth("View")).withHeight(static_cast<float>(menuHeight)).withMargin({ 0.0f, 0.0f, 0.0f, 2.0f }));
    menuRow.items.add(juce::FlexItem(windowMenuButton).withWidth(menuWidth("Window")).withHeight(static_cast<float>(menuHeight)).withMargin({ 0.0f, 0.0f, 0.0f, 2.0f }));
    menuRow.items.add(juce::FlexItem(helpMenuButton).withWidth(menuWidth("Help")).withHeight(static_cast<float>(menuHeight)));
    menuRow.performLayout(leftBounds.toFloat());

    juce::FlexBox rightRow;
    rightRow.flexDirection = juce::FlexBox::Direction::row;
    rightRow.alignItems = juce::FlexBox::AlignItems::stretch;
    rightRow.justifyContent = juce::FlexBox::JustifyContent::flexEnd;
    rightRow.items.add(juce::FlexItem(minimiseButton).withWidth(44.0f).withHeight(static_cast<float>(getHeight())));
    rightRow.items.add(juce::FlexItem(maximiseButton).withWidth(44.0f).withHeight(static_cast<float>(getHeight())));
    rightRow.items.add(juce::FlexItem(closeButton).withWidth(44.0f).withHeight(static_cast<float>(getHeight())));
    rightRow.performLayout(rightBounds.toFloat());
}

void CustomTitleBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(ModernAppleLookAndFeel::titleBarBackground());
}

void CustomTitleBarComponent::mouseDown(const juce::MouseEvent& event)
{
    dragger.startDraggingComponent(&hostWindow, event);
}

void CustomTitleBarComponent::mouseDrag(const juce::MouseEvent& event)
{
    dragger.dragComponent(&hostWindow, event, hostWindow.getConstrainer());
}
} // namespace singleeditor
