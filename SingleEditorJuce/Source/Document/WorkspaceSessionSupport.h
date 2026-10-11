#pragma once

#include <map>
#include <memory>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Bnk/BnkDocument.h"
#include "Document/PlaylistModel.h"
#include "Document/SingleTrackDocument.h"
#include "Waveform/WaveformModel.h"

namespace singleeditor
{
namespace recovery { struct RecoveredCandidateDocument; }
class WorkspaceSessionSupport
{
public:
    enum class AudioDocumentReuseResult
    {
        none = 0,
        alreadyCurrent,
        restoredRetained
    };

    WorkspaceSessionSupport(juce::ApplicationProperties& appPropertiesToUse,
                            SingleTrackDocument& documentToUse,
                            BnkDocument& bnkDocumentToUse,
                            PlaylistModel& playlistModelToUse,
                            WaveformModel& waveformModelToUse);

    void restoreRecentFiles(int maxRecentFiles);
    void persistRecentFiles();
    void rememberOpenedFiles(const std::vector<juce::File>& files);
    std::vector<juce::File> getValidRecentFiles(bool pruneMissing);
    void clearRecentFiles();
    bool hasRecentFiles() const;

    bool shouldRetainPlaylistAudio() const;
    bool shouldWarmRetainableAudioFile(const juce::File& file) const;
    std::vector<juce::File> collectWarmableFiles(const std::vector<juce::File>& candidateFiles,
                                                 int maxCount = -1) const;
    void clearRetainedPlaylistAudio();
    void discardRetainedAudioDocument(const juce::File& file);
    void pruneRetainedPlaylistAudio();
    void retainCurrentAudioDocumentIfNeeded();
    bool restoreRetainedAudioDocumentIfAvailable(const juce::File& file);
    AudioDocumentReuseResult reuseAudioDocumentIfAvailable(const juce::File& file);
    juce::File audioActivationFile(const juce::File& target) const;
    bool restoreRecoveredDocument(const juce::Uuid& entryId, juce::String& error);
    bool isActiveRecoveredDocument(const juce::Uuid& entryId) const;
    void releaseRecoveredActivation();
    struct UnsavedDocument final
    {
        // Null for an ordinary live Document. Recovered rows use stable entry
        // identity, including multiple independent attempts at the same path.
        juce::Uuid recoveredEntry{ nullptr };
        bool current = false;
        SingleTrackDocument::WorkspaceSnapshot::Ptr image;
        bool sameRevision(const UnsavedDocument& other) const noexcept
        {
            return recoveredEntry == other.recoveredEntry && image && other.image && image->sameContents(*other.image);
        }
    };
    // Close-time in-memory inventory only. Live state overrides a stale retained
    // image, and a temporarily uncapturable dirty live Document is still listed.
    std::vector<UnsavedDocument> collectUnsavedDocuments() const;
    // Called by the UI owner after a worker transfers a complete result. Old
    // attempts, removed entries and states changed since activation are rejected.
    bool acceptRecoveredPeaks(const juce::Uuid& attempt, SingleTrackDocument::StatePtr originalCore,
                              std::shared_ptr<WaveformModel::PeakCache> peaks);

    juce::File getMappedBnkProjectRoot(const juce::File& bankFile) const;
    void setMappedBnkProjectRoot(const juce::File& bankFile, const juce::File& projectRoot);

    juce::File getDefaultBrowseDirectory() const;
    juce::File getCurrentLoadedFile() const;

    static juce::String serialisePlaylistFiles(const PlaylistModel& playlistModel);
    static int serialisedPlaylistIndex(const PlaylistModel& playlistModel);
    static std::vector<juce::File> deserialisePlaylistFiles(const juce::String& serialised);

private:
    struct RetainedAudioDocument
    {
        SingleTrackDocument::WorkspaceSnapshot::Ptr document;
        std::shared_ptr<WaveformModel::PeakCache> peakCache;
        std::shared_ptr<WaveformModel::SpectrogramCache> spectrogramCache;
    };

    static juce::String makeRetainedAudioKey(const juce::File& file);
    static bool isRetainablePlaylistAudioFile(const juce::File& file) noexcept;
    static juce::String makeBnkProjectMappingKey(const juce::File& bankFile);

    juce::ApplicationProperties& appProperties;
    SingleTrackDocument& document;
    BnkDocument& bnkDocument;
    PlaylistModel& playlistModel;
    WaveformModel& waveformModel;
    juce::RecentlyOpenedFilesList recentFiles;
    juce::String pendingLastBrowseDirectory;
    std::map<juce::String, RetainedAudioDocument> retainedPlaylistAudio;
    std::optional<juce::Uuid> activeRecoveredEntry;
    media::DocumentIdentity activeRecoveredIdentity;
    SingleTrackDocument::StatePtr activeRecoveredCoreState;
};
} // namespace singleeditor
