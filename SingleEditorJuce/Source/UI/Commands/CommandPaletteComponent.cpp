#include "UI/Commands/CommandPaletteComponent.h"

namespace singleeditor
{
CommandPaletteComponent::CommandPaletteComponent(const GuiCommandRegistry& registry,
                                                 GuiCommandDispatchContext context)
{
    setComponentID("command-palette");
    setTitle("Command Palette");
    setDescription("Search and run available Single Editor commands.");
    setFocusContainerType(juce::Component::FocusContainerType::keyboardFocusContainer);
    setWantsKeyboardFocus(true);

    if (!RendererVisualStyle::isClassic())
    {
        materialSurface = std::make_unique<ModernGlassSurface>(16.0f);
        materialSurface->setComponentID("command-palette.material");
        materialSurface->getProperties().set("glassChrome", true);
        materialSurface->getProperties().set("skinRole", "popup");
        materialSurface->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*materialSurface);
        materialSurface->setInteractionHost(this);
        addAndMakeVisible(heading);
        heading.setText("Commands", juce::dontSendNotification);
        heading.setComponentID("command-palette.heading");
        heading.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        heading.setBorderSize(juce::BorderSize<int>(0));
        addAndMakeVisible(selectionStatus);
        selectionStatus.setComponentID("command-palette.status");
        selectionStatus.setFont(juce::Font(juce::FontOptions(11.5f)));
        selectionStatus.setBorderSize(juce::BorderSize<int>(0));
    }

    model.open(registry, context);
    addAndMakeVisible(queryEditor);
    queryEditor.setComponentID("command-palette.search");
    queryEditor.setTitle("Search commands");
    queryEditor.setTextToShowWhenEmpty("Type a command name", juce::Colours::grey);
    queryEditor.onTextChange = [this] { refreshMatches(); };
    queryEditor.onNavigationKey = [this](const juce::KeyPress& key) { return navigateSearch(key); };
    queryEditor.onReturnKey = [this] { invokeSelected(); };
    queryEditor.onEscapeKey = [this]
    {
        if (const auto dismiss = onDismiss) dismiss();
    };

    addAndMakeVisible(resultsList);
    resultsList.setComponentID("command-palette.results");
    resultsList.setTitle("Available commands");
    resultsList.setModel(this);
    resultsList.setRowHeight(materialSurface != nullptr ? 36 : 30);
    if (materialSurface != nullptr)
    {
        resultsList.setOpaque(false);
        resultsList.getViewport()->setOpaque(false);
        resultsList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        resultsList.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        runButton.getProperties().set("accent", true);
    }

    addAndMakeVisible(runButton);
    runButton.setComponentID("command-palette.run");
    runButton.setTitle("Run selected command");
    runButton.onClick = [this] { invokeSelected(); };

    addAndMakeVisible(closeButton);
    closeButton.setComponentID("command-palette.close");
    closeButton.setTitle("Close Command Palette");
    closeButton.onClick = [this]
    {
        if (const auto dismiss = onDismiss) dismiss();
    };

    lookAndFeelChanged();
    refreshMatches();
}

CommandPaletteComponent::~CommandPaletteComponent()
{
    resultsList.setModel(nullptr);
    model.close();
}

void CommandPaletteComponent::paint(juce::Graphics& g)
{
    if (materialSurface != nullptr) return;
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour::fromRGB(246, 247, 250));
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(juce::Colour::fromRGB(116, 122, 135));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 10.0f, 1.0f);
}

void CommandPaletteComponent::resized()
{
    if (materialSurface != nullptr)
    {
        materialSurface->setBounds(getLocalBounds());
        auto area = getLocalBounds().reduced(16);
        auto title = area.removeFromTop(28);
        closeButton.setBounds(title.removeFromRight(64));
        title.removeFromRight(12);
        heading.setBounds(title);
        area.removeFromTop(10);
        queryEditor.setBounds(area.removeFromTop(36));
        area.removeFromTop(10);
        auto footer = area.removeFromBottom(32);
        runButton.setBounds(footer.removeFromRight(84));
        footer.removeFromRight(12);
        selectionStatus.setBounds(footer);
        area.removeFromBottom(10);
        resultsList.setBounds(area);
        return;
    }
    auto bounds = getLocalBounds().reduced(14);
    queryEditor.setBounds(bounds.removeFromTop(34));
    bounds.removeFromTop(10);
    auto actions = bounds.removeFromBottom(30);
    closeButton.setBounds(actions.removeFromRight(84));
    actions.removeFromRight(8);
    runButton.setBounds(actions.removeFromRight(84));
    bounds.removeFromBottom(10);
    resultsList.setBounds(bounds);
}

void CommandPaletteComponent::paintOverChildren(juce::Graphics& g)
{
    if (materialSurface == nullptr || !matches.empty()) return;
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText("No matching commands", resultsList.getBounds().reduced(10), juce::Justification::centred, true);
}

void CommandPaletteComponent::lookAndFeelChanged()
{
    if (materialSurface == nullptr) return;
    heading.setColour(juce::Label::textColourId, RendererVisualStyle::primaryText());
    selectionStatus.setColour(juce::Label::textColourId, RendererVisualStyle::secondaryText());
    queryEditor.setColour(juce::TextEditor::textColourId, RendererVisualStyle::primaryText());
    queryEditor.setColour(juce::TextEditor::highlightColourId, RendererVisualStyle::accentBlue().withAlpha(0.24f));
    queryEditor.setTextToShowWhenEmpty("Search commands", RendererVisualStyle::secondaryText());
    queryEditor.setFont(juce::Font(juce::FontOptions(14.0f)));
    resultsList.repaint();
}

void CommandPaletteComponent::setBackdropSource(juce::Component* source, bool effectsAllowed, juce::Component* overlay)
{
    backdropAllowed = effectsAllowed;
    if (materialSurface == nullptr) return;
    materialSurface->setBackdropSource(source);
    materialSurface->setBackdropOverlay(overlay);
    materialSurface->setEffectsEnabled(backdropAllowed && isVisible());
}

void CommandPaletteComponent::visibilityChanged()
{
    if (materialSurface != nullptr)
        materialSurface->setEffectsEnabled(backdropAllowed && isVisible());
}

void CommandPaletteComponent::invalidateBackdrop()
{
    if (materialSurface != nullptr) materialSurface->invalidateBackdrop();
}

void CommandPaletteComponent::focusSearch()
{
    queryEditor.grabKeyboardFocus();
}

int CommandPaletteComponent::getNumRows()
{
    return static_cast<int>(matches.size());
}

void CommandPaletteComponent::paintListBoxItem(int rowNumber,
                                               juce::Graphics& g,
                                               int width,
                                               int height,
                                               bool rowIsSelected)
{
    if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(matches.size())))
        return;
    const auto& match = matches[static_cast<std::size_t>(rowNumber)];
    if (materialSurface != nullptr)
    {
        auto row = juce::Rectangle<int>(0, 0, width, height).reduced(3, 1);
        if (rowIsSelected)
        {
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(RendererVisualStyle::isNight() ? 0.28f : 0.22f));
            g.fillRoundedRectangle(row.toFloat(), 7.0f);
            // A blue waveform can sit directly behind these rows. Retain a
            // crisp edge and leading marker instead of relying on tint alone.
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.75f));
            g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 7.0f, 1.0f);
            g.fillRoundedRectangle(static_cast<float>(row.getX() + 3),
                static_cast<float>(row.getY() + 7), 2.0f, static_cast<float>(row.getHeight() - 14), 1.0f);
        }
        row.reduce(10, 0);
        const auto shortcut = juce::String::fromUTF8(match.command.descriptor.shortcut.c_str());
        if (shortcut.isNotEmpty())
        {
            const auto font = juce::Font(juce::FontOptions(11.5f));
            const auto keyWidth = juce::jmin(110, juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, shortcut)) + 14);
            const auto key = row.removeFromRight(keyWidth).withSizeKeepingCentre(keyWidth, 22);
            g.setColour(RendererVisualStyle::primaryText().withAlpha(RendererVisualStyle::isNight() ? 0.06f : 0.035f));
            g.fillRoundedRectangle(key.toFloat(), 4.0f);
            g.setColour(RendererVisualStyle::borderColour());
            g.drawRoundedRectangle(key.toFloat(), 4.0f, 0.6f);
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(font);
            g.drawText(shortcut, key, juce::Justification::centred, true);
            row.removeFromRight(10);
        }
        g.setColour(match.command.state.enabled ? RendererVisualStyle::primaryText() : RendererVisualStyle::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(13.5f)));
        g.drawText(juce::String::fromUTF8(match.command.state.label.c_str()), row, juce::Justification::centredLeft, true);
        return;
    }
    if (rowIsSelected)
    {
        g.setColour(juce::Colour::fromRGB(220, 230, 248));
        g.fillRect(0, 0, width, height);
    }
    g.setColour(match.command.state.enabled ? juce::Colour::fromRGB(35, 40, 50)
                                            : juce::Colour::fromRGB(130, 135, 145));
    g.setFont(juce::FontOptions(13.0f));
    auto row = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);
    const auto shortcut = juce::String::fromUTF8(match.command.descriptor.shortcut.c_str());
    if (shortcut.isNotEmpty())
        g.drawText(shortcut, row.removeFromRight(100), juce::Justification::centredRight, true);
    g.drawText(juce::String::fromUTF8(match.command.state.label.c_str()),
               row,
               juce::Justification::centredLeft,
               true);
}

void CommandPaletteComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    resultsList.selectRow(row);
    invokeSelected();
}

void CommandPaletteComponent::returnKeyPressed(int row)
{
    resultsList.selectRow(row);
    invokeSelected();
}

juce::String CommandPaletteComponent::getNameForRow(int row)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(matches.size())))
        return {};
    const auto& command = matches[static_cast<std::size_t>(row)].command;
    auto name = juce::String::fromUTF8(command.state.label.c_str());
    if (!command.state.enabled && !command.state.disabledReason.empty())
        name += ", unavailable: " + juce::String::fromUTF8(command.state.disabledReason.c_str());
    return name;
}

void CommandPaletteComponent::refreshMatches()
{
    matches = model.search(queryEditor.getText().toStdString());
    resultsList.updateContent();
    if (!matches.empty())
        resultsList.selectRow(0, false, false);
    else
        resultsList.deselectAllRows();
    updateSelectedState();
    repaint();
}

void CommandPaletteComponent::selectedRowsChanged(int) { updateSelectedState(); }

void CommandPaletteComponent::updateSelectedState()
{
    const auto selected = resultsList.getSelectedRow();
    const auto valid = juce::isPositiveAndBelow(selected, static_cast<int>(matches.size()));
    runButton.setEnabled(valid && matches[static_cast<std::size_t>(selected)].command.state.enabled);
    auto status = juce::String(static_cast<int>(matches.size())) + " commands";
    if (valid && !matches[static_cast<std::size_t>(selected)].command.state.enabled)
        status = juce::String::fromUTF8(matches[static_cast<std::size_t>(selected)].command.state.disabledReason.c_str());
    selectionStatus.setText(status, juce::dontSendNotification);
    selectionStatus.setTooltip(status);
}

bool CommandPaletteComponent::navigateSearch(const juce::KeyPress& key)
{
    if (key.getKeyCode() != juce::KeyPress::upKey && key.getKeyCode() != juce::KeyPress::downKey) return false;
    if (matches.empty()) return true;
    const auto selected = resultsList.getSelectedRow();
    const auto next = selected < 0 ? 0 : selected + (key.getKeyCode() == juce::KeyPress::upKey ? -1 : 1);
    const auto row = juce::jlimit(0, static_cast<int>(matches.size()) - 1, next);
    resultsList.selectRow(row);
    resultsList.scrollToEnsureRowIsOnscreen(row);
    return true;
}

void CommandPaletteComponent::invokeSelected()
{
    const auto row = resultsList.getSelectedRow();
    if (!juce::isPositiveAndBelow(row, static_cast<int>(matches.size())))
        return;
    const auto& command = matches[static_cast<std::size_t>(row)].command;
    if (!command.state.enabled)
        return;
    const auto chosen = onCommandChosen;
    const auto id = command.descriptor.id;
    if (chosen) chosen(id);
}
} // namespace singleeditor
