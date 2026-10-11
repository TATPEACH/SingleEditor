#pragma once

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Audio/AudioEngine.h"
#include "Audio/OptionalMediaOperation.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "AudioSuite/ParametricEqProcessor.h"
#include "AudioSuite/TruePeakLimiterProcessor.h"
#include "Bnk/BnkDocument.h"
#include "Bnk/BnkPlaybackBackend.h"
#include "Bnk/BnkPreviewRequest.h"
#include "Bnk/BnkRuntimeSession.h"
#include "Bnk/BnkOpenRequest.h"
#include "Core/CoreLifecycleCoordinator.h"
#include "Core/OptionalLifecycleObserver.h"
#include "Document/DocumentSaveCoordinator.h"
#include "Document/PlaylistModel.h"
#include "Document/SingleTrackDocument.h"
#include "Document/WorkspaceSessionSupport.h"
#include "Diagnostics/DiagnosticSnapshot.h"
#include "UI/Components/BnkAudioListViewComponent.h"
#include "UI/Components/BnkEventListViewComponent.h"
#include "UI/Components/AmbisonicFieldPanelComponent.h"
#include "UI/Components/SpatialEnergyPanelComponent.h"
#include "UI/Components/CardSurface.h"
#include "UI/Components/MeterBridgeComponent.h"
#include "UI/Components/MetadataSidebarComponent.h"
#include "UI/Components/ModernIconButton.h"
#include "UI/Components/ModernGlassSurface.h"
#include "UI/Components/ModernTransportWidgets.h"
#include "UI/Components/OutputMeterPanelComponent.h"
#include "UI/Components/ParametricEqMagComponent.h"
#include "UI/Components/TruePeakLimiterMagComponent.h"
#include "UI/Components/PlaylistSidebarComponent.h"
#include "UI/Components/PreferencesComponent.h"
#include "UI/Components/CustomTitleBarComponent.h"
#include "UI/Components/WaveformViewComponent.h"
#include "UI/Commands/CommandPaletteComponent.h"
#include "UI/Commands/FocusNavigationModel.h"
#include "UI/Commands/GuiCommandModel.h"
#include "UI/Workspace/WorkspaceModuleHostComponent.h"
#include "UI/Workspace/WorkspacePresetCatalog.h"
#include "Waveform/WaveformModel.h"

namespace singleeditor
{
namespace recovery { class RecoveryService; class RecoveryRestoreQueue; class RecoveryDiagnosticState; }
namespace diagnostics { class GuiDeviceProbe; }
class AsioSettingsSession;
class SaveEncodingOptionsDialog;
class MainComponent final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private PlaylistModel::Listener,
                            private AudioEngine::Listener,
                            private juce::ComponentListener,
                            private juce::KeyListener,
                            private juce::Timer
{
    friend struct AsioSettingsTestAccess;
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    friend struct DeferredSettingsTestAccess;
    friend struct DeferredWorkspaceControlsTestAccess;
    friend struct DeferredCacheWarmTestAccess;
    friend struct OptionalLifecycleTestAccess;
    friend struct MagLifecycleTestAccess;
    friend struct BackendLifecycleTestAccess;
#endif
public:
    MainComponent(juce::ResizableWindow& hostWindow,
                  juce::ApplicationProperties& appPropertiesToUse,
                  SingleTrackDocument& documentToUse,
                  BnkDocument& bnkDocumentToUse,
                  PlaylistModel& playlistModelToUse,
                  AudioEngine& audioEngineToUse,
                  WaveformModel& waveformModelToUse,
                  bool startupHasInitialFile = false,
                  bool hostProvidesTitleBar = false);
    ~MainComponent() override;
    std::function<void(bool)> onSimpleViewChanged;
    WindowControlKind findControlAtPoint(juce::Point<float> point) const override;

    void openInitialFile(const juce::File& file);
    void primeStartupLoadingState(const juce::File& file);
    void showPreparedInitialVisualPreview(const juce::File& file,
                                          SingleTrackDocument::FileInfo previewInfo,
                                          std::shared_ptr<WaveformModel::PeakCache> previewPeakCache);
    void beginPreparedInitialAudioLoad(const juce::File& file,
                                       SingleTrackDocument::FileInfo previewInfo);
    void showPreparedInitialAudioPreview(const juce::File& file,
                                         SingleTrackDocument::FileInfo previewInfo,
                                         std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                         bool beginLoadIfNeeded = true);
    void openPreparedInitialAudio(const juce::File& file,
                                  SingleTrackDocument::FileInfo previewInfo,
                                  std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                  std::shared_ptr<SingleTrackDocument::State> state);
    void setStartupOpenTiming(const juce::File& file,
                              double openRequestedAtMs,
                              double windowShownAtMs,
                              std::optional<double> primaryContentShownElapsedMs = std::nullopt,
                              std::optional<double> fileNameShownElapsedMs = std::nullopt,
                              std::optional<double> firstWaveformElapsedMs = std::nullopt);
    void requestCloseApplication();
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    const recovery::RecoveryService* recoveryServiceForTesting() const noexcept { return recoveryService.get(); }
    const recovery::RecoveryRestoreQueue* recoveryRestoreForTesting() const noexcept { return recoveryRestore.get(); }
    bool hasRecoveryCenterForTesting() const noexcept { return recoveryCenter != nullptr; }
    void showRecoveryCenterForTesting() { showRecoveryCenter(); }
    bool hasLocalDiagnosticsWindowForTesting() const noexcept { return diagnosticsWindow != nullptr; }
    std::weak_ptr<diagnostics::GuiDeviceProbe> deviceProbeForTesting() const noexcept { return deviceProbe; }
    void showLocalDiagnosticsForTesting() { showLocalDiagnostics(); }
#endif
    bool attachRecoveredCandidate(const recovery::RecoveredCandidateDocument&);
    bool publishRecoveredPeaks(const juce::Uuid& attempt, SingleTrackDocument::StatePtr originalCore,
                               std::shared_ptr<WaveformModel::PeakCache> peaks);
    bool isCoreReadyForWorkspaceResources() const noexcept { return coreLifecycle.isCoreReady(activeDocumentGeneration); }
    bool canPersistDeferredSettings() const noexcept;
    void setStartupCoreReadyCallback(std::function<void()> callback);
    void setStartupCoreSurfacePreparedCallback(std::function<void()> callback);
    void restoreSavedAudioOutputAfterStartupHandoff();
    void setStartupRendererPrepareCallback(std::function<void()> callback);
    void setStartupRendererReadyCallback(std::function<void(bool)> callback);
    void setStartupTransitionSnapshot(juce::Image snapshot);

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool handleTopLevelKeyPress(const juce::KeyPress& key);
    void showAsyncLoadPreview(int64_t loadToken,
                              SingleTrackDocument::FileInfo info,
                              std::shared_ptr<WaveformModel::PeakCache> peakCache);
    void showAsyncLoadMetadata(int64_t loadToken,
                               SingleTrackDocument::FileInfo info);
    void completeAsyncLoad(int64_t loadToken,
                           const juce::File& file,
                           std::shared_ptr<SingleTrackDocument::State> state,
                           std::shared_ptr<WaveformModel::PeakCache> peakCache,
                           bool bankSignatureAfterReaderFailure,
                           juce::String errorMessage = {},
                           media::OptionalMediaOperation::Ptr optionalOperation = {});

private:
    enum class BnkCenterMode
    {
        events = 0,
        audio
    };

    struct LoadTimingProbe
    {
        juce::File file;
        double openRequestedAtMs = 0.0;
        std::optional<double> windowShownElapsedMs;
        std::optional<double> primaryContentElapsedMs;
        std::optional<double> fileNameShownElapsedMs;
        std::optional<double> firstWaveformElapsedMs;
        std::optional<double> playableElapsedMs;
        std::optional<double> fullWaveformElapsedMs;
        bool fromStartupLaunch = false;
        juce::String lastLoggedOutcome;
    };

    class WindowDragZone final : public juce::Component,
                                 public juce::SettableTooltipClient
    {
    public:
        explicit WindowDragZone(juce::ResizableWindow& hostWindowToDrag)
            : hostWindow(hostWindowToDrag)
        {
            setTooltip("Drag window");
        }

        void paint(juce::Graphics&) override {}

        void mouseDown(const juce::MouseEvent& event) override
        {
            dragger.startDraggingComponent(&hostWindow, event);
        }

        void mouseDrag(const juce::MouseEvent& event) override
        {
            dragger.dragComponent(&hostWindow, event, hostWindow.getConstrainer());
        }

    private:
        juce::ResizableWindow& hostWindow;
        juce::ComponentDragger dragger;
    };

    class SidebarResizeHandle final : public juce::Component,
                                      public juce::SettableTooltipClient
    {
    public:
        explicit SidebarResizeHandle(bool affectsLeftSidebar);

        void paint(juce::Graphics& g) override;
        void mouseEnter(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;

        std::function<void(int)> onDragDelta;

    private:
        bool isLeftSidebar = true;
        bool hovered = false;
        int dragAnchorScreenX = 0;
    };

    static bool isSupportedAudioPath(const juce::String& path);
    static juce::String formatTimecode(double seconds);
    static juce::String formatAudioDetails(const AudioEngine::LoadedFileInfo& info);
    static juce::String formatBnkDetails(const BnkDocument::BankInfo& info);
    static juce::String formatPlaybackState(AudioEngine::PlaybackState state);
    static juce::String formatSelectionRange(const std::optional<juce::Range<double>>& selection);
    static juce::String formatMeterReadout(AudioEngine::MeterMode mode, const AudioEngine& engine);
    static bool isBnkFile(const juce::File& file) noexcept;
    static bool isWemFile(const juce::File& file) noexcept;

    void chooseAndOpenFile();
    void showRecentFilesMenu();
    void showTitleBarMenu(CustomTitleBarComponent::MenuKind menuKind, juce::Rectangle<int> targetScreenBounds);
    void showPreferences();
    void showAsioSettings();
    void showLocalDiagnostics(diagnostics::ProviderScope = {});
    void setModernSimpleView(bool enabled);
    void attachBnkProjectRoot();
    void openFiles(const std::vector<juce::File>& files, int preferredIndex = 0);
    void loadPlaylistIndex(int index);
    enum class AudioLoadPolicy { reuseSession, reloadFromDisk };
    void beginAsyncLoad(const juce::File& file,
                        AudioLoadPolicy policy = AudioLoadPolicy::reuseSession,
                        bool generationAlreadyBegun = false);
    void beginVideoAudioLoad(const juce::File&, media::OptionalMediaFailureState::Intent = media::OptionalMediaFailureState::Intent::initial,
                             std::optional<media::OptionalMediaFailureState::Key> reviewed = {});
    void showVideoAudioFailure(const juce::File&, const media::OptionalMediaOperation::Ptr&);
    void exportVideoAudioDiagnostics(const media::OptionalMediaOperation::Ptr&);
    void beginPackedAudioLoad(const juce::File&, media::OptionalMediaFailureState::Intent = media::OptionalMediaFailureState::Intent::initial,
                             std::optional<media::OptionalMediaFailureState::Key> reviewed = {});
    void pollPackedAudioLoad();
    void showPackedAudioFailure(const juce::File&, const media::OptionalMediaOperation::Ptr&);
    void cancelPackedAudioLoad();
    struct AudioReturnImage;
    AudioReturnImage captureAudioReturnImage() const;
    bool restoreAudioReturn(const AudioReturnImage&, bool restorePlaylist = true);
    bool ordinaryAudioLoadOwnsDocument() const;
    void observeOrdinaryAudioLoadState();
    bool cancelOrdinaryAudioLoad(bool preservePlaylist = true);
    void selectLoadedAudioEntry(const juce::File&);
    void beginBnkOpen(const juce::File&, BnkOpenRequest::Capability = BnkOpenRequest::Capability::core,
        media::OptionalMediaFailureState::Intent = media::OptionalMediaFailureState::Intent::initial,
        std::optional<media::OptionalMediaFailureState::Key> reviewed = {});
    void pollBnkOpen();
    void cancelBnkOpen();
    void showBnkOpenFailure(const BnkOpenRequest::Ptr&, BnkOpenRequest::Capability);
    void exportBnkOpenDiagnostics(const BnkOpenRequest::Ptr&);
    struct BnkOpenIntent;
    bool bnkOpenWorkspaceMatches(const BnkOpenIntent&) const;
    bool restoreBnkOpenSelection(const BnkOpenIntent&);
    void beginPreparedInitialAudioLoadInternal(const juce::File& file,
                                               SingleTrackDocument::FileInfo previewInfo,
                                               bool refreshImmediately,
                                               bool scheduleBackgroundLoad = true);
    void saveDocument(bool forceSaveAs, std::function<void(bool)> completion = {});
    void exportDocument(std::function<void(bool)> completion = {});
    void chooseSaveTarget(SingleTrackDocument::SaveIntent intent,
                          std::function<void(bool)> completion = {});
    SingleTrackDocument::SaveEncodingOptions getStoredSaveEncodingOptions() const;
    void chooseSaveEncodingOptions(
        const juce::File& targetFile,
        std::function<void(std::optional<SingleTrackDocument::SaveEncodingOptions>)> completion);
    void beginSaveToTarget(const juce::File& targetFile,
                           SingleTrackDocument::SaveIntent intent,
                           SingleTrackDocument::SaveEncodingOptions encodingOptions,
                           std::function<void(bool)> completion);
    void showExternalSourceSaveConflict(std::function<void(bool)> completion = {});
    void setDocumentOperationStatus(juce::String message);
    void refreshUiState();
    void repaintMetadataSidebarIfVisible();
    bool hasPlayableAudioReady() const noexcept;
    bool canQueuePlaybackDuringLoad() const noexcept;
    bool hasProgressiveWaveformPending() const noexcept;
    void requestAudioPlaybackOrQueue();
    void toggleAudioPlaybackOrQueue();
    void refreshTransportReadout();
    void refreshTimelineMarkers();
    void setModernAppearanceTheme(ModernTheme theme);
    void refreshFooterReadout();
    void refreshWindowAndTitle();
    void applyChromePresentation(bool relayoutNow = true);
    void ensureWorkspaceModuleHost();
    void ensureDeferredWorkspaceControls();
    void ensureDeferredOutputMeter();
    void ensureAmbisonicFieldPanelIfNeeded();
    OutputMeterPanelComponent* getOutputMeterPanel() const noexcept;
    AmbisonicFieldPanelComponent* getAmbisonicFieldPanel() const noexcept;
    void promoteDeferredChromeIfNeeded(int delayMs = 0);
    bool isChromeDeferredForStartup() const noexcept;
    void startLoadTimingProbe(const juce::File& file);
    void markPrimaryContentVisibleIfNeeded();
    void markFileNameShownIfNeeded();
    void markLoadPlayableIfNeeded(const juce::File& file);
    void markWaveformPresentedIfNeeded(const juce::File& file, bool hasFullWaveform);
    void beginDocumentGeneration(const juce::File& file);
    void ensureDocumentGeneration(const juce::File& file);
    void schedulePostCorePromotionIfNeeded(CoreMilestoneResult result);
    void notifyStartupCoreReadyIfNeeded();
    void notifyStartupCoreSurfacePreparedIfNeeded();
    void notifyStartupRendererReadyIfNeeded(bool succeeded);
    void logLoadTimingIfReady(const juce::String& outcome);
    void abandonActiveLoadTimingProbe(const juce::String& outcome);
    void updateSidebarToggleButtons();
    void updateTransportAvailability();
    void updateSelectionPopup();
    void togglePlaylistSidebar();
    void toggleMetadataSidebar();
    bool confirmDiscardOrSaveIfNeeded(const juce::String& actionDescription, std::function<void()> continuation,
                                     std::function<void()> cancelled = {});
    void continueCloseApplication();
    void cancelCloseApplication();
    void finishCloseApplication();
    void attachRecoveryAfterCore();
    void ensureRecoveryService();
    void discoverRecoveryAfterCore();
    void showRecoveryCenter();
    void pollRecovery();
    void pollRecoveryRestore();
    juce::Rectangle<int> layoutRecoveryStatus();
    void showRecoveryActions();
    void retryRecovery();
    void continueRecoveryClose();
    bool hasUnconfirmedCloseChanges() const;
    void requestPlaylistActivation(int index);
    void requestPlaylistRemoval(int index);
    void removeCurrentPlaylistEntry();
    bool finishReusedAudioLoad(const juce::File& file, bool shouldAutoPlay);
    void scheduleCacheWarm(std::vector<juce::File> candidateFiles, int delayMs, bool skipIfAudioLoaded);
    void dispatchPendingCacheWarmIfReady();
    void cancelCacheWarm();
    void scheduleWaveformCacheWarm(const std::vector<juce::File>& files, int preferredIndex);
    void scheduleAdjacentPlaylistCacheWarm(int currentIndex);
    void scheduleRecentCacheWarm();
    void restorePlaylistSession();
    void restoreSessionSettings(bool includeWaveformViewPreferences = true);
    void restoreStartupCriticalSettings();
    void applyDeferredStartupUiSettingsIfNeeded();
    void persistSessionSettings();
    void requestSessionSettingsPersist(int delayMs = -1);
    void flushPendingSessionSettingsPersist();
    juce::File getDefaultBrowseDirectory() const;
    juce::File getCurrentLoadedFile() const;
    BnkMediaResolveMode getBnkMediaResolveMode() const;
    void updateCenterViewVisibility();
    void ensureBnkControls();
    void ensureBnkRuntime();
    void ensureBnkUiComponents();
    void releaseBnkRuntime();
    void notifyPrimaryContentVisibleIfNeeded();
    void prepareSelectedBnkEvent(bool autoPlay);
    void prepareSelectedBnkMedia(uint32_t mediaId, bool autoPlay);
    void beginBnkPreview(BnkPreviewRequest::Ptr, bool autoPlay,
        media::OptionalMediaFailureState::Intent = media::OptionalMediaFailureState::Intent::initial,
        std::optional<media::OptionalMediaFailureState::Key> reviewed = {});
    void queueBnkPreview(BnkPreviewRequest::Ptr, bool autoPlay, media::OptionalMediaFailureState::Intent,
        std::optional<media::OptionalMediaFailureState::Key>);
    void pollBnkPreview();
    void cancelBnkPreview();
    bool bnkPreviewWorkspaceMatches(const BnkRuntimeSession::PreviewIntent&) const;
    void showBnkPreviewFailure();
    void syncBnkAudioSelectionForCurrentContext();
    bool shouldAutoPlayBnkContextChange() const;
    void refreshBnkControlSurface(bool syncMediaSelection = true);
    void refreshAdmittedBnkControls(bool syncMediaSelection);
    void refreshBnkRtpcSelectionUi();
    BnkPlaybackContext buildBnkPlaybackContext() const;
    void handleSelectionChanged(std::optional<juce::Range<double>> newSelection);
    void handleSourceChannelSelectionGesture(int sourceChannel, bool extendSelection);
    AudioSelectionRequest buildAudioSuiteSelectionRequest() const;
    AudioSuiteReferenceProcessor* ensureReferenceAudioSuiteProcessor();
    void updateReferenceAudioSuitePreview();
    void clearReferenceAudioSuitePreview(bool releaseProcessor);
    void dismissSelectionPopup();
    void finishReferenceAudioSuiteApply(AudioSuiteReferenceProcessor::ApplyResult result,
                                        AudioSuitePlanPtr appliedPlan,
                                        std::uint64_t applyGeneration);
    void cancelReferenceAudioSuiteApply(bool waitForExit);
    ParametricEqMagComponent* getParametricEqMag() const noexcept;
    ParametricEqProcessor* ensureParametricEqProcessor();
    void setParametricEqPreviewEnabled(bool enabled);
    void updateParametricEqPreview();
    void clearParametricEqPreview(bool releaseProcessor, bool restoreTransport = true);
    void applyParametricEq();
    void finishParametricEqApply(AudioSuiteApplyResult result,
                                 ParametricEqPlanPtr appliedPlan,
                                 std::uint64_t applyGeneration);
    void cancelParametricEqApply(bool waitForExit);
    void updateParametricEqCurve(const ParametricEqPlanPtr& plan);
    TruePeakLimiterMagComponent* getTruePeakLimiterMag() const noexcept;
    TruePeakLimiterProcessor* ensureTruePeakLimiterProcessor();
    void setTruePeakLimiterPreviewEnabled(bool enabled);
    void updateTruePeakLimiterPreview();
    void clearTruePeakLimiterPreview(bool releaseProcessor,
                                     bool restoreTransport = true);
    void applyTruePeakLimiter();
    void finishTruePeakLimiterApply(AudioSuiteApplyResult result,
                                    TruePeakLimiterPlanPtr appliedPlan,
                                    std::uint64_t applyGeneration);
    void cancelTruePeakLimiterApply(bool waitForExit);
    void recallLastAppliedTruePeakLimiter();
    void releaseDspLifecycleIfIdle();
    void applySelectionGain();
    void componentMovedOrResized(juce::Component&, bool, bool) override;
    void componentParentHierarchyChanged(juce::Component&) override;
    void componentVisibilityChanged(juce::Component&) override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    void applyFadeIn();
    void applyFadeOut();
    void deleteSelection();
    void undoEdit();
    void redoEdit();
    void zoomTimeOut();
    void zoomTimeIn();
    void zoomAmplitudeOut();
    void zoomAmplitudeIn();
    void resetWaveformView();
    void registerCoreCommandProvider();
    void registerPostCoreCommandProvider();
    void registerWorkspaceCommandProvider();
    GuiCommandState resolveCoreCommandState(std::string_view commandId,
                                            const GuiCommandDispatchContext& context) const;
    GuiCommandState resolveWorkspaceCommandState(std::string_view commandId,
                                                 const GuiCommandDispatchContext& context) const;
    GuiCommandResult performCoreCommand(std::string_view commandId,
                                        const GuiCommandDispatchContext& context);
    GuiCommandResult performWorkspaceCommand(std::string_view commandId,
                                             const GuiCommandDispatchContext& context,
                                             bool waveformFloatConfirmed = false);
    GuiCommandDispatchContext makeCommandContext(GuiCommandSource source,
                                                 std::string targetScope = {}) const;
    GuiCommandResult dispatchGuiCommand(std::string_view commandId,
                                        GuiCommandSource source,
                                        std::string targetScope = {});
    void reportCommandResult(const GuiCommandResult& result);
    void showCommandPalette();
    void dismissCommandPalette();
    ShortcutFocusContext currentShortcutFocusContext() const;
    FocusReturnToken captureFocusReturnToken(std::string sourceScope = {},
                                             std::string semanticAnchor = {}) const;
    void restoreFocusReturnToken(FocusReturnToken token);
    juce::Component* componentForSemanticAnchor(std::string_view anchor) const;
    bool cycleMajorRegionFocus(bool reverse);
    void playlistChanged() override;
    void currentPlaylistIndexChanged(int newIndex) override;
    void loadedFileChanged(const AudioEngine::LoadedFileInfo& info) override;
    void playbackStateChanged(AudioEngine::PlaybackState newState) override;
    void timerCallback() override;

    juce::ResizableWindow& hostWindow;
    juce::ApplicationProperties& appProperties;
    SingleTrackDocument& document;
    BnkDocument& bnkDocument;
    PlaylistModel& playlistModel;
    AudioEngine& audioEngine;
    std::shared_ptr<AsioSettingsSession> asioSettingsSession;
    juce::Component::SafePointer<juce::DialogWindow> asioSettingsWindow;
    bool savedAudioOutputStarted = false;
    juce::File pendingSelectedOutputPlayable;
    WaveformModel& waveformModel;
    CoreLifecycleCoordinator coreLifecycle;
    CoreLifecycleCoordinator::Generation activeDocumentGeneration;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext = currentOptionalLifecycleContext();
#endif
    juce::File activeGenerationFile;
    bool postCorePromotionScheduled = false;
    juce::Component::SafePointer<juce::Component> playlistFocusBeforeLoad;
    GuiCommandRegistry commandRegistry;
    bool postCoreCommandProviderRegistered = false;
    bool workspaceCommandProviderRegistered = false;
    std::uint64_t workspaceFocusGeneration = 1;

    CustomTitleBarComponent titleBar;
    CardSurface actionBar;
    CardSurface footerBar;
    ModernGlassSurface selectionPopup;
    struct BnkControls final
    {
        CardSurface surface { 14.0f };
        juce::Label switchLabel, stateLabel, rtpcLabel, rtpcValueLabel;
        juce::TextButton eventsTab, audioTab, audioFilter, attachProject;
        juce::ComboBox switchBox, stateBox, rtpcBox;
        juce::Slider rtpcSlider;
    };
    std::unique_ptr<BnkControls> bnkControls;
    WindowDragZone actionDragZone;
    std::unique_ptr<PlaylistSidebarComponent> playlistSidebar;
    std::unique_ptr<MetadataSidebarComponent> metadataSidebar;
    std::unique_ptr<WorkspaceModuleHostComponent> workspaceModuleHost;
    std::unique_ptr<WorkspacePresetCatalog> workspacePresetCatalog;
    std::unique_ptr<CommandPaletteComponent> commandPalette;
    std::optional<FocusReturnToken> commandPaletteReturnToken;
    WorkspaceContentCategory activeWorkspaceCategory = WorkspaceContentCategory::general;
    bool outputMeterWorkspaceModuleRegistered = false;
    bool ambisonicWorkspaceModuleRegistered = false;
    bool parametricEqWorkspaceModuleRegistered = false;
    bool truePeakLimiterWorkspaceModuleRegistered = false;
    WaveformViewComponent waveformView;
    std::unique_ptr<BnkEventListViewComponent> bnkEventListView;
    std::unique_ptr<BnkAudioListViewComponent> bnkAudioListView;
    SidebarResizeHandle playlistResizeHandle;
    SidebarResizeHandle metadataResizeHandle;
    ModernIconButton togglePlaylistSidebarButton;
    ModernIconButton toggleMetadataSidebarButton;

    ModernIconButton openButton;
    juce::TextButton toolbarMoreButton { "More" };
    juce::TextButton generalSceneButton { "General" }, simpleViewButton { "Simple" };
    ModernThemeButton lightThemeButton { false }, nightThemeButton { true };
    bool modernSimpleView = false;
    juce::Component::SafePointer<juce::Component> focusBeforeSimpleView;
    ModernIconButton recentButton;
    ModernIconButton removeFileButton;
    ModernIconButton preferencesButton;
    ModernIconButton saveButton;
    ModernIconButton saveAsButton;
    std::unique_ptr<ModernIconButton> parametricEqButton;
    std::unique_ptr<ModernIconButton> truePeakLimiterButton;
    ModernIconButton startButton;
    ModernIconButton previousButton;
    ModernIconButton nextButton;
    ModernIconButton playButton;
    ModernIconButton pauseButton;
    ModernIconButton stopButton;
    ModernIconButton gainDownButton;
    ModernIconButton gainUpButton;
    ModernIconButton fadeInButton;
    ModernIconButton fadeOutButton;
    ModernIconButton deleteSelectionButton;
    ModernIconButton undoButton;
    ModernIconButton redoButton;
    ModernIconButton resetEnvelopeButton;
    ModernIconButton timeZoomOutButton;
    ModernIconButton timeZoomInButton;
    ModernIconButton ampZoomOutButton;
    ModernIconButton ampZoomInButton;
    ModernIconButton resetViewButton;
    juce::Slider selectionGainSlider;
    juce::Label selectionGainLabel;
    juce::TextButton applySelectionGainButton;
    juce::TextButton cancelSelectionGainButton;
    ModernIconButton returnToStartButton;
    ModernIconButton loopButton;
    juce::TextButton autoPlayButton;
    juce::Slider monitorSlider;
    juce::Label monitorLabel;
    juce::ComboBox meterModeBox;
    std::unique_ptr<MeterBridgeComponent> meterBridge;
    ModernTransportMeterLabel peakLabel;
    juce::Label filePillLabel;
    ModernStatusLabel engineStatusLabel;
    ModernTransportTimeLabel transportStatusLabel;
    ModernStatusLabel footerLeftLabel;
    ModernStatusLabel footerRightLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<SaveEncodingOptionsDialog> encodingOptionsDialog;
    bool externalSourceSaveConflictPending = false;
    juce::String documentOperationStatus;
    uint64_t documentOperationStatusGeneration = 0;
    int64_t documentOperationStatusChangeGeneration = 0;
    WorkspaceSessionSupport workspaceSession;
    DocumentSaveCoordinator saveCoordinator;
    std::shared_ptr<recovery::RecoveryDiagnosticState> recoveryDiagnostics;
    std::unique_ptr<recovery::RecoveryService> recoveryService;
    std::unique_ptr<recovery::RecoveryRestoreQueue> recoveryRestore;
    std::unique_ptr<juce::DocumentWindow> recoveryCenter;
    std::unique_ptr<juce::DocumentWindow> diagnosticsWindow;
    std::shared_ptr<diagnostics::GuiDeviceProbe> deviceProbe;
    std::thread recoveryDiscoveryWorker;
    std::shared_ptr<std::atomic<bool>> recoveryDiscoveryCancelled;
    bool recoveryDiscoveryScheduled = false;
    bool recoveryCandidateNoticeDeferred = false;
    juce::String recoveryCandidateNotice;
    juce::String recoveryRestoreNotice;
    std::unique_ptr<juce::TextButton> recoveryStatusButton;
    juce::String recoveryStatusText;
    bool closeApplicationPending = false;
    std::vector<media::OptionalMediaOperation::Ptr> closingOptionalOperations;
    bool recoveryCloseDraining = false, recoveryClosePrompt = false;
    double recoveryCloseStartedAtMs = 0;
    // UI decisions only: these images never authorize storage retirement or GC.
    std::vector<WorkspaceSessionSupport::UnsavedDocument> closeDiscardedDocuments;
    std::optional<juce::Range<double>> currentSelection;
    bool selectionPopupDismissed = false;
    std::vector<SourceChannelIdentity> selectedSourceChannels;
    std::unique_ptr<AudioSuiteReferenceProcessor> referenceAudioSuiteProcessor;
    AudioSuitePlanPtr activeReferenceAudioSuitePlan;
    AudioSuitePlanPtr lastAppliedReferenceAudioSuitePlan;
    AudioSuitePreviewHandle referenceAudioSuitePreview;
    std::optional<bool> loopBeforeReferenceAudioSuitePreview;
    class ReferenceAudioSuiteApplyThread;
    std::unique_ptr<ReferenceAudioSuiteApplyThread> referenceAudioSuiteApplyThread;
    bool referenceAudioSuiteApplyInFlight = false;
    ParametricEqParameters parametricEqParameters = ParametricEqParameters::flat();
    bool parametricEqAdvancedView = false;
    std::size_t parametricEqSelectedBand = 0;
    std::unique_ptr<ParametricEqProcessor> parametricEqProcessor;
    ParametricEqPlanPtr activeParametricEqPlan;
    ParametricEqPlanPtr lastAppliedParametricEqPlan;
    AudioSuitePreviewHandle parametricEqPreview;
    std::optional<bool> loopBeforeParametricEqPreview;
    std::optional<bool> playbackBeforeParametricEqPreview;
    class ParametricEqApplyThread;
    std::unique_ptr<ParametricEqApplyThread> parametricEqApplyThread;
    bool parametricEqApplyInFlight = false;
    TruePeakLimiterParameters truePeakLimiterParameters =
        TruePeakLimiterParameters::masteringDefault();
    std::unique_ptr<TruePeakLimiterProcessor> truePeakLimiterProcessor;
    TruePeakLimiterPlanPtr activeTruePeakLimiterPlan;
    TruePeakLimiterPlanPtr lastAppliedTruePeakLimiterPlan;
    AudioSuitePreviewHandle truePeakLimiterPreview;
    std::optional<bool> loopBeforeTruePeakLimiterPreview;
    std::optional<bool> playbackBeforeTruePeakLimiterPreview;
    class TruePeakLimiterApplyThread;
    std::unique_ptr<TruePeakLimiterApplyThread> truePeakLimiterApplyThread;
    bool truePeakLimiterApplyInFlight = false;
    std::function<void()> onStartupCoreReady;
    std::function<void()> onStartupCoreSurfacePrepared;
    bool startupCoreSurfacePreparedNotified = false;
    std::function<void()> prepareStartupRenderer;
    std::function<void(bool)> onStartupRendererReady;
    bool primaryContentVisibleNotified = false;
    bool startupCoreReadyNotified = false;
    bool startupRendererPrepared = false;
    bool startupRendererReadyNotified = false;
    bool dragHighlight = false;
    bool loadInProgress = false;
    bool playlistSidebarCollapsed = false;
    bool metadataSidebarCollapsed = false;
    int playlistSidebarWidth = 218;
    int metadataSidebarWidth = 232;
    juce::String pendingLoadLabel;
    juce::String failedLoadFileName, failedLoadMessage;
    std::shared_ptr<media::OptionalMediaFailureState> optionalFailureSession;
    media::OptionalMediaOperation::DiagnosticHandle lastOptionalDiagnostic;
    media::OptionalMediaOperation::Ptr pendingVideoOperation;
    SingleTrackDocument::StatePtr videoLoadPreviousState;
    std::optional<juce::Uuid> videoLoadPreviousEntry;
    juce::Component::SafePointer<juce::AlertWindow> videoProgressWindow;
    struct AudioReturnImage
    {
        juce::File previousFile;
        SingleTrackDocument::StatePtr previousState;
        SingleTrackDocument::WorkspaceSnapshot::Ptr previousAudio;
        BnkDocument::SelectionSnapshot::Ptr previousBank;
        std::shared_ptr<WaveformModel::PeakCache> previousPeaks;
        std::shared_ptr<WaveformModel::SpectrogramCache> previousSpectrogram;
        std::optional<juce::Uuid> previousEntry;
        std::optional<juce::Range<double>> previousSelection;
        bool previousRecovered = false;
    };
    struct PackedAudioLoad final : AudioReturnImage
    {
        media::OptionalMediaOperation::Ptr operation;
        juce::File file;
        int64_t token = 0;
        SingleTrackDocument::StatePtr admittedState;
        bool awaitingTerminal = false;
    };
    struct OrdinaryAudioLoad final
    {
        AudioReturnImage returnImage;
        SingleTrackDocument::StatePtr expectedState;
        SingleTrackDocument::WorkspaceSnapshot::Ptr expectedWorkspace;
        media::IdentitySnapshot expectedIdentity;
        std::optional<juce::Uuid> expectedEntry;
        int64_t token = 0;
    };
    std::optional<OrdinaryAudioLoad> ordinaryAudioLoad;
    std::optional<PackedAudioLoad> packedAudioLoad;
    juce::Component::SafePointer<juce::AlertWindow> packedProgressWindow;
    BnkRuntimeSession::Ptr bnkRuntime;
    juce::Component::SafePointer<juce::AlertWindow> bankProgressWindow;
    int64_t bankProgressToken = 0;
    struct BnkOpenIntent final
    {
        BnkOpenRequest::Ptr request;
        SingleTrackDocument::WorkspaceSnapshot::Ptr previousWorkspace;
        SingleTrackDocument::StatePtr previousState;
        std::shared_ptr<const BnkDocument::State> previousBank;
        media::DocumentIdentity previousDocument;
        std::optional<juce::Range<double>> previousSelection;
        std::optional<juce::Uuid> expectedEntry, returnEntry;
        std::optional<uint32_t> previousEvent, previousMedia;
        int64_t token = 0;
        bool previousWasEmpty = false, preserveAudio = false;
    };
    struct BnkOpenWork final
    {
        BnkOpenRequest::Ptr request;
        BnkParser::Result prepared;
        std::atomic<bool> finished { false };
    };
    struct BnkOpenLoad final { BnkOpenIntent intent; std::shared_ptr<BnkOpenWork> work; };
    std::optional<BnkOpenLoad> bankOpenLoad;
    std::optional<BnkOpenIntent> queuedBankOpen;
    BnkOpenRequest::Ptr lastBnkOpenRequest;
    BnkOpenRequest::DiagnosticHandle lastBnkOpenDiagnostic;
    juce::Component::SafePointer<juce::AlertWindow> bankOpenProgressWindow, bankOpenFailureWindow;
    int64_t bankOpenProgressToken = 0;
    std::vector<juce::String> bnkSwitchTokens;
    std::vector<juce::String> bnkStateTokens;
    BnkCenterMode bnkCenterMode = BnkCenterMode::events;
    bool bnkAudioLinkedOnly = true;
    bool suppressBnkControlCallbacks = false;
    bool pendingAutoPlayOnLoad = false;
    juce::File pendingAutoPlayFile;
    double lastDisplayedPosition = -1.0;
    AudioEngine::PlaybackState lastDisplayedState = AudioEngine::PlaybackState::stopped;
    juce::ThreadPool loadThreadPool { 1 };
    struct PendingCacheWarm
    {
        std::vector<juce::File> candidateFiles;
        CoreLifecycleCoordinator::Generation generation;
        double dueAtMs = 0.0;
        bool skipIfAudioLoaded = false;
    };
    std::optional<PendingCacheWarm> pendingCacheWarm;
    std::unique_ptr<juce::ThreadPool> cacheWarmThreadPool;
    std::atomic<int64_t> pendingLoadToken { 0 };
    bool suppressPlaylistLoad = false;
    bool startupHasInitialFile = false;
    bool hostProvidesTitleBar = false;
    bool deferChromeForStartup = false;
    bool deferredChromePromotionPending = false;
    bool startupTransitionClearScheduled = false;
    bool deferredStartupUiSettingsPending = false;
    bool sessionSettingsPersistPending = false;
    bool lossyOverwriteConfirmedForSession = false;
    double sessionSettingsPersistDueAtMs = 0.0;
    bool workspacePresetWritable = true;
    bool workspacePresetCatalogLoaded = false;
    bool workspacePresetCatalogWritable = true;
    WorkspacePresetCatalogRestoreStatus workspacePresetCatalogRestoreStatus =
        WorkspacePresetCatalogRestoreStatus::noState;
    juce::File pendingStartupTimingFile;
    double pendingStartupOpenRequestedAtMs = 0.0;
    double pendingStartupWindowShownAtMs = 0.0;
    std::optional<double> pendingStartupPrimaryContentShownElapsedMs;
    std::optional<double> pendingStartupFileNameShownElapsedMs;
    std::optional<double> pendingStartupFirstWaveformElapsedMs;
    std::optional<LoadTimingProbe> activeLoadTimingProbe;
    int64_t lastPolledFullWaveformRevision = -1;
    int64_t lastObservedFullCacheRevision = -1;
    int64_t lastObservedFullCacheDocumentRevision = -1;
    bool lastObservedFullCacheMatchedDocument = false;
    juce::Image startupTransitionSnapshot;
};
} // namespace singleeditor
