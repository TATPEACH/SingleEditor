#include "Audio/AudioDeviceWarmupTask.h"

#include <cstdlib>
#include <iostream>

namespace
{
void checkWarmup(bool passed, const char* message)
{
    if (!passed)
    {
        std::cerr << "AudioDeviceWarmupTests failed: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
void waitForWarmup(Predicate ready, const char* message)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    checkWarmup(ready(), message);
}
}

void runAudioDeviceWarmupTests()
{
    using singleeditor::AudioDeviceWarmupTask;

    std::atomic<int> calls { 0 };
    {
        AudioDeviceWarmupTask pending;
        checkWarmup(pending.schedule(50, [&] { ++calls; }), "accepts deferred initialization");
        checkWarmup(!pending.schedule(0, [&] { ++calls; }), "does not queue duplicate warmups");
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    checkWarmup(calls == 0, "late timer cannot invoke the destroyed owner's initializer");

    std::atomic<bool> initializerStarted { false };
    juce::WaitableEvent releaseInitializer, closeStarted, closeFinished;
    auto running = std::make_unique<AudioDeviceWarmupTask>();
    running->schedule(0, [&]
    {
        initializerStarted.store(true, std::memory_order_release);
        releaseInitializer.wait();
        ++calls;
    });
    waitForWarmup([&] { return initializerStarted.load(std::memory_order_acquire); },
                  "real worker reaches blocked initialization");
    std::thread closer([task = std::move(running), &closeStarted, &closeFinished]() mutable
    {
        closeStarted.signal();
        task.reset();
        closeFinished.signal();
    });
    checkWarmup(closeStarted.wait(3000), "owner destruction starts");
    checkWarmup(!closeFinished.wait(40), "owner cannot finish destruction while worker still uses its resources");
    releaseInitializer.signal();
    checkWarmup(closeFinished.wait(3000), "owner destruction finishes after initialization exits");
    closer.join();
    checkWarmup(calls == 1, "running initialization finished exactly once before ownership ended");

    AudioDeviceWarmupTask retry;
    retry.schedule(0, [&] { ++calls; });
    waitForWarmup([&] { return calls == 2 && !retry.isPending(); }, "first attempt finishes");
    checkWarmup(retry.schedule(0, [&] { ++calls; }), "a completed attempt can be retried safely");
    waitForWarmup([&] { return calls == 3 && !retry.isPending(); }, "retry joins its predecessor and finishes");
    retry.cancelAndWait();
    checkWarmup(retry.isStopping() && !retry.schedule(0, [&] { ++calls; }),
                "no new initialization can start after cancellation");
    std::cout << "AudioDeviceWarmupTests passed (late timer, running ownership, duplicate/retry/cancel)\n";
}
