#pragma once

#include <vector>
#include <memory>

#include <juce_core/juce_core.h>

namespace singleeditor
{
class RecoveredPlaylistDocument;
class PlaylistModel final
{
public:
    enum class EntryKind
    {
        audio,
        bnk,
        wem,
        video,
        unknown
    };

    struct Entry
    {
        juce::File file;
        juce::Uuid id;
        std::shared_ptr<RecoveredPlaylistDocument> recovered;

        bool isValid() const noexcept { return recovered != nullptr || file.existsAsFile(); }
        juce::String displayName() const { return (recovered ? "Recovered " : "") + file.getFileName(); }
        juce::File activationFile() const;
        EntryKind kind() const noexcept;
        juce::String kindLabel() const;
        juce::String extensionBadge() const;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void playlistChanged() {}
        virtual void currentPlaylistIndexChanged(int) {}
    };

    void addFiles(const std::vector<juce::File>& files);
    // Idempotent by attempt, independent of path. Never selects even in an
    // otherwise empty playlist; only explicit activation changes currentIndex.
    int addRecoveredDocument(std::shared_ptr<RecoveredPlaylistDocument>);
    int indexOfEntry(const juce::Uuid&) const noexcept;
    void restoreFiles(const std::vector<juce::File>& files, int selectedIndex);
    void clear();
    void removeCurrent();
    void removeAt(int index);
    bool replaceCurrentFile(const juce::File& file);
    bool moveEntry(int fromIndex, int toIndex);
    int getNumEntries() const noexcept;
    const Entry* getEntry(int index) const noexcept;
    int getCurrentIndex() const noexcept;
    const Entry* getCurrentEntry() const noexcept;
    bool setCurrentIndex(int index);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    int indexOfFile(const juce::File& file) const noexcept;

    std::vector<Entry> entries;
    int currentIndex = -1;
    juce::ListenerList<Listener> listeners;
};
} // namespace singleeditor
