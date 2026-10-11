#include "Document/WorkspaceSessionSupport.h"
#include "Document/RecoveredPlaylistDocument.h"

#include <set>

namespace singleeditor
{
namespace
{
constexpr auto recentFilesKey = "recentFiles";
constexpr auto lastBrowseDirectoryKey = "lastBrowseDirectory";
constexpr auto retainPlaylistAudioKey = "retainPlaylistAudioLoaded";
constexpr auto defaultWwiseProjectRootKey = "defaultWwiseProjectRoot";
} // namespace

WorkspaceSessionSupport::WorkspaceSessionSupport(juce::ApplicationProperties& appPropertiesToUse,
                                                 SingleTrackDocument& documentToUse,
                                                 BnkDocument& bnkDocumentToUse,
                                                 PlaylistModel& playlistModelToUse,
                                                 WaveformModel& waveformModelToUse)
    : appProperties(appPropertiesToUse),
      document(documentToUse),
      bnkDocument(bnkDocumentToUse),
      playlistModel(playlistModelToUse),
      waveformModel(waveformModelToUse)
{
}

void WorkspaceSessionSupport::restoreRecentFiles(int maxRecentFiles)
{
    recentFiles.setMaxNumberOfItems(maxRecentFiles);
    if (auto* settings = appProperties.getUserSettings())
    {
        recentFiles.restoreFromString(settings->getValue(recentFilesKey));
        pendingLastBrowseDirectory = settings->getValue(lastBrowseDirectoryKey).trim();
    }
}

void WorkspaceSessionSupport::persistRecentFiles()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue(recentFilesKey, recentFiles.toString());
        if (pendingLastBrowseDirectory.isNotEmpty())
            settings->setValue(lastBrowseDirectoryKey, pendingLastBrowseDirectory);
    }
}

void WorkspaceSessionSupport::rememberOpenedFiles(const std::vector<juce::File>& files)
{
    for (const auto& file : files)
    {
        if (file.existsAsFile())
            recentFiles.addFile(file);
    }

    if (!files.empty())
        pendingLastBrowseDirectory = files.front().getParentDirectory().getFullPathName();
}

std::vector<juce::File> WorkspaceSessionSupport::getValidRecentFiles(bool pruneMissing)
{
    std::vector<juce::File> files;
    files.reserve(static_cast<std::size_t>(recentFiles.getNumFiles()));

    for (int index = 0; index < recentFiles.getNumFiles(); ++index)
    {
        const auto file = recentFiles.getFile(index);
        if (file.existsAsFile())
            files.push_back(file);
        else if (pruneMissing)
            recentFiles.removeFile(file);
    }

    return files;
}

void WorkspaceSessionSupport::clearRecentFiles()
{
    recentFiles.clear();
    persistRecentFiles();
}

bool WorkspaceSessionSupport::hasRecentFiles() const
{
    for (int index = 0; index < recentFiles.getNumFiles(); ++index)
    {
        if (recentFiles.getFile(index).existsAsFile())
            return true;
    }

    return false;
}

bool WorkspaceSessionSupport::shouldRetainPlaylistAudio() const
{
    if (auto* settings = appProperties.getUserSettings())
        return settings->getBoolValue(retainPlaylistAudioKey, true);

    return true;
}

bool WorkspaceSessionSupport::shouldWarmRetainableAudioFile(const juce::File& file) const
{
    if (!file.existsAsFile() || !isRetainablePlaylistAudioFile(file))
        return false;

    if (file == getCurrentLoadedFile())
        return false;

    if (WaveformModel::hasPersistentStartupPreviewCache(file)
        || WaveformModel::hasPersistentPeakCache(file))
        return false;

    return retainedPlaylistAudio.find(makeRetainedAudioKey(file)) == retainedPlaylistAudio.end();
}

std::vector<juce::File> WorkspaceSessionSupport::collectWarmableFiles(const std::vector<juce::File>& candidateFiles,
                                                                      int maxCount) const
{
    std::vector<juce::File> filesToWarm;
    filesToWarm.reserve(candidateFiles.size());
    std::set<juce::String> seenKeys;

    for (const auto& file : candidateFiles)
    {
        if (!shouldWarmRetainableAudioFile(file))
            continue;

        const auto key = makeRetainedAudioKey(file);
        if (!seenKeys.insert(key).second)
            continue;

        filesToWarm.push_back(file);
        if (maxCount > 0 && static_cast<int>(filesToWarm.size()) >= maxCount)
            break;
    }

    return filesToWarm;
}

void WorkspaceSessionSupport::clearRetainedPlaylistAudio()
{
    retainedPlaylistAudio.clear();
}

void WorkspaceSessionSupport::discardRetainedAudioDocument(const juce::File& file)
{
    retainedPlaylistAudio.erase(makeRetainedAudioKey(file));
}

void WorkspaceSessionSupport::pruneRetainedPlaylistAudio()
{
    if (!shouldRetainPlaylistAudio())
    {
        clearRetainedPlaylistAudio();
        return;
    }

    std::set<juce::String> validKeys;
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        if (const auto* entry = playlistModel.getEntry(i); entry != nullptr && !entry->recovered && isRetainablePlaylistAudioFile(entry->file))
            validKeys.insert(makeRetainedAudioKey(entry->file));
    }

    for (auto it = retainedPlaylistAudio.begin(); it != retainedPlaylistAudio.end();)
    {
        if (validKeys.find(it->first) == validKeys.end())
            it = retainedPlaylistAudio.erase(it);
        else
            ++it;
    }
}

void WorkspaceSessionSupport::retainCurrentAudioDocumentIfNeeded()
{
    if (activeRecoveredEntry && isActiveRecoveredDocument(*activeRecoveredEntry))
    {
        const auto* entry = playlistModel.getEntry(playlistModel.indexOfEntry(*activeRecoveredEntry));
        auto snapshot = document.captureWorkspaceSnapshot();
        if (entry && entry->recovered && snapshot)
        {
            auto& retained = *entry->recovered;
            retained.acceptsCorePeaks = retained.acceptsCorePeaks && snapshot->state() == activeRecoveredCoreState;
            if (!retained.acceptsCorePeaks)
            {
                retained.coreState.reset();
                activeRecoveredCoreState.reset();
            }
            retained.image = std::move(snapshot);
            const auto peaks = waveformModel.getPeakCache();
            retained.peaks = peaks && peaks->state == retained.image->state()
                && peaks->audioRevision == retained.image->state()->audioRevision
                ? std::const_pointer_cast<WaveformModel::PeakCache>(peaks) : nullptr;
            const auto spectral = waveformModel.getSpectrogramCache();
            retained.spectrogram = spectral && spectral->state == retained.image->state()
                && spectral->audioRevision == retained.image->state()->audioRevision
                ? std::const_pointer_cast<WaveformModel::SpectrogramCache>(spectral) : nullptr;
        }
        return;
    }
    if (!shouldRetainPlaylistAudio() || document.hasUnsavedChanges() || !document.hasAudio())
        return;

    const auto info = document.getFileInfo();
    if (info.recoveryBacked || !info.isValid() || !isRetainablePlaylistAudioFile(info.file))
        return;

    auto snapshot = document.captureWorkspaceSnapshot();
    if (!snapshot)
        return;

    std::shared_ptr<WaveformModel::PeakCache> peakCache;
    if (const auto currentPeak = waveformModel.getPeakCache(); currentPeak != nullptr
        && currentPeak->state == snapshot->state()
        && currentPeak->audioRevision == snapshot->state()->audioRevision)
    {
        peakCache = std::const_pointer_cast<WaveformModel::PeakCache>(currentPeak);
    }

    std::shared_ptr<WaveformModel::SpectrogramCache> spectrogramCache;
    if (const auto currentSpectrogram = waveformModel.getSpectrogramCache(); currentSpectrogram != nullptr
        && currentSpectrogram->state == snapshot->state()
        && currentSpectrogram->audioRevision == snapshot->state()->audioRevision)
    {
        spectrogramCache = std::const_pointer_cast<WaveformModel::SpectrogramCache>(currentSpectrogram);
    }

    retainedPlaylistAudio[makeRetainedAudioKey(info.file)] = { std::move(snapshot), std::move(peakCache), std::move(spectrogramCache) };
}

bool WorkspaceSessionSupport::restoreRetainedAudioDocumentIfAvailable(const juce::File& file)
{
    if (!shouldRetainPlaylistAudio() || !isRetainablePlaylistAudioFile(file))
        return false;

    const auto it = retainedPlaylistAudio.find(makeRetainedAudioKey(file));
    if (it == retainedPlaylistAudio.end() || !it->second.document)
        return false;

    // Retention pairs caches with the exact captured State, never just its path.
    // The existing waveform handoff rebinds metadata, without copying peak LODs
    // or spectrogram images on the UI thread.
    waveformModel.prepareIncomingCaches(it->second.peakCache, it->second.spectrogramCache);

    juce::String error;
    if (!document.adoptWorkspaceSnapshot(*it->second.document, error))
    {
        waveformModel.prepareIncomingCaches({}, {});
        return false;
    }

    if (bnkDocument.hasBank())
        bnkDocument.clear();

    releaseRecoveredActivation();
    return true;
}

juce::File WorkspaceSessionSupport::audioActivationFile(const juce::File& target) const
{
    const auto state = document.getState();
    if (state && state->hasPlaybackSource() && !state->info.recoveryBacked
        && document.getFileInfo().file == target)
        return state->info.file;
    const auto retained = retainedPlaylistAudio.find(makeRetainedAudioKey(target));
    if (shouldRetainPlaylistAudio() && retained != retainedPlaylistAudio.end() && retained->second.document)
        return retained->second.document->state()->info.file;
    return target;
}

WorkspaceSessionSupport::AudioDocumentReuseResult WorkspaceSessionSupport::reuseAudioDocumentIfAvailable(const juce::File& file)
{
    // Reuse owns a real admitted reader/history image. A missing historical
    // locator affects fresh open and Save identity checks, not that ownership.
    const auto currentInfo = document.getFileInfo();
    if (document.hasAudio() && !currentInfo.recoveryBacked && currentInfo.isValid() && currentInfo.file == file
        && !(activeRecoveredEntry && isActiveRecoveredDocument(*activeRecoveredEntry)))
        return AudioDocumentReuseResult::alreadyCurrent;

    if (restoreRetainedAudioDocumentIfAvailable(file))
        return AudioDocumentReuseResult::restoredRetained;

    return AudioDocumentReuseResult::none;
}

bool WorkspaceSessionSupport::isActiveRecoveredDocument(const juce::Uuid& entryId) const
{
    return activeRecoveredEntry && *activeRecoveredEntry == entryId
        && document.getMediaIdentity().document.sameDocument(activeRecoveredIdentity);
}

void WorkspaceSessionSupport::releaseRecoveredActivation()
{
    activeRecoveredEntry.reset();
    activeRecoveredIdentity = {};
    activeRecoveredCoreState.reset();
}

std::vector<WorkspaceSessionSupport::UnsavedDocument> WorkspaceSessionSupport::collectUnsavedDocuments() const
{
    std::vector<UnsavedDocument> result;
    const auto active = activeRecoveredEntry && isActiveRecoveredDocument(*activeRecoveredEntry)
        ? *activeRecoveredEntry : juce::Uuid(nullptr);
    if (document.hasUnsavedChanges())
        result.push_back({ active, true, document.captureWorkspaceSnapshot() });
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        const auto* entry = playlistModel.getEntry(i);
        if (!entry || !entry->recovered || entry->id == active) continue;
        const auto snapshot = entry->recovered->snapshot();
        if (snapshot && snapshot->hasUnsavedChanges()) result.push_back({ entry->id, false, snapshot });
    }
    return result;
}

bool WorkspaceSessionSupport::restoreRecoveredDocument(const juce::Uuid& entryId, juce::String& error)
{
    const auto* entry = playlistModel.getEntry(playlistModel.indexOfEntry(entryId));
    if (!entry || !entry->recovered || !entry->recovered->image)
    {
        error = "The recovered playlist entry is no longer available.";
        return false;
    }
    auto retained = entry->recovered;
    retainCurrentAudioDocumentIfNeeded();
    waveformModel.prepareIncomingCaches(retained->peaks, retained->spectrogram);
    if (!document.adoptWorkspaceSnapshot(*retained->image, error))
    {
        waveformModel.prepareIncomingCaches({}, {});
        return false;
    }
    activeRecoveredEntry = entryId;
    activeRecoveredIdentity = document.getMediaIdentity().document;
    activeRecoveredCoreState = retained->acceptsCorePeaks ? document.getState() : nullptr;
    if (bnkDocument.hasBank()) bnkDocument.clear();
    return true;
}

bool WorkspaceSessionSupport::acceptRecoveredPeaks(const juce::Uuid& attempt,
    SingleTrackDocument::StatePtr originalCore, std::shared_ptr<WaveformModel::PeakCache> peaks)
{
    const auto* entry = playlistModel.getEntry(playlistModel.indexOfEntry(attempt));
    if (!entry || !entry->recovered || !entry->recovered->acceptsCorePeaks
        || entry->recovered->coreState != originalCore || !originalCore || !peaks
        || peaks->state != originalCore || peaks->audioRevision != originalCore->audioRevision
        || peaks->isPreview || peaks->binCount <= 0 || peaks->loadedSampleRange.getStart() != 0
        || peaks->loadedSampleRange.getEnd() != originalCore->info.numSamples
        || originalCore->getPlayableSampleRange().getEnd() != originalCore->info.numSamples)
        return false;
    if (isActiveRecoveredDocument(attempt))
    {
        const auto current = document.getState();
        if (!current || current != activeRecoveredCoreState) return false;
        peaks->state = current;
        peaks->audioRevision = current->audioRevision;
        waveformModel.adoptPeakCache(peaks);
    }
    entry->recovered->peaks = std::move(peaks);
    return true;
}

juce::File WorkspaceSessionSupport::getMappedBnkProjectRoot(const juce::File& bankFile) const
{
    if (!bankFile.existsAsFile())
        return {};

    if (auto* settings = appProperties.getUserSettings())
    {
        const auto path = settings->getValue(makeBnkProjectMappingKey(bankFile)).trim();
        const auto directory = juce::File(path);
        if (directory.isDirectory())
            return directory;

        const auto defaultProjectPath = settings->getValue(defaultWwiseProjectRootKey).trim();
        const auto defaultProjectDirectory = juce::File(defaultProjectPath);
        if (defaultProjectDirectory.isDirectory())
            return defaultProjectDirectory;
    }

    return {};
}

void WorkspaceSessionSupport::setMappedBnkProjectRoot(const juce::File& bankFile, const juce::File& projectRoot)
{
    if (!bankFile.existsAsFile() || !projectRoot.isDirectory())
        return;

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue(makeBnkProjectMappingKey(bankFile), projectRoot.getFullPathName());
        settings->saveIfNeeded();
    }
}

juce::File WorkspaceSessionSupport::getDefaultBrowseDirectory() const
{
    if (const auto currentFile = getCurrentLoadedFile(); currentFile.existsAsFile())
        return currentFile.getParentDirectory();

    if (pendingLastBrowseDirectory.isNotEmpty())
    {
        const auto pendingDirectory = juce::File(pendingLastBrowseDirectory);
        if (pendingDirectory.isDirectory())
            return pendingDirectory;
    }

    if (auto* settings = appProperties.getUserSettings())
    {
        const auto path = settings->getValue(lastBrowseDirectoryKey);
        const auto directory = juce::File(path);
        if (directory.isDirectory())
            return directory;
    }

    return juce::File::getSpecialLocation(juce::File::userMusicDirectory);
}

juce::File WorkspaceSessionSupport::getCurrentLoadedFile() const
{
    if (const auto bankFile = bnkDocument.getBankInfo().file; bankFile.existsAsFile())
        return bankFile;

    if (const auto audioFile = document.getFileInfo().file; audioFile.existsAsFile())
        return audioFile;

    return {};
}

juce::String WorkspaceSessionSupport::serialisePlaylistFiles(const PlaylistModel& playlistModel)
{
    juce::StringArray paths;
    for (int index = 0; index < playlistModel.getNumEntries(); ++index)
    {
        if (const auto* entry = playlistModel.getEntry(index); entry != nullptr && !entry->recovered && entry->file.existsAsFile())
            paths.add(entry->file.getFullPathName());
    }

    return paths.joinIntoString("\n");
}

int WorkspaceSessionSupport::serialisedPlaylistIndex(const PlaylistModel& playlist)
{
    int persistedIndex = 0;
    for (int i = 0; i < playlist.getNumEntries(); ++i)
    {
        const auto* entry = playlist.getEntry(i);
        if (i == playlist.getCurrentIndex())
            return entry && !entry->recovered && entry->file.existsAsFile() ? persistedIndex : -1;
        if (entry && !entry->recovered && entry->file.existsAsFile()) ++persistedIndex;
    }
    return -1;
}

std::vector<juce::File> WorkspaceSessionSupport::deserialisePlaylistFiles(const juce::String& serialised)
{
    std::vector<juce::File> files;
    if (serialised.isEmpty())
        return files;

    const auto lines = juce::StringArray::fromLines(serialised);
    files.reserve(static_cast<std::size_t>(lines.size()));
    for (const auto& line : lines)
    {
        auto file = juce::File(line.trim());
        if (file.existsAsFile())
            files.push_back(file);
    }

    return files;
}

juce::String WorkspaceSessionSupport::makeRetainedAudioKey(const juce::File& file)
{
    return file.getFullPathName().toLowerCase();
}

bool WorkspaceSessionSupport::isRetainablePlaylistAudioFile(const juce::File& file) noexcept
{
    const auto lower = file.getFileExtension().toLowerCase();
    return lower == ".wav" || lower == ".flac" || lower == ".aiff" || lower == ".aif" || lower == ".ogg" || lower == ".mp3";
}

juce::String WorkspaceSessionSupport::makeBnkProjectMappingKey(const juce::File& bankFile)
{
    const auto folderPath = bankFile.getParentDirectory().getFullPathName();
    return "bnkProjectMap_" + juce::String::toHexString(static_cast<juce::int64>(folderPath.hashCode64()));
}
} // namespace singleeditor
