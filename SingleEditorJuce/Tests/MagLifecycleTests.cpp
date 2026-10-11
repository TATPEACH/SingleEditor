#include "OptionalLifecycleTestSupport.h"
#include "Audio/AudioEngine.h"
#include "Audio/SpatialEnergyService.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "AudioSuite/TruePeakLimiterProcessor.h"
#include "UI/MainComponent.h"
#include "UI/Components/SpatialEnergyPanelComponent.h"
#include "UI/Components/TruePeakLimiterMagComponent.h"
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

#if !SINGLE_EDITOR_ENABLE_TEST_OBSERVERS || !SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
 #error MagLifecycleTests belongs only to the native observer test target
#endif

namespace singleeditor
{
// Reads product state and invokes existing operations. No factory replacement,
// synthetic readiness, document-generation mutation, or device-route override.
struct MagLifecycleTestAccess
{
    static std::uint64_t generation(const MainComponent& main) { return main.activeDocumentGeneration.value; }
    static bool ready(const MainComponent& main) { return main.isCoreReadyForWorkspaceResources(); }
    static WorkspaceModuleHostComponent* host(MainComponent& main) { return main.workspaceModuleHost.get(); }
    static SingleTrackDocument& document(MainComponent& main) { return main.document; }
    static PlaylistModel& playlist(MainComponent& main) { return main.playlistModel; }
    static bool command(MainComponent& main, std::string_view id)
    { return main.dispatchGuiCommand(id, GuiCommandSource::mag, "workspace.modules").succeeded(); }
    static void apply(MainComponent& main) { main.applyParametricEq(); }
    static void cancelApply(MainComponent& main) { main.cancelParametricEqApply(false); }
    static bool applying(const MainComponent& main) { return main.parametricEqApplyInFlight; }
    static bool limiterEnabled(const MainComponent& main) { return main.truePeakLimiterParameters.enabled; }
    static bool limiterPrepared(const MainComponent& main) { return main.truePeakLimiterProcessor != nullptr; }
    static bool limiterPreviewing(const MainComponent& main) { return main.truePeakLimiterPreview.isValid(); }
};

namespace
{
using Action = OptionalLifecycleAction;
using Kind = OptionalLifecycleResourceKind;
using Owner = OptionalLifecycleOwner;
using Access = MagLifecycleTestAccess;

struct ProcessChannel
{
    std::array<OptionalLifecycleEventSlot, 4096> slots;
    OptionalLifecycleObserver observer {std::span{slots}};
};
ProcessChannel& processChannel()
{
    // An abnormal shutdown may leave a SafePointer completion queued until the
    // MessageManager is destroyed, after Application::shutdown/session release.
    // This one bounded test-only allocation is reclaimed by process exit and
    // never owns a product object. It keeps those non-owning notes safe too.
    static auto* const channel = new ProcessChannel;
    return *channel;
}

using lifecycle_test::actionName;
using lifecycle_test::kindName;
using lifecycle_test::terminal;
juce::Button* discardButton(juce::Component& component)
{
    if (auto* button = dynamic_cast<juce::Button*>(&component);
        button && button->getButtonText() == "Discard" && button->isShowing() && button->isEnabled()) return button;
    for (auto* child : component.getChildren()) if (auto* found = discardButton(*child)) return found;
    return nullptr;
}
}

class MagLifecycleTestSession final
{
public:
    explicit MagLifecycleTestSession(juce::File output, bool remainingTransitions)
        : artifacts(std::move(output)), observer(processChannel().observer), armed({&observer, 1, 0}),
          transitionsOnly(remainingTransitions)
    {
        expect(observer.attemptedEventCount() == 0, "one fresh lifecycle channel per native process");
        secondFile = artifacts.getChildFile("mag-lifecycle-B.wav");
        expect(!secondFile.exists(), "second actual WAV belongs to fresh scenario artifacts");
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> outputStream(secondFile.createOutputStream());
        auto writer = format.createWriterFor(outputStream, juce::AudioFormatWriterOptions()
            .withSampleRate(48000).withNumChannels(4).withBitsPerSample(24));
        expect(writer != nullptr, "create actual four-channel WAV B");
        juce::AudioBuffer<float> pcm(4, 48000);
        for (int channel = 0; channel < 4; ++channel)
            for (int sample = 0; sample < pcm.getNumSamples(); ++sample)
                pcm.setSample(channel, sample, 0.15f * std::sin(static_cast<float>(sample + channel * 23) * 0.04f));
        expect(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write actual WAV B audio");
    }

    void expect(bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject;
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        checks.add(juce::var(row));
        if (!ok) throw std::runtime_error(name);
    }
    std::vector<OptionalLifecycleEvent> events() const
    {
        std::vector<OptionalLifecycleEvent> result;
        for (const auto& slot : observer.events())
            if (const auto committed = slot.committedSequence.load(std::memory_order_acquire); committed != 0)
            {
                if (committed != slot.event.sequence) throw std::runtime_error("uncommitted lifecycle event");
                result.push_back(slot.event);
            }
        return result;
    }
    std::vector<OptionalLifecycleEvent> matching(Owner owner, Kind kind, Action action, std::uint64_t after = 0) const
    {
        std::vector<OptionalLifecycleEvent> result;
        for (const auto& event : events())
            if (event.owner == owner && event.kind == kind && event.action == action && event.sequence > after)
                result.push_back(event);
        return result;
    }
    bool has(std::uint64_t id, Action action) const
    {
        for (const auto& event : events()) if (event.resourceId == id && event.action == action) return true;
        return false;
    }
    bool ended(std::uint64_t id) const
    {
        for (const auto& event : events()) if (event.resourceId == id && terminal(event.action)) return true;
        return false;
    }
    std::uint64_t newest(Owner owner, Kind kind) const
    {
        const auto rows = matching(owner, kind, Action::created);
        if (rows.empty()) throw std::runtime_error("actual created resource event missing");
        return rows.back().resourceId;
    }
    void command(MainComponent& main, std::string_view id)
    { expect(Access::command(main, id), "existing product command accepted"); }
    void next(int value, const char* name)
    {
        phase = value;
        auto* row = new juce::DynamicObject;
        row->setProperty("phase", value); row->setProperty("name", name);
        row->setProperty("event_sequence", static_cast<juce::int64>(observer.attemptedEventCount()));
        stages.add(juce::var(row));
        std::cout << "mag lifecycle " << value << ": " << name << std::endl;
    }
    bool advance(MainComponent& main, AudioEngine& engine)
    {
        if (observer.failureFlags() != 0) throw std::runtime_error("lifecycle producer reported a harness failure");
        if (transitionsOnly) return advanceTransitions(main, engine);
        // Apply may genuinely complete before cancellation. Discard only the
        // private fixture's real unsaved prompt; never modify document history.
        if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal != nullptr && phase >= 13)
        {
            if (modal->getName() == "Unsaved Changes")
                if (auto* button = discardButton(*modal)) { button->triggerClick(); ++discardedPrompts; }
            return false;
        }
        auto* host = Access::host(main);
        switch (phase)
        {
            case 0:
                if (!Access::ready(main) || host == nullptr
                    || host->getModuleComponent(WorkspaceModuleId::outputMeter) == nullptr) return false;
                firstFile = Access::document(main).getFileInfo().file;
                firstGeneration = Access::generation(main);
                expect(engine.hasLoadedFile() && firstFile.existsAsFile(), "actual WAV A is loaded");
                checkCore(firstGeneration);
                firstHost = newest(Owner::workspace, Kind::workspaceHost);
                meterView = host->getModuleComponent(WorkspaceModuleId::outputMeter);
                firstMeter = newest(Owner::outputMeter, Kind::ownedView);
                command(main, "workspace.view.simple"); next(1, "Simple hides actual meter View"); return false;
            case 1:
                if (meterView != nullptr && meterView->isShowing()) return false;
                expect(host != nullptr && host->isSimpleView() && meterView != nullptr && !ended(firstMeter),
                    "Simple hides but retains the actual owned View and user layout");
                expect(has(firstMeter, Action::quiesced), "hidden owned View has a real quiescent transition");
                command(main, "workspace.scene.general"); next(2, "General restores existing View"); return false;
            case 2:
                if (meterView == nullptr || !meterView->isShowing()) return false;
                expect(host->getModuleComponent(WorkspaceModuleId::outputMeter) == meterView.getComponent()
                    && !ended(firstMeter), "General restores the same View identity");
                command(main, "workspace.module.output-meter.float"); next(3, "float actual meter"); return false;
            case 3:
                if (!host->isModuleFloating(WorkspaceModuleId::outputMeter)) return false;
                expect(meterView != nullptr && !ended(firstMeter), "float retains real View lifetime");
                command(main, "workspace.module.output-meter.redock"); next(4, "redock actual meter"); return false;
            case 4:
                if (host->isModuleFloating(WorkspaceModuleId::outputMeter)) return false;
                expect(host->getModuleComponent(WorkspaceModuleId::outputMeter) == meterView.getComponent(),
                    "redock preserves owned View identity");
                command(main, "workspace.module.output-meter.remove"); next(5, "Remove then Add"); return false;
            case 5:
                expect(meterView == nullptr && ended(firstMeter), "Remove destroys the actual owned View");
                command(main, "workspace.module.output-meter.toggle"); next(6, "fresh View after Add"); return false;
            case 6:
                if (host->getModuleComponent(WorkspaceModuleId::outputMeter) == nullptr) return false;
                expect(newest(Owner::outputMeter, Kind::ownedView) != firstMeter,
                    "Add after Remove creates a new resource identity");
                audioSuiteSharedReaders(main);
                startSpatialService(firstGeneration);
                command(main, "workspace.module.spatial-energy.toggle");
                command(main, "workspace.module.spatial-energy.float");
                next(7, "actual Spatial view and independent shared service"); return false;
            case 7:
            {
                auto* panel = dynamic_cast<SpatialEnergyPanelComponent*>(host->getModuleComponent(WorkspaceModuleId::spatialEnergy));
                if (panel == nullptr || !host->isModuleFloating(WorkspaceModuleId::spatialEnergy)) return false;
                spatialView = panel;
                host->setModuleVisualSize(WorkspaceModuleId::spatialEnergy, RendererMagSize::large);
                auto* floating = panel->getTopLevelComponent();
                expect(floating != nullptr && floating != main.getTopLevelComponent(), "Spatial resize affects only its real floating window");
                floating->setSize(760, 640);
                next(8, "standard Spatial view starts visual worker and timer"); return false;
            }
            case 8:
            {
                auto* panel = dynamic_cast<SpatialEnergyPanelComponent*>(spatialView.getComponent());
                if (panel == nullptr || !panel->hasVisualWorker() || !panel->hasViewLease() || !panel->isViewTimerRunning()
                    || spatialService->diagnostics().liveMeters != 1) return false;
                expect(panel->currentView() != SpatialEnergyPanelComponent::View::compact,
                    "real noncompact Spatial panel has a visual worker");
                spatialVisual = newest(Owner::spatialEnergy, Kind::spatialVisualWorker);
                spatialTimer = newest(Owner::spatialEnergy, Kind::viewTimer);
                finishSpatialService();
                command(main, "workspace.view.simple"); next(9, "hide Spatial releases active work"); return false;
            }
            case 9:
            {
                auto* panel = dynamic_cast<SpatialEnergyPanelComponent*>(spatialView.getComponent());
                if (panel == nullptr || panel->isShowing()) return false;
                expect(!panel->hasVisualWorker() && !panel->hasViewLease() && !panel->isViewTimerRunning()
                    && ended(spatialVisual) && ended(spatialTimer),
                    "hidden Spatial view terminates its real visual worker and timer, retaining View");
                command(main, "workspace.scene.general"); next(10, "restore Spatial after Hide"); return false;
            }
            case 10:
            {
                auto* panel = dynamic_cast<SpatialEnergyPanelComponent*>(spatialView.getComponent());
                if (panel == nullptr || !panel->hasVisualWorker() || !panel->isViewTimerRunning()) return false;
                expect(newest(Owner::spatialEnergy, Kind::spatialVisualWorker) != spatialVisual
                    && newest(Owner::spatialEnergy, Kind::viewTimer) != spatialTimer,
                    "restored View starts new worker and timer identities");
                spatialVisual = newest(Owner::spatialEnergy, Kind::spatialVisualWorker);
                spatialTimer = newest(Owner::spatialEnergy, Kind::viewTimer);
                command(main, "workspace.module.spatial-energy.redock");
                command(main, "workspace.module.spatial-energy.remove"); next(11, "remove Spatial View"); return false;
            }
            case 11:
                expect(spatialView == nullptr && ended(spatialVisual) && ended(spatialTimer),
                    "Remove destroys Spatial View and its real restarted worker and timer");
                if (host->getModuleComponent(WorkspaceModuleId::parametricEq) == nullptr)
                    command(main, "workspace.module.parametric-eq.toggle");
                next(12, "one real EQ Apply and immediate cancellation request"); return false;
            case 12:
                if (host->getModuleComponent(WorkspaceModuleId::parametricEq) == nullptr) return false;
                applyStart = observer.attemptedEventCount();
                Access::apply(main);
                expect(Access::applying(main), "real Main Apply starts an actual task");
                Access::cancelApply(main);
                expect(matching(Owner::parametricEq, Kind::applyTask, Action::factoryRequested, applyStart).size() == 1,
                    "one observed actual ApplyTask starts, not a replacement executor");
                main.openInitialFile(firstFile);
                next(13, "reopen exact same WAV A through real open path"); return false;
            case 13:
                if (!Access::ready(main) || Access::generation(main) == firstGeneration || Access::applying(main)) return false;
                sameFileGeneration = Access::generation(main);
                expect(Access::document(main).getFileInfo().file == firstFile,
                    "same-file reopen preserves actual source and starts a new generation");
                checkCore(sameFileGeneration);
                expect(ended(firstHost), "old Host is destroyed after actual generation switch with old identity");
                main.openInitialFile(secondFile);
                next(14, "open actual distinct four-channel WAV B"); return false;
            case 14:
                if (!Access::ready(main) || Access::generation(main) == sameFileGeneration
                    || Access::document(main).getFileInfo().file != secondFile) return false;
                secondGeneration = Access::generation(main); checkCore(secondGeneration);
                expect(Access::document(main).getFileInfo().numChannels == 4 && Access::playlist(main).getNumEntries() == 2,
                    "different-file open adopted WAV B and preserved two actual playlist entries");
                ambisonicIndependentConsumers(secondGeneration);
                next(15, "await actual Apply callback terminal before document close"); return false;
            case 15:
                if (!applySettled()) return false;
                expect(!Access::applying(main) && Access::generation(main) == secondGeneration
                    && Access::document(main).getFileInfo().file == secondFile,
                    "old Apply completion cannot replace B or its actual generation");
                checkApply();
                command(main, "document.playlist.remove-current");
                removals = 1; next(16, "delete current entries using product command"); return false;
            case 16:
                if (Access::playlist(main).getNumEntries() > 0)
                {
                    if (!Access::ready(main)) return false;
                    expect(removals == 1 && Access::playlist(main).getNumEntries() == 1,
                        "current-entry removal activates only the retained A entry");
                    checkCore(Access::generation(main));
                    command(main, "document.playlist.remove-current"); ++removals; return false;
                }
                expect(removals == 2 && !Access::document(main).hasAudio() && !engine.hasLoadedFile(),
                    "second real removal closes the document and audio source");
                expect(applySettled(), "no queued observed Apply completion remains before actual quit");
                documentCloseSequence = observer.attemptedEventCount();
                next(17, "document closed; workspace resources may remain until actual app shutdown");
                return true;
            default: return true;
        }
    }

    void finish(bool completed)
    {
        // Also release test-owned real services on the failure path; the channel
        // remains alive independently of this session and queued callbacks.
        spatialFirst = {}; spatialSecond = {}; spatialService.reset();
        juce::String failure;
        try
        {
            expect(completed && phase == (transitionsOnly ? 13 : 17), "one complete native lifecycle workflow reached actual shutdown");
            validateTerminalStream();
            if (transitionsOnly) validateSupersededGenerations();
        }
        catch (const std::exception& error) { failure = error.what(); }
        auto* report = new juce::DynamicObject;
        report->setProperty("schema", "singleeditor.mag-lifecycle-native"); report->setProperty("version", 1);
        report->setProperty("status", failure.isEmpty() ? "pass" : "failed");
        report->setProperty("scenario", transitionsOnly ? "remaining-transitions" : "original-mag-lifecycle");
        report->setProperty("error", failure); report->setProperty("checks", checks); report->setProperty("stages", stages);
        report->setProperty("first_generation", static_cast<juce::int64>(firstGeneration));
        report->setProperty("same_file_generation", static_cast<juce::int64>(sameFileGeneration));
        report->setProperty("second_file_generation", static_cast<juce::int64>(secondGeneration));
        report->setProperty("document_close_sequence", static_cast<juce::int64>(documentCloseSequence));
        report->setProperty("discarded_private_unsaved_prompts", discardedPrompts);
        report->setProperty("observer_failures", static_cast<int>(observer.failureFlags()));
        report->setProperty("outstanding_resources", static_cast<juce::int64>(observer.outstandingResourceCount()));
        report->setProperty("attempted_events", static_cast<juce::int64>(observer.attemptedEventCount()));
        report->setProperty("backend_evidence", "Raw backend events including generation zero are retained and strictly validated; incidental backend observation does not extend this scenario to full D158 acceptance.");
        report->setProperty("scope", transitionsOnly
            ? "Actual Limiter Preview/Off bypass and re-enable, inactive tab/restore, rapid same/different-file supersession before dispatch, current-generation CoreReady, document/app close. Existing real controls and product open paths; no synthetic callbacks, cold-cache, external Release trace, physical playback or full D158 acceptance."
            : "Actual Main WAV generations, Simple/General, float/redock, Remove/Add, Apply/cancel, document/app close. Reference/Limiter shared readers, Ambisonic playback/field and Spatial shared State are explicitly service-level real product paths. No physical playback, hardware route, performance or full release acceptance.");
        report->setProperty("burst_generations", burstGenerations);
        report->setProperty("superseded_generations", supersededGenerations);
        juce::Array<juce::var> raw;
        for (const auto& event : events())
        {
            auto* row = new juce::DynamicObject;
            row->setProperty("sequence", static_cast<juce::int64>(event.sequence));
            row->setProperty("resource", static_cast<juce::int64>(event.resourceId));
            row->setProperty("session", static_cast<juce::int64>(event.session));
            row->setProperty("generation", static_cast<juce::int64>(event.generation));
            row->setProperty("related_resource", static_cast<juce::int64>(event.relatedResource));
            row->setProperty("owner", optionalLifecycleOwnerId(event.owner));
            row->setProperty("kind", kindName(event.kind)); row->setProperty("action", actionName(event.action));
            row->setProperty("operation_id", lifecycle_test::operationId(event));
            raw.add(juce::var(row));
        }
        report->setProperty("events", raw);
        if (!artifacts.getChildFile(transitionsOnly ? "mag-transitions.json" : "mag-lifecycle.json")
                .replaceWithText(juce::JSON::toString(juce::var(report), true)))
            throw std::runtime_error("write mag lifecycle receipt");
        if (failure.isNotEmpty()) throw std::runtime_error(failure.toStdString());
    }

private:
    juce::Button* findButton(juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id) return dynamic_cast<juce::Button*>(&root);
        for (auto* child : root.getChildren()) if (auto* found = findButton(*child, id)) return found;
        return nullptr;
    }
    void clickLimiter(const char* id)
    {
        expect(limiterView != nullptr, "actual Limiter View remains available for its user control");
        auto* button = findButton(*limiterView.getComponent(), id);
        expect(button != nullptr && button->isEnabled(), "actual Limiter control is enabled");
        button->triggerClick();
    }
    void rememberLimiterPreview()
    {
        limiterResources[0] = newest(Owner::truePeakLimiter, Kind::processor);
        limiterResources[1] = newest(Owner::truePeakLimiter, Kind::renderSession);
        limiterResources[2] = newest(Owner::truePeakLimiter, Kind::previewState);
        const auto publications = matching(Owner::truePeakLimiter, Kind::previewPublication, Action::published);
        expect(!publications.empty(), "actual Limiter preview is published to the engine");
        limiterResources[3] = publications.back().resourceId;
        for (const auto id : limiterResources) expect(id != 0 && !ended(id), "current preview resource has its own live identity");
    }
    bool limiterResourcesEnded() const
    {
        return std::all_of(limiterResources.begin(), limiterResources.end(),
            [&](auto id) { return id != 0 && ended(id); });
    }
    void referencePublicationLifetime(MainComponent& main)
    {
        const ScopedOptionalLifecycleContext context{{&observer, 1, firstGeneration}};
        auto processor = AudioSuiteReferenceProcessor::describe().create();
        const auto analysis = processor->analyze(Access::document(main).getState(),
            static_cast<int64_t>(firstGeneration), {}, 0.5f);
        expect(analysis.succeeded(), "real Reference service plan is admitted for publication lifetime checks");
        auto preview = processor->createPreview(analysis.plan);
        const auto stateId = preview.getState()->observedResourceId();
        std::uint64_t publication = 0;
        {
            // A real separate engine exercises destruction with a retained
            // publication; it never opens a device or replaces Main transport.
            AudioEngine detached(Access::document(main));
            detached.setAudioSuitePreview(preview.getState());
            const auto rows = matching(Owner::referenceGain, Kind::previewPublication, Action::published);
            expect(!rows.empty() && rows.back().relatedResource == stateId,
                "Reference publication names its real shared State, not a guessed processor");
            publication = rows.back().resourceId;
            detached.setAudioSuitePreview(nullptr);
            expect(ended(publication) && !ended(stateId), "null replacement withdraws only the engine lease while a real reader remains");
            preview = processor->createPreview(analysis.plan);
            detached.setAudioSuitePreview(preview.getState());
            publication = matching(Owner::referenceGain, Kind::previewPublication, Action::published).back().resourceId;
        }
        expect(ended(publication) && !ended(preview.getState()->observedResourceId()),
            "engine destruction withdraws its publication independently of a retained shared reader");
    }
    bool advanceTransitions(MainComponent& main, AudioEngine& engine)
    {
        auto* host = Access::host(main);
        switch (phase)
        {
            case 0:
                if (!Access::ready(main) || host == nullptr
                    || host->getModuleComponent(WorkspaceModuleId::outputMeter) == nullptr) return false;
                firstFile = Access::document(main).getFileInfo().file;
                firstGeneration = Access::generation(main); checkCore(firstGeneration);
                firstHost = newest(Owner::workspace, Kind::workspaceHost);
                referencePublicationLifetime(main);
                if (host->getModuleComponent(WorkspaceModuleId::truePeakLimiter) == nullptr)
                    command(main, "workspace.module.true-peak-limiter.toggle");
                next(1, "actual Limiter starts Off without a processor"); return false;
            case 1:
                limiterView = host->getModuleComponent(WorkspaceModuleId::truePeakLimiter);
                if (limiterView == nullptr) return false;
                limiterViewId = newest(Owner::truePeakLimiter, Kind::ownedView);
                expect(!Access::limiterEnabled(main) && !Access::limiterPrepared(main) && !Access::limiterPreviewing(main),
                    "product default Off has no processor or preview");
                clickLimiter("limiter.enabled"); next(2, "enable does not request Preview"); return false;
            case 2:
                if (!Access::limiterEnabled(main)) return false;
                expect(!Access::limiterPrepared(main) && !Access::limiterPreviewing(main),
                    "enabling a Limiter parameter alone leaves its processor unrequested");
                clickLimiter("limiter.preview"); next(3, "explicit Preview creates real resources"); return false;
            case 3:
                if (!Access::limiterPreviewing(main)) return false;
                expect(Access::limiterPrepared(main), "real Preview prepared its processor");
                rememberLimiterPreview(); firstLimiterProcessor = limiterResources[0];
                clickLimiter("limiter.enabled"); next(4, "Off bypasses Preview and releases processing"); return false;
            case 4:
                if (Access::limiterEnabled(main) || Access::limiterPreviewing(main) || !limiterResourcesEnded()) return false;
                expect(!Access::limiterPrepared(main) && !ended(limiterViewId) && limiterView != nullptr,
                    "Off terminates processor/render/state/publication while retaining the real View");
                clickLimiter("limiter.enabled"); next(5, "explicit re-enable still does not resume Preview"); return false;
            case 5:
                if (!Access::limiterEnabled(main)) return false;
                expect(!Access::limiterPrepared(main) && !Access::limiterPreviewing(main), "Off-to-On never resumes audition implicitly");
                clickLimiter("limiter.preview"); next(6, "new Preview before inactive tab"); return false;
            case 6:
                if (!Access::limiterPreviewing(main)) return false;
                rememberLimiterPreview();
                expect(limiterResources[0] != firstLimiterProcessor, "Preview after Off obtains a new processor identity");
                expect(host->dockModule(WorkspaceModuleId::truePeakLimiter, WorkspaceModuleId::outputMeter,
                    WorkspaceDockZone::centre) == WorkspaceGridMutationResult::committed, "real dock operation joins the two Views into one tab stack");
                (void) host->activateTab(WorkspaceModuleId::outputMeter);
                next(7, "inactive Limiter tab stops its real Preview"); return false;
            case 7:
                if (Access::limiterPreviewing(main) || !limiterResourcesEnded()) return false;
                // Main's existing Limiter descriptor explicitly forbids
                // inactive View residency. Only its parameter state survives.
                expect(limiterView == nullptr && !Access::limiterPrepared(main) && Access::limiterEnabled(main) && ended(limiterViewId)
                    && has(limiterViewId, Action::quiesced),
                    "inactive Limiter tab evicts its View and work under its descriptor while preserving user state");
                expect(host->activateTab(WorkspaceModuleId::truePeakLimiter) == WorkspaceGridMutationResult::committed,
                    "actual tab activation restores Limiter");
                next(8, "restored tab recreates View without resuming audition"); return false;
            case 8:
                limiterView = host->getModuleComponent(WorkspaceModuleId::truePeakLimiter);
                if (limiterView == nullptr || !limiterView->isShowing()) return false;
                expect(newest(Owner::truePeakLimiter, Kind::ownedView) != limiterViewId && Access::limiterEnabled(main)
                    && !Access::limiterPrepared(main) && !Access::limiterPreviewing(main),
                    "restore creates a fresh View from saved enabled state with processing still unrequested");
                limiterViewId = newest(Owner::truePeakLimiter, Kind::ownedView);
                clickLimiter("limiter.preview"); next(9, "active Preview is superseded by real ordinary opens"); return false;
            case 9:
                if (!Access::limiterPreviewing(main)) return false;
                rememberLimiterPreview();
                // Consecutive public open calls on this message turn. Do not
                // inject old callbacks or synthesize readiness/generations.
                burstStartSequence = observer.attemptedEventCount();
                main.openInitialFile(firstFile);
                sameFileGeneration = Access::generation(main);
                expect(sameFileGeneration > firstGeneration && !Access::ready(main), "same-file request begins a real generation before its first presentation");
                burstGenerations.add(static_cast<juce::int64>(sameFileGeneration));
                main.openInitialFile(secondFile);
                secondGeneration = Access::generation(main);
                expect(secondGeneration > sameFileGeneration && !Access::ready(main), "different-file request supersedes the unready generation");
                burstGenerations.add(static_cast<juce::int64>(secondGeneration));
                main.openInitialFile(firstFile);
                finalBurstGeneration = Access::generation(main);
                expect(finalBurstGeneration > secondGeneration && !Access::ready(main), "last same-file request supersedes B before event dispatch");
                burstGenerations.add(static_cast<juce::int64>(finalBurstGeneration));
                next(10, "only the last real open may publish current readiness"); return false;
            case 10:
                if (!Access::ready(main) || !limiterResourcesEnded()) return false;
                expect(Access::generation(main) == finalBurstGeneration && Access::document(main).getFileInfo().file == firstFile
                    && engine.hasLoadedFile(), "late work cannot replace the last requested ordinary file or generation");
                checkCore(finalBurstGeneration);
                expect(ended(firstHost) && ended(limiterViewId) && !Access::limiterPrepared(main)
                    && !Access::limiterPreviewing(main), "generation replacement terminates old host/View and Preview without automatic resurrection");
                expect(Access::playlist(main).getNumEntries() == 2, "rapid opens retain exactly the two actual source files");
                command(main, "document.playlist.remove-current"); removals = 1;
                next(11, "remove last current file through the existing document command"); return false;
            case 11:
                if (!Access::ready(main)) return false;
                expect(Access::playlist(main).getNumEntries() == 1, "first removal leaves only B");
                checkCore(Access::generation(main));
                command(main, "document.playlist.remove-current"); ++removals;
                next(12, "document close after supersession"); return false;
            case 12:
                expect(removals == 2 && Access::playlist(main).getNumEntries() == 0
                    && !Access::document(main).hasAudio() && !engine.hasLoadedFile(), "last actual removal closes document and audio source");
                documentCloseSequence = observer.attemptedEventCount(); next(13, "normal app shutdown owns the final terminals"); return true;
            default: return true;
        }
    }
    void checkCore(std::uint64_t generation)
    {
        std::uint64_t began = 0, first = 0, playable = 0, core = 0;
        for (const auto& event : events()) if (event.resourceId == 0 && event.generation == generation)
        {
            auto setOnce = [&](std::uint64_t& value) { if (value) throw std::runtime_error("duplicate real Core milestone"); value = event.sequence; };
            if (event.action == Action::generationBegan) setOnce(began);
            if (event.action == Action::firstWaveform) setOnce(first);
            if (event.action == Action::playable) setOnce(playable);
            if (event.action == Action::coreReady) setOnce(core);
        }
        expect(generation != 0 && began != 0 && first > began && playable > began && core > first && core > playable,
            "generation CoreReady follows both actual first-waveform and playable callbacks");
        for (const auto& event : events())
            if (event.generation == generation && event.kind == Kind::workspaceHost && event.action == Action::factoryRequested)
                expect(event.sequence > core, "real Host factory remains after this generation's CoreReady");
    }
    void validateSupersededGenerations()
    {
        const auto raw = events();
        const auto final = std::find_if(raw.begin(), raw.end(), [&](const auto& e)
        { return e.action == Action::generationBegan && e.generation == finalBurstGeneration; });
        expect(final != raw.end() && final->sequence > burstStartSequence, "last requested generation has a real begin event");
        // Cancelling an in-progress ordinary open can create an internal
        // return-to-previous-document generation. Include every actual begin,
        // not just the generation visible after each public open call returns.
        bool sawSame = false, sawDifferent = false;
        for (const auto& begin : raw)
        {
            if (begin.action != Action::generationBegan || begin.sequence <= burstStartSequence
                || begin.sequence >= final->sequence) continue;
            sawSame |= begin.generation == sameFileGeneration;
            sawDifferent |= begin.generation == secondGeneration;
            supersededGenerations.add(static_cast<juce::int64>(begin.generation));
            const auto invalid = std::any_of(raw.begin(), raw.end(), [&](const auto& e)
            {
                const auto acquire = e.action == Action::factoryRequested || e.action == Action::published
                    || e.action == Action::borrowedAttached;
                return e.generation == begin.generation && (e.action == Action::coreReady
                    || (acquire && !lifecycle_test::isBackendKind(e.kind)));
            });
            expect(!invalid, "every superseded generation including internal return has no CoreReady or observed Mag acquisition through shutdown");
        }
        expect(sawSame && sawDifferent, "complete begin-event inventory includes both explicitly superseded requests");
    }
    void sharedReaders(AudioSuitePreviewHandle handle, std::uint64_t processor)
    {
        expect(handle.isValid(), "actual service factory/analyze creates usable preview");
        auto state = handle.getState(); auto render = state->renderSession;
        const auto stateId = state->observedResourceId(), renderId = render->observedResourceId();
        expect(stateId != 0 && renderId != 0 && processor != 0,
            "real processor, preview state and render session have distinct observed resources");
        for (const auto& event : events())
        {
            if (event.resourceId == stateId) expect(event.relatedResource == renderId, "preview state names its real render session");
            if (event.resourceId == renderId) expect(event.relatedResource == processor, "render session names its real processor");
        }
        handle.cancel(); handle = {};
        expect(!ended(stateId) && !ended(renderId) && !state->isActive(),
            "cancel and handle release leave independently held actual state/session alive");
        state.reset();
        expect(ended(stateId) && !ended(renderId), "last state reference releases state but retained session stays alive");
        render.reset();
        expect(ended(renderId), "only last real render-session reference produces its terminal");
    }
    void audioSuiteSharedReaders(MainComponent& main)
    {
        const ScopedOptionalLifecycleContext context{{&observer, 1, Access::generation(main)}};
        const auto state = Access::document(main).getState();
        {
            auto processor = AudioSuiteReferenceProcessor::describe().create();
            const auto analysis = processor->analyze(state, static_cast<int64_t>(Access::generation(main)), {}, 0.8f);
            expect(analysis.succeeded(), "real Reference analysis on actual A succeeds");
            sharedReaders(processor->createPreview(analysis.plan), processor->observedResourceId());
        }
        {
            auto processor = TruePeakLimiterProcessor::describe().create();
            auto parameters = TruePeakLimiterParameters::masteringDefault();
            parameters.enabled = true; // Explicit service-level trigger; the product default remains Off.
            const auto analysis = processor->analyze(state, static_cast<int64_t>(Access::generation(main)), {},
                parameters);
            if (!analysis.succeeded()) std::cerr << "Limiter service analysis: " << analysis.errorMessage << std::endl;
            expect(analysis.succeeded(), "real Limiter analysis on actual A succeeds");
            sharedReaders(processor->createPreview(analysis.plan), processor->observedResourceId());
        }
    }
    void startSpatialService(std::uint64_t generation)
    {
        const ScopedOptionalLifecycleContext context{{&observer, 1, generation}};
        spatialServiceStart = observer.attemptedEventCount();
        spatialService = std::make_unique<SpatialEnergyService>();
        const auto geometry = AmbisonicStandardMatrixCatalog::standardProfile("standard-stereo-v1");
        expect(geometry != nullptr && geometry->speakers.size() == 2, "use actual standard speaker geometry for service-level fixture");
        DeviceRoutePlan::Bus bus {geometry->profileId.toStdString(), geometry->revision, {}};
        for (const auto& speaker : geometry->speakers) bus.speakers.push_back({speaker.stableId.toStdString(), speaker.role, true});
        const DeviceOutput::Snapshot device {"Fixture only", "mag-lifecycle:service", "No installed device", 48000, 128, 16,
            {0, 1}, {"0", "1"}, 0, 96};
        auto draft = DeviceRoutePlan::propose(bus, device); draft.outputsBySpeaker = {{0}, {1}};
        auto route = DeviceRoutePlan::validate(draft, device, bus).approved();
        expect(route != nullptr, "real route validator accepts explicitly fixture-only Spatial service input");
        spatialFirst = spatialService->acquire(true); spatialSecond = spatialService->acquire(true);
        spatialService->configure({route, geometry, device});
        expect(static_cast<bool>(spatialFirst) && static_cast<bool>(spatialSecond)
            && spatialService->diagnostics().consumers == 2, "two actual service consumers share one Spatial worker");
        const auto workers = matching(Owner::spatialEnergy, Kind::spatialWorker, Action::created, spatialServiceStart);
        expect(workers.size() == 1, "first service consumer creates exactly one real worker");
        serviceWorker = workers.front().resourceId;
    }
    void finishSpatialService()
    {
        // Select the State by this service worker, not another real Main panel's
        // concurrent resources with the same owner/generation.
        std::vector<std::uint64_t> states;
        for (const auto& event : matching(Owner::spatialEnergy, Kind::spatialState, Action::created, spatialServiceStart))
            if (event.relatedResource == serviceWorker) states.push_back(event.resourceId);
        expect(states.size() == 1, "actual shared worker prepared one State with its own identity");
        const auto state = states.front();
        spatialFirst = {};
        expect(spatialService->diagnostics().consumers == 1 && spatialService->diagnostics().liveMeters == 1
            && !ended(serviceWorker) && !ended(state), "first consumer release retains shared Spatial worker and State");
        spatialSecond = {};
        expect(spatialService->diagnostics().consumers == 0 && !spatialService->diagnostics().workerRunning
            && spatialService->diagnostics().liveMeters == 0 && ended(serviceWorker) && ended(state),
            "last consumer joins the actual worker and destroys its retired State");
        expect(has(serviceWorker, Action::taskStarted) && has(serviceWorker, Action::taskCompleted),
            "real Spatial worker records actual thread start and completion");
        spatialService.reset();
    }
    void ambisonicIndependentConsumers(std::uint64_t generation)
    {
        const ScopedOptionalLifecycleContext context{{&observer, 1, generation}};
        SingleTrackDocument document;
        AudioEngine engine(document);
        expect(engine.loadFile(secondFile) && document.setChannelLayoutPreset("foa-acn-sn3d"),
            "service-level actual engine loads B and explicitly selects FOA interpretation");
        const auto before = observer.attemptedEventCount();
        auto field = engine.acquireAmbisonicFieldAnalysisLease();
        expect(static_cast<bool>(field) && engine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "actual field consumer creates its analyzer");
        const auto analyzer = newest(Owner::ambisonics, Kind::ambisonicFieldAnalyzer);
        juce::AudioBuffer<float> rendered(2, 256);
        expect(engine.renderDocumentBlockForDiagnostics(0, rendered) && engine.hasAmbisonicStageGraphForDiagnostics(),
            "device-free actual document render independently acquires playback graph");
        const auto graph = newest(Owner::ambisonics, Kind::ambisonicStageGraph);
        expect(!matching(Owner::ambisonics, Kind::ambisonicPlaybackConsumer, Action::created, before).empty(),
            "actual playback demand has a separately observed consumer");
        field = {};
        expect(!engine.hasAmbisonicFieldAnalyzerForDiagnostics() && ended(analyzer)
            && engine.hasAmbisonicStageGraphForDiagnostics() && !ended(graph),
            "field release destroys analyzer while independent playback keeps graph alive");
        expect(document.setChannelLayoutPreset("quad-lrlsrs"), "real document changes FOA interpretation to ordinary Quad");
        expect(engine.renderDocumentBlockForDiagnostics(0, rendered) && !engine.hasAmbisonicStageGraphForDiagnostics()
            && ended(graph), "ordinary interpretation withdraws and truly destroys the no-longer-used graph");
    }
    bool applySettled() const
    {
        const auto tasks = matching(Owner::parametricEq, Kind::applyTask, Action::factoryRequested, applyStart);
        const auto callbacks = matching(Owner::parametricEq, Kind::completionCallback, Action::factoryRequested, applyStart);
        if (tasks.size() != 1 || callbacks.size() != 1) return false;
        return ended(tasks.front().resourceId) && ended(callbacks.front().resourceId);
    }
    void checkApply()
    {
        const auto task = matching(Owner::parametricEq, Kind::applyTask, Action::factoryRequested, applyStart).front().resourceId;
        const auto callback = matching(Owner::parametricEq, Kind::completionCallback, Action::factoryRequested, applyStart).front().resourceId;
        expect(has(task, Action::taskStarted) && has(task, Action::cancelRequested) && has(task, Action::taskCompleted),
            "real Apply records start, cancellation request and actual completion before release");
        const auto outcomes = static_cast<int>(has(task, Action::taskSucceeded)) + static_cast<int>(has(task, Action::taskCancelled))
            + static_cast<int>(has(task, Action::taskRejected));
        expect(outcomes == 1, "actual Apply has one observed outcome; cancellation request is not an outcome");
        expect(has(callback, Action::callbackPostRequested)
            && (has(callback, Action::callbackInvoked) != has(callback, Action::callbackDiscarded)),
            "actual completion delivery is invoked or discarded exactly once before its final release");
    }
    void validateTerminalStream()
    {
        expect(observer.failureFlags() == 0 && observer.outstandingResourceCount() == 0,
            "actual app/engine shutdown leaves no observer failures or acquired resources");
        const auto raw = events();
        expect(raw.size() == observer.attemptedEventCount(), "every attempted event was committed without overflow");
        const auto resources = lifecycle_test::validateResourceStream(observer, raw);
        expect(!resources.empty(), "all acquired identities have one terminal with their original creation generation");
        // At least one old Host was released after beginning its replacement
        // generation. The complete stream validates that release still names A.
        std::uint64_t switched = 0, oldRelease = 0;
        for (const auto& event : raw)
        {
            if (event.generation == sameFileGeneration && event.action == Action::generationBegan) switched = event.sequence;
            if (event.resourceId == firstHost && event.action == Action::released)
            { oldRelease = event.sequence; expect(event.generation == firstGeneration, "old Host terminal retains creation generation"); }
        }
        expect(switched != 0 && oldRelease > switched, "actual same-file generation switch releases the old Host under its old identity");
    }

    juce::File artifacts, firstFile, secondFile;
    OptionalLifecycleObserver& observer;
    ScopedOptionalLifecycleContext armed;
    juce::Array<juce::var> checks, stages;
    juce::Component::SafePointer<juce::Component> meterView, spatialView;
    std::unique_ptr<SpatialEnergyService> spatialService;
    SpatialEnergyService::Lease spatialFirst, spatialSecond;
    std::uint64_t firstGeneration = 0, sameFileGeneration = 0, secondGeneration = 0;
    std::uint64_t firstHost = 0, firstMeter = 0, spatialVisual = 0, spatialTimer = 0;
    std::uint64_t serviceWorker = 0, spatialServiceStart = 0, applyStart = 0, documentCloseSequence = 0;
    int phase = 0, removals = 0, discardedPrompts = 0;
    const bool transitionsOnly;
    juce::Component::SafePointer<juce::Component> limiterView;
    std::array<std::uint64_t, 4> limiterResources {};
    std::uint64_t limiterViewId = 0, firstLimiterProcessor = 0, finalBurstGeneration = 0, burstStartSequence = 0;
    juce::Array<juce::var> burstGenerations, supersededGenerations;
};

std::shared_ptr<MagLifecycleTestSession> createMagLifecycleTests(const juce::File& artifacts, bool remainingTransitions)
{ return std::make_shared<MagLifecycleTestSession>(artifacts, remainingTransitions); }
bool advanceMagLifecycleTests(MagLifecycleTestSession& session, MainComponent& main, AudioEngine& engine)
{ return session.advance(main, engine); }
void finishMagLifecycleTests(MagLifecycleTestSession& session, bool completed) { session.finish(completed); }
} // namespace singleeditor
