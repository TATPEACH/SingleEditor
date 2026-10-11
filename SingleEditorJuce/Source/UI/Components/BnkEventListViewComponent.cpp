#include "UI/Components/BnkEventListViewComponent.h"

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

namespace singleeditor
{
BnkEventListViewComponent::BnkEventListViewComponent(BnkDocument& documentToUse)
    : document(documentToUse)
{
    document.addListener(this);
    addAndMakeVisible(searchBox);
    searchBox.setTextToShowWhenEmpty("Search events...", ModernAppleLookAndFeel::secondaryText().withAlpha(0.8f));
    searchBox.onTextChange = [this]
    {
        rebuildVisibleEvents();
        listBox.updateContent();
        repaint();
    };
    addAndMakeVisible(listBox);
    listBox.setModel(this);
    listBox.setOutlineThickness(0);
    listBox.setRowHeight(60);
    listBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    rebuildVisibleEvents();
    listBox.updateContent();

    if (const auto selectedEventId = document.getSelectedEventId(); selectedEventId.has_value())
    {
        for (int row = 0; row < static_cast<int>(visibleEventIds.size()); ++row)
        {
            if (visibleEventIds[static_cast<std::size_t>(row)] == *selectedEventId)
            {
                listBox.selectRow(row, false, false);
                break;
            }
        }
    }
}

BnkEventListViewComponent::~BnkEventListViewComponent()
{
    document.removeListener(this);
}

void BnkEventListViewComponent::resized()
{
    auto bounds = getLocalBounds().reduced(22, 18);
    bounds.removeFromTop(26);
    searchBox.setBounds(bounds.removeFromTop(30));
    bounds.removeFromTop(12);
    listBox.setBounds(bounds);
}

void BnkEventListViewComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(ModernAppleLookAndFeel::workspaceSurface());
    g.fillRoundedRectangle(bounds.reduced(0.5f), 24.0f);

    auto content = getLocalBounds().reduced(22, 18);
    const auto info = document.getBankInfo();

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText(info.isValid() ? info.file.getFileName() : "Wwise SoundBank",
               content.removeFromTop(28),
               juce::Justification::centredLeft);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(13.0f)));

    if (!info.isValid())
    {
        g.drawFittedText("Open a BNK file to browse its Event graph.",
                         content,
                         juce::Justification::centred,
                         2);
        return;
    }

    juce::StringArray summary;
    summary.add("Wwise BNK");
    summary.add("Events " + juce::String(info.eventCount));
    summary.add("Media " + juce::String(info.mediaCount));
    summary.add("HIRC " + juce::String(info.hircObjectCount));
    if (info.version != 0)
        summary.add("Version " + juce::String(info.version));
    if (info.bankId != 0)
        summary.add("Bank " + juce::String::toHexString(static_cast<juce::int64>(info.bankId)).toUpperCase());

    g.drawText(summary.joinIntoString("   |   "),
               content.removeFromTop(20),
               juce::Justification::centredLeft);

    if (visibleEventIds.empty() && !searchBox.getText().trim().isEmpty())
    {
        g.drawFittedText("No events matched the current search.",
                         content,
                         juce::Justification::centred,
                         2);
    }
}

int BnkEventListViewComponent::getNumRows()
{
    return static_cast<int>(visibleEventIds.size());
}

void BnkEventListViewComponent::paintListBoxItem(int rowNumber,
                                                 juce::Graphics& g,
                                                 int width,
                                                 int height,
                                                 bool rowIsSelected)
{
    const auto state = document.getState();
    if (state == nullptr || rowNumber < 0 || rowNumber >= static_cast<int>(visibleEventIds.size()))
        return;

    const auto eventId = visibleEventIds[static_cast<std::size_t>(rowNumber)];
    const auto eventIt = std::find_if(state->events.begin(),
                                      state->events.end(),
                                      [eventId](const BnkDocument::EventDescriptor& event)
                                      {
                                          return event.id == eventId;
                                      });
    if (eventIt == state->events.end())
        return;

    const auto& event = *eventIt;
    const auto hasRtpc = BnkDocument::hasRtpcMetadataForEvent(*state,event);

    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(3.0f, 3.0f);

    auto rowFill = rowIsSelected ? ModernAppleLookAndFeel::accentBlue().withAlpha(0.12f)
                                 : ModernAppleLookAndFeel::panelBackground();
    if (event.playbackHint == BnkDocument::PlaybackHint::unavailable)
        rowFill = ModernAppleLookAndFeel::panelBackground().interpolatedWith(ModernAppleLookAndFeel::workspaceSurface(), 0.35f);

    g.setColour(rowFill);
    g.fillRoundedRectangle(bounds, 15.0f);

    if (rowIsSelected)
    {
        auto accent = bounds.removeFromLeft(3.0f).reduced(0.0f, 10.0f);
        g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.8f));
        g.fillRoundedRectangle(accent, 1.5f);
    }
    else if (event.playbackHint == BnkDocument::PlaybackHint::unavailable)
    {
        auto accent = bounds.removeFromLeft(3.0f).reduced(0.0f, 10.0f);
        g.setColour(ModernAppleLookAndFeel::accentOrange().withAlpha(0.55f));
        g.fillRoundedRectangle(accent, 1.5f);
    }

    auto text = bounds.reduced(14.0f, 10.0f).toNearestInt();
    auto badgeArea = text.removeFromRight(170);
    juce::StringArray badges;
    badges.add(BnkDocument::describePlaybackHint(event.playbackHint));
    if (hasRtpc)
        badges.add("RTPC");
    if (event.stateModifiersResolved && !event.stateModifiers.empty()
        && event.stateTargetScope != BnkDocument::EventDescriptor::StateTargetScope::unavailable)
        badges.add("State");
    if (event.playbackHint == BnkDocument::PlaybackHint::switchDefault)
        badges.add("Switch");
    if (!event.hasPlayableMedia())
        badges.add("Partial");

    const auto badgeText = badges.joinIntoString(" · ");
    auto badgeBounds = badgeArea.removeFromTop(20).removeFromRight(juce::jmax(96, juce::roundToInt(7.0f * static_cast<float>(badgeText.length()))));
    g.setColour(ModernAppleLookAndFeel::mutedSurface());
    g.fillRoundedRectangle(badgeBounds.toFloat(), 10.0f);
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    g.drawFittedText(badgeText, badgeBounds.reduced(8, 2), juce::Justification::centred, 1);

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    g.drawText(event.displayName, text.removeFromTop(18), juce::Justification::centredLeft);

    juce::StringArray detailParts;
    if (event.targetObjectName.isNotEmpty())
        detailParts.add("Target " + event.targetObjectName);
    detailParts.add("Media " + juce::String(static_cast<int>(event.media.size())));
    if (!event.actionIds.empty())
        detailParts.add("Actions " + juce::String(static_cast<int>(event.actionIds.size())));
    if (event.playbackSummary.isNotEmpty())
        detailParts.add(event.playbackSummary);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText(detailParts.joinIntoString("   |   "), text, juce::Justification::centredLeft);
}

void BnkEventListViewComponent::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    const auto state = document.getState();
    if (state == nullptr || row < 0 || row >= static_cast<int>(visibleEventIds.size()))
        return;

    const auto eventId = visibleEventIds[static_cast<std::size_t>(row)];
    listBox.selectRow(row);
    document.selectEventById(eventId);
    if (onEventActivated)
        onEventActivated(eventId);
}

void BnkEventListViewComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    const auto state = document.getState();
    if (state == nullptr || row < 0 || row >= static_cast<int>(visibleEventIds.size()))
        return;

    const auto eventId = visibleEventIds[static_cast<std::size_t>(row)];
    listBox.selectRow(row);
    document.selectEventById(eventId);

    if (onEventPlayRequested)
        onEventPlayRequested(eventId);
    else if (onEventActivated)
        onEventActivated(eventId);
}

void BnkEventListViewComponent::bankDocumentChanged(BnkDocument::StatePtr)
{
    rebuildVisibleEvents();
    listBox.updateContent();
    if (const auto selectedEventId = document.getSelectedEventId(); selectedEventId.has_value())
    {
        if (const auto state = document.getState(); state != nullptr)
        {
            for (int row = 0; row < static_cast<int>(visibleEventIds.size()); ++row)
            {
                if (visibleEventIds[static_cast<std::size_t>(row)] == *selectedEventId)
                {
                    listBox.selectRow(row, false, false);
                    repaint();
                    return;
                }
            }
        }
    }

    listBox.deselectAllRows();
    repaint();
}

void BnkEventListViewComponent::rebuildVisibleEvents()
{
    visibleEventIds.clear();

    const auto state = document.getState();
    if (state == nullptr)
        return;

    const auto filter = searchBox.getText().trim().toLowerCase();
    for (const auto& event : state->events)
    {
        if (filter.isNotEmpty())
        {
            juce::String haystack;
            haystack << event.displayName << "\n"
                     << event.summary << "\n"
                     << event.playbackSummary << "\n"
                     << event.targetObjectName;
            if (!haystack.toLowerCase().contains(filter))
                continue;
        }

        visibleEventIds.push_back(event.id);
    }
}
} // namespace singleeditor
