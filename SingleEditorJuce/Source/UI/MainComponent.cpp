#include "UI/MainComponent.h"
#include "UI/Components/AsioSettingsComponent.h"
#include "Bnk/BnkStateSelection.h"
#include "Audio/VgmstreamDecodeService.h"
#include "Document/RecoveredPlaylistDocument.h"
#include "Recovery/RecoveryService.h"
#include "UI/Commands/NativeFocusTrace.h"
#include "App/ShutdownDiagnostics.h"
#include "UI/Components/SelectionPopupLayout.h"
#include "UI/Components/SaveEncodingOptionsDialog.h"
#include "UI/Components/RecoveryCenterComponent.h"
#include "UI/Components/LocalDiagnosticsComponent.h"

#include "App/CoreOpenProbe.h"
#include "App/CoreOpenCycleProbe.h"
#include <algorithm>
#include <array>
#include <set>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#include "Audio/WindowsMediaAudioService.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Components/ModernTransportLayout.h"
#include "UI/Components/TimelineMarkersComponent.h"
#include "UI/Workspace/WorkspaceCanvasAuthority.h"
#include "UI/Workspace/WorkspaceModuleRuntimePolicy.h"
#include "UI/Workspace/ModernWorkspaceScene.h"

#ifndef SINGLE_EDITOR_ENABLE_OPEN_TIMING
#define SINGLE_EDITOR_ENABLE_OPEN_TIMING 1
#endif

namespace singleeditor
{
namespace
{
juce::String recoverySourceAdvice(const recovery::RecoverySessionHost::Health& health)
{
    bool unavailable = false, changed = false;
    const auto inspect = [&](const recovery::Status& failure)
    {
        if (failure) return;
        unavailable = unavailable || failure.code == "source_lease_unavailable"
            || failure.code == "audio_source_read_failed";
        changed = changed || (failure.error == recovery::Error::sourceConflict
            && (failure.code.startsWith("audio_source_") || failure.code == "audio_retained_input_changed"));
    };
    for (const auto& session : health.sessions)
    {
        inspect(session.protection.current.failure);
        for (const auto& epoch : session.protection.retired) inspect(epoch.failure);
    }
    if (!unavailable && !changed) return {};
    const juce::String reason = changed
        ? "An audio source needed by recovery has changed. Retry cannot accept a different source version. "
        : "An audio source needed by recovery is unavailable or busy. Retry can help once the same source is accessible again. ";
    return "\n\n" + reason + "Save As writes the current audio; complete Undo/Redo history still needs recovery protection. Existing recovery points are kept.";
}

juce::String recoveryCapacityAdvice(const recovery::RecoveryService::Health& health)
{
    recovery::ByteCapacity observed;
    const auto inspect = [&](const recovery::Status& status)
    {
        if (observed.resource == recovery::ByteCapacity::Resource::none
            && status.error == recovery::Error::budgetExceeded) observed = status.capacity;
    };
    // Prefer the current document's refusal. These are captured gate results;
    // opening the dialog does not scan files or recalculate capacity on the UI.
    for (const auto& session : health.protection.sessions)
        if (session.active) { inspect(session.protection.current.failure); inspect(session.protection.failure); }
    inspect(health.protection.failure);
    inspect(health.maintenance.failure);
    inspect(health.dispositionFailure);
    for (const auto& session : health.protection.sessions)
    {
        inspect(session.protection.current.failure);
        for (const auto& epoch : session.protection.retired) inspect(epoch.failure);
    }
    using Resource = recovery::ByteCapacity::Resource;
    if (observed.resource == Resource::none) return {};
    const auto amount = [](uint64_t bytes)
    { return juce::String(static_cast<double>(bytes) / (1024.0 * 1024.0), 2) + " MiB"; };
    const bool memory = observed.resource == Resource::retainedMemory;
    const bool graph = observed.resource == Resource::audioGraph;
    const bool disk = observed.resource == Resource::freeDisk;
    const juce::String name = memory ? "Recovery queue memory" : graph ? "Recovery audio capacity"
        : disk ? "Free disk space (including reserve)"
        : observed.resource == Resource::ownerStorage ? "This document's recovery storage"
        : observed.resource == Resource::globalStorage ? "Total recovery storage" : "Recovery storage category";
    juce::String message = "\n\n" + name + ": " + (observed.lowerBound ? "at least " : "")
        + amount(observed.required) + " required; " + (disk ? "available at failure: " : "current limit: ")
        + amount(observed.limit) + ".";
    if (observed.required > observed.limit)
        message += " Shortfall: " + juce::String(observed.lowerBound ? "at least " : "")
            + amount(observed.required - observed.limit) + ".";
    message += graph
        ? " The complete current audio and Undo/Redo graph exceed this recovery limit. Freeing disk space does not raise this limit. Existing recovery points are kept."
        : memory
        ? " This includes audio held for Undo/Redo. Retry after pending recovery work finishes; a document above this limit remains unprotected. Freeing disk space does not raise this memory limit."
        : " Open Recovery Center > Storage to refresh usage, or review records for cleanup, then Retry. Existing recovery points are kept.";
    message += " Save As writes current audio; it does not save the complete Undo/Redo history.";
    return message;
}

juce::File recoveryRootDirectory()
{
#if JUCE_WINDOWS
    const auto local = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    return juce::File::isAbsolutePath(local) ? juce::File(local).getChildFile("SingleEditor/Recovery") : juce::File();
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("SingleEditor/Recovery");
#endif
}
namespace commandIds
{
constexpr std::string_view open = "document.open";
constexpr std::string_view save = "document.save";
constexpr std::string_view saveAs = "document.save-as";
constexpr std::string_view exportAudio = "document.export-audio";
constexpr std::string_view undo = "document.undo";
constexpr std::string_view redo = "document.redo";
constexpr std::string_view deleteSelection = "document.selection.delete";
constexpr std::string_view fadeIn = "document.selection.fade-in";
constexpr std::string_view fadeOut = "document.selection.fade-out";
constexpr std::string_view resetEnvelope = "document.envelope.reset";
constexpr std::string_view preferences = "application.preferences";
constexpr std::string_view close = "application.close";
constexpr std::string_view palette = "application.command-palette.open";
constexpr std::string_view about = "application.about";
constexpr std::string_view recoveryCenter = "application.recovery-center";
constexpr std::string_view diagnostics = "application.local-diagnostics";
constexpr std::string_view minimize = "application.window.minimize";
constexpr std::string_view maximize = "application.window.toggle-maximize";
constexpr std::string_view togglePlayback = "core.transport.toggle-playback";
constexpr std::string_view play = "core.transport.play";
constexpr std::string_view pause = "core.transport.pause";
constexpr std::string_view stop = "core.transport.stop";
constexpr std::string_view start = "core.transport.start";
constexpr std::string_view previousFrame = "core.transport.previous-frame";
constexpr std::string_view nextFrame = "core.transport.next-frame";
constexpr std::string_view returnToStart = "core.transport.return-to-start";
constexpr std::string_view loop = "core.transport.loop";
constexpr std::string_view autoPlay = "core.transport.auto-play";
constexpr std::string_view previousPlaylist = "document.playlist.previous";
constexpr std::string_view nextPlaylist = "document.playlist.next";
constexpr std::string_view removePlaylist = "document.playlist.remove-current";
constexpr std::string_view waveform = "core.waveform.view";
constexpr std::string_view spectral = "core.waveform.spectral";
constexpr std::string_view stacked = "core.waveform.channels.stacked";
constexpr std::string_view mixed = "core.waveform.channels.mixed";
constexpr std::string_view zoomTimeOut = "core.waveform.zoom-time-out";
constexpr std::string_view zoomTimeIn = "core.waveform.zoom-time-in";
constexpr std::string_view zoomAmplitudeOut = "core.waveform.zoom-amplitude-out";
constexpr std::string_view zoomAmplitudeIn = "core.waveform.zoom-amplitude-in";
constexpr std::string_view resetView = "core.waveform.reset-view";
constexpr std::string_view togglePlaylistSidebar = "workspace.sidebar.playlist.toggle";
constexpr std::string_view toggleMetadataSidebar = "workspace.sidebar.metadata.toggle";
constexpr std::string_view toggleOutputMeter = "workspace.module.output-meter.toggle";
constexpr std::string_view toggleAmbisonicEnergy = "workspace.module.ambisonic-energy.toggle";
constexpr std::string_view toggleSpatialEnergy = "workspace.module.spatial-energy.toggle";
constexpr std::string_view floatSpatialEnergy = "workspace.module.spatial-energy.float";
constexpr std::string_view redockSpatialEnergy = "workspace.module.spatial-energy.redock";
constexpr std::string_view removeSpatialEnergy = "workspace.module.spatial-energy.remove";
constexpr std::string_view toggleParametricEq = "workspace.module.parametric-eq.toggle";
constexpr std::string_view generalScene = "workspace.scene.general";
constexpr std::string_view simpleView = "workspace.view.simple";
constexpr std::string_view toggleTruePeakLimiter = "workspace.module.true-peak-limiter.toggle";
constexpr std::string_view floatWaveform = "workspace.module.core-waveform.float";
constexpr std::string_view redockWaveform = "workspace.module.core-waveform.redock";
constexpr std::string_view floatOutputMeter = "workspace.module.output-meter.float";
constexpr std::string_view redockOutputMeter = "workspace.module.output-meter.redock";
constexpr std::string_view removeOutputMeter = "workspace.module.output-meter.remove";
constexpr std::string_view floatAmbisonicEnergy = "workspace.module.ambisonic-energy.float";
constexpr std::string_view redockAmbisonicEnergy = "workspace.module.ambisonic-energy.redock";
constexpr std::string_view removeAmbisonicEnergy = "workspace.module.ambisonic-energy.remove";
constexpr std::string_view floatParametricEq = "workspace.module.parametric-eq.float";
constexpr std::string_view redockParametricEq = "workspace.module.parametric-eq.redock";
constexpr std::string_view removeParametricEq = "workspace.module.parametric-eq.remove";
constexpr std::string_view floatTruePeakLimiter = "workspace.module.true-peak-limiter.float";
constexpr std::string_view redockTruePeakLimiter = "workspace.module.true-peak-limiter.redock";
constexpr std::string_view removeTruePeakLimiter = "workspace.module.true-peak-limiter.remove";
constexpr std::string_view floatPlaylistMag = "workspace.module.playlist.float";
constexpr std::string_view redockPlaylistMag = "workspace.module.playlist.redock";
constexpr std::string_view removePlaylistMag = "workspace.module.playlist.remove";
constexpr std::string_view floatMetadataMag = "workspace.module.metadata.float";
constexpr std::string_view redockMetadataMag = "workspace.module.metadata.redock";
constexpr std::string_view removeMetadataMag = "workspace.module.metadata.remove";
constexpr std::string_view toggleMarkers = "workspace.module.markers.toggle";
constexpr std::string_view floatMarkers = "workspace.module.markers.float";
constexpr std::string_view redockMarkers = "workspace.module.markers.redock";
constexpr std::string_view removeMarkers = "workspace.module.markers.remove";
constexpr std::string_view resetModuleLayout = "workspace.layout.reset";
}

std::optional<WorkspaceModuleId> sidebarModuleForWindowCommand(std::string_view commandId)
{
    if (commandId == commandIds::floatPlaylistMag || commandId == commandIds::redockPlaylistMag
        || commandId == commandIds::removePlaylistMag)
        return WorkspaceModuleId::playlist;
    if (commandId == commandIds::floatMetadataMag || commandId == commandIds::redockMetadataMag
        || commandId == commandIds::removeMetadataMag)
        return WorkspaceModuleId::metadata;
    if (commandId == commandIds::floatMarkers || commandId == commandIds::redockMarkers
        || commandId == commandIds::removeMarkers)
        return WorkspaceModuleId::markers;
    return std::nullopt;
}

std::string_view removeCommandForModule(WorkspaceModuleId id)
{
    switch (id)
    {
        case WorkspaceModuleId::outputMeter: return commandIds::removeOutputMeter;
        case WorkspaceModuleId::ambisonicEnergy: return commandIds::removeAmbisonicEnergy;
        case WorkspaceModuleId::spatialEnergy: return commandIds::removeSpatialEnergy;
        case WorkspaceModuleId::parametricEq: return commandIds::removeParametricEq;
        case WorkspaceModuleId::truePeakLimiter: return commandIds::removeTruePeakLimiter;
        case WorkspaceModuleId::playlist: return commandIds::removePlaylistMag;
        case WorkspaceModuleId::metadata: return commandIds::removeMetadataMag;
        case WorkspaceModuleId::markers: return commandIds::removeMarkers;
        case WorkspaceModuleId::coreWaveform: break;
    }
    return {};
}

bool lifecycleTransitionAccepted(LifecycleTransitionResult result) noexcept
{
    return result == LifecycleTransitionResult::applied
        || result == LifecycleTransitionResult::unchanged;
}

constexpr auto playlistSidebarCollapsedKey = "playlistSidebarCollapsed";
constexpr auto metadataSidebarCollapsedKey = "metadataSidebarCollapsed";
constexpr auto playlistSidebarWidthKey = "playlistSidebarWidth";
constexpr auto metadataSidebarWidthKey = "metadataSidebarWidth";
constexpr auto waveformViewModeKey = "waveformViewMode";
constexpr auto waveformDisplayModeKey = "waveformDisplayMode";
constexpr auto meterModeKey = "meterMode";
constexpr auto monitorLevelKey = "monitorLevel";
constexpr auto returnToStartKey = "returnToStartOnStop";
constexpr auto loopKey = "loopEnabled";
constexpr auto autoPlayKey = "autoPlayEnabled";
constexpr auto restorePlaylistSessionKey = "restorePlaylistSession";
constexpr auto playlistSessionFilesKey = "playlistSessionFiles";
constexpr auto playlistSessionIndexKey = "playlistSessionIndex";
constexpr auto workspaceModuleLayoutKey = "workspaceModuleLayoutV1";
constexpr auto workspaceModuleLayoutV2Key = "workspaceModuleLayoutV2";
constexpr auto workspacePresetCatalogKey = "workspacePresetCatalogV1";
constexpr auto mp3SaveBitrateKey = "saveMp3BitrateKbps";
constexpr auto oggSaveQualityKey = "saveOggQualityIndex";
constexpr auto flacSaveCompressionKey = "saveFlacCompressionLevel";
constexpr int maxRecentFiles = 16;
constexpr auto openTimingLogFileName = "open_timing.log";
constexpr int normalSessionSettingsPersistDelayMs = 750;
constexpr int startupSessionSettingsPersistDelayMs = 2600;

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
juce::File getOpenTimingLogFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SingleEditor");
    directory.createDirectory();
    return directory.getChildFile(openTimingLogFileName);
}

juce::File getPortableOpenTimingLogFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    if (directory != juce::File())
        return directory.getChildFile(openTimingLogFileName);

    return {};
}

juce::String formatTimingField(const char* label, const std::optional<double>& value)
{
    return juce::String(label) + " "
         + (value.has_value() ? juce::String(juce::roundToInt(*value)) + " ms"
                              : juce::String("--"));
}

juce::CriticalSection& getOpenTimingLogLock()
{
    static juce::CriticalSection lock;
    return lock;
}

juce::StringArray& getPendingOpenTimingLogLines()
{
    static juce::StringArray pendingLines;
    return pendingLines;
}

bool shouldFlushOpenTimingLogBufferForLine(const juce::String& line)
{
    return line.contains("outcome=primary-content")
        || line.contains("outcome=ui-mounted")
        || line.contains("outcome=window-shown")
        || line.contains("outcome=file-name")
        || line.contains("outcome=load-complete")
        || line.contains("outcome=full-cache-polled")
        || line.contains("outcome=full-cache-published-ui")
        || line.contains("outcome=playable")
        || line.contains("outcome=first-waveform")
        || line.contains("outcome=full-waveform")
        || line.contains("outcome=failed")
        || line.contains("outcome=bnk-fallback")
        || line.contains("outcome=component-destroyed")
        || line.contains("outcome=superseded");
}

void appendTimingLinesToFile(const juce::File& targetFile, const juce::StringArray& lines)
{
    if (targetFile == juce::File() || lines.isEmpty())
        return;

    if (auto parent = targetFile.getParentDirectory(); parent != juce::File())
        parent.createDirectory();

    juce::FileOutputStream stream(targetFile);
    if (!stream.openedOk())
        return;

    if (targetFile.existsAsFile())
        stream.setPosition(targetFile.getSize());

    stream.writeText(lines.joinIntoString(juce::newLine) + juce::newLine, false, false, nullptr);
    stream.flush();
}

void flushPendingOpenTimingLogLines()
{
    juce::StringArray linesToFlush;
    {
        const juce::ScopedLock lock(getOpenTimingLogLock());
        auto& pendingLines = getPendingOpenTimingLogLines();
        if (pendingLines.isEmpty())
            return;

        linesToFlush.swapWith(pendingLines);
    }

    appendTimingLinesToFile(getOpenTimingLogFile(), linesToFlush);
    appendTimingLinesToFile(getPortableOpenTimingLogFile(), linesToFlush);
}

void appendOpenTimingLogLine(const juce::String& line)
{
    auto shouldFlush = false;
    {
        const juce::ScopedLock lock(getOpenTimingLogLock());
        auto& pendingLines = getPendingOpenTimingLogLines();
        pendingLines.add(line);
        shouldFlush = pendingLines.size() >= 24 || shouldFlushOpenTimingLogBufferForLine(line);
    }

    if (shouldFlush)
        flushPendingOpenTimingLogLines();
}
#else
void appendOpenTimingLogLine(const juce::String&)
{
}
#endif

void configureLabel(juce::Label& label, float fontHeight, juce::Justification justification, juce::Colour colour)
{
    label.setFont(juce::Font(juce::FontOptions(fontHeight)));
    label.setJustificationType(justification);
    label.setColour(juce::Label::textColourId, colour);
}

void configureTransportButton(juce::Button& button, const juce::String& text)
{
    button.setButtonText(text);
    button.setEnabled(false);
    button.getProperties().set("ghostButton", true);
}

void configureTransportToggle(juce::Button& button, const juce::String& text, bool defaultState)
{
    button.setButtonText(text);
    button.setClickingTogglesState(true);
    button.setToggleState(defaultState, juce::dontSendNotification);
    button.getProperties().set("ghostButton", true);
}

void configureActionButton(juce::Button& button, const juce::String& text)
{
    button.setButtonText(text);
    button.setEnabled(false);
    button.getProperties().set("ghostButton", true);
}

bool fileHasAnyExtension(const juce::File& file, std::initializer_list<const char*> extensions)
{
    const auto lower = file.getFileExtension().toLowerCase();
    for (const auto* ext : extensions)
        if (lower == ext)
            return true;

    return false;
}

bool fileInfoMatches(const SingleTrackDocument::FileInfo& lhs,
                     const SingleTrackDocument::FileInfo& rhs)
{
    return lhs.isValid()
        && rhs.isValid()
        && lhs.file == rhs.file
        && lhs.numChannels == rhs.numChannels
        && lhs.numSamples == rhs.numSamples
        && std::abs(lhs.sampleRate - rhs.sampleRate) < 0.0001
        && std::abs(lhs.durationSeconds - rhs.durationSeconds) < 0.0001;
}

void setButtonIcon(ModernIconButton& button,
                   RendererAssetId assetId,
                   const juce::String& tooltip = {})
{
    const auto iconFile = juce::String(RendererVisualStyle::assetToken(assetId).fileName);
    button.setIconName(iconFile);
    button.setIconPadding(4);
    button.setNominalIconSize(15);
    button.getProperties().set("iconName", iconFile);
    button.getProperties().set("iconOnly", true);
    button.getProperties().set("ghostButton", true);
    if (tooltip.isNotEmpty())
        button.setTooltip(tooltip);
    else if (button.getButtonText().isNotEmpty())
        button.setTooltip(button.getButtonText());
}

class AsyncFileLoadJob final : public juce::ThreadPoolJob
{
public:
    AsyncFileLoadJob(juce::Component::SafePointer<MainComponent> ownerToUse,
                     juce::File fileToLoad,
                     int64_t tokenToUse,
                     media::OptionalMediaOperation::Ptr operationToUse = {})
        : juce::ThreadPoolJob("Document File Load"),
          owner(ownerToUse),
          file(std::move(fileToLoad)),
          token(tokenToUse), optionalOperation(std::move(operationToUse))
    {
    }

    ~AsyncFileLoadJob() override
    {
        // A removed queued job never obtained decoder authority. Its owner may
        // still be waiting for close cancellation, so settle that inert request.
        if (optionalOperation && !started.load(std::memory_order_acquire)) optionalOperation->cancelled();
    }

    JobStatus runJob() override
    {
        started.store(true, std::memory_order_release);
        // A failed optional video must not publish cached metadata/peaks over
        // the current working Document before its candidate is accepted.
        auto startupPreview = optionalOperation ? decltype(WaveformModel::loadPersistentStartupPreviewForFile(file)) {}
                                               : WaveformModel::loadPersistentStartupPreviewForFile(file);
        auto cachedHeader = startupPreview.has_value()
                                ? startupPreview
                                : optionalOperation ? decltype(startupPreview) {}
                                                    : WaveformModel::lookupPersistentPeakCacheHeaderForFile(file);
        auto dispatchedCachedPreview = false;
        if (startupPreview.has_value() && startupPreview->info.isValid() && startupPreview->cache != nullptr)
        {
            dispatchedCachedPreview = true;
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            info = startupPreview->info,
                                            peakCache = startupPreview->cache]
            {
                if (owner != nullptr)
                {
                    owner->showAsyncLoadMetadata(token, info);
                    owner->showAsyncLoadPreview(token, info, peakCache);
                }
            });
        }

        auto preparedFile = SingleTrackDocument::prepareAudioFileForOpen(file, optionalOperation);
        auto info = cachedHeader.has_value() ? cachedHeader->info
                                             : preparedFile.info;
        auto peakCache = startupPreview.has_value() ? startupPreview->cache
                                                    : std::shared_ptr<WaveformModel::PeakCache> {};

        if (info.isValid() && peakCache != nullptr && !dispatchedCachedPreview)
        {
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            info,
                                            peakCache]
            {
                if (owner != nullptr)
                {
                    owner->showAsyncLoadMetadata(token, info);
                    owner->showAsyncLoadPreview(token, info, peakCache);
                }
            });
        }
        else if (info.isValid() && !optionalOperation)
        {
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            info]
            {
                if (owner != nullptr)
                    owner->showAsyncLoadMetadata(token, info);
            });
        }

        if (!preparedFile.hasReader())
        {
            if (optionalOperation) optionalOperation->fail(media::OptionalMediaFailureState::Reason::cacheInvalid,
                media::OptionalMediaFailureState::Stage::cache);
            auto errorMessage = preparedFile.errorMessage;
            // This is classification only, after the ordinary reader failed.
            // No bank table/sidecar parse or admission runs in this old job.
            bool bankSignature = false;
            if (!shouldExit() && !optionalOperation && !WindowsMediaAudioService::isSupportedVideoExtension(file.getFileExtension()))
            {
                juce::FileInputStream signature(file);
                char bytes[4] {};
                bankSignature = signature.openedOk() && signature.read(bytes, 4) == 4
                    && bytes[0] == 'B' && bytes[1] == 'K' && bytes[2] == 'H' && bytes[3] == 'D';
            }
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            file = file,
                                            errorMessage = std::move(errorMessage),
                                            bankSignature, optionalOperation = optionalOperation]() mutable
            {
                if (owner != nullptr)
                    owner->completeAsyncLoad(token,
                                             file,
                                             {},
                                             {},
                                             bankSignature,
                                             std::move(errorMessage), std::move(optionalOperation));
            });
            return jobHasFinished;
        }

        if (!info.isValid())
        {
            info = preparedFile.info;
            if (info.isValid() && !optionalOperation)
            {
                juce::MessageManager::callAsync([owner = owner,
                                                token = token,
                                                info]
                {
                    if (owner != nullptr)
                        owner->showAsyncLoadMetadata(token, info);
                });
            }
        }

        auto preparedErrorMessage = preparedFile.errorMessage;
        auto state = SingleTrackDocument::createStateFromPreparedFile(std::move(preparedFile));
        if (state == nullptr)
        {
            if (optionalOperation) optionalOperation->fail(media::OptionalMediaFailureState::Reason::cacheInvalid,
                media::OptionalMediaFailureState::Stage::cache);
            auto errorMessage = std::move(preparedErrorMessage);
            if (errorMessage.isEmpty())
                errorMessage = "The prepared audio state could not be adopted.";
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            file = file,
                                            errorMessage = std::move(errorMessage), optionalOperation = optionalOperation]() mutable
            {
                if (owner != nullptr)
                    owner->completeAsyncLoad(token, file, {}, {}, {}, std::move(errorMessage), std::move(optionalOperation));
            });
            return jobHasFinished;
        }

        if (peakCache == nullptr && info.isValid())
        {
            peakCache = WaveformModel::buildLeadingPreviewPeakCacheForState(state,
                                                                            file.hasFileExtension(".wem") ? 192 : 384,
                                                                            file.hasFileExtension(".wem") ? 1.0 : 1.5);
        }

        if (peakCache != nullptr && !startupPreview.has_value() && !optionalOperation)
        {
            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            info,
                                            peakCache]
            {
                if (owner != nullptr)
                    owner->showAsyncLoadPreview(token, info, peakCache);
            });
        }

        auto initialPeakCache = peakCache;
        if (optionalOperation) optionalOperation->succeed();
        juce::MessageManager::callAsync([owner = owner,
                                        token = token,
                                        file = file,
                                        state,
                                        peakCache = std::move(initialPeakCache), optionalOperation = optionalOperation]() mutable
        {
            if (owner != nullptr)
                owner->completeAsyncLoad(token, file, std::move(state), std::move(peakCache), {}, {}, std::move(optionalOperation));
        });

        return jobHasFinished;
    }

private:
    juce::Component::SafePointer<MainComponent> owner;
    juce::File file;
    int64_t token = 0;
    media::OptionalMediaOperation::Ptr optionalOperation;
    std::atomic<bool> started { false };
};

class AsyncCacheWarmJob final : public juce::ThreadPoolJob
{
public:
    explicit AsyncCacheWarmJob(std::vector<juce::File> filesToWarm)
        : juce::ThreadPoolJob("Waveform Cache Warm"),
          files(std::move(filesToWarm))
    {
    }

    JobStatus runJob() override
    {
        for (const auto& file : files)
        {
            if (shouldExit())
                break;

            WaveformModel::ensurePersistentPreviewCache(file, 768);
        }

        return jobHasFinished;
    }

private:
    std::vector<juce::File> files;
};
} // namespace

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
namespace
{
// This base dies after juce::Thread and all derived captures. It observes the
// existing join; it neither extends the deadline nor keeps the task alive.
class OptionalApplyTaskObservation
{
public:
    void observeLifetime(OptionalLifecycleToken lifetime) noexcept { observedTask = std::move(lifetime); }

protected:
    ~OptionalApplyTaskObservation()
    {
        if (!observedTask.resourceId()) return;
        if (observedJoin) observedTask.release();
        else observedTask.creationContext().observer->fail(OptionalLifecycleFailure::missingTerminal);
    }
    struct RunScope
    {
        explicit RunScope(const OptionalLifecycleToken& tokenToUse) noexcept : token(tokenToUse)
        { token.note(OptionalLifecycleAction::taskStarted); }
        ~RunScope() { token.note(OptionalLifecycleAction::taskCompleted); }
        const OptionalLifecycleToken& token;
    };
    void observeOutcome(const AudioSuiteApplyResult& result) const noexcept
    {
        observedTask.note(result.status == AudioSuiteApplyStatus::applied ? OptionalLifecycleAction::taskSucceeded
            : result.status == AudioSuiteApplyStatus::cancelled ? OptionalLifecycleAction::taskCancelled
            : OptionalLifecycleAction::taskRejected);
    }
    struct CallbackObservation
    {
        OptionalLifecycleToken token;
        std::atomic<bool> invoked { false };
        ~CallbackObservation()
        {
            if (!invoked.load(std::memory_order_acquire)) token.note(OptionalLifecycleAction::callbackDiscarded);
            token.release();
        }
    };
    struct ObservedCompletion
    {
        // Last-copy metadata dies after the actual completion/plan captures.
        std::shared_ptr<CallbackObservation> observation;
        std::function<void(AudioSuiteApplyResult)> completion;
        AudioSuiteApplyResult result;
        void operator()()
        {
            if (observation->invoked.exchange(true, std::memory_order_acq_rel))
                observation->token.creationContext().observer->fail(OptionalLifecycleFailure::invalidTransition);
            observation->token.note(OptionalLifecycleAction::callbackInvoked);
            completion(result);
        }
    };
    bool postObservedCompletion(std::function<void(AudioSuiteApplyResult)>&& completion,
                                AudioSuiteApplyResult result)
    {
        if (!observedTask.resourceId()) return false;
        auto lifetime = OptionalLifecycleToken::request(observedTask.creationContext(), observedTask.resourceOwner(),
            OptionalLifecycleResourceKind::completionCallback, observedTask.resourceId());
        std::shared_ptr<CallbackObservation> observation;
        try { observation = std::make_shared<CallbackObservation>(); }
        catch (...) { lifetime.failed(); throw; }
        lifetime.created();
        observation->token = std::move(lifetime);
        observation->token.note(OptionalLifecycleAction::callbackPostRequested);
        juce::MessageManager::callAsync(ObservedCompletion { std::move(observation), std::move(completion), result });
        return true;
    }
    OptionalLifecycleToken observedTask;
    bool observedJoin = false;
};
}
#endif

class MainComponent::ReferenceAudioSuiteApplyThread final
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    : public OptionalApplyTaskObservation, public juce::Thread
#else
    : public juce::Thread
#endif
{
public:
    using Completion = std::function<void(AudioSuiteReferenceProcessor::ApplyResult)>;

    ReferenceAudioSuiteApplyThread(SingleTrackDocument& documentToUse,
                                   AudioSuitePlanPtr planToUse,
                                   int64_t documentGenerationToUse,
                                   Completion completionToUse)
        : juce::Thread("AudioSuite Apply"),
          document(documentToUse),
          plan(std::move(planToUse)),
          documentGeneration(documentGenerationToUse),
          completion(std::move(completionToUse))
    {
    }

    ~ReferenceAudioSuiteApplyThread() override
    {
        cancel();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedJoin = stopThread(8000);
#else
        stopThread(8000);
#endif
    }

    void cancel() noexcept
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedTask.note(OptionalLifecycleAction::cancelRequested);
#endif
        cancellation.cancel();
        signalThreadShouldExit();
    }

    void run() override
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const RunScope runObservation(observedTask);
        const ScopedOptionalLifecycleContext workerContext(observedTask.creationContext());
#endif
        auto processor = AudioSuiteReferenceProcessor::describe().create();
        auto result = processor != nullptr
            ? processor->apply(document,
                               plan,
                               documentGeneration,
                               cancellation)
            : AudioSuiteReferenceProcessor::ApplyResult {};

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observeOutcome(result);
#endif
        if (completion)
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (postObservedCompletion(std::move(completion), result)) return;
#endif
            juce::MessageManager::callAsync(
                [completionToCall = std::move(completion), result]() mutable
                {
                    completionToCall(result);
                });
        }
    }

private:
    SingleTrackDocument& document;
    AudioSuitePlanPtr plan;
    int64_t documentGeneration = 0;
    AudioSuiteCancellationToken cancellation;
    Completion completion;
};

class MainComponent::ParametricEqApplyThread final
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    : public OptionalApplyTaskObservation, public juce::Thread
#else
    : public juce::Thread
#endif
{
public:
    using Completion = std::function<void(AudioSuiteApplyResult)>;

    ParametricEqApplyThread(SingleTrackDocument& documentToUse,
                            AudioSuitePlanPtr planToUse,
                            int64_t documentGenerationToUse,
                            Completion completionToUse)
        : juce::Thread("Parametric EQ Apply"),
          document(documentToUse),
          plan(std::move(planToUse)),
          documentGeneration(documentGenerationToUse),
          completion(std::move(completionToUse))
    {
    }

    ~ParametricEqApplyThread() override
    {
        cancel();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedJoin = stopThread(8000);
#else
        stopThread(8000);
#endif
    }

    void cancel() noexcept
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedTask.note(OptionalLifecycleAction::cancelRequested);
#endif
        cancellation.cancel();
        signalThreadShouldExit();
    }

    void run() override
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const RunScope runObservation(observedTask);
        const ScopedOptionalLifecycleContext workerContext(observedTask.creationContext());
#endif
        const auto result = AudioSuiteTransactionExecutor::apply(document,
                                                                 plan,
                                                                 documentGeneration,
                                                                 cancellation);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observeOutcome(result);
#endif
        if (completion)
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (postObservedCompletion(std::move(completion), result)) return;
#endif
            juce::MessageManager::callAsync(
                [completionToCall = std::move(completion), result]() mutable
                {
                    completionToCall(result);
                });
        }
    }

private:
    SingleTrackDocument& document;
    AudioSuitePlanPtr plan;
    int64_t documentGeneration = 0;
    AudioSuiteCancellationToken cancellation;
    Completion completion;
};

class MainComponent::TruePeakLimiterApplyThread final
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    : public OptionalApplyTaskObservation, public juce::Thread
#else
    : public juce::Thread
#endif
{
public:
    using Completion = std::function<void(AudioSuiteApplyResult)>;

    TruePeakLimiterApplyThread(SingleTrackDocument& documentToUse,
                               AudioSuitePlanPtr planToUse,
                               int64_t documentGenerationToUse,
                               Completion completionToUse)
        : juce::Thread("True-Peak Limiter Apply"),
          document(documentToUse),
          plan(std::move(planToUse)),
          documentGeneration(documentGenerationToUse),
          completion(std::move(completionToUse))
    {
    }

    ~TruePeakLimiterApplyThread() override
    {
        cancel();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedJoin = stopThread(8000);
#else
        stopThread(8000);
#endif
    }

    void cancel() noexcept
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedTask.note(OptionalLifecycleAction::cancelRequested);
#endif
        cancellation.cancel();
        signalThreadShouldExit();
    }

    void run() override
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const RunScope runObservation(observedTask);
        const ScopedOptionalLifecycleContext workerContext(observedTask.creationContext());
#endif
        const auto result = AudioSuiteTransactionExecutor::apply(document,
                                                                 plan,
                                                                 documentGeneration,
                                                                 cancellation);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observeOutcome(result);
#endif
        if (completion)
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (postObservedCompletion(std::move(completion), result)) return;
#endif
            juce::MessageManager::callAsync(
                [completionToCall = std::move(completion), result]() mutable
                {
                    completionToCall(result);
                });
        }
    }

private:
    SingleTrackDocument& document;
    AudioSuitePlanPtr plan;
    int64_t documentGeneration = 0;
    AudioSuiteCancellationToken cancellation;
    Completion completion;
};

MainComponent::SidebarResizeHandle::SidebarResizeHandle(bool affectsLeftSidebar)
    : isLeftSidebar(affectsLeftSidebar)
{
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    setTooltip(isLeftSidebar ? "Drag to resize playlist sidebar" : "Drag to resize metadata sidebar");
}

void MainComponent::SidebarResizeHandle::paint(juce::Graphics& g)
{
    if (!hovered)
        return;

    auto bounds = getLocalBounds().toFloat();
    auto grip = bounds.withSizeKeepingCentre(4.0f, 56.0f);
    g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.24f));
    g.fillRoundedRectangle(grip, 2.0f);
}

void MainComponent::SidebarResizeHandle::mouseEnter(const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}

void MainComponent::SidebarResizeHandle::mouseExit(const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

void MainComponent::SidebarResizeHandle::mouseDown(const juce::MouseEvent& event)
{
    dragAnchorScreenX = event.getScreenPosition().x;
}

void MainComponent::SidebarResizeHandle::mouseDrag(const juce::MouseEvent& event)
{
    const auto screenX = event.getScreenPosition().x;
    auto delta = screenX - dragAnchorScreenX;
    dragAnchorScreenX = screenX;

    if (!isLeftSidebar)
        delta = -delta;

    if (delta != 0 && onDragDelta)
        onDragDelta(delta);
}

MainComponent::MainComponent(juce::ResizableWindow& hostWindowToUse,
                             juce::ApplicationProperties& appPropertiesToUse,
                             SingleTrackDocument& documentToUse,
                             BnkDocument& bnkDocumentToUse,
                             PlaylistModel& playlistModelToUse,
                             AudioEngine& audioEngineToUse,
                             WaveformModel& waveformModelToUse,
                             bool startupHasInitialFileToUse,
                             bool hostProvidesTitleBarToUse)
    : hostWindow(hostWindowToUse),
      appProperties(appPropertiesToUse),
      document(documentToUse),
      bnkDocument(bnkDocumentToUse),
      playlistModel(playlistModelToUse),
      audioEngine(audioEngineToUse),
      waveformModel(waveformModelToUse),
      startupHasInitialFile(startupHasInitialFileToUse),
      hostProvidesTitleBar(hostProvidesTitleBarToUse),
      deferChromeForStartup(startupHasInitialFileToUse),
      workspaceSession(appPropertiesToUse, documentToUse, bnkDocumentToUse, playlistModelToUse, waveformModelToUse),
      saveCoordinator(documentToUse),
      titleBar(hostWindowToUse),
      actionBar(18.0f),
      footerBar(16.0f),
      selectionPopup(14.0f),
      actionDragZone(hostWindowToUse),
      waveformView(audioEngine, document, waveformModel),
      playlistResizeHandle(true),
      metadataResizeHandle(false)
{
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    const auto constructorStartedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif

    if (!deferChromeForStartup)
        ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);

    playlistModel.addListener(this);
    audioEngine.addListener(this);
    startTimerHz(30);
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setComponentID("single-editor.workspace");
    setTitle("Single Editor workspace");
    setDescription("Audio waveform, transport, editing controls, and optional modules.");
    registerCoreCommandProvider();
    if (!deferChromeForStartup)
        registerPostCoreCommandProvider();

    addAndMakeVisible(titleBar);
    titleBar.onMenuRequested = [this](CustomTitleBarComponent::MenuKind menuKind, juce::Rectangle<int> targetScreenBounds)
    {
        showTitleBarMenu(menuKind, targetScreenBounds);
    };
    titleBar.onMinimiseRequested = [this]
    {
        dispatchGuiCommand(commandIds::minimize, GuiCommandSource::toolbar, "title-bar");
    };
    titleBar.onMaximiseRequested = [this]
    {
        dispatchGuiCommand(commandIds::maximize, GuiCommandSource::toolbar, "title-bar");
    };
    titleBar.onCloseRequested = [this]
    {
        dispatchGuiCommand(commandIds::close, GuiCommandSource::toolbar, "title-bar");
    };
    addAndMakeVisible(actionBar);
    addAndMakeVisible(footerBar);
    actionBar.getProperties().set("fillAlpha", 0.0);
    actionBar.getProperties().set("shadowAlpha", 0.0);
    footerBar.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::mutedSurface().getARGB()));
    footerBar.getProperties().set("fillAlpha", 0.98);
    footerBar.getProperties().set("shadowAlpha", 0.0);
    addChildComponent(selectionPopup);
    addAndMakeVisible(actionDragZone);
    selectionPopup.setInterceptsMouseClicks(!RendererVisualStyle::isClassic(), true);
    selectionPopup.setBackdropSource(&waveformView);
    selectionPopup.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::elevatedPanelBackground().getARGB()));
    selectionPopup.getProperties().set("fillAlpha", RendererVisualStyle::isClassic() ? 0.9 : 1.0);
    selectionPopup.getProperties().set("shadowAlpha", 0.018);
    addAndMakeVisible(waveformView);
    waveformView.setComponentID("core.waveform");
    waveformView.setTitle("Audio waveform");
    waveformView.setDescription("Read-only overview of the current audio, playback position, selection, and playable range.");
    waveformView.setHelpText("Space plays or pauses. F6 moves between major regions.");
    waveformView.onWaveformPresentation = [this](bool hasFullWaveform)
    {
        if (const auto state = document.getState(); state != nullptr && state->info.isValid())
            markWaveformPresentedIfNeeded(state->info.file, hasFullWaveform);
        if (hasFullWaveform)
        {
            // Completion can arrive while stopped. Refresh outside paint, and
            // discard this update if another document generation replaces it.
            const auto generation = activeDocumentGeneration;
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this), generation]
            {
                if (safe == nullptr || !safe->coreLifecycle.isCurrent(generation)
                    || !safe->coreLifecycle.isCoreReady(generation) || safe->deferChromeForStartup)
                    return;
                safe->refreshTransportReadout();
                safe->refreshFooterReadout();
            });
        }
    };
    waveformView.onViewModeRequested = [this] (WaveformViewComponent::ViewMode mode)
    {
        dispatchGuiCommand(mode == WaveformViewComponent::ViewMode::waveform
                               ? commandIds::waveform
                               : commandIds::spectral,
                           GuiCommandSource::mag,
                           "core.waveform");
    };
    waveformView.onDisplayModeRequested = [this] (WaveformViewComponent::DisplayMode mode)
    {
        dispatchGuiCommand(mode == WaveformViewComponent::DisplayMode::stacked
                               ? commandIds::stacked
                               : commandIds::mixed,
                           GuiCommandSource::mag,
                           "core.waveform");
    };
    waveformView.onNavigationRequested = [this] (WaveformOverviewComponent::Action action)
    {
        dispatchGuiCommand(action == WaveformOverviewComponent::Action::reset ? commandIds::resetView
            : action == WaveformOverviewComponent::Action::zoomIn ? commandIds::zoomTimeIn : commandIds::zoomTimeOut,
            GuiCommandSource::mag, "core.waveform");
    };
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    appendOpenTimingLogLine(juce::Time::getCurrentTime().toISO8601(true) + " | ui-mounted");
#endif
    addAndMakeVisible(playlistResizeHandle);
    addAndMakeVisible(metadataResizeHandle);
    addAndMakeVisible(togglePlaylistSidebarButton);
    addAndMakeVisible(toggleMetadataSidebarButton);

    playlistResizeHandle.onDragDelta = [this](int delta)
    {
        playlistSidebarWidth = juce::jlimit(180, 420, playlistSidebarWidth + delta);
        persistSessionSettings();
        resized();
        repaint();
    };
    metadataResizeHandle.onDragDelta = [this](int delta)
    {
        metadataSidebarWidth = juce::jlimit(190, 460, metadataSidebarWidth + delta);
        persistSessionSettings();
        resized();
        repaint();
    };

    configureActionButton(togglePlaylistSidebarButton, juce::CharPointer_UTF8("\xE2\x80\xB9"));
    configureActionButton(toggleMetadataSidebarButton, juce::CharPointer_UTF8("\xE2\x80\xBA"));
    togglePlaylistSidebarButton.setEnabled(true);
    toggleMetadataSidebarButton.setEnabled(true);
    togglePlaylistSidebarButton.setTooltip("Toggle playlist sidebar");
    toggleMetadataSidebarButton.setTooltip("Toggle metadata sidebar");
    togglePlaylistSidebarButton.setCornerRadius(10.0f);
    toggleMetadataSidebarButton.setCornerRadius(10.0f);
    togglePlaylistSidebarButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::togglePlaylistSidebar, GuiCommandSource::toolbar);
    };
    toggleMetadataSidebarButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::toggleMetadataSidebar, GuiCommandSource::toolbar);
    };

    addChildComponent(toolbarMoreButton);
    addChildComponent(generalSceneButton);
    addChildComponent(simpleViewButton);
    addChildComponent(lightThemeButton);
    addChildComponent(nightThemeButton);
    lightThemeButton.onClick = [this] { setModernAppearanceTheme(ModernTheme::light); };
    nightThemeButton.onClick = [this] { setModernAppearanceTheme(ModernTheme::night); };
    generalSceneButton.setComponentID("workspace.scene.general");
    generalSceneButton.setTitle("General workspace scene");
    generalSceneButton.setTooltip("Full workspace with your current Mag layout");
    simpleViewButton.setComponentID("workspace.view.simple");
    simpleViewButton.setTitle("Simple waveform view");
    simpleViewButton.setTooltip("Focus on the waveform; preserve playback, effects and Mag layout");
    waveformView.onReturnToGeneral = [this] { setModernSimpleView(false); };
    generalSceneButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    simpleViewButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    generalSceneButton.onClick = [this] { dispatchGuiCommand(commandIds::generalScene, GuiCommandSource::toolbar); };
    simpleViewButton.onClick = [this] { dispatchGuiCommand(commandIds::simpleView, GuiCommandSource::toolbar); };
    toolbarMoreButton.setComponentID("toolbar.more");
    toolbarMoreButton.setTitle("More file, edit, Mag and view commands");
    toolbarMoreButton.setTooltip("File, edit, Mag, view and audio commands");
    toolbarMoreButton.onClick = [this]
    {
        juce::PopupMenu menu;
        std::vector<juce::Component::SafePointer<juce::Button>> actions;
        std::vector<std::string_view> magCommands;
        auto add = [&] (juce::PopupMenu& target, juce::Button& button)
        {
            actions.emplace_back(&button);
            target.addItem(static_cast<int>(actions.size()), button.getButtonText(),
                           button.isEnabled(), button.getToggleState());
        };
        juce::PopupMenu::Item paletteItem("Command Palette...");
        paletteItem.itemID = 1201;
        paletteItem.isEnabled = !deferChromeForStartup;
        paletteItem.shortcutKeyDescription = "Ctrl+Shift+P";
        menu.addItem(std::move(paletteItem));
        menu.addSeparator();
        juce::PopupMenu fileMenu;
        for (auto* button : { &recentButton, &saveButton, &saveAsButton,
                              &removeFileButton, &preferencesButton })
            add(fileMenu, *button);
        menu.addSubMenu("File", fileMenu);
        juce::PopupMenu editMenu;
        for (auto* button : { &fadeInButton, &fadeOutButton,
                              &deleteSelectionButton, &resetEnvelopeButton })
            add(editMenu, *button);
        menu.addSubMenu("Edit", editMenu);
        juce::PopupMenu viewMenu;
        if (!RendererVisualStyle::isClassic())
        {
            const auto hasWorkspace = workspaceModuleHost != nullptr;
            const auto playlistActive = hasWorkspace && workspaceModuleHost->isModuleActive(WorkspaceModuleId::playlist);
            const auto metadataActive = hasWorkspace && workspaceModuleHost->isModuleActive(WorkspaceModuleId::metadata);
            viewMenu.addItem(1205, playlistActive ? "Hide Playlist" : "Show Playlist", hasWorkspace, playlistActive);
            viewMenu.addItem(1206, metadataActive ? "Hide File information" : "Show File information", hasWorkspace, metadataActive);
            viewMenu.addSeparator();
            const auto active = workspaceModuleHost != nullptr && workspaceModuleHost->isModuleActive(WorkspaceModuleId::markers);
            const auto floating = workspaceModuleHost != nullptr && workspaceModuleHost->isModuleFloating(WorkspaceModuleId::markers);
            juce::PopupMenu markersMenu;
            markersMenu.addItem(1202, active ? "Hide Markers" : "Show Markers", true, active);
            markersMenu.addItem(1203, "Float Markers", active && !floating);
            markersMenu.addItem(1204, "Redock Markers", floating);
            viewMenu.addSubMenu("Markers", markersMenu, hasWorkspace);
            viewMenu.addSeparator();
        }
        for (auto* button : { &timeZoomOutButton, &timeZoomInButton,
                              &ampZoomOutButton, &ampZoomInButton, &resetViewButton })
            add(viewMenu, *button);
        menu.addSubMenu("View", viewMenu);
        juce::PopupMenu magMenu;
        const auto magContext = makeCommandContext(GuiCommandSource::menu, "more-mag-menu");
        const auto addMag = [&] (std::string_view commandId, const char* label)
        {
            magCommands.push_back(commandId);
            const auto state = commandRegistry.resolve(commandId, magContext);
            magMenu.addItem(1300 + static_cast<int>(magCommands.size()), label,
                            state && state->state.enabled, state && state->state.checked);
        };
        addMag(commandIds::togglePlaylistSidebar, "Playlist");
        addMag(commandIds::toggleMetadataSidebar, "File Information");
        addMag(commandIds::toggleMarkers, "Markers");
        addMag(commandIds::toggleOutputMeter, "Output Meter");
        addMag(commandIds::toggleParametricEq, "Parametric EQ");
        addMag(commandIds::toggleTruePeakLimiter, "True-Peak Limiter");
        addMag(commandIds::toggleAmbisonicEnergy, "Ambisonic Energy");
        addMag(commandIds::toggleSpatialEnergy, "Rendered Output Spatial Energy");
        magMenu.addSeparator();
        addMag(commandIds::resetModuleLayout, "Reset Mag Layout");
        menu.addSubMenu("Mag", magMenu);
        juce::PopupMenu audioMenu;
        add(audioMenu, autoPlayButton);
        if (parametricEqButton != nullptr) add(audioMenu, *parametricEqButton);
        if (truePeakLimiterButton != nullptr) add(audioMenu, *truePeakLimiterButton);
        menu.addSubMenu("Audio", audioMenu);
        juce::PopupMenu appearanceMenu;
        constexpr int lightThemeItem = 1001, nightThemeItem = 1002;
        appearanceMenu.addItem(lightThemeItem, "Modern Glass - Light", true, !RendererVisualStyle::isNight());
        appearanceMenu.addItem(nightThemeItem, "Modern Glass - Night", true, RendererVisualStyle::isNight());
        appearanceMenu.addSeparator();
        appearanceMenu.addItem(1101, "Solid - Reduce transparency", true, RendererVisualStyle::glassMaterial() == ModernGlassMaterial::solid);
        appearanceMenu.addItem(1102, "Frosted glass", true, RendererVisualStyle::glassMaterial() == ModernGlassMaterial::frosted);
        appearanceMenu.addItem(1103, "Liquid glass", true, RendererVisualStyle::glassMaterial() == ModernGlassMaterial::liquid);
        menu.addSubMenu("Appearance", appearanceMenu);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&toolbarMoreButton).withMinimumWidth(220),
            [actions = std::move(actions), magCommands = std::move(magCommands),
             safe = juce::Component::SafePointer<MainComponent>(this)] (int result)
            {
                if (safe == nullptr) return;
                if (result > 1300 && result <= 1300 + static_cast<int>(magCommands.size()))
                {
                    safe->dispatchGuiCommand(magCommands[static_cast<std::size_t>(result - 1301)],
                                             GuiCommandSource::menu);
                    return;
                }
                if (result == 1201)
                {
                    safe->dispatchGuiCommand(commandIds::palette, GuiCommandSource::menu);
                    return;
                }
                if (result >= 1202 && result <= 1204)
                {
                    safe->dispatchGuiCommand(result == 1202 ? commandIds::toggleMarkers
                                               : result == 1203 ? commandIds::floatMarkers : commandIds::redockMarkers,
                                             GuiCommandSource::menu);
                    return;
                }
                if (result == 1205 || result == 1206)
                {
                    safe->dispatchGuiCommand(result == 1205 ? commandIds::togglePlaylistSidebar
                                                           : commandIds::toggleMetadataSidebar,
                                             GuiCommandSource::menu);
                    return;
                }
                if (result >= 1101 && result <= 1103)
                {
                    const auto material = result == 1101 ? ModernGlassMaterial::solid
                        : result == 1103 ? ModernGlassMaterial::liquid : ModernGlassMaterial::frosted;
                    RendererVisualStyle::setGlassMaterial(material);
                    safe->selectionPopup.invalidateBackdrop();
                    if (safe->commandPalette != nullptr) safe->commandPalette->invalidateBackdrop();
                    if (auto* settings = safe->appProperties.getUserSettings())
                    {
                        settings->setValue(PreferencesComponent::modernMaterialKey,
                            material == ModernGlassMaterial::solid ? "solid" : material == ModernGlassMaterial::liquid ? "liquid" : "frosted");
                        settings->saveIfNeeded();
                    }
                    return;
                }
                if (result == lightThemeItem || result == nightThemeItem)
                {
                    safe->setModernAppearanceTheme(result == nightThemeItem ? ModernTheme::night : ModernTheme::light);
                    return;
                }
                if (result > 0 && result <= static_cast<int>(actions.size()))
                    if (auto* button = actions[static_cast<size_t>(result - 1)].getComponent())
                        if (button->isEnabled())
                            button->triggerClick();
            });
    };

    addAndMakeVisible(openButton);
    openButton.setButtonText("Open Audio");
    setButtonIcon(openButton, ModernNorthStarAssetId::openFiles);
    openButton.onClick = [this] { dispatchGuiCommand(commandIds::open, GuiCommandSource::toolbar); };
    addAndMakeVisible(recentButton);
    configureActionButton(recentButton, "Recent");
    setButtonIcon(recentButton, ModernNorthStarAssetId::toggle);
    recentButton.onClick = [this] { showRecentFilesMenu(); };
    addAndMakeVisible(removeFileButton);
    configureActionButton(removeFileButton, "Remove");
    setButtonIcon(removeFileButton, ModernNorthStarAssetId::remove);
    removeFileButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::removePlaylist, GuiCommandSource::toolbar);
    };
    addAndMakeVisible(preferencesButton);
    configureActionButton(preferencesButton, "Preferences");
    setButtonIcon(preferencesButton, ModernNorthStarAssetId::preferences);
    preferencesButton.setEnabled(true);
    preferencesButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::preferences, GuiCommandSource::toolbar);
    };
    addAndMakeVisible(saveButton);
    addAndMakeVisible(saveAsButton);
    configureActionButton(saveButton, "Save");
    configureActionButton(saveAsButton, "Save As");
    setButtonIcon(saveButton, ModernNorthStarAssetId::save);
    setButtonIcon(saveAsButton, ModernNorthStarAssetId::save);
    saveButton.onClick = [this] { dispatchGuiCommand(commandIds::save, GuiCommandSource::toolbar); };
    saveAsButton.onClick = [this] { dispatchGuiCommand(commandIds::saveAs, GuiCommandSource::toolbar); };

    addAndMakeVisible(startButton);
    addAndMakeVisible(previousButton);
    addAndMakeVisible(nextButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(returnToStartButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(autoPlayButton);
    configureTransportButton(startButton, "Start");
    configureTransportButton(previousButton, "< Frame");
    configureTransportButton(nextButton, "Frame >");
    configureTransportButton(playButton, "Play");
    configureTransportButton(pauseButton, "Pause");
    configureTransportButton(stopButton, "Stop");
    configureTransportToggle(returnToStartButton, "RTS", true);
    configureTransportToggle(loopButton, "Loop", false);
    configureTransportToggle(autoPlayButton, "Auto Play", false);
    returnToStartButton.setClickingTogglesState(false);
    loopButton.setClickingTogglesState(false);
    autoPlayButton.setClickingTogglesState(false);
    setButtonIcon(startButton, ModernNorthStarAssetId::skipBack, "Go To Start");
    setButtonIcon(previousButton, ModernNorthStarAssetId::previousFrame, "Previous Frame");
    setButtonIcon(nextButton, ModernNorthStarAssetId::nextFrame, "Next Frame");
    setButtonIcon(playButton, ModernNorthStarAssetId::play, "Play");
    setButtonIcon(pauseButton, ModernNorthStarAssetId::pause, "Pause");
    setButtonIcon(stopButton, ModernNorthStarAssetId::stop, "Stop");
    setButtonIcon(returnToStartButton, ModernNorthStarAssetId::toggle);
    setButtonIcon(loopButton, ModernNorthStarAssetId::loop);
    autoPlayButton.getProperties().remove("iconName");
    autoPlayButton.getProperties().set("iconOnly", false);
    autoPlayButton.getProperties().remove("ghostButton");
    autoPlayButton.setTooltip("Automatically play clicked audio files, events, and bank media from the start");

    startButton.onClick = [this] { dispatchGuiCommand(commandIds::start, GuiCommandSource::toolbar); };
    previousButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::previousFrame, GuiCommandSource::toolbar);
    };
    nextButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::nextFrame, GuiCommandSource::toolbar);
    };
    playButton.onClick = [this] { dispatchGuiCommand(commandIds::play, GuiCommandSource::toolbar); };
    pauseButton.onClick = [this] { dispatchGuiCommand(commandIds::pause, GuiCommandSource::toolbar); };
    stopButton.onClick = [this] { dispatchGuiCommand(commandIds::stop, GuiCommandSource::toolbar); };
    returnToStartButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::returnToStart, GuiCommandSource::toolbar);
    };
    loopButton.onClick = [this] { dispatchGuiCommand(commandIds::loop, GuiCommandSource::toolbar); };
    autoPlayButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::autoPlay, GuiCommandSource::toolbar);
    };

    selectionPopup.addAndMakeVisible(gainDownButton);
    selectionPopup.addAndMakeVisible(gainUpButton);
    addAndMakeVisible(fadeInButton);
    addAndMakeVisible(fadeOutButton);
    addAndMakeVisible(deleteSelectionButton);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addAndMakeVisible(resetEnvelopeButton);
    addAndMakeVisible(timeZoomOutButton);
    addAndMakeVisible(timeZoomInButton);
    addAndMakeVisible(ampZoomOutButton);
    addAndMakeVisible(ampZoomInButton);
    addAndMakeVisible(resetViewButton);
    configureActionButton(gainDownButton, RendererVisualStyle::isClassic() ? "-3 dB" : "-3");
    configureActionButton(gainUpButton, RendererVisualStyle::isClassic() ? "+3 dB" : "+3");
    gainDownButton.setTooltip("Decrease selection gain by 3 dB");
    gainUpButton.setTooltip("Increase selection gain by 3 dB");
    configureActionButton(fadeInButton, "Fade In");
    configureActionButton(fadeOutButton, "Fade Out");
    configureActionButton(deleteSelectionButton, "Delete Sel");
    configureActionButton(undoButton, "Undo");
    configureActionButton(redoButton, "Redo");
    configureActionButton(resetEnvelopeButton, "Reset Env");
    configureActionButton(timeZoomOutButton, "Time -");
    configureActionButton(timeZoomInButton, "Time +");
    configureActionButton(ampZoomOutButton, "Amp -");
    configureActionButton(ampZoomInButton, "Amp +");
    configureActionButton(resetViewButton, "Reset View");
    setButtonIcon(fadeInButton, ModernNorthStarAssetId::fadeIn);
    setButtonIcon(fadeOutButton, ModernNorthStarAssetId::fadeOut);
    setButtonIcon(deleteSelectionButton, ModernNorthStarAssetId::remove);
    setButtonIcon(undoButton, ModernNorthStarAssetId::undo);
    setButtonIcon(redoButton, ModernNorthStarAssetId::redo);
    setButtonIcon(resetEnvelopeButton, ModernNorthStarAssetId::reset);
    setButtonIcon(timeZoomOutButton, ModernNorthStarAssetId::zoomOut);
    setButtonIcon(timeZoomInButton, ModernNorthStarAssetId::zoomIn);
    setButtonIcon(ampZoomOutButton, ModernNorthStarAssetId::zoomOut);
    setButtonIcon(ampZoomInButton, ModernNorthStarAssetId::zoomIn);
    setButtonIcon(resetViewButton, ModernNorthStarAssetId::reset);
    gainDownButton.onClick = [this]
    {
        selectionGainSlider.setValue(selectionGainSlider.getValue() - 3.0, juce::sendNotificationSync);
    };
    gainUpButton.onClick = [this]
    {
        selectionGainSlider.setValue(selectionGainSlider.getValue() + 3.0, juce::sendNotificationSync);
    };
    fadeInButton.onClick = [this] { dispatchGuiCommand(commandIds::fadeIn, GuiCommandSource::toolbar); };
    fadeOutButton.onClick = [this] { dispatchGuiCommand(commandIds::fadeOut, GuiCommandSource::toolbar); };
    deleteSelectionButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::deleteSelection, GuiCommandSource::toolbar);
    };
    undoButton.onClick = [this] { dispatchGuiCommand(commandIds::undo, GuiCommandSource::toolbar); };
    redoButton.onClick = [this] { dispatchGuiCommand(commandIds::redo, GuiCommandSource::toolbar); };
    timeZoomOutButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::zoomTimeOut, GuiCommandSource::toolbar);
    };
    timeZoomInButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::zoomTimeIn, GuiCommandSource::toolbar);
    };
    ampZoomOutButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::zoomAmplitudeOut, GuiCommandSource::toolbar);
    };
    ampZoomInButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::zoomAmplitudeIn, GuiCommandSource::toolbar);
    };
    resetViewButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::resetView, GuiCommandSource::toolbar);
    };
    resetEnvelopeButton.onClick = [this]
    {
        dispatchGuiCommand(commandIds::resetEnvelope, GuiCommandSource::toolbar);
    };

    const auto exposeCommandControl = [] (juce::Button& button,
                                          std::string_view commandId,
                                          const juce::String& accessibleName)
    {
        button.setComponentID(juce::String::fromUTF8(commandId.data(),
                                                     static_cast<int>(commandId.size())));
        button.setTitle(accessibleName);
    };
    exposeCommandControl(togglePlaylistSidebarButton, commandIds::togglePlaylistSidebar, "Toggle playlist sidebar");
    exposeCommandControl(toggleMetadataSidebarButton, commandIds::toggleMetadataSidebar, "Toggle metadata sidebar");
    exposeCommandControl(openButton, commandIds::open, "Open audio");
    recentButton.setComponentID("document.open-recent");
    recentButton.setTitle("Open recent audio");
    exposeCommandControl(removeFileButton, commandIds::removePlaylist, "Remove current playlist item");
    exposeCommandControl(preferencesButton, commandIds::preferences, "Preferences");
    exposeCommandControl(saveButton, commandIds::save, "Save");
    exposeCommandControl(saveAsButton, commandIds::saveAs, "Save as");
    exposeCommandControl(startButton, commandIds::start, "Go to start");
    exposeCommandControl(previousButton, commandIds::previousFrame, "Previous frame");
    exposeCommandControl(nextButton, commandIds::nextFrame, "Next frame");
    exposeCommandControl(playButton, commandIds::play, "Play");
    exposeCommandControl(pauseButton, commandIds::pause, "Pause");
    exposeCommandControl(stopButton, commandIds::stop, "Stop");
    exposeCommandControl(returnToStartButton, commandIds::returnToStart, "Return to start on stop");
    exposeCommandControl(loopButton, commandIds::loop, "Loop playback");
    exposeCommandControl(autoPlayButton, commandIds::autoPlay, "Auto play");
    exposeCommandControl(fadeInButton, commandIds::fadeIn, "Fade in selection");
    exposeCommandControl(fadeOutButton, commandIds::fadeOut, "Fade out selection");
    exposeCommandControl(deleteSelectionButton, commandIds::deleteSelection, "Delete selection");
    exposeCommandControl(undoButton, commandIds::undo, "Undo");
    exposeCommandControl(redoButton, commandIds::redo, "Redo");
    exposeCommandControl(resetEnvelopeButton, commandIds::resetEnvelope, "Reset envelope");
    exposeCommandControl(timeZoomOutButton, commandIds::zoomTimeOut, "Zoom time out");
    exposeCommandControl(timeZoomInButton, commandIds::zoomTimeIn, "Zoom time in");
    exposeCommandControl(ampZoomOutButton, commandIds::zoomAmplitudeOut, "Zoom amplitude out");
    exposeCommandControl(ampZoomInButton, commandIds::zoomAmplitudeIn, "Zoom amplitude in");
    exposeCommandControl(resetViewButton, commandIds::resetView, "Reset waveform view");

    selectionPopup.addAndMakeVisible(selectionGainLabel);
    selectionPopup.setName("AudioSuite reference gain");
    configureLabel(selectionGainLabel, 12.5f, juce::Justification::centredLeft, ModernAppleLookAndFeel::primaryText());
    selectionGainLabel.setText("0.0 dB", juce::dontSendNotification);

    selectionPopup.addAndMakeVisible(selectionGainSlider);
    selectionGainSlider.setName("AudioSuite Reference Gain");
    selectionGainSlider.setTitle("AudioSuite Reference Gain");
    selectionGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    selectionGainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    selectionGainSlider.setRange(-24.0, 24.0, 0.1);
    selectionGainSlider.setValue(0.0, juce::dontSendNotification);
    selectionGainSlider.setDoubleClickReturnValue(true, 0.0);
    selectionGainSlider.setTooltip("Double-click to reset gain to 0.0 dB");
    selectionGainSlider.onValueChange = [this]
    {
        const auto dbValue = static_cast<float>(selectionGainSlider.getValue());
        const auto linear = juce::Decibels::decibelsToGain(dbValue);
        selectionGainLabel.setText(juce::String(dbValue, 1) + " dB", juce::dontSendNotification);

        if (currentSelection.has_value())
        {
            updateReferenceAudioSuitePreview();
            waveformView.setSelectionPreviewGain(linear);
        }
        updateTransportAvailability();
    };

    selectionPopup.addAndMakeVisible(applySelectionGainButton);
    selectionPopup.addAndMakeVisible(cancelSelectionGainButton);
    configureActionButton(applySelectionGainButton, "Apply");
    configureActionButton(cancelSelectionGainButton, "Cancel");
    applySelectionGainButton.setTooltip("Apply selection gain");
    cancelSelectionGainButton.setTooltip("Cancel gain preview and close controls; keep the selection");
    applySelectionGainButton.onClick = [this] { applySelectionGain(); };
    cancelSelectionGainButton.onClick = [this] { dismissSelectionPopup(); };

    addAndMakeVisible(filePillLabel);
    configureLabel(filePillLabel, 12.8f, juce::Justification::centredLeft, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(engineStatusLabel);
    configureLabel(engineStatusLabel, 11.8f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(transportStatusLabel);
    configureLabel(transportStatusLabel, 11.8f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(monitorLabel);
    configureLabel(monitorLabel, 11.5f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText());
    monitorLabel.setText("Monitor", juce::dontSendNotification);

    addAndMakeVisible(monitorSlider);
    monitorSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    monitorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    monitorSlider.setRange(0.0, 100.0, 1.0);
    monitorSlider.setValue(100.0, juce::dontSendNotification);
    monitorSlider.onValueChange = [this]
    {
        audioEngine.setMonitorGainLinear(static_cast<float>(monitorSlider.getValue() / 100.0));
        refreshFooterReadout();
        requestSessionSettingsPersist();
    };

    addAndMakeVisible(meterModeBox);
    meterModeBox.addItem("TP", static_cast<int>(AudioEngine::MeterMode::truePeak));
    meterModeBox.addItem("RMS", static_cast<int>(AudioEngine::MeterMode::rms));
    meterModeBox.addItem("LUFS", static_cast<int>(AudioEngine::MeterMode::lufs));
    meterModeBox.setSelectedId(static_cast<int>(AudioEngine::MeterMode::truePeak), juce::dontSendNotification);
    meterModeBox.onChange = [this]
    {
        const auto mode = static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId());
        if (meterBridge) meterBridge->setMeterMode(mode);
        if (auto* outputMeterPanel = getOutputMeterPanel())
            outputMeterPanel->setMeterMode(mode);
        refreshFooterReadout();
        peakLabel.setText(formatMeterReadout(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()), audioEngine),
                          juce::dontSendNotification);
        requestSessionSettingsPersist();
    };

    addAndMakeVisible(peakLabel);
    configureLabel(peakLabel, 11.8f, juce::Justification::centredRight, ModernAppleLookAndFeel::accentOrange());
    peakLabel.setText("TP --", juce::dontSendNotification);
    peakLabel.setTooltip("TP: highest output-channel true peak. RMS: power average across all device outputs, "
                         "including silent channels; Output shows RMS max for the loudest channel. "
                         "LUFS: global momentary loudness.");

    addAndMakeVisible(footerLeftLabel);
    configureLabel(footerLeftLabel, 12.0f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText().withAlpha(0.92f));

    addAndMakeVisible(footerRightLabel);
    configureLabel(footerRightLabel, 12.0f, juce::Justification::centredRight, ModernAppleLookAndFeel::primaryText().withAlpha(0.82f));

    waveformView.onVisualContentChanged = [this]
    {
        notifyStartupCoreSurfacePreparedIfNeeded();
        // Only a selection needs to track zoom/scroll geometry. Ordinary first
        // waveform and playback frames keep the existing inactive-glass path.
        if (currentSelection.has_value())
            updateSelectionPopup();
        else
            selectionPopup.invalidateBackdrop();
    };
    waveformView.onSelectionChanged = [this](std::optional<juce::Range<double>> newSelection)
    {
        handleSelectionChanged(std::move(newSelection));
    };
    waveformView.onSourceChannelSelectionGesture = [this](int sourceChannel,
                                                           bool extendSelection)
    {
        handleSourceChannelSelectionGesture(sourceChannel, extendSelection);
    };
    waveformView.onWholeFileSelectionGesture = [this]
    {
        selectedSourceChannels.clear();
        waveformView.setSelectedSourceChannels({});
        if (parametricEqPreview.isValid())
            updateParametricEqPreview();
        if (truePeakLimiterPreview.isValid())
            updateTruePeakLimiterPreview();
    };
    if (!startupHasInitialFile)
        ensureDeferredWorkspaceControls();

    waveformView.addComponentListener(this);
    waveformView.addKeyListener(this);
    selectionPopup.addKeyListener(this);
    if (startupHasInitialFile)
        restoreStartupCriticalSettings();
    else
        restoreSessionSettings();
    if (!startupHasInitialFile)
        ensureDeferredOutputMeter();
    applyChromePresentation(false);
    refreshUiState();
    if (!startupHasInitialFile)
    {
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (safeThis != nullptr)
            {
                safeThis->restorePlaylistSession();
                // The constructor's posted callback runs after the host has
                // shown the empty workspace. A restored/opening audio file
                // must instead reach its real waveform/playable CoreReady.
                if (safeThis->isShowing() && !safeThis->loadInProgress && !safeThis->document.hasAudio())
                    safeThis->discoverRecoveryAfterCore();
                juce::Timer::callAfterDelay(4200, [safeThis]
                {
                    if (safeThis != nullptr && !safeThis->loadInProgress && !safeThis->document.hasAudio())
                        safeThis->scheduleRecentCacheWarm();
                });
            }
        });
    }
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    appendOpenTimingLogLine(juce::Time::getCurrentTime().toISO8601(true)
                            + " | ui-main-ctor-ms="
                            + juce::String(static_cast<int>(std::round(juce::Time::getMillisecondCounterHiRes() - constructorStartedAtMs)))
                            + " | startup="
                            + juce::String(startupHasInitialFile ? 1 : 0));
#endif
}

MainComponent::~MainComponent()
{
    if (asioSettingsSession) asioSettingsSession->shutdown();
    if (asioSettingsWindow) asioSettingsWindow->exitModalState(0);
    if (deviceProbe) deviceProbe->abandon();
    diagnosticsWindow.reset();
    releaseBnkRuntime();
    if (packedAudioLoad) packedAudioLoad->operation->requestCancel();
    if (packedProgressWindow) packedProgressWindow->exitModalState(0);
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    traceShutdown("main.destructor.begin");
    if (recoveryDiscoveryCancelled) recoveryDiscoveryCancelled->store(true);
    // A readonly worker owns its paths/result/cancel state. Component teardown
    // must not wait for an OS metadata read; late UI callbacks are weak/fenced.
    if (recoveryDiscoveryWorker.joinable()) recoveryDiscoveryWorker.detach();
    if (recoveryRestore) recoveryRestore->close();
    recoveryCenter.reset();
    waveformView.removeComponentListener(this);
    waveformView.removeKeyListener(this);
    selectionPopup.removeKeyListener(this);
    waveformView.onVisualContentChanged = nullptr;
    selectionPopup.setEffectsEnabled(false);
    selectionPopup.setVisible(false);
    if (auto* parent = selectionPopup.getParentComponent())
        parent->removeChildComponent(&selectionPopup);
    cancelTruePeakLimiterApply(true);
    clearTruePeakLimiterPreview(true);
    cancelParametricEqApply(true);
    clearParametricEqPreview(true);
    cancelReferenceAudioSuiteApply(true);
    clearReferenceAudioSuitePreview(true);
    traceShutdown("main.dsp.released");
    flushPendingSessionSettingsPersist();
    if (workspaceModuleHost != nullptr)
    {
        workspaceModuleHost->setPrivilegedCoreComponent(nullptr);
        workspaceModuleHost.reset();
    }
    traceShutdown("main.workspace.released");
    abandonActiveLoadTimingProbe("component-destroyed");
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    flushPendingOpenTimingLogLines();
#endif
    playlistModel.removeListener(this);
    audioEngine.removeListener(this);
    traceShutdown("main.load-jobs.cancel.begin");
    loadThreadPool.removeAllJobs(true, 8000);
    traceShutdown("main.load-jobs.cancel.end");
    pendingCacheWarm.reset();
    if (cacheWarmThreadPool) cacheWarmThreadPool->removeAllJobs(true, 4000);
    traceShutdown("main.cache-jobs.cancel.end");
}

void MainComponent::openInitialFile(const juce::File& file)
{
    openFiles({file}, 0);
}

void MainComponent::primeStartupLoadingState(const juce::File& file)
{
    if (!file.existsAsFile())
        return;

    ensureDocumentGeneration(file);
    pendingLoadLabel = file.getFileName();
    pendingAutoPlayOnLoad = autoPlayButton.getToggleState();
    pendingAutoPlayFile = file;
    loadInProgress = true;
    refreshUiState();
    if (metadataSidebar) metadataSidebar->repaint();
    waveformView.refreshNow();
}

void MainComponent::showPreparedInitialVisualPreview(const juce::File& file,
                                                     SingleTrackDocument::FileInfo previewInfo,
                                                     std::shared_ptr<WaveformModel::PeakCache> previewPeakCache)
{
    if (!file.existsAsFile() || !previewInfo.isValid() || previewPeakCache == nullptr)
        return;

    ensureDocumentGeneration(file);
    startLoadTimingProbe(file);
    pendingLoadLabel = file.getFileName();
    pendingAutoPlayFile = file;
    loadInProgress = true;

    auto currentState = document.getState();
    if (currentState == nullptr || !fileInfoMatches(currentState->info, previewInfo))
    {
        if (!document.adoptPreviewState(previewInfo))
            return;

        currentState = document.getState();
        if (bnkDocument.hasBank())
            bnkDocument.clear();

        updateCenterViewVisibility();
        notifyPrimaryContentVisibleIfNeeded();
    }

    if (currentState != nullptr && fileInfoMatches(currentState->info, previewInfo))
    {
        previewPeakCache->state = currentState;
        previewPeakCache->audioRevision = currentState->audioRevision;
        previewPeakCache->durationSeconds = currentState->info.durationSeconds;
        previewPeakCache->sampleRate = currentState->info.sampleRate;
        previewPeakCache->totalSamples = currentState->info.numSamples;
        previewPeakCache->channelCount = currentState->info.numChannels;
    }
    else
    {
        previewPeakCache->durationSeconds = previewInfo.durationSeconds;
        previewPeakCache->sampleRate = previewInfo.sampleRate;
        previewPeakCache->totalSamples = previewInfo.numSamples;
        previewPeakCache->channelCount = previewInfo.numChannels;
    }

    waveformModel.adoptPeakCache(std::move(previewPeakCache));
    audioEngine.warmUpAudioDeviceAsync(0);
    refreshUiState();
    repaintMetadataSidebarIfVisible();
    waveformView.refreshNow();
}

void MainComponent::beginPreparedInitialAudioLoad(const juce::File& file,
                                                  SingleTrackDocument::FileInfo previewInfo)
{
    beginPreparedInitialAudioLoadInternal(file, previewInfo, true);
}

void MainComponent::beginPreparedInitialAudioLoadInternal(const juce::File& file,
                                                          SingleTrackDocument::FileInfo previewInfo,
                                                          bool refreshImmediately,
                                                          bool scheduleBackgroundLoad)
{
    if (isBnkFile(file)) { beginBnkOpen(file); return; }
    if (!file.existsAsFile())
        return;
    if (!cancelOrdinaryAudioLoad()) return;
    AudioReturnImage returnImage;
    if (document.hasAudio() || bnkDocument.hasBank()) returnImage = captureAudioReturnImage();
    if (document.hasAudio() && !returnImage.previousAudio)
    { setDocumentOperationStatus("Finish the current edit before changing files."); return; }

    ensureDocumentGeneration(file);
    startLoadTimingProbe(file);

    if (!workspaceSession.shouldRetainPlaylistAudio())
        workspaceSession.clearRetainedPlaylistAudio();

    workspaceSession.retainCurrentAudioDocumentIfNeeded();
    releaseBnkRuntime();

    cancelCacheWarm();
    loadThreadPool.removeAllJobs(false, 0);
    pendingLoadLabel = file.getFileName();
    loadInProgress = true;
    lastPolledFullWaveformRevision = -1;
    lastObservedFullCacheRevision = -1;
    lastObservedFullCacheDocumentRevision = -1;
    lastObservedFullCacheMatchedDocument = false;
    pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
    pendingAutoPlayOnLoad = autoPlayButton.getToggleState();
    pendingAutoPlayFile = file;

    currentSelection.reset();
    waveformView.clearSelection();
    selectionGainSlider.setValue(0.0, juce::dontSendNotification);
    clearReferenceAudioSuitePreview(true);
    waveformView.clearSelectionPreviewGain();

    if (finishReusedAudioLoad(file, pendingAutoPlayOnLoad))
    {
        pendingAutoPlayOnLoad = false;
        pendingAutoPlayFile = {};
        return;
    }

    workspaceSession.releaseRecoveredActivation();
    document.clear();
    if (returnImage.previousAudio || returnImage.previousBank)
    {
        ordinaryAudioLoad.emplace(); ordinaryAudioLoad->returnImage = std::move(returnImage);
        ordinaryAudioLoad->token = pendingLoadToken.load(std::memory_order_relaxed);
        if (const auto* entry = playlistModel.getCurrentEntry()) ordinaryAudioLoad->expectedEntry = entry->id;
        observeOrdinaryAudioLoadState();
    }
    if (refreshImmediately || !previewInfo.isValid())
    {
        refreshUiState();
        repaintMetadataSidebarIfVisible();
        waveformView.refreshNow();
    }

    showAsyncLoadMetadata(pendingLoadToken.load(std::memory_order_relaxed), previewInfo);

    if (scheduleBackgroundLoad)
    {
        const auto token = pendingLoadToken.load(std::memory_order_relaxed);
        loadThreadPool.addJob(new AsyncFileLoadJob(juce::Component::SafePointer<MainComponent>(this),
                                                   file,
                                                   token),
                              true);
    }
}

void MainComponent::showPreparedInitialAudioPreview(const juce::File& file,
                                                    SingleTrackDocument::FileInfo previewInfo,
                                                    std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                                    bool beginLoadIfNeeded)
{
    if (!file.existsAsFile() || !previewInfo.isValid() || previewPeakCache == nullptr)
        return;

    const auto shouldBeginLoad = !loadInProgress || pendingAutoPlayFile != file;
    if (shouldBeginLoad)
    {
        if (const auto currentState = document.getState();
            currentState != nullptr && fileInfoMatches(currentState->info, previewInfo))
        {
            if (const auto currentPeakCache = waveformModel.getPeakCache();
                currentPeakCache != nullptr
                && currentPeakCache->audioRevision == currentState->audioRevision
                && (!currentPeakCache->isPreview
                    || currentPeakCache->loadedSampleRange.getEnd() >= previewPeakCache->loadedSampleRange.getEnd()))
            {
                return;
            }

            previewPeakCache->state = currentState;
            previewPeakCache->audioRevision = currentState->audioRevision;
            previewPeakCache->durationSeconds = currentState->info.durationSeconds;
            previewPeakCache->sampleRate = currentState->info.sampleRate;
            previewPeakCache->totalSamples = currentState->info.numSamples;
            previewPeakCache->channelCount = currentState->info.numChannels;
            waveformModel.adoptPeakCache(std::move(previewPeakCache));
            refreshUiState();
            waveformView.refreshNow();
            return;
        }

        beginPreparedInitialAudioLoadInternal(file, previewInfo, false, beginLoadIfNeeded);
    }

    const auto token = pendingLoadToken.load(std::memory_order_relaxed);
    showAsyncLoadPreview(token, previewInfo, std::move(previewPeakCache));
}

void MainComponent::setStartupOpenTiming(const juce::File& file,
                                         double openRequestedAtMs,
                                         double windowShownAtMs,
                                         std::optional<double> primaryContentShownElapsedMs,
                                         std::optional<double> fileNameShownElapsedMs,
                                         std::optional<double> firstWaveformElapsedMs)
{
    if (!file.existsAsFile() || openRequestedAtMs <= 0.0)
        return;

    pendingStartupTimingFile = file;
    pendingStartupOpenRequestedAtMs = openRequestedAtMs;
    pendingStartupWindowShownAtMs = windowShownAtMs;
    pendingStartupPrimaryContentShownElapsedMs = primaryContentShownElapsedMs;
    pendingStartupFileNameShownElapsedMs = fileNameShownElapsedMs;
    pendingStartupFirstWaveformElapsedMs = firstWaveformElapsedMs;

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    if (activeLoadTimingProbe.has_value() && activeLoadTimingProbe->file == file && openRequestedAtMs > 0.0)
    {
        auto& probe = *activeLoadTimingProbe;
        const auto alreadyHasMeasuredMilestone = probe.primaryContentElapsedMs.has_value()
                                              || probe.fileNameShownElapsedMs.has_value()
                                              || probe.firstWaveformElapsedMs.has_value()
                                              || probe.playableElapsedMs.has_value()
                                              || probe.fullWaveformElapsedMs.has_value();

        if (!alreadyHasMeasuredMilestone)
            probe.openRequestedAtMs = openRequestedAtMs;

        if (windowShownAtMs > openRequestedAtMs && !probe.windowShownElapsedMs.has_value())
            probe.windowShownElapsedMs = windowShownAtMs - openRequestedAtMs;
        if (primaryContentShownElapsedMs.has_value() && !probe.primaryContentElapsedMs.has_value())
            probe.primaryContentElapsedMs = primaryContentShownElapsedMs;
        if (fileNameShownElapsedMs.has_value() && !probe.fileNameShownElapsedMs.has_value())
            probe.fileNameShownElapsedMs = fileNameShownElapsedMs;
        if (firstWaveformElapsedMs.has_value() && !probe.firstWaveformElapsedMs.has_value())
            probe.firstWaveformElapsedMs = firstWaveformElapsedMs;
        probe.fromStartupLaunch = true;
    }
#endif
}

void MainComponent::openPreparedInitialAudio(const juce::File& file,
                                             SingleTrackDocument::FileInfo previewInfo,
                                             std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                             std::shared_ptr<SingleTrackDocument::State> state)
{
    if (isBnkFile(file)) { beginBnkOpen(file); return; }
    if (!file.existsAsFile())
        return;

    const auto shouldBeginLoad = !loadInProgress || pendingAutoPlayFile != file;
    if (shouldBeginLoad)
        beginPreparedInitialAudioLoadInternal(file, previewInfo, previewPeakCache == nullptr, false);

    loadThreadPool.removeAllJobs(false, 0);
    const auto token = pendingLoadToken.load(std::memory_order_relaxed);
    if (previewInfo.isValid() && previewPeakCache != nullptr)
        showAsyncLoadPreview(token, previewInfo, previewPeakCache);

    completeAsyncLoad(token, file, std::move(state), std::move(previewPeakCache), {});
}

void MainComponent::startLoadTimingProbe(const juce::File& file)
{
#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    juce::ignoreUnused(file);
    return;
#else
    if (!file.existsAsFile())
        return;

    if (activeLoadTimingProbe.has_value()
        && activeLoadTimingProbe->file == file
        && loadInProgress)
    {
        return;
    }

    abandonActiveLoadTimingProbe("superseded");

    LoadTimingProbe probe;
    probe.file = file;
    probe.openRequestedAtMs = juce::Time::getMillisecondCounterHiRes();

    if (pendingStartupTimingFile == file && pendingStartupOpenRequestedAtMs > 0.0)
    {
        probe.openRequestedAtMs = pendingStartupOpenRequestedAtMs;
        if (pendingStartupWindowShownAtMs > pendingStartupOpenRequestedAtMs)
            probe.windowShownElapsedMs = pendingStartupWindowShownAtMs - pendingStartupOpenRequestedAtMs;
        probe.primaryContentElapsedMs = pendingStartupPrimaryContentShownElapsedMs;
        probe.fileNameShownElapsedMs = pendingStartupFileNameShownElapsedMs;
        probe.firstWaveformElapsedMs = pendingStartupFirstWaveformElapsedMs;
        probe.fromStartupLaunch = true;
        pendingStartupTimingFile = {};
        pendingStartupOpenRequestedAtMs = 0.0;
        pendingStartupWindowShownAtMs = 0.0;
        pendingStartupPrimaryContentShownElapsedMs.reset();
        pendingStartupFileNameShownElapsedMs.reset();
        pendingStartupFirstWaveformElapsedMs.reset();
    }

    activeLoadTimingProbe = std::move(probe);
    logLoadTimingIfReady(activeLoadTimingProbe->windowShownElapsedMs.has_value() ? "window-shown"
                                                                                 : "open-requested");
    if (activeLoadTimingProbe.has_value() && activeLoadTimingProbe->fileNameShownElapsedMs.has_value())
        logLoadTimingIfReady("file-name");
    if (activeLoadTimingProbe.has_value() && activeLoadTimingProbe->firstWaveformElapsedMs.has_value())
        logLoadTimingIfReady("first-waveform");
#endif
}

void MainComponent::markFileNameShownIfNeeded()
{
    if (CoreOpenProbe::isActive())
    {
        if (const auto state = document.getState(); state != nullptr
            && state->info.isValid()
            && filePillLabel.getText().contains(state->info.file.getFileName()))
        {
            CoreOpenProbe::mark(CoreOpenMilestone::fileName);
        }
    }

#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    return;
#else
    if (!activeLoadTimingProbe.has_value() || activeLoadTimingProbe->fileNameShownElapsedMs.has_value())
        return;

    if (filePillLabel.getText().contains(activeLoadTimingProbe->file.getFileName()))
    {
        activeLoadTimingProbe->fileNameShownElapsedMs = juce::Time::getMillisecondCounterHiRes() - activeLoadTimingProbe->openRequestedAtMs;
        logLoadTimingIfReady("file-name");
    }
#endif
}

void MainComponent::markPrimaryContentVisibleIfNeeded()
{
#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    return;
#else
    if (!activeLoadTimingProbe.has_value() || activeLoadTimingProbe->primaryContentElapsedMs.has_value())
        return;

    activeLoadTimingProbe->primaryContentElapsedMs = juce::Time::getMillisecondCounterHiRes() - activeLoadTimingProbe->openRequestedAtMs;
    logLoadTimingIfReady("primary-content");
#endif
}

void MainComponent::markLoadPlayableIfNeeded(const juce::File& file)
{
    if (audioEngine.hasExplicitOutputSelection() && !audioEngine.isSelectedOutputReady())
    {
        if (coreLifecycle.isCurrent(activeDocumentGeneration) && file == activeGenerationFile)
            pendingSelectedOutputPlayable = file;
        return;
    }
    pendingSelectedOutputPlayable = {};
    CoreOpenProbe::mark(CoreOpenMilestone::playable);
    if (coreLifecycle.isCurrent(activeDocumentGeneration) && file == activeGenerationFile)
    {
        CoreOpenCycleProbe::mark(activeDocumentGeneration.value, CoreOpenMilestone::playable);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const auto lifecycleResult = coreLifecycle.markPlayable(activeDocumentGeneration);
        if ((lifecycleResult == CoreMilestoneResult::applied || lifecycleResult == CoreMilestoneResult::coreReadyReached)
            && optionalLifecycleContext.observer != nullptr)
            optionalLifecycleContext.observer->milestone(optionalLifecycleContext,
                                                         OptionalLifecycleAction::playable);
        schedulePostCorePromotionIfNeeded(lifecycleResult);
#else
        schedulePostCorePromotionIfNeeded(coreLifecycle.markPlayable(activeDocumentGeneration));
#endif
    }
#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    juce::ignoreUnused(file);
    return;
#else
    if (!activeLoadTimingProbe.has_value()
        || activeLoadTimingProbe->playableElapsedMs.has_value()
        || activeLoadTimingProbe->file != file)
    {
        return;
    }

    activeLoadTimingProbe->playableElapsedMs = juce::Time::getMillisecondCounterHiRes() - activeLoadTimingProbe->openRequestedAtMs;
    logLoadTimingIfReady("playable");
#endif
}

void MainComponent::markWaveformPresentedIfNeeded(const juce::File& file, bool hasFullWaveform)
{
    CoreOpenProbe::mark(CoreOpenMilestone::firstWaveform);
    if (hasFullWaveform)
        CoreOpenProbe::mark(CoreOpenMilestone::fullWaveform);
    if (coreLifecycle.isCurrent(activeDocumentGeneration) && file == activeGenerationFile)
    {
        CoreOpenCycleProbe::mark(activeDocumentGeneration.value, CoreOpenMilestone::firstWaveform);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const auto lifecycleResult = coreLifecycle.markFirstWaveform(activeDocumentGeneration);
        if ((lifecycleResult == CoreMilestoneResult::applied || lifecycleResult == CoreMilestoneResult::coreReadyReached)
            && optionalLifecycleContext.observer != nullptr)
            optionalLifecycleContext.observer->milestone(optionalLifecycleContext,
                                                         OptionalLifecycleAction::firstWaveform);
        schedulePostCorePromotionIfNeeded(lifecycleResult);
#else
        schedulePostCorePromotionIfNeeded(coreLifecycle.markFirstWaveform(activeDocumentGeneration));
#endif
    }

#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    juce::ignoreUnused(file, hasFullWaveform);
    return;
#else
    if (!activeLoadTimingProbe.has_value() || activeLoadTimingProbe->file != file)
        return;

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - activeLoadTimingProbe->openRequestedAtMs;
    if (!activeLoadTimingProbe->firstWaveformElapsedMs.has_value())
        activeLoadTimingProbe->firstWaveformElapsedMs = elapsedMs;

    if (hasFullWaveform && !activeLoadTimingProbe->fullWaveformElapsedMs.has_value())
        activeLoadTimingProbe->fullWaveformElapsedMs = elapsedMs;

    logLoadTimingIfReady(hasFullWaveform ? "full-waveform" : "first-waveform");
#endif
}

void MainComponent::logLoadTimingIfReady(const juce::String& outcome)
{
#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    juce::ignoreUnused(outcome);
    return;
#else
    if (!activeLoadTimingProbe.has_value())
        return;

    auto& probe = *activeLoadTimingProbe;
    if (probe.lastLoggedOutcome == outcome)
        return;

    const auto logLine = juce::Time::getCurrentTime().toISO8601(true)
                       + " | open=" + probe.file.getFullPathName()
                       + " | outcome=" + outcome
                       + " | origin=" + (probe.fromStartupLaunch ? "startup-launch" : "in-app")
                       + " | " + formatTimingField("window", probe.windowShownElapsedMs)
                       + " | " + formatTimingField("primary", probe.primaryContentElapsedMs)
                       + " | " + formatTimingField("name", probe.fileNameShownElapsedMs)
                       + " | " + formatTimingField("wave", probe.firstWaveformElapsedMs)
                       + " | " + formatTimingField("playable", probe.playableElapsedMs)
                       + " | " + formatTimingField("full", probe.fullWaveformElapsedMs);
    juce::Logger::writeToLog(logLine);
    appendOpenTimingLogLine(logLine);
    probe.lastLoggedOutcome = outcome;
#endif
}

void MainComponent::abandonActiveLoadTimingProbe(const juce::String& outcome)
{
#if !SINGLE_EDITOR_ENABLE_OPEN_TIMING
    juce::ignoreUnused(outcome);
    activeLoadTimingProbe.reset();
    return;
#else
    if (!activeLoadTimingProbe.has_value())
        return;

    if (activeLoadTimingProbe->lastLoggedOutcome != outcome)
    {
        auto& probe = *activeLoadTimingProbe;
        const auto logLine = juce::Time::getCurrentTime().toISO8601(true)
                           + " | open=" + probe.file.getFullPathName()
                           + " | outcome=" + outcome
                           + " | origin=" + (probe.fromStartupLaunch ? "startup-launch" : "in-app")
                           + " | " + formatTimingField("window", probe.windowShownElapsedMs)
                           + " | " + formatTimingField("primary", probe.primaryContentElapsedMs)
                           + " | " + formatTimingField("name", probe.fileNameShownElapsedMs)
                           + " | " + formatTimingField("wave", probe.firstWaveformElapsedMs)
                           + " | " + formatTimingField("playable", probe.playableElapsedMs)
                           + " | " + formatTimingField("full", probe.fullWaveformElapsedMs);
        juce::Logger::writeToLog(logLine);
        appendOpenTimingLogLine(logLine);
    }

    activeLoadTimingProbe.reset();
#endif
}

void MainComponent::requestCloseApplication()
{
    if (closeApplicationPending) return;
    if (!cancelOrdinaryAudioLoad(false)) return;
    cancelBnkPreview();
    cancelBnkOpen();
    // Also fence a metadata-only/empty ordinary open, which has no return
    // image. A late failed-reader callback must not start bank work after close.
    pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
    loadThreadPool.removeAllJobs(false, 0); cancelCacheWarm();
    loadInProgress = false; pendingLoadLabel.clear(); pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
    closingOptionalOperations.clear();
    if (pendingVideoOperation && !pendingVideoOperation->report().terminal)
    {
        closingOptionalOperations.push_back(pendingVideoOperation);
        pendingVideoOperation->requestCancel();
        if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    }
    if (packedAudioLoad && !packedAudioLoad->operation->report().terminal)
        closingOptionalOperations.push_back(packedAudioLoad->operation);
    if (packedAudioLoad)
    {
        cancelPackedAudioLoad();
        pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
        loadThreadPool.removeAllJobs(false, 0);
        loadInProgress = false; pendingLoadLabel.clear(); pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
    }
    if (!closingOptionalOperations.empty()) loadThreadPool.removeAllJobs(false, 0);
    closeApplicationPending = true;
    closeDiscardedDocuments.clear();
    continueCloseApplication();
}

void MainComponent::cancelCloseApplication()
{
    closingOptionalOperations.clear();
    closeApplicationPending = false;
    recoveryCloseDraining = false;
    recoveryClosePrompt = false;
    closeDiscardedDocuments.clear();
    if (recoveryService && recoveryService->health().protection.closing)
    {
        const auto* entry = playlistModel.getCurrentEntry();
        recoveryService->cancelClose(entry && coreLifecycle.isCoreReady(activeDocumentGeneration)
                                    ? entry->id : juce::Uuid(nullptr));
        pollRecovery();
    }
    refreshUiState();
}

void MainComponent::finishCloseApplication()
{
    closingOptionalOperations.clear();
    if (recoveryRestore) recoveryRestore->close();
    closeApplicationPending = false;
    recoveryCloseDraining = false;
    recoveryClosePrompt = false;
    closeDiscardedDocuments.clear();
    if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
}

void MainComponent::continueCloseApplication()
{
    if (!closeApplicationPending) return;
    if (bankOpenLoad)
    {
        if (!bankOpenLoad->work->finished.load(std::memory_order_acquire))
        { setDocumentOperationStatus("Stopping the BNK open before closing..."); return; }
        // The worker can finish between the first cancellation and this check.
        // Release its settled candidate/input leases before any close callback.
        cancelBnkOpen();
    }
    if (bnkRuntime && bnkRuntime->isPreparing())
    { setDocumentOperationStatus("Stopping the BNK preview before closing..."); return; }
    if (std::any_of(closingOptionalOperations.begin(), closingOptionalOperations.end(),
        [](const auto& operation) { return !operation->report().terminal; }))
    { setDocumentOperationStatus("Stopping the optional audio task before closing..."); return; }
    closingOptionalOperations.clear();
    if (recoveryCloseDraining) { continueRecoveryClose(); return; }
    juce::Component::SafePointer<MainComponent> safeThis(this);
    if (saveCoordinator.isSaveInProgress())
    {
        saveCoordinator.whenIdle([safeThis](bool succeeded)
        {
            if (safeThis == nullptr) return;
            if (succeeded) safeThis->continueCloseApplication();
            else safeThis->cancelCloseApplication();
        });
        return;
    }
    const auto unsaved = workspaceSession.collectUnsavedDocuments();
    const auto next = std::find_if(unsaved.begin(), unsaved.end(), [this](const auto& item)
    {
        return std::none_of(closeDiscardedDocuments.begin(), closeDiscardedDocuments.end(),
                            [&](const auto& approved) { return approved.sameRevision(item); });
    });
    if (next == unsaved.end())
    {
        if (recoveryService)
        {
            recoveryCloseDraining = true;
            recoveryCloseStartedAtMs = juce::Time::getMillisecondCounterHiRes();
            recoveryService->beginClose();
            continueRecoveryClose();
        }
        else finishCloseApplication();
        return;
    }
    if (!next->current)
    {
        const auto index = playlistModel.indexOfEntry(next->recoveredEntry);
        if (index < 0) { cancelCloseApplication(); return; }
        // The preceding live Document has already been saved or explicitly
        // discarded. Use the full-image activation path, never loadFile.
        if (!playlistModel.setCurrentIndex(index)) loadPlaylistIndex(index);
        if (!workspaceSession.isActiveRecoveredDocument(next->recoveredEntry))
        { cancelCloseApplication(); return; }
    }
    confirmDiscardOrSaveIfNeeded("closing Single Editor",
        [safeThis]
        {
            if (safeThis == nullptr || !safeThis->closeApplicationPending) return;
            if (safeThis->document.hasUnsavedChanges())
            {
                // Only Discard can continue with a dirty Document. Remember the
                // exact image for this close attempt; any edit/history change
                // or fresh activation invalidates the decision automatically.
                const auto remaining = safeThis->workspaceSession.collectUnsavedDocuments();
                const auto current = std::find_if(remaining.begin(), remaining.end(), [](const auto& item) { return item.current; });
                if (current == remaining.end() || !current->image)
                { safeThis->cancelCloseApplication(); return; }
                safeThis->closeDiscardedDocuments.push_back(*current);
            }
            juce::MessageManager::callAsync([safeThis]
            { if (safeThis != nullptr) safeThis->continueCloseApplication(); });
        },
        [safeThis] { if (safeThis != nullptr) safeThis->cancelCloseApplication(); });
}

void MainComponent::attachRecoveryAfterCore()
{
    // A real progressive prefix is playable, but not independently recoverable
    // until its decoder/seal completes. Completion explicitly retries this hook.
    if (packedAudioLoad && packedAudioLoad->awaitingTerminal) return;
    const auto* entry = playlistModel.getCurrentEntry();
    if (!entry || !document.hasAudio() || bnkDocument.hasBank()
        || !coreLifecycle.isCoreReady(activeDocumentGeneration) || recoveryCloseDraining) return;
    ensureRecoveryService();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    recoveryService->setOptionalLifecycleContext(optionalLifecycleContext);
#endif
    recoveryService->attachPostCore(entry->id);
    pollRecovery();
}

void MainComponent::ensureRecoveryService()
{
    if (recoveryService) return;
    if (!recoveryDiagnostics) recoveryDiagnostics = std::make_shared<recovery::RecoveryDiagnosticState>();
    recovery::RecoveryService::Config settings; settings.root = recoveryRootDirectory();
    settings.diagnostics = recoveryDiagnostics;
    recoveryService = std::make_unique<recovery::RecoveryService>(document, saveCoordinator, std::move(settings));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    recoveryService->setOptionalLifecycleContext(optionalLifecycleContext);
#endif
}

void MainComponent::discoverRecoveryAfterCore()
{
    if (recoveryDiscoveryScheduled || recoveryCloseDraining) return;
    recoveryDiscoveryScheduled = true;
    const auto root = recoveryRootDirectory();
    const juce::Component::SafePointer<MainComponent> safeThis(this);
    try
    {
        auto result = std::make_shared<recovery::RecoveryDiscovery::Page>();
        recoveryDiscoveryCancelled = std::make_shared<std::atomic<bool>>(false);
        const auto cancellation = recoveryDiscoveryCancelled;
        recoveryDiscoveryWorker = std::thread([safeThis, root, result, cancellation]
        {
            // No permanent discovery service, watcher or timer on the no-owner
            // path. The bounded cursor and all storage handles die on this worker.
            try
            {
#if JUCE_WINDOWS
                if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                    result->status = { recovery::Error::resourceOrEnvironmentFault, "discovery_priority" };
#endif
                if (result->status)
                {
                    recovery::RecoveryDiscovery discovery(root == juce::File() ? root : recovery::RecoveryStorageLayout(root).journalDirectory());
                    *result = discovery.nextPage({}, [cancellation] { return cancellation->load(); });
                }
            }
            catch (...) { result->status = { recovery::Error::resourceOrEnvironmentFault, "discovery_worker_exception" }; }
            juce::MessageManager::callAsync([safeThis, result]
            {
                if (!safeThis) return;
                if (safeThis->recoveryDiscoveryWorker.joinable()) safeThis->recoveryDiscoveryWorker.join();
                // The user may already have edited while this low-priority
                // read was pending. Do not advertise that same live producer
                // as a newly discovered recovery copy.
                std::set<juce::String> live;
                if (safeThis->recoveryService)
                    for (const auto& session : safeThis->recoveryService->health().protection.sessions)
                        live.insert(session.protection.current.owner.toString());
                juce::String notice;
                if (!result->status) notice = "Recovery discovery needs attention";
                else if (std::any_of(result->rows.begin(), result->rows.end(), [&](const auto& row)
                         { return !live.contains(row.owner.toString()) && !row.summary.retired && row.summary.status.code != "owner_collecting"; }))
                    notice = "Recovery records available";
                else if (result->more) notice = "More recovery records to check";
                safeThis->recoveryCandidateNotice = safeThis->recoveryCandidateNoticeDeferred ? juce::String{} : notice;
                if (result->status && result->presence && !safeThis->recoveryCloseDraining)
                {
                    safeThis->ensureRecoveryService();
                    safeThis->recoveryService->sweepTerminalOwnersAfterPresence();
                }
                safeThis->pollRecovery();
            });
        });
    }
    catch (...) { recoveryCandidateNotice = "Recovery discovery could not start"; pollRecovery(); }
}

void MainComponent::showRecoveryCenter()
{
    if (recoveryCloseDraining || loadInProgress || deferChromeForStartup) return;
    if (!recoveryCenter)
    {
        class Window final : public juce::DocumentWindow
        {
        public:
            Window() : DocumentWindow("Recovery Center", juce::Colours::darkgrey, DocumentWindow::closeButton) {}
            std::function<void()> onClosed;
            void closeButtonPressed() override
            {
                setVisible(false);
                if (onClosed) onClosed();
            }
            void lookAndFeelChanged() override
            {
                DocumentWindow::lookAndFeelChanged();
                setBackgroundColour(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
            }
        };
        auto window = std::make_unique<Window>();
        window->setUsingNativeTitleBar(true);
        window->setLookAndFeel(&getLookAndFeel());
        const juce::Component::SafePointer<MainComponent> safeThis(this);
        const juce::Component::SafePointer<Window> expected(window.get());
        window->onClosed = [safeThis, expected]
        {
            // Later can originate inside the content's button callback. Release
            // the view after that stack returns, without cancelling Main's jobs.
            juce::MessageManager::callAsync([safeThis, expected]
            {
                if (safeThis && expected && safeThis->recoveryCenter.get() == expected.getComponent()
                    && !expected->isVisible()) safeThis->recoveryCenter.reset();
            });
        };
        const auto root = recoveryRootDirectory();
        RecoveryCenterComponent::RestoreActions restore;
        restore.attemptRoot = root == juce::File() ? root : recovery::RecoveryStorageLayout(root).attemptDirectory();
        restore.appBuild = juce::JUCEApplication::getInstance() ? juce::JUCEApplication::getInstance()->getApplicationVersion() : "unknown";
        if (!recoveryDiagnostics) recoveryDiagnostics = std::make_shared<recovery::RecoveryDiagnosticState>();
        restore.diagnostics = recoveryDiagnostics;
        restore.showDiagnostics = [safeThis]
        { if (safeThis) safeThis->showLocalDiagnostics(std::set { diagnostics::ProviderKind::recovery }); };
        restore.storageSettings = [safeThis]() -> std::optional<RecoveryCenterComponent::StorageSettings>
        {
            if (!safeThis) return {};
            safeThis->ensureRecoveryService();
            return RecoveryCenterComponent::StorageSettings { safeThis->recoveryService->configuredStorageBudget(),
                safeThis->recoveryService->configuredOwnerByteLimit() };
        };
        restore.retentionChanged = [safeThis]
        {
            if (!safeThis || safeThis->closeApplicationPending) return;
            safeThis->ensureRecoveryService();
            safeThis->recoveryService->sweepTerminalOwnersAfterPresence(true);
            safeThis->pollRecovery();
        };
        restore.start = [safeThis, root](recovery::RecoveryRestoreQueue::Request request, juce::Uuid& attempt)
        {
            if (!safeThis || safeThis->closeApplicationPending)
                return recovery::Status { recovery::Error::resourceOrEnvironmentFault, "recovery_window_closing" };
            if (!safeThis->recoveryRestore)
            {
                recovery::RecoveryRestoreQueue::Config config; config.root = root;
                config.diagnostics = safeThis->recoveryDiagnostics;
                config.scratch = root == juce::File() ? juce::File() : root.getSiblingFile("RecoveryScratch");
                config.appBuild = juce::JUCEApplication::getInstance() ? juce::JUCEApplication::getInstance()->getApplicationVersion() : "unknown";
                safeThis->recoveryRestore = std::make_unique<recovery::RecoveryRestoreQueue>(std::move(config));
            }
            const auto status = safeThis->recoveryRestore->start(std::move(request), attempt);
            safeThis->pollRecoveryRestore(); return status;
        };
        restore.health = [safeThis]
        { return safeThis && safeThis->recoveryRestore ? safeThis->recoveryRestore->health() : recovery::RecoveryRestoreQueue::Health {}; };
        restore.cancel = [safeThis](juce::Uuid attempt)
        {
            if (!safeThis || !safeThis->recoveryRestore) return recovery::Status { recovery::Error::staleHead, "restore_unavailable" };
            const auto status = safeThis->recoveryRestore->cancel(attempt); safeThis->pollRecoveryRestore(); return status;
        };
        restore.retry = [safeThis](juce::Uuid previous, juce::Uuid& attempt)
        {
            if (!safeThis || !safeThis->recoveryRestore || safeThis->closeApplicationPending)
                return recovery::Status { recovery::Error::staleHead, "restore_unavailable" };
            const auto status = safeThis->recoveryRestore->retry(previous, false, attempt); safeThis->pollRecoveryRestore(); return status;
        };
        restore.forget = [safeThis](juce::Uuid attempt)
        {
            if (!safeThis || !safeThis->recoveryRestore) return recovery::Status { recovery::Error::staleHead, "restore_unavailable" };
            const auto status = safeThis->recoveryRestore->forget(attempt); safeThis->pollRecoveryRestore(); return status;
        };
        window->setContentOwned(new RecoveryCenterComponent(root == juce::File() ? root : recovery::RecoveryStorageLayout(root).journalDirectory(),
            [safeThis](std::unique_ptr<recovery::OwnerDiscardReview>& review)
            {
                if (!safeThis || safeThis->recoveryCloseDraining)
                    return recovery::Status { recovery::Error::resourceOrEnvironmentFault, "recovery_window_closing" };
                safeThis->ensureRecoveryService();
                const auto status = safeThis->recoveryService->submitConfirmedDiscard(review);
                safeThis->pollRecovery(); return status;
            }, [safeThis] { if (safeThis) safeThis->retryRecovery(); }, std::move(restore)), true);
        auto* content = static_cast<RecoveryCenterComponent*>(window->getContentComponent());
        content->onResumeCleanup = [safeThis](juce::Uuid owner, const recovery::SafeHead& head)
        {
            if (!safeThis || safeThis->recoveryCloseDraining)
                return recovery::Status { recovery::Error::resourceOrEnvironmentFault, "recovery_window_closing" };
            safeThis->ensureRecoveryService();
            const auto status = safeThis->recoveryService->resumeTerminalCleanup(owner, head);
            safeThis->pollRecovery(); return status;
        };
        content->onLater = [safeThis, expected]
        {
            if (!safeThis || !expected) return;
            // Defer only this session's discovery notice, including a discovery
            // result still in flight. Protection/restore failures remain visible.
            safeThis->recoveryCandidateNoticeDeferred = true;
            safeThis->recoveryCandidateNotice.clear();
            expected->closeButtonPressed();
            safeThis->pollRecovery();
        };
        window->setResizable(true, false); window->setResizeLimits(740, 680, 1200, 1000);
        window->centreAroundComponent(this, 800, 780);
        recoveryCenter = std::move(window);
    }
    // Explicit command only: discovery never opens, focuses or activates this UI.
    recoveryCenter->setVisible(true); recoveryCenter->toFront(true);
}

void MainComponent::pollRecoveryRestore()
{
    if (!recoveryRestore) return;
    for (auto& publication : recoveryRestore->takePublications())
    {
        if (publication.core)
        {
            // This is a new, unselected playlist workspace with complete
            // history. Do not call loadFile or change transport/current audio.
            const bool admitted = !closeApplicationPending && attachRecoveredCandidate(*publication.core);
            recoveryRestore->acknowledgeCore(publication.attempt, admitted);
        }
        if (publication.fullPeaks)
            publishRecoveredPeaks(publication.attempt, std::move(publication.originalCore), std::move(publication.fullPeaks));
    }
    const auto status = recoveryRestore->health();
    const bool failed = !status.failure || std::any_of(status.jobs.begin(), status.jobs.end(),
        [](const auto& job) { return job.stage == recovery::RecoveryAttemptStage::failed; });
    const bool pending = std::any_of(status.jobs.begin(), status.jobs.end(), [](const auto& job)
    { return job.stage != recovery::RecoveryAttemptStage::ready && job.stage != recovery::RecoveryAttemptStage::failed && job.stage != recovery::RecoveryAttemptStage::cancelled; });
    recoveryRestoreNotice = failed ? "Restore needs attention" : pending ? "Restoring recovery copy..."
        : status.jobs.empty() ? juce::String() : "Recovery restore tasks finished";
    if (recoveryCenter)
        if (auto* content = dynamic_cast<RecoveryCenterComponent*>(recoveryCenter->getContentComponent())) content->updateRestoreJobs();
    pollRecovery();
}

juce::Rectangle<int> MainComponent::layoutRecoveryStatus()
{
    auto content = getLocalBounds();
    if (!recoveryStatusButton || !recoveryStatusButton->isVisible()) return content;
    // Reserve space while a recovery action is present. An overlay would cover
    // the Simple waveform's scrollbar, or the bottom row of a General module.
    auto row = content.removeFromBottom(44).reduced(12, 7);
    recoveryStatusButton->setBounds(row.removeFromLeft(juce::jmin(340, row.getWidth())));
    return content;
}

void MainComponent::pollRecovery()
{
    if (recoveryService && recoveryService->needsPoll()) recoveryService->poll();
    const auto status = recoveryService ? recoveryService->health() : recovery::RecoveryService::Health {};
    if (status.maintenance.sweep.completed && status.maintenance.sweep.preservedOwners == 0
        && (recoveryCandidateNotice == "More recovery records to check" || recoveryCandidateNotice == "Recovery records available"))
        recoveryCandidateNotice.clear();
    const bool failed = !status.protection.failure || !status.maintenance.failure || !status.dispositionFailure
        || !status.maintenance.sweep.failure
        || std::any_of(status.protection.sessions.begin(), status.protection.sessions.end(),
                       [](const auto& item) { return item.protection.checkpointRequired; });
    juce::String text;
    if (failed)
    {
        const bool currentFailed = document.hasUnsavedChanges()
            && std::none_of(status.protection.sessions.begin(), status.protection.sessions.end(),
                            [](const auto& item) { return item.active && item.protection.currentProtected; });
        text = currentFailed ? "Latest changes aren't protected" : "Recovery needs attention";
    }
    else if (status.protection.pendingPackets != 0) text = "Protecting changes...";
    else if (status.waitingRetirements != 0 || status.protection.pendingRetirements != 0 || !status.maintenance.jobs.empty()
        || status.maintenance.sweep.active)
        text = "Finishing recovery cleanup...";
    if (text.isEmpty()) text = recoveryRestoreNotice.isNotEmpty() ? recoveryRestoreNotice : recoveryCandidateNotice;
    if (text == recoveryStatusText) return;
    recoveryStatusText = text;
    if (text.isNotEmpty() && !recoveryStatusButton)
    {
        recoveryStatusButton = std::make_unique<juce::TextButton>();
        recoveryStatusButton->setName("Recovery status and actions");
        recoveryStatusButton->onClick = [this]
        {
            if (recoveryStatusText == recoveryCandidateNotice || recoveryStatusText == recoveryRestoreNotice) showRecoveryCenter();
            else showRecoveryActions();
        };
        addChildComponent(*recoveryStatusButton);
    }
    if (recoveryStatusButton)
    {
        recoveryStatusButton->setButtonText(text);
        recoveryStatusButton->setTooltip(text + ". Open recovery actions.");
        const bool visibilityChanged = recoveryStatusButton->isVisible() != text.isNotEmpty();
        recoveryStatusButton->setVisible(text.isNotEmpty());
        if (visibilityChanged) resized();
    }
}

void MainComponent::retryRecovery()
{
    if (!recoveryService) return;
    const auto status = recoveryService->health();
    for (const auto& item : status.protection.sessions)
    {
        const auto retry = [this](const auto& epoch)
        {
            if (!epoch.owner.isNull() && !epoch.writer.workerRunning && !epoch.failure)
                recoveryService->retryProtection(epoch.owner, true);
        };
        retry(item.protection.current);
        for (const auto& epoch : item.protection.retired) retry(epoch);
        if (item.active && item.protection.checkpointRequired && !recoveryCloseDraining)
            recoveryService->checkpointCurrent();
    }
    for (const auto& job : status.maintenance.jobs)
        if (job.phase == recovery::RecoveryMaintenanceQueue::Phase::failed
            || (job.phase == recovery::RecoveryMaintenanceQueue::Phase::queued && !status.maintenance.workerRunning))
            recoveryService->retryMaintenance(job.owner);
    if (!recoveryCloseDraining)
    {
        if (!status.maintenance.sweep.failure
            || (status.maintenance.sweep.active && !status.maintenance.workerRunning && !status.maintenance.failure))
            recoveryService->sweepTerminalOwnersAfterPresence();
        attachRecoveryAfterCore();
    }
    pollRecovery();
}

void MainComponent::showRecoveryActions()
{
    if (!recoveryService || recoveryClosePrompt) return;
    const auto status = recoveryService->health();
    const bool currentUnprotected = document.hasUnsavedChanges()
        && std::none_of(status.protection.sessions.begin(), status.protection.sessions.end(),
                        [](const auto& item) { return item.active && item.protection.currentProtected; });
    const juce::String message = currentUnprotected || status.protection.pendingPackets != 0 || !status.protection.failure
        ? "Some edits have not reached a durable recovery point. Editing and playback can continue. Save As writes an explicit audio file."
        : "Recovery cleanup is unfinished. Existing saved audio and recovery data are preserved while it waits or needs attention.";
    auto* dialog = new juce::AlertWindow("Recovery", message + recoverySourceAdvice(status.protection)
        + recoveryCapacityAdvice(status), juce::MessageBoxIconType::InfoIcon, this);
    dialog->addButton("Retry", 1); dialog->addButton("Save As", 2); dialog->addButton("Diagnostics", 3); dialog->addButton("Close", 0);
    dialog->addButton("Recovery Center", 4);
    const juce::Component::SafePointer<MainComponent> safeThis(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result)
    {
        if (safeThis == nullptr || !safeThis->recoveryService) return;
        if (result == 1) safeThis->retryRecovery();
        else if (result == 2) safeThis->saveDocument(true);
        else if (result == 4) safeThis->showRecoveryCenter();
        else if (result == 3)
            safeThis->showLocalDiagnostics(std::set { diagnostics::ProviderKind::recovery });
    }), true);
}

bool MainComponent::hasUnconfirmedCloseChanges() const
{
    const auto dirty = workspaceSession.collectUnsavedDocuments();
    return std::any_of(dirty.begin(), dirty.end(), [this](const auto& item)
    {
        return std::none_of(closeDiscardedDocuments.begin(), closeDiscardedDocuments.end(),
                            [&](const auto& approved) { return approved.sameRevision(item); });
    });
}

void MainComponent::continueRecoveryClose()
{
    if (!closeApplicationPending || !recoveryCloseDraining || !recoveryService || recoveryClosePrompt) return;
    if (hasUnconfirmedCloseChanges())
    {
        cancelCloseApplication();
        setDocumentOperationStatus("Closing cancelled because the document changed.");
        return;
    }
    pollRecovery();
    const auto status = recoveryService->health();
    if (status.settled) { finishCloseApplication(); return; }
    // Pilot wait bound; a longer wait is always an explicit user action. This
    // uses the existing UI timer, never a recovery timer on ordinary open.
    if (juce::Time::getMillisecondCounterHiRes() - recoveryCloseStartedAtMs < 750.0) return;
    recoveryClosePrompt = true;
    const bool protectionUnfinished = status.protection.pendingPackets != 0 || !status.protection.failure
        || (!status.dispositionFailure && status.dispositionFailure.code == "service_root")
        || std::any_of(status.protection.sessions.begin(), status.protection.sessions.end(),
                       [](const auto& item) { return item.protection.checkpointRequired; });
    const juce::String message = protectionUnfinished
        ? "Recovery has not finished. Exit Anyway may lose edits after the last durable recovery point; existing recovery data is preserved."
        : "Recovery cleanup has not finished. Saved audio is safe. Exit Anyway keeps the unfinished recovery data for later handling.";
    auto* dialog = new juce::AlertWindow("Recovery still pending", message + recoverySourceAdvice(status.protection)
        + recoveryCapacityAdvice(status), juce::MessageBoxIconType::WarningIcon, this);
    dialog->addButton("Wait", 1); dialog->addButton("Retry", 2); dialog->addButton("Save As", 3);
    dialog->addButton("Exit Anyway", 4); dialog->addButton("Cancel", 0);
    const juce::Component::SafePointer<MainComponent> safeThis(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis](int result)
    {
        if (safeThis == nullptr || !safeThis->closeApplicationPending) return;
        safeThis->recoveryClosePrompt = false;
        if (safeThis->hasUnconfirmedCloseChanges()) { safeThis->cancelCloseApplication(); return; }
        if (result == 4) { safeThis->finishCloseApplication(); return; }
        if (result == 1 || result == 2)
        {
            safeThis->recoveryCloseStartedAtMs = juce::Time::getMillisecondCounterHiRes();
            if (result == 2) safeThis->retryRecovery();
            safeThis->continueRecoveryClose();
            return;
        }
        safeThis->cancelCloseApplication();
        if (result == 3) safeThis->saveDocument(true, [safeThis](bool saved)
        { if (safeThis != nullptr && saved && !safeThis->document.hasUnsavedChanges()) safeThis->requestCloseApplication(); });
    }), true);
}

void MainComponent::setStartupCoreReadyCallback(std::function<void()> callback)
{
    onStartupCoreReady = std::move(callback);
    notifyStartupCoreReadyIfNeeded();
}

void MainComponent::setStartupCoreSurfacePreparedCallback(std::function<void()> callback)
{
    onStartupCoreSurfacePrepared = std::move(callback);
    notifyStartupCoreSurfacePreparedIfNeeded();
}

void MainComponent::notifyStartupCoreSurfacePreparedIfNeeded()
{
    if (!startupHasInitialFile || !deferChromeForStartup
        || startupCoreSurfacePreparedNotified || !onStartupCoreSurfacePrepared)
        return;
    const auto state = waveformModel.getState();
    const auto peaks = waveformModel.getPeakCache();
    if (state == nullptr || !state->info.isValid() || peaks == nullptr
        || peaks->audioRevision != state->audioRevision || peaks->binCount <= 0)
        return;
    // Allow the minimal Core view to become visible and paint. This is only a
    // surface handoff request, not a waveform or CoreReady milestone.
    startupCoreSurfacePreparedNotified = true;
    onStartupCoreSurfacePrepared();
}

void MainComponent::setStartupRendererPrepareCallback(std::function<void()> callback)
{
    prepareStartupRenderer = std::move(callback);
}

void MainComponent::setStartupRendererReadyCallback(std::function<void(bool)> callback)
{
    onStartupRendererReady = std::move(callback);
    if (!deferChromeForStartup && startupHasInitialFile)
        notifyStartupRendererReadyIfNeeded(coreLifecycle.isCoreReady(activeDocumentGeneration));
}

void MainComponent::beginDocumentGeneration(const juce::File& file)
{
    const auto cycleStarted = CoreOpenCycleProbe::captureStart();
    cancelCacheWarm();
    if (meterBridge) meterBridge->setCoreReady(false);
    if (auto* panel = getOutputMeterPanel()) panel->setCoreReady(false);
    if (workspaceModuleHost)
        if (auto* panel = dynamic_cast<SpatialEnergyPanelComponent*>(
            workspaceModuleHost->getModuleComponent(WorkspaceModuleId::spatialEnergy)))
            panel->setCoreReady(false);
    cancelPackedAudioLoad();
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    pendingVideoOperation.reset();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    if (recoveryCloseDraining) cancelCloseApplication();
    if (recoveryService) recoveryService->leaveDocument();
    // A file switch rebuilds the lazy Host and temporarily reparents its borrowed
    // Playlist view. Preserve its exact focus owner for this generation only.
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    playlistFocusBeforeLoad = focused != nullptr && playlistSidebar
        && (focused == playlistSidebar.get() || playlistSidebar->isParentOf(focused)) ? focused : nullptr;
    failedLoadFileName.clear();
    failedLoadMessage.clear();
    cancelTruePeakLimiterApply(false);
    clearTruePeakLimiterPreview(true);
    truePeakLimiterProcessor.reset();
    activeTruePeakLimiterPlan.reset();
    lastAppliedTruePeakLimiterPlan.reset();
    truePeakLimiterParameters = TruePeakLimiterParameters::masteringDefault();
    cancelParametricEqApply(false);
    clearParametricEqPreview(true);
    parametricEqProcessor.reset();
    activeParametricEqPlan.reset();
    lastAppliedParametricEqPlan.reset();
    cancelReferenceAudioSuiteApply(false);
    clearReferenceAudioSuitePreview(true);
    referenceAudioSuiteProcessor.reset();
    lastAppliedReferenceAudioSuitePlan.reset();
    selectedSourceChannels.clear();
    waveformView.setSelectedSourceChannels({});
    activeDocumentGeneration = coreLifecycle.beginGeneration();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    optionalLifecycleContext.generation = activeDocumentGeneration.value;
    audioEngine.setOptionalLifecycleContext(optionalLifecycleContext);
    if (recoveryService) recoveryService->setOptionalLifecycleContext(optionalLifecycleContext);
    if (optionalLifecycleContext.observer != nullptr)
        optionalLifecycleContext.observer->milestone(optionalLifecycleContext,
                                                     OptionalLifecycleAction::generationBegan);
#endif
    CoreOpenCycleProbe::begin(activeDocumentGeneration.value, cycleStarted);
    waveformView.beginPresentationGeneration();
    if (metadataSidebar) metadataSidebar->beginTypedMetadataGeneration(activeDocumentGeneration.value);
    commandRegistry.setActiveDocumentGeneration(activeDocumentGeneration.value);
    commandRegistry.unregisterProvider("workspace");
    workspaceCommandProviderRegistered = false;
    ++workspaceFocusGeneration;
    if (commandPalette != nullptr)
    {
        commandPalette->setVisible(false);
        removeChildComponent(commandPalette.get());
        commandPalette.reset();
    }
    commandPaletteReturnToken.reset();
    activeGenerationFile = file;
    pendingSelectedOutputPlayable = {};
    postCorePromotionScheduled = false;
    workspaceModuleHost.reset();
    outputMeterWorkspaceModuleRegistered = false;
    ambisonicWorkspaceModuleRegistered = false;
    parametricEqWorkspaceModuleRegistered = false;
    truePeakLimiterWorkspaceModuleRegistered = false;
}

void MainComponent::ensureDocumentGeneration(const juce::File& file)
{
    if (!coreLifecycle.isCurrent(activeDocumentGeneration) || activeGenerationFile != file)
        beginDocumentGeneration(file);
}

void MainComponent::schedulePostCorePromotionIfNeeded(CoreMilestoneResult result)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (result == CoreMilestoneResult::coreReadyReached && optionalLifecycleContext.observer != nullptr)
        optionalLifecycleContext.observer->milestone(optionalLifecycleContext,
                                                     OptionalLifecycleAction::coreReady);
#endif
    if (result == CoreMilestoneResult::coreReadyReached)
        notifyStartupCoreReadyIfNeeded();

    if (result != CoreMilestoneResult::coreReadyReached || postCorePromotionScheduled)
        return;

    postCorePromotionScheduled = true;
    const auto generation = activeDocumentGeneration;
    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this), generation]
    {
        if (safeThis == nullptr
            || !safeThis->coreLifecycle.isCurrent(generation)
            || !safeThis->coreLifecycle.isCoreReady(generation))
        {
            return;
        }

        safeThis->postCorePromotionScheduled = false;
        safeThis->ensureDeferredWorkspaceControls();
        safeThis->waveformView.notifyCoreReady();
        safeThis->metadataSidebar->activateTypedMetadataInspector(
            generation.value,
            safeThis->document.getFileInfo());
        const auto loadedInfo = safeThis->document.getFileInfo();
        const auto* currentEntry = safeThis->playlistModel.getCurrentEntry();
        if (loadedInfo.isValid() && (currentEntry == nullptr || currentEntry->file != loadedInfo.file))
        {
            // Prepared startup bypasses openFiles: publish its already playable
            // document only after CoreReady, without re-entering the audio loader.
            const juce::ScopedValueSetter<bool> suppress(safeThis->suppressPlaylistLoad, true);
            safeThis->playlistModel.addFiles({ loadedInfo.file });
            for (int index = 0; index < safeThis->playlistModel.getNumEntries(); ++index)
                if (!safeThis->playlistModel.getEntry(index)->recovered
                    && safeThis->playlistModel.getEntry(index)->file == loadedInfo.file)
                {
                    safeThis->playlistModel.setCurrentIndex(index);
                    break;
                }
            safeThis->workspaceSession.rememberOpenedFiles({ loadedInfo.file });
            safeThis->recentButton.setEnabled(safeThis->workspaceSession.hasRecentFiles());
            safeThis->requestSessionSettingsPersist();
        }
        safeThis->playlistSidebar->updateLoadedDuration(loadedInfo.file, loadedInfo.durationSeconds);
        if (safeThis->deferChromeForStartup)
        {
            safeThis->promoteDeferredChromeIfNeeded();
        }
        else
        {
            safeThis->ensureDeferredOutputMeter();
            safeThis->ensureAmbisonicFieldPanelIfNeeded();
            safeThis->applyChromePresentation();
        }
        const auto focusToRestore = safeThis->playlistFocusBeforeLoad;
        safeThis->playlistFocusBeforeLoad = nullptr;
        // Consume only after CoreReady and the final Host layout. A different
        // focused control or an inactive window means the user moved elsewhere.
        // Never reactivate the window or replace that newer focus decision.
        if (focusToRestore != nullptr && focusToRestore->isShowing()
            && focusToRestore->isEnabled() && focusToRestore->getWantsKeyboardFocus()
            && juce::Component::getCurrentlyFocusedComponent() == safeThis.getComponent()
            && safeThis->getPeer() != nullptr && safeThis->getPeer()->isFocused())
        {
            focusToRestore->grabKeyboardFocus();
        }
        safeThis->attachRecoveryAfterCore();
        safeThis->discoverRecoveryAfterCore();
    });
}

void MainComponent::notifyStartupCoreReadyIfNeeded()
{
    if (!startupHasInitialFile
        || startupCoreReadyNotified
        || !coreLifecycle.isCoreReady(activeDocumentGeneration)
        || !onStartupCoreReady)
    {
        return;
    }

    startupCoreReadyNotified = true;
    onStartupCoreReady();
}

void MainComponent::notifyStartupRendererReadyIfNeeded(bool succeeded)
{
    if (!startupHasInitialFile || startupRendererReadyNotified || !onStartupRendererReady)
        return;

    startupRendererReadyNotified = true;
    onStartupRendererReady(succeeded);
}

void MainComponent::setStartupTransitionSnapshot(juce::Image snapshot)
{
    startupTransitionSnapshot = std::move(snapshot);
    startupTransitionClearScheduled = false;
    if (startupTransitionSnapshot.isValid())
        repaint();
}

bool MainComponent::isChromeDeferredForStartup() const noexcept
{
    return deferChromeForStartup;
}

void MainComponent::lookAndFeelChanged()
{
    if (RendererVisualStyle::isClassic()) return;
    lightThemeButton.setToggleState(!RendererVisualStyle::isNight(), juce::dontSendNotification);
    nightThemeButton.setToggleState(RendererVisualStyle::isNight(), juce::dontSendNotification);
    footerBar.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::mutedSurface().getARGB()));
    selectionPopup.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::elevatedPanelBackground().getARGB()));
    if (bnkControls)
        bnkControls->surface.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::panelBackground().getARGB()));
    for (auto* label : { &selectionGainLabel, &filePillLabel })
        label->setColour(juce::Label::textColourId, ModernAppleLookAndFeel::primaryText());
    for (auto* label : std::initializer_list<juce::Label*> { &engineStatusLabel, &monitorLabel })
        label->setColour(juce::Label::textColourId, ModernAppleLookAndFeel::secondaryText());
    if (bnkControls)
        for (auto* label : { &bnkControls->switchLabel, &bnkControls->stateLabel })
            label->setColour(juce::Label::textColourId, ModernAppleLookAndFeel::secondaryText());
    transportStatusLabel.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::accentBlue());
    peakLabel.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::accentOrange());
    footerLeftLabel.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::secondaryText().withAlpha(0.92f));
    footerRightLabel.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::primaryText().withAlpha(0.82f));
}

void MainComponent::applyChromePresentation(bool relayoutNow)
{
    if (!deferChromeForStartup)
        ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);

    actionBar.getProperties().set("fillAlpha", deferChromeForStartup ? 0.96 : (RendererVisualStyle::isClassic() ? 0.0 : 1.0));
    actionBar.getProperties().set("shadowAlpha", deferChromeForStartup ? 0.012 : 0.0);
    actionBar.getProperties().set("glassChrome", !deferChromeForStartup && !RendererVisualStyle::isClassic());
    footerBar.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::mutedSurface().getARGB()));
    footerBar.getProperties().set("fillAlpha", deferChromeForStartup ? 0.0 : 0.98);
    selectionPopup.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::elevatedPanelBackground().getARGB()));
    if (bnkControls)
        bnkControls->surface.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::panelBackground().getARGB()));

    const auto showExtendedChrome = !deferChromeForStartup && !modernSimpleView;
    for (auto* component : std::initializer_list<juce::Component*> {
             &actionBar, &actionDragZone, &openButton, &playButton, &pauseButton,
             &stopButton, &filePillLabel, &engineStatusLabel, &transportStatusLabel })
        component->setVisible(!modernSimpleView);
    const auto showLegacyToolbarCommands = showExtendedChrome && RendererVisualStyle::isClassic();
    recentButton.setVisible(deferChromeForStartup || RendererVisualStyle::isClassic());
    preferencesButton.setVisible(deferChromeForStartup || RendererVisualStyle::isClassic());
    toolbarMoreButton.setVisible(showExtendedChrome && !RendererVisualStyle::isClassic());
    generalSceneButton.setVisible(showExtendedChrome && !RendererVisualStyle::isClassic());
    simpleViewButton.setVisible(showExtendedChrome && !RendererVisualStyle::isClassic());
    lightThemeButton.setVisible(showExtendedChrome && !RendererVisualStyle::isClassic());
    nightThemeButton.setVisible(showExtendedChrome && !RendererVisualStyle::isClassic());
    lightThemeButton.setToggleState(!RendererVisualStyle::isNight(), juce::dontSendNotification);
    nightThemeButton.setToggleState(RendererVisualStyle::isNight(), juce::dontSendNotification);
    transportStatusLabel.setModernPresentation(showExtendedChrome && !RendererVisualStyle::isClassic());
    for (auto* label : { &engineStatusLabel, &footerLeftLabel, &footerRightLabel })
        label->setModernPresentation(showExtendedChrome && !RendererVisualStyle::isClassic());
    generalSceneButton.setEnabled(workspaceModuleHost != nullptr);
    simpleViewButton.setEnabled(workspaceModuleHost != nullptr);
    generalSceneButton.setToggleState(!modernSimpleView, juce::dontSendNotification);
    simpleViewButton.setToggleState(modernSimpleView, juce::dontSendNotification);
    titleBar.setVisible(showExtendedChrome && !hostProvidesTitleBar);
    footerBar.setVisible(showExtendedChrome);
    footerLeftLabel.setVisible(showExtendedChrome);
    footerRightLabel.setVisible(showExtendedChrome);
    // Playlist and Metadata have no fixed-sidebar presentation after startup.
    // Until the workspace host exists they stay absent instead of reviving a
    // second, incompatible layout system.
    if (workspaceModuleHost == nullptr)
    {
        if (playlistSidebar) playlistSidebar->setVisible(false);
        if (metadataSidebar) metadataSidebar->setVisible(false);
    }
    if (workspaceModuleHost != nullptr)
        workspaceModuleHost->setVisible(!deferChromeForStartup && workspaceModuleHost->hasActiveModules());
    playlistResizeHandle.setVisible(false);
    metadataResizeHandle.setVisible(false);
    // Modern Mags own their headers and visibility controls. The fixed-sidebar
    // arrows would otherwise cover the Waveform title/ruler; More and the
    // command palette retain the same sidebar commands.
    togglePlaylistSidebarButton.setVisible(showLegacyToolbarCommands);
    toggleMetadataSidebarButton.setVisible(showLegacyToolbarCommands);

    removeFileButton.setVisible(showLegacyToolbarCommands);
    saveButton.setVisible(showLegacyToolbarCommands);
    saveAsButton.setVisible(showLegacyToolbarCommands);
    startButton.setVisible(showExtendedChrome);
    previousButton.setVisible(showExtendedChrome);
    nextButton.setVisible(showExtendedChrome);
    returnToStartButton.setVisible(showExtendedChrome);
    loopButton.setVisible(showExtendedChrome);
    autoPlayButton.setVisible(showLegacyToolbarCommands);
    monitorLabel.setVisible(showExtendedChrome);
    monitorSlider.setVisible(showExtendedChrome);
    meterModeBox.setVisible(showExtendedChrome);
    if (meterBridge) meterBridge->setVisible(showExtendedChrome);
    peakLabel.setVisible(showExtendedChrome);
    gainDownButton.setVisible(showExtendedChrome && selectionPopup.isVisible());
    gainUpButton.setVisible(showExtendedChrome && selectionPopup.isVisible());
    selectionGainLabel.setVisible(showExtendedChrome && selectionPopup.isVisible());
    selectionGainSlider.setVisible(showExtendedChrome && selectionPopup.isVisible());
    applySelectionGainButton.setVisible(showExtendedChrome && selectionPopup.isVisible());
    cancelSelectionGainButton.setVisible(showExtendedChrome && selectionPopup.isVisible());
    fadeInButton.setVisible(showLegacyToolbarCommands);
    fadeOutButton.setVisible(showLegacyToolbarCommands);
    deleteSelectionButton.setVisible(showLegacyToolbarCommands);
    undoButton.setVisible(showExtendedChrome);
    redoButton.setVisible(showExtendedChrome);
    resetEnvelopeButton.setVisible(showLegacyToolbarCommands);
    timeZoomOutButton.setVisible(showLegacyToolbarCommands);
    timeZoomInButton.setVisible(showLegacyToolbarCommands);
    ampZoomOutButton.setVisible(showLegacyToolbarCommands);
    ampZoomInButton.setVisible(showLegacyToolbarCommands);
    resetViewButton.setVisible(showLegacyToolbarCommands);

    if (!showExtendedChrome)
        selectionPopup.setVisible(false);

    updateSidebarToggleButtons();
    updateTransportAvailability();
    updateSelectionPopup();
    updateCenterViewVisibility();
    if (relayoutNow)
    {
        resized();
        repaint();
    }
}

void MainComponent::promoteDeferredChromeIfNeeded(int delayMs)
{
    if (!deferChromeForStartup)
        return;

    if (delayMs > 0)
    {
        if (deferredChromePromotionPending)
            return;

        deferredChromePromotionPending = true;
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::Timer::callAfterDelay(delayMs,
                                    [safeThis]
                                    {
                                        if (safeThis == nullptr)
                                            return;

                                        safeThis->deferredChromePromotionPending = false;
                                        safeThis->promoteDeferredChromeIfNeeded();
                                    });
        return;
    }

    deferredChromePromotionPending = false;
    if (startupHasInitialFile && !startupRendererPrepared)
    {
        startupRendererPrepared = true;
        if (prepareStartupRenderer)
            prepareStartupRenderer();
    }

    deferChromeForStartup = false;
    ensureDeferredWorkspaceControls();
    registerPostCoreCommandProvider();
    applyDeferredStartupUiSettingsIfNeeded();
    ensureDeferredOutputMeter();
    ensureAmbisonicFieldPanelIfNeeded();
    applyChromePresentation();
    refreshTransportReadout();
    refreshFooterReadout();
    notifyStartupRendererReadyIfNeeded(coreLifecycle.isCoreReady(activeDocumentGeneration));
}

void MainComponent::ensureDeferredWorkspaceControls()
{
    // Startup installs the core view after waveform presentation, before the
    // selected output may be playable. Hidden optional views must not be
    // instantiated in that gap. Empty-workspace construction keeps its UI.
    if (startupHasInitialFile && !coreLifecycle.isCoreReady(activeDocumentGeneration))
        return;

    if (!playlistSidebar)
    {
        playlistSidebar = std::make_unique<PlaylistSidebarComponent>(playlistModel);
        playlistSidebar->setWantsKeyboardFocus(true);
        playlistSidebar->setComponentID("workspace.playlist");
        playlistSidebar->setTitle("Playlist");
        playlistSidebar->onEntryActivated = [this](int index) { requestPlaylistActivation(index); };
        playlistSidebar->onEntryRemoveRequested = [this](int index) { requestPlaylistRemoval(index); };
        playlistSidebar->onRevealFileRequested = [this](const juce::File& file)
        {
            if (file.existsAsFile()) file.revealToUser();
        };
        addChildComponent(*playlistSidebar);
    }
    if (!metadataSidebar)
    {
        metadataSidebar = std::make_unique<MetadataSidebarComponent>(audioEngine, document, bnkDocument);
        metadataSidebar->setWantsKeyboardFocus(true);
        metadataSidebar->setComponentID("workspace.inspector.metadata");
        metadataSidebar->setTitle("Metadata inspector");
        if (coreLifecycle.isCurrent(activeDocumentGeneration))
            metadataSidebar->beginTypedMetadataGeneration(activeDocumentGeneration.value);
        addChildComponent(*metadataSidebar);
    }
    if (!meterBridge)
    {
        meterBridge = std::make_unique<MeterBridgeComponent>(audioEngine);
        meterBridge->setMeterMode(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()));
        // Visibility and producer leases follow the existing presentation
        // and CoreReady paths, including a startup that restores Simple view.
        addChildComponent(*meterBridge);
    }
}

void MainComponent::ensureDeferredOutputMeter()
{
    if (deferChromeForStartup || !coreLifecycle.isCoreReady(activeDocumentGeneration))
        return;

    ensureDeferredWorkspaceControls();
    meterBridge->setCoreReady(true);

    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration,
                                                            OptionalModuleId::meter))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration,
                                                               OptionalModuleId::meter)))
    {
        return;
    }

    ensureWorkspaceModuleHost();
    if (workspaceModuleHost == nullptr)
        return;

    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    if (auto* panel = getOutputMeterPanel()) panel->setCoreReady(true);
    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::spatialEnergy, true);
    if (workspaceModuleHost->isModuleVisible(WorkspaceModuleId::outputMeter))
    {
        coreLifecycle.activate(activeDocumentGeneration, OptionalModuleId::meter);
        coreLifecycle.attachView(activeDocumentGeneration, OptionalModuleId::meter);
    }
    else if (workspaceModuleHost->isModuleActive(WorkspaceModuleId::outputMeter))
    {
        coreLifecycle.suspend(activeDocumentGeneration, OptionalModuleId::meter);
    }
    else
    {
        coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::meter);
    }
}

void MainComponent::ensureAmbisonicFieldPanelIfNeeded()
{
    const auto state = document.getState();
    const auto shouldExist = !deferChromeForStartup
                          && state != nullptr
                          && state->info.isValid()
                          && state->info.channelLayout.isAmbisonic;
    if (!shouldExist)
    {
        coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::ambisonics);
        if (workspaceModuleHost != nullptr && ambisonicWorkspaceModuleRegistered)
            workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::ambisonicEnergy, false);
        return;
    }

    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration,
                                                            OptionalModuleId::ambisonics))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration,
                                                               OptionalModuleId::ambisonics)))
    {
        return;
    }

    ensureWorkspaceModuleHost();
    if (workspaceModuleHost == nullptr)
        return;

    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::ambisonicEnergy, true);
    if (auto* panel = getAmbisonicFieldPanel())
        panel->setAmbisonicOrder(state->info.channelLayout.ambisonicOrder);
    if (workspaceModuleHost->isModuleVisible(WorkspaceModuleId::ambisonicEnergy))
    {
        coreLifecycle.activate(activeDocumentGeneration, OptionalModuleId::ambisonics);
        coreLifecycle.attachView(activeDocumentGeneration, OptionalModuleId::ambisonics);
    }
    else if (workspaceModuleHost->isModuleActive(WorkspaceModuleId::ambisonicEnergy))
    {
        coreLifecycle.suspend(activeDocumentGeneration, OptionalModuleId::ambisonics);
    }
    else
    {
        coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::ambisonics);
    }
}

void MainComponent::ensureWorkspaceModuleHost()
{
    if (deferChromeForStartup || !coreLifecycle.isCoreReady(activeDocumentGeneration))
        return;
    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration,
                                                            OptionalModuleId::workspace))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration,
                                                               OptionalModuleId::workspace)))
    {
        return;
    }

    if (workspaceModuleHost != nullptr)
    {
        registerWorkspaceCommandProvider();
        coreLifecycle.activate(activeDocumentGeneration, OptionalModuleId::workspace);
        coreLifecycle.attachView(activeDocumentGeneration, OptionalModuleId::workspace);
        return;
    }

    ensureDeferredWorkspaceControls();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto hostLifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::workspace, OptionalLifecycleResourceKind::workspaceHost);
    try
    {
#endif
    workspaceModuleHost = std::make_unique<WorkspaceModuleHostComponent>();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    }
    catch (...)
    {
        hostLifetime.failed();
        throw;
    }
    hostLifetime.created();
    workspaceModuleHost->observeLifetime(std::move(hostLifetime));
    workspaceModuleHost->setOptionalLifecycleContext(optionalLifecycleContext);
#endif
    workspaceModuleHost->setSimpleView(modernSimpleView);
    workspaceModuleHost->setPrivilegedCoreComponent(&waveformView);
    workspaceModuleHost->setPresentationReady(
        coreLifecycle.isCoreReady(activeDocumentGeneration));
    registerWorkspaceCommandProvider();
    if (parametricEqButton == nullptr)
    {
        parametricEqButton = std::make_unique<ModernIconButton>();
        configureActionButton(*parametricEqButton, "EQ");
        parametricEqButton->setComponentID(juce::String::fromUTF8(
            commandIds::toggleParametricEq.data(),
            static_cast<int>(commandIds::toggleParametricEq.size())));
        parametricEqButton->setTitle("Open Parametric EQ");
        parametricEqButton->setTooltip("Open or close the AudioSuite Parametric EQ Mag");
        parametricEqButton->onClick = [this]
        {
            dispatchGuiCommand(commandIds::toggleParametricEq,
                               GuiCommandSource::toolbar,
                               "workspace.modules");
        };
        addAndMakeVisible(*parametricEqButton);
    }
    if (truePeakLimiterButton == nullptr)
    {
        truePeakLimiterButton = std::make_unique<ModernIconButton>();
        configureActionButton(*truePeakLimiterButton, "LIM");
        truePeakLimiterButton->setComponentID(juce::String::fromUTF8(
            commandIds::toggleTruePeakLimiter.data(),
            static_cast<int>(commandIds::toggleTruePeakLimiter.size())));
        truePeakLimiterButton->setTitle("Open True-Peak Limiter");
        truePeakLimiterButton->setTooltip("Open or close the AudioSuite True-Peak Limiter Mag");
        truePeakLimiterButton->onClick = [this]
        {
            dispatchGuiCommand(commandIds::toggleTruePeakLimiter,
                               GuiCommandSource::toolbar,
                               "workspace.modules");
        };
        addAndMakeVisible(*truePeakLimiterButton);
    }
    if (workspacePresetCatalog == nullptr)
        workspacePresetCatalog = std::make_unique<WorkspacePresetCatalog>();

    workspaceModuleHost->registerBorrowedModule(
        { WorkspaceModuleId::playlist, 3, 1000, 120, { 1, 1 }, { 1, 1 }, { 4, 4 },
          -1, 0, "playlist", "playlist" },
        playlistSidebarWidth, *playlistSidebar, !playlistSidebarCollapsed);
    workspaceModuleHost->registerBorrowedModule(
        { WorkspaceModuleId::metadata, 4, 1000, 120, { 1, 1 }, { 1, 1 }, { 4, 4 },
          -1, 1, "metadata", "metadata" },
        metadataSidebarWidth, *metadataSidebar, !metadataSidebarCollapsed);
    workspaceModuleHost->registerModule({ WorkspaceModuleId::outputMeter,
                                          1,
                                          1000,
                                          160,
                                          { 1, 1 },
                                          { 1, 1 },
                                          { 2, 3 },
                                          -1,
                                          0,
                                          "output-meter",
                                          "output-meter",
                                          256 * 1024,
                                          true },
                                        154,
                                        [this]
                                        {
                                            auto panel = std::make_unique<OutputMeterPanelComponent>(audioEngine);
                                            panel->setCoreReady(!deferChromeForStartup && coreLifecycle.isCoreReady(activeDocumentGeneration));
                                            panel->setMeterMode(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()));
                                            return panel;
                                        });
    outputMeterWorkspaceModuleRegistered = true;
    workspaceModuleHost->registerModule(
        { WorkspaceModuleId::spatialEnergy, 6, 1000, 240, { 1, 1 }, { 2, 2 }, { 4, 4 },
          -1, 0, "spatial-energy", "spatial-energy", 1024 * 1024, false, false, false },
        400, [this]
        {
            auto panel = std::make_unique<SpatialEnergyPanelComponent>(audioEngine);
            panel->setCoreReady(!deferChromeForStartup && coreLifecycle.isCoreReady(activeDocumentGeneration));
            return panel;
        });
    if (!RendererVisualStyle::isClassic())
    {
        workspaceModuleHost->registerModule(
            { WorkspaceModuleId::markers, 5, 1000, 100, { 1, 1 }, { 1, 1 }, { 4, 4 },
              -1, 2, "markers", "markers", 64 * 1024, true, false, false },
            180, [this]
            {
                auto panel = std::make_unique<TimelineMarkersComponent>();
                panel->setDocument(!bnkDocument.hasBank() && !loadInProgress ? document.getState() : nullptr);
                panel->setTransport(audioEngine.getCurrentPositionSeconds(), currentSelection);
                panel->onSeek = [this](double seconds)
                {
                    if (!loadInProgress && coreLifecycle.isCoreReady(activeDocumentGeneration)
                        && hasPlayableAudioReady() && !bnkDocument.hasBank())
                    {
                        waveformView.seekToTime(seconds);
                        refreshTransportReadout();
                        refreshFooterReadout();
                    }
                };
                return panel;
            });
        workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::markers, true);
    }
    workspaceModuleHost->setModuleVisualSize(WorkspaceModuleId::outputMeter,
                                             ModernNorthStarMagSize::large);
    workspaceModuleHost->registerModule({ WorkspaceModuleId::ambisonicEnergy,
                                          0,
                                          780,
                                          180,
                                          { 1, 1 },
                                          { 1, 1 },
                                          { 2, 3 },
                                          -1,
                                          0,
                                          "ambisonic-energy",
                                          "ambisonic-energy",
                                          1024 * 1024,
                                          true },
                                        280,
                                        [this]
                                        {
                                            auto panel = std::make_unique<AmbisonicFieldPanelComponent>(audioEngine);
                                            if (const auto state = document.getState();
                                                state != nullptr
                                                    && state->info.channelLayout.isAmbisonic)
                                            {
                                                panel->setAmbisonicOrder(
                                                    state->info.channelLayout.ambisonicOrder);
                                            }
                                            return panel;
                                        });
    ambisonicWorkspaceModuleRegistered = true;
    workspaceModuleHost->setModuleVisualSize(WorkspaceModuleId::ambisonicEnergy,
                                             ModernNorthStarMagSize::medium);
    workspaceModuleHost->registerModule({ WorkspaceModuleId::parametricEq,
                                          2,
                                          920,
                                          180,
                                          { 1, 1 },
                                          { 2, 1 },
                                          { 4, 3 },
                                          2,
                                          2,
                                          "parametric-eq",
                                          "parametric-eq",
                                          768 * 1024,
                                          false },
                                        360,
                                        [this]
                                        {
                                            auto mag = std::make_unique<ParametricEqMagComponent>(
                                                parametricEqParameters);
                                            // Optional views may be reclaimed while General is active.
                                            // Restore the user's view without requesting another Mag resize.
                                            mag->setViewMode(parametricEqAdvancedView
                                                ? ParametricEqMagComponent::ViewMode::advanced
                                                : ParametricEqMagComponent::ViewMode::simple, false);
                                            mag->setSelectedAdvancedBand(parametricEqSelectedBand);
                                            mag->onViewModeChanged = [this] (ParametricEqMagComponent::ViewMode mode)
                                            {
                                                parametricEqAdvancedView = mode == ParametricEqMagComponent::ViewMode::advanced;
                                            };
                                            mag->onSelectedAdvancedBandChanged = [this] (std::size_t band)
                                            {
                                                parametricEqSelectedBand = band;
                                            };
                                            if (const auto state = document.getState(); state != nullptr)
                                                mag->setSourceSampleRate(state->info.sampleRate);
                                            mag->onParametersChanged = [this] (const ParametricEqParameters& parameters)
                                            {
                                                parametricEqParameters = parameters;
                                                if (parametricEqPreview.isValid())
                                                    updateParametricEqPreview();
                                            };
                                            mag->onPreviewRequested = [this] (bool enabled)
                                            {
                                                setParametricEqPreviewEnabled(enabled);
                                            };
                                            mag->onApplyRequested = [this] { applyParametricEq(); };
                                            mag->onMagSizeRequested = [this] (RendererMagSize size)
                                            {
                                                if (workspaceModuleHost == nullptr)
                                                    return;
                                                workspaceModuleHost->setModuleVisualSize(
                                                    WorkspaceModuleId::parametricEq,
                                                    size);
                                            };
                                            return mag;
                                        });
    parametricEqWorkspaceModuleRegistered = true;
    workspaceModuleHost->setModuleVisualSize(WorkspaceModuleId::parametricEq,
                                             ModernNorthStarMagSize::medium);
    workspaceModuleHost->setModuleEnabled(WorkspaceModuleId::parametricEq, false);
    const auto eqDocumentState = document.getState();
    workspaceModuleHost->setModuleAvailable(
        WorkspaceModuleId::parametricEq,
        eqDocumentState != nullptr && eqDocumentState->info.isValid() && !bnkDocument.hasBank());
    workspaceModuleHost->registerModule({ WorkspaceModuleId::truePeakLimiter,
                                          3,
                                          760,
                                          180,
                                          { 1, 1 },
                                          { 2, 1 },
                                          { 4, 3 },
                                          2,
                                          2,
                                          "true-peak-limiter",
                                          "true-peak-limiter",
                                          512 * 1024,
                                          false },
                                        340,
                                        [this]
                                        {
                                            auto mag = std::make_unique<TruePeakLimiterMagComponent>(
                                                truePeakLimiterParameters);
                                            mag->setRecallAvailable(lastAppliedTruePeakLimiterPlan != nullptr);
                                            mag->onParametersChanged = [this] (const TruePeakLimiterParameters& parameters)
                                            {
                                                truePeakLimiterParameters = parameters;
                                                if (truePeakLimiterPreview.isValid())
                                                    updateTruePeakLimiterPreview();
                                            };
                                            mag->onPreviewRequested = [this] (bool enabled)
                                            {
                                                setTruePeakLimiterPreviewEnabled(enabled);
                                            };
                                            mag->onApplyRequested = [this] { applyTruePeakLimiter(); };
                                            mag->onRecallLastAppliedRequested = [this]
                                            {
                                                recallLastAppliedTruePeakLimiter();
                                            };
                                            mag->onMagSizeRequested = [this] (RendererMagSize size)
                                            {
                                                if (workspaceModuleHost == nullptr)
                                                    return;
                                                workspaceModuleHost->setModuleVisualSize(
                                                    WorkspaceModuleId::truePeakLimiter,
                                                    size);
                                            };
                                            return mag;
                                        });
    truePeakLimiterWorkspaceModuleRegistered = true;
    workspaceModuleHost->setModuleVisualSize(WorkspaceModuleId::truePeakLimiter,
                                             ModernNorthStarMagSize::medium);
    workspaceModuleHost->setModuleEnabled(WorkspaceModuleId::truePeakLimiter, false);
    workspaceModuleHost->setModuleAvailable(
        WorkspaceModuleId::truePeakLimiter,
        eqDocumentState != nullptr && eqDocumentState->info.isValid() && !bnkDocument.hasBank());
    workspaceModuleHost->onBeforeModuleViewCreated = [this] (WorkspaceModuleId id)
    {
        if (id == WorkspaceModuleId::spatialEnergy)
            return !deferChromeForStartup && coreLifecycle.isCoreReady(activeDocumentGeneration);
        const auto module = workspaceViewRuntime(id);
        if (!module.has_value())
            return true;
        return lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration, *module))
            && lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration, *module));
    };
    workspaceModuleHost->onModuleVisibilityChanged = [this] (WorkspaceModuleId id, bool visible)
    {
        if (id == WorkspaceModuleId::parametricEq)
        {
            if (!visible && (!modernSimpleView || !workspaceModuleHost->isModuleActiveInUserLayout(id)))
                clearParametricEqPreview(true);
            return;
        }
        if (id == WorkspaceModuleId::truePeakLimiter)
        {
            if (!visible && (!modernSimpleView || !workspaceModuleHost->isModuleActiveInUserLayout(id)))
                clearTruePeakLimiterPreview(true);
            return;
        }
        const auto module = workspaceViewRuntime(id);
        if (!module.has_value())
            return;
        if (visible)
        {
            coreLifecycle.activate(activeDocumentGeneration, *module);
            coreLifecycle.attachView(activeDocumentGeneration, *module);
        }
        else
        {
            coreLifecycle.suspend(activeDocumentGeneration, *module);
        }
    };
    workspaceModuleHost->onFloatingWindowCommandRequested =
        [this] (WorkspaceModuleId id, WorkspaceFloatingWindowCommand command)
    {
        if (command == WorkspaceFloatingWindowCommand::togglePlayback)
        {
            dispatchGuiCommand(commandIds::togglePlayback,
                               GuiCommandSource::nativeWindow,
                               "floating-window");
            return;
        }

        auto commandId = command == WorkspaceFloatingWindowCommand::remove
            ? removeCommandForModule(id) : std::string_view {};
        if (command != WorkspaceFloatingWindowCommand::remove
            && id == WorkspaceModuleId::coreWaveform)
            commandId = commandIds::redockWaveform;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::outputMeter)
            commandId = commandIds::redockOutputMeter;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::ambisonicEnergy)
            commandId = commandIds::redockAmbisonicEnergy;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::spatialEnergy)
            commandId = commandIds::redockSpatialEnergy;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::parametricEq)
            commandId = commandIds::redockParametricEq;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::truePeakLimiter)
            commandId = commandIds::redockTruePeakLimiter;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::playlist)
            commandId = commandIds::redockPlaylistMag;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::metadata)
            commandId = commandIds::redockMetadataMag;
        else if (command != WorkspaceFloatingWindowCommand::remove
                 && id == WorkspaceModuleId::markers)
            commandId = commandIds::redockMarkers;
        if (!commandId.empty())
            dispatchGuiCommand(commandId, GuiCommandSource::nativeWindow, "floating-window");
    };
    workspaceModuleHost->onModuleCloseRequested = [this] (WorkspaceModuleId id)
    {
        const auto commandId = removeCommandForModule(id);
        if (!commandId.empty())
            dispatchGuiCommand(commandId, GuiCommandSource::mag, "workspace.modules");
    };

    // Borrowed file Mags are ready now. Restore the complete preset only after
    // their availability is known, so late gap-filling cannot swap its rows.
    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::playlist, true);
    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::metadata, true);
    workspaceModuleHost->setModuleAvailable(WorkspaceModuleId::outputMeter, true);
    const auto documentState = document.getState();
    const auto isAmbisonic = documentState != nullptr
                          && documentState->info.channelLayout.isAmbisonic;
    const auto channelCount = documentState != nullptr ? documentState->info.numChannels : 0;
    auto isVideoAudio = false;
    if (const auto* currentEntry = playlistModel.getCurrentEntry())
        isVideoAudio = currentEntry->kind() == PlaylistModel::EntryKind::video;
    activeWorkspaceCategory = WorkspacePresetCatalog::classify(bnkDocument.hasBank(),
                                                                isVideoAudio,
                                                                isAmbisonic,
                                                                channelCount);

    WorkspaceRestoreResult workspaceRestore;
    if (auto* settings = appProperties.getUserSettings())
    {
        if (!workspacePresetCatalogLoaded)
        {
            workspacePresetCatalogRestoreStatus = workspacePresetCatalog->restore(
                settings->getValue(workspacePresetCatalogKey).toStdString());
            workspacePresetCatalogLoaded = true;
            workspacePresetCatalogWritable =
                workspacePresetCatalogRestoreStatus != WorkspacePresetCatalogRestoreStatus::corrupt
                && workspacePresetCatalogRestoreStatus
                       != WorkspacePresetCatalogRestoreStatus::futureSchema;
        }

        auto state = juce::String();
        if (const auto selected = workspacePresetCatalog->resolve(activeWorkspaceCategory))
            state = juce::String::fromUTF8(selected->workspaceState.c_str());
        if (state.isEmpty() && activeWorkspaceCategory == WorkspaceContentCategory::general)
            state = settings->getValue(workspaceModuleLayoutV2Key);
        if (state.isEmpty() && activeWorkspaceCategory == WorkspaceContentCategory::general)
            state = settings->getValue(workspaceModuleLayoutKey);
        workspaceRestore = workspaceModuleHost->restoreUserState(state);
    }
    if (!RendererVisualStyle::isClassic() && activeWorkspaceCategory == WorkspaceContentCategory::general
        && (workspaceRestore.status == WorkspaceRestoreStatus::noState || workspaceRestore.status == WorkspaceRestoreStatus::corrupt))
    {
        // The existing corrupt payload stays read-only. Only the live safe
        // fallback changes to the approved Modern composition.
        const auto fallback = workspaceModuleHost->restoreUserState(modernWorkspaceDefaultLayout);
        jassert(fallback.applied() && fallback.quarantinedModuleCount == 0);
    }
    playlistSidebarCollapsed = !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::playlist);
    metadataSidebarCollapsed = !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::metadata);
    updateSidebarToggleButtons();
    workspacePresetWritable = workspaceRestore.status != WorkspaceRestoreStatus::corrupt
                           && workspaceRestore.status != WorkspaceRestoreStatus::futureSchema;
    if (workspaceRestore.status == WorkspaceRestoreStatus::corrupt)
    {
        engineStatusLabel.setText("Workspace preset is corrupt; using the safe default layout.",
                                  juce::dontSendNotification);
    }
    else if (workspaceRestore.status == WorkspaceRestoreStatus::futureSchema)
    {
        engineStatusLabel.setText("Workspace preset is from a newer version; preserved read-only.",
                                  juce::dontSendNotification);
    }
    else if (workspaceRestore.status == WorkspaceRestoreStatus::repairedLegacyOverlap)
    {
        engineStatusLabel.setText("Legacy workspace overlap repaired; the filled layout will be saved.",
                                  juce::dontSendNotification);
    }
    else if (workspacePresetCatalogRestoreStatus == WorkspacePresetCatalogRestoreStatus::corrupt)
    {
        engineStatusLabel.setText("Workspace preset catalog is corrupt; preserved read-only.",
                                  juce::dontSendNotification);
    }
    else if (workspacePresetCatalogRestoreStatus
             == WorkspacePresetCatalogRestoreStatus::futureSchema)
    {
        engineStatusLabel.setText("Workspace preset catalog is from a newer version; preserved read-only.",
                                  juce::dontSendNotification);
    }
    workspaceModuleHost->onLayoutChanged = [this]
    {
        if (workspaceModuleHost != nullptr)
        {
            workspaceModuleHost->setVisible(!deferChromeForStartup && workspaceModuleHost->hasActiveModules());
            playlistSidebarCollapsed = !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::playlist);
            metadataSidebarCollapsed = !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::metadata);
            updateSidebarToggleButtons();
        }
        resized();
        repaint();
    };
    workspaceModuleHost->onUserLayoutCommitted = [this]
    {
        ++workspaceFocusGeneration;
        if (workspacePresetWritable && workspacePresetCatalogWritable
            && workspaceModuleHost != nullptr)
        {
            const auto category = WorkspacePresetCatalog::categoryKey(activeWorkspaceCategory);
            const auto presetId = "last-" + category;
            if (workspacePresetCatalog->savePreset(presetId,
                                                   "Last " + category,
                                                   workspaceModuleHost->serialiseUserState().toStdString()))
            {
                workspacePresetCatalog->bindCategory(activeWorkspaceCategory, presetId);
            }
        }
        requestSessionSettingsPersist(120);
    };

    addAndMakeVisible(*workspaceModuleHost);
    workspaceModuleHost->setVisible(false);
    coreLifecycle.activate(activeDocumentGeneration, OptionalModuleId::workspace);
    coreLifecycle.attachView(activeDocumentGeneration, OptionalModuleId::workspace);
    if (workspaceRestore.status == WorkspaceRestoreStatus::migratedLegacyV1
        || workspaceRestore.status == WorkspaceRestoreStatus::repairedLegacyOverlap)
        requestSessionSettingsPersist(120);
}

OutputMeterPanelComponent* MainComponent::getOutputMeterPanel() const noexcept
{
    if (workspaceModuleHost == nullptr)
        return nullptr;
    return dynamic_cast<OutputMeterPanelComponent*>(
        workspaceModuleHost->getModuleComponent(WorkspaceModuleId::outputMeter));
}

AmbisonicFieldPanelComponent* MainComponent::getAmbisonicFieldPanel() const noexcept
{
    if (workspaceModuleHost == nullptr)
        return nullptr;
    return dynamic_cast<AmbisonicFieldPanelComponent*>(
        workspaceModuleHost->getModuleComponent(WorkspaceModuleId::ambisonicEnergy));
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return files.size() > 0 && isSupportedAudioPath(files[0]);
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    dragHighlight = false;
    repaint();

    if (files.isEmpty())
        return;

    std::vector<juce::File> fileList;
    fileList.reserve(static_cast<std::size_t>(files.size()));
    for (const auto& path : files)
    {
        auto file = juce::File(path);
        if (file.existsAsFile() && isSupportedAudioPath(path))
            fileList.push_back(file);
    }

    if (!fileList.empty())
        openFiles(fileList, 0);
}

void MainComponent::fileDragEnter(const juce::StringArray&, int, int)
{
    dragHighlight = true;
    repaint();
}

void MainComponent::fileDragExit(const juce::StringArray&)
{
    dragHighlight = false;
    repaint();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(ModernAppleLookAndFeel::windowBackground());

    if (dragHighlight)
    {
        auto bounds = getLocalBounds().toFloat().reduced(static_cast<float>(ModernAppleLookAndFeel::outerPadding() - 4));
        g.setColour(ModernAppleLookAndFeel::accentBlue().withAlpha(0.1f));
        g.fillRoundedRectangle(bounds, ModernAppleLookAndFeel::cardRadius() + 6.0f);
    }
}

void MainComponent::paintOverChildren(juce::Graphics& g)
{
    if (!startupTransitionSnapshot.isValid())
        return;

    g.drawImageWithin(startupTransitionSnapshot,
                      0,
                      0,
                      getWidth(),
                      getHeight(),
                      juce::RectanglePlacement::stretchToFit);

    if (!startupTransitionClearScheduled)
    {
        startupTransitionClearScheduled = true;
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (safeThis == nullptr)
                return;

            safeThis->startupTransitionSnapshot = {};
            safeThis->startupTransitionClearScheduled = false;
            safeThis->repaint();
        });
    }
}

void MainComponent::resized()
{
    auto bounds = layoutRecoveryStatus();
    if (modernSimpleView && !deferChromeForStartup && workspaceModuleHost != nullptr)
    {
        const auto content = bounds.reduced(ModernAppleLookAndFeel::outerPadding());
        workspaceModuleHost->setBounds(content);
        waveformView.setBounds(workspaceModuleHost->getCoreBounds().translated(content.getX(), content.getY()));
        updateCenterViewVisibility();
        updateSelectionPopup();
        if (commandPalette != nullptr)
        {
            const auto width = juce::jmin(560, juce::jmax(320, getWidth() - 80));
            const auto height = juce::jmin(390, juce::jmax(240, getHeight() - 100));
            commandPalette->setBounds(juce::Rectangle<int>(0, 0, width, height).withCentre(getLocalBounds().getCentre()));
            commandPalette->toFront(false);
        }
        return;
    }
    const auto showInClientTitleBar = !hostProvidesTitleBar && !deferChromeForStartup;
    if (showInClientTitleBar)
        titleBar.setBounds(bounds.removeFromTop(ModernAppleLookAndFeel::topBarHeight()));
    else
        titleBar.setBounds({});

    auto contentBounds = bounds.reduced(ModernAppleLookAndFeel::outerPadding());
    auto& content = contentBounds;

    if (deferChromeForStartup)
    {
        const auto startupHeaderHeight = 84;
        auto headerBounds = content.removeFromTop(startupHeaderHeight);
        actionBar.setBounds(headerBounds);

        auto headerContent = headerBounds.reduced(18, 12);
        auto topRow = headerContent.removeFromTop(30);
        headerContent.removeFromTop(6);
        auto bottomRow = headerContent.removeFromTop(20);

        constexpr auto iconButtonSize = 30;
        constexpr auto iconGap = 6;
        auto layoutCompactIcons = [iconButtonSize, iconGap](std::initializer_list<juce::Component*> components, juce::Rectangle<int>& row)
        {
            auto clusterWidth = static_cast<int>(components.size()) * iconButtonSize
                              + juce::jmax(0, static_cast<int>(components.size()) - 1) * iconGap;
            auto cluster = row.removeFromLeft(clusterWidth);
            for (auto* component : components)
            {
                component->setBounds(cluster.removeFromLeft(iconButtonSize));
                if (!cluster.isEmpty())
                    cluster.removeFromLeft(iconGap);
            }
        };

        auto leftCluster = topRow.removeFromLeft(108);
        layoutCompactIcons({ &openButton, &recentButton, &preferencesButton }, leftCluster);

        auto rightCluster = topRow.removeFromRight(108);
        layoutCompactIcons({ &playButton, &pauseButton, &stopButton }, rightCluster);

        filePillLabel.setBounds(topRow);
        auto engineBounds = bottomRow.removeFromLeft(juce::jmin(440, bottomRow.getWidth() / 2 + 80));
        engineStatusLabel.setBounds(engineBounds);
        bottomRow.removeFromLeft(12);
        transportStatusLabel.setBounds(bottomRow);

        footerBar.setBounds({});
        actionDragZone.setBounds({});
        if (playlistSidebar) playlistSidebar->setBounds({});
        if (metadataSidebar) metadataSidebar->setBounds({});
        if (workspaceModuleHost != nullptr)
            workspaceModuleHost->setBounds({});
        playlistResizeHandle.setBounds({});
        metadataResizeHandle.setBounds({});
        togglePlaylistSidebarButton.setBounds({});
        toggleMetadataSidebarButton.setBounds({});
        if (parametricEqButton != nullptr)
            parametricEqButton->setBounds({});
        if (truePeakLimiterButton != nullptr)
            truePeakLimiterButton->setBounds({});
        if (bnkControls) bnkControls->surface.setBounds({});
        if (bnkEventListView != nullptr)
            bnkEventListView->setBounds(content);
        if (bnkAudioListView != nullptr)
            bnkAudioListView->setBounds(content);

        content.removeFromTop(8);
        waveformView.setBounds(content);
        updateCenterViewVisibility();
        updateSelectionPopup();
        return;
    }

    if (!RendererVisualStyle::isClassic())
    {
        const auto layout = ModernTransportLayout::calculate(content.getWidth());
        const auto actionBounds = content.removeFromTop(layout.height);
        actionBar.setBounds(actionBounds);
        auto place = [&] (juce::Component& component, juce::Rectangle<int> local)
        {
            component.setBounds(local.translated(actionBounds.getX(), actionBounds.getY()));
        };
        for (auto* component : std::initializer_list<juce::Component*> {
                 &recentButton, &removeFileButton, &preferencesButton, &saveButton,
                 &saveAsButton, &fadeInButton, &fadeOutButton, &deleteSelectionButton,
                 &resetEnvelopeButton, &timeZoomOutButton, &timeZoomInButton,
                 &ampZoomOutButton, &ampZoomInButton, &resetViewButton, &autoPlayButton,
                 &filePillLabel, &actionDragZone })
        {
            component->setVisible(false);
            component->setBounds({});
        }
        if (parametricEqButton != nullptr)
        {
            parametricEqButton->setVisible(false);
            parametricEqButton->setBounds({});
        }
        if (truePeakLimiterButton != nullptr)
        {
            truePeakLimiterButton->setVisible(false);
            truePeakLimiterButton->setBounds({});
        }
        place(openButton, layout.open);
        place(toolbarMoreButton, layout.more);
        place(generalSceneButton, layout.generalScene);
        place(simpleViewButton, layout.simpleView);
        place(lightThemeButton, layout.lightTheme);
        place(nightThemeButton, layout.nightTheme);
        std::array<ModernIconButton*, 8> transport {
            &startButton, &previousButton, &nextButton, &playButton,
            &pauseButton, &stopButton, &returnToStartButton, &loopButton };
        for (size_t i = 0; i < transport.size(); ++i)
        {
            transport[i]->getProperties().set("glassControl", true);
            place(*transport[i], layout.transport[i]);
        }
        openButton.getProperties().set("glassControl", true);
        place(transportStatusLabel, layout.clock);
        place(undoButton, layout.undo);
        place(redoButton, layout.redo);
        place(meterModeBox, layout.meterMode);
        if (meterBridge) place(*meterBridge, layout.meter);
        place(peakLabel, layout.peak);
        place(monitorLabel, layout.monitorLabel);
        place(monitorSlider, layout.monitor);
        monitorLabel.setFont(ModernTypography::font(11.0f));
        monitorLabel.setBorderSize(juce::BorderSize<int>(0, 2, 0, 2));
        transportStatusLabel.setFont(juce::Font(juce::FontOptions(18.0f)));
        transportStatusLabel.setColour(juce::Label::textColourId, ModernAppleLookAndFeel::accentBlue());
    }
    else
    {
        auto actionBounds = content.removeFromTop(70);
        actionBar.setBounds(actionBounds);
        auto actionContent = actionBounds.reduced(0, 4);
        constexpr auto rowGap = 6;
        constexpr auto topRowHeight = 30;
        constexpr auto bottomRowHeight = 30;

        auto topActionRow = actionContent.removeFromTop(topRowHeight);
        actionContent.removeFromTop(rowGap);
        auto bottomActionRow = actionContent.removeFromTop(bottomRowHeight);

        constexpr auto iconButtonSize = 31;
        constexpr auto iconGap = 5;
        auto layoutIconCluster = [&](std::initializer_list<juce::Component*> components, juce::Rectangle<int>& row)
        {
            const auto count = static_cast<int>(components.size());
            if (count <= 0)
                return;

            auto clusterWidth = count * iconButtonSize + (count - 1) * iconGap;
            auto cluster = row.removeFromLeft(clusterWidth);

            for (auto* component : components)
            {
                auto cell = cluster.removeFromLeft(iconButtonSize);
                component->setBounds(cell);
                if (!cluster.isEmpty())
                    cluster.removeFromLeft(iconGap);
            }
        };

        const auto topLeftWidth = truePeakLimiterButton != nullptr
            ? 283
            : (parametricEqButton != nullptr ? 247 : 246);
        auto topLeft = topActionRow.removeFromLeft(topLeftWidth);
        auto topRight = topActionRow.removeFromRight(520);
        auto topCenter = topActionRow;

        if (truePeakLimiterButton != nullptr)
        {
            layoutIconCluster({ &openButton, &recentButton, &removeFileButton, &preferencesButton,
                                &saveButton, &saveAsButton, parametricEqButton.get(),
                                truePeakLimiterButton.get() },
                              topLeft);
        }
        else if (parametricEqButton != nullptr)
        {
            layoutIconCluster({ &openButton, &recentButton, &removeFileButton, &preferencesButton,
                                &saveButton, &saveAsButton, parametricEqButton.get() },
                              topLeft);
        }
        else
        {
            layoutIconCluster({ &openButton, &recentButton, &removeFileButton, &preferencesButton,
                                &saveButton, &saveAsButton },
                              topLeft);
        }

        constexpr auto transportButtonCount = 8;
        constexpr auto transportClusterWidth = transportButtonCount * iconButtonSize + (transportButtonCount - 1) * iconGap;
        juce::Rectangle<int> transportRowArea = topCenter.withSizeKeepingCentre(transportClusterWidth, topCenter.getHeight());
        layoutIconCluster({ &startButton, &previousButton, &nextButton, &playButton, &pauseButton, &stopButton, &returnToStartButton, &loopButton }, transportRowArea);

        auto topStatusLeft = topRight.removeFromLeft(292);
        meterModeBox.setBounds(topStatusLeft.removeFromLeft(82).reduced(0, 1));
        topStatusLeft.removeFromLeft(12);
        const auto meterBounds = topStatusLeft.removeFromLeft(136).reduced(0, 1);
        if (meterBridge) meterBridge->setBounds(meterBounds);
        topStatusLeft.removeFromLeft(14);
        peakLabel.setBounds(topStatusLeft.removeFromLeft(64));
        topRight.removeFromLeft(16);
        transportStatusLabel.setBounds(topRight.removeFromLeft(212).reduced(0, 1));
        actionDragZone.setBounds({});

        auto bottomLeft = bottomActionRow.removeFromLeft(410);
        auto bottomRight = bottomActionRow.removeFromRight(520);
        auto bottomCenter = bottomActionRow;

        layoutIconCluster({ &undoButton, &redoButton, &fadeInButton, &fadeOutButton, &deleteSelectionButton, &resetEnvelopeButton,
                            &timeZoomOutButton, &timeZoomInButton, &ampZoomOutButton, &ampZoomInButton, &resetViewButton }, bottomLeft);

        auto centerControls = bottomCenter.withSizeKeepingCentre(286, bottomCenter.getHeight());
        autoPlayButton.setBounds(centerControls.removeFromLeft(98));
        centerControls.removeFromLeft(14);
        monitorLabel.setBounds(centerControls.removeFromLeft(60));
        centerControls.removeFromLeft(8);
        monitorSlider.setBounds(centerControls.removeFromLeft(106));

        filePillLabel.setBounds(bottomRight.removeFromLeft(238).reduced(0, 1));
        bottomRight.removeFromLeft(16);
        engineStatusLabel.setBounds(bottomRight.removeFromLeft(266).reduced(0, 1));

    }

    content.removeFromTop(6);
    auto footerBounds = content.removeFromBottom(RendererVisualStyle::isClassic()
        ? ModernAppleLookAndFeel::bottomBarHeight() + 8 : ModernStatusLayout::height);
    footerBar.setBounds(footerBounds);

    auto footerContent = footerBounds.reduced(16, 4);
    if (!RendererVisualStyle::isClassic())
    {
        // Status may contain a failed save, read-only source or corrupt preset.
        // It must survive the compact toolbar, not disappear into overflow.
        const auto layout = ModernStatusLayout::calculate(footerBounds.getWidth());
        footerLeftLabel.setBounds(layout.file.translated(footerBounds.getX(), footerBounds.getY()));
        engineStatusLabel.setBounds(layout.status.translated(footerBounds.getX(), footerBounds.getY()));
        footerRightLabel.setBounds(layout.details.translated(footerBounds.getX(), footerBounds.getY()));
    }
    else
    {
        footerLeftLabel.setBounds(footerContent.removeFromLeft(820));
        footerRightLabel.setBounds(footerContent);
    }

    content.removeFromBottom(10);
    playlistResizeHandle.setVisible(false);
    metadataResizeHandle.setVisible(false);
    playlistResizeHandle.setBounds({});
    metadataResizeHandle.setBounds({});
    if (workspaceModuleHost == nullptr)
    {
        if (playlistSidebar) playlistSidebar->setVisible(false);
        if (metadataSidebar) metadataSidebar->setVisible(false);
        if (playlistSidebar) playlistSidebar->setBounds({});
        if (metadataSidebar) metadataSidebar->setBounds({});
    }

    const auto showWorkspaceModules = workspaceOwnsCanvas(
        { workspaceModuleHost != nullptr,
          workspaceModuleHost != nullptr && workspaceModuleHost->hasActiveModules(),
          workspaceModuleHost != nullptr && workspaceModuleHost->isVisible() });

    if (showWorkspaceModules)
    {
        workspaceModuleHost->setBounds(content);
        workspaceModuleHost->revalidateFloatingWindows();
        workspaceModuleHost->toFront(false);
    }
    else if (workspaceModuleHost != nullptr)
    {
        workspaceModuleHost->setBounds({});
    }

    juce::Rectangle<int> waveformBounds;
    if (showWorkspaceModules)
    {
        auto waveformGridBounds = workspaceModuleHost->getCoreBounds();
        waveformGridBounds.translate(workspaceModuleHost->getX(), workspaceModuleHost->getY());
        waveformBounds = waveformGridBounds;
        if (!workspaceModuleHost->isModuleFloating(WorkspaceModuleId::coreWaveform))
            waveformView.setBounds(waveformGridBounds);
    }
    else
    {
        waveformView.setBounds(content);
        waveformBounds = content;
    }
    if (RendererVisualStyle::isClassic())
    {
        const auto waveformHeaderY = waveformBounds.getY() + 28;
        togglePlaylistSidebarButton.setBounds(juce::Rectangle<int>(0, 0, 28, 28).withCentre({ waveformBounds.getX() + 24, waveformHeaderY }));
        toggleMetadataSidebarButton.setBounds(juce::Rectangle<int>(0, 0, 28, 28).withCentre({ waveformBounds.getRight() - 24, waveformHeaderY }));
    }
    else
    {
        togglePlaylistSidebarButton.setBounds({});
        toggleMetadataSidebarButton.setBounds({});
    }
    updateCenterViewVisibility();
    updateSelectionPopup();
    if (commandPalette != nullptr)
    {
        const auto width = juce::jmin(560, juce::jmax(320, getWidth() - 80));
        const auto height = juce::jmin(390, juce::jmax(240, getHeight() - 100));
        commandPalette->setBounds(juce::Rectangle<int>(0, 0, width, height)
                                      .withCentre(getLocalBounds().getCentre()));
        commandPalette->toFront(false);
    }
}

void MainComponent::registerCoreCommandProvider()
{
    GuiCommandProvider provider;
    provider.ownerId = "core-minimal";
    provider.commands = {
        { std::string(commandIds::togglePlayback), GuiCommandDomain::core, "Play or Pause", "Space", {} }
    };
    provider.resolveState = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return resolveCoreCommandState(id, context);
    };
    provider.dispatch = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return performCoreCommand(id, context);
    };
    const auto result = commandRegistry.registerProvider(std::move(provider));
    jassert(result.succeeded());
}

void MainComponent::registerPostCoreCommandProvider()
{
    if (postCoreCommandProviderRegistered)
        return;

    GuiCommandProvider provider;
    provider.ownerId = "core-document";
    provider.commands = {
        { std::string(commandIds::open), GuiCommandDomain::document, "Open Audio", "Ctrl+O", { "file" } },
        { std::string(commandIds::save), GuiCommandDomain::document, "Save", "Ctrl+S", { "file", "write" } },
        { std::string(commandIds::saveAs), GuiCommandDomain::document, "Save As", "Ctrl+Shift+S", { "file", "write" } },
        { std::string(commandIds::exportAudio), GuiCommandDomain::document, "Export Audio", {}, { "render", "file" } },
        { std::string(commandIds::undo), GuiCommandDomain::document, "Undo", "Ctrl+Z", { "edit" } },
        { std::string(commandIds::redo), GuiCommandDomain::document, "Redo", "Ctrl+Y", { "edit" } },
        { std::string(commandIds::deleteSelection), GuiCommandDomain::document, "Delete Selection", "Delete", { "edit" } },
        { std::string(commandIds::fadeIn), GuiCommandDomain::document, "Fade In", {}, { "selection", "edit" } },
        { std::string(commandIds::fadeOut), GuiCommandDomain::document, "Fade Out", {}, { "selection", "edit" } },
        { std::string(commandIds::resetEnvelope), GuiCommandDomain::document, "Reset Envelope", {}, { "edit" } },
        { std::string(commandIds::previousPlaylist), GuiCommandDomain::document, "Previous Playlist Item", "Page Up", { "file" } },
        { std::string(commandIds::nextPlaylist), GuiCommandDomain::document, "Next Playlist Item", "Page Down", { "file" } },
        { std::string(commandIds::removePlaylist), GuiCommandDomain::document, "Remove Current Playlist Item", {}, { "file" } },
        { std::string(commandIds::play), GuiCommandDomain::core, "Play", {}, { "transport" } },
        { std::string(commandIds::pause), GuiCommandDomain::core, "Pause", {}, { "transport" } },
        { std::string(commandIds::stop), GuiCommandDomain::core, "Stop", {}, { "transport" } },
        { std::string(commandIds::start), GuiCommandDomain::core, "Go to Start", "Home", { "transport", "seek" } },
        { std::string(commandIds::previousFrame), GuiCommandDomain::core, "Previous Frame", "Left", { "transport", "seek" } },
        { std::string(commandIds::nextFrame), GuiCommandDomain::core, "Next Frame", "Right", { "transport", "seek" } },
        { std::string(commandIds::returnToStart), GuiCommandDomain::core, "Return to Start on Stop", {}, { "transport" } },
        { std::string(commandIds::loop), GuiCommandDomain::core, "Loop Playback", {}, { "transport" } },
        { std::string(commandIds::autoPlay), GuiCommandDomain::core, "Auto Play", {}, { "transport" } },
        { std::string(commandIds::waveform), GuiCommandDomain::core, "Waveform View", {}, { "view" } },
        { std::string(commandIds::spectral), GuiCommandDomain::core, "Spectral View", {}, { "view" } },
        { std::string(commandIds::stacked), GuiCommandDomain::core, "Stacked Channels", {}, { "view" } },
        { std::string(commandIds::mixed), GuiCommandDomain::core, "Mixed Channels", {}, { "view" } },
        { std::string(commandIds::zoomTimeOut), GuiCommandDomain::core, "Zoom Time Out", "R", { "waveform" } },
        { std::string(commandIds::zoomTimeIn), GuiCommandDomain::core, "Zoom Time In", "T", { "waveform" } },
        { std::string(commandIds::zoomAmplitudeOut), GuiCommandDomain::core, "Zoom Amplitude Out", "Q", { "waveform" } },
        { std::string(commandIds::zoomAmplitudeIn), GuiCommandDomain::core, "Zoom Amplitude In", "W", { "waveform" } },
        { std::string(commandIds::resetView), GuiCommandDomain::core, "Reset Waveform View", "E", { "waveform" } },
        { std::string(commandIds::preferences), GuiCommandDomain::core, "Preferences", "Ctrl+,", { "settings" } },
        { std::string(commandIds::palette), GuiCommandDomain::core, "Command Palette", "Ctrl+Shift+P", { "search", "commands" }, false },
        { std::string(commandIds::minimize), GuiCommandDomain::core, "Minimize Window", {}, { "window" } },
        { std::string(commandIds::maximize), GuiCommandDomain::core, "Toggle Maximize", {}, { "window" } },
        { std::string(commandIds::close), GuiCommandDomain::core, "Close Window", "Ctrl+W", { "window", "exit" } },
        { std::string(commandIds::recoveryCenter), GuiCommandDomain::core, "Recovery Center", {}, { "recovery", "discard", "restore" } },
        { std::string(commandIds::diagnostics), GuiCommandDomain::core, "Local Diagnostics", {}, { "diagnostics", "support", "bundle" } },
        { std::string(commandIds::about), GuiCommandDomain::core, "About Single Editor", {}, { "help" } }
    };
    provider.resolveState = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return resolveCoreCommandState(id, context);
    };
    provider.dispatch = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return performCoreCommand(id, context);
    };
    const auto result = commandRegistry.registerProvider(std::move(provider));
    postCoreCommandProviderRegistered = result.succeeded();
    jassert(result.succeeded());
}

void MainComponent::setModernSimpleView(bool enabled)
{
    if (RendererVisualStyle::isClassic() || workspaceModuleHost == nullptr || modernSimpleView == enabled) return;
    if (enabled)
        focusBeforeSimpleView = juce::Component::getCurrentlyFocusedComponent();
    modernSimpleView = enabled;
    workspaceModuleHost->setSimpleView(enabled);
    waveformView.setSimplePresentation(enabled);
    if (onSimpleViewChanged) onSimpleViewChanged(enabled);
    applyChromePresentation();
    refreshUiState();
    // The Simple button belongs to the main window. Restoring its focus after
    // recreating a floating waveform would cover the primary editor with its placeholder.
    const auto focusedFloatingWaveform = !enabled
        && workspaceModuleHost->focusFloatingModule(WorkspaceModuleId::coreWaveform);
    if (!focusedFloatingWaveform)
    {
        if (!enabled && focusBeforeSimpleView != nullptr && focusBeforeSimpleView->isShowing()
            && focusBeforeSimpleView->getWantsKeyboardFocus())
            focusBeforeSimpleView->grabKeyboardFocus();
        else
            waveformView.grabKeyboardFocus();
    }
    if (!enabled) focusBeforeSimpleView = nullptr;
}

juce::Component::WindowControlKind MainComponent::findControlAtPoint(juce::Point<float> point) const
{
    return modernSimpleView ? waveformView.findControlAtPoint(waveformView.getLocalPoint(this, point))
                            : WindowControlKind::client;
}

void MainComponent::registerWorkspaceCommandProvider()
{
    if (workspaceCommandProviderRegistered)
        return;

    GuiCommandProvider provider;
    provider.ownerId = "workspace";
    provider.commands = {
        { std::string(commandIds::generalScene), GuiCommandDomain::workspace, "General Workspace Scene", {}, { "view", "layout" } },
        { std::string(commandIds::simpleView), GuiCommandDomain::workspace, "Simple Waveform View", {}, { "view", "focus" } },
        { std::string(commandIds::togglePlaylistSidebar), GuiCommandDomain::workspace, "Toggle Playlist Sidebar", {}, { "view" } },
        { std::string(commandIds::toggleMetadataSidebar), GuiCommandDomain::workspace, "Toggle Metadata Sidebar", {}, { "view" } },
        { std::string(commandIds::toggleOutputMeter), GuiCommandDomain::workspace, "Toggle Output Meter", {}, { "module", "levels" } },
        { std::string(commandIds::toggleAmbisonicEnergy), GuiCommandDomain::workspace, "Toggle Ambisonic Energy", {}, { "module", "spatial" } },
        { std::string(commandIds::toggleSpatialEnergy), GuiCommandDomain::workspace, "Toggle Rendered Output Spatial Energy", {}, { "module", "spatial", "energy", "meter" } },
        { std::string(commandIds::floatSpatialEnergy), GuiCommandDomain::workspace, "Float Spatial Energy", {}, { "module", "window" } },
        { std::string(commandIds::redockSpatialEnergy), GuiCommandDomain::workspace, "Redock Spatial Energy", {}, { "module", "window" } },
        { std::string(commandIds::removeSpatialEnergy), GuiCommandDomain::module, "Remove Spatial Energy", {}, { "module", "close" }, false },
        { std::string(commandIds::toggleParametricEq), GuiCommandDomain::workspace, "Toggle Parametric EQ", {}, { "module", "dsp", "equalizer" } },
        { std::string(commandIds::toggleTruePeakLimiter), GuiCommandDomain::workspace, "Toggle True-Peak Limiter", {}, { "module", "dsp", "limiter", "mastering" } },
        { std::string(commandIds::floatWaveform), GuiCommandDomain::workspace, "Float Audio Waveform", {}, { "module", "window" } },
        { std::string(commandIds::redockWaveform), GuiCommandDomain::workspace, "Redock Audio Waveform", {}, { "module", "window" } },
        { std::string(commandIds::floatOutputMeter), GuiCommandDomain::workspace, "Float Output Meter", {}, { "module", "window" } },
        { std::string(commandIds::redockOutputMeter), GuiCommandDomain::workspace, "Redock Output Meter", {}, { "module", "window" } },
        { std::string(commandIds::removeOutputMeter), GuiCommandDomain::module, "Remove Output Meter", {}, { "module", "close" }, false },
        { std::string(commandIds::floatAmbisonicEnergy), GuiCommandDomain::workspace, "Float Ambisonic Energy", {}, { "module", "window" } },
        { std::string(commandIds::redockAmbisonicEnergy), GuiCommandDomain::workspace, "Redock Ambisonic Energy", {}, { "module", "window" } },
        { std::string(commandIds::removeAmbisonicEnergy), GuiCommandDomain::module, "Remove Ambisonic Energy", {}, { "module", "close" }, false },
        { std::string(commandIds::floatParametricEq), GuiCommandDomain::workspace, "Float Parametric EQ", {}, { "module", "dsp", "window" } },
        { std::string(commandIds::redockParametricEq), GuiCommandDomain::workspace, "Redock Parametric EQ", {}, { "module", "dsp", "window" } },
        { std::string(commandIds::removeParametricEq), GuiCommandDomain::module, "Remove Parametric EQ", {}, { "module", "dsp", "close" }, false },
        { std::string(commandIds::floatTruePeakLimiter), GuiCommandDomain::workspace, "Float True-Peak Limiter", {}, { "module", "dsp", "window" } },
        { std::string(commandIds::redockTruePeakLimiter), GuiCommandDomain::workspace, "Redock True-Peak Limiter", {}, { "module", "dsp", "window" } },
        { std::string(commandIds::removeTruePeakLimiter), GuiCommandDomain::module, "Remove True-Peak Limiter", {}, { "module", "dsp", "close" }, false },
        { std::string(commandIds::floatPlaylistMag), GuiCommandDomain::workspace, "Float Playlist", {}, { "module", "window" } },
        { std::string(commandIds::redockPlaylistMag), GuiCommandDomain::workspace, "Redock Playlist", {}, { "module", "window" } },
        { std::string(commandIds::removePlaylistMag), GuiCommandDomain::module, "Remove Playlist Mag", {}, { "module", "close" }, false },
        { std::string(commandIds::floatMetadataMag), GuiCommandDomain::workspace, "Float Metadata", {}, { "module", "window" } },
        { std::string(commandIds::redockMetadataMag), GuiCommandDomain::workspace, "Redock Metadata", {}, { "module", "window" } },
        { std::string(commandIds::removeMetadataMag), GuiCommandDomain::module, "Remove Metadata Mag", {}, { "module", "close" }, false },
        { std::string(commandIds::toggleMarkers), GuiCommandDomain::workspace, "Toggle Markers", {}, { "cue", "timeline", "selection" } },
        { std::string(commandIds::floatMarkers), GuiCommandDomain::workspace, "Float Markers", {}, { "module", "window" } },
        { std::string(commandIds::redockMarkers), GuiCommandDomain::workspace, "Redock Markers", {}, { "module", "window" } },
        { std::string(commandIds::removeMarkers), GuiCommandDomain::module, "Remove Markers", {}, { "module", "close" }, false },
        { std::string(commandIds::resetModuleLayout), GuiCommandDomain::workspace, "Reset Module Layout", {}, { "module", "workspace" } }
    };
    provider.resolveState = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return resolveWorkspaceCommandState(id, context);
    };
    provider.dispatch = [this] (std::string_view id, const GuiCommandDispatchContext& context)
    {
        return performWorkspaceCommand(id, context);
    };
    const auto result = commandRegistry.registerProvider(std::move(provider));
    workspaceCommandProviderRegistered = result.succeeded();
    jassert(workspaceCommandProviderRegistered);
}

GuiCommandState MainComponent::resolveCoreCommandState(
    std::string_view commandId,
    const GuiCommandDispatchContext&) const
{
    const auto audioReady = hasPlayableAudioReady();
    const auto playbackAvailable = audioReady || canQueuePlaybackDuringLoad() || bnkDocument.hasBank();
    const auto hasAudio = document.hasAudio();
    const auto canEditWaveform = hasAudio && audioEngine.hasLoadedFile()
                               && !loadInProgress && !bnkDocument.hasBank();
    const auto canMutateWaveform = canEditWaveform
                                && !referenceAudioSuiteApplyInFlight
                                && !parametricEqApplyInFlight
                                && !truePeakLimiterApplyInFlight;
    const auto hasSelection = currentSelection.has_value() && currentSelection->getLength() > 0.0;
    const auto playbackState = audioEngine.getPlaybackState();
    GuiCommandState state;

    if (commandId == commandIds::save || commandId == commandIds::saveAs
        || commandId == commandIds::exportAudio)
    {
        state.enabled = canEditWaveform;
        state.disabledReason = hasAudio ? "Wait for an editable audio document." : "No audio document is open.";
    }
    else if (commandId == commandIds::undo)
    {
        state.enabled = document.canUndo() && canMutateWaveform;
        state.disabledReason = "There is no edit to undo.";
    }
    else if (commandId == commandIds::redo)
    {
        state.enabled = document.canRedo() && canMutateWaveform;
        state.disabledReason = "There is no edit to redo.";
    }
    else if (commandId == commandIds::deleteSelection || commandId == commandIds::fadeIn
             || commandId == commandIds::fadeOut)
    {
        state.enabled = canMutateWaveform && hasSelection;
        state.disabledReason = "Create an audio selection first.";
    }
    else if (commandId == commandIds::resetEnvelope)
    {
        state.enabled = canMutateWaveform;
        state.disabledReason = "No audio document is open.";
    }
    else if (commandId == commandIds::togglePlayback || commandId == commandIds::play)
    {
        state.enabled = playbackAvailable;
        state.disabledReason = "No playable audio is ready.";
        state.checked = playbackState == AudioEngine::PlaybackState::playing;
    }
    else if (commandId == commandIds::pause)
    {
        state.enabled = playbackState == AudioEngine::PlaybackState::playing;
        state.disabledReason = "Playback is not running.";
    }
    else if (commandId == commandIds::stop || commandId == commandIds::start
             || commandId == commandIds::previousFrame || commandId == commandIds::nextFrame)
    {
        state.enabled = audioEngine.hasLoadedFile();
        state.disabledReason = "No playable audio is loaded.";
    }
    else if (commandId == commandIds::returnToStart)
    {
        state.checked = audioEngine.shouldReturnToPlayStartOnStop();
    }
    else if (commandId == commandIds::loop)
    {
        state.enabled = audioEngine.hasLoadedFile();
        state.checked = audioEngine.isLoopEnabled();
        state.disabledReason = "No playable audio is loaded.";
    }
    else if (commandId == commandIds::autoPlay)
    {
        state.checked = autoPlayButton.getToggleState();
    }
    else if (commandId == commandIds::previousPlaylist)
    {
        state.enabled = playlistModel.getCurrentIndex() > 0;
        state.disabledReason = "The first playlist item is already active.";
    }
    else if (commandId == commandIds::nextPlaylist)
    {
        state.enabled = playlistModel.getCurrentIndex() >= 0
                     && playlistModel.getCurrentIndex() + 1 < playlistModel.getNumEntries();
        state.disabledReason = "The last playlist item is already active.";
    }
    else if (commandId == commandIds::removePlaylist)
    {
        state.enabled = playlistModel.getCurrentIndex() >= 0 && !loadInProgress;
        state.disabledReason = "There is no playlist item to remove.";
    }
    else if (commandId == commandIds::waveform || commandId == commandIds::spectral
             || commandId == commandIds::stacked || commandId == commandIds::mixed)
    {
        state.enabled = hasAudio || loadInProgress;
        state.disabledReason = "No audio waveform is open.";
        state.checked = (commandId == commandIds::waveform
                         && waveformView.getViewMode() == WaveformViewComponent::ViewMode::waveform)
                     || (commandId == commandIds::spectral
                         && waveformView.getViewMode() == WaveformViewComponent::ViewMode::spectral)
                     || (commandId == commandIds::stacked
                         && waveformView.getDisplayMode() == WaveformViewComponent::DisplayMode::stacked)
                     || (commandId == commandIds::mixed
                         && waveformView.getDisplayMode() == WaveformViewComponent::DisplayMode::mixed);
    }
    else if (commandId == commandIds::zoomTimeOut || commandId == commandIds::zoomTimeIn
             || commandId == commandIds::zoomAmplitudeOut || commandId == commandIds::zoomAmplitudeIn
             || commandId == commandIds::resetView)
    {
        state.enabled = canEditWaveform;
        state.disabledReason = "No editable audio waveform is open.";
    }
    else if (commandId == commandIds::palette)
    {
        state.enabled = !deferChromeForStartup;
        state.disabledReason = "Command Palette becomes available after CoreReady.";
        state.checked = commandPalette != nullptr;
    }
    else if (commandId == commandIds::maximize)
    {
        state.checked = hostWindow.isFullScreen();
        state.label = hostWindow.isFullScreen() ? "Restore Window" : "Maximize Window";
    }

    return state;
}

GuiCommandState MainComponent::resolveWorkspaceCommandState(
    std::string_view commandId,
    const GuiCommandDispatchContext&) const
{
    GuiCommandState state;
    if (commandId == commandIds::toggleSpatialEnergy || commandId == commandIds::floatSpatialEnergy
        || commandId == commandIds::redockSpatialEnergy || commandId == commandIds::removeSpatialEnergy)
    {
        const auto ready = !deferChromeForStartup && coreLifecycle.isCoreReady(activeDocumentGeneration);
        const auto active = workspaceModuleHost && workspaceModuleHost->isModuleActiveInUserLayout(WorkspaceModuleId::spatialEnergy);
        const auto floating = workspaceModuleHost && workspaceModuleHost->isModuleFloating(WorkspaceModuleId::spatialEnergy);
        state.enabled = ready && workspaceModuleHost && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::spatialEnergy)
            && (commandId == commandIds::toggleSpatialEnergy || (commandId == commandIds::floatSpatialEnergy ? active && !floating
                : commandId == commandIds::redockSpatialEnergy ? floating : active));
        state.checked = commandId == commandIds::toggleSpatialEnergy ? active : floating;
        state.disabledReason = "Spatial Energy requires CoreReady and an applicable workspace view.";
        return state;
    }
    if (commandId == commandIds::generalScene || commandId == commandIds::simpleView)
    {
        state.enabled = !RendererVisualStyle::isClassic() && workspaceModuleHost != nullptr;
        state.checked = commandId == commandIds::simpleView ? modernSimpleView : !modernSimpleView;
        state.disabledReason = "Workspace scenes become available after CoreReady in Modern.";
        return state;
    }
    state.enabled = workspaceModuleHost != nullptr;
    state.disabledReason = "Workspace is not available before CoreReady.";
    // Workspace commands operate after leaving Simple, so their checkmarks and
    // availability must describe the retained layout, not its hidden projection.
    const auto activeInUserLayout = [this] (WorkspaceModuleId id)
    {
        return workspaceModuleHost != nullptr && workspaceModuleHost->isModuleActiveInUserLayout(id);
    };
    if (commandId == commandIds::togglePlaylistSidebar)
        state.checked = !playlistSidebarCollapsed;
    else if (commandId == commandIds::toggleMetadataSidebar)
        state.checked = !metadataSidebarCollapsed;
    else if (commandId == commandIds::toggleOutputMeter)
    {
        state.enabled = workspaceModuleHost != nullptr
                     && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::outputMeter);
        state.checked = activeInUserLayout(WorkspaceModuleId::outputMeter);
        state.disabledReason = "Output Meter is not available in this workspace.";
    }
    else if (commandId == commandIds::toggleAmbisonicEnergy)
    {
        state.enabled = workspaceModuleHost != nullptr
                     && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::ambisonicEnergy);
        state.checked = activeInUserLayout(WorkspaceModuleId::ambisonicEnergy);
        state.disabledReason = "Ambisonic Energy is only available for Ambisonics content.";
    }
    else if (commandId == commandIds::toggleParametricEq)
    {
        state.enabled = workspaceModuleHost != nullptr
                     && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::parametricEq)
                     && document.hasAudio() && !bnkDocument.hasBank();
        state.checked = activeInUserLayout(WorkspaceModuleId::parametricEq);
        state.disabledReason = "Parametric EQ requires an editable ordinary-audio document.";
    }
    else if (commandId == commandIds::toggleTruePeakLimiter)
    {
        state.enabled = workspaceModuleHost != nullptr
                     && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::truePeakLimiter)
                     && document.hasAudio() && !bnkDocument.hasBank();
        state.checked = activeInUserLayout(WorkspaceModuleId::truePeakLimiter);
        state.disabledReason = "True-Peak Limiter requires an editable ordinary-audio document.";
    }
    else if (commandId == commandIds::toggleMarkers)
    {
        state.enabled = workspaceModuleHost != nullptr
                     && workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::markers);
        state.checked = activeInUserLayout(WorkspaceModuleId::markers);
        state.disabledReason = "Markers are available in the Modern workspace after CoreReady.";
    }
    else if (commandId == commandIds::floatWaveform || commandId == commandIds::redockWaveform)
    {
        const auto floating = workspaceModuleHost != nullptr
                           && workspaceModuleHost->isModuleFloating(WorkspaceModuleId::coreWaveform);
        state.enabled = workspaceModuleHost != nullptr
                     && (commandId == commandIds::floatWaveform ? !floating : floating);
        state.checked = floating;
        state.disabledReason = commandId == commandIds::floatWaveform
            ? "The waveform is already floating."
            : "The waveform is already docked.";
    }
    else if (commandId == commandIds::floatOutputMeter
             || commandId == commandIds::redockOutputMeter
             || commandId == commandIds::removeOutputMeter
             || commandId == commandIds::floatAmbisonicEnergy
             || commandId == commandIds::redockAmbisonicEnergy
             || commandId == commandIds::removeAmbisonicEnergy
             || commandId == commandIds::floatParametricEq
             || commandId == commandIds::redockParametricEq
             || commandId == commandIds::removeParametricEq
             || commandId == commandIds::floatTruePeakLimiter
             || commandId == commandIds::redockTruePeakLimiter
             || commandId == commandIds::removeTruePeakLimiter
             || sidebarModuleForWindowCommand(commandId).has_value())
    {
        const auto meter = commandId == commandIds::floatOutputMeter
                        || commandId == commandIds::redockOutputMeter
                        || commandId == commandIds::removeOutputMeter;
        const auto eq = commandId == commandIds::floatParametricEq
                     || commandId == commandIds::redockParametricEq
                     || commandId == commandIds::removeParametricEq;
        const auto limiter = commandId == commandIds::floatTruePeakLimiter
                          || commandId == commandIds::redockTruePeakLimiter
                          || commandId == commandIds::removeTruePeakLimiter;
        const auto id = sidebarModuleForWindowCommand(commandId).value_or(
                       meter ? WorkspaceModuleId::outputMeter
                              : (eq ? WorkspaceModuleId::parametricEq
                                    : (limiter ? WorkspaceModuleId::truePeakLimiter
                                               : WorkspaceModuleId::ambisonicEnergy)));
        const auto floating = workspaceModuleHost != nullptr
                           && workspaceModuleHost->isModuleFloating(id);
        const auto isFloat = commandId == commandIds::floatOutputMeter
                           || commandId == commandIds::floatAmbisonicEnergy
                           || commandId == commandIds::floatParametricEq
                           || commandId == commandIds::floatTruePeakLimiter
                           || commandId == commandIds::floatPlaylistMag
                           || commandId == commandIds::floatMetadataMag
                           || commandId == commandIds::floatMarkers;
        const auto isRemove = commandId == commandIds::removeOutputMeter
                           || commandId == commandIds::removeAmbisonicEnergy
                           || commandId == commandIds::removeParametricEq
                           || commandId == commandIds::removeTruePeakLimiter
                           || commandId == commandIds::removePlaylistMag
                           || commandId == commandIds::removeMetadataMag
                           || commandId == commandIds::removeMarkers;
        state.enabled = workspaceModuleHost != nullptr
                     && (isFloat ? activeInUserLayout(id) && !floating
                                 : (isRemove ? activeInUserLayout(id)
                                             : floating));
        state.checked = floating;
        state.disabledReason = isFloat
            ? "This module must be enabled and docked before it can float."
            : (isRemove ? "This module is already closed."
                        : "This module is already docked.");
    }
    return state;
}

GuiCommandResult MainComponent::performCoreCommand(
    std::string_view commandId,
    const GuiCommandDispatchContext&)
{
    if (commandId == commandIds::open)
        chooseAndOpenFile();
    else if (commandId == commandIds::save)
        saveDocument(false);
    else if (commandId == commandIds::saveAs)
        saveDocument(true);
    else if (commandId == commandIds::exportAudio)
        exportDocument();
    else if (commandId == commandIds::undo)
        undoEdit();
    else if (commandId == commandIds::redo)
        redoEdit();
    else if (commandId == commandIds::deleteSelection)
        deleteSelection();
    else if (commandId == commandIds::fadeIn)
        applyFadeIn();
    else if (commandId == commandIds::fadeOut)
        applyFadeOut();
    else if (commandId == commandIds::resetEnvelope)
    {
        document.resetEnvelope();
        waveformView.refreshNow();
        refreshFooterReadout();
        updateTransportAvailability();
    }
    else if (commandId == commandIds::previousPlaylist)
        requestPlaylistActivation(juce::jmax(0, playlistModel.getCurrentIndex() - 1));
    else if (commandId == commandIds::nextPlaylist)
        requestPlaylistActivation(juce::jmin(playlistModel.getNumEntries() - 1,
                                             playlistModel.getCurrentIndex() + 1));
    else if (commandId == commandIds::removePlaylist)
        removeCurrentPlaylistEntry();
    else if (commandId == commandIds::togglePlayback)
        toggleAudioPlaybackOrQueue();
    else if (commandId == commandIds::play)
    {
        if (bnkDocument.hasBank())
        {
            if (bnkCenterMode == BnkCenterMode::audio)
            {
                if (const auto media = bnkDocument.getSelectedMediaId(); media.has_value())
                    prepareSelectedBnkMedia(*media, true);
                else
                    prepareSelectedBnkEvent(true);
            }
            else
            {
                prepareSelectedBnkEvent(true);
            }
            refreshTransportReadout();
        }
        else
        {
            requestAudioPlaybackOrQueue();
        }
    }
    else if (commandId == commandIds::pause)
    {
        audioEngine.pause();
        refreshTransportReadout();
    }
    else if (commandId == commandIds::stop)
    {
        audioEngine.stop();
        waveformView.refreshNow();
        refreshTransportReadout();
    }
    else if (commandId == commandIds::start)
    {
        audioEngine.seekToSeconds(0.0);
        waveformView.refreshNow();
        refreshTransportReadout();
    }
    else if (commandId == commandIds::previousFrame || commandId == commandIds::nextFrame)
    {
        audioEngine.stepByFrames(commandId == commandIds::previousFrame ? -1 : 1);
        waveformView.refreshNow();
        refreshTransportReadout();
        refreshFooterReadout();
    }
    else if (commandId == commandIds::returnToStart)
    {
        const auto enabled = !audioEngine.shouldReturnToPlayStartOnStop();
        audioEngine.setReturnToPlayStartOnStop(enabled);
        returnToStartButton.setToggleState(enabled, juce::dontSendNotification);
        refreshFooterReadout();
    }
    else if (commandId == commandIds::loop)
    {
        const auto enabled = !audioEngine.isLoopEnabled();
        audioEngine.setLoopEnabled(enabled);
        loopButton.setToggleState(enabled, juce::dontSendNotification);
        refreshFooterReadout();
    }
    else if (commandId == commandIds::autoPlay)
    {
        autoPlayButton.setToggleState(!autoPlayButton.getToggleState(), juce::dontSendNotification);
        persistSessionSettings();
        refreshFooterReadout();
    }
    else if (commandId == commandIds::waveform || commandId == commandIds::spectral)
    {
        const auto requested = commandId == commandIds::waveform
            ? WaveformViewComponent::ViewMode::waveform
            : WaveformViewComponent::ViewMode::spectral;
        if (waveformView.getViewMode() == requested
            && !(requested == WaveformViewComponent::ViewMode::spectral
                 && waveformModel.getSpectrogramStatus() == WaveformModel::SpectrogramStatus::failed))
            return GuiCommandResult::noChange();
        waveformView.setViewMode(requested);
        persistSessionSettings();
        refreshUiState();
        resized();
    }
    else if (commandId == commandIds::stacked || commandId == commandIds::mixed)
    {
        const auto requested = commandId == commandIds::stacked
            ? WaveformViewComponent::DisplayMode::stacked
            : WaveformViewComponent::DisplayMode::mixed;
        if (waveformView.getDisplayMode() == requested)
            return GuiCommandResult::noChange();
        waveformView.setDisplayMode(requested);
        persistSessionSettings();
        refreshUiState();
    }
    else if (commandId == commandIds::zoomTimeOut)
        zoomTimeOut();
    else if (commandId == commandIds::zoomTimeIn)
        zoomTimeIn();
    else if (commandId == commandIds::zoomAmplitudeOut)
        zoomAmplitudeOut();
    else if (commandId == commandIds::zoomAmplitudeIn)
        zoomAmplitudeIn();
    else if (commandId == commandIds::resetView)
        resetWaveformView();
    else if (commandId == commandIds::preferences)
        showPreferences();
    else if (commandId == commandIds::palette)
        showCommandPalette();
    else if (commandId == commandIds::minimize)
        hostWindow.setMinimised(true);
    else if (commandId == commandIds::maximize)
        hostWindow.setFullScreen(!hostWindow.isFullScreen());
    else if (commandId == commandIds::close)
    {
        if (auto* documentWindow = dynamic_cast<juce::DocumentWindow*>(&hostWindow))
            documentWindow->closeButtonPressed();
        else
            requestCloseApplication();
    }
    else if (commandId == commandIds::recoveryCenter)
        showRecoveryCenter();
    else if (commandId == commandIds::diagnostics)
        showLocalDiagnostics();
    else if (commandId == commandIds::about)
    {
        const auto focusToken = captureFocusReturnToken("dialog", "application.about");
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "About Single Editor",
                                               "Single Editor\nModern waveform and BNK audition workspace.",
                                               {},
                                               nullptr,
                                               juce::ModalCallbackFunction::create(
                                               [safeThis = juce::Component::SafePointer<MainComponent>(this), focusToken] (int)
                                               {
                                                   if (safeThis != nullptr)
                                                       safeThis->restoreFocusReturnToken(focusToken);
                                               }));
    }
    else
        return { GuiCommandStatus::notFound, "Unknown Core command." };

    return GuiCommandResult::committed();
}

GuiCommandResult MainComponent::performWorkspaceCommand(
    std::string_view commandId,
    const GuiCommandDispatchContext& context,
    bool waveformFloatConfirmed)
{
    if (commandId == commandIds::generalScene || commandId == commandIds::simpleView)
    {
        setModernSimpleView(commandId == commandIds::simpleView);
        return GuiCommandResult::committed();
    }
    // An explicit Mag/layout command must reveal the workspace it operates on.
    if (modernSimpleView) setModernSimpleView(false);
    if (commandId == commandIds::toggleSpatialEnergy || commandId == commandIds::floatSpatialEnergy
        || commandId == commandIds::redockSpatialEnergy || commandId == commandIds::removeSpatialEnergy)
    {
        if (!workspaceModuleHost || deferChromeForStartup || !coreLifecycle.isCoreReady(activeDocumentGeneration))
            return GuiCommandResult::failed("Spatial Energy waits for CoreReady.");
        constexpr auto id = WorkspaceModuleId::spatialEnergy;
        auto result = WorkspaceGridMutationResult::noChange;
        if (commandId == commandIds::toggleSpatialEnergy)
            workspaceModuleHost->setModuleEnabled(id, !workspaceModuleHost->isModuleActive(id));
        else if (commandId == commandIds::floatSpatialEnergy) result = workspaceModuleHost->floatModule(id);
        else if (commandId == commandIds::redockSpatialEnergy) result = workspaceModuleHost->redockModule(id);
        else result = workspaceModuleHost->closeModule(id);
        if (result != WorkspaceGridMutationResult::committed && result != WorkspaceGridMutationResult::noChange)
            return GuiCommandResult::failed("Spatial Energy layout change could not be applied.");
        requestSessionSettingsPersist(120); applyChromePresentation();
        if (commandId == commandIds::floatSpatialEnergy) workspaceModuleHost->focusFloatingModule(id);
        return GuiCommandResult::committed();
    }
    if (commandId == commandIds::togglePlaylistSidebar)
        togglePlaylistSidebar();
    else if (commandId == commandIds::toggleMetadataSidebar)
        toggleMetadataSidebar();
    else if (commandId == commandIds::toggleMarkers)
    {
        if (workspaceModuleHost == nullptr)
            return GuiCommandResult::failed("Workspace is unavailable.");
        workspaceModuleHost->setModuleEnabled(WorkspaceModuleId::markers,
            !workspaceModuleHost->isModuleActive(WorkspaceModuleId::markers));
        refreshTimelineMarkers();
        requestSessionSettingsPersist(120);
        applyChromePresentation();
    }
    else if (commandId == commandIds::toggleOutputMeter
             || commandId == commandIds::toggleAmbisonicEnergy
             || commandId == commandIds::toggleParametricEq
             || commandId == commandIds::toggleTruePeakLimiter)
    {
        if (workspaceModuleHost == nullptr)
            return GuiCommandResult::failed("Workspace is unavailable.");
        const auto id = commandId == commandIds::toggleOutputMeter
            ? WorkspaceModuleId::outputMeter
            : (commandId == commandIds::toggleParametricEq
                   ? WorkspaceModuleId::parametricEq
                   : (commandId == commandIds::toggleTruePeakLimiter
                          ? WorkspaceModuleId::truePeakLimiter
                          : WorkspaceModuleId::ambisonicEnergy));
        const auto enable = !workspaceModuleHost->isModuleActive(id);
        if (id != WorkspaceModuleId::parametricEq
            && id != WorkspaceModuleId::truePeakLimiter
            && enable)
        {
            const auto lifecycleId = id == WorkspaceModuleId::outputMeter
                ? OptionalModuleId::meter
                : OptionalModuleId::ambisonics;
            const auto triggered = lifecycleTransitionAccepted(
                coreLifecycle.trigger(activeDocumentGeneration, lifecycleId));
            const auto prepared = triggered && lifecycleTransitionAccepted(
                coreLifecycle.prepare(activeDocumentGeneration, lifecycleId));
            if (!prepared)
            {
                coreLifecycle.release(activeDocumentGeneration, lifecycleId);
                return GuiCommandResult::failed("Optional module could not be prepared; no layout change was applied.");
            }
        }
        workspaceModuleHost->setModuleEnabled(id, enable);
        if (!enable)
        {
            if (id == WorkspaceModuleId::parametricEq)
                clearParametricEqPreview(true);
            else if (id == WorkspaceModuleId::truePeakLimiter)
                clearTruePeakLimiterPreview(true);
            else
                coreLifecycle.release(activeDocumentGeneration,
                                      id == WorkspaceModuleId::outputMeter
                                          ? OptionalModuleId::meter
                                          : OptionalModuleId::ambisonics);
        }
        requestSessionSettingsPersist(120);
        applyChromePresentation();
    }
    else if (commandId == commandIds::floatWaveform
             || commandId == commandIds::redockWaveform
             || commandId == commandIds::floatOutputMeter
             || commandId == commandIds::redockOutputMeter
             || commandId == commandIds::removeOutputMeter
             || commandId == commandIds::floatAmbisonicEnergy
             || commandId == commandIds::redockAmbisonicEnergy
             || commandId == commandIds::removeAmbisonicEnergy
             || commandId == commandIds::floatParametricEq
             || commandId == commandIds::redockParametricEq
             || commandId == commandIds::removeParametricEq
             || commandId == commandIds::floatTruePeakLimiter
             || commandId == commandIds::redockTruePeakLimiter
             || commandId == commandIds::removeTruePeakLimiter
             || sidebarModuleForWindowCommand(commandId).has_value())
    {
        if (workspaceModuleHost == nullptr)
            return GuiCommandResult::failed("Workspace is unavailable.");

        const auto id = sidebarModuleForWindowCommand(commandId).value_or(
                       commandId == commandIds::floatWaveform
                     || commandId == commandIds::redockWaveform
            ? WorkspaceModuleId::coreWaveform
            : (commandId == commandIds::floatOutputMeter
               || commandId == commandIds::redockOutputMeter
               || commandId == commandIds::removeOutputMeter
                ? WorkspaceModuleId::outputMeter
                : (commandId == commandIds::floatParametricEq
                   || commandId == commandIds::redockParametricEq
                   || commandId == commandIds::removeParametricEq
                    ? WorkspaceModuleId::parametricEq
                    : (commandId == commandIds::floatTruePeakLimiter
                       || commandId == commandIds::redockTruePeakLimiter
                       || commandId == commandIds::removeTruePeakLimiter
                           ? WorkspaceModuleId::truePeakLimiter
                           : WorkspaceModuleId::ambisonicEnergy))));
        const auto isFloat = commandId == commandIds::floatWaveform
                          || commandId == commandIds::floatOutputMeter
                           || commandId == commandIds::floatAmbisonicEnergy
                           || commandId == commandIds::floatParametricEq
                           || commandId == commandIds::floatTruePeakLimiter
                           || commandId == commandIds::floatPlaylistMag
                           || commandId == commandIds::floatMetadataMag
                           || commandId == commandIds::floatMarkers;
        const auto isRemove = commandId == commandIds::removeOutputMeter
                           || commandId == commandIds::removeAmbisonicEnergy
                           || commandId == commandIds::removeParametricEq
                           || commandId == commandIds::removeTruePeakLimiter
                           || commandId == commandIds::removePlaylistMag
                           || commandId == commandIds::removeMetadataMag
                           || commandId == commandIds::removeMarkers;
        const auto focusToken = captureFocusReturnToken("floating-window",
            id == WorkspaceModuleId::coreWaveform ? "core.waveform" : "workspace.modules");

        if (isFloat && id == WorkspaceModuleId::coreWaveform && !waveformFloatConfirmed)
        {
            // This build does not run nested JUCE modal loops. The immediate
            // bool return is not the user's answer; resume only in the callback.
            juce::Component::SafePointer<MainComponent> safeThis(this);
            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::NoIcon,
                "Float Audio Waveform",
                "The waveform will move to a separate native window. "
                "The main window keeps a lightweight return anchor, and closing the waveform window redocks it. Continue?",
                "Float Waveform",
                "Cancel",
                this,
                juce::ModalCallbackFunction::create([safeThis, context, focusToken] (int choice)
                {
                    if (safeThis == nullptr
                        || safeThis->activeDocumentGeneration.value != context.documentGeneration
                        || (context.cancellation != nullptr && context.cancellation->isCancelled()))
                    {
                        return;
                    }
                    if (choice != 1)
                    {
                        safeThis->restoreFocusReturnToken(focusToken);
                        return;
                    }
                    const auto available = safeThis->resolveWorkspaceCommandState(commandIds::floatWaveform, context);
                    if (!available.enabled || !available.visible)
                    {
                        safeThis->reportCommandResult({ GuiCommandStatus::disabled, available.disabledReason });
                        safeThis->restoreFocusReturnToken(focusToken);
                        return;
                    }
                    const auto result = safeThis->performWorkspaceCommand(commandIds::floatWaveform, context, true);
                    safeThis->reportCommandResult(result);
                    if (!result.succeeded())
                        safeThis->restoreFocusReturnToken(focusToken);
                }));
            return GuiCommandResult::noChange(); // No layout mutation while awaiting an answer.
        }

        const auto meterWasActive = workspaceModuleHost->isModuleActive(
            WorkspaceModuleId::outputMeter);
        const auto ambisonicsWasActive = workspaceModuleHost->isModuleActive(
            WorkspaceModuleId::ambisonicEnergy);
        const auto eqWasActive = workspaceModuleHost->isModuleActive(
            WorkspaceModuleId::parametricEq);
        const auto limiterWasActive = workspaceModuleHost->isModuleActive(
            WorkspaceModuleId::truePeakLimiter);
        const auto mutation = isFloat
            ? workspaceModuleHost->floatModule(id)
            : (isRemove ? (workspaceModuleHost->isModuleFloating(id)
                              ? workspaceModuleHost->removeFloatingTile(id)
                              : workspaceModuleHost->closeModule(id))
                        : workspaceModuleHost->redockModule(id));
        if (mutation == WorkspaceGridMutationResult::noChange)
            return GuiCommandResult::noChange();
        if (mutation != WorkspaceGridMutationResult::committed)
            return GuiCommandResult::failed("The floating-window layout change could not be applied safely.");

        if (isRemove)
        {
            if (meterWasActive
                && !workspaceModuleHost->isModuleActive(WorkspaceModuleId::outputMeter))
            {
                coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::meter);
            }
            if (ambisonicsWasActive
                && !workspaceModuleHost->isModuleActive(WorkspaceModuleId::ambisonicEnergy))
            {
                coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::ambisonics);
            }
            if (eqWasActive
                && !workspaceModuleHost->isModuleActive(WorkspaceModuleId::parametricEq))
            {
                clearParametricEqPreview(true);
            }
            if (limiterWasActive
                && !workspaceModuleHost->isModuleActive(WorkspaceModuleId::truePeakLimiter))
            {
                clearTruePeakLimiterPreview(true);
            }
        }

        if (isFloat)
        {
            juce::Component::SafePointer<MainComponent> safeThis(this);
            juce::MessageManager::callAsync([safeThis, id, generation = activeDocumentGeneration.value]
            {
                if (safeThis != nullptr && safeThis->activeDocumentGeneration.value == generation
                    && safeThis->workspaceModuleHost != nullptr)
                {
                    safeThis->workspaceModuleHost->focusFloatingModule(id);
                }
            });
        }
        else
        {
            restoreFocusReturnToken(focusToken);
        }
        applyChromePresentation();
    }
    else if (commandId == commandIds::resetModuleLayout)
    {
        if (workspaceModuleHost == nullptr)
            return GuiCommandResult::failed("Workspace is unavailable.");
        const auto prepareMeter = workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::outputMeter)
                               && !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::outputMeter);
        const auto prepareAmbisonics = workspaceModuleHost->isModuleAvailable(WorkspaceModuleId::ambisonicEnergy)
                                    && !workspaceModuleHost->isModuleEnabled(WorkspaceModuleId::ambisonicEnergy);
        const auto prepareModule = [this] (OptionalModuleId module)
        {
            if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration, module)))
                return false;
            if (lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration, module)))
                return true;
            coreLifecycle.release(activeDocumentGeneration, module);
            return false;
        };
        if (prepareMeter && !prepareModule(OptionalModuleId::meter))
        {
            return GuiCommandResult::failed("Output Meter could not be prepared; no layout change was applied.");
        }
        if (prepareAmbisonics && !prepareModule(OptionalModuleId::ambisonics))
        {
            if (prepareMeter)
                coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::meter);
            return GuiCommandResult::failed("Ambisonic Energy could not be prepared; no layout change was applied.");
        }
        workspacePresetWritable = true;
        if (!workspacePresetCatalogWritable)
        {
            workspacePresetCatalog->reset();
            workspacePresetCatalogLoaded = true;
            workspacePresetCatalogWritable = true;
            workspacePresetCatalogRestoreStatus = WorkspacePresetCatalogRestoreStatus::noState;
        }
        workspaceModuleHost->resetUserLayout();
        applyChromePresentation();
    }
    else
        return { GuiCommandStatus::notFound, "Unknown Workspace command." };

    return GuiCommandResult::committed();
}

GuiCommandDispatchContext MainComponent::makeCommandContext(
    GuiCommandSource source,
    std::string targetScope) const
{
    return { activeDocumentGeneration.value,
             source,
             std::move(targetScope),
             {} };
}

GuiCommandResult MainComponent::dispatchGuiCommand(
    std::string_view commandId,
    GuiCommandSource source,
    std::string targetScope)
{
    const auto result = commandRegistry.dispatch(commandId,
                                                 makeCommandContext(source, std::move(targetScope)));
    reportCommandResult(result);
    return result;
}

void MainComponent::reportCommandResult(const GuiCommandResult& result)
{
    if (result.succeeded() || result.feedback.empty())
        return;
    engineStatusLabel.setText(juce::String::fromUTF8(result.feedback.c_str()), juce::dontSendNotification);
}

void MainComponent::showCommandPalette()
{
    if (commandPalette != nullptr)
    {
        commandPalette->toFront(true);
        commandPalette->focusSearch();
        return;
    }

    commandPaletteReturnToken = captureFocusReturnToken("palette", "application.command-palette.open");
    commandPalette = std::make_unique<CommandPaletteComponent>(
        commandRegistry,
        makeCommandContext(GuiCommandSource::palette, "command-palette"));
    commandPalette->onCommandChosen = [this] (std::string commandId)
    {
        dismissCommandPalette();
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, commandId = std::move(commandId)]
        {
            if (safeThis != nullptr)
                safeThis->dispatchGuiCommand(commandId,
                                             GuiCommandSource::palette,
                                             "command-palette");
        });
    };
    commandPalette->onDismiss = [this] { dismissCommandPalette(); };
    addAndMakeVisible(*commandPalette);
    resized();
    commandPalette->setBackdropSource(workspaceModuleHost.get(), coreLifecycle.isCoreReady(activeDocumentGeneration), &waveformView);
    commandPalette->focusSearch();
}

void MainComponent::dismissCommandPalette()
{
    if (commandPalette == nullptr)
        return;

    commandPalette->setVisible(false);
    removeChildComponent(commandPalette.get());
    commandPalette.reset();
    if (commandPaletteReturnToken.has_value())
    {
        auto token = std::move(*commandPaletteReturnToken);
        commandPaletteReturnToken.reset();
        restoreFocusReturnToken(std::move(token));
    }
}

ShortcutFocusContext MainComponent::currentShortcutFocusContext() const
{
    if (juce::Component::getCurrentlyModalComponent() != nullptr)
        return ShortcutFocusContext::modalFlow;

    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused == nullptr)
        return ShortcutFocusContext::none;
    if (commandPalette != nullptr
        && (focused == commandPalette.get() || commandPalette->isParentOf(focused)))
    {
        return ShortcutFocusContext::shortcutCapture;
    }
    if (dynamic_cast<juce::TextEditor*>(focused) != nullptr)
        return ShortcutFocusContext::textEditing;
    if (dynamic_cast<juce::ComboBox*>(focused) != nullptr)
        return ShortcutFocusContext::menuOrPopup;
    if (auto* button = dynamic_cast<juce::Button*>(focused))
        return button->getClickingTogglesState() ? ShortcutFocusContext::nativeToggle
                                                 : ShortcutFocusContext::nativeButton;
    if (focused == &waveformView || waveformView.isParentOf(focused))
        return ShortcutFocusContext::readOnlySurface;
    return ShortcutFocusContext::none;
}

FocusReturnToken MainComponent::captureFocusReturnToken(
    std::string sourceScope,
    std::string semanticAnchor) const
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused != nullptr && focused->getComponentID().isNotEmpty())
        semanticAnchor = focused->getComponentID().toStdString();
    auto placementId = std::string {};
    auto tabStackId = std::string {};
    auto tabInstanceId = std::string {};
    if (workspaceModuleHost != nullptr)
    {
        if (const auto identity = workspaceModuleHost->currentFocusIdentity())
        {
            placementId = identity->placementId;
            tabStackId = identity->tabStackId;
            tabInstanceId = identity->tabInstanceId;
        }
    }
    return { activeDocumentGeneration.value,
             workspaceFocusGeneration,
             std::move(sourceScope),
             std::move(semanticAnchor),
             std::move(placementId),
             std::move(tabStackId),
             std::move(tabInstanceId),
             reinterpret_cast<std::uintptr_t>(focused),
             false };
}

void MainComponent::restoreFocusReturnToken(FocusReturnToken token)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, token = std::move(token)]
    {
        if (safeThis == nullptr)
            return;

        auto* semantic = safeThis->componentForSemanticAnchor(token.semanticAnchor);
        const auto semanticUsable = semantic != nullptr && semantic->isShowing()
                                 && semantic->isEnabled() && semantic->getWantsKeyboardFocus();
        const auto hostUsable = safeThis->workspaceModuleHost != nullptr
                             && safeThis->workspaceModuleHost->isShowing()
                             && safeThis->workspaceModuleHost->hasFocusableVisibleModule();
        FocusCandidateState candidates;
        candidates.sameNativeControl = semanticUsable
                                    && reinterpret_cast<std::uintptr_t>(semantic)
                                           == token.weakNativeHandle;
        candidates.sameSemanticControl = semanticUsable;
        candidates.activeTabHeader = hostUsable;
        candidates.sameMagChrome = hostUsable;
        candidates.nearestReadingOrderMag = hostUsable;
        candidates.coreWaveform = safeThis->waveformView.isShowing();
        candidates.minimalTransport = safeThis->playButton.isShowing() || safeThis->openButton.isShowing();
        const auto target = chooseFocusRestoreTarget(token,
                                                     safeThis->activeDocumentGeneration.value,
                                                     safeThis->workspaceFocusGeneration,
                                                     candidates);
        switch (target)
        {
            case FocusRestoreTarget::sameNativeControl:
            case FocusRestoreTarget::sameSemanticControl:
                semantic->grabKeyboardFocus();
                break;
            case FocusRestoreTarget::activeTabHeader:
            case FocusRestoreTarget::sameMagChrome:
            case FocusRestoreTarget::nearestReadingOrderMag:
                safeThis->workspaceModuleHost->focusActiveModule();
                break;
            case FocusRestoreTarget::coreWaveform:
                safeThis->waveformView.grabKeyboardFocus();
                break;
            case FocusRestoreTarget::minimalTransport:
                (safeThis->playButton.isShowing() && safeThis->playButton.isEnabled()
                     ? static_cast<juce::Component&>(safeThis->playButton)
                     : static_cast<juce::Component&>(safeThis->openButton)).grabKeyboardFocus();
                break;
            case FocusRestoreTarget::none:
                break;
        }
    });
}

juce::Component* MainComponent::componentForSemanticAnchor(std::string_view anchor) const
{
    if (anchor.empty())
        return nullptr;
    std::function<juce::Component*(juce::Component*)> find = [&] (juce::Component* root) -> juce::Component*
    {
        if (root == nullptr)
            return nullptr;
        if (root->getComponentID().toStdString() == anchor)
            return root;
        for (auto* child : root->getChildren())
            if (auto* result = find(child))
                return result;
        return nullptr;
    };
    return find(const_cast<MainComponent*>(this));
}

bool MainComponent::cycleMajorRegionFocus(bool reverse)
{
    traceNativeFocus("cycle.before", *this);
    const auto workspaceVisible = workspaceModuleHost != nullptr && workspaceModuleHost->isShowing()
                               && workspaceModuleHost->hasFocusableVisibleModule();
    const std::vector<MajorRegionState> regions {
        { MajorRegion::transport, actionBar.isShowing(), true },
        { MajorRegion::coreWaveform, waveformView.isShowing(), true },
        { MajorRegion::workspace, workspaceVisible, workspaceVisible },
        { MajorRegion::inspector, workspaceModuleHost == nullptr
            && metadataSidebar && metadataSidebar->isShowing() && !metadataSidebarCollapsed, true }
    };
    auto current = MajorRegion::transport;
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (focused == &waveformView || (focused != nullptr && waveformView.isParentOf(focused)))
        current = MajorRegion::coreWaveform;
    else if (workspaceModuleHost != nullptr && focused != nullptr
             && (focused == workspaceModuleHost.get() || workspaceModuleHost->isParentOf(focused)))
        current = MajorRegion::workspace;
    else if (metadataSidebar && focused != nullptr
             && (focused == metadataSidebar.get() || metadataSidebar->isParentOf(focused)))
        current = MajorRegion::inspector;

    switch (cycleMajorRegion(regions, current, reverse))
    {
        case MajorRegion::transport:
            (playButton.isEnabled() ? static_cast<juce::Component&>(playButton)
                                    : static_cast<juce::Component&>(openButton)).grabKeyboardFocus();
            break;
        case MajorRegion::coreWaveform:
            waveformView.grabKeyboardFocus();
            break;
        case MajorRegion::workspace:
            workspaceModuleHost->focusActiveModule();
            break;
        case MajorRegion::inspector:
            if (metadataSidebar) metadataSidebar->grabKeyboardFocus();
            break;
    }
    traceNativeFocus("cycle.after", *this);
    return true;
}

bool MainComponent::handleTopLevelKeyPress(const juce::KeyPress& key)
{
    // The custom window caption can own focus without a workspace child in the
    // key dispatch chain. Route only this global command, not text/edit keys.
    if (hostWindow.isActiveWindow() && juce::Component::getCurrentlyModalComponent() == nullptr
        && (key == juce::KeyPress('p', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0)
            || key == juce::KeyPress('p', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)))
    {
        dispatchGuiCommand(commandIds::palette, GuiCommandSource::nativeWindow, "active-window");
        return true;
    }
    if (key == juce::KeyPress::spaceKey)
    {
        const auto route = routeTransportShortcut({ juce::JUCEApplicationBase::isStandaloneApp()
                                                        && hostWindow.isActiveWindow(),
                                                    hostWindow.isActiveWindow(),
                                                    true,
                                                    false,
                                                    false,
                                                    currentShortcutFocusContext() });
        if (route == TransportShortcutRoute::dispatchTogglePlayback)
        {
            dispatchGuiCommand(commandIds::togglePlayback, GuiCommandSource::nativeWindow, "active-window");
            return true;
        }
        return false;
    }
    return false;
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    traceNativeFocus("main.key", *this, &key);
    const auto commandOrCtrl = [] (const juce::KeyPress& candidate, int character, bool shift)
    {
        const auto shiftFlag = shift ? juce::ModifierKeys::shiftModifier : 0;
        return candidate == juce::KeyPress(character, juce::ModifierKeys::commandModifier | shiftFlag, 0)
            || candidate == juce::KeyPress(character, juce::ModifierKeys::ctrlModifier | shiftFlag, 0);
    };

    if (!deferChromeForStartup
        && key == juce::KeyPress('v', juce::ModifierKeys::altModifier, 0))
    {
        const auto menuAnchor = localPointToGlobal(juce::Point<int>(40, 40));
        showTitleBarMenu(CustomTitleBarComponent::MenuKind::view,
                         { menuAnchor.x, menuAnchor.y, 1, 1 });
        return true;
    }

    if (commandOrCtrl(key, 'p', true))
    {
        dispatchGuiCommand(commandIds::palette, GuiCommandSource::shortcut);
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::F6Key
        && !key.getModifiers().isCtrlDown()
        && !key.getModifiers().isCommandDown()
        && !key.getModifiers().isAltDown())
    {
        return cycleMajorRegionFocus(key.getModifiers().isShiftDown());
    }

    if (key.getKeyCode() == juce::KeyPress::tabKey
        && (key.getModifiers().isCtrlDown() || key.getModifiers().isCommandDown())
        && workspaceModuleHost != nullptr)
    {
        const auto focusToken = captureFocusReturnToken("tab-stack", "workspace.modules");
        if (workspaceModuleHost->activateAdjacentTab(key.getModifiers().isShiftDown()))
        {
            restoreFocusReturnToken(focusToken);
            return true;
        }
    }

    if (key == juce::KeyPress::escapeKey)
    {
        if (workspaceModuleHost != nullptr && workspaceModuleHost->cancelPointerGesture())
            return true;
        const auto context = commandPalette != nullptr
            ? EscapeContext::palette
            : (!referenceAudioSuiteApplyInFlight
               && (selectionPopup.isVisible() || std::abs(selectionGainSlider.getValue()) > 0.01)
                   ? EscapeContext::localEdit
                   : EscapeContext::none);
        switch (escapeOutcome(context))
        {
            case EscapeOutcome::closeTransientSurface:
                dismissCommandPalette();
                return true;
            case EscapeOutcome::cancelLocalEdit:
            case EscapeOutcome::cancelPreviewTransaction:
                dismissSelectionPopup();
                return true;
            case EscapeOutcome::notHandled:
                break;
        }
    }

    if (key == juce::KeyPress::spaceKey)
        return handleTopLevelKeyPress(key);

    if (key == juce::KeyPress::homeKey)
    {
        dispatchGuiCommand(commandIds::start, GuiCommandSource::shortcut);
        return true;
    }
    if (key == juce::KeyPress::pageUpKey)
    {
        dispatchGuiCommand(commandIds::previousPlaylist, GuiCommandSource::shortcut);
        return true;
    }
    if (key == juce::KeyPress::pageDownKey)
    {
        dispatchGuiCommand(commandIds::nextPlaylist, GuiCommandSource::shortcut);
        return true;
    }
    if (key == juce::KeyPress::leftKey)
    {
        dispatchGuiCommand(commandIds::previousFrame, GuiCommandSource::shortcut);
        return true;
    }
    if (key == juce::KeyPress::rightKey)
    {
        dispatchGuiCommand(commandIds::nextFrame, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 'a', false)
        && document.hasAudio()
        && !referenceAudioSuiteApplyInFlight
        && !parametricEqApplyInFlight
        && !truePeakLimiterApplyInFlight)
    {
        selectedSourceChannels.clear();
        waveformView.setSelectedSourceChannels({});
        waveformView.selectWholeFile();
        return true;
    }
    if (commandOrCtrl(key, 'z', false))
    {
        dispatchGuiCommand(commandIds::undo, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 'y', false))
    {
        dispatchGuiCommand(commandIds::redo, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 's', true))
    {
        dispatchGuiCommand(commandIds::saveAs, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 's', false))
    {
        dispatchGuiCommand(commandIds::save, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 'o', false))
    {
        dispatchGuiCommand(commandIds::open, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, ',', false))
    {
        dispatchGuiCommand(commandIds::preferences, GuiCommandSource::shortcut);
        return true;
    }
    if (commandOrCtrl(key, 'w', false))
    {
        dispatchGuiCommand(commandIds::close, GuiCommandSource::shortcut);
        return true;
    }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        && currentSelection.has_value())
    {
        dispatchGuiCommand(commandIds::deleteSelection, GuiCommandSource::shortcut);
        return true;
    }

    const auto character = key.getTextCharacter();
    if (character == 'q' || character == 'Q')
    {
        dispatchGuiCommand(commandIds::zoomAmplitudeOut, GuiCommandSource::shortcut);
        return true;
    }
    if (character == 'w' || character == 'W')
    {
        dispatchGuiCommand(commandIds::zoomAmplitudeIn, GuiCommandSource::shortcut);
        return true;
    }
    if (character == 'r' || character == 'R')
    {
        dispatchGuiCommand(commandIds::zoomTimeOut, GuiCommandSource::shortcut);
        return true;
    }
    if (character == 't' || character == 'T')
    {
        dispatchGuiCommand(commandIds::zoomTimeIn, GuiCommandSource::shortcut);
        return true;
    }
    if (character == 'e' || character == 'E')
    {
        dispatchGuiCommand(commandIds::resetView, GuiCommandSource::shortcut);
        return true;
    }

    return false;
}
bool MainComponent::isSupportedAudioPath(const juce::String& path)
{
    return juce::File(path).existsAsFile();
}

bool MainComponent::isBnkFile(const juce::File& file) noexcept
{
    return file.hasFileExtension(".bnk");
}

bool MainComponent::isWemFile(const juce::File& file) noexcept
{
    return file.hasFileExtension(".wem");
}

juce::String MainComponent::formatTimecode(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;

    constexpr double displayFps = 30.0;
    const auto totalFrames = juce::roundToInt(seconds * displayFps);
    const auto hours = totalFrames / static_cast<int>(displayFps * 3600.0);
    const auto minutes = (totalFrames / static_cast<int>(displayFps * 60.0)) % 60;
    const auto secs = (totalFrames / static_cast<int>(displayFps)) % 60;
    const auto frames = totalFrames % static_cast<int>(displayFps);
    return juce::String::formatted("%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
}

juce::String MainComponent::formatAudioDetails(const AudioEngine::LoadedFileInfo& info)
{
    if (!info.isValid())
        return "Native formats only at startup";

    auto rate = info.sampleRate > 0.0 ? juce::String(info.sampleRate / 1000.0, 1) + " kHz" : "--";
    auto channels = info.numChannels == 1 ? juce::String("Mono")
                  : info.numChannels == 2 ? juce::String("Stereo")
                  : juce::String(info.numChannels) + " ch";
    return rate + " / " + channels + " / " + juce::String(info.bitsPerSample) + "-bit";
}

juce::String MainComponent::formatBnkDetails(const BnkDocument::BankInfo& info)
{
    if (!info.isValid())
        return "Wwise BNK";

    juce::StringArray summary;
    summary.add("Wwise BNK");
    summary.add("Events " + juce::String(info.eventCount));
    summary.add("Media " + juce::String(info.mediaCount));
    summary.add("HIRC " + juce::String(info.hircObjectCount));

    if (info.version != 0)
        summary.add("Version " + juce::String(info.version));

    if (info.bankId != 0)
        summary.add("Bank 0x" + juce::String::toHexString(static_cast<juce::int64>(info.bankId)).toUpperCase());

    return summary.joinIntoString(" / ");
}

juce::String MainComponent::formatPlaybackState(AudioEngine::PlaybackState state)
{
    switch (state)
    {
        case AudioEngine::PlaybackState::playing: return "Playing";
        case AudioEngine::PlaybackState::paused: return "Paused";
        case AudioEngine::PlaybackState::stopped: return "Stopped";
    }

    return "Stopped";
}

juce::String MainComponent::formatSelectionRange(const std::optional<juce::Range<double>>& selection)
{
    if (!selection.has_value())
        return "Selection: --";

    constexpr double displayFps = 30.0;
    const auto frameCount = juce::roundToInt(selection->getLength() * displayFps);
    return "Selection: " + formatTimecode(selection->getStart())
         + " -> " + formatTimecode(selection->getEnd())
         + "  (" + juce::String(selection->getLength(), 3) + " s | " + juce::String(frameCount) + " fr)";
}

juce::String MainComponent::formatMeterReadout(AudioEngine::MeterMode mode, const AudioEngine& engine)
{
    switch (mode)
    {
        case AudioEngine::MeterMode::truePeak:
            return "TP " + juce::String(juce::Decibels::gainToDecibels(engine.getTruePeakLinear(), -120.0f), 1) + " dBTP";

        case AudioEngine::MeterMode::rms:
            return "RMS " + juce::String(juce::Decibels::gainToDecibels(engine.getOutputRmsLinear(), -120.0f), 1) + " dBFS";

        case AudioEngine::MeterMode::lufs:
            return "LUFS " + juce::String(engine.getMomentaryLufs(), 1);
    }

    return "TP --";
}

void MainComponent::chooseAndOpenFile()
{
    const auto focusToken = captureFocusReturnToken("dialog", "document.open");
    fileChooser = std::make_unique<juce::FileChooser>("Open file",
                                                      workspaceSession.getDefaultBrowseDirectory(),
                                                      "*");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
                             [this, focusToken](const juce::FileChooser& chooser)
                             {
                                 auto results = chooser.getResults();
                                 fileChooser.reset();
                                 std::vector<juce::File> files;
                                 files.reserve(static_cast<std::size_t>(results.size()));
                                 for (const auto& result : results)
                                 {
                                     if (result.existsAsFile())
                                         files.push_back(result);
                                 }

                                 if (!files.empty())
                                     openFiles(files, 0);
                                 restoreFocusReturnToken(focusToken);
                             });
}

void MainComponent::showRecentFilesMenu()
{
    auto validFiles = workspaceSession.getValidRecentFiles(true);
    workspaceSession.persistRecentFiles();
    persistSessionSettings();
    recentButton.setEnabled(workspaceSession.hasRecentFiles());

    juce::PopupMenu menu;
    for (int index = 0; index < static_cast<int>(validFiles.size()); ++index)
    {
        const auto& file = validFiles[static_cast<std::size_t>(index)];
        menu.addItem(index + 1, file.getFileName() + "   " + file.getParentDirectory().getFullPathName());
    }

    if (validFiles.empty())
        menu.addItem(900, "No recent files", false, false);

    menu.addSeparator();
    menu.addItem(1000, "Clear Recent Files", !validFiles.empty());

    auto* anchor = recentButton.isVisible() ? static_cast<juce::Component*>(&recentButton)
                                           : static_cast<juce::Component*>(&toolbarMoreButton);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(anchor),
                       [this, validFiles = std::move(validFiles)](int result)
                       {
                           if (result >= 1 && result <= static_cast<int>(validFiles.size()))
                           {
                               openFiles({ validFiles[static_cast<std::size_t>(result - 1)] }, 0);
                               return;
                           }

                           if (result == 1000)
                           {
                               workspaceSession.clearRecentFiles();
                               persistSessionSettings();
                               recentButton.setEnabled(false);
                               engineStatusLabel.setText("Recent file list cleared.", juce::dontSendNotification);
                           }
                       });
}

void MainComponent::showTitleBarMenu(CustomTitleBarComponent::MenuKind menuKind, juce::Rectangle<int> targetScreenBounds)
{
    auto validRecentFiles = workspaceSession.getValidRecentFiles(true);

    juce::PopupMenu recentSubmenu;
    for (int index = 0; index < static_cast<int>(validRecentFiles.size()) && index < 8; ++index)
    {
        const auto& file = validRecentFiles[static_cast<std::size_t>(index)];
        recentSubmenu.addItem(100 + index, file.getFileName());
    }

    if (validRecentFiles.empty())
        recentSubmenu.addItem(199, "No recent files", false, false);

    const auto context = makeCommandContext(GuiCommandSource::menu, "title-menu");
    const auto addCommandItem = [this, &context] (juce::PopupMenu& target,
                                                   int itemId,
                                                   std::string_view commandId,
                                                   const juce::String& fallbackLabel)
    {
        if (const auto snapshot = commandRegistry.resolve(commandId, context))
        {
            auto label = juce::String::fromUTF8(snapshot->descriptor.label.c_str());
            if (label.isEmpty())
                label = fallbackLabel;
            target.addItem(itemId, label, snapshot->state.enabled, snapshot->state.checked);
        }
        else
        {
            target.addItem(itemId, fallbackLabel, false, false);
        }
    };

    juce::PopupMenu menu;
    switch (menuKind)
    {
        case CustomTitleBarComponent::MenuKind::file:
            addCommandItem(menu, 1, commandIds::open, "Open...");
            menu.addSubMenu("Open Recent", recentSubmenu, !validRecentFiles.empty());
            menu.addSeparator();
            addCommandItem(menu, 2, commandIds::save, "Save");
            addCommandItem(menu, 3, commandIds::saveAs, "Save As...");
            addCommandItem(menu, 6, commandIds::exportAudio, "Export Audio...");
            menu.addSeparator();
            addCommandItem(menu, 4, commandIds::preferences, "Preferences");
            menu.addSeparator();
            addCommandItem(menu, 5, commandIds::close, "Exit");
            break;

        case CustomTitleBarComponent::MenuKind::edit:
            addCommandItem(menu, 10, commandIds::undo, "Undo");
            addCommandItem(menu, 11, commandIds::redo, "Redo");
            break;

        case CustomTitleBarComponent::MenuKind::view:
        {
            addCommandItem(menu, 29, commandIds::palette, "Command Palette");
            menu.addSeparator();
            addCommandItem(menu, 20, commandIds::togglePlaylistSidebar, "Toggle Playlist");
            addCommandItem(menu, 21, commandIds::toggleMetadataSidebar, "Toggle Metadata");
            menu.addSeparator();
            addCommandItem(menu, 22, commandIds::waveform, "Waveform View");
            addCommandItem(menu, 23, commandIds::spectral, "Spectral View");
            menu.addSeparator();
            addCommandItem(menu, 24, commandIds::stacked, "Stacked Channels");
            addCommandItem(menu, 25, commandIds::mixed, "Mixed Channels");

            juce::PopupMenu moduleMenu;
            addCommandItem(moduleMenu, 26, commandIds::toggleOutputMeter, "Output Meter");
            addCommandItem(moduleMenu, 27, commandIds::toggleAmbisonicEnergy, "Ambisonic Energy");
            addCommandItem(moduleMenu, 70, commandIds::toggleSpatialEnergy, "Rendered Output Spatial Energy");
            addCommandItem(moduleMenu, 71, commandIds::floatSpatialEnergy, "Float Spatial Energy");
            addCommandItem(moduleMenu, 72, commandIds::redockSpatialEnergy, "Redock Spatial Energy");
            addCommandItem(moduleMenu, 47, commandIds::toggleParametricEq, "Parametric EQ");
            addCommandItem(moduleMenu, 50, commandIds::toggleTruePeakLimiter, "True-Peak Limiter");
            moduleMenu.addSeparator();
            addCommandItem(moduleMenu, 41, commandIds::floatWaveform, "Float Audio Waveform");
            addCommandItem(moduleMenu, 42, commandIds::redockWaveform, "Redock Audio Waveform");
            addCommandItem(moduleMenu, 43, commandIds::floatOutputMeter, "Float Output Meter");
            addCommandItem(moduleMenu, 44, commandIds::redockOutputMeter, "Redock Output Meter");
            addCommandItem(moduleMenu, 45, commandIds::floatAmbisonicEnergy, "Float Ambisonic Energy");
            addCommandItem(moduleMenu, 46, commandIds::redockAmbisonicEnergy, "Redock Ambisonic Energy");
            addCommandItem(moduleMenu, 48, commandIds::floatParametricEq, "Float Parametric EQ");
            addCommandItem(moduleMenu, 49, commandIds::redockParametricEq, "Redock Parametric EQ");
            addCommandItem(moduleMenu, 51, commandIds::floatTruePeakLimiter, "Float True-Peak Limiter");
            addCommandItem(moduleMenu, 52, commandIds::redockTruePeakLimiter, "Redock True-Peak Limiter");
            moduleMenu.addSeparator();
            addCommandItem(moduleMenu, 28, commandIds::resetModuleLayout, "Reset Module Layout");
            menu.addSeparator();
            menu.addSubMenu("Modules", moduleMenu, workspaceModuleHost != nullptr);
            break;
        }

        case CustomTitleBarComponent::MenuKind::window:
            addCommandItem(menu, 30, commandIds::minimize, "Minimize");
            addCommandItem(menu, 31, commandIds::maximize,
                           hostWindow.isFullScreen() ? "Restore" : "Maximize");
            menu.addSeparator();
            addCommandItem(menu, 32, commandIds::close, "Close Window");
            break;

        case CustomTitleBarComponent::MenuKind::help:
            addCommandItem(menu, 53, commandIds::recoveryCenter, "Recovery Center");
            addCommandItem(menu, 54, commandIds::diagnostics, "Local Diagnostics");
            addCommandItem(menu, 40, commandIds::about, "About Single Editor");
            break;
    }

    const auto focusToken = captureFocusReturnToken("menu", "title-menu");
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(targetScreenBounds).withParentComponent(this),
        [safeThis = juce::Component::SafePointer<MainComponent>(this),
         validRecentFiles = std::move(validRecentFiles),
         focusToken] (int result)
        {
            if (safeThis == nullptr)
                return;

            if (result == 0)
            {
                safeThis->restoreFocusReturnToken(focusToken);
                return;
            }

            if (result >= 100 && result < 108)
            {
                const auto index = result - 100;
                if (juce::isPositiveAndBelow(index, static_cast<int>(validRecentFiles.size())))
                    safeThis->openFiles({ validRecentFiles[static_cast<std::size_t>(index)] }, 0);
                return;
            }

            std::string_view commandId;
            switch (result)
            {
                case 1: commandId = commandIds::open; break;
                case 2: commandId = commandIds::save; break;
                case 3: commandId = commandIds::saveAs; break;
                case 4: commandId = commandIds::preferences; break;
                case 5: commandId = commandIds::close; break;
                case 6: commandId = commandIds::exportAudio; break;
                case 10: commandId = commandIds::undo; break;
                case 11: commandId = commandIds::redo; break;
                case 20: commandId = commandIds::togglePlaylistSidebar; break;
                case 21: commandId = commandIds::toggleMetadataSidebar; break;
                case 22: commandId = commandIds::waveform; break;
                case 23: commandId = commandIds::spectral; break;
                case 24: commandId = commandIds::stacked; break;
                case 25: commandId = commandIds::mixed; break;
                case 26: commandId = commandIds::toggleOutputMeter; break;
                case 27: commandId = commandIds::toggleAmbisonicEnergy; break;
                case 70: commandId = commandIds::toggleSpatialEnergy; break;
                case 71: commandId = commandIds::floatSpatialEnergy; break;
                case 72: commandId = commandIds::redockSpatialEnergy; break;
                case 47: commandId = commandIds::toggleParametricEq; break;
                case 50: commandId = commandIds::toggleTruePeakLimiter; break;
                case 28: commandId = commandIds::resetModuleLayout; break;
                case 29: commandId = commandIds::palette; break;
                case 30: commandId = commandIds::minimize; break;
                case 31: commandId = commandIds::maximize; break;
                case 32: commandId = commandIds::close; break;
                case 41: commandId = commandIds::floatWaveform; break;
                case 42: commandId = commandIds::redockWaveform; break;
                case 43: commandId = commandIds::floatOutputMeter; break;
                case 44: commandId = commandIds::redockOutputMeter; break;
                case 45: commandId = commandIds::floatAmbisonicEnergy; break;
                case 46: commandId = commandIds::redockAmbisonicEnergy; break;
                case 48: commandId = commandIds::floatParametricEq; break;
                case 49: commandId = commandIds::redockParametricEq; break;
                case 51: commandId = commandIds::floatTruePeakLimiter; break;
                case 52: commandId = commandIds::redockTruePeakLimiter; break;
                case 40: commandId = commandIds::about; break;
                case 53: commandId = commandIds::recoveryCenter; break;
                case 54: commandId = commandIds::diagnostics; break;
                default: break;
            }

            if (!commandId.empty())
                safeThis->dispatchGuiCommand(commandId, GuiCommandSource::menu, "title-menu");
        });
}
void MainComponent::restoreSavedAudioOutputAfterStartupHandoff()
{
    if (savedAudioOutputStarted || !AsioSettingsSession::hasSavedSelection(appProperties)) return;
    savedAudioOutputStarted = true;
    if (!asioSettingsSession) asioSettingsSession = std::make_shared<AsioSettingsSession>(audioEngine, appProperties);
    asioSettingsSession->restoreSavedSelection();
    setDocumentOperationStatus(asioSettingsSession->status());
    if (!asioSettingsSession->busy() && !audioEngine.isSelectedOutputReady()) promoteDeferredChromeIfNeeded();
}

void MainComponent::showAsioSettings()
{
    if (asioSettingsWindow) { asioSettingsWindow->toFront(true); return; }
    if (!asioSettingsSession) asioSettingsSession = std::make_shared<AsioSettingsSession>(audioEngine, appProperties);
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Audio output routing";
    options.content.setOwned(new AsioSettingsComponent(asioSettingsSession));
    options.componentToCentreAround = this;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.escapeKeyTriggersCloseButton = true;
    options.dialogBackgroundColour = ModernAppleLookAndFeel::windowBackground();
    asioSettingsWindow = options.launchAsync();
    if (asioSettingsWindow)
    {
        asioSettingsWindow->setResizeLimits(820, 600, 1800, 1200);
        asioSettingsWindow->centreWithSize(1040, 720);
    }
    if (!asioSettingsSession->capabilities() && !asioSettingsSession->busy()) asioSettingsSession->refreshDrivers();
}

void MainComponent::showPreferences()
{
    const auto focusToken = captureFocusReturnToken("dialog", "application.preferences");
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Preferences";
    std::function<void()> openAsio;
    openAsio = [safeThis = juce::Component::SafePointer<MainComponent>(this)] { if (safeThis) safeThis->showAsioSettings(); };
    options.content.setOwned(new PreferencesComponent(audioEngine, appProperties, std::move(openAsio)));
    options.componentToCentreAround = this;
    options.useNativeTitleBar = false;
    options.resizable = true;
    options.escapeKeyTriggersCloseButton = true;
    options.dialogBackgroundColour = ModernAppleLookAndFeel::windowBackground();

    if (auto* dialog = options.launchAsync())
    {
        juce::ModalComponentManager::getInstance()->attachCallback(
            dialog,
            juce::ModalCallbackFunction::create(
                [safeThis = juce::Component::SafePointer<MainComponent>(this), focusToken] (int)
                {
                    if (safeThis != nullptr)
                        safeThis->restoreFocusReturnToken(focusToken);
                }));
        dialog->setResizable(true, true);
        dialog->centreWithSize(920, 860);
    }
}

void MainComponent::attachBnkProjectRoot()
{
    const auto bankFile = bnkDocument.getBankInfo().file;
    if (!bankFile.existsAsFile())
        return;

    auto initialRoot = workspaceSession.getMappedBnkProjectRoot(bankFile);
    if (!initialRoot.isDirectory())
        initialRoot = bnkDocument.getState() != nullptr ? bnkDocument.getState()->projectRoot : juce::File();
    if (!initialRoot.isDirectory())
        initialRoot = bankFile.getParentDirectory();

    const auto focusToken = captureFocusReturnToken("dialog", "bnk.attach-project-root");
    fileChooser = std::make_unique<juce::FileChooser>("Attach Wwise Project Root",
                                                      initialRoot,
                                                      "*");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectDirectories,
                             [this, bankFile, focusToken](const juce::FileChooser& chooser)
                             {
                                 auto chosenDirectory = chooser.getResult();
                                 fileChooser.reset();
                                 if (!chosenDirectory.isDirectory())
                                 {
                                     restoreFocusReturnToken(focusToken);
                                     return;
                                 }

                                 workspaceSession.setMappedBnkProjectRoot(bankFile, chosenDirectory);
                                 engineStatusLabel.setText("Attached Wwise project root: " + chosenDirectory.getFileName(),
                                                           juce::dontSendNotification);
            beginBnkOpen(bankFile, BnkOpenRequest::Capability::sidecar);
                                 restoreFocusReturnToken(focusToken);
                             });
}

void MainComponent::openFiles(const std::vector<juce::File>& files, int preferredIndex)
{
    if (files.empty())
        return;

    const auto safePreferredIndex = juce::jlimit(0, static_cast<int>(files.size()) - 1, preferredIndex);
    const auto targetFile = files[static_cast<std::size_t>(safePreferredIndex)];
    auto proceed = [this, files, safePreferredIndex]
    {
        workspaceSession.rememberOpenedFiles(files);
        requestSessionSettingsPersist();
        recentButton.setEnabled(workspaceSession.hasRecentFiles());
        const auto previousCount = playlistModel.getNumEntries();
        playlistModel.addFiles(files);

        int targetIndex = playlistModel.getCurrentIndex();
        if (previousCount == 0)
            targetIndex = juce::jlimit(0, playlistModel.getNumEntries() - 1, safePreferredIndex);
        else if (safePreferredIndex >= 0 && safePreferredIndex < static_cast<int>(files.size()))
        {
            const auto preferredFile = files[static_cast<std::size_t>(safePreferredIndex)];
            for (int i = 0; i < playlistModel.getNumEntries(); ++i)
            {
                const auto* entry = playlistModel.getEntry(i);
                if (entry != nullptr && !entry->recovered && entry->file == preferredFile)
                {
                    targetIndex = i;
                    break;
                }
            }
        }

        if (!playlistModel.setCurrentIndex(targetIndex))
            loadPlaylistIndex(targetIndex);
        // Playlist selection can synchronously begin another generation.
        scheduleWaveformCacheWarm(files, safePreferredIndex);
    };

    const auto currentFile = workspaceSession.getCurrentLoadedFile();
    const auto* currentEntry = playlistModel.getCurrentEntry();
    if (currentFile == juce::File() || currentFile == targetFile
        || (currentEntry && currentEntry->recovered && workspaceSession.isActiveRecoveredDocument(currentEntry->id)))
    {
        proceed();
        return;
    }

    confirmDiscardOrSaveIfNeeded("opening another file", std::move(proceed));
}

bool MainComponent::attachRecoveredCandidate(const recovery::RecoveredCandidateDocument& candidate)
{
    auto recovered = RecoveredPlaylistDocument::fromCandidate(candidate);
    return recovered && playlistModel.addRecoveredDocument(std::move(recovered)) >= 0;
}

void MainComponent::showLocalDiagnostics(diagnostics::ProviderScope scope)
{
    if (deferChromeForStartup || closeApplicationPending) return;
    if (!diagnosticsWindow)
    {
        class Window final : public juce::DocumentWindow
        {
        public:
            Window() : DocumentWindow("Local Diagnostics", ModernAppleLookAndFeel::windowBackground(), DocumentWindow::closeButton) {}
            std::function<void()> onClosed;
            void closeButtonPressed() override
            {
                setVisible(false); clearContentComponent();
                if (onClosed) onClosed();
            }
        };
        auto window = std::make_unique<Window>();
        const juce::Component::SafePointer<MainComponent> owner(this);
        const juce::Component::SafePointer<Window> expected(window.get());
        window->onClosed = [owner, expected]
        {
            // Defer deletion until the close handler has returned. A command
            // may reopen this window before delivery; do not delete that UI.
            juce::MessageManager::callAsync([owner, expected]
            {
                if (owner && expected && owner->diagnosticsWindow.get() == expected.getComponent()
                    && !expected->isVisible() && expected->getContentComponent() == nullptr)
                    owner->diagnosticsWindow.reset();
            });
        };
        diagnosticsWindow = std::move(window);
        diagnosticsWindow->setUsingNativeTitleBar(true);
        diagnosticsWindow->setLookAndFeel(&getLookAndFeel());
        diagnosticsWindow->setResizable(true, false);
        diagnosticsWindow->setResizeLimits(740, 550, 1400, 1100);
    }
    if (!diagnosticsWindow->getContentComponent())
    {
        const juce::Component::SafePointer<MainComponent> safe(this);
        std::vector<juce::File> exportExcludedDirectories;
        if (const auto root = recoveryRootDirectory(); root != juce::File())
        {
            const recovery::RecoveryStorageLayout layout(root);
            exportExcludedDirectories = { layout.journalDirectory(), layout.attemptDirectory() };
        }
        diagnosticsWindow->setContentOwned(new LocalDiagnosticsComponent([safe]
        {
            if (!safe) return std::vector<diagnostics::CapturedProvider>{};
            std::optional<recovery::RecoveryInspection::DiagnosticObservation> inspection;
            juce::Uuid restoreAttempt { nullptr };
            if (safe->recoveryCenter)
                if (const auto* center = dynamic_cast<RecoveryCenterComponent*>(safe->recoveryCenter->getContentComponent()))
                { inspection = center->diagnosticObservation(); restoreAttempt = center->selectedRestoreAttempt(); }
            return diagnostics::captureLiveDiagnosticProviders(safe->audioEngine, safe->document, safe->recoveryService.get(),
                safe->lastOptionalDiagnostic, safe->lastBnkOpenDiagnostic, safe->optionalFailureSession.get(),
                inspection ? &*inspection : nullptr, safe->recoveryDiagnostics.get(), safe->recoveryRestore.get(), restoreAttempt);
        }, std::move(scope), std::move(exportExcludedDirectories), [safe]
        { return safe && !safe->closeApplicationPending && safe->audioEngine.isIdleForDiagnosticProbe(); }, DiagnosticDeviceActions {
            [safe]() -> std::optional<AudioEngine::DeviceProbeConfiguration>
            { return safe && !safe->closeApplicationPending ? safe->audioEngine.getDeviceProbeConfiguration() : std::nullopt; },
            [safe](const AudioEngine::DeviceProbeConfiguration& selected, const diagnostics::DeviceProbeRequest& request)
                -> std::shared_ptr<diagnostics::GuiDeviceProbe>
            {
                if (!safe || safe->closeApplicationPending || (safe->deviceProbe
                    && safe->deviceProbe->phase() != diagnostics::GuiDeviceProbe::Phase::finished)) return {};
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                const ScopedOptionalLifecycleContext operationContext(safe->optionalLifecycleContext);
#endif
                safe->deviceProbe = diagnostics::GuiDeviceProbe::start(safe->audioEngine, selected, request,
                    juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("SingleEditorCLI.exe"));
                return safe->deviceProbe;
            } }), true);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (auto* content = dynamic_cast<LocalDiagnosticsComponent*>(diagnosticsWindow->getContentComponent()))
            content->setOptionalLifecycleContextProvider([safe]
            { return safe ? safe->optionalLifecycleContext : OptionalLifecycleContext{}; });
#endif
        diagnosticsWindow->centreAroundComponent(this, 780, 660);
    }
    else if (auto* content = dynamic_cast<LocalDiagnosticsComponent*>(diagnosticsWindow->getContentComponent()))
        content->requestScope(std::move(scope));
    diagnosticsWindow->setVisible(true); diagnosticsWindow->toFront(true);
}

bool MainComponent::publishRecoveredPeaks(const juce::Uuid& attempt, SingleTrackDocument::StatePtr originalCore,
                                         std::shared_ptr<WaveformModel::PeakCache> peaks)
{
    return workspaceSession.acceptRecoveredPeaks(attempt, std::move(originalCore), std::move(peaks));
}

void MainComponent::loadPlaylistIndex(int index)
{
    if (!cancelOrdinaryAudioLoad()) return;
    const auto* entry = playlistModel.getEntry(index);
    if (entry == nullptr)
        return;
    if (!entry->recovered && isBnkFile(entry->file)) { beginBnkOpen(entry->file); return; }
    cancelBnkOpen();
    if (!entry->recovered && entry->file.hasFileExtension("wem")
        && VgmstreamDecodeService::chooseWemOpenStrategy(entry->file).kind == VgmstreamDecodeService::WemOpenStrategy::Kind::decodedFallback)
    {
        if (finishReusedAudioLoad(entry->file, autoPlayButton.getToggleState())) return;
        beginPackedAudioLoad(entry->file);
        return;
    }
    if (!entry->recovered && WindowsMediaAudioService::isSupportedVideoExtension(entry->file.getFileExtension()))
    {
        if (finishReusedAudioLoad(entry->file, autoPlayButton.getToggleState())) return;
        beginVideoAudioLoad(entry->file);
        return;
    }

    if (!workspaceSession.shouldRetainPlaylistAudio())
        workspaceSession.clearRetainedPlaylistAudio();

    workspaceSession.retainCurrentAudioDocumentIfNeeded();
    if (entry->recovered)
    {
        const auto entryId = entry->id;
        // Save As can change the display/target path while preserved playback
        // and Undo still carry the original State locator. Fence actual rendered
        // State callbacks with that locator; keep milestone timing unchanged.
        const auto file = entry->activationFile();
        // Invalidate ordinary async results, including the same historical path,
        // before issuing a new active generation for explicit selection only.
        pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
        loadThreadPool.removeAllJobs(false, 0);
        cancelCacheWarm();
        pendingAutoPlayOnLoad = false;
        pendingAutoPlayFile = {};
        loadInProgress = false;
        pendingLoadLabel.clear();
        audioEngine.stop(); // Same-path binding must not keep an old transport playing.
        abandonActiveLoadTimingProbe("recovered-playlist-switch");
        beginDocumentGeneration(file);
        juce::String error;
        if (!workspaceSession.restoreRecoveredDocument(entryId, error))
        {
            setDocumentOperationStatus("Recovery activation failed: " + error);
            return;
        }
        releaseBnkRuntime();
        currentSelection.reset();
        waveformView.clearSelection();
        selectionGainSlider.setValue(0.0, juce::dontSendNotification);
        updateCenterViewVisibility();
        refreshUiState();
        markFileNameShownIfNeeded();
        markLoadPlayableIfNeeded(file);
        if (metadataSidebar) metadataSidebar->repaint();
        waveformView.refreshNow();
        notifyPrimaryContentVisibleIfNeeded();
        // Selection admits the document; playback remains a separate action.
        return;
    }

    if (document.getFileInfo().recoveryBacked) audioEngine.stop();
    pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
    pendingAutoPlayOnLoad = false;
    pendingAutoPlayFile = {};
    beginDocumentGeneration(workspaceSession.audioActivationFile(entry->file));
    if (finishReusedAudioLoad(entry->file, autoPlayButton.getToggleState()))
        return;

    beginAsyncLoad(entry->file, AudioLoadPolicy::reuseSession, true);
}

bool MainComponent::confirmDiscardOrSaveIfNeeded(const juce::String& actionDescription, std::function<void()> continuation,
                                               std::function<void()> cancelled)
{
    if (!document.hasUnsavedChanges())
    {
        if (continuation)
            continuation();
        return true;
    }

    const auto info = document.getFileInfo();
    const auto fileLabel = info.isValid() ? info.file.getFileName() : "the current file";
    const auto generation = activeDocumentGeneration.value;
    const auto prompted = document.captureWorkspaceSnapshot();
    juce::Component::SafePointer<MainComponent> safeThis(this);
    // Product builds do not run nested modal loops. Only the callback contains
    // the user's choice; the immediate return value is always zero.
    juce::AlertWindow::showYesNoCancelBox(juce::MessageBoxIconType::WarningIcon,
                                                              "Unsaved Changes",
                                                              "Save changes to " + fileLabel + " before " + actionDescription + "?",
                                                              "Save",
                                                              "Discard",
                                                              "Cancel",
                                                              this,
        juce::ModalCallbackFunction::create(
            [safeThis, generation, prompted, continuation = std::move(continuation), cancelled = std::move(cancelled)](int choice) mutable
            {
                if (safeThis == nullptr || safeThis->activeDocumentGeneration.value != generation)
                {
                    if (cancelled) cancelled();
                    return;
                }
                if (choice == 1)
                {
                    safeThis->saveDocument(!safeThis->document.canSaveOverSourceFile(),
                        [safeThis, generation, continuation = std::move(continuation), cancelled = std::move(cancelled)](bool didSave)
                        {
                            if (didSave && safeThis != nullptr
                                && safeThis->activeDocumentGeneration.value == generation
                                && !safeThis->document.hasUnsavedChanges() && continuation)
                                continuation();
                            else if (cancelled) cancelled();
                        });
                }
                else if (choice == 2 && continuation)
                {
                    const auto current = safeThis->document.captureWorkspaceSnapshot();
                    if (prompted && current && prompted->sameContents(*current)) continuation();
                    else
                    {
                        safeThis->setDocumentOperationStatus("The document changed while confirmation was open. Review its changes before continuing.");
                        if (cancelled) cancelled();
                    }
                }
                else if (cancelled) cancelled();
            }));

    return false;
}

void MainComponent::requestPlaylistActivation(int index)
{
    const auto* entry = playlistModel.getEntry(index);
    if (entry == nullptr)
        return;

    auto proceed = [this, entryId = entry->id]
    {
        const auto selected = playlistModel.indexOfEntry(entryId);
        if (selected >= 0 && !playlistModel.setCurrentIndex(selected))
            loadPlaylistIndex(selected);
    };

    const auto currentFile = workspaceSession.getCurrentLoadedFile();
    if (index == playlistModel.getCurrentIndex()
        && (entry->recovered ? workspaceSession.isActiveRecoveredDocument(entry->id) : entry->file == currentFile))
    {
        if (!entry->recovered && autoPlayButton.getToggleState() && document.hasAudio())
        {
            audioEngine.seekToSeconds(0.0);
            audioEngine.play();
            waveformView.refreshNow();
            refreshTransportReadout();
            refreshFooterReadout();
        }
        return;
    }

    const auto* active = playlistModel.getCurrentEntry();
    if (active && active->recovered && workspaceSession.isActiveRecoveredDocument(active->id))
        proceed(); // Dirty document/history remain owned by the playlist row.
    else
        confirmDiscardOrSaveIfNeeded("switching playlist items", std::move(proceed));
}

void MainComponent::requestPlaylistRemoval(int index)
{
    const auto* requested = playlistModel.getEntry(index);
    if (requested == nullptr)
        return;

    if (index == playlistModel.getCurrentIndex())
    {
        removeCurrentPlaylistEntry();
        return;
    }

    if (requested->recovered && requested->recovered->snapshot()
        && requested->recovered->snapshot()->hasUnsavedChanges())
    {
        auto reviewRemoval = [this, entryId = requested->id]
        {
            const auto selected = playlistModel.indexOfEntry(entryId);
            if (selected < 0) return;
            if (!playlistModel.setCurrentIndex(selected)) loadPlaylistIndex(selected);
            if (workspaceSession.isActiveRecoveredDocument(entryId)) removeCurrentPlaylistEntry();
        };
        const auto* active = playlistModel.getCurrentEntry();
        if (active && active->recovered && workspaceSession.isActiveRecoveredDocument(active->id))
            reviewRemoval(); // The previous recovered image keeps its full history.
        else
            confirmDiscardOrSaveIfNeeded("reviewing the recovered item for removal", std::move(reviewRemoval));
        return;
    }

    playlistModel.removeAt(index);
}

void MainComponent::removeCurrentPlaylistEntry()
{
    const auto* entry = playlistModel.getCurrentEntry();
    if (!entry)
        return;

    confirmDiscardOrSaveIfNeeded("removing the current playlist item",
                                 [this, entryId = entry->id]
                                 {
                                     playlistModel.removeAt(playlistModel.indexOfEntry(entryId));
                                 });
}

bool MainComponent::finishReusedAudioLoad(const juce::File& file, bool shouldAutoPlay)
{
    const auto reuseResult = workspaceSession.reuseAudioDocumentIfAvailable(file);
    if (reuseResult == WorkspaceSessionSupport::AudioDocumentReuseResult::none)
        return false;

    releaseBnkRuntime();
    loadInProgress = false;
    pendingLoadLabel.clear();
    refreshUiState();
    markFileNameShownIfNeeded();
    markLoadPlayableIfNeeded(document.getState()->info.file);
    if (metadataSidebar) metadataSidebar->repaint();
    waveformView.refreshNow();
    notifyPrimaryContentVisibleIfNeeded();
    if (shouldAutoPlay && document.hasAudio())
    {
        audioEngine.seekToSeconds(0.0);
        audioEngine.play();
        refreshTransportReadout();
        refreshFooterReadout();
    }
    return true;
}

void MainComponent::scheduleCacheWarm(std::vector<juce::File> candidateFiles, int delayMs, bool skipIfAudioLoaded)
{
    if (candidateFiles.empty()) return;
    // Recording a request must not inspect cache files or start a worker.
    pendingCacheWarm = PendingCacheWarm { std::move(candidateFiles), activeDocumentGeneration,
        juce::Time::getMillisecondCounterHiRes() + juce::jmax(0, delayMs), skipIfAudioLoaded };
}

void MainComponent::cancelCacheWarm()
{
    pendingCacheWarm.reset();
    waveformModel.cancelPendingPeakCachePersistence();
    if (cacheWarmThreadPool) cacheWarmThreadPool->removeAllJobs(false, 0);
}

void MainComponent::dispatchPendingCacheWarmIfReady()
{
    if (!closeApplicationPending && !loadInProgress
        && coreLifecycle.isCoreReady(activeDocumentGeneration))
        waveformModel.dispatchPendingPeakCachePersistence();
    if (!pendingCacheWarm) return;
    if (closeApplicationPending || pendingCacheWarm->generation != activeDocumentGeneration)
    {
        pendingCacheWarm.reset();
        return;
    }
    // An initially empty, visible workspace has no audio milestones. Once a
    // document generation exists, only that generation's real CoreReady admits work.
    const auto emptyWorkspaceReady = !activeDocumentGeneration && !startupHasInitialFile
        && isShowing() && !document.hasAudio();
    if (loadInProgress || (!emptyWorkspaceReady && !coreLifecycle.isCoreReady(activeDocumentGeneration))
        || juce::Time::getMillisecondCounterHiRes() < pendingCacheWarm->dueAtMs)
        return;

    auto request = std::move(*pendingCacheWarm);
    pendingCacheWarm.reset();
    if (request.skipIfAudioLoaded && document.hasAudio()) return;
    auto filesToWarm = workspaceSession.collectWarmableFiles(request.candidateFiles);
    if (filesToWarm.empty()) return;

    if (!cacheWarmThreadPool) cacheWarmThreadPool = std::make_unique<juce::ThreadPool>(1);
    cacheWarmThreadPool->removeAllJobs(false, 0);
    cacheWarmThreadPool->addJob(new AsyncCacheWarmJob(std::move(filesToWarm)), true);
}

void MainComponent::scheduleWaveformCacheWarm(const std::vector<juce::File>& files, int preferredIndex)
{
    std::vector<juce::File> candidateFiles;
    candidateFiles.reserve(files.size());

    for (int i = 0; i < static_cast<int>(files.size()); ++i)
    {
        if (preferredIndex >= 0 && i == preferredIndex)
            continue;

        candidateFiles.push_back(files[static_cast<std::size_t>(i)]);
    }

    scheduleCacheWarm(std::move(candidateFiles), 2400, false);
}

void MainComponent::scheduleAdjacentPlaylistCacheWarm(int currentIndex)
{
    std::vector<juce::File> candidateFiles;
    candidateFiles.reserve(2);

    for (const auto neighbor : { currentIndex - 1, currentIndex + 1 })
    {
        const auto* entry = playlistModel.getEntry(neighbor);
        if (entry != nullptr && !entry->recovered && entry->isValid())
            candidateFiles.push_back(entry->file);
    }

    scheduleCacheWarm(std::move(candidateFiles), 1800, false);
}

void MainComponent::scheduleRecentCacheWarm()
{
    if (document.hasAudio())
        return;

    const auto recent = workspaceSession.getValidRecentFiles(true);
    std::vector<juce::File> candidateFiles;
    candidateFiles.reserve(static_cast<std::size_t>(juce::jmin(6, static_cast<int>(recent.size()))));
    for (int index = 0; index < recent.size() && index < 6; ++index)
        candidateFiles.push_back(recent[static_cast<std::size_t>(index)]);

    scheduleCacheWarm(std::move(candidateFiles), 4600, true);
}

void MainComponent::restoreSessionSettings(bool includeWaveformViewPreferences)
{
    workspaceSession.restoreRecentFiles(maxRecentFiles);
    if (auto* settings = appProperties.getUserSettings())
    {
        playlistSidebarCollapsed = settings->getBoolValue(playlistSidebarCollapsedKey, false);
        metadataSidebarCollapsed = settings->getBoolValue(metadataSidebarCollapsedKey, false);
        playlistSidebarWidth = settings->getIntValue(playlistSidebarWidthKey, 218);
        metadataSidebarWidth = settings->getIntValue(metadataSidebarWidthKey, 232);
        if (includeWaveformViewPreferences)
        {
            waveformView.setViewMode(settings->getIntValue(waveformViewModeKey, 0) == 1
                                         ? WaveformViewComponent::ViewMode::spectral
                                         : WaveformViewComponent::ViewMode::waveform);
        }
        waveformView.setDisplayMode(settings->getIntValue(waveformDisplayModeKey, 0) == 1
                                        ? WaveformViewComponent::DisplayMode::mixed
                                        : WaveformViewComponent::DisplayMode::stacked);
        meterModeBox.setSelectedId(settings->getIntValue(meterModeKey, static_cast<int>(AudioEngine::MeterMode::truePeak)),
                                   juce::dontSendNotification);
        monitorSlider.setValue(settings->getDoubleValue(monitorLevelKey, 100.0), juce::dontSendNotification);
        audioEngine.setMonitorGainLinear(static_cast<float>(monitorSlider.getValue() / 100.0));
        returnToStartButton.setToggleState(settings->getBoolValue(returnToStartKey, true), juce::dontSendNotification);
        audioEngine.setReturnToPlayStartOnStop(returnToStartButton.getToggleState());
        loopButton.setToggleState(settings->getBoolValue(loopKey, false), juce::dontSendNotification);
        audioEngine.setLoopEnabled(loopButton.getToggleState());
        autoPlayButton.setToggleState(settings->getBoolValue(autoPlayKey, false), juce::dontSendNotification);
    }

    playlistSidebarWidth = juce::jlimit(180, 420, playlistSidebarWidth);
    metadataSidebarWidth = juce::jlimit(190, 460, metadataSidebarWidth);
    recentButton.setEnabled(workspaceSession.hasRecentFiles());
    updateSidebarToggleButtons();
}

void MainComponent::restoreStartupCriticalSettings()
{
    deferredStartupUiSettingsPending = true;
    waveformView.setViewMode(WaveformViewComponent::ViewMode::waveform);
    waveformView.setDisplayMode(WaveformViewComponent::DisplayMode::stacked);
    recentButton.setEnabled(false);

    if (auto* settings = appProperties.getUserSettings())
    {
        monitorSlider.setValue(settings->getDoubleValue(monitorLevelKey, 100.0), juce::dontSendNotification);
        audioEngine.setMonitorGainLinear(static_cast<float>(monitorSlider.getValue() / 100.0));
        returnToStartButton.setToggleState(settings->getBoolValue(returnToStartKey, true), juce::dontSendNotification);
        audioEngine.setReturnToPlayStartOnStop(returnToStartButton.getToggleState());
        loopButton.setToggleState(settings->getBoolValue(loopKey, false), juce::dontSendNotification);
        audioEngine.setLoopEnabled(loopButton.getToggleState());
        autoPlayButton.setToggleState(settings->getBoolValue(autoPlayKey, false), juce::dontSendNotification);
    }

    updateSidebarToggleButtons();
}

void MainComponent::applyDeferredStartupUiSettingsIfNeeded()
{
    if (!deferredStartupUiSettingsPending)
        return;

    deferredStartupUiSettingsPending = false;
    restoreSessionSettings(false);
}

void MainComponent::restorePlaylistSession()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        if (!settings->getBoolValue(restorePlaylistSessionKey, false))
            return;

        const auto files = WorkspaceSessionSupport::deserialisePlaylistFiles(settings->getValue(playlistSessionFilesKey));
        if (files.empty())
            return;

        suppressPlaylistLoad = true;
        playlistModel.restoreFiles(files, settings->getIntValue(playlistSessionIndexKey, 0));
        suppressPlaylistLoad = false;

        scheduleWaveformCacheWarm(files, -1);
    }
}

void MainComponent::persistSessionSettings()
{
    requestSessionSettingsPersist();
}

bool MainComponent::canPersistDeferredSettings() const noexcept
{
    // A debounce deadline is not evidence that opening has reached CoreReady.
    // Empty/failed-open idle surfaces have no audio milestones to wait for.
    return coreLifecycle.isCoreReady(activeDocumentGeneration)
        || (!loadInProgress && !document.hasAudio());
}

void MainComponent::requestSessionSettingsPersist(int delayMs)
{
    if (delayMs < 0)
    {
        const auto startupPathStillHot = deferChromeForStartup
                                      || loadInProgress
                                      || (activeLoadTimingProbe.has_value()
                                          && !activeLoadTimingProbe->fullWaveformElapsedMs.has_value());
        delayMs = startupPathStillHot ? startupSessionSettingsPersistDelayMs
                                      : normalSessionSettingsPersistDelayMs;
    }

    sessionSettingsPersistPending = true;
    sessionSettingsPersistDueAtMs = juce::Time::getMillisecondCounterHiRes() + juce::jmax(120, delayMs);
}

void MainComponent::flushPendingSessionSettingsPersist()
{
    if (!sessionSettingsPersistPending)
        return;

    sessionSettingsPersistPending = false;
    sessionSettingsPersistDueAtMs = 0.0;

    if (auto* settings = appProperties.getUserSettings())
    {
        workspaceSession.persistRecentFiles();
        settings->setValue(playlistSidebarCollapsedKey, playlistSidebarCollapsed);
        settings->setValue(metadataSidebarCollapsedKey, metadataSidebarCollapsed);
        settings->setValue(playlistSidebarWidthKey, playlistSidebarWidth);
        settings->setValue(metadataSidebarWidthKey, metadataSidebarWidth);
        settings->setValue(waveformViewModeKey, waveformView.getViewMode() == WaveformViewComponent::ViewMode::spectral ? 1 : 0);
        settings->setValue(waveformDisplayModeKey, waveformView.getDisplayMode() == WaveformViewComponent::DisplayMode::mixed ? 1 : 0);
        settings->setValue(meterModeKey, meterModeBox.getSelectedId());
        settings->setValue(monitorLevelKey, monitorSlider.getValue());
        settings->setValue(returnToStartKey, returnToStartButton.getToggleState());
        settings->setValue(loopKey, loopButton.getToggleState());
        settings->setValue(autoPlayKey, autoPlayButton.getToggleState());
        if (workspaceModuleHost != nullptr && workspacePresetWritable
            && activeWorkspaceCategory == WorkspaceContentCategory::general)
            settings->setValue(workspaceModuleLayoutV2Key, workspaceModuleHost->serialiseUserState());
        if (workspacePresetCatalog != nullptr && workspacePresetCatalogLoaded
            && workspacePresetCatalogWritable)
            settings->setValue(workspacePresetCatalogKey,
                               juce::String::fromUTF8(workspacePresetCatalog->serialise().c_str()));
        const auto shouldRestorePlaylist = settings->getBoolValue(restorePlaylistSessionKey, false);
        if (shouldRestorePlaylist)
        {
            settings->setValue(playlistSessionFilesKey, WorkspaceSessionSupport::serialisePlaylistFiles(playlistModel));
            settings->setValue(playlistSessionIndexKey, WorkspaceSessionSupport::serialisedPlaylistIndex(playlistModel));
        }
        else
        {
            settings->removeValue(playlistSessionFilesKey);
            settings->removeValue(playlistSessionIndexKey);
        }
        settings->saveIfNeeded();
    }
}

juce::File MainComponent::getDefaultBrowseDirectory() const
{
    return workspaceSession.getDefaultBrowseDirectory();
}

juce::File MainComponent::getCurrentLoadedFile() const
{
    return workspaceSession.getCurrentLoadedFile();
}

BnkMediaResolveMode MainComponent::getBnkMediaResolveMode() const
{
    if (auto* settings = appProperties.getUserSettings())
    {
        const auto mode = settings->getIntValue(PreferencesComponent::bnkPlaybackModeKey, 1);
        return mode == 2 ? BnkMediaResolveMode::sourceMedia
                         : BnkMediaResolveMode::decodePackedMedia;
    }

    return BnkMediaResolveMode::decodePackedMedia;
}

void MainComponent::ensureBnkRuntime()
{
    // A restored selection may outlive its original locator. It still needs
    // one release owner; source-current checks continue to guard preview/UI.
    if (bnkDocument.getBankInfo().file == juce::File()) return;
    if (!bnkRuntime) bnkRuntime = BnkRuntimeSession::create(bnkDocument, activeDocumentGeneration.value);
    else bnkRuntime->bind(activeDocumentGeneration.value);
}

void MainComponent::ensureBnkControls()
{
    ensureBnkRuntime();
    // A loaded bank is the module trigger. Ordinary chrome promotion must not create these controls.
    if (bnkControls || !bnkDocument.hasBank()) return;
    bnkControls = std::make_unique<BnkControls>();
    addChildComponent(bnkControls->surface);
    bnkControls->surface.getProperties().set("fillColour", static_cast<int64_t>(ModernAppleLookAndFeel::panelBackground().getARGB()));
    bnkControls->surface.getProperties().set("fillAlpha", 0.98);
    bnkControls->surface.getProperties().set("shadowAlpha", 0.018);
    bnkControls->surface.addAndMakeVisible(bnkControls->switchLabel);
    bnkControls->surface.addAndMakeVisible(bnkControls->stateLabel);
    bnkControls->surface.addAndMakeVisible(bnkControls->eventsTab);
    bnkControls->surface.addAndMakeVisible(bnkControls->audioTab);
    bnkControls->surface.addAndMakeVisible(bnkControls->audioFilter);
    bnkControls->surface.addAndMakeVisible(bnkControls->attachProject);
    bnkControls->surface.addAndMakeVisible(bnkControls->switchBox);
    bnkControls->surface.addAndMakeVisible(bnkControls->stateBox);
    configureLabel(bnkControls->switchLabel, 12.0f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText());
    configureLabel(bnkControls->stateLabel, 12.0f, juce::Justification::centredLeft, ModernAppleLookAndFeel::secondaryText());
    bnkControls->switchLabel.setText("Switch", juce::dontSendNotification);
    bnkControls->stateLabel.setText("State", juce::dontSendNotification);
    configureActionButton(bnkControls->eventsTab, "Events");
    configureActionButton(bnkControls->audioTab, "Audio");
    configureActionButton(bnkControls->audioFilter, "Linked Only");
    configureActionButton(bnkControls->attachProject, "Attach Project");
    bnkControls->eventsTab.setEnabled(true);
    bnkControls->audioTab.setEnabled(true);
    bnkControls->audioFilter.setEnabled(true);
    bnkControls->attachProject.setEnabled(true);
    bnkControls->eventsTab.setClickingTogglesState(true);
    bnkControls->audioTab.setClickingTogglesState(true);
    bnkControls->audioFilter.setClickingTogglesState(true);
    bnkControls->audioFilter.setToggleState(bnkAudioLinkedOnly, juce::dontSendNotification);
    bnkControls->audioFilter.setTooltip("Show only raw media linked to the selected Event");
    bnkControls->eventsTab.onClick = [this]
    {
        bnkCenterMode = BnkCenterMode::events;
        updateCenterViewVisibility();
        resized();
    };
    bnkControls->audioTab.onClick = [this]
    {
        bnkCenterMode = BnkCenterMode::audio;
        updateCenterViewVisibility();
        syncBnkAudioSelectionForCurrentContext();
        resized();
    };
    bnkControls->audioFilter.onClick = [this]
    {
        bnkAudioLinkedOnly = bnkControls->audioFilter.getToggleState();
        if (bnkAudioListView != nullptr)
        {
            bnkAudioListView->setLinkedOnly(bnkAudioLinkedOnly);
            bnkAudioListView->setSwitchTokenFilter(buildBnkPlaybackContext().switchValueToken);
        }
    };
    bnkControls->attachProject.onClick = [this] { attachBnkProjectRoot(); };
    bnkControls->switchBox.onChange = [this]
    {
        if (suppressBnkControlCallbacks)
            return;

        const auto index = bnkControls->switchBox.getSelectedItemIndex();
        auto context = buildBnkPlaybackContext();
        context.switchValueToken = juce::isPositiveAndBelow(index, static_cast<int>(bnkSwitchTokens.size()))
                                     ? bnkSwitchTokens[static_cast<std::size_t>(index)]
                                     : juce::String();
        if (bnkRuntime) bnkRuntime->setContext(std::move(context), bnkDocument.getSelectedEventId());
        prepareSelectedBnkEvent(shouldAutoPlayBnkContextChange());
    };
    bnkControls->stateBox.onChange = [this]
    {
        if (suppressBnkControlCallbacks)
            return;

        const auto index = bnkControls->stateBox.getSelectedItemIndex();
        auto context = buildBnkPlaybackContext();
        context.stateValueToken = juce::isPositiveAndBelow(index, static_cast<int>(bnkStateTokens.size()))
                                    ? bnkStateTokens[static_cast<std::size_t>(index)]
                                    : juce::String();
        if (bnkRuntime) bnkRuntime->setContext(std::move(context), bnkDocument.getSelectedEventId());
        prepareSelectedBnkEvent(shouldAutoPlayBnkContextChange());
    };
}

void MainComponent::ensureBnkUiComponents()
{
    ensureBnkControls();
    if (!bnkControls) return;

    if (bnkEventListView == nullptr)
    {
        bnkEventListView = std::make_unique<BnkEventListViewComponent>(bnkDocument);
        bnkEventListView->onEventActivated = [this](uint32_t)
        {
            prepareSelectedBnkEvent(autoPlayButton.getToggleState());
        };
        bnkEventListView->onEventPlayRequested = [this](uint32_t)
        {
            prepareSelectedBnkEvent(true);
        };
        addChildComponent(*bnkEventListView);
    }

    if (bnkAudioListView == nullptr)
    {
        bnkAudioListView = std::make_unique<BnkAudioListViewComponent>(bnkDocument);
        bnkAudioListView->setLinkedOnly(bnkAudioLinkedOnly);
        bnkAudioListView->setSwitchTokenFilter(buildBnkPlaybackContext().switchValueToken);
        bnkAudioListView->onMediaSelected = [this](uint32_t)
        {
            if (metadataSidebar) metadataSidebar->repaint();
        };
        bnkAudioListView->onMediaActivated = [this](uint32_t mediaId)
        {
            prepareSelectedBnkMedia(mediaId, autoPlayButton.getToggleState());
        };
        bnkAudioListView->onMediaPlayRequested = [this](uint32_t mediaId)
        {
            prepareSelectedBnkMedia(mediaId, true);
        };
        addChildComponent(*bnkAudioListView);
    }

    // Bank controls and lists share the actual core slot. Reapplying the
    // complete waveform bounds to a frontmost list used to cover the controls.
    auto bankBounds = waveformView.getBounds();
    const bool showSwitch = bnkControls->switchBox.isVisible(), showState = bnkControls->stateBox.isVisible();
    const auto innerWidth = juce::jmax(0, bankBounds.getWidth() - 32);
    const bool wrapHeader = innerWidth < 510;
    const bool wrapContext = showSwitch && showState && innerWidth < 510;
    const int contextRows = (showSwitch || showState) ? (wrapContext ? 2 : 1) : 0;
    const int rows = (wrapHeader ? 2 : 1) + contextRows;
    const int controlsHeight = 20 + rows * 30 + (rows - 1) * 8;
    bnkControls->surface.setBounds(bankBounds.removeFromTop(controlsHeight));
    bankBounds.removeFromTop(8);
    auto controls = bnkControls->surface.getLocalBounds().reduced(16, 10);
    auto header = controls.removeFromTop(30);
    const auto tabWidth = juce::jmin(102, juce::jmax(0, (header.getWidth() - 8) / 2));
    bnkControls->eventsTab.setBounds(header.removeFromLeft(tabWidth)); header.removeFromLeft(8);
    bnkControls->audioTab.setBounds(header.removeFromLeft(tabWidth)); header.removeFromLeft(8);
    if (wrapHeader) { controls.removeFromTop(8); header = controls.removeFromTop(30); }
    const auto attachWidth = juce::jmin(142, header.getWidth() / 2);
    bnkControls->attachProject.setBounds(header.removeFromRight(attachWidth)); header.removeFromRight(8);
    bnkControls->audioFilter.setBounds(header.removeFromLeft(juce::jmin(116, header.getWidth())));
    bnkControls->switchLabel.setBounds({}); bnkControls->switchBox.setBounds({});
    bnkControls->stateLabel.setBounds({}); bnkControls->stateBox.setBounds({});
    auto placeCombo = [](juce::Rectangle<int> section, juce::Label& label, juce::ComboBox& box, int labelWidth)
    {
        label.setBounds(section.removeFromLeft(juce::jmin(labelWidth, section.getWidth() / 3)));
        section.removeFromLeft(8); box.setBounds(section);
    };
    if (contextRows > 0)
    {
        controls.removeFromTop(8); auto row = controls.removeFromTop(30);
        if (showSwitch)
        {
            const auto width = showState && !wrapContext ? juce::jmin(282, (row.getWidth() - 14) / 2) : juce::jmin(368, row.getWidth());
            placeCombo(row.removeFromLeft(width), bnkControls->switchLabel, bnkControls->switchBox, 62); row.removeFromLeft(14);
        }
        if (showState)
        {
            if (wrapContext) { controls.removeFromTop(8); row = controls.removeFromTop(30); }
            placeCombo(row.removeFromLeft(juce::jmin(312, row.getWidth())), bnkControls->stateLabel, bnkControls->stateBox, 52);
        }
    }
    if (bnkEventListView != nullptr) bnkEventListView->setBounds(bankBounds);
    if (bnkAudioListView != nullptr) bnkAudioListView->setBounds(bankBounds);
    bnkControls->surface.toFront(false);
}

void MainComponent::releaseBnkRuntime()
{
    cancelBnkOpen();
    cancelBnkPreview();
    bnkEventListView.reset();
    bnkAudioListView.reset();
    bnkControls.reset();
    if (bnkRuntime) bnkRuntime->release();
    bnkRuntime.reset();
}

void MainComponent::beginPackedAudioLoad(const juce::File& file, media::OptionalMediaFailureState::Intent intent,
                                        std::optional<media::OptionalMediaFailureState::Key> reviewed)
{
    if (!cancelOrdinaryAudioLoad()) return;
    cancelBnkOpen();
    cancelBnkPreview();
    cancelPackedAudioLoad();
    if (!file.existsAsFile()) { setDocumentOperationStatus("WEM source is unavailable; current audio was kept."); return; }
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    pendingVideoOperation.reset();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    if (!optionalFailureSession) optionalFailureSession = std::make_shared<media::OptionalMediaFailureState>();
    PackedAudioLoad load;
    load.file = file;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext operationContext(optionalLifecycleContext);
#endif
    load.operation = std::make_shared<media::OptionalMediaOperation>(optionalFailureSession, intent, std::move(reviewed),
        media::OptionalMediaFailureState::Module::packedAudio);
    lastOptionalDiagnostic = load.operation->diagnosticHandle();
    static_cast<AudioReturnImage&>(load) = captureAudioReturnImage();
    startLoadTimingProbe(file);
    cancelCacheWarm(); loadThreadPool.removeAllJobs(false, 0);
    load.token = pendingLoadToken.fetch_add(1, std::memory_order_relaxed) + 1;
    pendingLoadLabel = file.getFileName(); loadInProgress = true;
    pendingAutoPlayOnLoad = intent == media::OptionalMediaFailureState::Intent::initial && autoPlayButton.getToggleState();
    pendingAutoPlayFile = pendingAutoPlayOnLoad ? file : juce::File();
    packedAudioLoad = std::move(load);
    auto operation = packedAudioLoad->operation;
    auto* progress = new juce::AlertWindow("Preparing WEM audio", "Waiting for the first playable portion. Your previous audio and history are retained.",
        juce::MessageBoxIconType::NoIcon, this);
    progress->addButton("Cancel WEM audio", 1, juce::KeyPress(juce::KeyPress::escapeKey));
    packedProgressWindow = progress;
    progress->enterModalState(true, juce::ModalCallbackFunction::create([operation](int choice)
        { if (choice == 1) operation->requestCancel(); }), true);
    refreshUiState();
    loadThreadPool.addJob(new AsyncFileLoadJob(juce::Component::SafePointer<MainComponent>(this), file, packedAudioLoad->token, operation), true);
}

MainComponent::AudioReturnImage MainComponent::captureAudioReturnImage() const
{
    AudioReturnImage load;
    load.previousState = document.getState();
    load.previousAudio = document.captureWorkspaceSnapshot();
    load.previousBank = bnkDocument.captureSelection();
    load.previousFile = load.previousBank ? bnkDocument.getBankInfo().file : document.getFileInfo().file;
    load.previousSelection = currentSelection;
    const auto peaks = waveformModel.getPeakCache();
    if (peaks && peaks->state == load.previousState) load.previousPeaks = std::const_pointer_cast<WaveformModel::PeakCache>(peaks);
    const auto spectral = waveformModel.getSpectrogramCache();
    if (spectral && spectral->state == load.previousState) load.previousSpectrogram = std::const_pointer_cast<WaveformModel::SpectrogramCache>(spectral);
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        const auto* entry = playlistModel.getEntry(i);
        if (!entry) continue;
        const bool recovered = entry->recovered && workspaceSession.isActiveRecoveredDocument(entry->id);
        // A recovered copy may share its historical locator with an ordinary
        // row. Its active identity must win even when that row appears first.
        if (recovered)
        { load.previousEntry = entry->id; load.previousRecovered = true; break; }
        if (!load.previousEntry && !entry->recovered && entry->file == load.previousFile)
            load.previousEntry = entry->id;
    }
    return load;
}

void MainComponent::selectLoadedAudioEntry(const juce::File& file)
{
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        const auto* entry = playlistModel.getEntry(i);
        if (!entry || entry->recovered || entry->file != file) continue;
        const juce::ScopedValueSetter<bool> suppress(suppressPlaylistLoad, true);
        playlistModel.setCurrentIndex(i);
        return;
    }
}

bool MainComponent::restoreAudioReturn(const AudioReturnImage& load, bool restorePlaylist)
{
    audioEngine.stop();
    abandonActiveLoadTimingProbe("return-to-retained-workspace");
    beginDocumentGeneration(load.previousFile);
    juce::String error;
    bool restored = false;
    if (load.previousRecovered && load.previousEntry)
        restored = workspaceSession.restoreRecoveredDocument(*load.previousEntry, error);
    else if (load.previousAudio)
    {
        waveformModel.prepareIncomingCaches(load.previousPeaks, load.previousSpectrogram);
        restored = document.adoptWorkspaceSnapshot(*load.previousAudio, error);
        workspaceSession.releaseRecoveredActivation();
    }
    else { document.clear(); workspaceSession.releaseRecoveredActivation(); restored = true; }
    if (!restored) { setDocumentOperationStatus("The retained workspace could not be restored: " + error); return false; }
    if (load.previousBank)
    {
        if (!bnkDocument.restoreSelection(*load.previousBank))
        { setDocumentOperationStatus("Audio was restored, but the retained bank selection could not be restored."); return false; }
        ensureBnkUiComponents();
    }
    if (restorePlaylist && load.previousEntry)
    {
        const juce::ScopedValueSetter<bool> suppress(suppressPlaylistLoad, true);
        playlistModel.setCurrentIndex(playlistModel.indexOfEntry(*load.previousEntry));
    }
    waveformView.restoreSelection(load.previousSelection);
    updateCenterViewVisibility(); refreshUiState(); waveformView.refreshNow(); repaintMetadataSidebarIfVisible();
    return true;
}

bool MainComponent::ordinaryAudioLoadOwnsDocument() const
{
    if (!ordinaryAudioLoad || ordinaryAudioLoad->token != pendingLoadToken.load(std::memory_order_relaxed)
        || document.getState() != ordinaryAudioLoad->expectedState) return false;
    const auto& expected = ordinaryAudioLoad->expectedIdentity;
    const auto current = document.getMediaIdentity();
    if (current.document.lineage != expected.document.lineage || current.document.runtimeInstance != expected.document.runtimeInstance
        || current.document.generation != expected.document.generation || current.savedGeneration != expected.savedGeneration
        || current.source.binding != expected.source.binding) return false;
    if (ordinaryAudioLoad->expectedWorkspace)
    {
        const auto workspace = document.captureWorkspaceSnapshot();
        return workspace && ordinaryAudioLoad->expectedWorkspace->sameContents(*workspace);
    }
    // Empty and metadata-only preview states cannot authorise overwriting a
    // history created after them, including edit/Undo back to the same State.
    return !document.canUndo() && !document.canRedo();
}

void MainComponent::observeOrdinaryAudioLoadState()
{
    if (!ordinaryAudioLoad) return;
    ordinaryAudioLoad->expectedState = document.getState();
    ordinaryAudioLoad->expectedIdentity = document.getMediaIdentity();
    ordinaryAudioLoad->expectedWorkspace = document.captureWorkspaceSnapshot();
}

bool MainComponent::cancelOrdinaryAudioLoad(bool preservePlaylist)
{
    if (!ordinaryAudioLoad) return true;
    const bool current = ordinaryAudioLoad->token == pendingLoadToken.load(std::memory_order_relaxed);
    const bool ownsDocument = ordinaryAudioLoadOwnsDocument();
    auto pending = std::move(*ordinaryAudioLoad); ordinaryAudioLoad.reset();
    if (!current) return true;
    const auto token = pendingLoadToken.fetch_add(1, std::memory_order_relaxed) + 1;
    loadThreadPool.removeAllJobs(false, 0); cancelCacheWarm();
    loadInProgress = false; pendingLoadLabel.clear(); pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
    if (!ownsDocument) return true;
    const auto* entry = playlistModel.getCurrentEntry();
    const bool ownsRow = (entry ? std::optional<juce::Uuid>(entry->id) : std::nullopt) == pending.expectedEntry;
    if (restoreAudioReturn(pending.returnImage, !preservePlaylist && ownsRow)) return true;
    // A failed return must keep the only complete image reachable for a later
    // attempt. It cannot authorise a new file to replace that work.
    pending.token = token; ordinaryAudioLoad = std::move(pending);
    observeOrdinaryAudioLoadState();
    return false;
}

void MainComponent::cancelPackedAudioLoad()
{
    if (!packedAudioLoad) return;
    auto load = std::move(*packedAudioLoad); packedAudioLoad.reset();
    load.operation->requestCancel();
    if (packedProgressWindow) packedProgressWindow->exitModalState(0);
    // UI decisions compare exact images, never ordered version numbers. A new
    // edit/activation is retained rather than overwritten by an old callback.
    if (load.awaitingTerminal && document.getState() == load.admittedState)
        restoreAudioReturn(load);
}

void MainComponent::pollPackedAudioLoad()
{
    if (!packedAudioLoad || !packedAudioLoad->awaitingTerminal) return;
    auto& load = *packedAudioLoad;
    const auto report = load.operation->report();
    if (!report.terminal)
    {
        if (packedProgressWindow && report.totalFrames > 0)
            packedProgressWindow->setMessage("The first portion is playable. Decoding the remaining audio ("
                + juce::String(static_cast<int>(100.0 * report.readyFrames / report.totalFrames)) + "%). Your previous workspace remains retained.");
        return;
    }
    if (packedProgressWindow) packedProgressWindow->exitModalState(0);
    auto completed = std::move(load); packedAudioLoad.reset();
    const bool cancelled = report.cancelled || completed.operation->cancelFlag().load(std::memory_order_acquire);
    if (report.succeeded && !cancelled)
    {
        // CoreReady retains its original real-prefix timing. Only independent
        // protection waits for a sealed backing, then uses the existing history.
        attachRecoveryAfterCore();
        setDocumentOperationStatus("WEM audio decode complete.");
        return;
    }
    const bool unchanged = document.getState() == completed.admittedState;
    const bool restored = unchanged && restoreAudioReturn(completed);
    completed.awaitingTerminal = false;
    if (restored) completed.admittedState.reset();
    packedAudioLoad = std::move(completed);
    if (restored) setDocumentOperationStatus("WEM audio ended; the previous workspace and complete history were restored.");
    else if (!unchanged) setDocumentOperationStatus("WEM audio ended; changes made during loading were kept with their history.");
    if (!cancelled) showPackedAudioFailure(packedAudioLoad->file, packedAudioLoad->operation);
}

void MainComponent::showPackedAudioFailure(const juce::File& file, const media::OptionalMediaOperation::Ptr& operation)
{
    using S = media::OptionalMediaFailureState;
    const auto report = operation->report(); const auto view = operation->policyView();
    juce::String reason = "This WEM request was declined by the session policy.";
    if (report.failure)
    {
        switch (report.failure->reason)
        {
            case S::Reason::deadline: reason = "The decoder exceeded its time budget."; break;
            case S::Reason::outputQuota: reason = "Decoded output exceeded its size budget."; break;
            case S::Reason::memoryQuota: reason = "The decoder exceeded its memory budget."; break;
            case S::Reason::missingDependency: reason = "vgmstream is unavailable. Select a supported local runtime before retrying."; break;
            case S::Reason::decoderExit: reason = "The WEM decoder exited unsuccessfully (code " + juce::String(report.failure->exitCode) + ")."; break;
            case S::Reason::cacheInvalid: reason = "The decoded audio or cache could not be validated."; break;
            case S::Reason::containment: reason = "Decoder containment or cleanup could not be confirmed."; break;
            default: reason = "The source, decoder or decoded cache could not be accessed."; break;
        }
    }
    if (view.circuitOpen) reason += "\nTwo matching failures have disabled this policy for this application session.";
    if (report.denied == S::Denied::invalidKey) reason += "\nThe source or decoder changed; open the file as a fresh request.";
    reason += "\nYour retained audio and complete Undo/Redo history were preserved. No automatic retry will run.";
    if (view.expansionAvailable) reason += "\nExpanded Budget allows up to 2 minutes, 4 GiB decoder memory and 4 GiB output.";
    auto* alert = new juce::AlertWindow("WEM audio unavailable", reason, juce::MessageBoxIconType::WarningIcon, this);
    if (view.retryAvailable && report.key) alert->addButton("Retry once", 1);
    if (view.expansionAvailable && report.key) alert->addButton("Retry with Expanded Budget", 2);
    alert->addButton("Export diagnostics...", 3);
    alert->addButton("Keep current audio", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainComponent>(this), file, operation, key = report.key](int choice)
    {
        if (!safe || !safe->packedAudioLoad || safe->packedAudioLoad->operation != operation) return;
        if (choice == 3) { safe->exportVideoAudioDiagnostics(operation); return; }
        if ((choice != 1 && choice != 2) || !key) return;
        auto retry = [safe, file, key, choice] { if (safe) safe->beginPackedAudioLoad(file,
            choice == 1 ? S::Intent::retryOnce : S::Intent::expandBudget, key); };
        if (safe->packedAudioLoad->admittedState && safe->document.hasUnsavedChanges())
            safe->confirmDiscardOrSaveIfNeeded("retrying WEM audio", std::move(retry));
        else retry();
    }), true);
}

void MainComponent::beginVideoAudioLoad(const juce::File& file, media::OptionalMediaFailureState::Intent intent,
                                       std::optional<media::OptionalMediaFailureState::Key> reviewed)
{
    if (!cancelOrdinaryAudioLoad()) return;
    cancelBnkOpen();
    cancelBnkPreview();
    cancelPackedAudioLoad();
    if (!file.existsAsFile())
    { setDocumentOperationStatus("Video source is unavailable; current audio was kept."); return; }
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    if (!optionalFailureSession) optionalFailureSession = std::make_shared<media::OptionalMediaFailureState>();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext operationContext(optionalLifecycleContext);
#endif
    auto operation = std::make_shared<media::OptionalMediaOperation>(optionalFailureSession, intent, std::move(reviewed));
    lastOptionalDiagnostic = operation->diagnosticHandle();
    pendingVideoOperation = operation;
    videoLoadPreviousState = document.getState();
    videoLoadPreviousEntry.reset();
    for (int i = 0; i < playlistModel.getNumEntries(); ++i)
    {
        const auto* entry = playlistModel.getEntry(i);
        if (entry && ((entry->recovered && workspaceSession.isActiveRecoveredDocument(entry->id))
            || (!entry->recovered && !videoLoadPreviousState->info.recoveryBacked
                && entry->file == videoLoadPreviousState->info.file)))
        { videoLoadPreviousEntry = entry->id; break; }
    }
    // Timestamp the user's request before worker admission/decoding. Current
    // content/generation stays active until a complete candidate is accepted.
    startLoadTimingProbe(file);
    cancelCacheWarm();
    loadThreadPool.removeAllJobs(false, 0);
    const auto token = pendingLoadToken.fetch_add(1, std::memory_order_relaxed) + 1;
    pendingLoadLabel = file.getFileName();
    loadInProgress = true;
    // Preserve the user's ordinary Open/Auto Play preference for a fresh open;
    // an explicit failure retry remains stopped after candidate admission.
    pendingAutoPlayOnLoad = intent == media::OptionalMediaFailureState::Intent::initial && autoPlayButton.getToggleState();
    pendingAutoPlayFile = pendingAutoPlayOnLoad ? file : juce::File();
    refreshUiState();
    auto* progress = new juce::AlertWindow("Preparing video audio", "The current audio and its history are kept until this video is ready.",
        juce::MessageBoxIconType::NoIcon, this);
    progress->addButton("Cancel video audio", 1, juce::KeyPress(juce::KeyPress::escapeKey));
    videoProgressWindow = progress;
    progress->enterModalState(true, juce::ModalCallbackFunction::create([operation](int choice)
        { if (choice == 1) operation->requestCancel(); }), true);
    loadThreadPool.addJob(new AsyncFileLoadJob(juce::Component::SafePointer<MainComponent>(this), file, token, operation), true);
}

void MainComponent::showVideoAudioFailure(const juce::File& file, const media::OptionalMediaOperation::Ptr& operation)
{
    using S = media::OptionalMediaFailureState;
    const auto report = operation->report();
    const auto view = operation->policyView();
    juce::String reason = "This request was declined by the session policy.";
    if (report.failure)
    {
        switch (report.failure->reason)
        {
            case S::Reason::deadline: reason = "The decoder exceeded its time budget."; break;
            case S::Reason::outputQuota: reason = "The decoded output exceeded its size budget."; break;
            case S::Reason::memoryQuota: reason = "The decoder exceeded its memory budget."; break;
            case S::Reason::missingDependency: reason = "FFmpeg is unavailable. Install or select a supported local runtime before retrying."; break;
            case S::Reason::decoderExit: reason = "The video decoder exited unsuccessfully (code " + juce::String(report.failure->exitCode) + ")."; break;
            case S::Reason::cacheInvalid: reason = "The decoded audio or cache could not be validated."; break;
            case S::Reason::containment: reason = "Decoder containment or process cleanup could not be confirmed."; break;
            default: reason = "The video source or decoder could not be accessed."; break;
        }
    }
    if (view.circuitOpen) reason += "\nTwo matching failures have disabled this policy for this application session.";
    else if (report.denied == S::Denied::invalidKey) reason += "\nThe source or decoder changed; open the file again as a fresh request.";
    reason += "\nCurrent audio and its complete Undo/Redo history were kept. No automatic retry will run.";
    if (view.expansionAvailable)
        reason += "\nExpanded Budget permits up to 10 minutes, 4 GiB decoder memory and 4 GiB output; it may use more time and disk space.";
    auto* alert = new juce::AlertWindow("Video audio unavailable", reason, juce::MessageBoxIconType::WarningIcon, this);
    if (view.retryAvailable && report.key) alert->addButton("Retry once", 1);
    if (view.expansionAvailable && report.key) alert->addButton("Retry with Expanded Budget", 2);
    alert->addButton("Export diagnostics...", 3);
    alert->addButton("Keep current audio", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<MainComponent>(this), file, operation, key = report.key](int choice)
        {
            if (!safe || safe->pendingVideoOperation != operation) return;
            if (choice == 1 && key) safe->beginVideoAudioLoad(file, S::Intent::retryOnce, key);
            else if (choice == 2 && key) safe->beginVideoAudioLoad(file, S::Intent::expandBudget, key);
            else if (choice == 3) safe->exportVideoAudioDiagnostics(operation);
        }), true);
}

void MainComponent::exportVideoAudioDiagnostics(const media::OptionalMediaOperation::Ptr& operation)
{
    if (!operation) return;
    lastOptionalDiagnostic = operation->diagnosticHandle();
    showLocalDiagnostics(std::set { diagnostics::ProviderKind::codec, diagnostics::ProviderKind::cache });
}

void MainComponent::beginAsyncLoad(const juce::File& file,
                                   AudioLoadPolicy policy,
                                   bool generationAlreadyBegun)
{
    if (isBnkFile(file)) { beginBnkOpen(file); return; }
    cancelBnkOpen();
    cancelBnkPreview();
    if (file.hasFileExtension("wem")
        && VgmstreamDecodeService::chooseWemOpenStrategy(file).kind == VgmstreamDecodeService::WemOpenStrategy::Kind::decodedFallback)
    { beginPackedAudioLoad(file); return; }
    if (WindowsMediaAudioService::isSupportedVideoExtension(file.getFileExtension()))
    { beginVideoAudioLoad(file); return; }
    if (!file.existsAsFile())
    {
        if (policy == AudioLoadPolicy::reloadFromDisk)
            setDocumentOperationStatus("Reload failed: the source file is no longer available; in-memory edits were kept.");
        return;
    }

    if (!cancelOrdinaryAudioLoad()) return;
    AudioReturnImage returnImage;
    if (document.hasAudio() || bnkDocument.hasBank()) returnImage = captureAudioReturnImage();
    if (document.hasAudio() && !returnImage.previousAudio)
    { setDocumentOperationStatus("Finish the current edit before changing files."); return; }

    if (generationAlreadyBegun)
        ensureDocumentGeneration(file);
    else
        beginDocumentGeneration(file);
    startLoadTimingProbe(file);

    if (!workspaceSession.shouldRetainPlaylistAudio())
        workspaceSession.clearRetainedPlaylistAudio();

    workspaceSession.retainCurrentAudioDocumentIfNeeded();
    if (policy == AudioLoadPolicy::reloadFromDisk)
        workspaceSession.discardRetainedAudioDocument(file);
    releaseBnkRuntime();

    cancelCacheWarm();
    loadThreadPool.removeAllJobs(false, 0);
    pendingLoadLabel = file.getFileName();
    loadInProgress = true;
    pendingLoadToken.fetch_add(1, std::memory_order_relaxed);
    pendingAutoPlayOnLoad = autoPlayButton.getToggleState();
    pendingAutoPlayFile = file;

    currentSelection.reset();
    waveformView.clearSelection();
    selectionGainSlider.setValue(0.0, juce::dontSendNotification);
    clearReferenceAudioSuitePreview(true);
    waveformView.clearSelectionPreviewGain();

    // Explicit Reload must read the disk version, even when this path is already
    // active or has retained playlist caches. Ordinary activation still reuses it.
    if (policy == AudioLoadPolicy::reuseSession && finishReusedAudioLoad(file, pendingAutoPlayOnLoad))
    {
        pendingAutoPlayOnLoad = false;
        pendingAutoPlayFile = {};
        return;
    }

    workspaceSession.releaseRecoveredActivation();
    document.clear();
    if (returnImage.previousAudio || returnImage.previousBank)
    {
        ordinaryAudioLoad.emplace(); ordinaryAudioLoad->returnImage = std::move(returnImage);
        ordinaryAudioLoad->token = pendingLoadToken.load(std::memory_order_relaxed);
        if (const auto* entry = playlistModel.getCurrentEntry()) ordinaryAudioLoad->expectedEntry = entry->id;
        observeOrdinaryAudioLoadState();
    }
    refreshUiState();
    if (metadataSidebar) metadataSidebar->repaint();
    waveformView.refreshNow();

    const auto token = pendingLoadToken.load(std::memory_order_relaxed);
    loadThreadPool.addJob(new AsyncFileLoadJob(juce::Component::SafePointer<MainComponent>(this),
                                               file,
                                               token),
                          true);
}

void MainComponent::showAsyncLoadPreview(int64_t loadToken,
                                         SingleTrackDocument::FileInfo info,
                                         std::shared_ptr<WaveformModel::PeakCache> peakCache)
{
    if (loadToken != pendingLoadToken.load(std::memory_order_relaxed) || peakCache == nullptr)
        return;
    if (ordinaryAudioLoad && !ordinaryAudioLoadOwnsDocument()) return;

    if (const auto currentState = document.getState(); currentState != nullptr && currentState->info.isValid())
    {
        if (fileInfoMatches(currentState->info, info))
        {
            if (const auto currentPeakCache = waveformModel.getPeakCache(); currentPeakCache != nullptr
                && currentPeakCache->audioRevision == currentState->audioRevision
                && currentPeakCache->binCount == peakCache->binCount
                && currentPeakCache->loadedSampleRange.getStart() == peakCache->loadedSampleRange.getStart()
                && currentPeakCache->loadedSampleRange.getEnd() >= peakCache->loadedSampleRange.getEnd())
            {
                return;
            }
        }

        if (peakCache != nullptr)
        {
            peakCache->state = currentState;
            peakCache->audioRevision = currentState->audioRevision;
            peakCache->durationSeconds = currentState->info.durationSeconds;
            peakCache->sampleRate = currentState->info.sampleRate;
            peakCache->totalSamples = currentState->info.numSamples;
            peakCache->channelCount = currentState->info.numChannels;
        }
    }

    waveformModel.adoptPeakCache(std::move(peakCache));
    audioEngine.warmUpAudioDeviceAsync(0);
    refreshUiState();
    waveformView.refreshNow();
}

void MainComponent::showAsyncLoadMetadata(int64_t loadToken, SingleTrackDocument::FileInfo info)
{
    if (loadToken != pendingLoadToken.load(std::memory_order_relaxed) || !loadInProgress)
        return;
    if (ordinaryAudioLoad && !ordinaryAudioLoadOwnsDocument()) return;

    if (const auto currentState = document.getState(); currentState != nullptr
        && fileInfoMatches(currentState->info, info))
    {
        markFileNameShownIfNeeded();
        return;
    }

    if (!info.isValid() || !document.adoptPreviewState(info))
        return;
    observeOrdinaryAudioLoadState();

    if (bnkDocument.hasBank())
        bnkDocument.clear();

    updateCenterViewVisibility();
    notifyPrimaryContentVisibleIfNeeded();
    refreshUiState();
    repaintMetadataSidebarIfVisible();
    waveformView.refreshNow();
}

void MainComponent::completeAsyncLoad(int64_t loadToken,
                                      const juce::File& file,
                                      std::shared_ptr<SingleTrackDocument::State> state,
                                      std::shared_ptr<WaveformModel::PeakCache> peakCache,
                                      bool bankSignatureAfterReaderFailure,
                                      juce::String errorMessage,
                                      media::OptionalMediaOperation::Ptr optionalOperation)
{
    if (loadToken != pendingLoadToken.load(std::memory_order_relaxed))
        return;
    if (!optionalOperation && ordinaryAudioLoad && !ordinaryAudioLoadOwnsDocument())
    {
        ordinaryAudioLoad.reset();
        loadInProgress = false; pendingLoadLabel.clear(); pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
        abandonActiveLoadTimingProbe("ordinary-workspace-changed");
        setDocumentOperationStatus("Current audio changed while loading; the old result was not applied.");
        refreshUiState(); return;
    }

    const bool packedAdmission = optionalOperation && optionalOperation->module() == media::OptionalMediaFailureState::Module::packedAudio;
    if (packedAdmission)
    {
        if (!packedAudioLoad || packedAudioLoad->operation != optionalOperation || packedAudioLoad->token != loadToken) return;
        const auto outcome = optionalOperation->report();
        const bool cancelled = outcome.cancelled || optionalOperation->cancelFlag().load(std::memory_order_acquire);
        if (!state || cancelled || (outcome.terminal && !outcome.succeeded)
            || document.getState() != packedAudioLoad->previousState)
        {
            if (packedProgressWindow) packedProgressWindow->exitModalState(0);
            loadInProgress = false; pendingLoadLabel.clear(); pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
            abandonActiveLoadTimingProbe(cancelled ? "wem-cancelled" : "wem-not-applied");
            if (packedAudioLoad->previousEntry)
            {
                const juce::ScopedValueSetter<bool> suppress(suppressPlaylistLoad, true);
                playlistModel.setCurrentIndex(playlistModel.indexOfEntry(*packedAudioLoad->previousEntry));
            }
            refreshUiState();
            setDocumentOperationStatus(cancelled ? "WEM audio cancelled; current audio and history were kept."
                : "WEM candidate was not applied; current audio and history were kept.");
            if (!cancelled && !outcome.succeeded) showPackedAudioFailure(file, optionalOperation);
            return;
        }
        // Refresh the return image at the actual activation boundary, including
        // any Save publication that settled while this exact State was loading.
        packedAudioLoad->previousAudio = document.captureWorkspaceSnapshot();
        if (packedAudioLoad->previousAudio && !packedAudioLoad->previousBank)
            packedAudioLoad->previousFile = packedAudioLoad->previousAudio->state()->info.file;
        auto admitted = std::move(*packedAudioLoad); packedAudioLoad.reset();
        workspaceSession.retainCurrentAudioDocumentIfNeeded();
        audioEngine.stop();
        beginDocumentGeneration(file);
        selectLoadedAudioEntry(file);
        workspaceSession.releaseRecoveredActivation();
        releaseBnkRuntime();
        currentSelection.reset(); waveformView.clearSelection();
        admitted.awaitingTerminal = true;
        packedAudioLoad = std::move(admitted);
    }
    else if (optionalOperation)
    {
        if (pendingVideoOperation != optionalOperation) return;
        if (videoProgressWindow) videoProgressWindow->exitModalState(0);
        loadInProgress = false;
        pendingLoadLabel.clear();
        const auto outcome = optionalOperation->report();
        const auto cancelledBeforeAdmission = optionalOperation->cancelFlag().load(std::memory_order_acquire);
        if (!outcome.succeeded || cancelledBeforeAdmission || state == nullptr || document.getState() != videoLoadPreviousState)
        {
            abandonActiveLoadTimingProbe(outcome.cancelled ? "video-cancelled" : "video-not-applied");
            // No preview or state from this candidate has reached the model.
            // Preserve the exact current image, Undo/Redo, playback and owner.
            pendingAutoPlayOnLoad = false;
            pendingAutoPlayFile = {};
            if (videoLoadPreviousEntry)
            {
                const juce::ScopedValueSetter<bool> suppress(suppressPlaylistLoad, true);
                playlistModel.setCurrentIndex(playlistModel.indexOfEntry(*videoLoadPreviousEntry));
            }
            refreshUiState();
            setDocumentOperationStatus(outcome.cancelled || cancelledBeforeAdmission ? "Video audio cancelled; current audio was kept."
                : outcome.succeeded ? "Current audio changed while video was loading; its candidate was not applied."
                : "Video audio failed; current audio and history were kept.");
            if (!outcome.cancelled && !cancelledBeforeAdmission && !outcome.succeeded) showVideoAudioFailure(file, optionalOperation);
            return;
        }
        pendingVideoOperation.reset();
        videoLoadPreviousState.reset();
        videoLoadPreviousEntry.reset();
        workspaceSession.retainCurrentAudioDocumentIfNeeded();
        beginDocumentGeneration(file);
        selectLoadedAudioEntry(file);
        workspaceSession.releaseRecoveredActivation();
        releaseBnkRuntime();
        currentSelection.reset();
        waveformView.clearSelection();
    }

    loadInProgress = false;
    pendingLoadLabel.clear();

    waveformModel.prepareIncomingCaches(peakCache);

    if (state == nullptr || !document.adoptLoadedState(std::move(state)))
    {
        if (packedAdmission && packedAudioLoad)
        {
            auto rejected = std::move(*packedAudioLoad); packedAudioLoad.reset();
            rejected.operation->requestCancel();
            if (packedProgressWindow) packedProgressWindow->exitModalState(0);
            restoreAudioReturn(rejected);
            setDocumentOperationStatus("WEM reader could not be activated; the retained workspace was restored.");
            return;
        }
        if (!optionalOperation && ordinaryAudioLoad)
        {
            if (!cancelOrdinaryAudioLoad(false)) return;
            if (bankSignatureAfterReaderFailure) { beginBnkOpen(file); return; }
            setDocumentOperationStatus("Open failed: " + (errorMessage.isNotEmpty() ? errorMessage
                : juce::String("No supported audio reader could open this file.")) + " Previous audio and complete history were kept.");
            refreshUiState(); repaintMetadataSidebarIfVisible(); waveformView.refreshNow();
            return;
        }
        if (!optionalOperation && bankSignatureAfterReaderFailure)
        { beginBnkOpen(file); return; }

        bnkDocument.clear();
        document.clear();
        abandonActiveLoadTimingProbe("failed");
        playlistFocusBeforeLoad = nullptr;
        failedLoadFileName = file.getFileName();
        failedLoadMessage = errorMessage.isNotEmpty()
            ? errorMessage : "No supported audio reader could open this file.";
        updateCenterViewVisibility();
        refreshUiState();
        repaintMetadataSidebarIfVisible();
        waveformView.refreshNow();
        promoteDeferredChromeIfNeeded(180);
        pendingAutoPlayOnLoad = false;
        pendingAutoPlayFile = {};
        return;
    }

    if (!optionalOperation) { ordinaryAudioLoad.reset(); selectLoadedAudioEntry(file); }
    if (packedAdmission && packedAudioLoad) packedAudioLoad->admittedState = document.getState();

    if (bnkDocument.hasBank())
        bnkDocument.clear();

    workspaceSession.retainCurrentAudioDocumentIfNeeded();

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    if (const auto currentState = document.getState(); currentState != nullptr && currentState->info.isValid())
    {
        appendOpenTimingLogLine(juce::Time::getCurrentTime().toISO8601(true)
                                + " | outcome=load-complete | file="
                                + currentState->info.file.getFullPathName()
                                + " | revision="
                                + juce::String(currentState->audioRevision)
                                + " | incoming-peak="
                                + juce::String(peakCache != nullptr ? (peakCache->isPreview ? "preview" : "full")
                                                                   : "none"));
    }
#endif

    updateCenterViewVisibility();
    notifyPrimaryContentVisibleIfNeeded();
    refreshUiState();
    repaintMetadataSidebarIfVisible();
    waveformView.refreshNow();
    if (pendingAutoPlayOnLoad && file == pendingAutoPlayFile && document.hasAudio())
    {
        audioEngine.seekToSeconds(0.0);
        audioEngine.play();
        refreshTransportReadout();
        refreshFooterReadout();
    }
    pendingAutoPlayOnLoad = false;
    pendingAutoPlayFile = {};

}

void MainComponent::notifyPrimaryContentVisibleIfNeeded()
{
    if (primaryContentVisibleNotified)
        return;

    if (!document.getFileInfo().isValid() && !bnkDocument.hasBank())
        return;

    primaryContentVisibleNotified = true;
    markPrimaryContentVisibleIfNeeded();
    if (bnkDocument.hasBank())
        promoteDeferredChromeIfNeeded(180);
}

void MainComponent::repaintMetadataSidebarIfVisible()
{
    if (!metadataSidebar) return;
    const auto info = document.getFileInfo();
    if (!info.isValid())
    {
        metadataSidebar->clearTypedMetadataInspector();
    }
    else if (coreLifecycle.isCurrent(activeDocumentGeneration)
        && coreLifecycle.isCoreReady(activeDocumentGeneration))
    {
        metadataSidebar->refreshTypedMetadataFacts(activeDocumentGeneration.value, info);
        if (playlistSidebar) playlistSidebar->updateLoadedDuration(info.file, info.durationSeconds);
    }
    if (metadataSidebar->isVisible())
        metadataSidebar->repaint();
}

void MainComponent::saveDocument(bool forceSaveAs, std::function<void(bool)> completion)
{
    if (!document.hasAudio())
    {
        if (completion)
            completion(false);
        return;
    }

    if (!forceSaveAs)
    {
        if (!document.canSaveOverSourceFile())
        {
            setDocumentOperationStatus("Current source format is read-only here. Use Save As to export.");
            if (completion)
                completion(false);
            return;
        }
        else
        {
            const auto sourceExtension = document.getFileInfo().file.getFileExtension().toLowerCase();
            const auto isLossySource = sourceExtension == ".mp3" || sourceExtension == ".ogg";
            if (isLossySource && !lossyOverwriteConfirmedForSession)
            {
                const auto generation = activeDocumentGeneration.value;
                juce::Component::SafePointer<MainComponent> safeThis(this);
                juce::AlertWindow::showOkCancelBox(
                    juce::MessageBoxIconType::WarningIcon,
                    "Lossy In-place Save",
                    "Ctrl+S will re-encode and replace the current "
                        + sourceExtension.trimCharactersAtStart(".").toUpperCase()
                        + " file. This can reduce audio quality. Continue?",
                    "Save and Re-encode",
                    "Cancel",
                    this,
                    juce::ModalCallbackFunction::create(
                        [safeThis, generation, completion = std::move(completion)](int choice) mutable
                        {
                            if (safeThis == nullptr)
                                return;
                            if (choice != 1 || safeThis->activeDocumentGeneration.value != generation)
                            {
                                if (completion) completion(false);
                                return;
                            }
                            safeThis->lossyOverwriteConfirmedForSession = true;
                            safeThis->saveDocument(false, std::move(completion));
                        }));
                return;
            }

            const auto snapshot = document.createSaveSnapshot({},
                                                              SingleTrackDocument::SaveIntent::checkpoint,
                                                              getStoredSaveEncodingOptions());
            if (!snapshot.isValid())
            {
                setDocumentOperationStatus("Save failed: no valid audio snapshot.");
                if (completion)
                    completion(false);
                return;
            }

            setDocumentOperationStatus("Saving " + snapshot.targetFile.getFileName() + "...");
            auto safeThis = juce::Component::SafePointer<MainComponent>(this);
            auto requestAccepted = saveCoordinator.requestSave(snapshot,
                                                                [safeThis, generation = activeDocumentGeneration.value, completion](const auto& result) mutable
                                                                {
                                                                    if (safeThis == nullptr)
                                                                        return;
                                                                    if (safeThis->activeDocumentGeneration.value != generation)
                                                                    {
                                                                        if (completion) completion(false);
                                                                        return;
                                                                    }

                                                                    const auto hasPendingSave = safeThis->saveCoordinator.hasPendingSave();
                                                                    if (result.succeeded)
                                                                    {
                                                                        if (!safeThis->document.hasUnsavedChanges())
                                                                            safeThis->workspaceSession.retainCurrentAudioDocumentIfNeeded();

                                                                        auto resultDetails = result.encodingDescription;
                                                                        if (result.metadataDescription.startsWithIgnoreCase("Metadata transformed"))
                                                                            resultDetails = resultDetails.isNotEmpty()
                                                                                              ? resultDetails + "; " + result.metadataDescription
                                                                                              : result.metadataDescription;
                                                                        safeThis->setDocumentOperationStatus(hasPendingSave
                                                                                                                ? "Saving latest edit..."
                                                                                                                : (safeThis->document.hasUnsavedChanges()
                                                                                                                       ? "Saved earlier edits; newer changes still need saving: "
                                                                                                                       : "Saved ") + result.targetFile.getFileName()
                                                                                                                    + (resultDetails.isNotEmpty()
                                                                                                                           ? " (" + resultDetails + ")"
                                                                                                                           : juce::String()));
                                                                    }
                                                                    else if (result.failureReason == SingleTrackDocument::SaveFailureReason::staleSource)
                                                                    {
                                                                        safeThis->refreshUiState();
                                                                        safeThis->showExternalSourceSaveConflict(std::move(completion));
                                                                        return;
                                                                    }
                                                                    else
                                                                    {
                                                                        safeThis->setDocumentOperationStatus("Save failed for " + result.targetFile.getFileName()
                                                                                                                + ": " + result.errorMessage);
                                                                    }

                                                                    safeThis->refreshUiState();
                                                                    if (completion)
                                                                    {
                                                                        safeThis->saveCoordinator.whenIdle([safeThis, completion = std::move(completion)](bool succeeded) mutable
                                                                        {
                                                                            if (safeThis != nullptr && completion)
                                                                                completion(succeeded);
                                                                        });
                                                                    }
                                                                });

            if (!requestAccepted)
            {
                setDocumentOperationStatus("Save failed: the snapshot was rejected.");
                if (completion)
                    completion(false);
            }
            return;
        }
    }

    chooseSaveTarget(SingleTrackDocument::SaveIntent::saveAs,
                     std::move(completion));
}

void MainComponent::exportDocument(std::function<void(bool)> completion)
{
    if (!document.hasAudio())
    {
        if (completion)
            completion(false);
        return;
    }

    chooseSaveTarget(SingleTrackDocument::SaveIntent::exportCopy,
                     std::move(completion));
}

SingleTrackDocument::SaveEncodingOptions MainComponent::getStoredSaveEncodingOptions() const
{
    SingleTrackDocument::SaveEncodingOptions options;
    if (auto* settings = appProperties.getUserSettings())
    {
        options.mp3BitrateKbps = settings->getIntValue(mp3SaveBitrateKey, options.mp3BitrateKbps);
        options.oggQualityIndex = settings->getIntValue(oggSaveQualityKey, options.oggQualityIndex);
        options.flacCompressionLevel = settings->getIntValue(flacSaveCompressionKey,
                                                             options.flacCompressionLevel);
    }

    const std::array supportedMp3Bitrates { 96, 128, 160, 192, 256, 320 };
    if (std::find(supportedMp3Bitrates.begin(), supportedMp3Bitrates.end(), options.mp3BitrateKbps)
        == supportedMp3Bitrates.end())
    {
        options.mp3BitrateKbps = 192;
    }
    options.oggQualityIndex = juce::jlimit(0, 10, options.oggQualityIndex);
    options.flacCompressionLevel = juce::jlimit(0, 8, options.flacCompressionLevel);
    return options;
}

void MainComponent::chooseSaveEncodingOptions(
    const juce::File& targetFile,
    std::function<void(std::optional<SingleTrackDocument::SaveEncodingOptions>)> completion)
{
    auto options = getStoredSaveEncodingOptions();
    const auto extension = targetFile.getFileExtension().toLowerCase();
    if (extension != ".mp3" && extension != ".ogg" && extension != ".flac")
    {
        completion(options);
        return;
    }

    juce::StringArray choices;
    juce::String fieldName;
    auto selectedIndex = 0;
    if (extension == ".mp3")
    {
        const std::array bitrates { 96, 128, 160, 192, 256, 320 };
        for (auto bitrate : bitrates)
        {
            choices.add(juce::String(bitrate) + " kbps");
            if (bitrate == options.mp3BitrateKbps)
                selectedIndex = choices.size() - 1;
        }
        fieldName = "MP3 bitrate";
    }
    else if (extension == ".ogg")
    {
        for (auto quality = 0; quality <= 10; ++quality)
            choices.add("Quality " + juce::String(quality));
        selectedIndex = options.oggQualityIndex;
        fieldName = "Ogg Vorbis quality";
    }
    else
    {
        for (auto level = 0; level <= 8; ++level)
            choices.add("Level " + juce::String(level));
        selectedIndex = options.flacCompressionLevel;
        fieldName = "FLAC compression";
    }

    encodingOptionsDialog = std::make_unique<SaveEncodingOptionsDialog>(
        *this, fieldName, choices, selectedIndex);

    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    encodingOptionsDialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis, extension, options, completion = std::move(completion)](int result) mutable
            {
                if (safeThis == nullptr)
                    return;

                const auto* choice = safeThis->encodingOptionsDialog != nullptr
                                       ? &safeThis->encodingOptionsDialog->getChoice()
                                       : nullptr;
                if (result != 1 || choice == nullptr || choice->getSelectedItemIndex() < 0)
                {
                    safeThis->encodingOptionsDialog.reset();
                    completion(std::nullopt);
                    return;
                }

                if (extension == ".mp3")
                {
                    constexpr std::array bitrates { 96, 128, 160, 192, 256, 320 };
                    options.mp3BitrateKbps = bitrates[static_cast<size_t>(choice->getSelectedItemIndex())];
                }
                else if (extension == ".ogg")
                {
                    options.oggQualityIndex = choice->getSelectedItemIndex();
                }
                else
                {
                    options.flacCompressionLevel = choice->getSelectedItemIndex();
                }

                if (auto* settings = safeThis->appProperties.getUserSettings())
                {
                    settings->setValue(mp3SaveBitrateKey, options.mp3BitrateKbps);
                    settings->setValue(oggSaveQualityKey, options.oggQualityIndex);
                    settings->setValue(flacSaveCompressionKey, options.flacCompressionLevel);
                }

                safeThis->encodingOptionsDialog.reset();
                completion(options);
            }));
}

void MainComponent::showExternalSourceSaveConflict(std::function<void(bool)> completion)
{
    if (externalSourceSaveConflictPending)
    {
        if (completion)
            completion(false);
        return;
    }

    externalSourceSaveConflictPending = true;
    setDocumentOperationStatus("The source changed outside Single Editor. Waiting for your decision...");
    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    saveCoordinator.whenIdle(
        [safeThis, completion = std::move(completion)](bool) mutable
        {
            if (safeThis == nullptr)
                return;

            const auto sourceFile = safeThis->document.getFileInfo().file;
            const auto options = juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Source File Changed")
                                     .withMessage("The file on disk changed after it was opened.\n\n"
                                                  "Reload discards the current in-memory edits and opens the disk version. "
                                                  "Save As keeps the edits and writes them to a new file. Cancel changes nothing.")
                                     .withButton("Reload")
                                     .withButton("Save As")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(safeThis.getComponent());
            juce::AlertWindow::showAsync(
                options,
                [safeThis, sourceFile, completion = std::move(completion)](int result) mutable
                {
                    if (safeThis == nullptr)
                        return;

                    safeThis->externalSourceSaveConflictPending = false;
                    if (result == 1)
                    {
                        safeThis->setDocumentOperationStatus("Reloading the externally changed source...");
                        safeThis->beginAsyncLoad(sourceFile, AudioLoadPolicy::reloadFromDisk);
                        if (completion)
                            completion(false);
                        return;
                    }

                    if (result == 2)
                    {
                        safeThis->chooseSaveTarget(SingleTrackDocument::SaveIntent::saveAs,
                                                   std::move(completion));
                        return;
                    }

                    safeThis->setDocumentOperationStatus("Save cancelled; in-memory edits were kept.");
                    if (completion)
                        completion(false);
                });
        });
}

void MainComponent::beginSaveToTarget(const juce::File& targetFile,
                                      SingleTrackDocument::SaveIntent intent,
                                      SingleTrackDocument::SaveEncodingOptions encodingOptions,
                                      std::function<void(bool)> completion)
{
    const auto snapshot = document.createSaveSnapshot(targetFile, intent, encodingOptions);
    if (!snapshot.isValid())
    {
        setDocumentOperationStatus("Save failed: no valid audio snapshot.");
        if (completion)
            completion(false);
        return;
    }

    setDocumentOperationStatus(
        intent == SingleTrackDocument::SaveIntent::saveAs
            ? "Saving as " + targetFile.getFileName() + "..."
            : "Exporting " + targetFile.getFileName() + "...");
    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
    const auto accepted = saveCoordinator.requestSave(
        snapshot,
        [safeThis, intent, generation = activeDocumentGeneration.value, completion](const auto& saveResult) mutable
        {
            if (safeThis == nullptr)
                return;
            if (safeThis->activeDocumentGeneration.value != generation)
            {
                if (completion) completion(false);
                return;
            }

            if (saveResult.succeeded)
            {
                auto resultDetails = saveResult.encodingDescription;
                if (saveResult.metadataDescription.startsWithIgnoreCase("Metadata omitted")
                    || saveResult.metadataDescription.startsWithIgnoreCase("Metadata transformed"))
                    resultDetails = resultDetails.isNotEmpty()
                                      ? resultDetails + "; " + saveResult.metadataDescription
                                      : saveResult.metadataDescription;
                const auto resultSuffix = resultDetails.isNotEmpty()
                                            ? " (" + resultDetails + ")"
                                            : juce::String();
                if (intent == SingleTrackDocument::SaveIntent::saveAs)
                {
                    safeThis->playlistModel.replaceCurrentFile(saveResult.targetFile);
                    safeThis->workspaceSession.retainCurrentAudioDocumentIfNeeded();
                    safeThis->workspaceSession.rememberOpenedFiles({ saveResult.targetFile });
                    safeThis->requestSessionSettingsPersist();
                    safeThis->setDocumentOperationStatus(
                        "Saved as " + saveResult.targetFile.getFileName() + resultSuffix
                            + (safeThis->document.hasUnsavedChanges()
                                   ? "; newer changes still need saving" : juce::String()));
                }
                else
                {
                    safeThis->setDocumentOperationStatus(
                        "Exported " + saveResult.targetFile.getFileName() + resultSuffix);
                }
            }
            else
            {
                safeThis->setDocumentOperationStatus(
                    (intent == SingleTrackDocument::SaveIntent::saveAs
                         ? "Save As failed for "
                         : "Export failed for ")
                        + saveResult.targetFile.getFileName() + ": " + saveResult.errorMessage);
            }

            safeThis->refreshUiState();
            if (completion)
                completion(saveResult.succeeded);
        });
    if (!accepted)
    {
        setDocumentOperationStatus("Save failed: the snapshot was rejected.");
        if (completion)
            completion(false);
    }
}

void MainComponent::chooseSaveTarget(SingleTrackDocument::SaveIntent intent,
                                     std::function<void(bool)> completion)
{
    const auto generation = activeDocumentGeneration.value;
    if (saveCoordinator.isSaveInProgress())
    {
        setDocumentOperationStatus("Finishing the current save...");
        auto safeThis = juce::Component::SafePointer<MainComponent>(this);
        saveCoordinator.whenIdle([safeThis, intent, generation, completion = std::move(completion)](bool) mutable
        {
            if (safeThis != nullptr && safeThis->activeDocumentGeneration.value == generation)
                safeThis->chooseSaveTarget(intent, std::move(completion));
            else if (completion) completion(false);
        });
        return;
    }

    const auto defaultExtension = document.getDefaultSaveExtension();
    auto initialTarget = document.getFileInfo().file;
    if (initialTarget == juce::File())
        initialTarget = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Single Editor Export" + defaultExtension);
    else if (initialTarget.getFileExtension().isEmpty() || !document.canSaveToFile(initialTarget))
        initialTarget = initialTarget.withFileExtension(defaultExtension);

    if (intent == SingleTrackDocument::SaveIntent::exportCopy)
    {
        initialTarget = initialTarget.getSiblingFile(
            initialTarget.getFileNameWithoutExtension() + " Export"
            + initialTarget.getFileExtension());
    }

    fileChooser = std::make_unique<juce::FileChooser>(
                                                      intent == SingleTrackDocument::SaveIntent::saveAs
                                                          ? "Save edited audio as"
                                                          : "Export edited audio",
                                                      initialTarget,
                                                      "*.wav;*.flac;*.aiff;*.aif;*.ogg;*.mp3");
      fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                               | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                               [this, safeThis = juce::Component::SafePointer<MainComponent>(this), generation,
                                intent, completion = std::move(completion)](const juce::FileChooser& chooser) mutable
                               {
                                   if (safeThis == nullptr) return;
                                   auto result = chooser.getResult();
                                   fileChooser.reset();
                                   if (activeDocumentGeneration.value != generation)
                                   {
                                       if (completion) completion(false);
                                       return;
                                   }
                                   if (result == juce::File())
                                   {
                                       if (completion)
                                           completion(false);
                                       return;
                                   }

                                   if (result.getFileExtension().isEmpty())
                                       result = result.withFileExtension(document.getDefaultSaveExtension());

                                   const auto overwritingSourceFile = result == document.getFileInfo().file;
                                   if (overwritingSourceFile)
                                   {
                                       if (intent == SingleTrackDocument::SaveIntent::saveAs)
                                           saveDocument(false, std::move(completion));
                                       else
                                       {
                                           setDocumentOperationStatus("Export target must differ from the current document.");
                                           if (completion)
                                               completion(false);
                                       }
                                       return;
                                   }

                                   chooseSaveEncodingOptions(
                                       result,
                                       [safeThis, generation, result, intent, completion = std::move(completion)](
                                           std::optional<SingleTrackDocument::SaveEncodingOptions> encodingOptions) mutable
                                       {
                                           if (safeThis == nullptr)
                                               return;
                                           if (safeThis->activeDocumentGeneration.value != generation || !encodingOptions.has_value())
                                           {
                                               if (completion)
                                                   completion(false);
                                               return;
                                           }
                                           safeThis->beginSaveToTarget(result,
                                                                       intent,
                                                                       *encodingOptions,
                                                                       std::move(completion));
                                       });
                               });
}

void MainComponent::setDocumentOperationStatus(juce::String message)
{
    documentOperationStatus = std::move(message);
    documentOperationStatusGeneration = activeDocumentGeneration.value;
    const auto state = document.getState();
    documentOperationStatusChangeGeneration = state != nullptr ? state->changeGeneration : 0;
    engineStatusLabel.setText(documentOperationStatus, juce::dontSendNotification);
}

void MainComponent::refreshUiState()
{
    const auto allowExtendedChromeRefresh = !deferChromeForStartup;

    if (loadInProgress)
    {
        const auto canTransportDuringLoad = hasPlayableAudioReady();
        const auto playbackQueuedDuringLoad = !canTransportDuringLoad && pendingAutoPlayOnLoad;
        const auto previewInfo = audioEngine.getLoadedFileInfo();
        const auto currentState = document.getState();
        const auto currentPeakCache = waveformModel.getPeakCache();
        const auto hasPreviewWaveform = currentState != nullptr
                                     && currentState->info.isValid()
                                     && currentPeakCache != nullptr
                                     && currentPeakCache->audioRevision == currentState->audioRevision
                                     && currentPeakCache->binCount > 0;
        const auto loadingFileName = previewInfo.isValid() ? previewInfo.file.getFileName()
                                                           : (pendingLoadLabel.isNotEmpty()
                                                                  ? pendingLoadLabel
                                                                  : juce::String("Opening audio..."));
        filePillLabel.setText(previewInfo.isValid() ? previewInfo.file.getFileName()
                                                    : "Loading " + pendingLoadLabel + "...",
                              juce::dontSendNotification);
        if (canTransportDuringLoad)
        {
            titleBar.setSubtitle(hasProgressiveWaveformPending()
                                   ? "Playback ready, extending waveform in the background"
                                   : "Playback ready");
            engineStatusLabel.setText(hasProgressiveWaveformPending()
                                          ? "You can already play from 0 s while the waveform tail keeps filling in."
                                          : "Playback is ready.",
                                      juce::dontSendNotification);
            waveformView.setLoadingHint(loadingFileName,
                                        hasProgressiveWaveformPending()
                                            ? "Playback is ready. The waveform tail is still filling in."
                                            : "Playback is ready.");
        }
        else if (playbackQueuedDuringLoad)
        {
            titleBar.setSubtitle(hasPreviewWaveform
                                   ? "Waveform preview visible, playback queued from 0 s"
                                   : "Playback queued while the first readable segment is prepared");
            engineStatusLabel.setText(hasPreviewWaveform
                                          ? "Waveform preview is visible. Playback will start automatically as soon as the audio head is ready."
                                          : "Playback is queued from 0 s. Starting as soon as the audio head is ready.",
                                      juce::dontSendNotification);
            waveformView.setLoadingHint(loadingFileName,
                                        hasPreviewWaveform
                                            ? "Playback is queued. The waveform tail is still filling in."
                                            : "Playback is queued while the first readable audio segment is prepared.");
        }
        else
        {
            titleBar.setSubtitle(hasPreviewWaveform
                                   ? "Waveform preview ready, finalizing editable document"
                                   : (previewInfo.isValid()
                                          ? "File opened, generating first waveform preview"
                                          : "Preparing editable document state in the background"));
            engineStatusLabel.setText(hasPreviewWaveform
                                          ? "Preview waveform visible, decoding editable buffer..."
                                          : (previewInfo.isValid()
                                                 ? "File metadata ready, building waveform preview..."
                                                 : "Loading audio into editor core..."),
                                      juce::dontSendNotification);
            waveformView.setLoadingHint(loadingFileName,
                                        hasPreviewWaveform
                                            ? "Preview waveform is visible. Preparing immediate playback..."
                                            : (previewInfo.isValid()
                                                   ? "Metadata is ready. Drawing the first waveform..."
                                                   : "Starting the audio document..."));
        }
        peakLabel.setText("TP --", juce::dontSendNotification);
        refreshTransportReadout();
        refreshFooterReadout();
        refreshWindowAndTitle();
        recentButton.setEnabled(workspaceSession.hasRecentFiles());
        updateTransportAvailability();
        updateCenterViewVisibility();
        if (allowExtendedChromeRefresh)
        {
            refreshBnkControlSurface();
            updateSelectionPopup();
        }
        markFileNameShownIfNeeded();
        return;
    }

    if (!RendererVisualStyle::isClassic() && failedLoadFileName.isNotEmpty())
        waveformView.setOpenFailureHint(failedLoadFileName, failedLoadMessage);
    else
        waveformView.setLoadingHint({}, {});

    const auto bankInfo = bnkDocument.getBankInfo();
    if (bankInfo.isValid())
    {
        filePillLabel.setText(bankInfo.file.getFileName(), juce::dontSendNotification);
        titleBar.setSubtitle(formatBnkDetails(bankInfo));
        auto bankStatus = formatBnkDetails(bankInfo);
        if (const auto selectedEventId = bnkDocument.getSelectedEventId(); selectedEventId.has_value())
        {
            if (const auto state = bnkDocument.getState(); state != nullptr)
            {
                const auto found = std::find_if(state->events.begin(),
                                                state->events.end(),
                                                [selectedEventId](const BnkDocument::EventDescriptor& event)
                                                {
                                                    return event.id == *selectedEventId;
                                                });
                if (found != state->events.end())
                    bankStatus << "  |  " << found->displayName << "  |  " << found->playbackSummary;
            }
        }
        engineStatusLabel.setText(bankStatus, juce::dontSendNotification);
        monitorSlider.setValue(audioEngine.getMonitorGainLinear() * 100.0f, juce::dontSendNotification);
        peakLabel.setText(audioEngine.hasLoadedFile()
                              ? formatMeterReadout(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()), audioEngine)
                              : "BNK",
                          juce::dontSendNotification);
        recentButton.setEnabled(workspaceSession.hasRecentFiles());
        refreshTransportReadout();
        refreshFooterReadout();
        refreshWindowAndTitle();
        updateTransportAvailability();
        updateCenterViewVisibility();
        if (allowExtendedChromeRefresh)
        {
            refreshBnkControlSurface();
            updateSelectionPopup();
        }
        markFileNameShownIfNeeded();
        return;
    }

    const auto info = audioEngine.getLoadedFileInfo();
    const auto fileName = info.isValid() ? info.file.getFileName()
        : failedLoadFileName.isNotEmpty() ? "Failed to load " + failedLoadFileName : "No file loaded";
    filePillLabel.setText((document.hasUnsavedChanges() ? "* " : "") + fileName, juce::dontSendNotification);
    titleBar.setSubtitle(info.isValid() ? formatAudioDetails(info) : juce::String());
    if (documentOperationStatus.isNotEmpty())
    {
        const auto state = document.getState();
        // Keep a save result through ordinary UI refreshes, not through another edit or document.
        if (documentOperationStatusGeneration != activeDocumentGeneration.value
            || state == nullptr || documentOperationStatusChangeGeneration != state->changeGeneration)
            documentOperationStatus.clear();
    }
    engineStatusLabel.setText(!info.isValid() && failedLoadFileName.isNotEmpty()
                                 ? failedLoadMessage
                                 : documentOperationStatus.isNotEmpty() ? documentOperationStatus
                                                                       : formatAudioDetails(info),
                              juce::dontSendNotification);
    monitorSlider.setValue(audioEngine.getMonitorGainLinear() * 100.0f, juce::dontSendNotification);
    returnToStartButton.setToggleState(audioEngine.shouldReturnToPlayStartOnStop(), juce::dontSendNotification);
    loopButton.setToggleState(audioEngine.isLoopEnabled(), juce::dontSendNotification);
    if (meterBridge) meterBridge->setMeterMode(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()));
    peakLabel.setText(formatMeterReadout(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()), audioEngine),
                      juce::dontSendNotification);
    refreshTransportReadout();
    refreshFooterReadout();
    refreshWindowAndTitle();
    recentButton.setEnabled(workspaceSession.hasRecentFiles());
    updateTransportAvailability();
    updateCenterViewVisibility();
    if (allowExtendedChromeRefresh)
    {
        refreshBnkControlSurface();
        updateSelectionPopup();
    }
    markFileNameShownIfNeeded();
}

bool MainComponent::hasPlayableAudioReady() const noexcept
{
    const auto state = document.getState();
    return state != nullptr
        && state->info.isValid()
        && state->hasPlaybackSource()
        && audioEngine.hasLoadedFile();
}

bool MainComponent::canQueuePlaybackDuringLoad() const noexcept
{
    if (!loadInProgress)
        return false;

    const auto state = document.getState();
    return state != nullptr
        && state->info.isValid()
        && !state->hasPlaybackSource()
        && pendingAutoPlayFile.existsAsFile()
        && state->info.file == pendingAutoPlayFile;
}

bool MainComponent::hasProgressiveWaveformPending() const noexcept
{
    const auto peakCache = waveformModel.getPeakCache();
    return peakCache != nullptr
        && peakCache->isPreview
        && peakCache->loadedSampleRange.getLength() > 0
        && peakCache->loadedSampleRange.getEnd() < peakCache->totalSamples;
}

void MainComponent::requestAudioPlaybackOrQueue()
{
    if (audioEngine.isDeviceLeasedForProbe())
    {
        if (deviceProbe) deviceProbe->cancel();
        return; // retry playback only after the exact device has been restored
    }
    if (bnkDocument.hasBank())
        return;

    if (hasPlayableAudioReady())
    {
        audioEngine.play();
        waveformView.refreshNow();
        refreshTransportReadout();
        refreshFooterReadout();
        return;
    }

    if (!canQueuePlaybackDuringLoad())
        return;

    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid())
        return;

    pendingAutoPlayOnLoad = true;
    pendingAutoPlayFile = state->info.file;
    audioEngine.seekToSeconds(0.0);
    audioEngine.warmUpAudioDeviceAsync(0);
    refreshUiState();
    waveformView.refreshNow();
}

void MainComponent::toggleAudioPlaybackOrQueue()
{
    if (audioEngine.isPlaying())
    {
        audioEngine.stop();
        waveformView.refreshNow();
        refreshTransportReadout();
        refreshFooterReadout();
        return;
    }

    requestAudioPlaybackOrQueue();
}

void MainComponent::setModernAppearanceTheme(ModernTheme theme)
{
    if (RendererVisualStyle::isClassic() || theme == RendererVisualStyle::modernTheme()) return;
    RendererVisualStyle::setModernTheme(theme);
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        ModernAppleLookAndFeel::refreshTheme(*desktop.getComponent(i));
    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue(PreferencesComponent::modernThemeKey, theme == ModernTheme::night ? "night" : "light");
        settings->saveIfNeeded();
    }
}

void MainComponent::refreshTimelineMarkers()
{
    if (workspaceModuleHost == nullptr) return;
    auto* markers = dynamic_cast<TimelineMarkersComponent*>(workspaceModuleHost->getModuleComponent(WorkspaceModuleId::markers));
    if (markers == nullptr || !markers->isShowing()) return;
    markers->setDocument(!loadInProgress && !bnkDocument.hasBank()
                            && coreLifecycle.isCoreReady(activeDocumentGeneration) ? document.getState() : nullptr);
    markers->setTransport(audioEngine.getCurrentPositionSeconds(), currentSelection);
}

void MainComponent::refreshTransportReadout()
{
    refreshTimelineMarkers();
    if (loadInProgress && !hasPlayableAudioReady())
    {
        transportStatusLabel.setText((pendingAutoPlayOnLoad ? "Queued  " : "Loading  ") + pendingLoadLabel,
                                     juce::dontSendNotification);
        return;
    }

    if (const auto bankInfo = bnkDocument.getBankInfo(); bankInfo.isValid())
    {
        if (audioEngine.hasLoadedFile())
        {
            const auto state = audioEngine.getPlaybackState();
            const auto position = audioEngine.getCurrentPositionSeconds();
            const auto duration = audioEngine.getDurationSeconds();
            transportStatusLabel.setText("BNK Preview  " + formatPlaybackState(state) + "  "
                                             + formatTimecode(position) + " / " + formatTimecode(duration),
                                         juce::dontSendNotification);
        }
        else
        {
            transportStatusLabel.setText("BNK Loaded  " + juce::String(bankInfo.eventCount) + " event(s) / "
                                             + juce::String(bankInfo.mediaCount) + " media",
                                         juce::dontSendNotification);
        }
        return;
    }

    const auto state = audioEngine.getPlaybackState();
    const auto position = audioEngine.getCurrentPositionSeconds();
    const auto duration = audioEngine.getDurationSeconds();
    auto transportText = formatPlaybackState(state) + "  " + formatTimecode(position) + " / " + formatTimecode(duration);
    if (hasProgressiveWaveformPending())
        transportText << "  |  Waveform tail loading";
    transportStatusLabel.setTooltip(transportText);
    transportStatusLabel.setText(!deferChromeForStartup && !RendererVisualStyle::isClassic()
                                    ? formatTimecode(position) : transportText, juce::dontSendNotification);
}

void MainComponent::refreshFooterReadout()
{
    refreshTimelineMarkers();
    engineStatusLabel.setTooltip(engineStatusLabel.getText());
    if (loadInProgress && !hasPlayableAudioReady())
    {
        footerRightLabel.setModernText({});
        footerLeftLabel.setText("Selection: --", juce::dontSendNotification);
        footerRightLabel.setText(pendingAutoPlayOnLoad
                                     ? "Playback queued from 0 s while the first readable audio segment is prepared"
                                     : "Background load in progress",
                                 juce::dontSendNotification);
        return;
    }

    if (const auto bankInfo = bnkDocument.getBankInfo(); bankInfo.isValid())
    {
        footerRightLabel.setModernText({});
        auto leftText = "BNK browser ready  |  " + bankInfo.file.getFileName();
        auto rightText = "Events " + juce::String(bankInfo.eventCount)
                       + "  |  Media " + juce::String(bankInfo.mediaCount)
                       + "  |  HIRC " + juce::String(bankInfo.hircObjectCount);

        if (const auto selectedEventId = bnkDocument.getSelectedEventId(); selectedEventId.has_value())
        {
            if (const auto state = bnkDocument.getState(); state != nullptr)
            {
                const auto found = std::find_if(state->events.begin(),
                                                state->events.end(),
                                                [selectedEventId](const BnkDocument::EventDescriptor& event)
                                                {
                                                    return event.id == *selectedEventId;
                                                });
                if (found != state->events.end())
                {
                    leftText << "  |  Event " << found->displayName;
                    rightText << "  |  " << found->playbackSummary;
                }
            }
        }

        footerLeftLabel.setText(leftText, juce::dontSendNotification);
        footerRightLabel.setText(rightText, juce::dontSendNotification);
        return;
    }

    footerLeftLabel.setText((document.hasUnsavedChanges() ? "Modified  |  " : "")
                                + (RendererVisualStyle::isClassic() ? formatSelectionRange(currentSelection)
                                                                   : filePillLabel.getText()),
                            juce::dontSendNotification);

    auto cursorText = "Cursor: " + formatTimecode(audioEngine.getCurrentPositionSeconds());
    cursorText += "  |  Start: " + formatTimecode(audioEngine.getPlayStartSeconds());
    cursorText += "  |  Monitor " + juce::String(monitorSlider.getValue(), 0) + "%";
    cursorText += "  |  " + formatMeterReadout(static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()), audioEngine);
    cursorText += document.canUndo() ? "  |  Undo ready" : "  |  Undo --";
    cursorText += document.canRedo() ? "  |  Redo ready" : "  |  Redo --";
    if (hasProgressiveWaveformPending())
        cursorText += "  |  Waveform tail loading";
    footerRightLabel.setModernText(hasProgressiveWaveformPending()
        ? "Waveform tail loading"
        : "Monitor " + juce::String(monitorSlider.getValue(), 0) + "%");
    footerRightLabel.setText(cursorText, juce::dontSendNotification);
    footerRightLabel.setTooltip(cursorText);
}

void MainComponent::refreshWindowAndTitle()
{
    if (const auto bankInfo = bnkDocument.getBankInfo(); bankInfo.isValid())
    {
        const auto topTitle = bankInfo.file.getFileName();
        titleBar.setTitle(topTitle);
        hostWindow.setName(topTitle + " - Single Editor");
        return;
    }

    const auto info = audioEngine.getLoadedFileInfo();
    const auto dirtyPrefix = document.hasUnsavedChanges() ? "* " : "";
    const auto topTitle = info.isValid() ? dirtyPrefix + info.file.getFileName() : "Single Editor";
    const auto windowTitle = info.isValid() ? topTitle + " - Single Editor" : "Single Editor";

    titleBar.setTitle(topTitle);
    hostWindow.setName(windowTitle);
}

void MainComponent::updateSidebarToggleButtons()
{
    togglePlaylistSidebarButton.setTooltip(playlistSidebarCollapsed ? "Show Playlist Sidebar" : "Hide Playlist Sidebar");
    toggleMetadataSidebarButton.setTooltip(metadataSidebarCollapsed ? "Show Metadata Sidebar" : "Hide Metadata Sidebar");
    togglePlaylistSidebarButton.setToggleState(!playlistSidebarCollapsed, juce::dontSendNotification);
    toggleMetadataSidebarButton.setToggleState(!metadataSidebarCollapsed, juce::dontSendNotification);
    togglePlaylistSidebarButton.setButtonText(playlistSidebarCollapsed ? ">" : "<");
    toggleMetadataSidebarButton.setButtonText(metadataSidebarCollapsed ? "<" : ">");
}

void MainComponent::updateTransportAvailability()
{
    const auto canTransportDuringLoad = hasPlayableAudioReady();
    const auto canQueuePlayback = canQueuePlaybackDuringLoad();
    const auto canTransport = audioEngine.hasLoadedFile() && (!loadInProgress || canTransportDuringLoad);
    const auto canEditWaveform = canTransport && !loadInProgress && !bnkDocument.hasBank();
    const auto canMutateWaveform = canEditWaveform
                                && !referenceAudioSuiteApplyInFlight
                                && !parametricEqApplyInFlight
                                && !truePeakLimiterApplyInFlight;
    const auto hasSelection = currentSelection.has_value() && currentSelection->getLength() > 0.0;
    startButton.setEnabled(canTransport);
    saveButton.setEnabled(canEditWaveform);
    saveAsButton.setEnabled(canEditWaveform);
    removeFileButton.setEnabled(playlistModel.getCurrentIndex() >= 0 && !loadInProgress);
    previousButton.setEnabled(canTransport);
    nextButton.setEnabled(canTransport);
    playButton.setEnabled(canTransport || canQueuePlayback);
    pauseButton.setEnabled(canTransport);
    stopButton.setEnabled(canTransport);
    returnToStartButton.setEnabled(canTransport);
    loopButton.setEnabled(canTransport);
    monitorSlider.setEnabled(!loadInProgress || canTransportDuringLoad);
    meterModeBox.setEnabled(!loadInProgress || canTransportDuringLoad);
    preferencesButton.setEnabled(true);

    gainDownButton.setEnabled(hasSelection && canMutateWaveform);
    gainUpButton.setEnabled(hasSelection && canMutateWaveform);
    fadeInButton.setEnabled(hasSelection && canMutateWaveform);
    fadeOutButton.setEnabled(hasSelection && canMutateWaveform);
    deleteSelectionButton.setEnabled(hasSelection && canMutateWaveform);
    selectionGainSlider.setEnabled(hasSelection && canMutateWaveform);
    applySelectionGainButton.setEnabled(hasSelection && canMutateWaveform && std::abs(selectionGainSlider.getValue()) > 0.01);
    cancelSelectionGainButton.setEnabled(hasSelection && canMutateWaveform);
    undoButton.setEnabled(document.canUndo() && canMutateWaveform);
    redoButton.setEnabled(document.canRedo() && canMutateWaveform);
    resetEnvelopeButton.setEnabled(canMutateWaveform);
    timeZoomOutButton.setEnabled(canEditWaveform);
    timeZoomInButton.setEnabled(canEditWaveform);
    ampZoomOutButton.setEnabled(canEditWaveform);
    ampZoomInButton.setEnabled(canEditWaveform);
    resetViewButton.setEnabled(canEditWaveform);
    togglePlaylistSidebarButton.setEnabled(true);
    toggleMetadataSidebarButton.setEnabled(true);

    const auto projectCommandState = [this] (juce::Button& button,
                                              std::string_view commandId,
                                              bool projectsCheckedState = false)
    {
        const auto snapshot = commandRegistry.resolve(
            commandId,
            makeCommandContext(GuiCommandSource::toolbar, "toolbar-state"));
        if (!snapshot.has_value())
            return;

        button.setEnabled(snapshot->state.enabled);
        if (projectsCheckedState)
            button.setToggleState(snapshot->state.checked, juce::dontSendNotification);
        button.setDescription(snapshot->state.enabled
                                  ? juce::String()
                                  : juce::String::fromUTF8(snapshot->state.disabledReason.c_str()));
        auto help = juce::String::fromUTF8(snapshot->descriptor.shortcut.c_str());
        if (!snapshot->state.enabled && !snapshot->state.disabledReason.empty())
        {
            if (help.isNotEmpty())
                help += ". ";
            help += juce::String::fromUTF8(snapshot->state.disabledReason.c_str());
        }
        button.setHelpText(help);
    };

    projectCommandState(togglePlaylistSidebarButton, commandIds::togglePlaylistSidebar, true);
    projectCommandState(toggleMetadataSidebarButton, commandIds::toggleMetadataSidebar, true);
    projectCommandState(openButton, commandIds::open);
    projectCommandState(removeFileButton, commandIds::removePlaylist);
    projectCommandState(preferencesButton, commandIds::preferences);
    projectCommandState(saveButton, commandIds::save);
    projectCommandState(saveAsButton, commandIds::saveAs);
    projectCommandState(startButton, commandIds::start);
    projectCommandState(previousButton, commandIds::previousFrame);
    projectCommandState(nextButton, commandIds::nextFrame);
    projectCommandState(playButton, commandIds::play);
    projectCommandState(pauseButton, commandIds::pause);
    projectCommandState(stopButton, commandIds::stop);
    projectCommandState(returnToStartButton, commandIds::returnToStart, true);
    projectCommandState(loopButton, commandIds::loop, true);
    projectCommandState(autoPlayButton, commandIds::autoPlay, true);
    projectCommandState(generalSceneButton, commandIds::generalScene, true);
    projectCommandState(simpleViewButton, commandIds::simpleView, true);
    projectCommandState(fadeInButton, commandIds::fadeIn);
    projectCommandState(fadeOutButton, commandIds::fadeOut);
    projectCommandState(deleteSelectionButton, commandIds::deleteSelection);
    projectCommandState(undoButton, commandIds::undo);
    projectCommandState(redoButton, commandIds::redo);
    projectCommandState(resetEnvelopeButton, commandIds::resetEnvelope);
    projectCommandState(timeZoomOutButton, commandIds::zoomTimeOut);
    projectCommandState(timeZoomInButton, commandIds::zoomTimeIn);
    projectCommandState(ampZoomOutButton, commandIds::zoomAmplitudeOut);
    projectCommandState(ampZoomInButton, commandIds::zoomAmplitudeIn);
    projectCommandState(resetViewButton, commandIds::resetView);
    if (parametricEqButton != nullptr)
        projectCommandState(*parametricEqButton, commandIds::toggleParametricEq, true);
    if (truePeakLimiterButton != nullptr)
        projectCommandState(*truePeakLimiterButton,
                            commandIds::toggleTruePeakLimiter,
                            true);

    const auto waveformModeState = commandRegistry.resolve(
        commandIds::waveform,
        makeCommandContext(GuiCommandSource::mag, "core.waveform"));
    const auto channelModeState = commandRegistry.resolve(
        commandIds::stacked,
        makeCommandContext(GuiCommandSource::mag, "core.waveform"));
    waveformView.setCommandAvailability(waveformModeState.has_value() && waveformModeState->state.enabled,
                                        channelModeState.has_value() && channelModeState->state.enabled);
}

void MainComponent::refreshBnkControlSurface(bool syncMediaSelection)
{
    ensureBnkControls();
    if (!bnkControls)
    {
        if (bnkRuntime) bnkRuntime->setContext({});
        return;
    }
    refreshAdmittedBnkControls(syncMediaSelection);
}

void MainComponent::refreshAdmittedBnkControls(bool syncMediaSelection)
{
    const auto showingBank = bnkDocument.hasBank() && !loadInProgress && !modernSimpleView;
    const auto selectedEventId = bnkDocument.getSelectedEventId();
    auto context = buildBnkPlaybackContext();

    suppressBnkControlCallbacks = true;

    bnkSwitchTokens.clear();
    bnkControls->switchBox.clear(juce::dontSendNotification);
    const auto switchOptions = bnkDocument.getSwitchValuesForSelectedEvent();
    if (!switchOptions.empty())
    {
        auto labelText = switchOptions.front().groupName.isNotEmpty() ? switchOptions.front().groupName : "Switch";
        labelText = labelText.substring(0, 1).toUpperCase() + labelText.substring(1);
        bnkControls->switchLabel.setText(labelText, juce::dontSendNotification);

        auto selectedIndex = -1;
        auto defaultIndex = 0;
        const auto selectedEventState = bnkDocument.getState();
        const auto currentEventId = bnkDocument.getSelectedEventId();
        const BnkDocument::EventDescriptor* selectedEvent = nullptr;
        if (selectedEventState != nullptr && currentEventId.has_value())
        {
            const auto found = std::find_if(selectedEventState->events.begin(),
                                            selectedEventState->events.end(),
                                            [currentEventId](const BnkDocument::EventDescriptor& event)
                                            {
                                                return event.id == *currentEventId;
                                            });
            if (found != selectedEventState->events.end())
                selectedEvent = &(*found);
        }

        for (int index = 0; index < static_cast<int>(switchOptions.size()); ++index)
        {
            const auto& option = switchOptions[static_cast<std::size_t>(index)];
            bnkControls->switchBox.addItem(option.name, index + 1);
            bnkSwitchTokens.push_back(option.token);

            const auto preferredToken = bnkRuntime->preferredSwitch(selectedEventId);

            if (option.token == preferredToken)
                selectedIndex = index;

            if (selectedEvent != nullptr)
            {
                const auto mediaFound = std::find_if(selectedEvent->media.begin(),
                                                     selectedEvent->media.end(),
                                                     [&option](const BnkDocument::EventDescriptor::MediaReference& media)
                                                     {
                                                         return media.switchValueToken == option.token && media.isDefaultSwitch;
                                                     });
                if (mediaFound != selectedEvent->media.end())
                    defaultIndex = index;
            }
        }

        if (selectedIndex < 0)
            selectedIndex = defaultIndex;

        if (juce::isPositiveAndBelow(selectedIndex, static_cast<int>(bnkSwitchTokens.size())))
            context.switchValueToken = bnkSwitchTokens[static_cast<std::size_t>(selectedIndex)];
        else
            context.switchValueToken.clear();

        bnkControls->switchBox.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
        bnkControls->switchBox.setEnabled(true);
        bnkControls->switchLabel.setVisible(true);
        bnkControls->switchBox.setVisible(true);
    }
    else
    {
        bnkControls->switchLabel.setText("Switch", juce::dontSendNotification);
        bnkControls->switchLabel.setVisible(false);
        bnkControls->switchBox.setVisible(false);
        context.switchValueToken.clear();
    }

    bnkStateTokens.clear();
    bnkControls->stateBox.clear(juce::dontSendNotification);
    const auto stateOptions = bnkDocument.getAvailableStateValues();
    if (!stateOptions.empty())
    {
        const auto multipleGroups = std::any_of(stateOptions.begin(),stateOptions.end(),[&](const auto& option)
            { return bnkstate::group(option)!=bnkstate::group(stateOptions.front()); });
        std::unordered_map<std::string, juce::String> groupNames;
        std::unordered_set<std::string> ambiguousGroupNames;
        for (const auto& option : stateOptions)
        {
            const auto nameKey = bnkstate::name(option.groupName).toStdString();
            const auto [entry, added] = groupNames.emplace(nameKey,bnkstate::group(option));
            if (!added && entry->second!=bnkstate::group(option)) ambiguousGroupNames.insert(nameKey);
        }
        auto labelText = !multipleGroups && stateOptions.front().groupName.isNotEmpty() ? stateOptions.front().groupName : "State";
        labelText = labelText.substring(0, 1).toUpperCase() + labelText.substring(1);
        bnkControls->stateLabel.setText(labelText, juce::dontSendNotification);
        auto selectedIndex = -1;
        auto defaultIndex = 0;
        for (int index = 0; index < static_cast<int>(stateOptions.size()); ++index)
        {
            const auto& option = stateOptions[static_cast<std::size_t>(index)];
            const auto optionToken = bnkstate::token(option);
            auto optionLabel = multipleGroups ? option.groupName + ": " + option.name : option.name;
            if (ambiguousGroupNames.count(bnkstate::name(option.groupName).toStdString()))
                optionLabel += " [" + (option.groupToken.isNotEmpty() ? bnkstate::id(option.groupToken) : juce::String(option.groupId)) + "]";
            bnkControls->stateBox.addItem(optionLabel, index + 1);
            bnkStateTokens.push_back(optionToken);
            const auto preferredToken = bnkRuntime->preferredState(selectedEventId);

            if (optionToken == preferredToken)
                selectedIndex = index;
            if (option.isDefault)
                defaultIndex = index;
        }
        if (selectedIndex < 0)
            selectedIndex = defaultIndex;

        if (juce::isPositiveAndBelow(selectedIndex, static_cast<int>(bnkStateTokens.size())))
            context.stateValueToken = bnkStateTokens[static_cast<std::size_t>(selectedIndex)];
        else
            context.stateValueToken.clear();

        bnkControls->stateBox.setSelectedItemIndex(selectedIndex, juce::dontSendNotification);
        bnkControls->stateBox.setTooltip(bnkControls->stateBox.getItemText(selectedIndex));
        bnkControls->stateBox.setEnabled(true);
        bnkControls->stateLabel.setVisible(true);
        bnkControls->stateBox.setVisible(true);
    }
    else
    {
        bnkControls->stateLabel.setText("State", juce::dontSendNotification);
        bnkControls->stateLabel.setVisible(false);
        bnkControls->stateBox.setVisible(false);
        context.stateValueToken.clear();
    }

    const auto hasRtpc = bnkDocument.hasRtpcMetadataForSelectedEvent();
    context.rtpcToken.clear(); context.rtpcValue = 0.0f;
    bnkRuntime->setContext(context, selectedEventId);
    bnkControls->rtpcLabel.setVisible(false);
    bnkControls->rtpcBox.setVisible(false);
    bnkControls->rtpcSlider.setVisible(false);
    bnkControls->rtpcValueLabel.setVisible(false);
    bnkControls->audioFilter.setToggleState(bnkAudioLinkedOnly, juce::dontSendNotification);
    bnkControls->audioFilter.setVisible(showingBank);
    if (bnkAudioListView != nullptr)
    {
        bnkAudioListView->setLinkedOnly(bnkAudioLinkedOnly);
        bnkAudioListView->setSwitchTokenFilter(context.switchValueToken);
    }

    suppressBnkControlCallbacks = false;
    if (syncMediaSelection) syncBnkAudioSelectionForCurrentContext();
    bnkControls->surface.setVisible(showingBank && !deferChromeForStartup);
    if (metadataSidebar) metadataSidebar->setBnkPreviewContext(bnkControls->switchBox.isVisible() ? bnkControls->switchLabel.getText() + ": " + bnkControls->switchBox.getText() : juce::String(),
                                         context.switchValueToken,
                                         bnkControls->stateBox.isVisible() ? bnkControls->stateLabel.getText() + ": " + bnkControls->stateBox.getText() : juce::String(),
                                         hasRtpc ? "Present (native Wwise runtime required)" : juce::String());
    resized();
}

void MainComponent::refreshBnkRtpcSelectionUi()
{
    if (!bnkControls) return;
    bnkControls->rtpcLabel.setVisible(false);
    bnkControls->rtpcBox.setVisible(false);
    bnkControls->rtpcSlider.setVisible(false);
    bnkControls->rtpcValueLabel.setVisible(false);
}

BnkPlaybackContext MainComponent::buildBnkPlaybackContext() const
{
    return bnkRuntime ? bnkRuntime->context() : BnkPlaybackContext{};
}

bool MainComponent::shouldAutoPlayBnkContextChange() const
{
    return autoPlayButton.getToggleState() || audioEngine.isPlaying();
}

void MainComponent::syncBnkAudioSelectionForCurrentContext()
{
    if (!bnkDocument.hasBank() || bnkCenterMode != BnkCenterMode::audio)
        return;

    const auto linkedMediaIds = bnkDocument.getSelectedEventMediaIds(buildBnkPlaybackContext().switchValueToken);
    if (linkedMediaIds.empty())
    {
        // The list shows all raw media when no event links exist. Keep its
        // visible selection, including media-only banks and retained returns.
        return;
    }

    if (!bnkAudioLinkedOnly)
        return;

    const auto selectedMediaId = bnkDocument.getSelectedMediaId();
    if (selectedMediaId.has_value()
        && std::find(linkedMediaIds.begin(), linkedMediaIds.end(), *selectedMediaId) != linkedMediaIds.end())
    {
        return;
    }

    bnkDocument.selectMediaById(linkedMediaIds.front());
}

void MainComponent::updateCenterViewVisibility()
{
    const auto showingBank = bnkDocument.hasBank() && !loadInProgress && !modernSimpleView;
    if (showingBank)
        ensureBnkUiComponents();
    else if (metadataSidebar)
        metadataSidebar->setBnkPreviewContext({}, {}, {}, {});

    if (bnkControls)
    {
        bnkControls->eventsTab.setToggleState(showingBank && bnkCenterMode == BnkCenterMode::events, juce::dontSendNotification);
        bnkControls->audioTab.setToggleState(showingBank && bnkCenterMode == BnkCenterMode::audio, juce::dontSendNotification);
    }
    waveformView.setVisible(!showingBank);
    if (bnkEventListView != nullptr)
        bnkEventListView->setVisible(showingBank && bnkCenterMode == BnkCenterMode::events);
    if (bnkAudioListView != nullptr)
    {
        bnkAudioListView->setLinkedOnly(bnkAudioLinkedOnly);
        bnkAudioListView->setSwitchTokenFilter(buildBnkPlaybackContext().switchValueToken);
        bnkAudioListView->setVisible(showingBank && bnkCenterMode == BnkCenterMode::audio);
    }
    if (bnkControls) bnkControls->surface.setVisible(showingBank);

    if (showingBank)
    {
        if (bnkCenterMode == BnkCenterMode::audio)
        {
            if (bnkAudioListView != nullptr)
                bnkAudioListView->toFront(false);
        }
        else
        {
            if (bnkEventListView != nullptr)
                bnkEventListView->toFront(false);
        }
    }
}

void MainComponent::prepareSelectedBnkEvent(bool autoPlay)
{
    if (!bnkDocument.hasBank()) return;
    const auto bank = bnkDocument.getState();
    if (!bank || bank->events.empty()) return;
    auto selected = bnkDocument.getSelectedEventId();
    if (!selected) { selected = bank->events.front().id; bnkDocument.selectEventById(selected); }
    // List activation is synchronous; the ordinary chrome refresh can occur
    // later. Freeze the selected event's controls now, without changing its
    // event/media selection, so the request never carries the prior event's State.
    refreshBnkControlSurface(false);
    beginBnkPreview(BnkPreviewRequest::forEvent(bank, *selected, buildBnkPlaybackContext(), getBnkMediaResolveMode()), autoPlay);
}

void MainComponent::prepareSelectedBnkMedia(uint32_t mediaId, bool autoPlay)
{
    if (!bnkDocument.hasBank() || mediaId == 0) return;
    const auto bank = bnkDocument.getState();
    if (!bank) return;
    const auto media = bnkDocument.getPlayableMedia();
    const auto found = std::find_if(media.begin(), media.end(), [mediaId](const auto& entry) { return entry.mediaId == mediaId; });
    if (found == media.end()) return;
    bnkDocument.selectMediaById(mediaId);
    beginBnkPreview(BnkPreviewRequest::forMedia(bank, *found, getBnkMediaResolveMode()), autoPlay);
}

bool MainComponent::bnkPreviewWorkspaceMatches(const BnkRuntimeSession::PreviewIntent& intent) const
{
    const auto* entry = playlistModel.getCurrentEntry();
    if ((entry ? std::optional<juce::Uuid>(entry->id) : std::nullopt) != intent.previousEntry
        || currentSelection != intent.previousSelection) return false;
    if (intent.previousWorkspace)
    {
        const auto current = document.captureWorkspaceSnapshot();
        return current && intent.previousWorkspace->sameContents(*current);
    }
    // A refused snapshot during a gesture/preview is not an empty workspace.
    return intent.previousWasEmpty && document.getState() == intent.previousState
        && !document.getFileInfo().isValid();
}

void MainComponent::beginBnkPreview(BnkPreviewRequest::Ptr request, bool autoPlay,
    media::OptionalMediaFailureState::Intent action, std::optional<media::OptionalMediaFailureState::Key> reviewed)
{
    if (!request || closeApplicationPending) return;
    auto proceed = [safe = juce::Component::SafePointer<MainComponent>(this), request = std::move(request),
                    autoPlay, action, reviewed = std::move(reviewed)]() mutable
    {
        if (safe) safe->queueBnkPreview(std::move(request), autoPlay, action, std::move(reviewed));
    };
    if (document.hasUnsavedChanges()) confirmDiscardOrSaveIfNeeded("previewing different BNK audio", std::move(proceed));
    else proceed();
}

void MainComponent::queueBnkPreview(BnkPreviewRequest::Ptr request, bool autoPlay,
    media::OptionalMediaFailureState::Intent action, std::optional<media::OptionalMediaFailureState::Key> reviewed)
{
    if (closeApplicationPending || !request || request->bankState() != bnkDocument.getState()
        || (request->eventId() && request->eventId() != bnkDocument.getSelectedEventId())
        || (request->mediaId() && request->mediaId() != bnkDocument.getSelectedMediaId())) return;
    ensureBnkRuntime();
    if (!bnkRuntime) return;
    cancelBnkOpen();
    cancelPackedAudioLoad();
    if (!optionalFailureSession) optionalFailureSession = std::make_shared<media::OptionalMediaFailureState>();

    BnkRuntimeSession::PreviewIntent intent;
    intent.request = std::move(request);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext operationContext(optionalLifecycleContext);
#endif
    intent.operation = std::make_shared<media::OptionalMediaOperation>(optionalFailureSession, action, std::move(reviewed),
        media::OptionalMediaFailureState::Module::bank);
    lastOptionalDiagnostic = intent.operation->diagnosticHandle();
    intent.previousState = document.getState();
    intent.previousWorkspace = document.captureWorkspaceSnapshot();
    intent.previousWasEmpty = !document.getFileInfo().isValid() && intent.previousState && !intent.previousState->hasPlaybackSource();
    intent.previousSelection = currentSelection;
    if (const auto* entry = playlistModel.getCurrentEntry()) intent.previousEntry = entry->id;
    intent.event = bnkDocument.getSelectedEventId(); intent.media = bnkDocument.getSelectedMediaId();
    intent.autoPlay = autoPlay && action == media::OptionalMediaFailureState::Intent::initial;
    if (!bnkRuntime->selectionMatches(intent, getBnkMediaResolveMode())) { intent.operation->cancelled(); return; }
    if (pendingVideoOperation) pendingVideoOperation->requestCancel();
    if (videoProgressWindow) videoProgressWindow->exitModalState(0);
    if (bankProgressWindow) bankProgressWindow->exitModalState(0);
    intent.token = pendingLoadToken.fetch_add(1, std::memory_order_relaxed) + 1;
    bankProgressToken = intent.token;
    const auto operation = intent.operation;
    bnkRuntime->queue(std::move(intent));
    pendingLoadLabel = "BNK preview"; loadInProgress = true;
    pendingAutoPlayOnLoad = false; pendingAutoPlayFile = {};
    auto* progress = new juce::AlertWindow("Preparing BNK preview",
        "Preparing the complete selected audio. Current audio and its history are retained.",
        juce::MessageBoxIconType::NoIcon, this);
    progress->addButton("Cancel BNK preview", 1, juce::KeyPress(juce::KeyPress::escapeKey));
    bankProgressWindow = progress;
    progress->enterModalState(true, juce::ModalCallbackFunction::create(
        [operation](int choice) { if (choice == 1) operation->requestCancel(); }), true);
    refreshUiState();
    pollBnkPreview();
}

void MainComponent::cancelBnkPreview()
{
    const auto currentToken = pendingLoadToken.load(std::memory_order_relaxed);
    const bool ownsCurrentLoad = bnkRuntime && bnkRuntime->cancel(currentToken);
    if (bankProgressWindow) bankProgressWindow->exitModalState(0);
    // Finished-but-unhandled work may be removed above, so no later poll can
    // clear its loading flag. Do not clear a newer audio request's status.
    if (ownsCurrentLoad) { loadInProgress = false; pendingLoadLabel.clear(); }
}

void MainComponent::pollBnkPreview()
{
    using S = media::OptionalMediaFailureState;
    const auto runtime = bnkRuntime;
    if (!runtime) return;
    runtime->poll([this, runtime](const BnkRuntimeSession::PreviewIntent& intent, bool starting)
    {
        return runtime == bnkRuntime && runtime->generation() == activeDocumentGeneration.value
            && !closeApplicationPending && intent.token == pendingLoadToken.load(std::memory_order_relaxed)
            && runtime->selectionMatches(intent, getBnkMediaResolveMode())
            && (!starting || bnkPreviewWorkspaceMatches(intent));
    }, [this, runtime](const BnkRuntimeSession::PreviewIntent& intent, BnkPreparedPlayback&& prepared, bool wasPrepared)
    {
        const bool current = intent.token == pendingLoadToken.load(std::memory_order_relaxed);
        if (bankProgressWindow && bankProgressToken == intent.token) bankProgressWindow->exitModalState(0);
        if (current) { loadInProgress = false; pendingLoadLabel.clear(); }
        if (!wasPrepared) { if (current) refreshUiState(); return; }
        const auto operation = intent.operation;
        const auto report = operation->report();
        const bool cancelled = report.cancelled || operation->cancelFlag().load(std::memory_order_acquire);
        const bool eligible = current && !closeApplicationPending && runtime == bnkRuntime
            && runtime->generation() == activeDocumentGeneration.value
            && runtime->selectionMatches(intent, getBnkMediaResolveMode()) && bnkPreviewWorkspaceMatches(intent);
        if (!cancelled && eligible && !report.terminal && prepared.isValid() && bnkDocument.getState()->isSourceCurrent())
        {
            audioEngine.stop();
            if (document.adoptLoadedState(std::move(prepared.state)))
            {
                operation->succeed();
                currentSelection.reset();
                clearReferenceAudioSuitePreview(true);
                waveformView.clearSelection(); waveformView.clearSelectionPreviewGain();
                if (prepared.detail.isNotEmpty()) engineStatusLabel.setText(prepared.detail, juce::dontSendNotification);
                refreshUiState(); if (metadataSidebar) metadataSidebar->repaint();
                if (intent.autoPlay) audioEngine.play();
            }
            else operation->fail(S::Reason::cacheInvalid, S::Stage::admission);
        }
        else if (!report.terminal && !cancelled && eligible)
            operation->fail(S::Reason::ioFailure, S::Stage::admission);
        else if (!report.terminal) operation->cancelled();

        if (current && !operation->report().succeeded)
        {
            setDocumentOperationStatus(eligible ? "BNK preview ended; current audio and complete history were kept."
                : "BNK preview ended; changes made while it was preparing were kept.");
            refreshUiState();
        }
        if (current && eligible && !closeApplicationPending && !operation->report().cancelled
            && !operation->report().succeeded) showBnkPreviewFailure();
        if (closeApplicationPending) { continueCloseApplication(); return; }
    });
}

void MainComponent::showBnkPreviewFailure()
{
    const auto* intent = bnkRuntime ? bnkRuntime->lastPreview() : nullptr;
    if (!intent) return;
    using S = media::OptionalMediaFailureState;
    const auto operation = intent->operation;
    const auto request = intent->request;
    const auto report = operation->report(); const auto view = operation->policyView();
    juce::String reason = "This BNK preview was declined by the session policy.";
    if (report.failure)
    {
        switch (report.failure->reason)
        {
            case S::Reason::deadline: reason = "The complete preview exceeded its time budget."; break;
            case S::Reason::outputQuota: reason = "Decoded output exceeded its size budget."; break;
            case S::Reason::memoryQuota: reason = "The preview exceeded its memory budget."; break;
            case S::Reason::parserBounds: reason = "The selected event or its members do not fit the supported preview limits."; break;
            case S::Reason::missingDependency: reason = "vgmstream is unavailable. Select a supported local runtime before retrying."; break;
            case S::Reason::decoderExit: reason = "A required decoder exited unsuccessfully (code " + juce::String(report.failure->exitCode) + ")."; break;
            case S::Reason::cacheInvalid: reason = "A required audio member or its decoded cache could not be validated."; break;
            case S::Reason::containment: reason = "Decoder containment or cleanup could not be confirmed."; break;
            default: reason = "The bank or a required source could not be accessed with its original identity."; break;
        }
    }
    if (view.circuitOpen) reason += "\nTwo matching failures have disabled this policy for this application session.";
    if (report.denied == S::Denied::invalidKey) reason += "\nThe selection or a dependency changed; select it again as a fresh request.";
    reason += "\nCurrent audio and complete Undo/Redo history were kept. No partial event or automatic retry was published.";
    if (view.expansionAvailable) reason += "\nExpanded Budget permits up to 2 minutes, 4 GiB decoder memory and 4 GiB output. Preview member and PCM limits stay fixed.";
    auto* alert = new juce::AlertWindow("BNK preview unavailable", reason, juce::MessageBoxIconType::WarningIcon, this);
    if (view.retryAvailable && report.key) alert->addButton("Retry once", 1);
    if (view.expansionAvailable && report.key) alert->addButton("Retry with Expanded Budget", 2);
    alert->addButton("Export diagnostics...", 3);
    alert->addButton("Keep current audio", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safe = juce::Component::SafePointer<MainComponent>(this), operation, request, key = report.key](int choice)
        {
            if (!safe || !safe->bnkRuntime || !safe->bnkRuntime->lastPreview()
                || safe->bnkRuntime->lastPreview()->operation != operation) return;
            if (choice == 3) { safe->exportVideoAudioDiagnostics(operation); return; }
            if ((choice == 1 || choice == 2) && key)
                safe->beginBnkPreview(request, false, choice == 1 ? S::Intent::retryOnce : S::Intent::expandBudget, key);
        }), true);
}

void MainComponent::togglePlaylistSidebar()
{
    if (workspaceModuleHost != nullptr)
    {
        workspaceModuleHost->setModuleEnabled(WorkspaceModuleId::playlist,
            !workspaceModuleHost->isModuleActive(WorkspaceModuleId::playlist));
        return;
    }
    playlistSidebarCollapsed = !playlistSidebarCollapsed;
    updateSidebarToggleButtons();
    persistSessionSettings();
    resized();
    repaint();
}

void MainComponent::toggleMetadataSidebar()
{
    if (workspaceModuleHost != nullptr)
    {
        workspaceModuleHost->setModuleEnabled(WorkspaceModuleId::metadata,
            !workspaceModuleHost->isModuleActive(WorkspaceModuleId::metadata));
        return;
    }
    metadataSidebarCollapsed = !metadataSidebarCollapsed;
    updateSidebarToggleButtons();
    persistSessionSettings();
    resized();
    repaint();
}

void MainComponent::handleSelectionChanged(std::optional<juce::Range<double>> newSelection)
{
    const auto previousSelection = currentSelection;
    currentSelection = std::move(newSelection);

    const auto selectionChanged = [previousSelection, this]() -> bool
    {
        if (!previousSelection.has_value() && !currentSelection.has_value())
            return false;
        if (previousSelection.has_value() != currentSelection.has_value())
            return true;

        return std::abs(previousSelection->getStart() - currentSelection->getStart()) > 0.0001
            || std::abs(previousSelection->getEnd() - currentSelection->getEnd()) > 0.0001;
    }();

    if (selectionChanged)
    {
        selectionPopupDismissed = false;
        selectionGainSlider.setValue(0.0, juce::dontSendNotification);
        selectionGainLabel.setText("0.0 dB", juce::dontSendNotification);
        applySelectionGainButton.setButtonText("Apply");
        applySelectionGainButton.setTooltip("Apply selection gain through AudioSuite");
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
    }

    if (!currentSelection.has_value())
    {
        selectedSourceChannels.clear();
        waveformView.setSelectedSourceChannels({});
    }

    if (currentSelection.has_value() && std::abs(selectionGainSlider.getValue()) > 0.01)
    {
        const auto linear = juce::Decibels::decibelsToGain(static_cast<float>(selectionGainSlider.getValue()));
        updateReferenceAudioSuitePreview();
        waveformView.setSelectionPreviewGain(linear);
    }
    else
    {
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
    }

    if (selectionChanged && parametricEqPreview.isValid())
        updateParametricEqPreview();
    if (selectionChanged && truePeakLimiterPreview.isValid())
        updateTruePeakLimiterPreview();

    refreshFooterReadout();
    updateTransportAvailability();
    updateSelectionPopup();
}

void MainComponent::handleSourceChannelSelectionGesture(int sourceChannel,
                                                        bool extendSelection)
{
    const auto state = document.getState();
    if (state == nullptr
        || !juce::isPositiveAndBelow(sourceChannel, state->info.numChannels))
    {
        return;
    }

    if (!extendSelection)
        selectedSourceChannels.clear();

    const auto identity = SourceChannelIdentity::fromSourceIndex(sourceChannel);
    if (std::find(selectedSourceChannels.begin(),
                  selectedSourceChannels.end(),
                  identity) == selectedSourceChannels.end())
    {
        selectedSourceChannels.push_back(identity);
    }

    std::vector<int> sourceIndices;
    sourceIndices.reserve(selectedSourceChannels.size());
    for (const auto& selected : selectedSourceChannels)
        sourceIndices.push_back(selected.sourceChannelIndex);
    waveformView.setSelectedSourceChannels(std::move(sourceIndices));
    if (parametricEqPreview.isValid())
        updateParametricEqPreview();
    if (truePeakLimiterPreview.isValid())
        updateTruePeakLimiterPreview();
}

AudioSelectionRequest MainComponent::buildAudioSuiteSelectionRequest() const
{
    AudioSelectionRequest request;
    request.timeRangeSeconds = currentSelection;
    request.sourceChannels = selectedSourceChannels;
    return request;
}

AudioSuiteReferenceProcessor* MainComponent::ensureReferenceAudioSuiteProcessor()
{
    if (referenceAudioSuiteProcessor != nullptr)
        return referenceAudioSuiteProcessor.get();
    if (!coreLifecycle.isCurrent(activeDocumentGeneration)
        || !coreLifecycle.isCoreReady(activeDocumentGeneration))
    {
        return nullptr;
    }

    const auto module = OptionalModuleId::dsp;
    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.describePlacement(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration, module)))
    {
        coreLifecycle.release(activeDocumentGeneration, module);
        return nullptr;
    }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext factoryContext(optionalLifecycleContext);
#endif
    referenceAudioSuiteProcessor = AudioSuiteReferenceProcessor::describe().create();
    if (referenceAudioSuiteProcessor == nullptr
        || !lifecycleTransitionAccepted(coreLifecycle.activate(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.attachView(activeDocumentGeneration, module)))
    {
        referenceAudioSuiteProcessor.reset();
        coreLifecycle.release(activeDocumentGeneration, module);
        return nullptr;
    }
    return referenceAudioSuiteProcessor.get();
}

void MainComponent::updateReferenceAudioSuitePreview()
{
    clearTruePeakLimiterPreview(true);
    clearParametricEqPreview(true);
    clearReferenceAudioSuitePreview(false);

    if (!currentSelection.has_value()
        || currentSelection->isEmpty()
        || std::abs(selectionGainSlider.getValue()) <= 0.01)
    {
        clearReferenceAudioSuitePreview(true);
        return;
    }

    auto* processor = ensureReferenceAudioSuiteProcessor();
    if (processor == nullptr)
    {
        clearReferenceAudioSuitePreview(true);
        return;
    }

    const auto linearGain = juce::Decibels::decibelsToGain(
        static_cast<float>(selectionGainSlider.getValue()));
    const auto analyzed = processor->analyze(document.getState(),
                                             activeDocumentGeneration.value,
                                             buildAudioSuiteSelectionRequest(),
                                             linearGain);
    if (!analyzed.succeeded())
    {
        clearReferenceAudioSuitePreview(true);
        return;
    }

    auto preview = processor->createPreview(analyzed.plan);
    if (!preview.isValid())
    {
        clearReferenceAudioSuitePreview(true);
        return;
    }

    activeReferenceAudioSuitePlan = analyzed.plan;
    referenceAudioSuitePreview = preview;
    audioEngine.setAudioSuitePreview(preview.getState());

    const auto state = document.getState();
    if (state != nullptr && state->info.sampleRate > 0.0)
    {
        const auto previewRange = preview.getPlaybackRange();
        const auto currentSample = static_cast<int64_t>(
            std::floor(audioEngine.getCurrentPositionSeconds() * state->info.sampleRate));
        if (!previewRange.contains(currentSample))
        {
            audioEngine.seekToSeconds(
                static_cast<double>(previewRange.getStart()) / state->info.sampleRate);
        }
        if (!loopBeforeReferenceAudioSuitePreview.has_value())
            loopBeforeReferenceAudioSuitePreview = audioEngine.isLoopEnabled();
        audioEngine.setLoopEnabled(true);
    }
}

void MainComponent::clearReferenceAudioSuitePreview(bool releaseProcessor)
{
    referenceAudioSuitePreview.cancel();
    referenceAudioSuitePreview = {};
    activeReferenceAudioSuitePlan.reset();
    audioEngine.clearAudioSuitePreview();
    if (loopBeforeReferenceAudioSuitePreview.has_value())
    {
        audioEngine.setLoopEnabled(*loopBeforeReferenceAudioSuitePreview);
        loopBeforeReferenceAudioSuitePreview.reset();
    }

    if (releaseProcessor && !referenceAudioSuiteApplyInFlight)
    {
        referenceAudioSuiteProcessor.reset();
        releaseDspLifecycleIfIdle();
    }
}

void MainComponent::dismissSelectionPopup()
{
    if (referenceAudioSuiteApplyInFlight)
        return;

    const auto popupHadFocus = selectionPopup.hasKeyboardFocus(true);
    selectionGainSlider.setValue(0.0, juce::sendNotificationSync);
    clearReferenceAudioSuitePreview(true);
    waveformView.clearSelectionPreviewGain();
    selectionPopupDismissed = true;
    updateSelectionPopup();
    if (popupHadFocus && waveformView.isShowing())
        waveformView.grabKeyboardFocus();
}

bool MainComponent::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    // The waveform and gain controls no longer bubble through MainComponent
    // while floating. Keep their local Escape transaction behavior identical;
    // all other keys retain the existing component/native-window routing.
    return key == juce::KeyPress::escapeKey && keyPressed(key);
}

void MainComponent::componentMovedOrResized(juce::Component& component, bool, bool)
{
    if (&component == &waveformView)
        updateSelectionPopup();
}

void MainComponent::componentParentHierarchyChanged(juce::Component& component)
{
    if (&component == &waveformView)
        updateSelectionPopup();
}

void MainComponent::componentVisibilityChanged(juce::Component& component)
{
    if (&component == &waveformView)
        updateSelectionPopup();
}

void MainComponent::updateSelectionPopup()
{
    // Keep the same gain controls beside the borrowed waveform in either native
    // window. Sampling the waveform remains acyclic: the popup is its sibling,
    // never a child included in the waveform's snapshot.
    auto* popupParent = waveformView.getParentComponent();
    if (selectionPopup.getParentComponent() != popupParent)
    {
        selectionPopup.setEffectsEnabled(false);
        if (auto* previous = selectionPopup.getParentComponent())
            previous->removeChildComponent(&selectionPopup);
        if (popupParent != nullptr)
            popupParent->addChildComponent(selectionPopup);
    }

    if (deferChromeForStartup || popupParent == nullptr || !waveformView.isVisible()
        || selectionPopupDismissed)
    {
        selectionPopup.setEffectsEnabled(false);
        selectionPopup.setVisible(false);
        selectionGainLabel.setVisible(false);
        selectionGainSlider.setVisible(false);
        applySelectionGainButton.setVisible(false);
        cancelSelectionGainButton.setVisible(false);
        gainDownButton.setVisible(false);
        gainUpButton.setVisible(false);
        return;
    }

    const auto hasSelection = currentSelection.has_value() && currentSelection->getLength() > 0.0;
    selectionPopup.setVisible(hasSelection);
    selectionGainLabel.setVisible(hasSelection);
    selectionGainSlider.setVisible(hasSelection);
    applySelectionGainButton.setVisible(hasSelection);
    cancelSelectionGainButton.setVisible(hasSelection);
    gainDownButton.setVisible(hasSelection);
    gainUpButton.setVisible(hasSelection);

    selectionPopup.setEffectsEnabled(hasSelection && coreLifecycle.isCoreReady(activeDocumentGeneration));
    if (!hasSelection)
        return;

    selectionPopup.toFront(false);

    const auto waveformBounds = waveformView.getBounds();
    // Layout and clipping use the current waveform host, including floating
    // resize/Tab changes; converting through MainComponent crosses native peers.
    auto available = waveformBounds.reduced(12).getIntersection(popupParent->getLocalBounds().reduced(8));
    if (available.getWidth() < 140 || available.getHeight() < 112)
        available = popupParent->getLocalBounds().reduced(8);
    if (available.getWidth() < 140 || available.getHeight() < 112)
    {
        selectionPopup.setEffectsEnabled(false);
        selectionPopup.setVisible(false);
        return;
    }
    const auto popupWidth = juce::jmin(422, available.getWidth());
    const auto layout = SelectionPopupLayout::calculate(popupWidth);
    auto popupBounds = juce::Rectangle<int>(0, 0, popupWidth, layout.height);
    if (const auto selectionBounds = waveformView.getSelectionBoundsInLocal())
    {
        auto inParent = popupParent->getLocalArea(&waveformView, selectionBounds->toNearestInt());
        const auto targetX = inParent.getCentreX() - popupBounds.getWidth() / 2;
        const auto targetY = juce::jmax(waveformBounds.getY() + 14,
            inParent.getY() + (RendererVisualStyle::isClassic() ? 10 : 34));
        popupBounds.setPosition(targetX, targetY);
    }
    else
    {
        popupBounds.setPosition(waveformBounds.getX() + 20, waveformBounds.getY() + 20);
    }

    popupBounds.setPosition(juce::jlimit(available.getX(), juce::jmax(available.getX(), available.getRight() - popupWidth), popupBounds.getX()),
                            juce::jlimit(available.getY(), juce::jmax(available.getY(), available.getBottom() - layout.height), popupBounds.getY()));

    selectionPopup.setBounds(popupBounds);
    selectionGainLabel.setBounds(layout.label);
    if (!RendererVisualStyle::isClassic())
    {
        selectionGainLabel.setFont(ModernTypography::font(11.0f));
        selectionGainLabel.setBorderSize(juce::BorderSize<int>(0, 2, 0, 2));
        selectionGainLabel.setMinimumHorizontalScale(0.9f);
    }
    gainDownButton.setBounds(layout.down);
    gainUpButton.setBounds(layout.up);
    selectionGainSlider.setBounds(layout.slider);
    applySelectionGainButton.setBounds(layout.apply);
    cancelSelectionGainButton.setBounds(layout.cancel);
    selectionPopup.invalidateBackdrop();
}

void MainComponent::applySelectionGain()
{
    if (!currentSelection.has_value()
        || referenceAudioSuiteApplyInFlight
        || parametricEqApplyInFlight
        || truePeakLimiterApplyInFlight)
        return;

    updateReferenceAudioSuitePreview();
    const auto plan = activeReferenceAudioSuitePlan;
    if (plan == nullptr)
        return;

    const auto applyGeneration = activeDocumentGeneration.value;
    clearReferenceAudioSuitePreview(false);
    referenceAudioSuiteApplyInFlight = true;
    applySelectionGainButton.setButtonText("Applying...");
    selectionGainLabel.setText("Rendering", juce::dontSendNotification);
    updateTransportAvailability();

    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto taskLifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::referenceGain, OptionalLifecycleResourceKind::applyTask);
    try
    {
#endif
    referenceAudioSuiteApplyThread = std::make_unique<ReferenceAudioSuiteApplyThread>(
        document,
        plan,
        applyGeneration,
        [safeThis, plan, applyGeneration](AudioSuiteReferenceProcessor::ApplyResult result) mutable
        {
            if (safeThis != nullptr)
                safeThis->finishReferenceAudioSuiteApply(result,
                                                         std::move(plan),
                                                         applyGeneration);
        });
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    }
    catch (...) { taskLifetime.failed(); throw; }
    taskLifetime.created();
    referenceAudioSuiteApplyThread->observeLifetime(std::move(taskLifetime));
#endif
    if (!referenceAudioSuiteApplyThread->startThread())
    {
        referenceAudioSuiteApplyThread.reset();
        referenceAudioSuiteApplyInFlight = false;
        applySelectionGainButton.setButtonText("Retry");
        selectionGainLabel.setText(juce::String(selectionGainSlider.getValue(), 1) + " dB",
                                   juce::dontSendNotification);
        updateTransportAvailability();
    }
}

void MainComponent::finishReferenceAudioSuiteApply(
    AudioSuiteReferenceProcessor::ApplyResult result,
    AudioSuitePlanPtr appliedPlan,
    std::uint64_t applyGeneration)
{
    referenceAudioSuiteApplyThread.reset();
    referenceAudioSuiteApplyInFlight = false;

    const auto sameGeneration = activeDocumentGeneration.value == applyGeneration
                             && coreLifecycle.isCurrent(activeDocumentGeneration);
    if (!sameGeneration)
    {
        referenceAudioSuiteProcessor.reset();
        applySelectionGainButton.setButtonText("Apply");
        selectionGainLabel.setText("0.0 dB", juce::dontSendNotification);
        clearReferenceAudioSuitePreview(true);
        updateTransportAvailability();
        return;
    }

    if (result.succeeded() && sameGeneration)
    {
        lastAppliedReferenceAudioSuitePlan = std::move(appliedPlan);
        selectionGainSlider.setValue(0.0, juce::dontSendNotification);
        selectionGainLabel.setText("0.0 dB", juce::dontSendNotification);
        applySelectionGainButton.setButtonText("Apply");
        waveformView.clearSelectionPreviewGain();
        clearReferenceAudioSuitePreview(true);
        refreshUiState();
        waveformView.refreshNow();
    }
    else
    {
        applySelectionGainButton.setButtonText("Retry");
        selectionGainLabel.setText(juce::String(selectionGainSlider.getValue(), 1) + " dB",
                                   juce::dontSendNotification);
        const auto failure = result.status == AudioSuiteReferenceProcessor::ApplyStatus::stalePlan
            ? "AudioSuite plan became stale; Retry will re-analyze."
            : (result.status == AudioSuiteReferenceProcessor::ApplyStatus::cancelled
                   ? "AudioSuite Apply was cancelled before publication."
                   : "AudioSuite Apply failed before atomic publication; Retry is available.");
        applySelectionGainButton.setTooltip(failure);
        updateTransportAvailability();
    }
}

void MainComponent::cancelReferenceAudioSuiteApply(bool waitForExit)
{
    if (referenceAudioSuiteApplyThread == nullptr)
        return;

    referenceAudioSuiteApplyThread->cancel();
    if (waitForExit)
    {
        referenceAudioSuiteApplyThread.reset();
        referenceAudioSuiteApplyInFlight = false;
    }
}

ParametricEqMagComponent* MainComponent::getParametricEqMag() const noexcept
{
    if (workspaceModuleHost == nullptr)
        return nullptr;
    return dynamic_cast<ParametricEqMagComponent*>(
        workspaceModuleHost->getModuleComponent(WorkspaceModuleId::parametricEq));
}

ParametricEqProcessor* MainComponent::ensureParametricEqProcessor()
{
    if (parametricEqProcessor != nullptr)
        return parametricEqProcessor.get();
    if (!coreLifecycle.isCurrent(activeDocumentGeneration)
        || !coreLifecycle.isCoreReady(activeDocumentGeneration)
        || workspaceModuleHost == nullptr
        || !workspaceModuleHost->isModuleActive(WorkspaceModuleId::parametricEq))
    {
        return nullptr;
    }

    const auto module = OptionalModuleId::dsp;
    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.describePlacement(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration, module)))
    {
        coreLifecycle.release(activeDocumentGeneration, module);
        return nullptr;
    }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext factoryContext(optionalLifecycleContext);
#endif
    parametricEqProcessor = ParametricEqProcessor::describe().create();
    if (parametricEqProcessor == nullptr
        || !lifecycleTransitionAccepted(coreLifecycle.activate(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.attachView(activeDocumentGeneration, module)))
    {
        parametricEqProcessor.reset();
        releaseDspLifecycleIfIdle();
        return nullptr;
    }
    return parametricEqProcessor.get();
}

void MainComponent::setParametricEqPreviewEnabled(bool enabled)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (optionalLifecycleContext.observer != nullptr)
        optionalLifecycleContext.observer->command(optionalLifecycleContext,
            OptionalLifecycleOwner::parametricEq, enabled ? OptionalLifecycleAction::previewRequested
                                                         : OptionalLifecycleAction::previewOffRequested);
#endif
    if (enabled)
        updateParametricEqPreview();
    else
        clearParametricEqPreview(true);
}

void MainComponent::updateParametricEqPreview()
{
    auto* mag = getParametricEqMag();
    if (mag == nullptr || parametricEqApplyInFlight
        || truePeakLimiterApplyInFlight
        || referenceAudioSuiteApplyInFlight)
        return;

    clearTruePeakLimiterPreview(true);
    clearReferenceAudioSuitePreview(true);
    clearParametricEqPreview(false, false);

    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid() || !audioEngine.hasLoadedFile())
    {
        mag->setStatusText("No editable audio is ready");
        clearParametricEqPreview(true);
        return;
    }

    auto* processor = ensureParametricEqProcessor();
    if (processor == nullptr)
    {
        mag->setStatusText("DSP module could not be prepared");
        clearParametricEqPreview(true);
        return;
    }

    const auto analyzed = processor->analyze(state,
                                             activeDocumentGeneration.value,
                                             buildAudioSuiteSelectionRequest(),
                                             parametricEqParameters);
    if (!analyzed.succeeded())
    {
        mag->setStatusText(analyzed.errorMessage.isNotEmpty()
                               ? analyzed.errorMessage
                               : "EQ analysis failed");
        clearParametricEqPreview(true);
        return;
    }

    auto preview = processor->createPreview(analyzed.plan);
    if (!preview.isValid())
    {
        mag->setStatusText("EQ preview could not be created");
        clearParametricEqPreview(true);
        return;
    }

    activeParametricEqPlan = analyzed.plan;
    parametricEqPreview = preview;
    audioEngine.setAudioSuitePreview(preview.getState());
    updateParametricEqCurve(analyzed.plan);

    if (!loopBeforeParametricEqPreview.has_value())
        loopBeforeParametricEqPreview = audioEngine.isLoopEnabled();
    if (!playbackBeforeParametricEqPreview.has_value())
        playbackBeforeParametricEqPreview = audioEngine.isPlaying();

    if (state->info.sampleRate > 0.0)
    {
        const auto previewRange = preview.getPlaybackRange();
        const auto currentSample = static_cast<int64_t>(
            std::floor(audioEngine.getCurrentPositionSeconds() * state->info.sampleRate));
        if (!previewRange.contains(currentSample))
        {
            audioEngine.seekToSeconds(
                static_cast<double>(previewRange.getStart()) / state->info.sampleRate);
        }
    }
    audioEngine.setLoopEnabled(true);
    if (!audioEngine.isPlaying())
        audioEngine.play();

    mag->setPreviewing(true);
    mag->setStatusText(currentSelection.has_value()
                           ? "Preview | selected time x channels"
                           : "Preview | whole document x all channels");
    updateTransportAvailability();
}

void MainComponent::clearParametricEqPreview(bool releaseProcessor, bool restoreTransport)
{
    const auto hadPreview = parametricEqPreview.isValid();
    parametricEqPreview.cancel();
    parametricEqPreview = {};
    activeParametricEqPlan.reset();
    if (hadPreview)
        audioEngine.clearAudioSuitePreview();

    if (restoreTransport)
    {
        if (loopBeforeParametricEqPreview.has_value())
            audioEngine.setLoopEnabled(*loopBeforeParametricEqPreview);
        if (playbackBeforeParametricEqPreview.has_value()
            && !*playbackBeforeParametricEqPreview
            && audioEngine.isPlaying())
        {
            audioEngine.stop();
        }
        loopBeforeParametricEqPreview.reset();
        playbackBeforeParametricEqPreview.reset();
    }

    if (auto* mag = getParametricEqMag())
    {
        mag->setPreviewing(false);
        mag->clearExactMagnitudeCurve();
        if (!parametricEqApplyInFlight)
            mag->setStatusText("Ready | Preview is audition-only");
    }

    if (releaseProcessor && !parametricEqApplyInFlight)
    {
        parametricEqProcessor.reset();
        releaseDspLifecycleIfIdle();
    }
    updateTransportAvailability();
}

void MainComponent::applyParametricEq()
{
    if (parametricEqApplyInFlight
        || truePeakLimiterApplyInFlight
        || referenceAudioSuiteApplyInFlight
        || !document.hasAudio()
        || !audioEngine.hasLoadedFile())
    {
        return;
    }

    if (!parametricEqPreview.isValid())
        updateParametricEqPreview();
    const auto plan = activeParametricEqPlan;
    if (plan == nullptr)
        return;

    const auto applyGeneration = activeDocumentGeneration.value;
    clearParametricEqPreview(false);
    parametricEqApplyInFlight = true;
    if (auto* mag = getParametricEqMag())
    {
        mag->setApplying(true);
        mag->setStatusText("Rendering atomically | document unchanged until commit");
    }
    updateTransportAvailability();

    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto taskLifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::parametricEq, OptionalLifecycleResourceKind::applyTask);
    try
    {
#endif
    parametricEqApplyThread = std::make_unique<ParametricEqApplyThread>(
        document,
        plan->getAudioSuitePlan(),
        applyGeneration,
        [safeThis, plan, applyGeneration](AudioSuiteApplyResult result) mutable
        {
            if (safeThis != nullptr)
                safeThis->finishParametricEqApply(result,
                                                  std::move(plan),
                                                  applyGeneration);
        });
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    }
    catch (...) { taskLifetime.failed(); throw; }
    taskLifetime.created();
    parametricEqApplyThread->observeLifetime(std::move(taskLifetime));
#endif
    if (!parametricEqApplyThread->startThread())
    {
        parametricEqApplyThread.reset();
        parametricEqApplyInFlight = false;
        if (auto* mag = getParametricEqMag())
        {
            mag->setApplying(false);
            mag->setStatusText("Apply thread could not start | Retry available");
        }
        parametricEqProcessor.reset();
        releaseDspLifecycleIfIdle();
        updateTransportAvailability();
    }
}

void MainComponent::finishParametricEqApply(AudioSuiteApplyResult result,
                                            ParametricEqPlanPtr appliedPlan,
                                            std::uint64_t applyGeneration)
{
    parametricEqApplyThread.reset();
    parametricEqApplyInFlight = false;
    const auto sameGeneration = activeDocumentGeneration.value == applyGeneration
                             && coreLifecycle.isCurrent(activeDocumentGeneration);
    parametricEqProcessor.reset();
    clearParametricEqPreview(true);

    if (auto* mag = getParametricEqMag())
        mag->setApplying(false);

    if (!sameGeneration)
    {
        updateTransportAvailability();
        return;
    }

    if (result.succeeded())
    {
        lastAppliedParametricEqPlan = std::move(appliedPlan);
        refreshUiState();
        waveformView.refreshNow();
        if (auto* mag = getParametricEqMag())
            mag->setStatusText("Applied | waveform committed | Undo available");
    }
    else if (auto* mag = getParametricEqMag())
    {
        const auto failure = result.status == AudioSuiteApplyStatus::stalePlan
            ? "Document changed | Preview again before Retry"
            : (result.status == AudioSuiteApplyStatus::cancelled
                   ? "Apply cancelled | document unchanged"
                   : "Apply failed before publication | Retry available");
        mag->setStatusText(failure);
    }
    updateTransportAvailability();
}

void MainComponent::cancelParametricEqApply(bool waitForExit)
{
    if (parametricEqApplyThread == nullptr)
        return;
    parametricEqApplyThread->cancel();
    if (waitForExit)
    {
        parametricEqApplyThread.reset();
        parametricEqApplyInFlight = false;
        parametricEqProcessor.reset();
        releaseDspLifecycleIfIdle();
    }
}

void MainComponent::updateParametricEqCurve(const ParametricEqPlanPtr& plan)
{
    auto* mag = getParametricEqMag();
    if (mag == nullptr || plan == nullptr || plan->getSampleRate() <= 0.0)
        return;

    const auto pointCount = RendererVisualStyle::isClassic() ? 96 : 512;
    const auto minimumFrequency = 20.0;
    const auto maximumFrequency = std::max(minimumFrequency,
        std::min(20000.0, plan->getSampleRate() * 0.49));
    const auto logMinimum = std::log(minimumFrequency);
    const auto logRange = std::log(maximumFrequency) - logMinimum;
    std::vector<float> curve;
    curve.reserve(pointCount);
    for (auto point = 0; point < pointCount; ++point)
    {
        const auto proportion = static_cast<double>(point) / (pointCount - 1);
        const auto frequency = std::exp(logMinimum + proportion * logRange);
        curve.push_back(static_cast<float>(juce::Decibels::gainToDecibels(
            plan->magnitudeAt(frequency),
            -120.0)));
    }
    mag->setExactMagnitudeCurve(std::move(curve), minimumFrequency, maximumFrequency);
}

TruePeakLimiterMagComponent* MainComponent::getTruePeakLimiterMag() const noexcept
{
    if (workspaceModuleHost == nullptr)
        return nullptr;
    return dynamic_cast<TruePeakLimiterMagComponent*>(
        workspaceModuleHost->getModuleComponent(WorkspaceModuleId::truePeakLimiter));
}

TruePeakLimiterProcessor* MainComponent::ensureTruePeakLimiterProcessor()
{
    if (truePeakLimiterProcessor != nullptr)
        return truePeakLimiterProcessor.get();
    if (!coreLifecycle.isCurrent(activeDocumentGeneration)
        || !coreLifecycle.isCoreReady(activeDocumentGeneration)
        || workspaceModuleHost == nullptr
        || !workspaceModuleHost->isModuleActive(WorkspaceModuleId::truePeakLimiter))
    {
        return nullptr;
    }

    const auto module = OptionalModuleId::dsp;
    if (!lifecycleTransitionAccepted(coreLifecycle.trigger(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.describePlacement(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.prepare(activeDocumentGeneration, module)))
    {
        coreLifecycle.release(activeDocumentGeneration, module);
        return nullptr;
    }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext factoryContext(optionalLifecycleContext);
#endif
    truePeakLimiterProcessor = TruePeakLimiterProcessor::describe().create();
    if (truePeakLimiterProcessor == nullptr
        || !lifecycleTransitionAccepted(coreLifecycle.activate(activeDocumentGeneration, module))
        || !lifecycleTransitionAccepted(coreLifecycle.attachView(activeDocumentGeneration, module)))
    {
        truePeakLimiterProcessor.reset();
        releaseDspLifecycleIfIdle();
        return nullptr;
    }
    return truePeakLimiterProcessor.get();
}

void MainComponent::setTruePeakLimiterPreviewEnabled(bool enabled)
{
    if (enabled)
        updateTruePeakLimiterPreview();
    else
        clearTruePeakLimiterPreview(true);
}

void MainComponent::updateTruePeakLimiterPreview()
{
    auto* mag = getTruePeakLimiterMag();
    if (mag == nullptr || truePeakLimiterApplyInFlight
        || parametricEqApplyInFlight || referenceAudioSuiteApplyInFlight)
    {
        return;
    }

    clearParametricEqPreview(true);
    clearReferenceAudioSuitePreview(true);
    clearTruePeakLimiterPreview(false, false);
    if (!truePeakLimiterParameters.enabled)
    {
        mag->setStatusText("Off | zero processing");
        clearTruePeakLimiterPreview(true);
        return;
    }

    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid() || !audioEngine.hasLoadedFile())
    {
        mag->setStatusText("No editable audio is ready");
        clearTruePeakLimiterPreview(true);
        return;
    }

    auto* processor = ensureTruePeakLimiterProcessor();
    if (processor == nullptr)
    {
        mag->setStatusText("DSP module could not be prepared");
        clearTruePeakLimiterPreview(true);
        return;
    }
    const auto analyzed = processor->analyze(state,
                                             activeDocumentGeneration.value,
                                             buildAudioSuiteSelectionRequest(),
                                             truePeakLimiterParameters);
    if (!analyzed.succeeded())
    {
        mag->setStatusText(analyzed.errorMessage.isNotEmpty()
                               ? analyzed.errorMessage
                               : "Limiter analysis failed");
        clearTruePeakLimiterPreview(true);
        return;
    }

    auto preview = processor->createPreview(analyzed.plan);
    if (!preview.isValid())
    {
        mag->setStatusText("Limiter preview could not be created");
        clearTruePeakLimiterPreview(true);
        return;
    }

    activeTruePeakLimiterPlan = analyzed.plan;
    truePeakLimiterPreview = preview;
    audioEngine.setAudioSuitePreview(preview.getState());
    if (!loopBeforeTruePeakLimiterPreview.has_value())
        loopBeforeTruePeakLimiterPreview = audioEngine.isLoopEnabled();
    if (!playbackBeforeTruePeakLimiterPreview.has_value())
        playbackBeforeTruePeakLimiterPreview = audioEngine.isPlaying();
    if (state->info.sampleRate > 0.0)
    {
        const auto previewRange = preview.getPlaybackRange();
        const auto currentSample = static_cast<int64_t>(
            std::floor(audioEngine.getCurrentPositionSeconds() * state->info.sampleRate));
        if (!previewRange.contains(currentSample))
        {
            audioEngine.seekToSeconds(
                static_cast<double>(previewRange.getStart()) / state->info.sampleRate);
        }
    }
    audioEngine.setLoopEnabled(true);
    if (!audioEngine.isPlaying())
        audioEngine.play();

    mag->setPreviewing(true);
    mag->setFeedback(analyzed.plan->getFeedbackSnapshot());
    mag->setStatusText(currentSelection.has_value()
                           ? "Preview | selected time x linked channels"
                           : "Preview | whole document x linked channels");
    updateTransportAvailability();
}

void MainComponent::clearTruePeakLimiterPreview(bool releaseProcessor,
                                                bool restoreTransport)
{
    const auto hadPreview = truePeakLimiterPreview.isValid();
    truePeakLimiterPreview.cancel();
    truePeakLimiterPreview = {};
    activeTruePeakLimiterPlan.reset();
    if (hadPreview)
        audioEngine.clearAudioSuitePreview();

    if (restoreTransport)
    {
        if (loopBeforeTruePeakLimiterPreview.has_value())
            audioEngine.setLoopEnabled(*loopBeforeTruePeakLimiterPreview);
        if (playbackBeforeTruePeakLimiterPreview.has_value()
            && !*playbackBeforeTruePeakLimiterPreview
            && audioEngine.isPlaying())
        {
            audioEngine.stop();
        }
        loopBeforeTruePeakLimiterPreview.reset();
        playbackBeforeTruePeakLimiterPreview.reset();
    }

    if (auto* mag = getTruePeakLimiterMag())
    {
        mag->setPreviewing(false);
        mag->clearFeedback();
        if (!truePeakLimiterApplyInFlight)
        {
            mag->setStatusText(truePeakLimiterParameters.enabled
                                   ? "Ready | Preview is audition-only"
                                   : "Off | zero processing");
        }
    }
    if (releaseProcessor && !truePeakLimiterApplyInFlight)
    {
        truePeakLimiterProcessor.reset();
        releaseDspLifecycleIfIdle();
    }
    updateTransportAvailability();
}

void MainComponent::applyTruePeakLimiter()
{
    if (truePeakLimiterApplyInFlight || parametricEqApplyInFlight
        || referenceAudioSuiteApplyInFlight
        || !truePeakLimiterParameters.enabled
        || !document.hasAudio() || !audioEngine.hasLoadedFile())
    {
        return;
    }

    if (!truePeakLimiterPreview.isValid())
        updateTruePeakLimiterPreview();
    const auto plan = activeTruePeakLimiterPlan;
    if (plan == nullptr)
        return;

    const auto applyGeneration = activeDocumentGeneration.value;
    clearTruePeakLimiterPreview(false);
    activeTruePeakLimiterPlan = plan;
    plan->resetFeedback();
    truePeakLimiterApplyInFlight = true;
    if (auto* mag = getTruePeakLimiterMag())
    {
        mag->setApplying(true);
        mag->setFeedback(plan->getFeedbackSnapshot());
        mag->setStatusText("Rendering + verifying | document unchanged until commit");
    }
    updateTransportAvailability();

    auto safeThis = juce::Component::SafePointer<MainComponent>(this);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto taskLifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::truePeakLimiter, OptionalLifecycleResourceKind::applyTask);
    try
    {
#endif
    truePeakLimiterApplyThread = std::make_unique<TruePeakLimiterApplyThread>(
        document,
        plan->getAudioSuitePlan(),
        applyGeneration,
        [safeThis, plan, applyGeneration](AudioSuiteApplyResult result) mutable
        {
            if (safeThis != nullptr)
                safeThis->finishTruePeakLimiterApply(result,
                                                     std::move(plan),
                                                     applyGeneration);
        });
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    }
    catch (...) { taskLifetime.failed(); throw; }
    taskLifetime.created();
    truePeakLimiterApplyThread->observeLifetime(std::move(taskLifetime));
#endif
    if (!truePeakLimiterApplyThread->startThread())
    {
        truePeakLimiterApplyThread.reset();
        truePeakLimiterApplyInFlight = false;
        activeTruePeakLimiterPlan.reset();
        if (auto* mag = getTruePeakLimiterMag())
        {
            mag->setApplying(false);
            mag->clearFeedback();
            mag->setStatusText("Apply thread could not start | Retry available");
        }
        truePeakLimiterProcessor.reset();
        releaseDspLifecycleIfIdle();
        updateTransportAvailability();
    }
}

void MainComponent::finishTruePeakLimiterApply(AudioSuiteApplyResult result,
                                               TruePeakLimiterPlanPtr appliedPlan,
                                               std::uint64_t applyGeneration)
{
    truePeakLimiterApplyThread.reset();
    truePeakLimiterApplyInFlight = false;
    activeTruePeakLimiterPlan.reset();
    const auto sameGeneration = activeDocumentGeneration.value == applyGeneration
                             && coreLifecycle.isCurrent(activeDocumentGeneration);
    truePeakLimiterProcessor.reset();

    auto* mag = getTruePeakLimiterMag();
    if (mag != nullptr)
    {
        mag->setApplying(false);
        mag->clearFeedback();
    }
    if (!sameGeneration)
    {
        releaseDspLifecycleIfIdle();
        updateTransportAvailability();
        return;
    }

    if (result.succeeded())
    {
        lastAppliedTruePeakLimiterPlan = std::move(appliedPlan);
        truePeakLimiterParameters = TruePeakLimiterParameters::masteringDefault();
        refreshUiState();
        waveformView.refreshNow();
        if (mag != nullptr)
        {
            mag->setParameters(truePeakLimiterParameters);
            mag->setRecallAvailable(true);
            mag->setStatusText("Applied + verified "
                + juce::String(result.verification.maximumTruePeakDbtp, 1)
                + " dBTP | Off reset | Undo available");
        }
    }
    else if (mag != nullptr)
    {
        const auto failure = result.status == AudioSuiteApplyStatus::stalePlan
            ? juce::String("Document changed | Preview again before Retry")
            : (result.status == AudioSuiteApplyStatus::cancelled
                   ? juce::String("Apply cancelled | document unchanged")
                   : (result.status == AudioSuiteApplyStatus::verificationFailed
                          ? "Verification rejected staging | document unchanged | "
                                + result.verification.failureMessage
                          : juce::String("Apply failed before publication | Retry available")));
        mag->setStatusText(failure);
    }
    releaseDspLifecycleIfIdle();
    updateTransportAvailability();
}

void MainComponent::cancelTruePeakLimiterApply(bool waitForExit)
{
    if (truePeakLimiterApplyThread == nullptr)
        return;
    truePeakLimiterApplyThread->cancel();
    if (waitForExit)
    {
        truePeakLimiterApplyThread.reset();
        truePeakLimiterApplyInFlight = false;
        activeTruePeakLimiterPlan.reset();
        truePeakLimiterProcessor.reset();
        releaseDspLifecycleIfIdle();
    }
}

void MainComponent::recallLastAppliedTruePeakLimiter()
{
    if (lastAppliedTruePeakLimiterPlan == nullptr || truePeakLimiterApplyInFlight)
        return;
    clearTruePeakLimiterPreview(true);
    truePeakLimiterParameters = lastAppliedTruePeakLimiterPlan->getParameters();
    if (auto* mag = getTruePeakLimiterMag())
    {
        mag->setParameters(truePeakLimiterParameters);
        mag->setRecallAvailable(true);
        mag->setStatusText("Last Applied recalled | Preview before applying again");
    }
}

void MainComponent::releaseDspLifecycleIfIdle()
{
    if (!coreLifecycle.isCurrent(activeDocumentGeneration)
        || referenceAudioSuiteProcessor != nullptr
        || parametricEqProcessor != nullptr
        || truePeakLimiterProcessor != nullptr
        || referenceAudioSuiteApplyInFlight
        || parametricEqApplyInFlight
        || truePeakLimiterApplyInFlight)
    {
        return;
    }
    coreLifecycle.release(activeDocumentGeneration, OptionalModuleId::dsp);
}

void MainComponent::applyFadeIn()
{
    if (currentSelection.has_value() && document.applyFadeIn(*currentSelection))
    {
        refreshUiState();
        waveformView.refreshNow();
    }
}

void MainComponent::applyFadeOut()
{
    if (currentSelection.has_value() && document.applyFadeOut(*currentSelection))
    {
        refreshUiState();
        waveformView.refreshNow();
    }
}

void MainComponent::deleteSelection()
{
    if (currentSelection.has_value() && document.deleteSelection(*currentSelection))
    {
        currentSelection.reset();
        waveformView.clearSelection();
        selectionGainSlider.setValue(0.0, juce::dontSendNotification);
        selectionGainLabel.setText("0.0 dB", juce::dontSendNotification);
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
        refreshUiState();
    }
}

void MainComponent::undoEdit()
{
    if (document.undo())
    {
        // Preview plans and their displayed results belong to the old document state.
        clearTruePeakLimiterPreview(true);
        clearParametricEqPreview(true);
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
        refreshUiState();
        waveformView.refreshNow();
    }
}

void MainComponent::redoEdit()
{
    if (document.redo())
    {
        clearTruePeakLimiterPreview(true);
        clearParametricEqPreview(true);
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
        refreshUiState();
        waveformView.refreshNow();
    }
}

void MainComponent::zoomTimeOut()
{
    waveformView.zoomHorizontal(1.0 / 1.35);
}

void MainComponent::zoomTimeIn()
{
    waveformView.zoomHorizontal(1.35);
}

void MainComponent::zoomAmplitudeOut()
{
    waveformView.zoomVertical(1.0f / 1.2f);
}

void MainComponent::zoomAmplitudeIn()
{
    waveformView.zoomVertical(1.2f);
}

void MainComponent::resetWaveformView()
{
    waveformView.resetView();
}

void MainComponent::loadedFileChanged(const AudioEngine::LoadedFileInfo& info)
{
    if (info.isValid())
    {
        audioEngine.warmUpAudioDeviceAsync(0);
        if (const auto state = document.getState();
            state != nullptr
            && state->hasPlaybackSource()
            && state->info.file == info.file
            && state->audioRevision == info.audioRevision
            && activeGenerationFile == info.file)
        {
            markLoadPlayableIfNeeded(info.file);
        }
    }
    refreshUiState();
    if (metadataSidebar) metadataSidebar->refreshChannelControls();
    if (!deferChromeForStartup || workspaceModuleHost != nullptr)
    {
        const auto hadAmbisonicFieldPanel = workspaceModuleHost != nullptr
                                         && workspaceModuleHost->isModuleActive(WorkspaceModuleId::ambisonicEnergy);
        ensureAmbisonicFieldPanelIfNeeded();
        const auto hasAmbisonicFieldPanel = workspaceModuleHost != nullptr
                                         && workspaceModuleHost->isModuleActive(WorkspaceModuleId::ambisonicEnergy);
        if (hadAmbisonicFieldPanel != hasAmbisonicFieldPanel)
            resized();
    }
    repaintMetadataSidebarIfVisible();
}

void MainComponent::playlistChanged()
{
    persistSessionSettings();
    workspaceSession.pruneRetainedPlaylistAudio();
    updateTransportAvailability();
}

void MainComponent::currentPlaylistIndexChanged(int newIndex)
{
    persistSessionSettings();

    if (suppressPlaylistLoad)
        return;

    if (newIndex >= 0)
    {
        const auto* entry = playlistModel.getEntry(newIndex);
        if (entry && entry->recovered && workspaceSession.isActiveRecoveredDocument(entry->id))
            return; // Reordering/removing another row is not an explicit switch.
        loadPlaylistIndex(newIndex);
        scheduleAdjacentPlaylistCacheWarm(newIndex);
    }
    else
    {
        currentSelection.reset();
        selectionGainSlider.setValue(0.0, juce::dontSendNotification);
        cancelTruePeakLimiterApply(false);
        clearTruePeakLimiterPreview(true);
        cancelParametricEqApply(false);
        clearParametricEqPreview(true);
        clearReferenceAudioSuitePreview(true);
        waveformView.clearSelectionPreviewGain();
        workspaceSession.releaseRecoveredActivation();
        document.clear();
        if (metadataSidebar) metadataSidebar->clearTypedMetadataInspector();
        bnkDocument.clear();
        updateCenterViewVisibility();
        updateTransportAvailability();
    }
}

void MainComponent::playbackStateChanged(AudioEngine::PlaybackState)
{
    refreshTransportReadout();
    refreshFooterReadout();
    updateTransportAvailability();
}

void MainComponent::timerCallback()
{
    audioEngine.collectRetiredBinauralRenderers();
    if (asioSettingsSession && asioSettingsSession->needsPoll())
    {
        const auto previousStatus = asioSettingsSession->status();
        asioSettingsSession->poll();
        if (previousStatus != asioSettingsSession->status())
        {
            setDocumentOperationStatus(asioSettingsSession->status());
            if (audioEngine.hasExplicitOutputSelection() && !audioEngine.isSelectedOutputReady()) promoteDeferredChromeIfNeeded();
        }
    }
    if (pendingSelectedOutputPlayable != juce::File{} && pendingSelectedOutputPlayable == activeGenerationFile
        && audioEngine.isSelectedOutputReady() && audioEngine.canPlayAtSeconds(0))
    {
        const auto readyFile = pendingSelectedOutputPlayable;
        markLoadPlayableIfNeeded(readyFile);
    }
    if (deviceProbe)
    {
        deviceProbe->pollReturn(audioEngine);
        if (deviceProbe->phase() == diagnostics::GuiDeviceProbe::Phase::finished && deviceProbe->resourcesReleased())
            deviceProbe.reset(); // the visible review may retain its own result until consumed
    }
    if (bankOpenLoad || queuedBankOpen) pollBnkOpen();
    if (bnkRuntime) pollBnkPreview();
    if (packedAudioLoad && packedAudioLoad->awaitingTerminal) pollPackedAudioLoad();
    if (closeApplicationPending && !closingOptionalOperations.empty()) continueCloseApplication();
    if (recoveryRestore && recoveryRestore->needsPoll()) pollRecoveryRestore();
    if (recoveryService && recoveryService->needsPoll()) pollRecovery();
    if (recoveryCloseDraining) continueRecoveryClose();
    if (sessionSettingsPersistPending
        && juce::Time::getMillisecondCounterHiRes() >= sessionSettingsPersistDueAtMs
        && canPersistDeferredSettings())
    {
        flushPendingSessionSettingsPersist();
    }

    if (const auto documentState = document.getState();
        documentState != nullptr && documentState->info.isValid())
    {
        if (const auto peakCache = waveformModel.getPeakCache();
            peakCache != nullptr
            && !peakCache->isPreview
            && peakCache->binCount > 0)
        {
            const auto matchesDocumentRevision = peakCache->audioRevision == documentState->audioRevision;
            if (lastObservedFullCacheRevision != peakCache->audioRevision
                || lastObservedFullCacheDocumentRevision != documentState->audioRevision
                || lastObservedFullCacheMatchedDocument != matchesDocumentRevision)
            {
                lastObservedFullCacheRevision = peakCache->audioRevision;
                lastObservedFullCacheDocumentRevision = documentState->audioRevision;
                lastObservedFullCacheMatchedDocument = matchesDocumentRevision;
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
                appendOpenTimingLogLine(juce::Time::getCurrentTime().toISO8601(true)
                                        + " | outcome=full-cache-polled | file="
                                        + documentState->info.file.getFullPathName()
                                        + " | cache-revision="
                                        + juce::String(peakCache->audioRevision)
                                        + " | document-revision="
                                        + juce::String(documentState->audioRevision)
                                        + " | match="
                                        + juce::String(matchesDocumentRevision ? 1 : 0));
#endif
            }

            if (matchesDocumentRevision && lastPolledFullWaveformRevision != peakCache->audioRevision)
            {
                lastPolledFullWaveformRevision = peakCache->audioRevision;
                // The completed cache still needs a real waveform paint. Normal
                // startup promotes chrome through the resulting CoreReady event.
                waveformView.refreshNow();
            }
        }
    }
    else
    {
        lastPolledFullWaveformRevision = -1;
        lastObservedFullCacheRevision = -1;
        lastObservedFullCacheDocumentRevision = -1;
        lastObservedFullCacheMatchedDocument = false;
    }

    const auto current = audioEngine.getCurrentPositionSeconds();
    const auto state = audioEngine.getPlaybackState();
    refreshTimelineMarkers();

    if (commandPalette != nullptr && (audioEngine.isPlaying() || hasProgressiveWaveformPending()))
        commandPalette->invalidateBackdrop();

    const auto meterText = formatMeterReadout(
        static_cast<AudioEngine::MeterMode>(meterModeBox.getSelectedId()), audioEngine);
    // Meter ballistics can still settle after transport stops. Keep the footer's
    // detailed tooltip/accessibility readout in step with the visible meter.
    if (std::abs(current - lastDisplayedPosition) > 0.01 || state != lastDisplayedState
        || peakLabel.getText() != meterText)
    {
        lastDisplayedPosition = current;
        lastDisplayedState = state;
        refreshTransportReadout();
        refreshFooterReadout();
    }

    if (activeTruePeakLimiterPlan != nullptr
        && (truePeakLimiterPreview.isValid() || truePeakLimiterApplyInFlight))
    {
        if (auto* mag = getTruePeakLimiterMag())
            mag->setFeedback(activeTruePeakLimiterPlan->getFeedbackSnapshot());
    }

    peakLabel.setText(meterText, juce::dontSendNotification);
    dispatchPendingCacheWarmIfReady();
}
} // namespace singleeditor
