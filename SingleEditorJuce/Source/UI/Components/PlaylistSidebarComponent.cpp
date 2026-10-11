#include "UI/Components/PlaylistSidebarComponent.h"
#include "Document/RecoveredPlaylistDocument.h"

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"
#include "UI/Components/ModernFileSummaryComponent.h"
#include <set>

namespace singleeditor
{
PlaylistSidebarComponent::PlaylistSidebarComponent(PlaylistModel& playlistToUse)
    : playlist(playlistToUse)
{
    playlist.addListener(this);
    if (!RendererVisualStyle::isClassic())
        getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
    addAndMakeVisible(listBox);
    listBox.setModel(this);
    listBox.setRowHeight(52);
    listBox.setOutlineThickness(0);
    listBox.setHasFocusOutline(!RendererVisualStyle::isClassic());
    listBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
}

PlaylistSidebarComponent::~PlaylistSidebarComponent()
{
    playlist.removeListener(this);
}

void PlaylistSidebarComponent::resized()
{
    if (!RendererVisualStyle::isClassic())
    {
        listBox.setRowHeight(31);
        listBox.setBounds(getLocalBounds().withTrimmedTop(ModernPanelPainter::headerHeight).reduced(8, 6));
        return;
    }
    const auto compact = getHeight() < 140;
    if (compact)
    {
        const auto headerHeight = 32;
        listBox.setRowHeight(44);
        listBox.setBounds(getLocalBounds().withTrimmedTop(headerHeight)
                             .reduced(10, 4));
        return;
    }

    listBox.setRowHeight(52);
    listBox.setBounds(getLocalBounds().withTrimmedTop(56)
                         .reduced(24, 12));
}

void PlaylistSidebarComponent::paint(juce::Graphics& g)
{
    if (!RendererVisualStyle::isClassic())
    {
        ModernPanelPainter::paint(g, getLocalBounds(), "Playlist", ModernPanelPainter::Icon::playlist);
        if (playlist.getNumEntries() == 0)
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(12.0f));
            g.drawFittedText("Open files to start auditioning.", getLocalBounds().withTrimmedTop(38).reduced(12, 4),
                            juce::Justification::topLeft, 3);
        }
        return;
    }
    auto bounds = getLocalBounds().toFloat();
    // Shadow: rgba(0, 0, 0, 0.04), offset-y: 4px, blur-radius: 20px
    g.setColour(juce::Colours::black.withAlpha(0.04f));
    g.fillRoundedRectangle(bounds.translated(0.0f, 4.0f).reduced(0.5f), ModernAppleLookAndFeel::cardRadius() + 1.0f);
    g.setColour(ModernAppleLookAndFeel::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::cardRadius());

    const auto compact = getHeight() < 140;
    auto content = getLocalBounds().reduced(compact ? 12 : 24,
                                            compact ? 8 : 26);
    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(compact ? 13.0f : 15.0f,
                                           juce::Font::bold)));
    g.drawText("Playlist", content.removeFromTop(compact ? 18 : 22),
               juce::Justification::centredLeft);
    if (!compact)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText("AUDIO ASSETS", content.removeFromTop(14),
                   juce::Justification::centredLeft);
    }

    if (playlist.getNumEntries() == 0)
    {
        content.removeFromTop(compact ? 6 : 16);
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(compact ? 11.0f : 14.0f)));
        g.drawFittedText("Open files to start auditioning.",
                         content,
                         juce::Justification::topLeft,
                         3);
    }
}

int PlaylistSidebarComponent::getNumRows()
{
    return playlist.getNumEntries();
}

void PlaylistSidebarComponent::paintListBoxItem(int rowNumber,
                                                juce::Graphics& g,
                                                int width,
                                                int height,
                                                bool rowIsSelected)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(4.0f, 5.0f);
    const auto* entry = playlist.getEntry(rowNumber);
    if (entry == nullptr)
        return;

    const auto isCurrent = rowNumber == playlist.getCurrentIndex();
    if (!RendererVisualStyle::isClassic())
    {
        auto row = juce::Rectangle<int>(0, 2, width, height - 4).reduced(1, 0);
        // Arrow navigation selects a candidate without replacing the loaded
        // document. Keep that selection distinct from the current audio source.
        if (rowIsSelected || (isCurrent && listBox.getNumSelectedRows() == 0))
        {
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(RendererVisualStyle::isNight() ? 0.22f : 0.12f));
            g.fillRoundedRectangle(row.toFloat(), 7.0f);
            g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.30f));
            g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 7.0f, 1.0f);
        }
        if (isCurrent)
        {
            g.setColour(RendererVisualStyle::accentBlue());
            g.fillRoundedRectangle(static_cast<float>(row.getX() + 2),
                static_cast<float>(row.getY() + 6), 2.0f,
                static_cast<float>(juce::jmax(0, row.getHeight() - 12)), 1.0f);
        }
        auto line = row.reduced(6, 4);
        auto index = line.removeFromLeft(22);
        g.setColour(RendererVisualStyle::accentBlue().withAlpha(0.10f));
        g.fillRoundedRectangle(index.toFloat(), 5.0f);
        g.setColour(RendererVisualStyle::accentBlue());
        g.setFont(ModernTypography::font(11.0f));
        g.drawText(juce::String(rowNumber + 1).paddedLeft('0', 2), index, juce::Justification::centred, true);
        line.removeFromLeft(7);
        if (width >= 180)
        {
            const auto found = loadedDurations.find(entry->file.getFullPathName());
            const auto trailing = entry->recovered
                ? ModernFileSummaryComponent::durationText(entry->recovered->snapshot()->state()->info.durationSeconds)
                : found != loadedDurations.end()
                ? ModernFileSummaryComponent::durationText(found->second) : entry->extensionBadge();
            auto time = line.removeFromRight(entry->recovered || found != loadedDurations.end() ? 68 : 34);
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(11.0f));
            g.drawText(trailing, time, juce::Justification::centredRight, true);
            line.removeFromRight(6);
        }
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(13.0f));
        g.drawText(entry->displayName(), line, juce::Justification::centredLeft, true);
        return;
    }
    auto fill = ModernAppleLookAndFeel::panelBackground().brighter(0.01f);
    if (isCurrent || rowIsSelected)
        fill = ModernAppleLookAndFeel::panelBackground();

    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 12.0f);

    if (isCurrent || rowIsSelected)
    {
        auto accentBar = bounds.removeFromLeft(3.0f).reduced(0.0f, 9.0f);
        g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.82f));
        g.fillRoundedRectangle(accentBar, 1.5f);

        g.setColour(juce::Colours::black.withAlpha(0.018f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), 12.0f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, 12.0f);
    }

    const auto compactHeight = height < 48;
    const auto narrow = width < 180;
    auto text = bounds.reduced(compactHeight ? 8.0f : 12.0f,
                               compactHeight ? 4.0f : 6.0f).toNearestInt();
    const auto badgeWidth = narrow ? 38 : 56;
    auto badgeArea = text.removeFromRight(badgeWidth)
                         .removeFromTop(compactHeight ? 18 : 20);
    const auto badgeColour = [&]() -> juce::Colour
    {
        switch (entry->kind())
        {
            case PlaylistModel::EntryKind::bnk: return juce::Colour(0xffc084fc);
            case PlaylistModel::EntryKind::wem: return juce::Colour(0xff60a5fa);
            case PlaylistModel::EntryKind::video: return juce::Colour(0xfff59e0b);
            case PlaylistModel::EntryKind::audio: return juce::Colour(0xff34d399);
            case PlaylistModel::EntryKind::unknown: break;
        }

        return ModernAppleLookAndFeel::secondaryText();
    }();

    g.setColour(badgeColour.withAlpha(0.14f));
    g.fillRoundedRectangle(badgeArea.toFloat(), 9.0f);
    g.setColour(badgeColour.darker(0.35f));
    g.setFont(juce::Font(juce::FontOptions(compactHeight ? 9.0f : 10.0f,
                                           juce::Font::bold)));
    g.drawText(entry->extensionBadge(), badgeArea.reduced(narrow ? 2 : 5, 2),
               juce::Justification::centred, true);

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(juce::Font(juce::FontOptions(compactHeight ? 11.0f : 13.0f,
                                           juce::Font::bold)));
    g.drawText(entry->displayName(), text.removeFromTop(compactHeight ? 18 : 20),
               juce::Justification::centredLeft, true);

    if (!compactHeight && !narrow)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(entry->kindLabel() + "  •  " + entry->file.getParentDirectory().getFileName(),
                   text.removeFromTop(14),
                   juce::Justification::centredLeft,
                   true);
    }
}

void PlaylistSidebarComponent::listBoxItemClicked(int row, const juce::MouseEvent& event)
{
    if (row < 0 || row >= playlist.getNumEntries())
        return;

    if (event.mods.isPopupMenu())
    {
        showEntryContextMenu(row, event);
        return;
    }

    activateEntry(row);
}

void PlaylistSidebarComponent::listBoxItemDoubleClicked(int row, const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        showEntryContextMenu(row, event);
        return;
    }

    activateEntry(row);
}

void PlaylistSidebarComponent::returnKeyPressed(int row)
{
    activateEntry(row);
}

void PlaylistSidebarComponent::activateEntry(int row)
{
    if (playlist.getEntry(row) == nullptr)
        return;

    // The application callback owns unsaved-change confirmation and loading.
    // Keyboard activation must use the same path as a click, without first
    // changing the model's current source or starting playback on its own.
    if (onEntryActivated)
        onEntryActivated(row);
    else
        playlist.setCurrentIndex(row);
}

void PlaylistSidebarComponent::showEntryContextMenu(int row, const juce::MouseEvent& event)
{
    const auto* entry = playlist.getEntry(row);
    if (entry == nullptr)
        return;

    juce::PopupMenu menu;
    menu.addItem(1, "Open");
    menu.addItem(2, entry->recovered ? "Reveal Original in Explorer" : "Reveal in Explorer", entry->file.existsAsFile());
    menu.addSeparator();
    menu.addItem(4, "Move Up", row > 0);
    menu.addItem(5, "Move Down", row + 1 < playlist.getNumEntries());
    menu.addSeparator();
    menu.addItem(3, "Remove from Playlist");

    juce::ignoreUnused(event);
    const auto screenPosition = juce::Desktop::getInstance().getMousePosition();
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetScreenArea(juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1))
                           .withDeletionCheck(*this),
                       [this, entryId = entry->id, file = entry->file](int result)
                       {
                           const auto row = playlist.indexOfEntry(entryId);
                           if (row < 0) return;
                           if (result == 1)
                           {
                               activateEntry(row);
                           }
                           else if (result == 2)
                           {
                               if (onRevealFileRequested)
                                   onRevealFileRequested(file);
                               else
                                   file.revealToUser();
                           }
                           else if (result == 4)
                           {
                               playlist.moveEntry(row, row - 1);
                           }
                           else if (result == 5)
                           {
                               playlist.moveEntry(row, row + 1);
                           }
                           else if (result == 3 && onEntryRemoveRequested)
                           {
                               onEntryRemoveRequested(row);
                           }
                       });
}

void PlaylistSidebarComponent::playlistChanged()
{
    std::set<juce::String> currentPaths;
    for (int i = 0; i < playlist.getNumEntries(); ++i)
        currentPaths.insert(playlist.getEntry(i)->file.getFullPathName());
    for (auto it = loadedDurations.begin(); it != loadedDurations.end();)
    {
        if (currentPaths.count(it->first) == 0) it = loadedDurations.erase(it); else ++it;
    }
    listBox.updateContent();
    repaint();
}

void PlaylistSidebarComponent::currentPlaylistIndexChanged(int newIndex)
{
    listBox.selectRow(newIndex, false, false);
    repaint();
}

void PlaylistSidebarComponent::updateLoadedDuration(const juce::File& file, double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0) return;
    loadedDurations[file.getFullPathName()] = seconds;
    listBox.repaint();
}

void PlaylistSidebarComponent::lookAndFeelChanged()
{
    listBox.repaint();
    repaint();
}
} // namespace singleeditor
