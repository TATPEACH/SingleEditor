#include "OptionalLifecycleTestSupport.h"
#include "Audio/AudioEngine.h"
#include "UI/MainComponent.h"
#include "UI/Workspace/ModernWorkspaceScene.h"
#include <array>
#include <map>
#include <stdexcept>
#include <vector>

#if !SINGLE_EDITOR_ENABLE_TEST_OBSERVERS || !SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
 #error OptionalLifecycleTests belongs only to the native observer test target
#endif

namespace singleeditor
{
// This seam invokes product commands and reads state. It never constructs an
// optional product object, changes a generation, or marks a Core milestone.
struct OptionalLifecycleTestAccess
{
    static std::uint64_t generation(const MainComponent& main)
    { return main.activeDocumentGeneration.value; }
    static bool ready(const MainComponent& main)
    { return main.isCoreReadyForWorkspaceResources(); }
    static bool workspaceHost(const MainComponent& main) { return main.workspaceModuleHost != nullptr; }
    static bool command(MainComponent& main, std::string_view id)
    { return main.dispatchGuiCommand(id, GuiCommandSource::mag, "workspace.modules").succeeded(); }
    static juce::Component* eqView(const MainComponent& main)
    {
        return main.workspaceModuleHost == nullptr ? nullptr
            : main.workspaceModuleHost->getModuleComponent(WorkspaceModuleId::parametricEq);
    }
    static bool processor(const MainComponent& main) { return main.parametricEqProcessor != nullptr; }
    static bool preview(const MainComponent& main) { return main.parametricEqPreview.isValid(); }
};

namespace
{
using Action = OptionalLifecycleAction;
using Kind = OptionalLifecycleResourceKind;
using Owner = OptionalLifecycleOwner;

juce::Button* findButton(juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return dynamic_cast<juce::Button*>(&component);
    for (auto* child : component.getChildren())
        if (auto* found = findButton(*child, id)) return found;
    return nullptr;
}

using lifecycle_test::actionName;
using lifecycle_test::kindName;
struct OptionalProcessChannel
{
    std::array<OptionalLifecycleEventSlot, 512> slots;
    OptionalLifecycleObserver observer {std::span{slots}};
};
OptionalLifecycleObserver& processObserver()
{
    // Failed runs may leave metadata-only callbacks queued past session teardown.
    static auto* const channel = new OptionalProcessChannel;
    return channel->observer;
}

}

class OptionalLifecycleTestSession final
{
public:
    OptionalLifecycleTestSession(juce::File output, juce::ApplicationProperties& properties)
        : artifacts(std::move(output)), observer(processObserver()), armed({ &observer, 1, 0 })
    { expect(observer.attemptedEventCount() == 0, "fresh process-lifetime lifecycle channel"); preparePrivateLayout(properties); channelChecks(); }

    void expect(bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        checks.add(juce::var(row));
        if (!ok) throw std::runtime_error(name);
    }

    std::vector<OptionalLifecycleEvent> committed() const
    {
        std::vector<OptionalLifecycleEvent> result;
        for (const auto& slot : observer.events())
            if (const auto sequence = slot.committedSequence.load(std::memory_order_acquire); sequence != 0)
            {
                if (sequence != slot.event.sequence) throw std::runtime_error("committed event sequence mismatch");
                result.push_back(slot.event);
            }
        return result;
    }

    std::vector<OptionalLifecycleEvent> matching(Owner owner, Kind kind, Action action,
                                                std::uint64_t after = 0) const
    {
        std::vector<OptionalLifecycleEvent> result;
        for (const auto& event : committed())
            if (event.owner == owner && event.kind == kind && event.action == action && event.sequence > after)
                result.push_back(event);
        return result;
    }

    std::uint64_t oneSequence(Owner owner, Kind kind, Action action) const
    {
        const auto events = matching(owner, kind, action);
        if (events.size() != 1) throw std::runtime_error("expected one exact lifecycle action");
        return events.front().sequence;
    }

    bool advance(MainComponent& main, AudioEngine& engine)
    {
        expectHealthy();
        switch (phase)
        {
            case 0:
            {
                if (!OptionalLifecycleTestAccess::ready(main))
                {
                    if (!preCoreStateSampled)
                    {
                        expect(!OptionalLifecycleTestAccess::workspaceHost(main)
                                   && !OptionalLifecycleTestAccess::processor(main)
                                   && !OptionalLifecycleTestAccess::preview(main),
                               "sampled real pre-Core Main has no workspace Host, EQ processor or preview");
                        preCoreStateSampled = true;
                    }
                    return false;
                }
                if (findButton(main, "workspace.module.parametric-eq.toggle") == nullptr) return false;
                generation = OptionalLifecycleTestAccess::generation(main);
                expect(generation != 0 && engine.hasLoadedFile(), "actual ordinary WAV reaches a nonzero document generation");
                const auto core = oneSequence(Owner::coreFile, Kind::marker, Action::coreReady);
                expect(oneSequence(Owner::coreFile, Kind::marker, Action::firstWaveform) < core
                           && oneSequence(Owner::coreFile, Kind::marker, Action::playable) < core,
                       "CoreReady follows both actual first-waveform and playable callbacks");
                expect(OptionalLifecycleTestAccess::workspaceHost(main)
                           && matching(Owner::workspace, Kind::workspaceHost, Action::factoryRequested).size() == 1
                           && matching(Owner::workspace, Kind::workspaceHost, Action::created).size() == 1,
                       "the actual workspace Host factory and instance are observed, independent of timer sampling");
                for (const auto& event : committed())
                    if (!lifecycle_test::isBackendKind(event.kind)
                        && (event.action == Action::factoryRequested || event.action == Action::published
                            || event.action == Action::borrowedAttached))
                        expect(event.sequence > core, "observed original Mag-scope acquisition occurs only after actual CoreReady");
                expect(OptionalLifecycleTestAccess::eqView(main) == nullptr
                           && !OptionalLifecycleTestAccess::processor(main)
                           && !OptionalLifecycleTestAccess::preview(main),
                       "EQ factory/View/processor/preview are absent before explicit Add");
                expect(matching(Owner::parametricEq, Kind::ownedView, Action::factoryRequested).empty()
                           && matching(Owner::parametricEq, Kind::processor, Action::factoryRequested).empty(),
                       "EQ has zero factory requests before its product trigger");
                expect(OptionalLifecycleTestAccess::command(main, "workspace.module.parametric-eq.toggle"),
                       "actual registered Add EQ command succeeds");
                phase = 1; return false;
            }
            case 1:
            {
                auto* view = OptionalLifecycleTestAccess::eqView(main);
                if (view == nullptr || !view->isShowing()) return false;
                expect(!OptionalLifecycleTestAccess::processor(main) && !OptionalLifecycleTestAccess::preview(main),
                       "actual Add creates the owned View without creating the DSP processor or preview");
                eqView = view;
                auto* preview = findButton(*view, "eq.preview");
                expect(preview != nullptr && preview->isEnabled(), "actual EQ Preview button is available");
                preview->triggerClick(); phase = 2; return false;
            }
            case 2:
            {
                if (!OptionalLifecycleTestAccess::preview(main)) return false;
                expect(OptionalLifecycleTestAccess::processor(main), "actual Preview creates the real ParametricEqProcessor");
                const auto requests = matching(Owner::parametricEq, Kind::processor, Action::factoryRequested);
                expect(requests.size() == 1
                           && matching(Owner::parametricEq, Kind::previewPublication, Action::published).size() == 1,
                       "real processor factory and engine preview publication each occur once");
                auto* stop = eqView == nullptr ? nullptr : findButton(*eqView, "eq.preview");
                expect(stop != nullptr && stop->getToggleState(), "actual Preview button reports its configured preview state");
                stop->triggerClick(); phase = 3; return false;
            }
            case 3:
                if (OptionalLifecycleTestAccess::preview(main)) return false;
                expect(!OptionalLifecycleTestAccess::processor(main) && eqView != nullptr,
                       "Off cancels publication and destroys the processor while retaining the owned View");
                expect(matching(Owner::parametricEq, Kind::previewPublication, Action::withdrawn).size() == 1
                           && matching(Owner::parametricEq, Kind::processor, Action::released).size() == 1,
                       "Off observes actual engine withdrawal and processor destructor terminal");
                expect(OptionalLifecycleTestAccess::command(main, "workspace.module.parametric-eq.remove"),
                       "actual registered Remove EQ command succeeds");
                phase = 4; return false;
            case 4:
                if (eqView != nullptr || OptionalLifecycleTestAccess::eqView(main) != nullptr) return false;
                expect(matching(Owner::parametricEq, Kind::ownedView, Action::released).size() == 1,
                       "Remove destroys the actual owned EQ View exactly once");
                // Existing toolbar views own genuine consumers. Hide this owned
                // fixture window and let their real visibility callbacks release.
                main.getTopLevelComponent()->setVisible(false);
                phase = 5; return false;
            case 5:
            {
                if (engine.getOutputMeterAnalysisLeaseCountForDiagnostics() != 0) return false;
                const auto before = observer.attemptedEventCount();
                auto first = engine.acquireOutputMeterAnalysisLease();
                auto second = engine.acquireOutputMeterAnalysisLease();
                expect(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 2,
                       "two real consumers acquire the same engine meter producer");
                const auto states = matching(Owner::outputMeter, Kind::meterState, Action::created, before);
                const auto consumers = matching(Owner::outputMeter, Kind::meterConsumer, Action::created, before);
                expect(states.size() == 1 && consumers.size() == 2
                           && consumers[0].resourceId != consumers[1].resourceId
                           && consumers[0].relatedResource == states[0].resourceId
                           && consumers[1].relatedResource == states[0].resourceId,
                       "two distinct consumer identities bind one actual shared MeterState identity");
                sharedProducer = states[0].resourceId;
                first = {};
                expect(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 1
                           && matching(Owner::outputMeter, Kind::meterState, Action::released, before).empty(),
                       "first consumer release keeps the independently consumed shared producer alive");
                second = {};
                const auto releases = matching(Owner::outputMeter, Kind::meterState, Action::released, before);
                expect(engine.getOutputMeterAnalysisLeaseCountForDiagnostics() == 0
                           && releases.size() == 1 && releases[0].resourceId == sharedProducer
                           && matching(Owner::outputMeter, Kind::meterConsumer, Action::released, before).size() == 2,
                       "last consumer release observes the real MeterState destructor and both consumer terminals");
                phase = 6; return true;
            }
            default: return true;
        }
    }

    void finish(bool scenarioCompleted)
    {
        juce::String error;
        try
        {
            expect(scenarioCompleted && phase == 6, "the single real native scenario completed before normal close");
            validateTerminalStream();
        }
        catch (const std::exception& e) { error = e.what(); }
        juce::Array<juce::var> events;
        for (const auto& event : committed())
        {
            auto* row = new juce::DynamicObject();
            row->setProperty("sequence", static_cast<juce::int64>(event.sequence));
            row->setProperty("resource_id", static_cast<juce::int64>(event.resourceId));
            row->setProperty("session", static_cast<juce::int64>(event.session));
            row->setProperty("generation", static_cast<juce::int64>(event.generation));
            row->setProperty("related_resource", static_cast<juce::int64>(event.relatedResource));
            row->setProperty("owner", optionalLifecycleOwnerId(event.owner));
            row->setProperty("kind", kindName(event.kind));
            row->setProperty("action", actionName(event.action));
            row->setProperty("operation_id", lifecycle_test::operationId(event));
            events.add(juce::var(row));
        }
        auto* report = new juce::DynamicObject();
        report->setProperty("schema", "singleeditor.native-optional-lifecycle-scenario");
        report->setProperty("version", 1);
        report->setProperty("status", error.isEmpty() ? "pass" : "failed");
        report->setProperty("release_eligible", false);
        report->setProperty("scope", "one ordinary WAV; actual EQ Add/Preview/Off/Remove; owned-window Hide; two actual shared meter consumers; normal close");
        report->setProperty("backend_evidence", "Raw backend events including generation zero are retained and strictly validated; the pre-Core assertion covers only the original Mag scope, not all backends or the full D158 gate.");
        report->setProperty("preview_evidence", "engine publication and withdrawal; not final shared preview-state destruction");
        report->setProperty("playback_evidence", "actual Preview may initialize a device; no physical playback, analyzer activity, route acceptance, or timing result is asserted");
        report->setProperty("uncovered", "other optional modules, generations, supersession, device recovery, formal Release lazy trace, complete D158 acceptance");
        report->setProperty("failure_flags", static_cast<int>(observer.failureFlags()));
        report->setProperty("attempted_events", static_cast<juce::int64>(observer.attemptedEventCount()));
        report->setProperty("outstanding_resources", static_cast<juce::int64>(observer.outstandingResourceCount()));
        report->setProperty("pre_core_state_sampled", preCoreStateSampled);
        report->setProperty("fixture_layout", artifacts.getChildFile("optional-lifecycle-layout.json").getFullPathName());
        report->setProperty("fixture_layout_change", "approved default layout with only parametric-eq enabled=false and placed=false before any product construction");
        report->setProperty("checks", checks); report->setProperty("events", events); report->setProperty("error", error);
        const auto file = artifacts.getChildFile("optional-lifecycle.json");
        if (file.exists() || !file.replaceWithText(juce::JSON::toString(juce::var(report), true)))
            throw std::runtime_error("cannot write fresh optional lifecycle scenario evidence");
        if (error.isNotEmpty()) throw std::runtime_error(error.toStdString());
    }

private:
    void preparePrivateLayout(juce::ApplicationProperties& properties)
    {
        // Preserve the approved scene, including all other enabled/placed Mags.
        // This scenario needs a genuinely absent EQ before its first Add; it
        // must not remove an already-created View to manufacture a zero count.
        auto layout = juce::JSON::parse(modernWorkspaceDefaultLayout);
        auto* modules = layout["modules"].getArray();
        expect(layout.isObject() && modules != nullptr, "parse the actual approved default workspace layout");
        juce::DynamicObject* eq = nullptr;
        int matches = 0;
        for (auto& module : *modules)
        {
            if (module["moduleId"].toString() != "parametric-eq") continue;
            ++matches;
            expect(module["instanceId"].toString() == "parametric-eq" && module.isObject(),
                   "the actual EQ fixture node has the exact module and instance identity");
            eq = module.getDynamicObject();
        }
        expect(matches == 1 && eq != nullptr && static_cast<bool>(eq->getProperty("enabled"))
                   && static_cast<bool>(eq->getProperty("placed")),
               "the approved scene has one enabled and placed EQ node before fixture adaptation");
        eq->setProperty("enabled", false);
        eq->setProperty("placed", false);
        auto* settings = properties.getUserSettings();
        expect(settings != nullptr, "private application settings exist before product construction");
        const auto serialized = juce::JSON::toString(layout, true);
        const auto layoutFile = artifacts.getChildFile("optional-lifecycle-layout.json");
        expect(!layoutFile.exists() && layoutFile.replaceWithText(serialized),
               "preserve the exact fixture layout for review without overwriting previous evidence");
        settings->setValue("workspaceModuleLayoutV2", serialized);
        expect(settings->getValue("workspaceModuleLayoutV2") == serialized,
               "only the private fixture settings receive the adapted approved layout");
    }

    void expectHealthy() const
    {
        if (observer.failureFlags() != 0) throw std::runtime_error("optional lifecycle observer reported a harness failure");
    }

    void channelChecks()
    {
        {
            std::array<OptionalLifecycleEventSlot, 1> buffer;
            OptionalLifecycleObserver probe{std::span{buffer}};
            const OptionalLifecycleContext context{&probe, 91, 1};
            probe.milestone(context, Action::firstWaveform); probe.milestone(context, Action::playable);
            expect(probe.failureFlags() == static_cast<std::uint32_t>(OptionalLifecycleFailure::overflow)
                       && probe.attemptedEventCount() == 2 && buffer[0].committedSequence.load() == 1,
                   "bounded channel overflow is explicit and never silently overwrites an event");
        }
        {
            std::array<OptionalLifecycleEventSlot, 8> buffer;
            OptionalLifecycleObserver probe{std::span{buffer}};
            auto token = OptionalLifecycleToken::request({&probe, 92, 1}, Owner::parametricEq, Kind::processor);
            token.created(); token.release(); token.release();
            expect(probe.failureFlags() == static_cast<std::uint32_t>(OptionalLifecycleFailure::invalidTransition)
                       && probe.outstandingResourceCount() == 0 && probe.attemptedEventCount() == 3,
                   "duplicate terminal is a harness failure without a second terminal event");
        }
        {
            std::array<OptionalLifecycleEventSlot, 8> buffer;
            OptionalLifecycleObserver probe{std::span{buffer}};
            { auto token = OptionalLifecycleToken::request({&probe, 93, 1}, Owner::parametricEq, Kind::processor); token.created(); }
            expect(probe.failureFlags() == static_cast<std::uint32_t>(OptionalLifecycleFailure::missingTerminal)
                       && probe.outstandingResourceCount() == 1,
                   "missing terminal is retained as a nonzero balance and explicit harness failure");
        }
        {
            std::array<OptionalLifecycleEventSlot, 8> buffer;
            OptionalLifecycleObserver probe{std::span{buffer}};
            OptionalLifecycleContext context{&probe, 94, 3};
            auto original = OptionalLifecycleToken::request(context, Owner::outputMeter, Kind::meterConsumer, 17);
            original.created(); const auto id = original.resourceId();
            context.generation = 4;
            const ScopedOptionalLifecycleContext changedContext{context};
            auto moved = std::move(original); moved.release();
            expect(original.resourceId() == 0 && moved.resourceId() == id && probe.failureFlags() == 0
                       && probe.outstandingResourceCount() == 0 && probe.attemptedEventCount() == 3
                       && buffer[2].event.resourceId == id && buffer[2].event.relatedResource == 17
                       && buffer[2].event.generation == 3 && moved.creationContext().generation == 3,
                   "token move and later context changes retain original identity and generation without a new acquire");
        }
    }

    void validateTerminalStream()
    {
        expect(observer.failureFlags() == 0 && observer.outstandingResourceCount() == 0,
               "after actual window and engine destruction the observer has no failures or open resources");
        const auto events = committed();
        expect(events.size() == observer.attemptedEventCount(), "every attempted event is committed in the bounded channel");
        (void) lifecycle_test::validateResourceStream(observer, events);
        struct Resource { OptionalLifecycleEvent first; bool created = false, active = false, terminal = false; };
        std::map<std::uint64_t, Resource> resources;
        std::uint64_t sequence = 0;
        for (const auto& event : events)
        {
            if (event.sequence != ++sequence || event.session != 1)
                throw std::runtime_error("event order/session identity is incomplete");
            if (event.resourceId == 0) continue; // Real product milestones/commands are non-owning markers.
            if (lifecycle_test::isBackendKind(event.kind)) continue; // Already fully validated above; original Mag checks remain below.
            if (event.action == Action::factoryRequested || event.action == Action::borrowedAttached || event.action == Action::published)
            {
                if (event.relatedResource != 0)
                {
                    const auto related = resources.find(event.relatedResource);
                    if (related == resources.end() || related->second.terminal || !related->second.created
                        || related->second.first.owner != event.owner || related->second.first.generation != event.generation)
                        throw std::runtime_error("acquisition refers to an absent, released or differently owned producer");
                }
                if (event.generation != generation || !resources.emplace(event.resourceId, Resource{event}).second)
                    throw std::runtime_error("resource generation or unique acquisition identity mismatch");
                continue;
            }
            auto found = resources.find(event.resourceId);
            if (found == resources.end()) throw std::runtime_error("resource event without its original acquisition");
            auto& resource = found->second;
            const auto& first = resource.first;
            if (resource.terminal || first.session != event.session || first.generation != event.generation
                || first.owner != event.owner || first.kind != event.kind || first.relatedResource != event.relatedResource)
                throw std::runtime_error("post-terminal access or changed resource identity");
            switch (event.action)
            {
                case Action::created:
                    if (first.action != Action::factoryRequested || resource.created) throw std::runtime_error("invalid real create");
                    resource.created = true; break;
                case Action::activated:
                    if (resource.active || (first.action == Action::factoryRequested && !resource.created)) throw std::runtime_error("invalid activation");
                    resource.active = true; break;
                case Action::quiesced:
                    if (!resource.active) throw std::runtime_error("quiesce without activation");
                    resource.active = false; break;
                case Action::factoryFailed:
                    if (resource.created || first.action != Action::factoryRequested) throw std::runtime_error("invalid factory failure");
                    resource.terminal = true; break;
                case Action::released:
                    if (!resource.created || resource.active || first.action != Action::factoryRequested) throw std::runtime_error("release before real create/quiesce");
                    resource.terminal = true; break;
                case Action::borrowedDetached:
                    if (first.action != Action::borrowedAttached || resource.active) throw std::runtime_error("invalid borrowed detach");
                    resource.terminal = true; break;
                case Action::withdrawn:
                    if (first.action != Action::published || resource.active) throw std::runtime_error("invalid publication withdrawal");
                    resource.terminal = true; break;
                default: throw std::runtime_error("unexpected or missing resource terminal action");
            }
        }
        for (const auto& [id, resource] : resources)
        {
            juce::ignoreUnused(id);
            if (!resource.terminal) throw std::runtime_error("resource has no unique terminal after actual shutdown");
        }
        expect(!resources.empty(), "all actual acquired identities have exactly one matching terminal");
        const auto add = oneSequence(Owner::parametricEq, Kind::marker, Action::moduleEnableRequested);
        const auto request = oneSequence(Owner::parametricEq, Kind::ownedView, Action::factoryRequested);
        const auto created = oneSequence(Owner::parametricEq, Kind::ownedView, Action::created);
        const auto preview = oneSequence(Owner::parametricEq, Kind::marker, Action::previewRequested);
        const auto processor = oneSequence(Owner::parametricEq, Kind::processor, Action::factoryRequested);
        const auto published = oneSequence(Owner::parametricEq, Kind::previewPublication, Action::published);
        const auto off = oneSequence(Owner::parametricEq, Kind::marker, Action::previewOffRequested);
        const auto withdrawn = oneSequence(Owner::parametricEq, Kind::previewPublication, Action::withdrawn);
        const auto processorReleased = oneSequence(Owner::parametricEq, Kind::processor, Action::released);
        const auto remove = oneSequence(Owner::parametricEq, Kind::marker, Action::moduleRemoveRequested);
        const auto viewReleased = oneSequence(Owner::parametricEq, Kind::ownedView, Action::released);
        expect(add < request && request < created && created < preview && preview < processor
                   && processor < published && published < off && off < withdrawn
                   && withdrawn < processorReleased && processorReleased < remove && remove < viewReleased,
               "real Add, View factory, Preview, processor, publication, Off and Remove form the exact causal sequence");
    }

    juce::File artifacts;
    OptionalLifecycleObserver& observer;
    ScopedOptionalLifecycleContext armed;
    juce::Array<juce::var> checks;
    juce::Component::SafePointer<juce::Component> eqView;
    std::uint64_t generation = 0, sharedProducer = 0;
    bool preCoreStateSampled = false;
    int phase = 0;
};

std::shared_ptr<OptionalLifecycleTestSession> createOptionalLifecycleTests(const juce::File& artifacts,
                                                                       juce::ApplicationProperties& properties)
{ return std::make_shared<OptionalLifecycleTestSession>(artifacts, properties); }
bool advanceOptionalLifecycleTests(OptionalLifecycleTestSession& session, MainComponent& main, AudioEngine& engine)
{ return session.advance(main, engine); }
void finishOptionalLifecycleTests(OptionalLifecycleTestSession& session, bool completed)
{ session.finish(completed); }
} // namespace singleeditor
