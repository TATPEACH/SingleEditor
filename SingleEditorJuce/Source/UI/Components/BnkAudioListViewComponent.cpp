#include "UI/Components/BnkAudioListViewComponent.h"

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

namespace singleeditor
{
BnkAudioListViewComponent::BnkAudioListViewComponent(BnkDocument& documentToUse)
    : document(documentToUse)
{
    document.addListener(this);
    addAndMakeVisible(searchBox);
    searchBox.setTextToShowWhenEmpty("Search audio...", ModernAppleLookAndFeel::secondaryText().withAlpha(0.8f));
    searchBox.onTextChange = [this]
    {
        rebuildVisibleMedia();
        listBox.updateContent();
        repaint();
    };
    addAndMakeVisible(listBox);
    listBox.setModel(this);
    listBox.setOutlineThickness(0);
    listBox.setRowHeight(60);
    listBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    highlightedMediaIds = document.getSelectedEventMediaIds();
    rebuildVisibleMedia();
    listBox.updateContent();

    if (const auto selectedMediaId = document.getSelectedMediaId(); selectedMediaId.has_value())
    {
        for (int row = 0; row < static_cast<int>(mediaEntries.size()); ++row)
        {
            if (mediaEntries[static_cast<std::size_t>(row)].mediaId == *selectedMediaId)
            {
                listBox.selectRow(row, false, false);
                break;
            }
        }
    }
}

BnkAudioListViewComponent::~BnkAudioListViewComponent()
{
    document.removeListener(this);
}

void BnkAudioListViewComponent::resized()
{
    auto bounds = getLocalBounds().reduced(22, 18);
    bounds.removeFromTop(26);
    searchBox.setBounds(bounds.removeFromTop(30));
    bounds.removeFromTop(12);
    listBox.setBounds(bounds);
}

void BnkAudioListViewComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(ModernAppleLookAndFeel::workspaceSurface());
    g.fillRoundedRectangle(bounds.reduced(0.5f), 24.0f);

    auto content = getLocalBounds().reduced(22, 18);
    const auto info = document.getBankInfo();

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText(info.isValid() ? "Bank Audio" : "Bank Audio",
               content.removeFromTop(28),
               juce::Justification::centredLeft);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    if (mediaEntries.empty())
    {
        const auto emptyText = linkedOnly && !highlightedMediaIds.empty()
                                 ? (switchValueToken.isNotEmpty()
                                        ? "No raw media matched the selected Event and current Switch. Turn off Linked Only to browse the whole bank."
                                        : "No raw media matched the selected Event. Turn off Linked Only to browse the whole bank.")
                                 : (!searchBox.getText().trim().isEmpty()
                                        ? "No audio entries matched the current search."
                                        : "No playable media entries are available for this SoundBank yet.");
        g.drawFittedText(emptyText,
                         content,
                         juce::Justification::centred,
                         2);
        return;
    }

    auto summary = juce::String(mediaEntries.size()) + " raw media item(s)";
    summary << "   |   " << (linkedOnly ? "Linked Only" : "All Media");
    if (linkedOnly && switchValueToken.isNotEmpty())
        summary << "   |   Switch filtered";
    summary << "   |   Raw media browser";
    g.drawText(summary,
               content.removeFromTop(20),
               juce::Justification::centredLeft);
}

void BnkAudioListViewComponent::setLinkedOnly(bool shouldShowLinkedOnly)
{
    if (linkedOnly == shouldShowLinkedOnly)
        return;

    linkedOnly = shouldShowLinkedOnly;
    rebuildVisibleMedia();
    listBox.updateContent();
    repaint();
}

void BnkAudioListViewComponent::setSwitchTokenFilter(juce::String token)
{
    if (switchValueToken == token)
        return;

    switchValueToken = std::move(token);
    rebuildVisibleMedia();
    listBox.updateContent();
    repaint();
}

int BnkAudioListViewComponent::getNumRows()
{
    return static_cast<int>(mediaEntries.size());
}

void BnkAudioListViewComponent::paintListBoxItem(int rowNumber,
                                                 juce::Graphics& g,
                                                 int width,
                                                 int height,
                                                 bool rowIsSelected)
{
    if (!juce::isPositiveAndBelow(rowNumber, static_cast<int>(mediaEntries.size())))
        return;

    const auto& media = mediaEntries[static_cast<std::size_t>(rowNumber)];
    const auto isHighlighted = std::find(highlightedMediaIds.begin(),
                                         highlightedMediaIds.end(),
                                         media.mediaId) != highlightedMediaIds.end();
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(3.0f, 3.0f);

    auto fill = rowIsSelected ? ModernAppleLookAndFeel::accentBlue().withAlpha(0.12f)
                              : ModernAppleLookAndFeel::panelBackground();
    if (isHighlighted && !rowIsSelected)
        fill = ModernAppleLookAndFeel::accentOrange().withAlpha(0.10f);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 15.0f);

    if (rowIsSelected)
    {
        auto accent = bounds.removeFromLeft(3.0f).reduced(0.0f, 10.0f);
        g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.8f));
        g.fillRoundedRectangle(accent, 1.5f);
    }
    else if (isHighlighted)
    {
        auto accent = bounds.removeFromLeft(3.0f).reduced(0.0f, 10.0f);
        g.setColour(ModernAppleLookAndFeel::accentOrange().withAlpha(0.55f));
        g.fillRoundedRectangle(accent, 1.5f);
    }

    auto text = bounds.reduced(14.0f, 10.0f).toNearestInt();
    auto badgeArea = text.removeFromRight(110);
    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(13.5f, juce::Font::bold)));
    g.drawText(media.displayName, text.removeFromTop(18), juce::Justification::centredLeft);

    auto badgeBounds = badgeArea.removeFromRight(84).withTrimmedTop(1).withHeight(22);
    juce::Colour badgeFill = ModernAppleLookAndFeel::panelBackground().brighter(0.06f);
    const auto sourceGreen = juce::Colour::fromRGB(50, 158, 106);
    switch (media.availability)
    {
        case BnkDocument::PlayableMediaDescriptor::Availability::packedMedia:
            badgeFill = ModernAppleLookAndFeel::accentBlue().withAlpha(0.12f);
            break;
        case BnkDocument::PlayableMediaDescriptor::Availability::sourceMedia:
            badgeFill = sourceGreen.withAlpha(0.12f);
            break;
        case BnkDocument::PlayableMediaDescriptor::Availability::embeddedMedia:
            badgeFill = ModernAppleLookAndFeel::accentOrange().withAlpha(0.12f);
            break;
        case BnkDocument::PlayableMediaDescriptor::Availability::unresolved:
        default:
            break;
    }
    g.setColour(badgeFill);
    g.fillRoundedRectangle(badgeBounds.toFloat(), 11.0f);
    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
    g.drawText(BnkDocument::describeAvailability(media.availability).toUpperCase(),
               badgeBounds,
               juce::Justification::centred);

    juce::String detail;
    detail << "Media 0x" << juce::String::toHexString(static_cast<juce::int64>(media.mediaId)).toUpperCase();
    if (media.referenceCount > 0)
        detail << "   |   Referenced " << media.referenceCount << " time(s)";
    if (media.mediaFile.existsAsFile())
        detail << "   |   " << media.mediaFile.getFileName();
    else if (media.sourceFile.existsAsFile())
        detail << "   |   " << media.sourceFile.getFileName();

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText(detail, text, juce::Justification::centredLeft);
}

void BnkAudioListViewComponent::listBoxItemClicked(int row, const juce::MouseEvent& event)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(mediaEntries.size())))
        return;

    if (event.mods.isPopupMenu())
    {
        showMediaContextMenu(row);
        return;
    }

    const auto mediaId = mediaEntries[static_cast<std::size_t>(row)].mediaId;
    listBox.selectRow(row);
    document.selectMediaById(mediaId);
    if (onMediaSelected)
        onMediaSelected(mediaId);
    if (onMediaActivated)
        onMediaActivated(mediaId);
}

void BnkAudioListViewComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(mediaEntries.size())))
        return;

    const auto mediaId = mediaEntries[static_cast<std::size_t>(row)].mediaId;
    listBox.selectRow(row);
    document.selectMediaById(mediaId);
    if (onMediaSelected)
        onMediaSelected(mediaId);
    if (onMediaPlayRequested)
        onMediaPlayRequested(mediaId);
    else if (onMediaActivated)
        onMediaActivated(mediaId);
}

void BnkAudioListViewComponent::showMediaContextMenu(int row)
{
    if (!juce::isPositiveAndBelow(row, static_cast<int>(mediaEntries.size())))
        return;

    const auto& media = mediaEntries[static_cast<std::size_t>(row)];
    const auto packedFile = media.mediaFile;
    const auto sourceFile = media.sourceFile;
    const auto extractedFile = document.getEmbeddedMediaCacheFile(media.mediaId);

    juce::PopupMenu menu;
    menu.addItem(1, "Play");
    menu.addSeparator();
    menu.addItem(2, "Reveal Packed Media in Explorer", packedFile.existsAsFile());
    menu.addItem(3, "Reveal Source in Explorer", sourceFile.existsAsFile());
    menu.addItem(4, "Reveal Extracted Media in Explorer", extractedFile.existsAsFile());

    const auto screenPosition = juce::Desktop::getInstance().getMousePosition();
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetScreenArea(juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1))
                           .withDeletionCheck(*this),
                       [this, row, mediaId = media.mediaId, packedFile, sourceFile, extractedFile](int result)
                       {
                           if (result == 1)
                           {
                               listBox.selectRow(row);
                               document.selectMediaById(mediaId);
                               if (onMediaSelected)
                                   onMediaSelected(mediaId);
                               if (onMediaPlayRequested)
                                   onMediaPlayRequested(mediaId);
                               else if (onMediaActivated)
                                   onMediaActivated(mediaId);
                           }
                           else if (result == 2 && packedFile.existsAsFile())
                           {
                               packedFile.revealToUser();
                           }
                           else if (result == 3 && sourceFile.existsAsFile())
                           {
                               sourceFile.revealToUser();
                           }
                           else if (result == 4 && extractedFile.existsAsFile())
                           {
                               extractedFile.revealToUser();
                           }
                       });
}

void BnkAudioListViewComponent::bankDocumentChanged(BnkDocument::StatePtr)
{
    highlightedMediaIds = document.getSelectedEventMediaIds(switchValueToken);
    rebuildVisibleMedia();
    listBox.updateContent();
    if (const auto selectedMediaId = document.getSelectedMediaId(); selectedMediaId.has_value())
    {
        for (int row = 0; row < static_cast<int>(mediaEntries.size()); ++row)
        {
            if (mediaEntries[static_cast<std::size_t>(row)].mediaId == *selectedMediaId)
            {
                listBox.selectRow(row, false, false);
                repaint();
                return;
            }
        }
    }

    listBox.deselectAllRows();
    repaint();
}

void BnkAudioListViewComponent::rebuildVisibleMedia()
{
    auto allMedia = document.getPlayableMedia();
    const auto filter = searchBox.getText().trim().toLowerCase();

    auto matchesSearch = [&filter](const BnkDocument::PlayableMediaDescriptor& media)
    {
        if (filter.isEmpty())
            return true;

        juce::String haystack;
        haystack << media.displayName << "\n"
                 << media.shortName << "\n"
                 << media.relativePath << "\n"
                 << media.mediaFile.getFileName() << "\n"
                 << media.sourceFile.getFileName() << "\n"
                 << "0x" + juce::String::toHexString(static_cast<juce::int64>(media.mediaId)).toUpperCase();
        return haystack.toLowerCase().contains(filter);
    };

    if (!linkedOnly || highlightedMediaIds.empty())
    {
        mediaEntries.clear();
        mediaEntries.reserve(allMedia.size());
        for (auto& media : allMedia)
        {
            if (matchesSearch(media))
                mediaEntries.push_back(std::move(media));
        }
        return;
    }

    mediaEntries.clear();
    mediaEntries.reserve(allMedia.size());
    for (auto& media : allMedia)
    {
        if (std::find(highlightedMediaIds.begin(), highlightedMediaIds.end(), media.mediaId) != highlightedMediaIds.end()
            && matchesSearch(media))
        {
            mediaEntries.push_back(std::move(media));
        }
    }
}
} // namespace singleeditor
