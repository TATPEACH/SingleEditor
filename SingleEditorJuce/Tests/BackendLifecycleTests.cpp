#include "OptionalLifecycleTestSupport.h"
#include "UI/MainComponent.h"
#include "Recovery/RecoveryService.h"
#include <array>
#include <map>
#include <memory>

#if !SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
 #error BackendLifecycleTests belongs to the existing native test target
#endif

namespace singleeditor
{
struct BackendLifecycleTestAccess
{
    static std::uint64_t generation(const MainComponent& main) { return main.activeDocumentGeneration.value; }
    static media::OptionalMediaOperation::DiagnosticHandle mediaDiagnostic(const MainComponent& main)
    { return main.lastOptionalDiagnostic; }
    static const recovery::RecoveryService* recovery(const MainComponent& main) { return main.recoveryService.get(); }
    static AudioEngine::DeviceDiagnostics device(const MainComponent& main) { return main.audioEngine.getDeviceDiagnostics(); }
    static std::weak_ptr<const void> bankWorker(const MainComponent& main)
    { return main.bnkRuntime ? main.bnkRuntime->workerForTesting() : std::weak_ptr<const void>(); }
    static bool bankWorkerFinished(const MainComponent& main)
    { return main.bnkRuntime && main.bnkRuntime->workerFinishedForTesting(); }
};
std::weak_ptr<const void> observeNativeBnkWorker(const MainComponent& main)
{ return BackendLifecycleTestAccess::bankWorker(main); }
bool nativeBnkWorkerFinished(const MainComponent& main)
{ return BackendLifecycleTestAccess::bankWorkerFinished(main); }
media::OptionalMediaOperation::DiagnosticHandle observeNativeBnkDiagnostic(const MainComponent& main)
{ return BackendLifecycleTestAccess::mediaDiagnostic(main); }
namespace
{
using namespace lifecycle_test;
struct BackendProcessChannel
{
    std::array<OptionalLifecycleEventSlot, 4096> slots;
    OptionalLifecycleObserver observer {std::span{slots}};
};
OptionalLifecycleObserver& processObserver()
{
    // Metadata only; survives Application::shutdown and delayed JUCE closures.
    // A failed run retains open-resource evidence instead of destroying its sink.
    static auto* const channel = new BackendProcessChannel;
    return channel->observer;
}
}

class BackendLifecycleTestSession final
{
public:
    BackendLifecycleTestSession(juce::File output, juce::String name)
        : artifacts(std::move(output)), scenario(std::move(name)), observer(processObserver()), armed({&observer, 1, 0})
    {
        expect(scenario == "recovery-save" || scenario == "packed-close" || scenario == "diagnostics-device" || scenario == "diagnostics-probe-real",
            "observe one of the selected existing bounded native workflows");
        expect(observer.attemptedEventCount() == 0, "fresh bounded process-lifetime observation channel");
    }
    OptionalLifecycleContext context(const MainComponent& main) noexcept
    { return {&observer, 1, BackendLifecycleTestAccess::generation(main)}; }
    void sample(MainComponent& main)
    {
        if (observer.failureFlags()) throw std::runtime_error("backend lifecycle channel failure");
        auto handle = BackendLifecycleTestAccess::mediaDiagnostic(main);
        if (handle && std::none_of(mediaHandles.begin(), mediaHandles.end(), [&](const auto& item) { return item == handle; }))
        {
            if (mediaHandles.size() == 16) throw std::runtime_error("bounded media diagnostic sample capacity exceeded");
            mediaHandles.push_back(std::move(handle)); // Data-only capsule; never retains operation/worker/reader.
        }
        if (const auto* service = BackendLifecycleTestAccess::recovery(main))
        {
            const auto health = service->health();
            for (const auto& session : health.protection.sessions)
            {
                recordRecovery(session.protection.current);
                for (const auto& retired : session.protection.retired) recordRecovery(retired);
            }
        }
        const auto device = BackendLifecycleTestAccess::device(main);
        if (device.latestRequest) recordDevice(*device.latestRequest);
        if (device.latestFailure) recordDevice(*device.latestFailure);
    }
    void finish(bool completed)
    {
        juce::String failure;
        const auto raw = lifecycle_test::readEvents(observer);
        juce::Array<juce::var> mediaSamples, recoverySamples, deviceSamples;
        // Preserve final data capsules even when a later strict assertion fails.
        std::vector<media::OptionalMediaOperation::Report> reports;
        for (const auto& handle : mediaHandles)
            if (const auto snapshot = handle->snapshot())
            {
                reports.push_back(*snapshot);
                auto* row = new juce::DynamicObject;
                row->setProperty("operation_id", snapshot->operationId.toString());
                row->setProperty("resource", static_cast<juce::int64>(snapshot->observedResourceId));
                row->setProperty("module", static_cast<int>(snapshot->module));
                row->setProperty("policy_terminal", snapshot->terminal);
                row->setProperty("cancelled", snapshot->cancelled);
                row->setProperty("succeeded", snapshot->succeeded);
                if (snapshot->process)
                {
                    row->setProperty("process_resource", static_cast<juce::int64>(snapshot->process->observedResourceId));
                    row->setProperty("job_cleanup_confirmed", snapshot->process->jobCleanupConfirmed);
                    row->setProperty("process_failure", static_cast<int>(snapshot->process->failure));
                    row->setProperty("process_exit", static_cast<juce::int64>(snapshot->process->exitCode));
                }
                mediaSamples.add(juce::var(row));
            }
        for (const auto& [resource, observed] : recoveryOwners)
        {
            auto* row = new juce::DynamicObject;
            row->setProperty("state_resource", static_cast<juce::int64>(resource));
            row->setProperty("owner_id", observed.owner.toString());
            row->setProperty("latest_attempt_id", observed.operation.toString());
            row->setProperty("first_sample_sequence", static_cast<juce::int64>(observed.firstSequence));
            row->setProperty("last_sample_sequence", static_cast<juce::int64>(observed.lastSequence));
            recoverySamples.add(juce::var(row));
        }
        for (const auto& [id, request] : deviceRequests)
        {
            auto* row = new juce::DynamicObject;
            row->setProperty("operation_id", id);
            row->setProperty("kind", static_cast<int>(request.kind));
            row->setProperty("outcome", static_cast<int>(request.outcome));
            row->setProperty("reported_resources_released", request.resourcesReleased);
            deviceSamples.add(juce::var(row));
        }
        try
        {
            expect(completed, "existing native product workflow completed before actual shutdown");
            const auto resources = lifecycle_test::validateResourceStream(observer, raw);
            expect(!resources.empty(), "all actual acquired resources have strict identities and real unique terminals");
            verifyBindings(resources, reports);
        }
        catch (const std::exception& error) { failure = error.what(); }
        auto* report = new juce::DynamicObject;
        report->setProperty("schema", "singleeditor.backend-lifecycle-native"); report->setProperty("version", 1);
        report->setProperty("scenario", scenario); report->setProperty("status", failure.isEmpty() ? "pass" : "failed");
        report->setProperty("release_eligible", false);
        report->setProperty("error", failure); report->setProperty("checks", checks);
        report->setProperty("attempted_events", static_cast<juce::int64>(observer.attemptedEventCount()));
        report->setProperty("observer_failures", static_cast<int>(observer.failureFlags()));
        report->setProperty("outstanding_resources", static_cast<juce::int64>(observer.outstandingResourceCount()));
        report->setProperty("media_samples", mediaSamples); report->setProperty("recovery_samples", recoverySamples);
        report->setProperty("device_samples", deviceSamples);
        auto* coverage = new juce::DynamicObject;
        coverage->setProperty("recovery_states_created", static_cast<juce::int64>(std::count_if(raw.begin(), raw.end(), [](const auto& event)
            { return event.kind == Kind::recoveryWriterState && event.action == Action::created; })));
        coverage->setProperty("recovery_states_uuid_sampled", static_cast<juce::int64>(recoveryOwners.size()));
        coverage->setProperty("sampling_scope", "UUID samples are passive observations, not complete polling coverage. Every committed resource is independently validated.");
        report->setProperty("sample_coverage", juce::var(coverage));
        report->setProperty("scope", "Existing native scenario only. Policy terminal/workerRunning/resourcesReleased are values, never resource terminals. Generation zero startup events are retained. No OS detached-thread join, actual compressed codec, physical device or complete D158/release acceptance is claimed.");
        juce::Array<juce::var> events;
        for (const auto& event : raw) events.add(lifecycle_test::eventJson(event));
        report->setProperty("events", events);
        const auto receipt = artifacts.getChildFile("backend-lifecycle.json");
        if (receipt.exists() || !receipt.replaceWithText(juce::JSON::toString(juce::var(report), true)))
            throw std::runtime_error("write backend lifecycle receipt");
        if (failure.isNotEmpty()) throw std::runtime_error(failure.toStdString());
    }
private:
    void expect(bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject;
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        checks.add(juce::var(row));
        if (!ok) throw std::runtime_error(name);
    }
    struct RecoveryOwner
    {
        juce::Uuid owner {nullptr}, operation {nullptr};
        std::uint64_t firstSequence = 0, lastSequence = 0;
    };
    void recordRecovery(const recovery::RecoveryMutationProducer::Health& health)
    {
        const auto id = health.writer.stateResourceId;
        if (!id) return; // Clean attached sessions intentionally have no writer resource.
        if (health.owner.isNull()) throw std::runtime_error("observed Recovery writer lacks actual owner UUID");
        auto found = recoveryOwners.find(id);
        if (found == recoveryOwners.end())
        {
            if (recoveryOwners.size() == 64) throw std::runtime_error("bounded recovery owner sample capacity exceeded");
            found = recoveryOwners.emplace(id, RecoveryOwner{health.owner, health.writer.operationId,
                observer.attemptedEventCount(), observer.attemptedEventCount()}).first;
        }
        if (found->second.owner != health.owner) throw std::runtime_error("Recovery State changed owner UUID");
        found->second.operation = health.writer.operationId;
        found->second.lastSequence = observer.attemptedEventCount();
    }
    void recordDevice(const DeviceRequestRecord& request)
    {
        if (request.operationId.isNull()) return;
        const auto id = request.operationId.toString();
        if (!deviceRequests.contains(id) && deviceRequests.size() == 64) throw std::runtime_error("bounded device sample capacity exceeded");
        // Value-only request: no outputPlan/manager/driver is retained.
        deviceRequests[id] = request;
    }
    void verifyBindings(const lifecycle_test::Resources& resources,
                        const std::vector<media::OptionalMediaOperation::Report>& reports)
    {
        const auto requireResource = [&](std::uint64_t id, Kind kind, Owner owner) -> const lifecycle_test::Resource&
        {
            const auto found = resources.find(id);
            if (id == 0 || found == resources.end() || found->second.first.kind != kind || found->second.first.owner != owner
                || !found->second.ended) throw std::runtime_error("sample lacks exact observed resource kind/owner/terminal");
            return found->second;
        };
        size_t packed = 0, sampledRecoveryWorkers = 0, deviceStates = 0, warmups = 0, diagnosticProcesses = 0;
        std::map<std::uint64_t, juce::String> mediaIds;
        for (const auto& report : reports)
        {
            const auto& operation = requireResource(report.observedResourceId, Kind::optionalMediaOperation, Owner::optionalMedia);
            expect(!report.operationId.isNull(), "observed media object has its actual existing operation UUID");
            auto [at, fresh] = mediaIds.emplace(report.observedResourceId, report.operationId.toString());
            expect(fresh || at->second == report.operationId.toString(), "media resource remains bound to one actual request UUID");
            if (report.process)
            {
                const auto& process = requireResource(report.process->observedResourceId, Kind::boundedDecodeProcess, Owner::optionalMedia);
                const auto parentId = process.first.relatedResource;
                if (parentId != report.observedResourceId)
                {
                    const auto& work = requireResource(parentId, Kind::packedDecodeWork, Owner::vgmstream);
                    expect(work.first.relatedResource == report.observedResourceId
                        && work.first.generation == operation.first.generation,
                        "process bundle names this request's actual progressive Work and immutable creation generation");
                }
                expect(report.process->jobCleanupConfirmed, "existing process report independently confirms owned Job cleanup");
            }
            if (report.module == media::OptionalMediaFailureState::Module::packedAudio)
            {
                expect(report.process.has_value() && report.cancelled,
                    "packed-close captures actual cancellation and bounded process cleanup, not only policy settlement");
                ++packed;
            }
        }
        for (const auto& [id, observed] : recoveryOwners)
        {
            juce::ignoreUnused(observed);
            (void) requireResource(id, Kind::recoveryWriterState, Owner::recovery);
        }
        for (const auto& [id, resource] : resources)
        {
            juce::ignoreUnused(id);
            if (resource.first.kind == Kind::recoveryWriterWorker && resource.created)
            {
                if (recoveryOwners.contains(resource.first.relatedResource)) ++sampledRecoveryWorkers;
            }
            if (resource.first.kind == Kind::deviceOperationState && resource.created) ++deviceStates;
            if (resource.first.kind == Kind::deviceWarmupWorker && resource.created && resource.bound) ++warmups;
            if (resource.first.kind == Kind::boundedDecodeProcess && resource.first.owner == Owner::diagnostics && resource.created)
            {
                expect(resource.first.generation != 0 && resource.first.relatedResource == 0 && resource.ended,
                    "explicit diagnostic worker has its captured document generation and actual standalone process terminal");
                ++diagnosticProcesses;
            }
        }
        if (scenario == "packed-close") expect(packed == 1, "one actual packed operation reaches native close with complete lifetime evidence");
        if (scenario == "recovery-save") expect(!recoveryOwners.empty() && sampledRecoveryWorkers != 0,
            "existing native edit/protect/Save exercised real owner-bound writer storage and worker lifetimes");
        if (scenario == "diagnostics-device") expect(deviceStates != 0 && warmups != 0,
            "focused device workflow exercised actual UUID-bound coordinator State and joined warmup worker");
        if (scenario == "diagnostics-device" || scenario == "diagnostics-probe-real") expect(diagnosticProcesses != 0,
            "existing active diagnostic admission reaches the observed real child-process resource bundle");
    }
    juce::File artifacts;
    juce::String scenario;
    OptionalLifecycleObserver& observer;
    ScopedOptionalLifecycleContext armed;
    juce::Array<juce::var> checks;
    std::vector<media::OptionalMediaOperation::DiagnosticHandle> mediaHandles;
    std::map<std::uint64_t, RecoveryOwner> recoveryOwners;
    std::map<juce::String, DeviceRequestRecord> deviceRequests;
};

std::shared_ptr<BackendLifecycleTestSession> createBackendLifecycleTests(const juce::File& artifacts, juce::String scenario)
{ return std::make_shared<BackendLifecycleTestSession>(artifacts, std::move(scenario)); }
void sampleBackendLifecycleTests(BackendLifecycleTestSession& session, MainComponent& main) { session.sample(main); }
OptionalLifecycleContext backendLifecycleContext(BackendLifecycleTestSession& session, MainComponent& main)
{ return session.context(main); }
void finishBackendLifecycleTests(BackendLifecycleTestSession& session, bool completed) { session.finish(completed); }
}
