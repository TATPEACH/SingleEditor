#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Document/SingleTrackDocument.h"
#include "Waveform/WaveformModel.h"
#include "App/WindowsWindowShell.h"

namespace singleeditor
{
class AudioEngine;
class BnkDocument;
class PlaylistModel;
class MainComponent;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
class RuntimePlaybackProbe;
#endif

class MainWindow final : public juce::DocumentWindow,
                         private juce::Timer
{
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    friend struct DeferredSettingsTestAccess;
#endif
public:
    MainWindow(juce::String name,
               juce::ApplicationProperties& appProperties,
               SingleTrackDocument& document,
               BnkDocument& bnkDocument,
               PlaylistModel& playlistModel,
               AudioEngine& audioEngine,
               WaveformModel& waveformModel,
               std::function<void()> prepareClientLookAndFeel = {},
               juce::File initialFileToOpen = {},
               double startupOpenRequestedAtMs = 0.0);
    ~MainWindow() override;

    MainComponent* getMainComponent() const noexcept;
    void openFileWhenReady(const juce::File& file);
    void completeStartupAudioPreview(int64_t token,
                                     const juce::File& file,
                                     SingleTrackDocument::FileInfo previewInfo,
                                     std::shared_ptr<WaveformModel::PeakCache> previewPeakCache = {});
    void completeStartupAudioPrepare(int64_t token,
                                     const juce::File& file,
                                     SingleTrackDocument::FileInfo previewInfo,
                                     std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                     std::shared_ptr<SingleTrackDocument::State> state);
    void restoreFromBackground();
    void requestUserCloseAction();
    void requestActualQuit();
    void closeButtonPressed() override;
    WindowControlKind findControlAtPoint(juce::Point<float> point) const override;
    bool keyPressed(const juce::KeyPress& key) override;
    void activeWindowStatusChanged() override;
    void moved() override;
    void resized() override;
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    juce::String trayResourceStatusForTesting() const;
    bool hasNativeTrayIconForTesting() const;
#endif

private:
    class TrayIconComponent;
    struct PreparedStartupAudioPreview
    {
        juce::File file;
        SingleTrackDocument::FileInfo previewInfo;
        std::shared_ptr<WaveformModel::PeakCache> previewPeakCache;
    };

    struct PreparedStartupAudioLoad
    {
        juce::File file;
        SingleTrackDocument::FileInfo previewInfo;
        std::shared_ptr<WaveformModel::PeakCache> previewPeakCache;
        std::shared_ptr<SingleTrackDocument::State> state;
    };

    bool shouldCloseToBackground() const;
    void hideToBackground();
    void beginStartupAudioPrepare(const juce::File& file);
    void scheduleMainContentInstallAfterStartupPreview(bool preferImmediateOnMessageThread = true);
    void initialiseMainContentAsync(bool preferImmediateOnMessageThread = false);
    void installMainContent();
    void ensureNativeScaleFactorNotifier();
    void ensureTrayIconVisible(bool shouldBeVisible);
    void restoreWindowState();
    void persistWindowState();
    void requestWindowStatePersist(int delayMs = -1);
    void flushPendingWindowStatePersist();
    void requestStartupKeyboardFocus();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void startRuntimeProbeAfterCoreReady();
#endif
    void timerCallback() override;

    juce::ApplicationProperties& appProperties;
    SingleTrackDocument& document;
    BnkDocument& bnkDocument;
    PlaylistModel& playlistModel;
    AudioEngine& audioEngine;
    WaveformModel& waveformModel;
    std::function<void()> prepareClientLookAndFeel;
    WindowsWindowShell windowShell;
    bool startupHasInitialFile = false;
    bool mainContentInstalled = false;
    bool mainContentInstallScheduled = false;
    bool mainContentInstallDelayScheduled = false;
    juce::File pendingFileToOpen;
    double startupOpenRequestedAtMs = 0.0;
    double startupWindowShownAtMs = 0.0;
    double startupPreviewSurfaceShownAtMs = 0.0;
    juce::CriticalSection startupPrepareLock;
    juce::ThreadPool startupPrepareThreadPool { 1 };
    std::optional<PreparedStartupAudioPreview> preparedStartupPreview;
    std::optional<PreparedStartupAudioLoad> preparedStartupAudio;
    bool startupPrepareCompleted = false;
    std::optional<double> startupPreviewFileNameElapsedMs;
    std::optional<double> startupPreviewWaveformElapsedMs;
    std::atomic<int64_t> startupPrepareToken { 0 };
    std::unique_ptr<TrayIconComponent> trayIcon;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<juce::NativeScaleFactorNotifier> nativeScaleFactorNotifier;
    bool hiddenToBackground = false;
    bool windowStatePersistPending = false;
    double windowStatePersistDueAtMs = 0.0;
    std::optional<juce::Rectangle<int>> lastPersistedWindowBounds;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::unique_ptr<RuntimePlaybackProbe> runtimePlaybackProbe;
#endif
};
} // namespace singleeditor
