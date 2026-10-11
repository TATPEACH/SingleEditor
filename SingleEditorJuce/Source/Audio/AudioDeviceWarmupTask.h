#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#include <array>
#endif

namespace singleeditor
{
// Owns the delayed callback and worker lifetime, so neither can outlive the
// engine resources used by the initializer. Cancel from the owning thread,
// never from inside the initializer itself.
class AudioDeviceWarmupTask
{
public:
    AudioDeviceWarmupTask() = default;
    AudioDeviceWarmupTask(const AudioDeviceWarmupTask&) = delete;
    AudioDeviceWarmupTask& operator=(const AudioDeviceWarmupTask&) = delete;
    ~AudioDeviceWarmupTask() { cancelAndWait(); }

    bool schedule(int delayMs, std::function<void()> initialise
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , OptionalLifecycleContext observedContext = currentOptionalLifecycleContext()
#endif
    )
    {
        auto shared = state;
        const std::lock_guard lock(shared->launchMutex);
        if (shared->stopping.load(std::memory_order_acquire)
            || shared->pending.exchange(true, std::memory_order_acq_rel))
            return false;

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        // This holder has exactly the original delayed callable's lifetime.
        // Its first member records terminal after that callable is destroyed.
        auto callbackRequest = OptionalLifecycleToken::request(observedContext, OptionalLifecycleOwner::audioOutput,
            OptionalLifecycleResourceKind::deviceWarmupCallback);
        std::shared_ptr<ObservedDelayedCallback> observedCallback;
        try
        {
            observedCallback = std::make_shared<ObservedDelayedCallback>(std::move(callbackRequest));
            observedCallback->callback =
#else
        juce::Timer::callAfterDelay(juce::jmax(0, delayMs),
#endif
            [shared, initialise = std::move(initialise)
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                , observedCallbackToken = &observedCallback->lifetime.token
#endif
            ]() mutable
            {
                const std::lock_guard lock(shared->launchMutex);
                if (shared->stopping.load(std::memory_order_acquire))
                    return;

                // A completed attempt may be followed by a retry.
                if (shared->worker.joinable())
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                {
#endif
                    shared->worker.join();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                    shared->observedWorker.release();
                }
#endif
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                shared->observedWorker = OptionalLifecycleToken::request(observedCallbackToken->creationContext(),
                    OptionalLifecycleOwner::audioOutput, OptionalLifecycleResourceKind::deviceWarmupWorker,
                    observedCallbackToken->resourceId());
#endif
                try
                {
                    shared->worker = std::thread([shared, initialise = std::move(initialise)]
                    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        shared->observedWorker.note(OptionalLifecycleAction::taskStarted);
#endif
                        if (!shared->stopping.load(std::memory_order_acquire))
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        {
                            const ScopedObservedWorker operationScope(shared->observedWorker);
                            const ScopedOptionalLifecycleContext contextScope(shared->observedWorker.creationContext());
#endif
                            initialise();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        }
#endif
                        shared->pending.store(false, std::memory_order_release);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                        shared->observedWorker.note(OptionalLifecycleAction::taskCompleted);
#endif
                    });
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                    shared->observedWorker.created();
#endif
                }
                catch (const std::system_error&)
                {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                    shared->observedWorker.failed();
#endif
                    // Explicit playback can still initialise synchronously.
                    shared->pending.store(false, std::memory_order_release);
                }
            }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            ;
            observedCallback->lifetime.token.created();
            observedCallback->lifetime.created = true;
        }
        catch (...)
        {
            // If allocation failed the request never moved into the holder.
            // Otherwise its guard reports failure after the partial callable
            // has been destroyed. Neither path reports a created callback.
            if (observedCallback) observedCallback.reset();
            else callbackRequest.failed();
            throw;
        }
        observedCallback->lifetime.token.note(OptionalLifecycleAction::callbackPostRequested);
        juce::Timer::callAfterDelay(juce::jmax(0, delayMs), [observedCallback]
        {
            observedCallback->invoked.store(true, std::memory_order_release);
            observedCallback->lifetime.token.note(OptionalLifecycleAction::callbackInvoked);
            observedCallback->callback();
        });
#else
            );
#endif
        return true;
    }

    void cancelAndWait()
    {
        std::thread running;
        {
            const std::lock_guard lock(state->launchMutex);
            state->stopping.store(true, std::memory_order_release);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (state->worker.joinable()) state->observedWorker.note(OptionalLifecycleAction::cancelRequested);
#endif
            running = std::move(state->worker);
        }
        // Do not hold launchMutex while the initializer observes cancellation.
        if (running.joinable())
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        {
#endif
            running.join();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            state->observedWorker.release();
        }
#endif
        state->pending.store(false, std::memory_order_release);
    }

    bool isPending() const noexcept { return state->pending.load(std::memory_order_acquire); }
    bool isStopping() const noexcept { return state->stopping.load(std::memory_order_acquire); }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // The pointer exists only while this actual warmup initializer is running.
    // It never escapes to a different thread or changes the token's identity.
    static void bindCurrentOperation(const std::array<std::uint8_t, 16>& operation) noexcept
    {
        if (currentObservedWorker != nullptr) currentObservedWorker->bindOperation(operation);
    }
#endif

private:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedTerminal
    {
        OptionalLifecycleToken token;
        bool created = false;
        ~ObservedTerminal()
        {
            if (token.isOpen())
            {
                if (created) token.release();
                else token.failed();
            }
        }
    };
    struct ObservedDelayedCallback
    {
        explicit ObservedDelayedCallback(OptionalLifecycleToken&& request) noexcept
        {
            lifetime.token = std::move(request);
        }
        ~ObservedDelayedCallback()
        {
            if (lifetime.created && !invoked.load(std::memory_order_acquire))
                lifetime.token.note(OptionalLifecycleAction::callbackDiscarded);
        }
        ObservedTerminal lifetime; // destroyed after callback and its original captures
        std::function<void()> callback;
        std::atomic<bool> invoked { false };
    };
    inline static thread_local const OptionalLifecycleToken* currentObservedWorker = nullptr;
    struct ScopedObservedWorker
    {
        explicit ScopedObservedWorker(const OptionalLifecycleToken& token) noexcept
            : previous(currentObservedWorker) { currentObservedWorker = &token; }
        ~ScopedObservedWorker() { currentObservedWorker = previous; }
        const OptionalLifecycleToken* previous;
    };
#endif
    struct State
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        OptionalLifecycleToken observedWorker; // released only by an existing join path
#endif
        std::mutex launchMutex;
        std::thread worker;
        std::atomic<bool> pending { false };
        std::atomic<bool> stopping { false };
    };
    std::shared_ptr<State> state = std::make_shared<State>();
};
}
