#include "App/MainWindow.h"
#include "App/ShutdownDiagnostics.h"
#include "App/CoreOpenProbe.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "App/RuntimePlaybackProbe.h"
#endif
#include "App/StartupFocusPolicy.h"
#include "App/WindowPlacementPolicy.h"
#include "UI/Commands/NativeFocusTrace.h"
#include "App/StartupPresentationCoordinator.h"
#include "App/WindowShellResources.h"

#include "Audio/AudioEngine.h"
#include "Audio/AsioSettingsSession.h"
#include "Audio/WindowsMediaAudioService.h"
#include "Bnk/BnkDocument.h"
#include "Document/PlaylistModel.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Components/PreferencesComponent.h"
#include "UI/MainComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernGlassTokens.h"
#include "UI/LookAndFeel/BrandMark.h"
#include "UI/LookAndFeel/StartupCoreSkin.h"
#include "Waveform/WaveformModel.h"
#include <cmath>

namespace singleeditor
{
namespace
{
constexpr auto windowBoundsKey = "mainWindowBounds";
constexpr auto windowBoundsVersionKey = "mainWindowBoundsVersion";
constexpr int currentWindowBoundsVersion = 2;
constexpr int defaultWindowWidth = 1560;
constexpr int defaultWindowHeight = 900;
constexpr int normalWindowStatePersistDelayMs = 700;
constexpr int startupWindowStatePersistDelayMs = 2600;
constexpr int startupTooltipDelayMs = 1900;
constexpr int normalTooltipDelayMs = 450;
constexpr int startupPreviewMinimumVisibleMs = 0;

StartupCoreSkin::Palette startupCorePalette() noexcept
{
    return StartupCoreSkin::palette(!RendererVisualStyle::isClassic(), RendererVisualStyle::modernTheme());
}

juce::Rectangle<int> constrainWindowBoundsToDisplay(juce::Rectangle<int> bounds)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    auto* display = displays.getDisplayForRect(bounds);
    if (display == nullptr)
        display = displays.getPrimaryDisplay();

    if (display == nullptr)
        return bounds;

    return windowPlacement::fitInWorkArea(bounds, display->userArea);
}

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
void appendStartupTimingLine(const juce::String& line)
{
    static juce::CriticalSection lock;
    const juce::ScopedLock scopedLock(lock);

    auto appDataDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                .getChildFile("SingleEditor");
    appDataDirectory.createDirectory();
    const juce::Array<juce::File> targets {
        appDataDirectory.getChildFile("open_timing.log"),
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getParentDirectory()
            .getChildFile("open_timing.log")
    };

    for (const auto& target : targets)
    {
        juce::FileOutputStream stream(target);
        if (!stream.openedOk())
            continue;
        if (target.existsAsFile())
            stream.setPosition(target.getSize());
        stream.writeText(line + juce::newLine, false, false, nullptr);
        stream.flush();
    }
}
#endif

juce::Image createFallbackTrayImage()
{
    juce::Image image(juce::Image::ARGB, 32, 32, true);
    juce::Graphics g(image);
    g.fillAll(juce::Colours::transparentBlack);
    BrandMark::paint(g, image.getBounds().toFloat());
    return image;
}

class StartupPlaceholderComponent final : public juce::Component
{
public:
    StartupPlaceholderComponent()
    {
        setOpaque(true);
    }

    void setPendingFile(juce::File fileToShow)
    {
        pendingFile = std::move(fileToShow);
        repaint();
    }

    void setPreviewState(SingleTrackDocument::FileInfo newInfo,
                         std::shared_ptr<WaveformModel::PeakCache> newPeakCache)
    {
        previewInfo = std::move(newInfo);
        if (previewInfo.file.existsAsFile())
            pendingFile = previewInfo.file;
        previewPeakCache = std::move(newPeakCache);
        repaint();
    }

    void setPlayableState(bool isPlayable)
    {
        playable = isPlayable;
        if (playable)
            rendererAttachFailed = false;
        repaint();
    }

    void setRendererAttachFailed()
    {
        rendererAttachFailed = true;
        repaint();
    }

    bool hasTruthfulWaveform() const noexcept
    {
        return previewPeakCache != nullptr
            && previewPeakCache->mixedBins.size() > 1
            && previewPeakCache->durationSeconds > 0.0;
    }

    void paint(juce::Graphics& g) override
    {
        const auto colours = startupCorePalette();
        g.fillAll(colours.windowBackground);

        const auto displayFile = previewInfo.file.existsAsFile() ? previewInfo.file : pendingFile;
        const auto hasMeaningfulContent = displayFile.existsAsFile() || previewInfo.isValid() || previewPeakCache != nullptr;
        if (!hasMeaningfulContent)
            return;

        auto bounds = getLocalBounds().toFloat().reduced(48.0f);
        auto card = bounds.withSizeKeepingCentre(680.0f, 268.0f);
        g.setColour(colours.panelBackground.withAlpha(0.96f));
        g.fillRoundedRectangle(card, 24.0f);

        auto titleBounds = card.reduced(28.0f, 24.0f);
        g.setColour(colours.primaryText);
        g.setFont(juce::Font(juce::FontOptions(24.0f, juce::Font::bold)));
        g.drawFittedText(displayFile.existsAsFile() ? displayFile.getFileName()
                                                    : "Launching Single Editor",
                         titleBounds.removeFromTop(46.0f).toNearestInt(),
                         juce::Justification::centred,
                         1);
        if (displayFile.existsAsFile())
            CoreOpenProbe::mark(CoreOpenMilestone::fileName);

        g.setColour(colours.secondaryText);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        const auto detailText = previewInfo.isValid()
            ? juce::String::formatted("%.1f kHz  |  %d ch  |  %.2f s",
                                      previewInfo.sampleRate / 1000.0,
                                      previewInfo.numChannels,
                                      previewInfo.durationSeconds)
            : (displayFile.existsAsFile()
                   ? "Opening audio and preparing a waveform preview..."
                   : "Preparing Single Editor...");
        g.drawFittedText(detailText,
                         titleBounds.removeFromTop(32.0f).toNearestInt(),
                         juce::Justification::centred,
                         1);

        auto waveformBounds = titleBounds.removeFromTop(110.0f).reduced(18.0f, 14.0f);
        g.setColour(colours.workspaceSurface.withAlpha(0.95f));
        g.fillRoundedRectangle(waveformBounds, 18.0f);

        if (previewPeakCache != nullptr
            && previewPeakCache->mixedBins.size() > 1
            && previewPeakCache->durationSeconds > 0.0)
        {
            const auto centerY = waveformBounds.getCentreY();
            const auto halfHeight = waveformBounds.getHeight() * 0.42f;
            const auto widthPixels = juce::jmax(1, static_cast<int>(waveformBounds.getWidth()));
            const auto& bins = previewPeakCache->mixedBins;
            const auto loadedEndSeconds = (previewPeakCache->isPreview && previewPeakCache->sampleRate > 0.0)
                ? static_cast<double>(previewPeakCache->loadedSampleRange.getEnd()) / previewPeakCache->sampleRate
                : previewPeakCache->durationSeconds;

            for (int x = 0; x < widthPixels; ++x)
            {
                const auto binStart = juce::jlimit(0,
                                                   static_cast<int>(bins.size()) - 1,
                                                   static_cast<int>((static_cast<double>(x) / widthPixels) * bins.size()));
                const auto binEnd = juce::jlimit(binStart + 1,
                                                 static_cast<int>(bins.size()),
                                                 static_cast<int>(std::ceil((static_cast<double>(x + 1) / widthPixels) * bins.size())));

                float minValue = 1.0f;
                float maxValue = -1.0f;
                bool hasLoadedPeak = false;
                for (int binIndex = binStart; binIndex < binEnd; ++binIndex)
                {
                    const auto& peak = bins[static_cast<std::size_t>(binIndex)];
                    if (previewPeakCache->isPreview && peak.startSeconds >= loadedEndSeconds)
                        break;

                    minValue = juce::jmin(minValue, peak.minValue);
                    maxValue = juce::jmax(maxValue, peak.maxValue);
                    hasLoadedPeak = true;
                }

                const auto lineX = waveformBounds.getX() + static_cast<float>(x);
                if (hasLoadedPeak)
                {
                    g.setColour(colours.waveform);
                    const auto top = juce::jlimit(waveformBounds.getY(),
                                                  waveformBounds.getBottom(),
                                                  centerY - juce::jlimit(-1.0f, 1.0f, maxValue) * halfHeight);
                    const auto bottom = juce::jlimit(waveformBounds.getY(),
                                                     waveformBounds.getBottom(),
                                                     centerY - juce::jlimit(-1.0f, 1.0f, minValue) * halfHeight);
                    g.drawVerticalLine(static_cast<int>(lineX), top, bottom);
                }
                else
                {
                    g.setColour(colours.secondaryText.withAlpha(0.13f));
                    g.drawVerticalLine(static_cast<int>(lineX),
                                       centerY - halfHeight * 0.28f,
                                       centerY + halfHeight * 0.28f);
                }
            }

            if (previewPeakCache->isPreview && loadedEndSeconds < previewPeakCache->durationSeconds)
            {
                const auto unloadedStartX = waveformBounds.getX()
                                          + waveformBounds.getWidth() * static_cast<float>(loadedEndSeconds / previewPeakCache->durationSeconds);
                if (unloadedStartX < waveformBounds.getRight())
                {
                    const auto unloadedBounds = juce::Rectangle<float>(unloadedStartX,
                                                                       waveformBounds.getY(),
                                                                       waveformBounds.getRight() - unloadedStartX,
                                                                       waveformBounds.getHeight());
                    g.setColour(colours.secondaryText.withAlpha(0.11f));
                    g.fillRoundedRectangle(unloadedBounds, 12.0f);
                }
            }

            CoreOpenProbe::mark(CoreOpenMilestone::firstWaveform);
        }
        else
        {
            g.setColour(colours.secondaryText.withAlpha(0.72f));
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawFittedText("Preparing waveform preview...",
                             waveformBounds.toNearestInt(),
                             juce::Justification::centred,
                             1);
        }

        g.setColour(rendererAttachFailed ? colours.failure
                                        : (playable ? colours.ready
                                                    : colours.secondaryText.withAlpha(0.82f)));
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        const auto statusText = rendererAttachFailed
            ? "The editor could not finish opening. The audio preview is still available."
            : (playable
                   ? "Ready to play. Opening the editor..."
                   : (hasTruthfulWaveform()
                          ? "Waveform preview ready. Preparing playback..."
                          : "Opening audio..."));
        g.drawFittedText(statusText,
                         titleBounds.removeFromTop(24.0f).toNearestInt(),
                         juce::Justification::centred,
                         1);
    }

private:
    juce::File pendingFile;
    SingleTrackDocument::FileInfo previewInfo;
    std::shared_ptr<WaveformModel::PeakCache> previewPeakCache;
    bool playable = false;
    bool rendererAttachFailed = false;
};

class StartupHostComponent final : public juce::Component
{
public:
    StartupHostComponent()
    {
        setOpaque(true);
        generation = presentation.beginGeneration();
        placeholder = std::make_unique<StartupPlaceholderComponent>();
        addAndMakeVisible(*placeholder);
    }

    void setPendingFile(juce::File file)
    {
        generation = presentation.beginGeneration();
        presentation.markFileNameVisible(generation);
        placeholder->setPendingFile(std::move(file));
        placeholder->setPlayableState(false);
    }

    void setPreviewState(SingleTrackDocument::FileInfo info,
                         std::shared_ptr<WaveformModel::PeakCache> peakCache)
    {
        placeholder->setPreviewState(std::move(info), std::move(peakCache));
        if (placeholder->hasTruthfulWaveform())
            presentation.markTruthfulWaveformVisible(generation);
    }

    MainComponent* getMainComponent() const noexcept
    {
        return mainContent.get();
    }

    void setMainContent(std::unique_ptr<MainComponent> content)
    {
        if (content == nullptr)
            return;

        mainContent = std::move(content);
        addAndMakeVisible(*mainContent);
        mainContent->setBounds(getLocalBounds());

        if (placeholder != nullptr)
            placeholder->toFront(false);

        repaint();
    }

    void coreReady()
    {
        presentation.markTruthfulWaveformVisible(generation);
        presentation.markPlayable(generation);
        if (presentation.beginRendererAttach(generation)
            == StartupPresentationResult::rejectedBeforeCoreReady)
        {
            return;
        }

        if (placeholder != nullptr)
            placeholder->setPlayableState(true);
        scheduleRevealMainContent();
    }

    void coreSurfacePrepared()
    {
        // The opaque placeholder otherwise prevents the minimal editor from
        // painting, while full renderer preparation must wait for that paint.
        scheduleRevealMainContent();
    }

    void rendererReady(bool succeeded)
    {
        if (succeeded)
        {
            if (!presentation.snapshot().coreReady)
                coreReady();
            presentation.completeRendererAttach(generation);
        }
        else
        {
            presentation.failRendererAttach(generation);
            if (placeholder != nullptr)
                placeholder->setRendererAttachFailed();
        }

        // A pre-Core document failure still needs to expose the existing editor
        // error surface. Post-Core renderer failure leaves the Core surface visible.
        scheduleRevealMainContent();
    }

    void noteHostMutation(StartupHostMutation mutation)
    {
        presentation.noteHostMutation(generation, mutation);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(startupCorePalette().windowBackground);
    }

    void resized() override
    {
        const auto bounds = getLocalBounds();
        if (mainContent != nullptr)
            mainContent->setBounds(bounds);
        if (placeholder != nullptr)
            placeholder->setBounds(bounds);
        noteHostMutation(StartupHostMutation::resize);
    }

private:
    void scheduleRevealMainContent()
    {
        if (mainContentRevealed || revealScheduled)
            return;

        revealScheduled = true;
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<StartupHostComponent>(this)]
        {
            if (safeThis == nullptr)
                return;

            safeThis->revealScheduled = false;
            safeThis->revealMainContent();
        });
    }

    void revealMainContent()
    {
        if (mainContent == nullptr || mainContentRevealed)
            return;

        if (presentation.snapshot().rendererState != StartupRendererAttachState::dormant
            && placeholder != nullptr && placeholder->isVisible())
        {
            auto snapshot = placeholder->createComponentSnapshot(placeholder->getLocalBounds(), true, 1.0f);
            mainContent->setStartupTransitionSnapshot(std::move(snapshot));
        }

        mainContent->toFront(false);
        if (placeholder != nullptr)
            placeholder->setVisible(false);

        mainContentRevealed = true;
        mainContent->repaint();
        repaint();
    }

    StartupPresentationCoordinator presentation;
    StartupPresentationCoordinator::Generation generation = 0;
    std::unique_ptr<StartupPlaceholderComponent> placeholder;
    std::unique_ptr<MainComponent> mainContent;
    bool revealScheduled = false;
    bool mainContentRevealed = false;
};

class StartupAudioPrepareJob final : public juce::ThreadPoolJob
{
public:
    StartupAudioPrepareJob(juce::Component::SafePointer<MainWindow> ownerToUse,
                           juce::File fileToPrepare,
                           int64_t tokenToUse,
                           double openRequestedAtMsToUse)
        : juce::ThreadPoolJob("Startup Audio Prepare"),
          owner(ownerToUse),
          file(std::move(fileToPrepare)),
          token(tokenToUse),
          openRequestedAtMs(openRequestedAtMsToUse)
    {
    }

    JobStatus runJob() override
    {
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        const auto jobStartedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif
        auto startupPreview = WaveformModel::loadPersistentStartupPreviewForFile(file);
        auto cachedHeader = startupPreview.has_value()
                                ? startupPreview
                                : WaveformModel::lookupPersistentPeakCacheHeaderForFile(file);
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        const auto cacheLookupFinishedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif
        auto previewInfo = cachedHeader.has_value() ? cachedHeader->info
                                                    : SingleTrackDocument::FileInfo {};
        auto previewPeakCache = startupPreview.has_value() ? startupPreview->cache
                                                           : std::shared_ptr<WaveformModel::PeakCache> {};
        auto dispatchedPreviewMetadata = false;
        auto dispatchedPreviewWaveform = false;

        auto dispatchPreview = [&](const SingleTrackDocument::FileInfo& infoToDispatch,
                                   std::shared_ptr<WaveformModel::PeakCache> cacheToDispatch)
        {
            if (!infoToDispatch.isValid())
                return;

            dispatchedPreviewMetadata = true;
            if (cacheToDispatch != nullptr)
                dispatchedPreviewWaveform = true;

            juce::MessageManager::callAsync([owner = owner,
                                            token = token,
                                            file = file,
                                            info = infoToDispatch,
                                            cache = std::move(cacheToDispatch)]() mutable
            {
                if (owner != nullptr)
                    owner->completeStartupAudioPreview(token, file, info, std::move(cache));
            });
        };

        if (previewInfo.isValid())
            dispatchPreview(previewInfo, previewPeakCache);

        auto preparedFile = SingleTrackDocument::prepareAudioFileForOpen(file);
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        const auto readerPreparedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif

        if (!preparedFile.hasReader())
        {
            juce::MessageManager::callAsync([owner = owner, token = token, file = file]
            {
                if (owner != nullptr)
                    owner->completeStartupAudioPrepare(token, file, {}, {}, {});
            });
            return jobHasFinished;
        }

        if (!previewInfo.isValid())
        {
            previewInfo = preparedFile.info;
            if (previewInfo.isValid() && !dispatchedPreviewMetadata)
                dispatchPreview(previewInfo, {});
        }

        if (previewPeakCache == nullptr && previewInfo.isValid() && preparedFile.hasReader())
        {
            previewPeakCache = WaveformModel::buildLeadingPreviewPeakCacheFromReader(*preparedFile.getReader(),
                                                                                     file.hasFileExtension(".wem") ? 192 : 384,
                                                                                     file.hasFileExtension(".wem") ? 1.0 : 1.5);
            if (previewPeakCache != nullptr && !dispatchedPreviewWaveform)
                dispatchPreview(previewInfo, previewPeakCache);
        }

        auto state = SingleTrackDocument::createStateFromPreparedFile(std::move(preparedFile));
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        const auto stateCreatedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif
        if (state == nullptr)
        {
            juce::MessageManager::callAsync([owner = owner, token = token, file = file]
            {
                if (owner != nullptr)
                    owner->completeStartupAudioPrepare(token, file, {}, {}, {});
            });
            return jobHasFinished;
        }

        if (previewPeakCache == nullptr && previewInfo.isValid())
        {
            previewPeakCache = WaveformModel::buildLeadingPreviewPeakCacheForState(state,
                                                                                   file.hasFileExtension(".wem") ? 192 : 384,
                                                                                   file.hasFileExtension(".wem") ? 1.0 : 1.5);
        }

        if (previewInfo.isValid() && previewPeakCache != nullptr && !dispatchedPreviewWaveform)
            dispatchPreview(previewInfo, previewPeakCache);

        auto initialPreviewPeakCache = previewPeakCache;
        juce::MessageManager::callAsync([owner = owner,
                                        token = token,
                                        file = file,
                                        previewInfo,
                                        previewPeakCache = std::move(initialPreviewPeakCache),
                                        state]() mutable
        {
            if (owner != nullptr)
                owner->completeStartupAudioPrepare(token,
                                                  file,
                                                  previewInfo,
                                                  std::move(previewPeakCache),
                                                  std::move(state));
        });

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        const auto finishedAtMs = juce::Time::getMillisecondCounterHiRes();
        appendStartupTimingLine(juce::Time::getCurrentTime().toISO8601(true)
                                + " | outcome=startup-prepare-stages | file=" + file.getFullPathName()
                                + " | queued " + juce::String(juce::roundToInt(jobStartedAtMs - openRequestedAtMs)) + " ms"
                                + " | cache " + juce::String(juce::roundToInt(cacheLookupFinishedAtMs - jobStartedAtMs)) + " ms"
                                + " | reader " + juce::String(juce::roundToInt(readerPreparedAtMs - cacheLookupFinishedAtMs)) + " ms"
                                + " | state " + juce::String(juce::roundToInt(stateCreatedAtMs - readerPreparedAtMs)) + " ms"
                                + " | post " + juce::String(juce::roundToInt(finishedAtMs - stateCreatedAtMs)) + " ms"
                                + " | total " + juce::String(juce::roundToInt(finishedAtMs - jobStartedAtMs)) + " ms");
#endif
        return jobHasFinished;
    }

private:
    juce::Component::SafePointer<MainWindow> owner;
    juce::File file;
    int64_t token = 0;
    double openRequestedAtMs = 0.0;
};
} // namespace

class MainWindow::TrayIconComponent final : public juce::SystemTrayIconComponent,
                                           private juce::Timer
{
public:
    explicit TrayIconComponent(MainWindow& ownerToUse)
        : owner(ownerToUse)
    {
        const auto iconImage = createFallbackTrayImage();
        setIconImage(iconImage, iconImage);
        setIconTooltip("Single Editor");
        startTimer(50);
    }

    ~TrayIconComponent() override { stopTimer(); worker.reset(); }

    const juce::String& resourceStatus() const noexcept { return status; }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Open Single Editor");
            menu.addSeparator();
            menu.addItem(2, "Quit");
            menu.showMenuAsync(juce::PopupMenu::Options(),
                               [this](int result)
                               {
                                   if (result == 1)
                                       owner.restoreFromBackground();
                                   else if (result == 2)
                                       owner.requestActualQuit();
                               });
            return;
        }

        owner.restoreFromBackground();
    }

private:
    struct PendingImage
    {
        windowShellResources::TrayImage result;
        std::atomic<bool> finished { false };
    };

    void timerCallback() override
    {
        if (!pending)
        {
            const auto* main = owner.getMainComponent();
            if (!main || !main->isCoreReadyForWorkspaceResources()) return;
            pending = std::make_shared<PendingImage>();
            worker = std::make_unique<juce::ThreadPool>(1);
            status = "loading";
            worker->addJob([state = pending]
            {
                state->result = windowShellResources::loadTrayImage();
                state->finished.store(true, std::memory_order_release);
            });
            return;
        }
        if (!pending->finished.load(std::memory_order_acquire)) return;
        worker.reset();
        status = pending->result.status;
        if (pending->result.image.isValid())
            setIconImage(pending->result.image, pending->result.image);
        else
            setIconTooltip("Single Editor - tray artwork unavailable (" + status + ")");
        pending.reset();
        stopTimer();
    }

    MainWindow& owner;
    juce::String status { "waiting-for-core" };
    std::shared_ptr<PendingImage> pending;
    std::unique_ptr<juce::ThreadPool> worker;
};

#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
juce::String MainWindow::trayResourceStatusForTesting() const
{
    return trayIcon ? trayIcon->resourceStatus() : "absent";
}

bool MainWindow::hasNativeTrayIconForTesting() const
{
    return trayIcon && trayIcon->getNativeHandle() != nullptr;
}
#endif

MainWindow::MainWindow(juce::String name,
                       juce::ApplicationProperties& appPropertiesToUse,
                       SingleTrackDocument& document,
                       BnkDocument& bnkDocument,
                       PlaylistModel& playlistModel,
                       AudioEngine& audioEngine,
                       WaveformModel& waveformModel,
                       std::function<void()> prepareClientLookAndFeelToUse,
                       juce::File initialFileToOpen,
                       double startupOpenRequestedAtMsToUse)
    : juce::DocumentWindow(std::move(name),
                           initialFileToOpen.existsAsFile() ? startupCorePalette().windowBackground
                                                           : ModernAppleLookAndFeel::windowBackground(),
                           juce::DocumentWindow::allButtons,
                           false),
      appProperties(appPropertiesToUse),
      document(document),
      bnkDocument(bnkDocument),
      playlistModel(playlistModel),
      audioEngine(audioEngine),
      waveformModel(waveformModel),
      prepareClientLookAndFeel(std::move(prepareClientLookAndFeelToUse)),
      startupHasInitialFile(initialFileToOpen.existsAsFile())
{
    if (AsioSettingsSession::hasSavedSelection(appProperties)) audioEngine.reserveExplicitStartupOutput();
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    const auto bodyEnteredAtMs = juce::Time::getMillisecondCounterHiRes();
#endif
    if (!startupHasInitialFile)
        ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);

    if (startupHasInitialFile)
        startupOpenRequestedAtMs = startupOpenRequestedAtMsToUse > 0.0
                                 ? startupOpenRequestedAtMsToUse
                                 : juce::Time::getMillisecondCounterHiRes();

    // Configure the native title-bar style before creating the heavyweight
    // peer. Changing it on an existing peer makes JUCE rebuild the Win32
    // window, which adds a second synchronous shell creation to Core Open.
    windowShell.configureWindow(*this);
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    const auto shellConfiguredAtMs = juce::Time::getMillisecondCounterHiRes();
#endif
#if JUCE_WINDOWS
    setResizable(true, false);
#else
    setResizable(true, true);
#endif
    const bool modern = RendererSelection::selected() == RendererKind::modern;
    setResizeLimits(modern ? ModernGlassTokens::minimumWindowWidth : 1180,
                    modern ? ModernGlassTokens::minimumWindowHeight : 760, 4096, 2160);
    setColour(juce::ResizableWindow::backgroundColourId,
              startupHasInitialFile ? startupCorePalette().windowBackground
                                    : ModernAppleLookAndFeel::windowBackground());
    restoreWindowState();
    addToDesktop(getDesktopWindowStyleFlags());
#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    const auto windowStateRestoredAtMs = juce::Time::getMillisecondCounterHiRes();
#endif

    if (startupHasInitialFile)
        pendingFileToOpen = initialFileToOpen;

    if (startupHasInitialFile)
    {
        // The startup host has no intrinsic size. Keep the restored window
        // bounds authoritative and fit the host into them; fitting the window
        // around a zero-sized host creates a needless Win32 resize/paint wave.
        setContentOwned(new StartupHostComponent(), false);
        if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
            host->setPendingFile(initialFileToOpen);
    }

    else
    {
        installMainContent();
    }

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    const auto startupContentAttachedAtMs = juce::Time::getMillisecondCounterHiRes();
#endif

    setVisible(true);
    CoreOpenProbe::mark(CoreOpenMilestone::window);
    if (!startupHasInitialFile)
        ensureNativeScaleFactorNotifier();
    requestStartupKeyboardFocus();
    if (startupHasInitialFile)
    {
        startupWindowShownAtMs = juce::Time::getMillisecondCounterHiRes();

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
        appendStartupTimingLine(juce::Time::getCurrentTime().toISO8601(true)
                                + " | outcome=startup-window-stages"
                                + " | body-entry " + juce::String(juce::roundToInt(bodyEnteredAtMs - startupOpenRequestedAtMs)) + " ms"
                                + " | shell " + juce::String(juce::roundToInt(shellConfiguredAtMs - bodyEnteredAtMs)) + " ms"
                                + " | restore " + juce::String(juce::roundToInt(windowStateRestoredAtMs - shellConfiguredAtMs)) + " ms"
                                + " | content " + juce::String(juce::roundToInt(startupContentAttachedAtMs - windowStateRestoredAtMs)) + " ms"
                                + " | reveal " + juce::String(juce::roundToInt(startupWindowShownAtMs - startupContentAttachedAtMs)) + " ms"
                                + " | total " + juce::String(juce::roundToInt(startupWindowShownAtMs - startupOpenRequestedAtMs)) + " ms");
#endif

        beginStartupAudioPrepare(initialFileToOpen);
    }
}

MainWindow::~MainWindow()
{
    traceShutdown("window.destructor.begin");
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    runtimePlaybackProbe.reset();
#endif
    flushPendingWindowStatePersist();
    stopTimer();
    traceShutdown("window.startup-jobs.cancel.begin");
    startupPrepareThreadPool.removeAllJobs(true, 4000);
    traceShutdown("window.startup-jobs.cancel.end");
}

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
void MainWindow::startRuntimeProbeAfterCoreReady()
{
    if (runtimePlaybackProbe != nullptr) return;
    const auto path = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_RUNTIME_PROBE_PATH", {});
    if (path.isEmpty()) return;
    const auto requestedSeconds = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_RUNTIME_PROBE_SECONDS", {});
    const int seconds = juce::jlimit(5, 600, requestedSeconds.isEmpty() ? 180 : requestedSeconds.getIntValue());
    runtimePlaybackProbe = RuntimePlaybackProbe::create(path, seconds * 1000, [this]
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("playing", audioEngine.isPlaying());
        row->setProperty("position_seconds", audioEngine.getCurrentPositionSeconds());
        row->setProperty("window_showing", isShowing());
        row->setProperty("window_x", getScreenX());
        row->setProperty("window_y", getScreenY());
        row->setProperty("window_width", getWidth());
        row->setProperty("window_height", getHeight());
        const auto* peer = getPeer();
        row->setProperty("peer_scale_factor", peer != nullptr
            ? juce::var(peer->getPlatformScaleFactor()) : juce::var());
        row->setProperty("night", RendererVisualStyle::isNight());
        row->setProperty("glass_material", static_cast<int>(RendererVisualStyle::glassMaterial()));
        row->setProperty("device", juce::var());
        if (const auto metrics = audioEngine.getRuntimeMetricsForDiagnostics())
        {
            auto* device = new juce::DynamicObject();
            device->setProperty("name", metrics->deviceName);
            device->setProperty("sample_rate", metrics->sampleRate);
            device->setProperty("buffer_samples", metrics->bufferSamples);
            device->setProperty("callback_load_fraction", metrics->callbackLoadFraction >= 0 ? juce::var(metrics->callbackLoadFraction) : juce::var());
            device->setProperty("total_xruns", metrics->totalXruns >= 0 ? juce::var(metrics->totalXruns) : juce::var());
            device->setProperty("driver_xruns", metrics->driverXruns);
            device->setProperty("audio_suite_preview_configured", metrics->audioSuitePreviewConfigured);
            row->setProperty("device", juce::var(device));
        }
        return juce::var(row);
    });
}

#endif

MainComponent* MainWindow::getMainComponent() const noexcept
{
    if (auto* main = dynamic_cast<MainComponent*>(getContentComponent()))
        return main;

    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
        return host->getMainComponent();

    return nullptr;
}

void MainWindow::openFileWhenReady(const juce::File& file)
{
    if (!file.existsAsFile())
        return;

    pendingFileToOpen = file;
    if (mainContentInstalled)
    {
        if (auto* main = getMainComponent())
        {
            main->openInitialFile(file);
            pendingFileToOpen = {};
        }
        return;
    }

    beginStartupAudioPrepare(file);
}

juce::Component::WindowControlKind MainWindow::findControlAtPoint(juce::Point<float> point) const
{
    const auto chrome = WindowsWindowShell::findControlAtPoint(*this, point);
    if (chrome != WindowControlKind::client) return chrome;
    if (const auto* main = getMainComponent())
        return main->findControlAtPoint(main->getLocalPoint(this, point));
    return chrome;
}

void MainWindow::restoreFromBackground()
{
    hiddenToBackground = false;
    ensureTrayIconVisible(false);
    setVisible(true);
    setMinimised(false);
    toFront(true);
}

void MainWindow::requestStartupKeyboardFocus()
{
    auto* main = getMainComponent();
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    auto focusLocation = StartupFocusLocation::none;
    if (focused != nullptr
        && main != nullptr
        && (focused == main || main->isParentOf(focused)))
    {
        focusLocation = StartupFocusLocation::mainContent;
    }
    else if (focused != nullptr && (focused == this || isParentOf(focused)))
    {
        focusLocation = StartupFocusLocation::windowChrome;
    }

    const auto decision = decideStartupFocus({ isShowing(),
                                               isActiveWindow(),
                                               main != nullptr,
                                               focusLocation });

    if (decision.activateWindow)
        toFront(true);

    if (!decision.focusMainContent || main == nullptr)
        return;

    juce::Component::SafePointer<MainComponent> safeMain(main);
    juce::MessageManager::callAsync([safeMain]
    {
        if (safeMain == nullptr)
            return;

        auto* top = safeMain->getTopLevelComponent();
        auto* focusedNow = juce::Component::getCurrentlyFocusedComponent();
        const auto focusInsideMain = focusedNow != nullptr
                                  && (focusedNow == safeMain.getComponent()
                                      || safeMain->isParentOf(focusedNow));
        if (top != nullptr
            && juce::TopLevelWindow::getActiveTopLevelWindow() == top
            && !focusInsideMain)
        {
            safeMain->grabKeyboardFocus();
        }
    });
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged();
    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
        host->noteHostMutation(StartupHostMutation::activate);
    if (isActiveWindow())
        requestStartupKeyboardFocus();
}

void MainWindow::requestUserCloseAction()
{
    if (shouldCloseToBackground())
    {
        hideToBackground();
        return;
    }

    requestActualQuit();
}

void MainWindow::requestActualQuit()
{
    hiddenToBackground = false;
    trayIcon.reset();
    if (auto* main = getMainComponent())
        main->requestCloseApplication();
    else if (auto* app = juce::JUCEApplication::getInstance())
        app->quit();
}

void MainWindow::closeButtonPressed()
{
    requestUserCloseAction();
}

bool MainWindow::keyPressed(const juce::KeyPress& key)
{
    traceNativeFocus("window.key", *this, &key);
    if (auto* main = getMainComponent(); main != nullptr && main->handleTopLevelKeyPress(key))
        return true;

    return juce::DocumentWindow::keyPressed(key);
}

void MainWindow::moved()
{
    juce::DocumentWindow::moved();
    requestWindowStatePersist();
}

void MainWindow::resized()
{
    juce::DocumentWindow::resized();
    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
        host->noteHostMutation(isFullScreen() ? StartupHostMutation::maximize
                                              : StartupHostMutation::resize);
    requestWindowStatePersist();
}

bool MainWindow::shouldCloseToBackground() const
{
    if (auto* settings = appProperties.getUserSettings())
        return settings->getBoolValue(PreferencesComponent::closeToTrayOnCloseKey, false);

    return false;
}

void MainWindow::hideToBackground()
{
    if (audioEngine.isPlaying())
        audioEngine.stop();

    hiddenToBackground = true;
    ensureTrayIconVisible(true);
    setVisible(false);
}

void MainWindow::beginStartupAudioPrepare(const juce::File& file)
{
    const auto token = startupPrepareToken.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        const juce::ScopedLock sl(startupPrepareLock);
        startupPrepareCompleted = false;
        preparedStartupPreview.reset();
        preparedStartupAudio.reset();
    }

    startupPreviewFileNameElapsedMs.reset();
    startupPreviewWaveformElapsedMs.reset();

    // Optional video/WEM reaches Main's lazy session failure owner once. Do not
    // decode here and automatically decode again through the fallback open.
    if (file.hasFileExtension("wem") || WindowsMediaAudioService::isSupportedVideoExtension(file.getFileExtension()))
    { completeStartupAudioPrepare(token, file, {}, {}, {}); return; }

    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
    {
        host->setPendingFile(file);
        host->setPreviewState({}, {});
    }

    if (auto* main = getMainComponent(); main != nullptr)
    {
        if (auto startupPreview = WaveformModel::loadPersistentStartupPreviewForFile(file);
            startupPreview.has_value()
            && startupPreview->info.isValid()
            && startupPreview->cache != nullptr)
        {
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           std::optional<double> {},
                                           std::optional<double> {},
                                           std::optional<double> {});

            main->showPreparedInitialAudioPreview(file,
                                                  startupPreview->info,
                                                  startupPreview->cache,
                                                  false);
        }
    }

    startupPrepareThreadPool.removeAllJobs(false, 0);
    const auto prepareRequestedAtMs = startupOpenRequestedAtMs > 0.0
                                        ? startupOpenRequestedAtMs
                                        : juce::Time::getMillisecondCounterHiRes();
    startupPrepareThreadPool.addJob(new StartupAudioPrepareJob(juce::Component::SafePointer<MainWindow>(this),
                                                              file,
                                                              token,
                                                              prepareRequestedAtMs),
                                    true);
}

void MainWindow::completeStartupAudioPreview(int64_t token,
                                             const juce::File& file,
                                             SingleTrackDocument::FileInfo previewInfo,
                                             std::shared_ptr<WaveformModel::PeakCache> previewPeakCache)
{
    if (token != startupPrepareToken.load(std::memory_order_relaxed) || !previewInfo.isValid())
        return;

    const auto hasPreviewWaveform = previewPeakCache != nullptr;
    auto startupPrepareStillRunning = false;
    {
        const juce::ScopedLock sl(startupPrepareLock);
        startupPrepareStillRunning = !startupPrepareCompleted;
    }

    if (mainContentInstalled)
    {
        if (auto* main = getMainComponent())
        {
            const auto startupPrimaryContentElapsedMs = (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                ? std::optional<double>(juce::Time::getMillisecondCounterHiRes() - startupOpenRequestedAtMs)
                : std::optional<double> {};
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           startupPrimaryContentElapsedMs,
                                           startupPreviewFileNameElapsedMs,
                                           startupPreviewWaveformElapsedMs);
            if (previewPeakCache != nullptr)
                main->showPreparedInitialAudioPreview(file,
                                                      previewInfo,
                                                      std::move(previewPeakCache),
                                                      false);
            else if (!startupPrepareStillRunning)
                main->beginPreparedInitialAudioLoad(file, previewInfo);
        }

        return;
    }

    if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
    {
        const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startupOpenRequestedAtMs;
        if (!startupPreviewFileNameElapsedMs.has_value())
            startupPreviewFileNameElapsedMs = elapsedMs;
        if (previewPeakCache != nullptr && !startupPreviewWaveformElapsedMs.has_value())
            startupPreviewWaveformElapsedMs = elapsedMs;
    }

    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
        host->setPreviewState(previewInfo, previewPeakCache);

    if (hasPreviewWaveform)
        startupPreviewSurfaceShownAtMs = juce::Time::getMillisecondCounterHiRes();

    {
        const juce::ScopedLock sl(startupPrepareLock);
        if (preparedStartupAudio.has_value()
            && preparedStartupAudio->file == file
            && preparedStartupAudio->previewInfo.file == previewInfo.file)
        {
            preparedStartupAudio->previewInfo = previewInfo;
            if (previewPeakCache != nullptr)
                preparedStartupAudio->previewPeakCache = previewPeakCache;
        }

        if (preparedStartupPreview.has_value()
            && preparedStartupPreview->file == file
            && preparedStartupPreview->previewInfo.file == previewInfo.file)
        {
            if (previewPeakCache != nullptr)
                preparedStartupPreview->previewPeakCache = std::move(previewPeakCache);
        }
        else
        {
            preparedStartupPreview = PreparedStartupAudioPreview { file, previewInfo, std::move(previewPeakCache) };
        }
    }

    if (hasPreviewWaveform)
    {
        // The startup host now keeps the placeholder visible while MainComponent
        // attaches, so we can hand off to the real editor as soon as we have a
        // truthful preview waveform instead of waiting for the full state.
        scheduleMainContentInstallAfterStartupPreview(true);
    }
}

void MainWindow::completeStartupAudioPrepare(int64_t token,
                                             const juce::File& file,
                                             SingleTrackDocument::FileInfo previewInfo,
                                             std::shared_ptr<WaveformModel::PeakCache> previewPeakCache,
                                             std::shared_ptr<SingleTrackDocument::State> state)
{
    if (token != startupPrepareToken.load(std::memory_order_relaxed))
        return;

    {
        const juce::ScopedLock sl(startupPrepareLock);
        startupPrepareCompleted = true;
    }

    if (mainContentInstalled)
    {
        if (auto* main = getMainComponent())
        {
            const auto startupPrimaryContentElapsedMs = (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                ? std::optional<double>(juce::Time::getMillisecondCounterHiRes() - startupOpenRequestedAtMs)
                : std::optional<double> {};
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           startupPrimaryContentElapsedMs,
                                           startupPreviewFileNameElapsedMs,
                                           startupPreviewWaveformElapsedMs);

            if (state != nullptr && previewInfo.isValid())
            {
                main->openPreparedInitialAudio(file,
                                               previewInfo,
                                               std::move(previewPeakCache),
                                               std::move(state));
            }
            else if (previewInfo.isValid())
            {
                if (previewPeakCache != nullptr)
                    main->showPreparedInitialAudioPreview(file, previewInfo, previewPeakCache);
                else
                    main->beginPreparedInitialAudioLoad(file, previewInfo);
            }
            else
            {
                main->openInitialFile(file);
            }
        }

        pendingFileToOpen = {};
        return;
    }

    {
        const juce::ScopedLock sl(startupPrepareLock);
        if (state != nullptr)
        {
            preparedStartupPreview.reset();
            preparedStartupAudio = PreparedStartupAudioLoad { file, previewInfo, std::move(previewPeakCache), std::move(state) };
        }
        else
        {
            preparedStartupAudio.reset();
        }
    }

    scheduleMainContentInstallAfterStartupPreview(true);
}

void MainWindow::scheduleMainContentInstallAfterStartupPreview(bool preferImmediateOnMessageThread)
{
    if (mainContentInstalled || mainContentInstallScheduled)
        return;

    if (!startupHasInitialFile || startupPreviewSurfaceShownAtMs <= 0.0)
    {
        initialiseMainContentAsync(preferImmediateOnMessageThread);
        return;
    }

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - startupPreviewSurfaceShownAtMs;
    const auto remainingMs = startupPreviewMinimumVisibleMs - static_cast<int>(std::floor(elapsedMs));
    if (remainingMs <= 0)
    {
        initialiseMainContentAsync(preferImmediateOnMessageThread);
        return;
    }

    if (mainContentInstallDelayScheduled)
        return;

    mainContentInstallDelayScheduled = true;
    juce::Component::SafePointer<MainWindow> safeThis(this);
    juce::Timer::callAfterDelay(remainingMs,
                                [safeThis, preferImmediateOnMessageThread]
                                {
                                    if (safeThis == nullptr)
                                        return;

                                    safeThis->mainContentInstallDelayScheduled = false;
                                    safeThis->initialiseMainContentAsync(preferImmediateOnMessageThread);
                                });
}

void MainWindow::initialiseMainContentAsync(bool preferImmediateOnMessageThread)
{
    if (mainContentInstalled || mainContentInstallScheduled)
        return;

    if (preferImmediateOnMessageThread)
    {
        if (auto* messageManager = juce::MessageManager::getInstance(); messageManager != nullptr && messageManager->isThisTheMessageThread())
        {
            installMainContent();
            return;
        }
    }

    mainContentInstallScheduled = true;
    juce::Component::SafePointer<MainWindow> safeThis(this);
    juce::MessageManager::callAsync([safeThis]
    {
        if (safeThis != nullptr)
            safeThis->installMainContent();
    });
}

void MainWindow::installMainContent()
{
    if (mainContentInstalled)
        return;

    mainContentInstallScheduled = false;
    mainContentInstallDelayScheduled = false;
    if (!startupHasInitialFile && prepareClientLookAndFeel)
        prepareClientLookAndFeel();

    std::optional<PreparedStartupAudioLoad> preparedAudio;
    std::optional<PreparedStartupAudioPreview> preparedPreview;
    auto shouldFallbackOpenPendingFile = false;
    auto startupPrepareStillRunning = false;
    {
        const juce::ScopedLock sl(startupPrepareLock);
        preparedPreview = preparedStartupPreview;
        preparedAudio = preparedStartupAudio;
        shouldFallbackOpenPendingFile = startupPrepareCompleted && !preparedStartupAudio.has_value() && pendingFileToOpen.existsAsFile();
        startupPrepareStillRunning = !startupPrepareCompleted;
        if (preparedPreview.has_value())
            preparedStartupPreview.reset();
        if (preparedAudio.has_value())
            preparedStartupAudio.reset();
    }

    auto mainContent = std::make_unique<MainComponent>(*this,
                                                       appProperties,
                                                       document,
                                                       bnkDocument,
                                                       playlistModel,
                                                       audioEngine,
                                                       waveformModel,
                                                       startupHasInitialFile,
                                                       windowShell.providesWindowTitleBar());
    auto* main = mainContent.get();
    main->onSimpleViewChanged = [this] (bool simple)
    {
        WindowsWindowShell::setSimplePresentation(*this, simple);
        resized();
    };

    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
    {
        const auto safeHost = juce::Component::SafePointer<StartupHostComponent>(host);
        const auto safeWindow = juce::Component::SafePointer<MainWindow>(this);
        main->setStartupCoreSurfacePreparedCallback([safeHost]
        {
            if (safeHost != nullptr)
                safeHost->coreSurfacePrepared();
        });
        main->setStartupCoreReadyCallback([safeHost, safeWindow]
        {
            if (safeHost != nullptr)
                safeHost->coreReady();
            if (safeWindow != nullptr)
            {
                safeWindow->ensureNativeScaleFactorNotifier();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                // Test targets may observe the real boundary without delaying it.
                safeWindow->startRuntimeProbeAfterCoreReady();
#endif
            }
        });
        main->setStartupRendererPrepareCallback([safeWindow]
        {
            if (safeWindow != nullptr && safeWindow->prepareClientLookAndFeel)
                safeWindow->prepareClientLookAndFeel();
        });
        main->setStartupRendererReadyCallback([safeHost, safeWindow](bool succeeded)
        {
            if (safeHost != nullptr)
                safeHost->rendererReady(succeeded);
            // A completed error surface also has the full skin, although its
            // document is neither playable nor CoreReady. Refresh only chrome;
            // the false result above must still reach the startup coordinator.
            if (safeWindow != nullptr && !RendererVisualStyle::isClassic()
                && (succeeded || !ModernAppleLookAndFeel::isStartupSkin()))
                ModernAppleLookAndFeel::refreshTheme(*safeWindow);
        });
    }

    if (main != nullptr)
    {
        const auto startupPrimaryContentElapsedMs = (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
            ? std::optional<double>(juce::Time::getMillisecondCounterHiRes() - startupOpenRequestedAtMs)
            : std::optional<double> {};
        if (!preparedAudio.has_value() && preparedPreview.has_value())
        {
            auto preview = std::move(*preparedPreview);
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(preview.file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           startupPrimaryContentElapsedMs,
                                           startupPreviewFileNameElapsedMs,
                                           startupPreviewWaveformElapsedMs);
            if (preview.previewPeakCache != nullptr)
                main->showPreparedInitialAudioPreview(preview.file,
                                                      preview.previewInfo,
                                                      std::move(preview.previewPeakCache),
                                                      !startupPrepareStillRunning);
            else if (!startupPrepareStillRunning)
                main->beginPreparedInitialAudioLoad(preview.file, preview.previewInfo);
        }

        if (preparedAudio.has_value())
        {
            auto prepared = std::move(*preparedAudio);
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(prepared.file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           startupPrimaryContentElapsedMs,
                                           startupPreviewFileNameElapsedMs,
                                           startupPreviewWaveformElapsedMs);
            pendingFileToOpen = {};
            if (prepared.previewInfo.isValid() && prepared.previewPeakCache != nullptr)
                main->showPreparedInitialAudioPreview(prepared.file,
                                                      prepared.previewInfo,
                                                      prepared.previewPeakCache,
                                                      false);
            main->openPreparedInitialAudio(prepared.file,
                                           prepared.previewInfo,
                                           std::move(prepared.previewPeakCache),
                                           std::move(prepared.state));
        }
        else if (shouldFallbackOpenPendingFile)
        {
            const auto file = pendingFileToOpen;
            if (startupHasInitialFile && startupOpenRequestedAtMs > 0.0)
                main->setStartupOpenTiming(file,
                                           startupOpenRequestedAtMs,
                                           startupWindowShownAtMs,
                                           startupPrimaryContentElapsedMs,
                                           startupPreviewFileNameElapsedMs,
                                           startupPreviewWaveformElapsedMs);
            pendingFileToOpen = {};
            main->openInitialFile(file);
        }
    }

    if (auto* host = dynamic_cast<StartupHostComponent*>(getContentComponent()))
        host->setMainContent(std::move(mainContent));
    else
        // Empty startup has already restored the outer window bounds too.
        // Fit the editor into them instead of resizing to its initial zero size,
        // which otherwise clamps the saved window down to the minimum size.
        setContentOwned(mainContent.release(), false);

    mainContentInstalled = true;
    requestStartupKeyboardFocus();

    main->restoreSavedAudioOutputAfterStartupHandoff();
    audioEngine.warmUpAudioDeviceAsync(startupHasInitialFile ? 1600 : 900);

    juce::Component::SafePointer<MainWindow> safeThis(this);
    juce::Timer::callAfterDelay(startupHasInitialFile ? startupTooltipDelayMs : normalTooltipDelayMs,
                                [safeThis]
                                {
                                    if (safeThis == nullptr || safeThis->tooltipWindow != nullptr)
                                        return;

                                    safeThis->tooltipWindow = std::make_unique<juce::TooltipWindow>(safeThis.getComponent(), 300);
                                    safeThis->tooltipWindow->setMillisecondsBeforeTipAppears(250);
                                });
    resized();
}

void MainWindow::ensureNativeScaleFactorNotifier()
{
    if (nativeScaleFactorNotifier != nullptr)
        return;

    nativeScaleFactorNotifier = std::make_unique<juce::NativeScaleFactorNotifier>(
        this,
        [safeThis = juce::Component::SafePointer<MainWindow>(this)](float)
        {
            if (safeThis == nullptr)
                return;

            if (auto* host = dynamic_cast<StartupHostComponent*>(safeThis->getContentComponent()))
            {
                host->noteHostMutation(StartupHostMutation::dpiChange);
                host->repaint();
            }
        });
}

void MainWindow::ensureTrayIconVisible(bool shouldBeVisible)
{
    if (shouldBeVisible)
    {
        if (trayIcon == nullptr)
            trayIcon = std::make_unique<TrayIconComponent>(*this);
        return;
    }

    trayIcon.reset();
}

void MainWindow::restoreWindowState()
{
    auto restored = false;

    if (auto* settings = appProperties.getUserSettings())
    {
        const auto encoded = settings->getValue(windowBoundsKey);
        if (encoded.isNotEmpty())
        {
            auto bounds = juce::Rectangle<int>::fromString(encoded);
            const auto modern = RendererSelection::selected() == RendererKind::modern;
            const auto minimumWidth = modern ? ModernGlassTokens::minimumWindowWidth : 900;
            // Older Modern windows persisted client bounds without the Windows
            // caption/frame. Keep their compact position and grow only to the new
            // outer minimum, rather than unexpectedly reopening at the default size.
            if (modern && bounds.getWidth() >= minimumWidth - 64 && bounds.getHeight() >= 536)
                bounds.setSize(juce::jmax(minimumWidth, bounds.getWidth()), juce::jmax(600, bounds.getHeight()));
            if (bounds.getWidth() >= minimumWidth && bounds.getHeight() >= 600)
            {
                const auto storedVersion = settings->getIntValue(windowBoundsVersionKey, 0);
                const auto savedBounds = bounds;
                if (storedVersion < currentWindowBoundsVersion)
                {
                    bounds.setSize(juce::jmax(bounds.getWidth(), defaultWindowWidth),
                                   juce::jmax(bounds.getHeight(), defaultWindowHeight));
                    bounds = constrainWindowBoundsToDisplay(bounds);
                }
                else
                {
                    const auto& displays = juce::Desktop::getInstance().getDisplays().displays;
                    const auto captionReachable = std::any_of(displays.begin(), displays.end(),
                        [bounds] (const auto& display)
                        {
                            return windowPlacement::hasReachableCaption(bounds, display.userArea);
                        });
                    if (!displays.isEmpty() && !captionReachable)
                        bounds = constrainWindowBoundsToDisplay(bounds);
                }

                setBounds(bounds);
                if (storedVersion >= currentWindowBoundsVersion)
                    lastPersistedWindowBounds = savedBounds;
                restored = true;
            }
        }
    }

    if (!restored)
        centreWithSize(defaultWindowWidth, defaultWindowHeight);
}

void MainWindow::persistWindowState()
{
    if (hiddenToBackground)
        return;

    const auto currentBounds = getBounds();
    if (lastPersistedWindowBounds.has_value() && *lastPersistedWindowBounds == currentBounds)
        return;

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue(windowBoundsKey, currentBounds.toString());
        settings->setValue(windowBoundsVersionKey, currentWindowBoundsVersion);
        settings->saveIfNeeded();
        lastPersistedWindowBounds = currentBounds;
    }
}

void MainWindow::requestWindowStatePersist(int delayMs)
{
    if (hiddenToBackground)
        return;

    if (delayMs < 0)
    {
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();
        const auto stillInStartupWindow = startupWindowShownAtMs <= 0.0
                                       || (nowMs - startupWindowShownAtMs) < startupWindowStatePersistDelayMs
                                       || !mainContentInstalled;
        delayMs = stillInStartupWindow ? startupWindowStatePersistDelayMs
                                       : normalWindowStatePersistDelayMs;
    }

    windowStatePersistPending = true;
    windowStatePersistDueAtMs = juce::Time::getMillisecondCounterHiRes() + juce::jmax(100, delayMs);
    startTimer(120);
}

void MainWindow::flushPendingWindowStatePersist()
{
    if (!windowStatePersistPending)
        return;

    windowStatePersistPending = false;
    windowStatePersistDueAtMs = 0.0;
    persistWindowState();
}

void MainWindow::timerCallback()
{
    if (!windowStatePersistPending)
    {
        stopTimer();
        return;
    }

    if (juce::Time::getMillisecondCounterHiRes() >= windowStatePersistDueAtMs)
    {
        // Keep the request pending across a slow open. The explicit shutdown
        // flush remains unconditional so closing early retains window settings.
        const auto* main = getMainComponent();
        if (main == nullptr || !main->canPersistDeferredSettings())
            return;
        flushPendingWindowStatePersist();
        stopTimer();
    }
}
} // namespace singleeditor
