#pragma once

// This entire channel is absent from ordinary no-timing Release targets.
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace singleeditor
{
enum class OptionalLifecycleOwner : std::uint8_t
{
    workspace, outputMeter, ambisonics, parametricEq, truePeakLimiter,
    workspaceView, metadata, markers, spatialEnergy, coreFile, referenceGain,
    optionalMedia, vgmstream, diagnostics, recovery, audioOutput
};
constexpr const char* optionalLifecycleOwnerId(OptionalLifecycleOwner owner) noexcept
{
    switch (owner)
    {
        case OptionalLifecycleOwner::workspace: return "singleeditor.workspace";
        case OptionalLifecycleOwner::outputMeter: return "singleeditor.output-meter";
        case OptionalLifecycleOwner::ambisonics: return "singleeditor.ambisonics";
        case OptionalLifecycleOwner::parametricEq: return "singleeditor.parametric-eq-v1";
        case OptionalLifecycleOwner::truePeakLimiter: return "singleeditor.true-peak-limiter-v1";
        case OptionalLifecycleOwner::workspaceView: return "singleeditor.workspace-view";
        case OptionalLifecycleOwner::metadata: return "singleeditor.metadata";
        case OptionalLifecycleOwner::markers: return "singleeditor.timeline-markers";
        case OptionalLifecycleOwner::spatialEnergy: return "singleeditor.spatial-energy";
        case OptionalLifecycleOwner::coreFile: return "singleeditor.core-file";
        case OptionalLifecycleOwner::referenceGain: return "singleeditor.reference-gain";
        case OptionalLifecycleOwner::optionalMedia: return "singleeditor.optional-media";
        case OptionalLifecycleOwner::vgmstream: return "singleeditor.vgmstream";
        case OptionalLifecycleOwner::diagnostics: return "singleeditor.diagnostics";
        case OptionalLifecycleOwner::recovery: return "singleeditor.recovery";
        case OptionalLifecycleOwner::audioOutput: return "singleeditor.audio-output";
    }
    return "invalid-owner";
}
enum class OptionalLifecycleResourceKind : std::uint8_t
{
    ownedView, borrowedView, processor, previewPublication, meterConsumer, meterState, marker, workspaceHost,
    renderSession, previewState, applyTask, completionCallback,
    ambisonicFieldConsumer, ambisonicFieldAnalyzer, ambisonicPlaybackConsumer, ambisonicStageGraph,
    spatialConsumer, spatialWorker, spatialState, spatialVisualWorker, viewTimer,
    optionalMediaOperation, packedDecodeWork, boundedDecodeProcess,
    recoveryWriterState, recoveryWriterWorker,
    deviceOperationState, deviceWarmupCallback, deviceWarmupWorker
};
enum class OptionalLifecycleAction : std::uint8_t
{
    factoryRequested, created, factoryFailed, activated, quiesced, released,
    borrowedAttached, borrowedDetached, published, withdrawn,
    generationBegan, firstWaveform, playable, coreReady, missingTerminal,
    moduleEnableRequested, moduleOffRequested, moduleRemoveRequested,
    previewRequested, previewOffRequested,
    taskStarted, cancelRequested, taskCompleted,
    callbackPostRequested, callbackInvoked, callbackDiscarded,
    taskSucceeded, taskCancelled, taskRejected, operationBound
};
enum class OptionalLifecycleFailure : std::uint32_t
{
    overflow = 1, identityExhausted = 2, invalidContext = 4,
    invalidTransition = 8, missingTerminal = 16
};
class OptionalLifecycleObserver;
struct OptionalLifecycleContext
{
    OptionalLifecycleObserver* observer = nullptr; // non-owning; outlives all tokens
    std::uint64_t session = 0, generation = 0;
};
struct OptionalLifecycleEvent
{
    std::uint64_t sequence = 0, resourceId = 0, session = 0, generation = 0, relatedResource = 0;
    OptionalLifecycleOwner owner = OptionalLifecycleOwner::workspace;
    OptionalLifecycleResourceKind kind = OptionalLifecycleResourceKind::ownedView;
    OptionalLifecycleAction action = OptionalLifecycleAction::factoryRequested;
    std::array<std::uint8_t, 16> operationId {};
};
struct OptionalLifecycleEventSlot
{
    OptionalLifecycleEvent event;
    std::atomic<std::uint64_t> committedSequence { 0 };
};

// The fixture owns fresh storage and the observer until every producer has
// stopped. Each slot has one writer. Read event only after acquire-loading its
// nonzero committedSequence; inspect the complete stream after producers join.
// Emission does not allocate, own product objects, call a clock, or write files.
class OptionalLifecycleObserver final
{
public:
    explicit OptionalLifecycleObserver(std::span<OptionalLifecycleEventSlot> storage) noexcept : slots(storage)
    {
        for (auto& slot : slots) slot.committedSequence.store(0, std::memory_order_relaxed);
    }
    OptionalLifecycleObserver(const OptionalLifecycleObserver&) = delete;
    OptionalLifecycleObserver& operator=(const OptionalLifecycleObserver&) = delete;
    [[nodiscard]] std::uint32_t failureFlags() const noexcept { return failures.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t attemptedEventCount() const noexcept { return nextSequence.load(std::memory_order_acquire) - 1; }
    [[nodiscard]] std::uint64_t outstandingResourceCount() const noexcept { return outstanding.load(std::memory_order_acquire); }
    [[nodiscard]] std::span<const OptionalLifecycleEventSlot> events() const noexcept { return slots; }
    void fail(OptionalLifecycleFailure reason) noexcept { failures.fetch_or(static_cast<std::uint32_t>(reason), std::memory_order_relaxed); }
    void milestone(OptionalLifecycleContext context, OptionalLifecycleAction action) noexcept
    {
        if (action != OptionalLifecycleAction::generationBegan && action != OptionalLifecycleAction::firstWaveform
            && action != OptionalLifecycleAction::playable && action != OptionalLifecycleAction::coreReady)
        { fail(OptionalLifecycleFailure::invalidTransition); return; }
        record(context, 0, OptionalLifecycleOwner::coreFile, OptionalLifecycleResourceKind::marker, action, 0);
    }
    void command(OptionalLifecycleContext context, OptionalLifecycleOwner owner, OptionalLifecycleAction action) noexcept
    {
        if (action != OptionalLifecycleAction::moduleEnableRequested && action != OptionalLifecycleAction::moduleOffRequested
            && action != OptionalLifecycleAction::moduleRemoveRequested && action != OptionalLifecycleAction::previewRequested
            && action != OptionalLifecycleAction::previewOffRequested)
        { fail(OptionalLifecycleFailure::invalidTransition); return; }
        record(context, 0, owner, OptionalLifecycleResourceKind::marker, action, 0);
    }

private:
    friend class OptionalLifecycleToken;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::uint64_t allocate(std::atomic<std::uint64_t>& counter) noexcept
    {
        auto value = counter.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<std::uint64_t>::max())
            if (counter.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
        fail(OptionalLifecycleFailure::identityExhausted); return 0;
    }
    void record(OptionalLifecycleContext context, std::uint64_t id, OptionalLifecycleOwner owner,
                OptionalLifecycleResourceKind kind, OptionalLifecycleAction action, std::uint64_t related,
                const std::array<std::uint8_t, 16>& operation = {}) noexcept
    {
        if (context.observer != this || context.session == 0)
        { fail(OptionalLifecycleFailure::invalidContext); return; }
        const auto sequence = allocate(nextSequence);
        if (sequence == 0) return;
        if (sequence > slots.size()) { fail(OptionalLifecycleFailure::overflow); return; }
        auto& slot = slots[static_cast<std::size_t>(sequence - 1)];
        slot.event = { sequence, id, context.session, context.generation, related, owner, kind, action, operation };
        slot.committedSequence.store(sequence, std::memory_order_release);
    }
    std::span<OptionalLifecycleEventSlot> slots;
    std::atomic<std::uint64_t> nextSequence { 1 }, nextResource { 1 }, outstanding { 0 };
    std::atomic<std::uint32_t> failures { 0 };
};

// Records an observed lifetime, never controls it. Explicit terminal is emitted
// after the real release. Destruction with no terminal is a harness failure.
class OptionalLifecycleToken final
{
public:
    OptionalLifecycleToken() noexcept = default;
    OptionalLifecycleToken(const OptionalLifecycleToken&) = delete;
    OptionalLifecycleToken& operator=(const OptionalLifecycleToken&) = delete;
    OptionalLifecycleToken(OptionalLifecycleToken&& other) noexcept { take(other); }
    OptionalLifecycleToken& operator=(OptionalLifecycleToken&& other) noexcept
    {
        if (this != &other) { reportMissingTerminal(); take(other); }
        return *this;
    }
    ~OptionalLifecycleToken() { reportMissingTerminal(); }
    static OptionalLifecycleToken request(OptionalLifecycleContext context, OptionalLifecycleOwner owner,
        OptionalLifecycleResourceKind kind, std::uint64_t relatedResource = 0) noexcept
    { return begin(context, owner, kind, OptionalLifecycleAction::factoryRequested, State::requested, relatedResource); }
    static OptionalLifecycleToken attachBorrowed(OptionalLifecycleContext context, OptionalLifecycleOwner owner) noexcept
    { return begin(context, owner, OptionalLifecycleResourceKind::borrowedView, OptionalLifecycleAction::borrowedAttached, State::quiescent, 0); }
    static OptionalLifecycleToken publish(OptionalLifecycleContext context, OptionalLifecycleOwner owner,
        std::uint64_t relatedResource = 0) noexcept
    { return begin(context, owner, OptionalLifecycleResourceKind::previewPublication, OptionalLifecycleAction::published, State::quiescent, relatedResource); }
    static OptionalLifecycleToken publish(OptionalLifecycleContext context, OptionalLifecycleOwner owner,
        OptionalLifecycleResourceKind kind, std::uint64_t relatedResource = 0) noexcept
    {
        if (kind == OptionalLifecycleResourceKind::previewPublication) return publish(context, owner, relatedResource);
        if (context.observer) context.observer->fail(OptionalLifecycleFailure::invalidTransition);
        return {};
    }
    [[nodiscard]] std::uint64_t resourceId() const noexcept { return id; }
    [[nodiscard]] OptionalLifecycleResourceKind resourceKind() const noexcept { return kind; }
    [[nodiscard]] OptionalLifecycleOwner resourceOwner() const noexcept { return owner; }
    [[nodiscard]] OptionalLifecycleContext creationContext() const noexcept { return context; }
    [[nodiscard]] bool isOpen() const noexcept { return id != 0 && state != State::terminal; }
    // Correlate the real diagnostic UUID without changing the resource's
    // immutable identity or retaining any diagnostic/product object.
    void bindOperation(const std::array<std::uint8_t, 16>& operation) const noexcept
    {
        if (!id) return;
        bool nonzero = false;
        for (auto byte : operation) nonzero = nonzero || byte != 0;
        if (!nonzero) { context.observer->fail(OptionalLifecycleFailure::invalidContext); return; }
        context.observer->record(context, id, owner, kind, OptionalLifecycleAction::operationBound, related, operation);
    }
    // A worker can annotate an identity captured before it starts. The owning
    // thread still controls transitions and joins before moving/releasing the
    // token. Notes never read or mutate that thread's transition state. The
    // stream consumer checks their causal order against the resource contract.
    void note(OptionalLifecycleAction action) const noexcept
    {
        if (!id) return;
        if (action != OptionalLifecycleAction::taskStarted && action != OptionalLifecycleAction::cancelRequested
            && action != OptionalLifecycleAction::taskCompleted && action != OptionalLifecycleAction::callbackPostRequested
            && action != OptionalLifecycleAction::callbackInvoked && action != OptionalLifecycleAction::callbackDiscarded
            && action != OptionalLifecycleAction::taskSucceeded && action != OptionalLifecycleAction::taskCancelled
            && action != OptionalLifecycleAction::taskRejected)
        { context.observer->fail(OptionalLifecycleFailure::invalidTransition); return; }
        context.observer->record(context, id, owner, kind, action, related);
    }
    void created() noexcept { transition(State::requested, State::quiescent, OptionalLifecycleAction::created); }
    void failed() noexcept { finish(State::requested, OptionalLifecycleAction::factoryFailed); }
    void activate() noexcept
    {
        if (state == State::active) return;
        transition(State::quiescent, State::active, OptionalLifecycleAction::activated);
    }
    void quiesce() noexcept
    {
        if (state == State::quiescent) return;
        transition(State::active, State::quiescent, OptionalLifecycleAction::quiesced);
    }
    void release() noexcept
    {
        if (!id) return;
        if (kind == OptionalLifecycleResourceKind::borrowedView || kind == OptionalLifecycleResourceKind::previewPublication)
        { context.observer->fail(OptionalLifecycleFailure::invalidTransition); return; }
        finish(State::quiescent, OptionalLifecycleAction::released);
    }
    void detachBorrowed() noexcept { finishKind(OptionalLifecycleResourceKind::borrowedView, OptionalLifecycleAction::borrowedDetached); }
    void withdraw() noexcept { finishKind(OptionalLifecycleResourceKind::previewPublication, OptionalLifecycleAction::withdrawn); }

private:
    enum class State : std::uint8_t { empty, requested, quiescent, active, terminal };
    static OptionalLifecycleToken begin(OptionalLifecycleContext context, OptionalLifecycleOwner owner,
        OptionalLifecycleResourceKind kind, OptionalLifecycleAction action, State state, std::uint64_t related) noexcept
    {
        OptionalLifecycleToken token;
        if (!context.observer) return token;
        if (context.session == 0) { context.observer->fail(OptionalLifecycleFailure::invalidContext); return token; }
        token.context = context; token.owner = owner; token.kind = kind; token.related = related;
        token.id = context.observer->allocate(context.observer->nextResource);
        if (token.id)
        {
            token.state = state;
            context.observer->outstanding.fetch_add(1, std::memory_order_relaxed);
            token.emit(action);
        }
        return token;
    }
    void emit(OptionalLifecycleAction action) noexcept { context.observer->record(context, id, owner, kind, action, related); }
    void transition(State expected, State next, OptionalLifecycleAction action) noexcept
    {
        if (!id) return;
        if (state != expected) { context.observer->fail(OptionalLifecycleFailure::invalidTransition); return; }
        emit(action); state = next;
    }
    void finish(State expected, OptionalLifecycleAction action) noexcept
    {
        if (!id) return;
        if (state != expected) { context.observer->fail(OptionalLifecycleFailure::invalidTransition); return; }
        emit(action); state = State::terminal;
        context.observer->outstanding.fetch_sub(1, std::memory_order_relaxed);
    }
    void finishKind(OptionalLifecycleResourceKind expected, OptionalLifecycleAction action) noexcept
    {
        if (!id) return;
        if (kind != expected) { context.observer->fail(OptionalLifecycleFailure::invalidTransition); return; }
        finish(State::quiescent, action);
    }
    void reportMissingTerminal() noexcept
    {
        if (!isOpen()) return;
        context.observer->fail(OptionalLifecycleFailure::missingTerminal);
        emit(OptionalLifecycleAction::missingTerminal);
        // Retain outstanding count: the observer did not witness a terminal.
        state = State::terminal;
    }
    void take(OptionalLifecycleToken& other) noexcept
    {
        context = other.context; owner = other.owner; kind = other.kind;
        id = std::exchange(other.id, 0); related = other.related; state = other.state;
        other.state = State::empty; other.context = {};
    }
    OptionalLifecycleContext context;
    OptionalLifecycleOwner owner = OptionalLifecycleOwner::workspace;
    OptionalLifecycleResourceKind kind = OptionalLifecycleResourceKind::ownedView;
    std::uint64_t id = 0, related = 0;
    State state = State::empty;
};

// Message-thread construction scope only. Worker resources receive an explicit
// context/token; they never consult this ambient construction context.
inline thread_local OptionalLifecycleContext optionalLifecycleConstructionContext;
inline OptionalLifecycleContext currentOptionalLifecycleContext() noexcept
{ return optionalLifecycleConstructionContext; }
class ScopedOptionalLifecycleContext final
{
public:
    explicit ScopedOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
        : previous(optionalLifecycleConstructionContext) { optionalLifecycleConstructionContext = context; }
    ~ScopedOptionalLifecycleContext() { optionalLifecycleConstructionContext = previous; }
    ScopedOptionalLifecycleContext(const ScopedOptionalLifecycleContext&) = delete;
    ScopedOptionalLifecycleContext& operator=(const ScopedOptionalLifecycleContext&) = delete;
private:
    OptionalLifecycleContext previous;
};
} // namespace singleeditor
#endif
