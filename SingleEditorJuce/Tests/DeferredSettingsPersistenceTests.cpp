#include "App/MainWindow.h"
#include "UI/MainComponent.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <iostream>
#include <stdexcept>

namespace singleeditor
{
struct DeferredSettingsTestAccess
{
    static CoreLifecycleCoordinator::Generation begin(MainComponent& main, bool loading)
    {
        main.activeDocumentGeneration = main.coreLifecycle.beginGeneration();
        main.loadInProgress = loading;
        return main.activeDocumentGeneration;
    }
    static void arm(MainWindow& window, MainComponent& main)
    {
        window.lastPersistedWindowBounds.reset();
        window.windowStatePersistPending = true;
        window.windowStatePersistDueAtMs = 0;
        main.sessionSettingsPersistPending = true;
        main.sessionSettingsPersistDueAtMs = 0;
    }
    static void tick(MainWindow& window, MainComponent& main)
    {
        window.timerCallback();
        main.timerCallback();
    }
    static bool pending(const MainWindow& window, const MainComponent& main)
    { return window.windowStatePersistPending && main.sessionSettingsPersistPending; }
    static bool drained(const MainWindow& window, const MainComponent& main)
    { return !window.windowStatePersistPending && !main.sessionSettingsPersistPending; }
    static void waveform(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.coreLifecycle.markFirstWaveform(generation); }
    static void playable(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.coreLifecycle.markPlayable(generation); }
};

int runDeferredSettingsPersistenceTests(const juce::File& artifacts)
{
    if (artifacts.exists() || artifacts.createDirectory().failed())
        throw std::runtime_error("Use a new deferred-settings artifact directory");
    juce::Array<juce::var> results;
    int failures = 0;
    const auto check = [&](bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        results.add(juce::var(row));
        if (!ok) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    };
    juce::ApplicationProperties properties;
    juce::PropertiesFile::Options options;
    options.applicationName = "DeferredSettingsTest"; options.filenameSuffix = "settings";
    options.folderName = artifacts.getChildFile("settings").getFullPathName();
    options.millisecondsBeforeSaving = -1;
    properties.setStorageParameters(options);
    auto* settings = properties.getUserSettings();
    const auto file = settings->getFile();
    if (!file.isAChildOf(artifacts)) throw std::runtime_error("Settings fixture escaped its artifact directory");
    settings->setValue("autoPlayEnabled", false);
    RendererSelection::select(RendererKind::modern);
    WindowChromeLookAndFeel chrome;
    juce::LookAndFeel::setDefaultLookAndFeel(&chrome);
    struct ResetDefaultLookAndFeel { ~ResetDefaultLookAndFeel() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); } } resetLook;
    SingleTrackDocument document;
    BnkDocument bank;
    PlaylistModel playlist;
    AudioEngine engine(document);
    if (!engine.reserveExplicitStartupOutput()) throw std::runtime_error("Reserve fixture output without opening a device");
    WaveformModel waveform(document);
    auto window = std::make_unique<MainWindow>("Deferred Settings Test", properties, document, bank, playlist, engine, waveform);
    auto* main = window->getMainComponent();
    if (!main) throw std::runtime_error("Actual native editor was not installed");
    if (!settings->saveIfNeeded()) throw std::runtime_error("Create private settings baseline");

    int witness = 0;
    const auto preparePendingWrite = [&]
    {
        // Automatic PropertiesFile writes are off. Only the actual production
        // persistence paths may move this dirty in-memory value to the file.
        settings->setValue("testWriteWitness", ++witness);
        DeferredSettingsTestAccess::arm(*window, *main);
    };
    const auto unchangedAfterTick = [&]
    {
        const auto before = file.loadFileAsString();
        DeferredSettingsTestAccess::tick(*window, *main);
        return file.loadFileAsString() == before && DeferredSettingsTestAccess::pending(*window, *main);
    };
    const auto writtenAfterTick = [&]
    {
        const auto before = file.loadFileAsString();
        DeferredSettingsTestAccess::tick(*window, *main);
        return file.loadFileAsString() != before && DeferredSettingsTestAccess::drained(*window, *main);
    };

    const auto first = DeferredSettingsTestAccess::begin(*main, true);
    preparePendingWrite();
    check(unchangedAfterTick(), "expired window and session timers retain pending writes during opening");
    DeferredSettingsTestAccess::waveform(*main, first);
    preparePendingWrite();
    check(unchangedAfterTick(), "first-waveform milestone alone cannot release deferred settings writes");
    DeferredSettingsTestAccess::playable(*main, first);
    preparePendingWrite();
    check(writtenAfterTick(), "current waveform and playable milestones release both actual settings writers");

    const auto second = DeferredSettingsTestAccess::begin(*main, true);
    DeferredSettingsTestAccess::playable(*main, second);
    DeferredSettingsTestAccess::waveform(*main, first);
    preparePendingWrite();
    check(unchangedAfterTick(), "playable alone plus a stale-generation waveform cannot release writes");
    DeferredSettingsTestAccess::waveform(*main, second);
    preparePendingWrite();
    check(writtenAfterTick(), "the new generation releases the retained writes after its own waveform");

    auto state = std::make_shared<SingleTrackDocument::State>();
    state->info.sourceAdmitted = true;
    state->info.sampleRate = 48000; state->info.numChannels = 2;
    state->info.numSamples = 128; state->info.durationSeconds = 128.0 / 48000;
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(2, 128);
    pcm->clear();
    state->audioBuffer = std::move(pcm);
    state->playbackAvailability = SingleTrackDocument::PlaybackAvailability::randomAccess();
    if (!document.adoptLoadedState(state)) throw std::runtime_error("Adopt actual PCM for the not-yet-presented document case");
    const auto original = document.getState();
    DeferredSettingsTestAccess::begin(*main, false);
    preparePendingWrite();
    check(unchangedAfterTick(), "finished decoding with audio still waits for current Core presentation");
    check(document.getState() == original, "settings gating does not replace the document or its history");

    document.clear();
    preparePendingWrite();
    check(writtenAfterTick(), "idle empty or failed-open-without-audio state can persist settings");
    check(!main->isCoreReadyForWorkspaceResources(), "empty-state persistence does not fabricate CoreReady");

    DeferredSettingsTestAccess::begin(*main, true);
    preparePendingWrite();
    const auto beforeClose = file.loadFileAsString();
    window.reset(); // Both production destructors deliberately flush on shutdown.
    check(file.loadFileAsString() != beforeClose, "explicit shutdown retains pending window and session preferences before CoreReady");
    check(!engine.getRuntimeMetricsForDiagnostics().has_value(), "fixture does not initialize or change an audio device");
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    properties.closeFiles();

    auto* report = new juce::DynamicObject();
    report->setProperty("schema", "singleeditor.deferred-settings-persistence-tests");
    report->setProperty("status", failures ? "failed" : "pass");
    report->setProperty("results", results);
    report->setProperty("scope", "Actual native editor timers and isolated settings writes with controlled lifecycle milestones; no slow-disk or hardware acceptance");
    if (!artifacts.getChildFile("receipt.json").replaceWithText(juce::JSON::toString(juce::var(report), true)))
        throw std::runtime_error("Write deferred settings test receipt");
    std::cout << "Deferred settings checks: " << results.size() << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}
}
