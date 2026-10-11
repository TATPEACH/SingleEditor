#include "UI/MainComponent.h"
#include "App/MainWindow.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace singleeditor
{
struct DeferredCacheWarmTestAccess
{
    // The same assertions exercise the old eager pool and the deferred owner.
    template <typename Main> static juce::ThreadPool* pool(Main& main)
    {
        if constexpr (std::is_same_v<decltype(main.cacheWarmThreadPool), juce::ThreadPool>)
            return &main.cacheWarmThreadPool;
        else
            return main.cacheWarmThreadPool.get();
    }
    static CoreLifecycleCoordinator::Generation begin(MainComponent& main)
    {
        main.beginDocumentGeneration({});
        main.loadInProgress = false;
        return main.activeDocumentGeneration;
    }
    static void playable(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.schedulePostCorePromotionIfNeeded(main.coreLifecycle.markPlayable(generation)); }
    static void waveform(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.schedulePostCorePromotionIfNeeded(main.coreLifecycle.markFirstWaveform(generation)); }
    static void schedule(MainComponent& main, const juce::File& file, int delay)
    { main.scheduleCacheWarm({file}, delay, false); }
    static void closing(MainComponent& main, bool value) { main.closeApplicationPending = value; }
    static bool loading(const MainComponent& main) { return main.loadInProgress; }
    static CoreLifecycleCoordinator::Generation generation(const MainComponent& main)
    { return main.activeDocumentGeneration; }
    static void open(MainComponent& main, const juce::File& file) { main.openFiles({ file }); }
};

namespace
{
void writeWarmWave(const juce::File& file, int samples = 4800)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    juce::AudioBuffer<float> pcm(2, samples);
    for (int c = 0; c < pcm.getNumChannels(); ++c)
        for (int n = 0; n < pcm.getNumSamples(); ++n)
            pcm.setSample(c, n, (c ? -1.0f : 1.0f) * static_cast<float>((n % 31) - 15) / 64.0f);
    if (!writer || !writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()))
        throw std::runtime_error("Cannot write private cache-warm PCM fixture");
}
void writeLargeWarmWave(const juce::File& file)
{
    // Cross the actual 256 MiB float PCM mutation boundary, using a compact
    // eight-bit source and bounded buffers rather than changing product limits.
    constexpr int64_t frames = (256LL * 1024 * 1024) / (2 * sizeof(float)) + 1;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(8);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    juce::AudioBuffer<float> pcm(2, 65536);
    for (int n = 0; n < pcm.getNumSamples(); ++n)
    {
        pcm.setSample(0, n, 0.25f);
        pcm.setSample(1, n, -0.25f);
    }
    if (!writer) throw std::runtime_error("Cannot create bounded large cache fixture");
    for (int64_t offset = 0; offset < frames; offset += pcm.getNumSamples())
        if (!writer->writeFromAudioSampleBuffer(pcm, 0, static_cast<int>(juce::jmin<int64_t>(pcm.getNumSamples(), frames - offset))))
            throw std::runtime_error("Cannot write bounded large cache fixture");
}
void pumpFor(int milliseconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + milliseconds;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        MSG message {};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) throw std::runtime_error("Unexpected native quit during cache-warm fixture");
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        juce::Thread::sleep(1);
    }
}
bool waitForCache(const juce::File& file)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + 2500.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        if (WaveformModel::hasPersistentStartupPreviewCache(file)) return true;
        pumpFor(5);
    }
    return false;
}
}

int runDeferredCacheWarmTests(const juce::File& artifacts)
{
    if (artifacts.exists() || artifacts.createDirectory().failed())
        throw std::runtime_error("Use a fresh deferred-cache-warm artifact directory");
    const auto cache = artifacts.getChildFile("private-waveform-cache");
    const auto oldLocalData = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    const auto oldCacheRoot = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_NATIVE_TEST_WAVEFORM_CACHE_ROOT", {});
    if (_wputenv_s(L"LOCALAPPDATA", artifacts.getChildFile("private-local-appdata").getFullPathName().toWideCharPointer()) != 0)
        throw std::runtime_error("Cannot isolate this native child's recovery directory");
    if (_wputenv_s(L"SINGLE_EDITOR_NATIVE_TEST_WAVEFORM_CACHE_ROOT", cache.getFullPathName().toWideCharPointer()) != 0)
        throw std::runtime_error("Cannot isolate this native child's waveform cache");
    struct ResetRoots
    {
        juce::String localData, waveform;
        ~ResetRoots()
        {
            _wputenv_s(L"LOCALAPPDATA", localData.toWideCharPointer());
            _wputenv_s(L"SINGLE_EDITOR_NATIVE_TEST_WAVEFORM_CACHE_ROOT", waveform.toWideCharPointer());
        }
    } resetRoots { oldLocalData, oldCacheRoot };
    int failures = 0;
    juce::Array<juce::var> results;
    const auto check = [&](bool ok, const char* name)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("name", name); item->setProperty("status", ok ? "pass" : "failed");
        results.add(juce::var(item));
        if (!ok) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    };
    const auto firstFile = artifacts.getChildFile("before-waveform.wav");
    const auto staleFile = artifacts.getChildFile("stale-generation.wav");
    const auto closingFile = artifacts.getChildFile("closing.wav");
    const auto destroyedFile = artifacts.getChildFile("destroyed.wav");
    const auto emptyFile = artifacts.getChildFile("empty-workspace.wav");
    for (const auto& file : {firstFile, staleFile, closingFile, destroyedFile, emptyFile}) writeWarmWave(file);
    juce::ApplicationProperties properties;
    juce::PropertiesFile::Options options;
    options.applicationName = "SingleEditorDeferredCacheWarmTest";
    options.filenameSuffix = "settings"; options.folderName = artifacts.getFullPathName();
    options.millisecondsBeforeSaving = -1; properties.setStorageParameters(options);
    properties.getUserSettings()->setValue("restoreLastSessionOnStartup", false);
    RendererSelection::resetForTests(RendererKind::modern);
    WindowChromeLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    struct ResetLook { ~ResetLook() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); } } resetLook;
    SingleTrackDocument document;
    BnkDocument bank;
    PlaylistModel playlist;
    AudioEngine engine(document);
    if (!engine.reserveExplicitStartupOutput()) throw std::runtime_error("Reserve native fixture output without opening a device");
    WaveformModel waveform(document);
    juce::DocumentWindow host("Deferred cache warming", juce::Colours::black, juce::DocumentWindow::allButtons, false);
    host.setBounds(60, 60, 1180, 760);
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, true, true);
        host.setContentNonOwned(main.get(), false);
        const auto generation = DeferredCacheWarmTestAccess::begin(*main);
        check(DeferredCacheWarmTestAccess::pool(*main) == nullptr,
              "ordinary startup creates no nonessential cache-warm pool or thread");
        DeferredCacheWarmTestAccess::playable(*main, generation);
        DeferredCacheWarmTestAccess::schedule(*main, firstFile, 20);
        check(!cache.exists(), "scheduling before CoreReady does not inspect or create the persistent-cache directory");
        pumpFor(200);
        check(!main->isCoreReadyForWorkspaceResources()
                  && !WaveformModel::hasPersistentStartupPreviewCache(firstFile),
              "elapsed warm delay and playable alone cannot read and cache another file");
        check(DeferredCacheWarmTestAccess::pool(*main) == nullptr,
              "elapsed delay retains absent cache-warm worker before first waveform");
        DeferredCacheWarmTestAccess::waveform(*main, generation);
        check(waitForCache(firstFile), "real deferred worker produces a valid preview cache after both current milestones");
        check(DeferredCacheWarmTestAccess::pool(*main) != nullptr,
              "positive cache request creates its worker only after CoreReady");
        DeferredCacheWarmTestAccess::schedule(*main, staleFile, 100);
        const auto next = DeferredCacheWarmTestAccess::begin(*main);
        DeferredCacheWarmTestAccess::playable(*main, generation);
        DeferredCacheWarmTestAccess::waveform(*main, next);
        pumpFor(160);
        check(!WaveformModel::hasPersistentStartupPreviewCache(staleFile),
              "a delayed old-generation warm request cannot run during replacement CoreReady");
        check(!main->isCoreReadyForWorkspaceResources(), "old playable does not satisfy replacement generation");
        DeferredCacheWarmTestAccess::playable(*main, next);
        DeferredCacheWarmTestAccess::schedule(*main, closingFile, 40);
        DeferredCacheWarmTestAccess::closing(*main, true);
        pumpFor(100);
        check(!WaveformModel::hasPersistentStartupPreviewCache(closingFile),
              "pending close prevents delayed cache admission");
        DeferredCacheWarmTestAccess::closing(*main, false);
        DeferredCacheWarmTestAccess::schedule(*main, destroyedFile, 80);
        host.clearContentComponent();
    }
    pumpFor(120);
    check(!WaveformModel::hasPersistentStartupPreviewCache(destroyedFile),
          "normal native destruction invalidates the delayed owner callback");
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, false, true);
        host.setContentNonOwned(main.get(), false);
        DeferredCacheWarmTestAccess::schedule(*main, emptyFile, 20);
        pumpFor(100);
        check(!WaveformModel::hasPersistentStartupPreviewCache(emptyFile)
                  && DeferredCacheWarmTestAccess::pool(*main) == nullptr,
              "unshown empty workspace does not admit background cache work");
        host.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        host.setVisible(true);
        check(main->isShowing(), "empty-workspace fixture has a real visible native peer");
        check(waitForCache(emptyFile) && !main->isCoreReadyForWorkspaceResources(),
              "visible initially empty workspace retains valid warming without inventing audio CoreReady");
        host.setVisible(false);
        host.clearContentComponent();
        host.removeFromDesktop();
    }
    // A startup prepare worker must not serialize ahead of its queued UI
    // publication. Deliberately withhold the message loop, so no real audio
    // CoreReady can be reached while that worker finishes reading the file.
    const auto startupFile = artifacts.getChildFile("startup-before-core.wav");
    const auto loadedFile = artifacts.getChildFile("loaded-before-core.wav");
    const auto currentCache = artifacts.getChildFile("current-document-cache");
    if (_wputenv_s(L"SINGLE_EDITOR_NATIVE_TEST_WAVEFORM_CACHE_ROOT", currentCache.getFullPathName().toWideCharPointer()) != 0)
        throw std::runtime_error("Cannot isolate current-document cache admission");
    writeWarmWave(startupFile);
    writeWarmWave(loadedFile, 48000 * 8);
    {
        MainWindow window("Startup cache admission", properties, document, bank, playlist,
                          engine, waveform, {}, startupFile);
        juce::Thread::sleep(1800);
        check(window.getMainComponent() == nullptr && !document.hasAudio(),
              "startup prepare fixture withholds actual client publication and CoreReady");
        check(!currentCache.exists(),
              "read-only startup cache lookup does not create a cache directory before CoreReady");
        check(!WaveformModel::hasPersistentStartupPreviewCache(startupFile),
              "startup prepare worker cannot write preview cache before actual CoreReady");
        window.setVisible(false);
    }
    pumpFor(20); // Drain cancelled SafePointer-bound startup callbacks.
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, true, true);
        host.setContentNonOwned(main.get(), false);
        DeferredCacheWarmTestAccess::open(*main, loadedFile);
        const auto loadedDeadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
        while ((!document.hasAudio() || DeferredCacheWarmTestAccess::loading(*main))
               && juce::Time::getMillisecondCounterHiRes() < loadedDeadline)
            pumpFor(5);
        check(document.hasAudio() && document.getState()->info.file == loadedFile
                  && !DeferredCacheWarmTestAccess::loading(*main),
              "ordinary async file load completes with its real eight-second PCM source");
        pumpFor(1800);
        check(!main->isCoreReadyForWorkspaceResources(),
              "hidden waveform fixture still lacks both current CoreReady milestones");
        check(!WaveformModel::hasPersistentStartupPreviewCache(loadedFile),
              "ordinary async preparation cannot write preview cache before CoreReady");
        check(!WaveformModel::hasPersistentPeakCache(loadedFile),
              "elapsed peak-publication timer cannot serialize full peaks before CoreReady");
        const auto generation = DeferredCacheWarmTestAccess::generation(*main);
        DeferredCacheWarmTestAccess::playable(*main, generation);
        DeferredCacheWarmTestAccess::waveform(*main, generation);
        check(waitForCache(loadedFile),
              "admitted current document still produces a valid persistent preview cache");
        const auto fullCache = WaveformModel::loadPersistentPeakCache(document.getState()->info);
        check(fullCache != nullptr && fullCache->channelCount == 2
                  && fullCache->totalSamples == 48000 * 8
                  && fullCache->loadedSampleRange.getEnd() == 48000 * 8,
              "admitted full cache retains complete current PCM timeline and channel count");
        const auto largeFile = artifacts.getChildFile("file-backed-dsp-cache.wav");
        writeLargeWarmWave(largeFile);
        const auto sourceHash = juce::SHA256(largeFile).toHexString();
        DeferredCacheWarmTestAccess::open(*main, largeFile);
        const auto largeDeadline = juce::Time::getMillisecondCounterHiRes() + 8000.0;
        while ((document.getState()->info.file != largeFile || DeferredCacheWarmTestAccess::loading(*main))
               && juce::Time::getMillisecondCounterHiRes() < largeDeadline)
            pumpFor(5);
        if (document.getState()->info.file != largeFile || DeferredCacheWarmTestAccess::loading(*main))
            throw std::runtime_error("Large cache fixture did not finish actual source admission");
        const auto largeGeneration = DeferredCacheWarmTestAccess::generation(*main);
        DeferredCacheWarmTestAccess::playable(*main, largeGeneration);
        DeferredCacheWarmTestAccess::waveform(*main, largeGeneration);
        std::shared_ptr<WaveformModel::PeakCache> originalPeaks;
        const auto originalDeadline = juce::Time::getMillisecondCounterHiRes() + 12000.0;
        do
        {
            pumpFor(20);
            originalPeaks = WaveformModel::loadPersistentPeakCache(document.getState()->info);
        } while ((!originalPeaks || originalPeaks->isPreview)
                 && juce::Time::getMillisecondCounterHiRes() < originalDeadline);
        check(originalPeaks && !originalPeaks->isPreview && originalPeaks->channelCount == 2,
              "large source has a real full original-file cache before the DSP transaction");
        const auto originalState = document.getState();
        auto mutation = document.beginEqualLengthAudioMutation(originalState);
        if (!mutation) throw std::runtime_error("Cannot admit real file-backed DSP cache fixture");
        juce::AudioBuffer<float> block;
        for (int64_t offset = 0; offset < originalState->info.numSamples; offset += 65536)
        {
            const auto count = static_cast<int>(juce::jmin<int64_t>(65536, originalState->info.numSamples - offset));
            if (!mutation->readSourceBlock(offset, count, block)) throw std::runtime_error("Cannot read DSP cache fixture source");
            block.applyGain(0.5f);
            if (!mutation->writeRenderedBlock(offset, block, 0, count)) throw std::runtime_error("Cannot render DSP cache fixture");
        }
        check(document.commitEqualLengthAudioMutation(std::move(mutation)),
              "real over-256MiB DSP transaction commits its bounded private float WAV");
        const auto editedState = document.getState();
        check(editedState != originalState && !editedState->audioBuffer && editedState->streamingReader
                  && editedState->readerSourceFile != editedState->info.file,
              "DSP fixture actually uses private reader-backed PCM with historical original file metadata");
        check(!WaveformModel::loadPersistentPeakCache(editedState),
              "file-backed DSP state cannot reuse the original-file peak cache");
        std::shared_ptr<const WaveformModel::PeakCache> editedPeaks;
        const auto editedDeadline = juce::Time::getMillisecondCounterHiRes() + 12000.0;
        do
        {
            pumpFor(20);
            editedPeaks = waveform.getPeakCache();
        } while ((!editedPeaks || editedPeaks->isPreview || editedPeaks->audioRevision != editedState->audioRevision)
                 && juce::Time::getMillisecondCounterHiRes() < editedDeadline);
        check(editedPeaks && editedPeaks->audioRevision == editedState->audioRevision
                  && !editedPeaks->channelBins.empty() && !editedPeaks->channelBins[0].empty()
                  && std::abs(editedPeaks->channelBins[0][0].maxValue - 0.125f) < 0.00001f,
              "visible waveform peaks read the actual gain-adjusted private PCM rather than cached original audio");
        check(document.undo() && document.getState() == originalState,
              "Undo restores the exact source-backed state after the file-backed cache test");
        check(WaveformModel::loadPersistentPeakCache(document.getState()) != nullptr,
              "Undo restores legitimate reuse of the original file cache");
        check(juce::SHA256(largeFile).toHexString() == sourceHash,
              "cache and DSP processing leave the physical original WAV unchanged");
        const auto reopenCache = WaveformModel::loadPersistentPeakCache(originalState->info);
        check(reopenCache && std::abs(reopenCache->channelBins[0][0].maxValue - 0.25f) < 0.00001f,
              "rendered private PCM never overwrites the original-file cache");

        const auto loadHidden = [&](const juce::File& file)
        {
            writeWarmWave(file, 48000 * 8);
            DeferredCacheWarmTestAccess::open(*main, file);
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while ((document.getState()->info.file != file || DeferredCacheWarmTestAccess::loading(*main))
                   && juce::Time::getMillisecondCounterHiRes() < deadline) pumpFor(5);
            if (document.getState()->info.file != file || DeferredCacheWarmTestAccess::loading(*main))
                throw std::runtime_error("Cannot finish current-document cancellation fixture load");
        };
        const auto replacedCurrent = artifacts.getChildFile("current-replaced-before-admission.wav");
        const auto closingCurrent = artifacts.getChildFile("current-closing-before-admission.wav");
        const auto destroyedCurrent = artifacts.getChildFile("current-destroyed-before-admission.wav");
        loadHidden(replacedCurrent);
        const auto oldGeneration = DeferredCacheWarmTestAccess::generation(*main);
        const auto replacementGeneration = DeferredCacheWarmTestAccess::begin(*main);
        DeferredCacheWarmTestAccess::playable(*main, oldGeneration);
        DeferredCacheWarmTestAccess::waveform(*main, replacementGeneration);
        pumpFor(1600);
        check(!WaveformModel::hasPersistentPeakCache(replacedCurrent)
                  && !WaveformModel::hasPersistentStartupPreviewCache(replacedCurrent),
              "current-file writes cannot borrow playable from a cancelled generation");
        DeferredCacheWarmTestAccess::playable(*main, replacementGeneration);
        pumpFor(1300);
        check(!WaveformModel::hasPersistentPeakCache(replacedCurrent),
              "cancelled peak-build publication cannot recreate an old pending write in the next ready generation");

        loadHidden(closingCurrent);
        const auto closingGeneration = DeferredCacheWarmTestAccess::generation(*main);
        DeferredCacheWarmTestAccess::playable(*main, closingGeneration);
        DeferredCacheWarmTestAccess::waveform(*main, closingGeneration);
        DeferredCacheWarmTestAccess::closing(*main, true);
        pumpFor(1600);
        check(!WaveformModel::hasPersistentPeakCache(closingCurrent)
                  && !WaveformModel::hasPersistentStartupPreviewCache(closingCurrent),
              "pending native close prevents current-file persistence even after both milestones");
        DeferredCacheWarmTestAccess::closing(*main, false);
        loadHidden(destroyedCurrent);
        host.clearContentComponent();
        main.reset();
        pumpFor(1600);
        check(!WaveformModel::hasPersistentPeakCache(destroyedCurrent)
                  && !WaveformModel::hasPersistentStartupPreviewCache(destroyedCurrent),
              "normal MainComponent destruction cancels pending current-file persistence");
    }
    auto* report = new juce::DynamicObject();
    report->setProperty("schema", "singleeditor.deferred-cache-warm-tests.v1");
    report->setProperty("status", failures ? "failed" : "pass");
    report->setProperty("results", results);
    report->setProperty("cacheRoot", cache.getFullPathName());
    report->setProperty("currentDocumentCacheRoot", currentCache.getFullPathName());
    report->setProperty("scope", "Actual native MainComponent timer, cache worker and valid PCM cache; controlled lifecycle milestones, not physical timing or external startup-I/O acceptance.");
    if (!artifacts.getChildFile("receipt.json").replaceWithText(juce::JSON::toString(juce::var(report))))
        throw std::runtime_error("Cannot write native cache-warm receipt");
    return failures ? 1 : 0;
}
}
