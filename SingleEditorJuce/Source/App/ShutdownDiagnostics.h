#pragma once

#include <juce_core/juce_core.h>

namespace singleeditor
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
// Queried only from teardown, never from startup, rendering or the audio callback.
// Opt-in diagnostics do not change the benchmark's close/kill deadline.
inline void traceShutdown(const char* stage)
{
    static const auto path = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_SHUTDOWN_LOG", {});
    if (path.isEmpty())
        return;

    juce::File(path).appendText(juce::String(juce::Time::getMillisecondCounterHiRes(), 3)
                               + " | " + stage + "\n");
}
#else
inline void traceShutdown(const char*) noexcept {}
#endif
}
