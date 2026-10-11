#pragma once

#include "Audio/SpatialEnergyLayout.h"
#include <mutex>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor
{
// Passive engine seam. Heavy state and the 20 Hz worker exist only under an
// explicit post-Core view lease. Public control methods run on the owner thread.
class SpatialEnergyService final
{
    struct Control;
    struct State;
    class Worker;
public:
    class Lease final
    {
    public:
        Lease() = default;
        Lease(Lease&&) noexcept;
        Lease& operator=(Lease&&) noexcept;
        ~Lease();
        explicit operator bool() const noexcept { return control != nullptr; }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        friend class SpatialEnergyService;
        explicit Lease(std::shared_ptr<Control> value) : control(std::move(value)) {}
        void reset() noexcept;
        std::shared_ptr<Control> control;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        OptionalLifecycleToken lifecycleToken;
#endif
    };
    struct Configuration
    {
        std::shared_ptr<const DeviceRoutePlan> route;
        std::shared_ptr<const SpeakerGeometryProfile> geometry;
        std::optional<DeviceOutput::Snapshot> device;
        bool sameAs(const Configuration&) const noexcept;
    };
    struct Diagnostics
    {
        int consumers = 0, liveMeters = 0;
        bool workerRunning = false;
        std::uint64_t preparations = 0, callbackBlocks = 0;
    };
    // Bracket control mutations. An overlapping render block is rejected; the
    // next complete block starts a new measurement epoch. Nesting is supported.
    class Change final
    {
    public:
        explicit Change(SpatialEnergyService& value, bool enabled = true) noexcept : owner(value), active(enabled)
        { if (active) { owner.changes.fetch_add(1); owner.revision.fetch_add(1); } }
        ~Change() { if (active) { owner.revision.fetch_add(1); owner.changes.fetch_sub(1); } }
        Change(const Change&) = delete;
        Change& operator=(const Change&) = delete;
    private:
        SpatialEnergyService& owner;
        bool active;
    };
    SpatialEnergyService();
    ~SpatialEnergyService();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept { optionalLifecycleContext = context; }
#endif
    Lease acquire(bool postCoreReady);
    void configure(Configuration);
    bool hasConsumers() const noexcept { return consumerCount.load(std::memory_order_acquire) != 0; }
    std::shared_ptr<const SpatialEnergyMeter::Snapshot> snapshot() const;
    Diagnostics diagnostics() const;
    std::uint64_t beginBlock() const noexcept { return revision.load(); }
    // Single callback writer, no mutex/shared_ptr operations or destruction.
    void process(const juce::AudioSourceChannelInfo&, const DeviceRoutePlan*, double actualRate,
        std::uint64_t blockRevision, SpatialEnergyMeter::Transport) noexcept;
private:
    void release() noexcept;
    void withdraw() noexcept;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext = currentOptionalLifecycleContext();
#endif
    std::shared_ptr<Control> control;
    std::unique_ptr<Worker> worker;
    std::optional<Configuration> configuration;
    std::atomic<State*> active {nullptr};
    std::atomic<unsigned> readers {0}, changes {0};
    std::atomic<int> consumerCount {0}, liveMeters {0};
    std::atomic<std::uint64_t> revision {1}, requested {0}, preparations {0}, callbackBlocks {0};
};
}
