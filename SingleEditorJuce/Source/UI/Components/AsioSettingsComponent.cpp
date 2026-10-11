#include "UI/Components/AsioSettingsComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include <algorithm>

namespace singleeditor
{
namespace
{
juce::String describeIssue(const DeviceRoutePlan::Issue& issue, const DeviceRoutePlan::Draft& draft)
{
    using Code = DeviceRoutePlan::IssueCode;
    const auto row = issue.speakerRow >= 0 && issue.speakerRow < static_cast<int>(draft.bus.speakers.size())
        ? juce::String(draft.bus.speakers[static_cast<size_t>(issue.speakerRow)].stableId) : juce::String{};
    const auto output = issue.physicalOutput >= 0 ? "output " + juce::String(issue.physicalOutput + 1) : juce::String{};
    switch (issue.code)
    {
        case Code::invalidDevice: return "Select between 1 and 32 active physical outputs.";
        case Code::staleDevice: return "Device capabilities changed; read them again.";
        case Code::invalidBus: return "Choose a canonical speaker layout.";
        case Code::staleGeometry: return "The current renderer geometry differs from this layout.";
        case Code::invalidMatrixShape: return "The matrix no longer matches the speaker layout.";
        case Code::inactiveOutput: return row + ": unavailable " + output + ".";
        case Code::duplicateConnection: return row + ": duplicate " + output + ".";
        case Code::requiredUnmapped: return row + ": required speaker has no physical output.";
        case Code::convergentOutput: return output + ": several speakers are summed at unity gain.";
        case Code::possibleClipping: return output + ": the summed signal may clip.";
        case Code::crossedRole: return row + ": connection differs from trusted device speaker semantics.";
    }
    return {};
}
}
AsioSettingsComponent::AsioSettingsComponent(std::shared_ptr<AsioSettingsSession> value) : session(std::move(value))
{
    setName("Audio output routing"); setComponentID("asio.settings");
    for (auto* label : {&title, &help, &status, &liveOutput})
    {
        addAndMakeVisible(*label); label->setJustificationType(juce::Justification::topLeft);
        label->setColour(juce::Label::textColourId, ModernAppleLookAndFeel::primaryText());
        label->setFont(juce::Font(juce::FontOptions(13.0f)));
    }
    title.setText("Audio output routing", juce::dontSendNotification);
    addAndMakeVisible(warnings); warnings.setReadOnly(true); warnings.setMultiLine(true);
    warnings.setScrollbarsShown(true); warnings.setComponentID("asio.warnings");
    title.setFont(juce::Font(juce::FontOptions(20.0f)));
    help.setText("Draft > Validate > Ping > Activate. Rows are canonical speakers; columns are physical outputs. Connections use unity gain. Editing does not change the active route.", juce::dontSendNotification);
    for (auto* combo : {&backends, &drivers, &profiles, &rates, &buffers, &pingOutput}) addAndMakeVisible(*combo);
    backends.setComponentID("audio-output.backend");
    int backendItem = 0;
    for (const auto* name : {"ASIO", "Windows Audio", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)"}) backends.addItem(name, ++backendItem);
    backends.setTooltip("Select a backend, then refresh its output list. Activation requires an explicit validated route.");
    backends.onChange = [this] { if (!updating) session->selectBackend(backends.getText().toStdString()); };
    drivers.setComponentID("asio.driver"); drivers.setTextWhenNothingSelected("Choose output device"); drivers.setTooltip("The selected device is bound by its stable identity, not its display name.");
    profiles.setComponentID("asio.profile"); profiles.setTextWhenNothingSelected("Choose canonical speaker layout"); profiles.setTooltip("Canonical renderer layout; does not infer physical wiring.");
    rates.setComponentID("asio.rate"); rates.setTextWhenNothingSelected("Choose sample rate"); rates.setTooltip("Exact device sample rate. No automatic fallback.");
    buffers.setComponentID("asio.buffer"); buffers.setTextWhenNothingSelected("Choose buffer size"); buffers.setTooltip("Exact device buffer size in samples.");
    pingOutput.setComponentID("asio.ping-output"); pingOutput.setTextWhenNothingSelected("Physical output to ping");
    for (auto* button : {&discover, &inspect, &cancel, &validate, &ping, &activate, &retrySaved, &windowsDefault, &controlPanel}) addAndMakeVisible(*button);
    windowsDefault.setComponentID("audio-output.windows-default");
    windowsDefault.setTooltip("Select the current Windows default endpoint in shared mode. Read capabilities and confirm its route before activating.");
    windowsDefault.onClick = [this] { session->chooseWindowsDefault(); };
    retrySaved.setComponentID("asio.retry-saved");
    retrySaved.onClick = [this] { session->retryOutput(); };
    controlPanel.setComponentID("asio.control-panel");
    controlPanel.setTooltip("Open the selected ASIO driver's panel, or the active ASIO output when browsing another backend. Recheck actual capabilities before using the route.");
    controlPanel.onClick = [this] { session->openControlPanel(); };
    addAndMakeVisible(followSource); followSource.setComponentID("audio-output.follow-source");
    followSource.setTooltip("Off by default. Requires an activated route. When enabled, each source-rate change requests that rate through a device switch; unsupported rates return the previous output. The buffer and routing stay confirmed.");
    followSource.onClick = [this] { session->setFollowSourceSampleRate(followSource.getToggleState()); refresh(); };
    liveOutput.setFont(juce::Font(juce::FontOptions(12.0f)));
    liveOutput.setTooltip("Aggregate playback latency uses the driver-reported output latency plus added processing delay, in the actual device rate. SRC/preview read-ahead is not extra timeline delay. This is not a measured acoustic round trip.");
    discover.setComponentID("asio.discover"); inspect.setComponentID("asio.inspect"); cancel.setComponentID("asio.cancel");
    validate.setComponentID("asio.validate"); ping.setComponentID("asio.ping"); activate.setComponentID("asio.activate");
    addAndMakeVisible(confirm); confirm.setComponentID("asio.confirm");
    addAndMakeVisible(matrix); matrix.setComponentID("asio.matrix"); matrix.setModel(this); matrix.setRowHeight(32);
    matrix.getHeader().addColumn("Canonical speaker", 1, 190, 130, 320, juce::TableHeaderComponent::notSortable);
    matrix.setHeaderHeight(34);
    drivers.onChange = [this]
    {
        const auto index = drivers.getSelectedId() - 1;
        if (!updating && index >= 0 && index < static_cast<int>(session->drivers().drivers.size())) session->selectDriver(session->drivers().drivers[static_cast<size_t>(index)].stableId);
    };
    profiles.onChange = [this] { if (!updating) session->selectProfile(profiles.getSelectedId() - 1); };
    const auto formatChanged = [this]
    {
        if (updating || !session->capabilities()) return;
        const auto& capabilities = session->capabilities()->capabilities;
        const auto r = rates.getSelectedId() - 1, b = buffers.getSelectedId() - 1;
        session->setFormat(r >= 0 && r < static_cast<int>(capabilities.sampleRates.size()) ? capabilities.sampleRates[static_cast<size_t>(r)] : 0,
            b >= 0 && b < static_cast<int>(capabilities.bufferSizes.size()) ? capabilities.bufferSizes[static_cast<size_t>(b)] : 0);
    };
    rates.onChange = formatChanged; buffers.onChange = formatChanged;
    discover.onClick = [this] { session->refreshDrivers(); };
    inspect.onClick = [this] { session->inspect(); };
    cancel.onClick = [this] { session->cancel(); };
    validate.onClick = [this] { confirm.setToggleState(false, juce::dontSendNotification); session->validate(); };
    ping.onClick = [this] { session->ping(pingOutput.getSelectedId() - 1); };
    activate.onClick = [this] { session->activate(confirm.getToggleState()); };
    confirm.onClick = [this] { refresh(); };
    session->addChangeListener(this);
    session->setDeviceUiConsumer(this, [safe = juce::Component::SafePointer<AsioSettingsComponent>(this)] { return safe && safe->isShowing(); });
    setSize(1040, 720); refresh();
}
AsioSettingsComponent::~AsioSettingsComponent()
{ session->setDeviceUiConsumer(this); session->removeChangeListener(this); session->cancel(); matrix.setModel(nullptr); }
void AsioSettingsComponent::paint(juce::Graphics& graphics)
{ graphics.fillAll(ModernAppleLookAndFeel::windowBackground()); }
void AsioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(20);
    title.setBounds(area.removeFromTop(30)); help.setBounds(area.removeFromTop(40)); area.removeFromTop(8);
    auto row = area.removeFromTop(32);
    windowsDefault.setBounds(row.removeFromRight(220)); row.removeFromRight(10); backends.setBounds(row);
    area.removeFromTop(10); row = area.removeFromTop(32);
    cancel.setBounds(row.removeFromRight(136)); row.removeFromRight(8); inspect.setBounds(row.removeFromRight(144)); row.removeFromRight(8);
    discover.setBounds(row.removeFromRight(124)); row.removeFromRight(10); drivers.setBounds(row);
    area.removeFromTop(10); row = area.removeFromTop(32);
    buffers.setBounds(row.removeFromRight(164)); row.removeFromRight(10); rates.setBounds(row.removeFromRight(164)); row.removeFromRight(10); profiles.setBounds(row);
    area.removeFromTop(8); row = area.removeFromTop(30);
    controlPanel.setBounds(row.removeFromRight(160)); row.removeFromRight(10); followSource.setBounds(row);
    liveOutput.setBounds(area.removeFromTop(28));
    area.removeFromTop(12);
    auto bottom = area.removeFromBottom(184);
    matrix.setBounds(area);
    bottom.removeFromTop(10); warnings.setBounds(bottom.removeFromTop(44));
    confirm.setBounds(bottom.removeFromTop(24)); bottom.removeFromTop(6);
    row = bottom.removeFromTop(32); validate.setBounds(row.removeFromLeft(108)); row.removeFromLeft(10);
    activate.setBounds(row.removeFromRight(112)); row.removeFromRight(10); ping.setBounds(row.removeFromRight(82)); row.removeFromRight(8); pingOutput.setBounds(row);
    bottom.removeFromTop(10);
    retrySaved.setBounds(bottom.removeFromRight(140).withHeight(30)); bottom.removeFromRight(10);
    status.setBounds(bottom);
}
void AsioSettingsComponent::refresh()
{
    const juce::ScopedValueSetter<bool> updatingScope(updating, true);
    for (auto* label : {&title, &help, &status, &liveOutput}) label->setColour(juce::Label::textColourId, ModernAppleLookAndFeel::primaryText());
    for (auto* combo : {&backends, &drivers, &profiles, &rates, &buffers, &pingOutput})
        combo->setColour(juce::ComboBox::textColourId, ModernAppleLookAndFeel::primaryText());
    warnings.setColour(juce::TextEditor::textColourId, ModernAppleLookAndFeel::primaryText());
    warnings.setColour(juce::TextEditor::backgroundColourId, ModernAppleLookAndFeel::mutedSurface());
    warnings.applyColourToAllText(ModernAppleLookAndFeel::primaryText(), true);
    matrix.setColour(juce::ListBox::backgroundColourId, ModernAppleLookAndFeel::panelBackground());
    matrix.setColour(juce::ListBox::outlineColourId, ModernAppleLookAndFeel::borderColour());
    matrix.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, ModernAppleLookAndFeel::mutedSurface());
    matrix.getHeader().setColour(juce::TableHeaderComponent::textColourId, ModernAppleLookAndFeel::primaryText());
    matrix.getHeader().setColour(juce::TableHeaderComponent::outlineColourId, ModernAppleLookAndFeel::borderColour());
    const bool busy = session->busy();
    for (int index = 0; index < backends.getNumItems(); ++index)
        if (backends.getItemText(index).toStdString() == session->selectedBackend()) backends.setSelectedItemIndex(index, juce::dontSendNotification);
    backends.setEnabled(!busy); windowsDefault.setEnabled(!busy);
    if (displayedRevision != session->revision())
    { displayedRevision = session->revision(); confirm.setToggleState(false, juce::dontSendNotification); }
    drivers.clear(juce::dontSendNotification);
    int item = 0;
    for (const auto& driver : session->drivers().drivers)
    {
        drivers.addItem(juce::String(driver.displayName), ++item);
        drivers.setItemEnabled(item, !driver.stableId.empty());
        if (!driver.stableId.empty() && driver.stableId == session->selectedDriver()) drivers.setSelectedId(item, juce::dontSendNotification);
    }
    profiles.clear(juce::dontSendNotification); item = 0;
    for (const auto& profile : session->profiles()) profiles.addItem(profile.name, ++item);
    profiles.setSelectedId(session->selectedProfile() + 1, juce::dontSendNotification);
    rates.clear(juce::dontSendNotification); buffers.clear(juce::dontSendNotification);
    if (const auto* facts = session->capabilities())
    {
        item = 0;
        for (double rate : facts->capabilities.sampleRates)
        { rates.addItem(juce::String(rate, 0) + " Hz", ++item); if (rate == session->sampleRate()) rates.setSelectedId(item, juce::dontSendNotification); }
        item = 0;
        for (int buffer : facts->capabilities.bufferSizes)
        { buffers.addItem(juce::String(buffer) + " samples", ++item); if (buffer == session->bufferSize()) buffers.setSelectedId(item, juce::dontSendNotification); }
    }
    const auto& names = session->draft().outputNames;
    if (names != visibleNames)
    {
        visibleNames = names; matrix.getHeader().removeAllColumns();
        matrix.getHeader().addColumn("Canonical speaker", 1, 190, 130, 320, juce::TableHeaderComponent::notSortable);
        pingOutput.clear(juce::dontSendNotification);
        for (size_t index = 0; index < names.size(); ++index)
        {
            const auto name = juce::String(static_cast<int>(index + 1)) + ": " + juce::String(names[index]);
            matrix.getHeader().addColumn(name, static_cast<int>(index + 2), 104, 70, 240, juce::TableHeaderComponent::notSortable);
            pingOutput.addItem(name, static_cast<int>(index + 1));
        }
    }
    juce::StringArray messages;
    if (const auto& review = session->review())
        for (const auto& issue : review->issues()) messages.add((issue.fatal ? "Error: " : "Warning: ") + describeIssue(issue, session->draft()));
    const auto warningText = messages.joinIntoString("\n");
    const auto reviewText = warningText.isEmpty() ? juce::String("Physical channel names do not establish speaker roles. Confirm wiring with Ping.") : warningText;
    if (warnings.getText() != reviewText) warnings.setText(reviewText, false);
    warnings.setTooltip(warningText);
    status.setText(session->status(), juce::dontSendNotification);
    drivers.setEnabled(!busy); discover.setEnabled(!busy); inspect.setEnabled(!busy && !session->selectedDriver().empty()); cancel.setEnabled(busy);
    retrySaved.setEnabled(!busy && (session->canMaintainOutput() || session->canRetrySavedSelection()));
    controlPanel.setEnabled(!busy && session->canOpenControlPanel());
    followSource.setToggleState(session->followsSourceSampleRate(), juce::dontSendNotification);
    followSource.setEnabled(session->followsSourceSampleRate() || (!busy && session->canFollowSourceSampleRate()));
    liveOutput.setText(session->liveOutputStatus(), juce::dontSendNotification);
    for (auto* combo : {&profiles, &rates, &buffers, &pingOutput}) combo->setEnabled(!busy && session->capabilities());
    const bool valid = session->review() && session->review()->isValid();
    confirm.setEnabled(!busy && valid && session->review()->needsConfirmation());
    validate.setEnabled(!busy && session->capabilities()); ping.setEnabled(!busy && valid);
    activate.setEnabled(!busy && valid && (!session->review()->needsConfirmation() || confirm.getToggleState()));
    matrix.updateContent(); matrix.repaint();
}
int AsioSettingsComponent::getNumRows() { return static_cast<int>(session->draft().bus.speakers.size()); }
void AsioSettingsComponent::paintRowBackground(juce::Graphics& graphics, int row, int width, int height, bool)
{ if (row % 2 == 0) { graphics.setColour(ModernAppleLookAndFeel::primaryText().withAlpha(.035f)); graphics.fillRect(0, 0, width, height); } }
void AsioSettingsComponent::paintCell(juce::Graphics& graphics, int row, int column, int width, int height, bool)
{
    if (column != 1 || row < 0 || row >= getNumRows()) return;
    const auto& speaker = session->draft().bus.speakers[static_cast<size_t>(row)];
    graphics.setColour(ModernAppleLookAndFeel::primaryText()); graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawFittedText(ChannelLayoutCatalog::roleToAbbreviation(speaker.role) + "  " + juce::String(speaker.stableId), 8, 0, width - 12, height, juce::Justification::centredLeft, 1);
}
juce::Component* AsioSettingsComponent::refreshComponentForCell(int row, int column, bool, juce::Component* existing)
{
    if (column < 2 || row < 0 || row >= getNumRows()) { delete existing; return nullptr; }
    auto* button = dynamic_cast<juce::ToggleButton*>(existing);
    if (!button) { delete existing; button = new juce::ToggleButton; }
    const auto physical = column - 2;
    const auto& draft = session->draft();
    const auto& outputs = draft.outputsBySpeaker[static_cast<size_t>(row)];
    button->setComponentID("asio.matrix." + juce::String(row) + "." + juce::String(physical));
    button->setToggleState(std::find(outputs.begin(), outputs.end(), physical) != outputs.end(), juce::dontSendNotification);
    button->setEnabled(!session->busy());
    button->setTooltip(juce::String(draft.bus.speakers[static_cast<size_t>(row)].stableId) + " -> " + juce::String(physical + 1) + ": " + juce::String(draft.outputNames[static_cast<size_t>(physical)]));
    button->onClick = [this, button, row, physical] { session->setConnection(row, physical, button->getToggleState()); };
    return button;
}
}
