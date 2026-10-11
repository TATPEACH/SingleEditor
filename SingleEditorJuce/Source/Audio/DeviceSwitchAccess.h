#pragma once
#include <atomic>
#include <memory>
#include <utility>

namespace singleeditor
{
// Revocable evidence that the exact previous owner has closed. No public
// constructor can grant access; copies do not extend the switch window.
class DeviceSwitchAccess final
{
public:
    DeviceSwitchAccess() = default;
    bool isValid() const noexcept
    {
        return state && state->ownerAlive.load(std::memory_order_acquire)
            && state->windowOpen.load(std::memory_order_acquire);
    }
private:
    friend class AudioEngine;
    friend struct JuceAsioBackendTestAccess;
    struct State
    {
        std::atomic<bool> ownerAlive { true }, windowOpen { false };
    };
    explicit DeviceSwitchAccess(std::shared_ptr<State> value) : state(std::move(value)) {}
    std::shared_ptr<State> state;
};
}
