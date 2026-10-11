#include "Audio/SpatialEnergyService.h"

#include <chrono>
#include <condition_variable>
#include <stdexcept>
#include <thread>
#include <utility>

namespace singleeditor
{
struct SpatialEnergyService::Control
{
    std::mutex mutex;
    SpatialEnergyService* owner = nullptr;
};
struct SpatialEnergyService::State
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedLifetime
    {
        explicit ObservedLifetime(OptionalLifecycleToken value) noexcept : token(std::move(value)) {}
        ~ObservedLifetime() { if (complete) token.release(); else token.failed(); }
        OptionalLifecycleToken token;
        bool complete = false;
    } observedLifetime; // destroyed after all actual state/filter members
#endif
    State(Configuration value, std::uint64_t serial, SpatialEnergyMeter::Layout layout
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , OptionalLifecycleToken lifetime
#endif
    )
        :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
          observedLifetime(std::move(lifetime)),
#endif
          configuration(std::move(value)), request(serial), meter(std::move(layout))
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.created(); observedLifetime.complete = true;
#endif
    }
    Configuration configuration; // pins route/geometry, released only by worker
    std::uint64_t request;
    SpatialEnergyMeter meter;
};
class SpatialEnergyService::Worker final
{
public:
    explicit Worker(SpatialEnergyService& value
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , OptionalLifecycleToken lifetime
#endif
    ) :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime(std::move(lifetime)),
#endif
        owner(value), thread([this] { run(); })
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.created(); observedLifetime.complete = true;
#endif
    }
    ~Worker()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.note(OptionalLifecycleAction::cancelRequested);
#endif
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one();
        thread.join();
    }
    void request(Configuration value, std::uint64_t serial)
    {
        { std::lock_guard lock(mutex); pending = std::make_pair(std::move(value), serial);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
          pendingContext = owner.optionalLifecycleContext;
#endif
        }
        wake.notify_one();
    }
    std::shared_ptr<const SpatialEnergyMeter::Snapshot> snapshot() const
    { return published.load(std::memory_order_acquire); }
private:
    void replace(std::unique_ptr<State> next)
    {
        owner.active.store(next.get());
        if (current) retired.push_back(std::move(current));
        current = std::move(next);
        collect();
    }
    void collect()
    {
        // Publication precedes this seq_cst quiescence check. A new reader can
        // only see the new pointer; an old reader prevents retirement.
        if (owner.readers.load() == 0) retired.clear();
        owner.liveMeters.store(static_cast<int>(retired.size()) + (current ? 1 : 0));
    }
    void unavailable(std::uint64_t serial)
    {
        auto empty = std::make_shared<SpatialEnergyMeter::Snapshot>();
        auto layout = std::make_shared<SpatialEnergyMeter::Layout>(); layout->generation = serial;
        empty->layout = std::move(layout);
        empty->transport = SpatialEnergyMeter::Transport::unavailable;
        published.store(std::move(empty), std::memory_order_release);
    }
    void run()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.note(OptionalLifecycleAction::taskStarted);
#endif
        for (;;)
        {
            std::optional<std::pair<Configuration, std::uint64_t>> next;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            OptionalLifecycleContext nextContext;
#endif
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopping || pending.has_value(); });
                if (stopping) break;
                next = std::move(pending); pending.reset();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                nextContext = pendingContext;
#endif
            }
            if (next)
            {
                try
                {
                    auto& config = next->first;
                    if (!config.device || config.device->sampleRate <= 0 || config.device->physicalOutputs.empty())
                    { replace({}); unavailable(next->second); }
                    else
                    {
                        if (config.route && (!config.route->matches(*config.device, config.route->bus(), config.route->trustedOutputRoles())
                            || config.route->device().sampleRate != config.device->sampleRate))
                            throw std::invalid_argument("Spatial Energy route no longer matches the actual output");
                        SpatialEnergyMeter::Layout layout;
                        if (config.route)
                        {
                            if (!config.geometry)
                                config.geometry = AmbisonicStandardMatrixCatalog::standardProfile(juce::String(config.route->bus().profileId));
                            layout = SpatialEnergyLayout::fromRoute(*config.route, config.geometry.get(), next->second);
                        }
                        else
                        {
                            layout.generation = next->second; layout.sampleRate = config.device->sampleRate;
                            for (const auto physical : config.device->physicalOutputs)
                            {
                                SpatialEnergyMeter::Channel channel; channel.physicalOutput = physical;
                                if (physical >= 0 && physical < static_cast<int>(config.device->outputNames.size()))
                                    channel.label = config.device->outputNames[static_cast<std::size_t>(physical)];
                                layout.channels.push_back(std::move(channel));
                            }
                        }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        auto stateLifetime = OptionalLifecycleToken::request(nextContext,
                            OptionalLifecycleOwner::spatialEnergy, OptionalLifecycleResourceKind::spatialState,
                            observedLifetime.token.resourceId());
                        try
                        {
#endif
                        auto prepared = std::make_unique<State>(std::move(config), next->second, std::move(layout)
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                            , std::move(stateLifetime)
#endif
                        );
                        ++owner.preparations;
                        // A superseded worker result never becomes the active tap.
                        if (next->second == owner.requested.load()) replace(std::move(prepared));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        }
                        catch (...)
                        {
                            if (stateLifetime.isOpen()) stateLifetime.failed();
                            throw;
                        }
#endif
                    }
                }
                catch (...)
                { replace({}); unavailable(next->second); }
            }
            if (current)
            {
                try
                {
                    if (auto latest = current->meter.poll()) published.store(std::move(latest), std::memory_order_release);
                }
                catch (...)
                { const auto serial = current->request; replace({}); unavailable(serial); }
            }
            collect();
        }
        replace({});
        while (owner.readers.load() != 0) std::this_thread::yield();
        retired.clear(); owner.liveMeters.store(0);
        published.store({}, std::memory_order_release);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.note(OptionalLifecycleAction::taskCompleted);
#endif
    }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedLifetime
    {
        explicit ObservedLifetime(OptionalLifecycleToken value) noexcept : token(std::move(value)) {}
        ~ObservedLifetime() { if (complete) token.release(); else token.failed(); }
        OptionalLifecycleToken token;
        bool complete = false;
    } observedLifetime; // stable identity before thread starts; terminal after all members
#endif
    SpatialEnergyService& owner;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::optional<std::pair<Configuration, std::uint64_t>> pending;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext pendingContext;
#endif
    std::unique_ptr<State> current;
    std::vector<std::unique_ptr<State>> retired;
    std::atomic<std::shared_ptr<const SpatialEnergyMeter::Snapshot>> published;
    std::thread thread;
};

SpatialEnergyService::SpatialEnergyService() = default;
SpatialEnergyService::~SpatialEnergyService()
{
    if (control)
    {
        std::lock_guard lock(control->mutex);
        control->owner = nullptr;
        withdraw();
    }
}
SpatialEnergyService::Lease::Lease(Lease&& other) noexcept : control(std::move(other.control))
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    , lifecycleToken(std::move(other.lifecycleToken))
#endif
{}
SpatialEnergyService::Lease& SpatialEnergyService::Lease::operator=(Lease&& other) noexcept
{ if (this != &other) { reset(); control = std::move(other.control);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    lifecycleToken = std::move(other.lifecycleToken);
#endif
} return *this; }
SpatialEnergyService::Lease::~Lease() { reset(); }
void SpatialEnergyService::Lease::reset() noexcept
{
    auto held = std::move(control);
    if (!held) return;
    std::lock_guard lock(held->mutex);
    if (held->owner) held->owner->release();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    lifecycleToken.release();
#endif
}
SpatialEnergyService::Lease SpatialEnergyService::acquire(bool postCoreReady)
{
    if (!postCoreReady) return {};
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto lifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::spatialEnergy, OptionalLifecycleResourceKind::spatialConsumer);
    try
    {
#endif
    if (!control) { control = std::make_shared<Control>(); control->owner = this; }
    if (!worker)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        auto workerLifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
            OptionalLifecycleOwner::spatialEnergy, OptionalLifecycleResourceKind::spatialWorker);
        try
        {
#endif
        worker = std::make_unique<Worker>(*this
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            , std::move(workerLifetime)
#endif
        );
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        }
        catch (...)
        {
            if (workerLifetime.isOpen()) workerLifetime.failed();
            throw;
        }
#endif
    }
    ++consumerCount;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    Lease result(control);
    lifetime.created(); result.lifecycleToken = std::move(lifetime);
    return result;
    }
    catch (...)
    {
        lifetime.failed();
        throw;
    }
#else
    return Lease(control);
#endif
}
void SpatialEnergyService::withdraw() noexcept
{
    active.store(nullptr);
    ++requested;
    worker.reset(); // worker drains callback readers and destroys all filters
    configuration.reset();
    consumerCount.store(0);
}
void SpatialEnergyService::release() noexcept
{ if (consumerCount.fetch_sub(1) == 1) withdraw(); }

bool SpatialEnergyService::Configuration::sameAs(const Configuration& other) const noexcept
{
    if (route != other.route || geometry != other.geometry || device.has_value() != other.device.has_value()) return false;
    if (!device) return true;
    return device->backend == other.device->backend && device->stableId == other.device->stableId
        && device->sampleRate == other.device->sampleRate && device->physicalOutputs == other.device->physicalOutputs
        && device->outputNames == other.device->outputNames;
}
void SpatialEnergyService::configure(Configuration next)
{
    if (!worker || (configuration && configuration->sameAs(next))) return;
    const auto serial = requested.fetch_add(1) + 1;
    configuration = next;
    worker->request(std::move(next), serial);
}
std::shared_ptr<const SpatialEnergyMeter::Snapshot> SpatialEnergyService::snapshot() const
{
    if (!worker) return {};
    auto result = worker->snapshot();
    return result && result->layout && result->layout->generation == requested.load() ? result : nullptr;
}
SpatialEnergyService::Diagnostics SpatialEnergyService::diagnostics() const
{ return {consumerCount.load(), liveMeters.load(), worker != nullptr, preparations.load(), callbackBlocks.load()}; }

void SpatialEnergyService::process(const juce::AudioSourceChannelInfo& buffer, const DeviceRoutePlan* route,
    double rate, std::uint64_t blockRevision, SpatialEnergyMeter::Transport transport) noexcept
{
    static_assert(std::atomic<State*>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);
    if (active.load() == nullptr || buffer.buffer == nullptr) return;
    readers.fetch_add(1);
    if (auto* state = active.load())
    {
        const auto epoch = revision.load();
        if (changes.load() != 0 || epoch != blockRevision || state->request != requested.load()
            || state->configuration.route.get() != route || !state->configuration.device
            || state->configuration.device->sampleRate != rate)
            transport = SpatialEnergyMeter::Transport::unavailable;
        state->meter.process(*buffer.buffer, buffer.startSample, buffer.numSamples, epoch, transport);
        callbackBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    readers.fetch_sub(1);
}
}
