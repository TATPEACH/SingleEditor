#include "UI/MainComponent.h"
#include "UI/LookAndFeel/WindowChromeLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"
#include "Bnk/BnkStateSelection.h"
#include <iostream>
#include <stdexcept>

namespace singleeditor
{
struct DeferredWorkspaceControlsTestAccess
{
    static CoreLifecycleCoordinator::Generation begin(MainComponent& main)
    {
        main.activeDocumentGeneration = main.coreLifecycle.beginGeneration();
        main.loadInProgress = true;
        return main.activeDocumentGeneration;
    }
    static void waveform(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.coreLifecycle.markFirstWaveform(generation); }
    static void playable(MainComponent& main, CoreLifecycleCoordinator::Generation generation)
    { main.coreLifecycle.markPlayable(generation); }
    static void promote(MainComponent& main)
    {
        main.promoteDeferredChromeIfNeeded();
        main.ensureDeferredOutputMeter();
    }
    static CoreLifecycleCoordinator::Generation next(MainComponent& main)
    {
        main.beginDocumentGeneration({});
        return main.activeDocumentGeneration;
    }
    static void exerciseBank(MainComponent& main, const juce::File& artifacts,
                             const std::function<void(bool, const char*)>& check)
    {
        main.ensureBnkUiComponents();
        main.refreshBnkControlSurface();
        check(!main.bnkRuntime && !main.bnkControls && !main.bnkEventListView && !main.bnkAudioListView,
              "ordinary refresh and direct ensure cannot instantiate the bank module");
        // Controlled admitted-state fixture, not real-media/parser/decoder acceptance.
        auto state = std::make_shared<BnkDocument::State>();
        state->info.file = artifacts.getChildFile("controls.bnk");
        juce::MemoryOutputStream bytes;
        bytes.write("BKHD", 4); bytes.writeInt(8); bytes.writeInt(128); bytes.writeInt(7);
        if (!state->info.file.replaceWithData(bytes.getData(), bytes.getDataSize()))
            throw std::runtime_error("Cannot write owned bank control fixture");
        state->info.version = 128; state->info.bankId = 7; state->info.eventCount = 2;
        state->sourceFileSize = state->info.file.getSize();
        state->sourceModifiedMs = state->info.file.getLastModificationTime().toMilliseconds();
        state->sourceFileIdentifier = state->info.file.getFileIdentifier();
        state->switchValues = { { 1, "dry", "Dry", "Weather", true }, { 2, "wet", "Wet", "Weather", false } };
        state->stateValues = { { 1, "none", "None", "mood", 1, "Mood", true },
                               { 2, "quiet", "Quiet", "mood", 1, "Mood", false } };
        BnkDocument::EventDescriptor event;
        event.id = 10; event.displayName = "Controlled event";
        event.stateTargetScope = BnkDocument::EventDescriptor::StateTargetScope::singleObject;
        event.targetObjectId = "controlled-target";
        BnkDocument::EventDescriptor::StateModifier modifier;
        modifier.stateGroupToken = "mood"; modifier.stateValueToken = "quiet";
        modifier.stateGroupName = "Mood"; modifier.stateName = "Quiet";
        event.stateModifiers.push_back(modifier);
        for (int index = 0; index < 2; ++index)
        {
            BnkDocument::EventDescriptor::MediaReference media;
            media.mediaId = 100 + index; media.displayName = index ? "Wet media" : "Dry media";
            media.switchValueToken = index ? "wet" : "dry"; media.isDefaultSwitch = index == 0;
            event.media.push_back(media);
        }
        state->events.push_back(event);
        event.id = 20; event.displayName = "No context event";
        event.media.clear(); event.stateModifiers.clear(); state->events.push_back(event);
        check(main.bnkDocument.adoptParsedState(state), "controlled bank state admitted through the real document");
        main.loadInProgress = false;
        main.refreshBnkControlSurface(); main.updateCenterViewVisibility(); main.resized();
        if (!main.bnkControls || !main.bnkEventListView || !main.bnkAudioListView)
            throw std::runtime_error("Bank trigger did not create complete native controls");
        auto& controls = *main.bnkControls;
        juce::Component::SafePointer<juce::Component> surface(&controls.surface);
        check(controls.surface.isVisible() && main.bnkEventListView->isVisible()
                  && !main.bnkAudioListView->isVisible() && !main.waveformView.isVisible(),
              "bank trigger creates the complete visible event surface");
        check(controls.switchBox.getNumItems() == 2 && controls.stateBox.getNumItems() == 2
                  && main.buildBnkPlaybackContext().switchValueToken == "dry"
                  && main.buildBnkPlaybackContext().stateValueToken == bnkstate::token(state->stateValues.front()),
              "lazy controls preserve event switch and exact group State defaults");
        controls.switchBox.setSelectedItemIndex(1, juce::sendNotificationSync);
        check(main.buildBnkPlaybackContext().switchValueToken == "wet" && main.bnkRuntime->preferredSwitch(10) == "wet",
              "native switch callback retains selection for the exact event");
        main.cancelBnkPreview(); main.refreshUiState();
        controls.stateBox.setSelectedItemIndex(1, juce::sendNotificationSync);
        check(main.buildBnkPlaybackContext().stateValueToken == bnkstate::token(state->stateValues[1])
                  && main.bnkRuntime->preferredState(10) == main.buildBnkPlaybackContext().stateValueToken,
              "native State callback retains its qualified group identity");
        main.cancelBnkPreview(); main.refreshUiState();
        controls.audioTab.onClick();
        check(main.bnkAudioListView->isVisible() && !main.bnkEventListView->isVisible()
                  && controls.audioTab.getToggleState(), "native Audio tab callback switches the real list");
        controls.audioFilter.setToggleState(false, juce::dontSendNotification);
        controls.audioFilter.onClick();
        check(!main.bnkAudioLinkedOnly && !main.bnkAudioListView->isLinkedOnly(),
              "native filter callback updates the actual media list");
        controls.eventsTab.onClick();
        main.bnkDocument.selectEventById(20); main.refreshBnkControlSurface();
        check(!controls.switchBox.isVisible() && !controls.stateBox.isVisible(),
              "an event without context hides stale switch and State choices");
        main.bnkDocument.selectEventById(10); main.refreshBnkControlSurface();
        check(main.buildBnkPlaybackContext().switchValueToken == "wet" && controls.stateBox.getSelectedItemIndex() == 1,
              "returning to an event restores remembered switch and State choices");
        const auto snapshot = [&](const char* name)
        {
            juce::PNGImageFormat png;
            const auto picture = main.createComponentSnapshot(main.getLocalBounds());
            auto output = artifacts.getChildFile(name).createOutputStream();
            if (!output || !png.writeImageToStream(picture, *output))
                throw std::runtime_error("Cannot retain native component render");
        };
        snapshot("bank-general-native.png");
        main.modernSimpleView = true; main.applyChromePresentation();
        check(!controls.surface.isVisible() && !main.bnkEventListView->isVisible()
                  && !main.bnkAudioListView->isVisible() && main.waveformView.isVisible(),
              "Simple mode hides the existing bank workspace without replacing controls");
        snapshot("bank-simple-native.png");
        main.modernSimpleView = false; main.applyChromePresentation();
        main.setSize(760, 620); main.lookAndFeelChanged(); main.ensureBnkUiComponents();
        check(surface.getComponent() == &main.bnkControls->surface && controls.surface.isVisible()
                  && controls.surface.getBounds().getBottom() <= main.bnkEventListView->getY()
                  && !controls.switchBox.getBounds().isEmpty() && !controls.stateBox.getBounds().isEmpty(),
              "General mode resize and theme preserve one bank surface and accessible control bounds");
        snapshot("bank-narrow-native.png");
        main.releaseBnkRuntime(); main.updateCenterViewVisibility(); main.refreshUiState();
        check(!surface && !main.bnkRuntime && !main.bnkControls && !main.bnkEventListView && !main.bnkAudioListView
                  && main.waveformView.isVisible() && !main.bnkDocument.hasBank(),
              "return to ordinary audio destroys bank controls and both list listeners");
        main.bnkDocument.adoptParsedState(state); main.refreshBnkControlSurface(); main.updateCenterViewVisibility();
        check(main.bnkControls && main.bnkControls->surface.isVisible() && main.bnkEventListView,
              "a later bank activation recreates working controls after release");
        main.releaseBnkRuntime(); main.refreshUiState();
    }
};

namespace
{
struct Controls
{
    juce::Component* playlist = nullptr;
    juce::Component* metadata = nullptr;
    juce::Component* meter = nullptr;
    int count = 0;
    int bankControls = 0;
};
Controls observe(juce::Component& root)
{
    Controls result;
    const auto visit = [&](auto&& self, juce::Component& node) -> void
    {
        if (dynamic_cast<PlaylistSidebarComponent*>(&node)) { result.playlist = &node; ++result.count; }
        if (dynamic_cast<MetadataSidebarComponent*>(&node)) { result.metadata = &node; ++result.count; }
        if (dynamic_cast<MeterBridgeComponent*>(&node)) { result.meter = &node; ++result.count; }
        if (auto* button = dynamic_cast<juce::TextButton*>(&node))
            if (button->getButtonText() == "Events" || button->getButtonText() == "Audio"
                || button->getButtonText() == "Linked Only" || button->getButtonText() == "Attach Project")
                ++result.bankControls;
        for (auto* child : node.getChildren()) self(self, *child);
    };
    visit(visit, root);
    return result;
}
}

int runDeferredWorkspaceControlsTests(const juce::File& artifacts)
{
    if (artifacts.exists() || artifacts.createDirectory().failed())
        throw std::runtime_error("Use a fresh deferred-workspace-controls artifact directory");
    int failures = 0;
    juce::Array<juce::var> results;
    const auto check = [&](bool ok, const char* name)
    {
        auto* item = new juce::DynamicObject();
        item->setProperty("name", name); item->setProperty("status", ok ? "pass" : "failed");
        results.add(juce::var(item));
        if (!ok) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    };
    juce::ApplicationProperties properties;
    juce::PropertiesFile::Options options;
    options.applicationName = "SingleEditorDeferredWorkspaceTest";
    options.filenameSuffix = "settings";
    options.folderName = artifacts.getFullPathName();
    options.millisecondsBeforeSaving = -1;
    properties.setStorageParameters(options);
    properties.getUserSettings()->setValue("restoreLastSessionOnStartup", false);
    RendererSelection::resetForTests(RendererKind::modern);
    WindowChromeLookAndFeel look;
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    struct ResetLook { ~ResetLook() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); } } resetLook;
    SingleTrackDocument document;
    BnkDocument bank;
    PlaylistModel playlist;
    AudioEngine engine(document);
    if (!engine.reserveExplicitStartupOutput()) throw std::runtime_error("Reserve fixture output without opening a device");
    WaveformModel waveform(document);
    juce::DocumentWindow host("Deferred workspace controls", juce::Colours::black,
                              juce::DocumentWindow::allButtons, false);
    host.setBounds(60, 60, 1180, 760);
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, true, true);
        host.setContentNonOwned(main.get(), false);
        main->setBounds(0, 0, 1180, 760);
        const auto first = DeferredWorkspaceControlsTestAccess::begin(*main);
        check(observe(*main).count == 0, "startup creates no playlist metadata or legacy meter native controls before CoreReady");
        check(observe(*main).bankControls == 0, "ordinary startup creates no bank controls before CoreReady");
        main->resized(); main->lookAndFeelChanged();
        check(observe(*main).count == 0, "pre-Core native layout and appearance callbacks do not create optional controls");
        check(observe(*main).bankControls == 0, "pre-Core layout and appearance do not instantiate hidden bank controls");
        check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0, "pre-Core controls acquire no meter producer");
        DeferredWorkspaceControlsTestAccess::waveform(*main, first);
        check(!main->isCoreReadyForWorkspaceResources() && observe(*main).count == 0,
              "a truthful waveform without playable keeps controls absent");
        DeferredWorkspaceControlsTestAccess::playable(*main, first);
        check(main->isCoreReadyForWorkspaceResources(), "both current milestones retain the real CoreReady conjunction");
        DeferredWorkspaceControlsTestAccess::promote(*main);
        const auto created = observe(*main);
        check(created.count == 3 && created.playlist && created.metadata && created.meter,
              "post-Core promotion creates the existing three native controls");
        check(created.bankControls == 0, "ordinary CoreReady does not activate the bank module");
        DeferredWorkspaceControlsTestAccess::promote(*main);
        main->resized(); main->lookAndFeelChanged();
        const auto repeated = observe(*main);
        check(repeated.count == 3 && repeated.playlist == created.playlist && repeated.metadata == created.metadata
                  && repeated.meter == created.meter, "repeated promotion and layout retain each control exactly once");
        const auto second = DeferredWorkspaceControlsTestAccess::next(*main);
        DeferredWorkspaceControlsTestAccess::waveform(*main, second);
        DeferredWorkspaceControlsTestAccess::playable(*main, first);
        DeferredWorkspaceControlsTestAccess::promote(*main);
        check(!main->isCoreReadyForWorkspaceResources()
                  && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "a previous generation playable cannot reactivate the next document controls");
        DeferredWorkspaceControlsTestAccess::playable(*main, second);
        DeferredWorkspaceControlsTestAccess::promote(*main);
        const auto switched = observe(*main);
        check(switched.count == 3 && switched.playlist == created.playlist
                  && switched.metadata == created.metadata && switched.meter == created.meter,
              "a new CoreReady host reparents the same borrowed controls across documents");
        host.clearContentComponent();
    }
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, false, true);
        host.setContentNonOwned(main.get(), false);
        main->resized(); main->lookAndFeelChanged();
        check(observe(*main).count == 3 && engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0,
              "empty startup retains its existing controls without activating a producer");
        check(observe(*main).bankControls == 0, "empty startup leaves bank controls uninstantiated");
        DeferredWorkspaceControlsTestAccess::exerciseBank(*main, artifacts, check);
        host.clearContentComponent();
    }
    check(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0, "normal native teardown releases all control leases");
    {
        auto main = std::make_unique<MainComponent>(host, properties, document, bank, playlist, engine, waveform, true, true);
        host.setContentNonOwned(main.get(), false);
        check(observe(*main).count == 0, "a fresh startup remains uninstantiated when closed before CoreReady");
        host.clearContentComponent();
    }
    auto* report = new juce::DynamicObject();
    report->setProperty("schema", "singleeditor.deferred-workspace-controls-tests");
    report->setProperty("status", failures == 0 ? "passed" : "failed");
    report->setProperty("checks", results); report->setProperty("failures", failures);
    report->setProperty("scope", "Actual native construction/layout/promotion/teardown and BNK controls/callbacks with controlled lifecycle milestones and admitted-state fixture. PNGs are actual JUCE component renders, not physical screenshots. No real compressed-media, decoder, device playback, physical input or CoreReady timing result.");
    if (!artifacts.getChildFile("receipt.json").replaceWithText(juce::JSON::toString(juce::var(report), true)))
        throw std::runtime_error("Cannot write deferred workspace receipt");
    std::cout << "Deferred workspace controls checks=" << results.size() << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
}
