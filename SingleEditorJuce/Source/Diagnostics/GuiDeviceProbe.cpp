#include "GuiDeviceProbe.h"
#include <condition_variable>
#include <mutex>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor::diagnostics
{
struct GuiDeviceProbe::State final
{
    std::atomic<Phase> phase { Phase::preparing };
    std::atomic<bool> cancelled { false }, abandoned { false };
    std::atomic<bool> resourcesReleased { false };
    std::atomic<double> driverStarted { 0.0 };
    std::mutex mutex;
    std::condition_variable changed;
    bool admitted = false, returned = false;
    std::unique_ptr<AudioEngine::DeviceLease> lease;
    DeviceProbeResult result;
};
GuiDeviceProbe::GuiDeviceProbe(std::shared_ptr<State> value) : state(std::move(value)) {}
GuiDeviceProbe::~GuiDeviceProbe() { abandon(); }
std::shared_ptr<GuiDeviceProbe> GuiDeviceProbe::start(AudioEngine& engine,
    const AudioEngine::DeviceProbeConfiguration& expected, const DeviceProbeRequest& request, const juce::File& worker)
{
    if (!DeviceDiagnosticProbe::valid(request) || request.backend != expected.backend
        || request.outputDevice != expected.setup.outputDeviceName || request.expectedOutputEndpoint != expected.endpoint) return {};
    auto shared = std::make_shared<State>();
    auto owner = std::shared_ptr<GuiDeviceProbe>(new GuiDeviceProbe(shared));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const auto observedContext = currentOptionalLifecycleContext();
#endif
    // Start the worker before taking the manager. A thread-launch failure must
    // never strand a transferred device or make driver calls on the UI thread.
    if (!juce::Thread::launch([shared, request, worker, operation = juce::Uuid()
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , observedContext
#endif
    ]
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const ScopedOptionalLifecycleContext operationContext(observedContext);
#endif
        struct Finished final
        {
            State& state;
            ~Finished() { state.resourcesReleased.store(true, std::memory_order_release); }
        } finished { *shared };
        {
            std::unique_lock lock(shared->mutex);
            shared->changed.wait(lock, [&] { return shared->admitted || shared->abandoned.load(); });
            if (!shared->lease) return;
        }
        auto& lease = shared->lease;
        shared->driverStarted.store(juce::Time::getMillisecondCounterHiRes());
        shared->phase.store(Phase::closing, std::memory_order_release);
        const auto closed = lease->closeForProbe();
        shared->result.evidence.operationIds.push_back(operation);
        if (closed && !shared->cancelled.load() && lease->ownerAlive())
        {
            shared->phase.store(Phase::probing, std::memory_order_release);
            try { shared->result = DeviceDiagnosticProbe::run(worker, request, operation, shared->cancelled); }
            catch (...) { shared->result.evidence.latestFailure = FailureCapsule { operation, FailureStage::negotiation, -1, {} }; }
        }
        else
        {
            shared->result.cancelled = shared->cancelled.load();
            shared->result.evidence.fields.push_back({ Field::negotiationOutcome, shared->result.cancelled ? "cancelled" : "failed" });
            if (!shared->result.cancelled)
                shared->result.evidence.latestFailure = FailureCapsule { operation, FailureStage::negotiation, -2, {} };
        }
        shared->driverStarted.store(juce::Time::getMillisecondCounterHiRes());
        shared->phase.store(Phase::restoring, std::memory_order_release);
        const auto restored = !shared->abandoned.load() && lease->restore(shared->result.workerReleased);
        for (auto& item : shared->result.evidence.fields)
            if (item.field == Field::negotiationRequestKind) item.value = "gui-active-probe";
        if (std::none_of(shared->result.evidence.fields.begin(), shared->result.evidence.fields.end(),
            [](const auto& item) { return item.field == Field::negotiationRequestKind; }))
            shared->result.evidence.fields.push_back({ Field::negotiationRequestKind, "gui-active-probe" });
        shared->result.evidence.fields.push_back({ Field::deviceProbeRestore, restored ? "exact-device-restored"
            : !shared->result.workerReleased ? "restore-deferred-worker-unresolved" : "restore-failed-device-unavailable" });
        if (!restored)
        {
            shared->result.succeeded = false;
            shared->result.evidence.latestFailure = FailureCapsule { operation, FailureStage::cleanup, -3, {} };
        }
        {
            std::unique_lock lock(shared->mutex);
            shared->phase.store(Phase::awaitingReturn, std::memory_order_release);
            shared->changed.wait(lock, [&] { return shared->returned || shared->abandoned.load(); });
        }
        // Any leftover manager belongs to an abandoned owner and is disposed
        // here, never by the UI or a dead AudioEngine. No callback references it.
        lease.reset();
    })) return {};
    auto lease = engine.takeDeviceLeaseForProbe(&expected);
    if (!lease) { owner->abandon(); return {}; }
    {
        const std::lock_guard lock(shared->mutex);
        shared->lease = std::move(lease); shared->admitted = true;
    }
    shared->changed.notify_one();
    return owner;
}
void GuiDeviceProbe::cancel() noexcept { state->cancelled.store(true); }
void GuiDeviceProbe::abandon() noexcept
{
    const std::lock_guard lock(state->mutex);
    state->cancelled.store(true); state->abandoned.store(true); state->changed.notify_one();
}
void GuiDeviceProbe::pollReturn(AudioEngine& engine)
{
    if (phase() != Phase::awaitingReturn || state->abandoned.load()) return;
    const std::lock_guard lock(state->mutex);
    if (engine.returnDeviceLease(state->lease))
    {
        state->returned = true;
        state->phase.store(Phase::finished, std::memory_order_release);
        state->changed.notify_one();
    }
}
GuiDeviceProbe::Phase GuiDeviceProbe::phase() const noexcept { return state->phase.load(std::memory_order_acquire); }
bool GuiDeviceProbe::cancellationRequested() const noexcept { return state->cancelled.load(); }
bool GuiDeviceProbe::resourcesReleased() const noexcept { return state->resourcesReleased.load(std::memory_order_acquire); }
bool GuiDeviceProbe::driverUnresolved() const noexcept
{
    const auto current = phase();
    return (current == Phase::closing || current == Phase::restoring)
        && juce::Time::getMillisecondCounterHiRes() - state->driverStarted.load() > 5000.0;
}
const DeviceProbeResult* GuiDeviceProbe::result() const noexcept
{ return phase() == Phase::finished ? &state->result : nullptr; }
}
