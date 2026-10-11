#include "Document/PlaylistModel.h"
#include "Document/RecoveredPlaylistDocument.h"

namespace singleeditor
{
namespace
{
PlaylistModel::EntryKind classifyEntryKind(const juce::File& file) noexcept
{
    const auto ext = file.getFileExtension().toLowerCase();
    if (ext == ".bnk")
        return PlaylistModel::EntryKind::bnk;
    if (ext == ".wem")
        return PlaylistModel::EntryKind::wem;
    if (ext == ".mp4" || ext == ".mov" || ext == ".mkv" || ext == ".avi" || ext == ".wmv" || ext == ".webm")
        return PlaylistModel::EntryKind::video;
    if (ext == ".wav" || ext == ".flac" || ext == ".aif" || ext == ".aiff" || ext == ".ogg" || ext == ".mp3" || ext == ".wma")
        return PlaylistModel::EntryKind::audio;

    return PlaylistModel::EntryKind::unknown;
}
} // namespace

juce::File PlaylistModel::Entry::activationFile() const
{
    return recovered ? recovered->snapshot()->state()->info.file : file;
}

PlaylistModel::EntryKind PlaylistModel::Entry::kind() const noexcept
{
    return recovered ? EntryKind::audio : classifyEntryKind(file);
}

juce::String PlaylistModel::Entry::kindLabel() const
{
    switch (kind())
    {
        case EntryKind::audio: return "Audio";
        case EntryKind::bnk: return "BNK";
        case EntryKind::wem: return "WEM";
        case EntryKind::video: return "Video";
        case EntryKind::unknown: break;
    }

    return "File";
}

juce::String PlaylistModel::Entry::extensionBadge() const
{
    auto ext = file.getFileExtension().trimCharactersAtStart(".");
    return ext.isNotEmpty() ? ext.toUpperCase() : juce::String("FILE");
}

void PlaylistModel::addFiles(const std::vector<juce::File>& files)
{
    bool changed = false;
    int firstAddedIndex = -1;

    for (const auto& file : files)
    {
        if (!file.existsAsFile())
            continue;

        const auto existing = indexOfFile(file);
        if (existing >= 0)
        {
            if (firstAddedIndex < 0)
                firstAddedIndex = existing;
            continue;
        }

        entries.push_back({file, juce::Uuid(), {}});
        changed = true;
        if (firstAddedIndex < 0)
            firstAddedIndex = static_cast<int>(entries.size()) - 1;
    }

    if (!changed && firstAddedIndex < 0)
        return;

    if (changed)
        listeners.call([](Listener& l) { l.playlistChanged(); });

    if (currentIndex < 0 && !entries.empty())
    {
        currentIndex = firstAddedIndex >= 0 ? firstAddedIndex : 0;
        listeners.call([this](Listener& l) { l.currentPlaylistIndexChanged(currentIndex); });
    }
}

int PlaylistModel::addRecoveredDocument(std::shared_ptr<RecoveredPlaylistDocument> recovered)
{
    if (!recovered || !recovered->snapshot()) return -1;
    const auto existing = indexOfEntry(recovered->attemptId());
    if (existing >= 0) return existing;
    entries.push_back({ recovered->snapshot()->state()->info.file, recovered->attemptId(), std::move(recovered) });
    const auto added = static_cast<int>(entries.size()) - 1;
    listeners.call([](Listener& l) { l.playlistChanged(); });
    return added;
}

int PlaylistModel::indexOfEntry(const juce::Uuid& id) const noexcept
{
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        if (entries[static_cast<std::size_t>(i)].id == id) return i;
    return -1;
}

void PlaylistModel::restoreFiles(const std::vector<juce::File>& files, int selectedIndex)
{
    entries.clear();

    for (const auto& file : files)
    {
        if (!file.existsAsFile())
            continue;

        if (indexOfFile(file) >= 0)
            continue;

        entries.push_back({ file, juce::Uuid(), {} });
    }

    if (entries.empty() || selectedIndex == -1)
        currentIndex = -1;
    else
        currentIndex = juce::jlimit(0, static_cast<int>(entries.size()) - 1, selectedIndex);

    listeners.call([](Listener& l) { l.playlistChanged(); });
    listeners.call([this](Listener& l) { l.currentPlaylistIndexChanged(currentIndex); });
}

void PlaylistModel::clear()
{
    entries.clear();
    currentIndex = -1;
    listeners.call([](Listener& l) { l.playlistChanged(); });
    listeners.call([](Listener& l) { l.currentPlaylistIndexChanged(-1); });
}

void PlaylistModel::removeCurrent()
{
    removeAt(currentIndex);
}

void PlaylistModel::removeAt(int index)
{
    if (index < 0 || index >= static_cast<int>(entries.size()))
        return;

    entries.erase(entries.begin() + index);
    if (entries.empty())
        currentIndex = -1;
    else if (currentIndex > index)
        --currentIndex;
    else if (currentIndex == index && currentIndex >= static_cast<int>(entries.size()))
        currentIndex = static_cast<int>(entries.size()) - 1;

    listeners.call([](Listener& l) { l.playlistChanged(); });
    listeners.call([this](Listener& l) { l.currentPlaylistIndexChanged(currentIndex); });
}

bool PlaylistModel::replaceCurrentFile(const juce::File& file)
{
    if (!file.existsAsFile()
        || currentIndex < 0
        || currentIndex >= static_cast<int>(entries.size()))
        return false;

    const auto existingIndex = indexOfFile(file);
    auto& current = entries[static_cast<std::size_t>(currentIndex)];
    if (current.recovered)
    {
        // Save As does not erase this document's history or its CAS readers.
        // Keep the independent image and identity through the row's lifetime.
        current.file = file;
    }
    else if (existingIndex >= 0 && existingIndex != currentIndex)
    {
        entries.erase(entries.begin() + currentIndex);
        currentIndex = existingIndex > currentIndex ? existingIndex - 1 : existingIndex;
    }
    else
    {
        entries[static_cast<std::size_t>(currentIndex)].file = file;
    }

    listeners.call([](Listener& l) { l.playlistChanged(); });
    return true;
}

bool PlaylistModel::moveEntry(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= static_cast<int>(entries.size()))
        return false;

    if (toIndex < 0 || toIndex >= static_cast<int>(entries.size()) || toIndex == fromIndex)
        return false;

    auto moved = entries[static_cast<std::size_t>(fromIndex)];
    entries.erase(entries.begin() + fromIndex);
    entries.insert(entries.begin() + toIndex, std::move(moved));

    if (currentIndex == fromIndex)
        currentIndex = toIndex;
    else if (fromIndex < currentIndex && toIndex >= currentIndex)
        --currentIndex;
    else if (fromIndex > currentIndex && toIndex <= currentIndex)
        ++currentIndex;

    listeners.call([](Listener& l) { l.playlistChanged(); });
    listeners.call([this](Listener& l) { l.currentPlaylistIndexChanged(currentIndex); });
    return true;
}

int PlaylistModel::getNumEntries() const noexcept
{
    return static_cast<int>(entries.size());
}

const PlaylistModel::Entry* PlaylistModel::getEntry(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(entries.size()))
        return nullptr;

    return &entries[static_cast<std::size_t>(index)];
}

int PlaylistModel::getCurrentIndex() const noexcept
{
    return currentIndex;
}

const PlaylistModel::Entry* PlaylistModel::getCurrentEntry() const noexcept
{
    return getEntry(currentIndex);
}

bool PlaylistModel::setCurrentIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(entries.size()) || index == currentIndex)
        return false;

    currentIndex = index;
    listeners.call([this](Listener& l) { l.currentPlaylistIndexChanged(currentIndex); });
    return true;
}

void PlaylistModel::addListener(Listener* listener)
{
    listeners.add(listener);
}

void PlaylistModel::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

int PlaylistModel::indexOfFile(const juce::File& file) const noexcept
{
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
    {
        if (!entries[static_cast<std::size_t>(i)].recovered && entries[static_cast<std::size_t>(i)].file == file)
            return i;
    }

    return -1;
}
} // namespace singleeditor
