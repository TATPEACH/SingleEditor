#include "Audio/DeviceSwitchCoordinator.h"
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include <algorithm>
#endif

namespace singleeditor
{
namespace
{
bool validSelection(const DeviceSwitchCoordinator::Selection& selection)
{
    return selection.generation != 0 && selection.plan
        && std::isfinite(selection.expected.sampleRate) && selection.expected.sampleRate > 0
        && selection.expected.bufferSamples > 0
        && selection.plan->matches(selection.expected, selection.bus, selection.roles);
}
bool sameSelection(const DeviceSwitchCoordinator::Selection& left, const DeviceSwitchCoordinator::Selection& right)
{
    return left.generation == right.generation && left.plan == right.plan && left.bus == right.bus && left.roles == right.roles
        && left.expected.backend == right.expected.backend && left.expected.stableId == right.expected.stableId
        && left.expected.sampleRate == right.expected.sampleRate && left.expected.bufferSamples == right.expected.bufferSamples
        && left.expected.outputNames == right.expected.outputNames && left.expected.physicalOutputs == right.expected.physicalOutputs;
}
bool exactCandidate(const DeviceSwitchCoordinator::Selection& selection, const DeviceOutput& output)
{
    const auto& actual = output.snapshot();
    return output.isOpen() && selection.plan->matches(actual, selection.bus, selection.roles)
        && actual.sampleRate == selection.expected.sampleRate && actual.bufferSamples == selection.expected.bufferSamples;
}
}

struct DeviceSwitchCoordinator::State final
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedTerminal
    {
        OptionalLifecycleToken token;
        ~ObservedTerminal() { if (token.isOpen()) token.release(); }
    };
    ObservedTerminal observed; // first member: terminal follows every actual State member
#endif
    enum class Command { none, close, committed, returned };
    Selection selection;
    bool inspecting = false;
    bool maintaining = false, showPanel = false, stopOnly = false, requireRebind = false, observedPanel = false;
    std::atomic<bool> awaitingPanel {false};
    std::shared_ptr<const DeviceRoutePlan> replacementPlan;
    Inspect inspect;
    PanelInspect panelInspect;
    std::shared_ptr<const InspectedCapabilities> inspected;
    Mode mode = Mode::exclusive;
    std::atomic<Phase> phase { Phase::preparing };
    // Keep the first reason and its actual phase in one atomic publication.
    std::atomic<std::uint32_t> failure { 0 };
    std::shared_ptr<DeviceRequestState> requests;
    DeviceRequestRecord request;
    std::atomic<bool> cancelled { false }, abandoned { false }, resourcesReleased { false };
    std::atomic<bool> suspended { false }, restorationAttempted { false }, restored { false }, workerBusy { false };
    std::atomic<double> driverStarted { 0.0 };
    std::mutex mutex;
    std::condition_variable changed;
    bool admitted = false, returnPending = false, closeAttempted = false;
    Command command = Command::none;
    std::unique_ptr<AudioEngine::DeviceLease> lease;
    std::unique_ptr<DeviceOutput> output;

    Error error() const noexcept { return static_cast<Error>(failure.load(std::memory_order_acquire) & 255u); }
    void publish(bool newFailure = false) const
    {
        if (!requests) return;
        requests->publish(request.operationId, [&]
        {
            auto value = request;
            value.switchPhase = phase.load(std::memory_order_acquire);
            const auto reason = failure.load(std::memory_order_acquire);
            value.switchError = static_cast<Error>(reason & 255u);
            value.failurePhase = static_cast<Phase>(reason >> 8u);
            value.restorationAttempted = restorationAttempted.load();
            value.restored = restored.load();
            value.resourcesReleased = resourcesReleased.load(std::memory_order_acquire);
            value.awaitingPanel = awaitingPanel.load();
            value.outcome = value.switchError == Error::cancelled ? DeviceRequestOutcome::cancelled
                : value.switchError == Error::superseded ? DeviceRequestOutcome::superseded
                : value.switchError != Error::none ? DeviceRequestOutcome::rejected
                : value.switchPhase == Phase::active ? DeviceRequestOutcome::succeeded
                : value.switchPhase == Phase::readyForReview ? DeviceRequestOutcome::reviewed : DeviceRequestOutcome::pending;
            return value;
        }, newFailure);
    }
    void fail(Error reason) noexcept
    {
        std::uint32_t expected = 0;
        const auto observed = (static_cast<std::uint32_t>(phase.load()) << 8u) | static_cast<std::uint32_t>(reason);
        if (failure.compare_exchange_strong(expected, observed, std::memory_order_acq_rel))
            publish(reason != Error::cancelled && reason != Error::superseded);
    }
    void beginDriver(Phase next) noexcept
    {
        phase.store(next, std::memory_order_release);
        driverStarted.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_release);
        workerBusy.store(true, std::memory_order_release);
        publish();
    }
    bool closeOld()
    {
        closeAttempted = true;
        beginDriver(Phase::switching);
        auto closed = lease->closeForSwitch();
        if (closed && !maintaining) closed = lease->releaseForReplacement();
        workerBusy.store(false, std::memory_order_release);
        if (!closed) fail(Error::closeFailed);
        return closed;
    }
};

DeviceSwitchCoordinator::DeviceSwitchCoordinator(std::shared_ptr<State> value) : state(std::move(value)) {}
DeviceSwitchCoordinator::~DeviceSwitchCoordinator() { abandon(); }

std::shared_ptr<DeviceSwitchCoordinator> DeviceSwitchCoordinator::start(AudioEngine& engine, Selection selection, Mode mode, Prepare prepare)
{
    return startOperation(engine, std::move(selection), mode, std::move(prepare), {});
}

std::shared_ptr<DeviceSwitchCoordinator> DeviceSwitchCoordinator::startInspection(AudioEngine& engine, InspectionSelection selection, Inspect inspect)
{
    Selection carrier;
    carrier.generation = selection.generation;
    carrier.expected.backend = std::move(selection.backend);
    carrier.expected.stableId = std::move(selection.stableId);
    return startOperation(engine, std::move(carrier), Mode::exclusive, {}, std::move(inspect));
}

std::shared_ptr<DeviceSwitchCoordinator> DeviceSwitchCoordinator::startOperation(AudioEngine& engine, Selection selection,
    Mode mode, Prepare prepare, Inspect inspect, bool maintaining, bool showPanel, bool stopOnly, bool requireRebind, PanelInspect panelInspect)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Selection.generation identifies device selection, not the document.
    auto stateRequest = OptionalLifecycleToken::request(engine.optionalLifecycleContext,
        OptionalLifecycleOwner::audioOutput, OptionalLifecycleResourceKind::deviceOperationState);
    std::shared_ptr<State> shared;
    try { shared = std::make_shared<State>(); }
    catch (...) { stateRequest.failed(); throw; }
    shared->observed.token = std::move(stateRequest);
    shared->observed.token.created();
#else
    auto shared = std::make_shared<State>();
#endif
    shared->selection = std::move(selection); shared->mode = mode;
    shared->inspecting = static_cast<bool>(inspect) || static_cast<bool>(panelInspect);
    shared->inspect = std::move(inspect); shared->panelInspect = std::move(panelInspect);
    shared->maintaining = maintaining; shared->showPanel = showPanel;
    shared->stopOnly = stopOnly;
    shared->requireRebind = requireRebind;
    shared->requests = engine.ensureDeviceRequestState();
    auto& request = shared->request;
    request.operationId = juce::Uuid(); request.direct = true;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::array<std::uint8_t, 16> observedOperation {};
    std::copy_n(request.operationId.getRawData(), observedOperation.size(), observedOperation.begin());
    shared->observed.token.bindOperation(observedOperation);
#endif
    request.kind = shared->inspecting ? (shared->panelInspect ? DeviceRequestKind::inspectPanel : DeviceRequestKind::inspectOutput)
        : maintaining ? (stopOnly ? DeviceRequestKind::suspendOutput : showPanel ? DeviceRequestKind::showOutputPanel : DeviceRequestKind::maintainOutput)
        : DeviceRequestKind::activateOutput;
    request.stage = request.kind;
    request.outcome = DeviceRequestOutcome::pending; request.resourcesReleased = false;
    request.requestedBackend = juce::String(shared->selection.expected.backend).substring(0, 128);
    request.requestedOutputIdentity = juce::String(shared->selection.expected.stableId);
    request.defaultOutputChannels = false;
    if (!shared->inspecting && !maintaining)
    {
        request.requestedRate = shared->selection.expected.sampleRate;
        request.requestedBuffer = shared->selection.expected.bufferSamples;
        request.requestedOutputChannels = static_cast<int>(shared->selection.expected.physicalOutputs.size());
    }
    shared->requests->begin(request);
    auto owner = std::shared_ptr<DeviceSwitchCoordinator>(new DeviceSwitchCoordinator(shared));
    const auto validInspection = shared->inspecting && shared->selection.generation != 0
        && (shared->selection.expected.backend == "ASIO" || isWindowsOutputBackend(shared->selection.expected.backend))
        && !shared->selection.expected.stableId.empty();
    if (shared->inspecting ? !validInspection : ((!prepare && !maintaining) || !validSelection(shared->selection)))
    {
        shared->fail(Error::invalidSelection); shared->phase.store(Phase::failed); shared->resourcesReleased.store(true); shared->publish();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        shared->observed.token.note(OptionalLifecycleAction::taskRejected);
#endif
        return owner;
    }
    if (const auto previous = engine.deviceSwitchOperation.lock(); previous && !previous->snapshot().resourcesReleased)
    {
        shared->fail(Error::engineBusy); shared->phase.store(Phase::failed); shared->resourcesReleased.store(true); shared->publish();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        shared->observed.token.note(OptionalLifecycleAction::taskRejected);
#endif
        return owner;
    }
    engine.deviceSwitchOperation = owner;
    // Launch before transferring the old owner so launch failure cannot strand it.
    if (!juce::Thread::launch([shared, prepare = std::move(prepare)]() mutable { run(shared, std::move(prepare)); }))
    {
        shared->fail(Error::launchFailed); shared->phase.store(Phase::failed); shared->resourcesReleased.store(true); shared->publish();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        shared->observed.token.note(OptionalLifecycleAction::taskRejected);
#endif
        return owner;
    }
    auto lease = mode == Mode::exclusive ? engine.takeDeviceLeaseForSwitch() : nullptr;
    {
        const std::lock_guard lock(shared->mutex);
        if (mode == Mode::exclusive && !lease) { shared->fail(Error::engineBusy); shared->cancelled.store(true); }
        shared->suspended.store(lease != nullptr);
        shared->lease = std::move(lease); shared->admitted = true;
    }
    shared->changed.notify_one();
    return owner;
}

std::shared_ptr<DeviceSwitchCoordinator> DeviceSwitchCoordinator::startMaintenance(AudioEngine& engine, Selection selection, bool panel, bool stopOnly, bool requireRebind)
{
    return startOperation(engine, std::move(selection), Mode::exclusive, {}, {}, true, panel, stopOnly, requireRebind);
}

std::shared_ptr<DeviceSwitchCoordinator> DeviceSwitchCoordinator::startPanelInspection(AudioEngine& engine, InspectionSelection selection, PanelInspect panel)
{
    Selection carrier;
    carrier.generation = selection.generation;
    carrier.expected.backend = std::move(selection.backend); carrier.expected.stableId = std::move(selection.stableId);
    if (carrier.expected.backend != "ASIO") panel = {};
    return startOperation(engine, std::move(carrier), Mode::exclusive, {}, {}, false, false, false, false, std::move(panel));
}

void DeviceSwitchCoordinator::run(const std::shared_ptr<State>& shared, Prepare prepare)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext observedContext(shared->observed.token.creationContext());
    shared->observed.token.note(OptionalLifecycleAction::taskStarted);
#endif
    struct Finished
    {
        State& state;
        Prepare& prepare;
        ~Finished()
        {
            state.output.reset(); state.lease.reset(); prepare = {}; state.inspect = {}; state.panelInspect = {};
            state.workerBusy.store(false, std::memory_order_release);
            state.resourcesReleased.store(true, std::memory_order_release);
            state.publish(state.restorationAttempted.load() && !state.restored.load()
                && (state.error() == Error::cancelled || state.error() == Error::superseded));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            // This is the real resource drain, not a claim that the detached
            // worker or its shared State has already been destroyed.
            state.observed.token.note(OptionalLifecycleAction::taskCompleted);
#endif
        }
    } finished { *shared, prepare };
    {
        std::unique_lock lock(shared->mutex);
        shared->changed.wait(lock, [&] { return shared->admitted || shared->abandoned.load(); });
    }
    auto canPrepare = !shared->cancelled.load() && !shared->abandoned.load();
    if (canPrepare && shared->mode == Mode::exclusive) canPrepare = shared->lease && shared->closeOld();
    if (canPrepare && !shared->cancelled.load() && !shared->abandoned.load())
    {
        shared->beginDriver(shared->mode == Mode::exclusive ? Phase::switching : Phase::preparing);
        try
        {
            const auto access = shared->lease ? shared->lease->exclusiveAccess() : DeviceSwitchAccess {};
            if (shared->maintaining)
            {
                // Reuse the exact closed driver owner; constructing a second
                // instance of an exclusive ASIO driver is not a reset.
                shared->output = std::move(shared->lease->output);
                const auto& choice = shared->selection;
                if (!shared->output || !shared->output->hasManagedEvents()
                    || !choice.plan->matches(shared->output->snapshot(), choice.bus, choice.roles)
                    || shared->output->snapshot().sampleRate != choice.expected.sampleRate
                    || shared->output->snapshot().bufferSamples != choice.expected.bufferSamples)
                    shared->fail(Error::staleCandidate);
                else if (shared->stopOnly) shared->fail(Error::circuitOpen);
                else
                {
                    const auto result = shared->output->maintain(shared->showPanel, shared->cancelled, shared->awaitingPanel);
                    shared->driverStarted.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_release);
                    if (result == DeviceOutput::Maintenance::rebindRequired || shared->requireRebind) shared->fail(Error::rebindRequired);
                    else if (result == DeviceOutput::Maintenance::cancelled) shared->fail(Error::cancelled);
                    else if (result != DeviceOutput::Maintenance::ready || !shared->output->isOpen()) shared->fail(Error::prepareFailed);
                    else
                    {
                        shared->replacementPlan = choice.plan->withActualFormat(shared->output->snapshot(), choice.bus, choice.roles);
                        if (!shared->replacementPlan) shared->fail(Error::rebindRequired);
                    }
                }
            }
            else if (shared->inspecting)
            {
                const InspectionSelection choice {shared->selection.generation, shared->selection.expected.backend, shared->selection.expected.stableId};
                auto result = shared->panelInspect ? shared->panelInspect(choice, access, shared->cancelled, shared->awaitingPanel)
                    : shared->inspect(choice, access, shared->cancelled);
                shared->inspected = std::move(result.capabilities);
                if (result.error != Error::none) shared->fail(result.error);
                else if (!shared->inspected) shared->fail(Error::prepareFailed);
                else if (shared->inspected->stableId != shared->selection.expected.stableId) shared->fail(Error::staleCandidate);
            }
            else
            {
                auto result = prepare(shared->selection, access, shared->cancelled);
                shared->output = std::move(result.output);
                if (result.error != Error::none) shared->fail(result.error);
                else if (!shared->output) shared->fail(Error::prepareFailed);
                else if (!exactCandidate(shared->selection, *shared->output)) shared->fail(Error::staleCandidate);
            }
        }
        catch (...) { shared->fail(Error::prepareFailed); }
        shared->workerBusy.store(false, std::memory_order_release);
    }
    if (shared->error() == Error::none && shared->output && !shared->cancelled.load() && !shared->abandoned.load())
    {
        std::unique_lock lock(shared->mutex);
        shared->phase.store(Phase::readyToSwitch, std::memory_order_release);
        shared->publish();
        for (;;)
        {
            shared->changed.wait(lock, [&] { return shared->command != State::Command::none || shared->cancelled.load() || shared->abandoned.load(); });
            const auto command = std::exchange(shared->command, State::Command::none);
            if (command == State::Command::committed)
            {
                shared->phase.store(Phase::active, std::memory_order_release);
                return;
            }
            if (shared->cancelled.load() || shared->abandoned.load()) break;
            if (command == State::Command::close)
            {
                lock.unlock();
                const auto closed = shared->closeOld();
                lock.lock();
                if (!closed) break;
                shared->phase.store(Phase::readyToSwitch, std::memory_order_release);
                shared->publish();
            }
        }
    }
    // Dispose the rejected candidate before reopening the last known good owner.
    // Cancellation cannot skip this ordering, even if a driver returned late.
    shared->beginDriver(Phase::restoring);
    if (shared->maintaining && shared->output && shared->lease)
    {
        shared->output->rollbackMaintenance();
        shared->lease->output = std::move(shared->output);
    }
    else shared->output.reset();
    prepare = {}; shared->inspect = {}; shared->panelInspect = {};
    shared->workerBusy.store(false, std::memory_order_release);
    if (shared->lease && !shared->abandoned.load())
    {
        if (!shared->closeAttempted) shared->closeOld();
        shared->beginDriver(Phase::restoring);
        shared->restorationAttempted.store(true);
        const auto restored = shared->lease->restore(!shared->requireRebind
            && shared->error() != Error::rebindRequired && shared->error() != Error::circuitOpen);
        shared->restored.store(restored, std::memory_order_release);
        shared->workerBusy.store(false, std::memory_order_release);
        if (!restored) shared->fail(Error::restoreFailed);
        std::unique_lock lock(shared->mutex);
        shared->returnPending = true;
        shared->changed.wait(lock, [&] { return shared->command == State::Command::returned || shared->abandoned.load(); });
    }
    const auto canReview = shared->inspecting && shared->inspected && shared->error() == Error::none
        && shared->restored.load() && !shared->cancelled.load() && !shared->abandoned.load();
    shared->phase.store(canReview ? Phase::readyForReview : Phase::failed, std::memory_order_release);
}

void DeviceSwitchCoordinator::pollInspection(AudioEngine& engine, const InspectionSelection& current)
{
    if (!state->inspecting) return;
    Selection carrier;
    carrier.generation = current.generation;
    carrier.expected.backend = current.backend; carrier.expected.stableId = current.stableId;
    poll(engine, carrier);
}

std::shared_ptr<const DeviceSwitchCoordinator::InspectedCapabilities> DeviceSwitchCoordinator::inspection(const InspectionSelection& current) const
{
    if (!state->inspecting || !state->resourcesReleased.load(std::memory_order_acquire)
        || state->phase.load(std::memory_order_acquire) != Phase::readyForReview || state->error() != Error::none
        || state->abandoned.load() || state->selection.generation != current.generation
        || state->selection.expected.backend != current.backend || state->selection.expected.stableId != current.stableId) return {};
    return state->inspected;
}

void DeviceSwitchCoordinator::poll(AudioEngine& engine, const Selection& current)
{
    if (engine.deviceSwitchOperation.lock().get() != this) return;
    state->publish();
    const auto phase = state->phase.load(std::memory_order_acquire);
    if (phase == Phase::active || phase == Phase::failed || phase == Phase::readyForReview || state->abandoned.load()) return;
    const std::lock_guard lock(state->mutex);
    const auto panelOpen = state->awaitingPanel.load();
    if (panelOpen || state->observedPanel) state->driverStarted.store(juce::Time::getMillisecondCounterHiRes());
    state->observedPanel = panelOpen;
    if (!sameSelection(state->selection, current)) { state->fail(Error::superseded); state->cancelled.store(true); state->changed.notify_one(); }
    if (snapshot().driverUnresolved) { state->fail(Error::driverUnresolved); state->cancelled.store(true); state->changed.notify_one(); }
    if (state->returnPending)
    {
        const auto hadOutput = state->lease && state->lease->originallyOpen();
        if (engine.returnDeviceLease(state->lease))
        {
            if (hadOutput && engine.deviceRestoreUnavailable.load(std::memory_order_acquire))
            {
                state->restored.store(false, std::memory_order_release);
                state->fail(Error::restoreFailed);
            }
            state->returnPending = false; state->suspended.store(false);
            state->command = State::Command::returned; state->changed.notify_one();
        }
        return;
    }
    if (state->phase.load() != Phase::readyToSwitch || state->cancelled.load()) return;
    if (!state->lease)
    {
        state->lease = engine.takeDeviceLeaseForSwitch();
        if (!state->lease) { state->fail(Error::engineBusy); state->cancelled.store(true); }
        else { state->suspended.store(true); state->phase.store(Phase::switching); state->command = State::Command::close; }
        state->changed.notify_one();
        return;
    }
    if (engine.commitDeviceLease(state->lease, state->output,
        state->maintaining ? state->replacementPlan : state->selection.plan, current.bus, current.roles))
    {
        state->suspended.store(false); state->phase.store(Phase::active, std::memory_order_release);
        state->command = State::Command::committed;
    }
    else { state->fail(Error::commitFailed); state->cancelled.store(true); }
    state->changed.notify_one();
}

void DeviceSwitchCoordinator::cancel() noexcept
{
    const std::lock_guard lock(state->mutex);
    if (state->phase.load() == Phase::active || state->phase.load() == Phase::failed || state->phase.load() == Phase::readyForReview) return;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    state->observed.token.note(OptionalLifecycleAction::cancelRequested);
#endif
    state->fail(Error::cancelled); state->cancelled.store(true); state->changed.notify_one();
}
void DeviceSwitchCoordinator::abandon() noexcept
{
    const std::lock_guard lock(state->mutex);
    if (state->phase.load() != Phase::active && state->phase.load() != Phase::failed && state->phase.load() != Phase::readyForReview) state->fail(Error::cancelled);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (!state->resourcesReleased.load(std::memory_order_acquire))
        state->observed.token.note(OptionalLifecycleAction::cancelRequested);
#endif
    state->abandoned.store(true); state->cancelled.store(true); state->changed.notify_one();
}
DeviceSwitchCoordinator::Snapshot DeviceSwitchCoordinator::snapshot() const
{
    return { state->phase.load(std::memory_order_acquire), state->error(), state->selection.generation,
        state->selection.expected.backend, state->selection.expected.stableId, state->suspended.load(),
        state->restorationAttempted.load(), state->restored.load(),
        state->workerBusy.load(std::memory_order_acquire) && !state->awaitingPanel.load()
            && juce::Time::getMillisecondCounterHiRes() - state->driverStarted.load() > 5000.0,
        state->resourcesReleased.load(std::memory_order_acquire), state->awaitingPanel.load() };
}
}
