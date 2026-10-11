#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

namespace singleeditor
{
// One control owner, potentially concurrent driver callbacks. Only arm/retire
// may wait, and those run after the owner has stopped/disposed the old stream.
// Callback publication never allocates, logs, starts timers or enters the driver.
class AsioDriverEvents final
{
public:
    enum Flag : std::uint32_t
    {
        reset = 1, sampleRate = 2, bufferSize = 4, latency = 8,
        resync = 16, overload = 32, controlPanel = 64
    };
    static constexpr auto requiresRevalidation = reset | sampleRate | bufferSize | resync | controlPanel;
    static constexpr std::uint32_t suspendedBit = 1u << 31;
    struct Snapshot
    {
        std::uint64_t generation = 0;
        std::uint32_t pending = 0;
        std::uint64_t resets = 0, xruns = 0, lastEventTicks = 0;
        bool outputSuspended = false;
    };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    bool arm(std::uint64_t next) noexcept
    {
        if (next == 0 || next <= lastGeneration) return false;
        retire();
        flags.store(0); resetCount.store(0); xrunCount.store(0); lastTicks.store(0);
        lastGeneration = next;
        current.store(next);
        return true;
    }
    void retire() noexcept
    {
        const auto wasArmed = current.exchange(0) != 0;
        while (writers.load() != 0) std::this_thread::yield();
        if (wasArmed) flags.store(suspendedBit, std::memory_order_release);
    }
    std::uint64_t generation() const noexcept { return current.load(std::memory_order_acquire); }
    bool outputSuspended() const noexcept { return (flags.load(std::memory_order_acquire) & suspendedBit) != 0; }
    // Call only after validating actual driver state. A newer pending event
    // rejects acknowledgement instead of clearing its output suspension.
    bool acknowledgeRevalidation(std::uint64_t token) noexcept
    {
        if (token == 0 || generation() != token) return false;
        auto expected = suspendedBit;
        return flags.compare_exchange_strong(expected, 0, std::memory_order_acq_rel) || expected == 0;
    }
    bool publish(std::uint64_t token, Flag flag, std::uint64_t monotonicTicks) noexcept
    {
        if (token == 0 || current.load() != token) return false;
        // These admission operations share one sequential order with retire,
        // so reset cannot miss a writer that still observed the old token.
        writers.fetch_add(1);
        if (token == 0 || current.load() != token)
        { writers.fetch_sub(1); return false; }
        if (flag == reset) incrementSaturated(resetCount);
        if (flag == overload) incrementSaturated(xrunCount);
        auto previous = lastTicks.load(std::memory_order_relaxed);
        while (previous < monotonicTicks && !lastTicks.compare_exchange_weak(previous, monotonicTicks, std::memory_order_relaxed)) {}
        flags.fetch_or(flag | ((flag & requiresRevalidation) != 0 ? suspendedBit : 0), std::memory_order_release);
        writers.fetch_sub(1);
        return true;
    }
    Snapshot read(std::uint64_t token, bool consume, bool includeCounters = true) noexcept
    {
        if (token == 0 || current.load(std::memory_order_acquire) != token) return {};
        const auto pending = consume ? flags.fetch_and(suspendedBit, std::memory_order_acq_rel) : flags.load(std::memory_order_acquire);
        Snapshot result{token, pending & ~suspendedBit,
            includeCounters ? resetCount.load(std::memory_order_relaxed) : 0, includeCounters ? xrunCount.load(std::memory_order_relaxed) : 0,
            includeCounters ? lastTicks.load(std::memory_order_relaxed) : 0, (pending & suspendedBit) != 0};
        return current.load(std::memory_order_acquire) == token ? result : Snapshot{};
    }
private:
    friend struct AsioDriverEventsTestAccess;
    static void incrementSaturated(std::atomic<std::uint64_t>& value) noexcept
    {
        auto previous = value.load(std::memory_order_relaxed);
        while (previous != std::numeric_limits<std::uint64_t>::max()
            && !value.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {}
    }
    std::atomic<std::uint64_t> current{0}, resetCount{0}, xrunCount{0}, lastTicks{0};
    std::atomic<std::uint32_t> flags{0}, writers{0};
    std::uint64_t lastGeneration = 0; // Only the serialized control owner writes.
};
}
