#pragma once
#include "RecoveryStore.h"
#include <mutex>

namespace singleeditor::recovery
{
// Mapping reservations follow the final reader/history reference, not the
// scheduling job. This counts bytes across every resident recovered copy.
// No filesystem, allocation of PCM, thread or timer in construction.
class RecoveryAllocationBudget final
{
public:
    explicit RecoveryAllocationBudget(size_t maximum) : state(std::make_shared<State>(maximum)) {}
    Status reserve(size_t bytes, std::shared_ptr<const void>& output) const
    {
        if (output || bytes == 0) return { Error::invalidPacket, "restore_allocation_request" };
        const std::lock_guard lock(state->mutex);
        if (bytes > state->maximum || state->used > state->maximum - bytes)
            return { Error::budgetExceeded, "restore_resident_allocation_budget" };
        try
        {
            auto lease = std::make_shared<Lease>(state, bytes);
            state->used += bytes;
            output = std::move(lease);
            return {};
        }
        catch (...) { return { Error::resourceOrEnvironmentFault, "restore_allocation_lease" }; }
    }
    size_t usedBytes() const { const std::lock_guard lock(state->mutex); return state->used; }
    bool tryUsedBytes(size_t& result) const
    {
        result = 0;
        const std::unique_lock lock(state->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        result = state->used;
        return true;
    }
private:
    struct State final
    {
        explicit State(size_t limit) : maximum(limit) {}
        std::mutex mutex;
        size_t maximum = 0, used = 0;
    };
    struct Lease final
    {
        Lease(std::shared_ptr<State> value, size_t size) : state(std::move(value)), bytes(size) {}
        ~Lease() { const std::lock_guard lock(state->mutex); state->used -= bytes; }
        std::shared_ptr<State> state;
        size_t bytes;
    };
    std::shared_ptr<State> state;
};
}
