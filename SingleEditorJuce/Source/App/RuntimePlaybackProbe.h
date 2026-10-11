#pragma once

#if !SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#error RuntimePlaybackProbe is restricted to explicit test targets.
#endif

#include <juce_events/juce_events.h>
#include <functional>
#include <memory>

namespace singleeditor
{
// Opt-in, message-thread observations only. The owner starts this after CoreReady.
// No audio callback instrumentation and no file I/O during the sampled interval.
class RuntimePlaybackProbe final : private juce::Timer
{
public:
    using Reader = std::function<juce::var()>;

    static std::unique_ptr<RuntimePlaybackProbe> create(const juce::String& path,
                                                       int durationMs, Reader reader)
    {
        if (path.isEmpty() || !juce::File::isAbsolutePath(path) || !reader)
            return {};
        const juce::File output(path);
        if (output.exists()) return {}; // Never overwrite an earlier receipt.
        return std::unique_ptr<RuntimePlaybackProbe>(new RuntimePlaybackProbe(output, durationMs, std::move(reader)));
    }

    ~RuntimePlaybackProbe() override { finish(false); }
    bool isFinished() const noexcept { return finished; }
    bool didWriteReport() const noexcept { return writeSucceeded; }

private:
    RuntimePlaybackProbe(juce::File output, int durationMs, Reader reader)
        : outputFile(std::move(output)), read(std::move(reader)),
          requestedDurationMs(juce::jlimit(200, 600000, durationMs)),
          startedAt(juce::Time::getMillisecondCounterHiRes()), previousTickAt(startedAt),
          startedUtc(juce::Time::getCurrentTime().toISO8601(true))
    {
        samples.ensureStorageAllocated(requestedDurationMs / intervalMs + 2);
        startTimer(intervalMs);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        auto sample = read();
        if (!sample.isObject())
        {
            auto* error = new juce::DynamicObject();
            error->setProperty("observation_unavailable", true);
            sample = juce::var(error);
        }
        sample.getDynamicObject()->setProperty("elapsed_ms", now - startedAt);
        sample.getDynamicObject()->setProperty("ui_tick_ms", now - previousTickAt);
        previousTickAt = now;
        samples.add(std::move(sample));
        if (now - startedAt >= requestedDurationMs) finish(true);
    }

    void finish(bool durationReached)
    {
        if (finished) return;
        stopTimer();
        finished = true;
        auto* report = new juce::DynamicObject();
        report->setProperty("schema", "singleeditor.runtime-playback-probe.v1");
        report->setProperty("started_utc", startedUtc);
        report->setProperty("duration_complete", durationReached);
        report->setProperty("requested_duration_ms", requestedDurationMs);
        report->setProperty("sample_interval_ms", intervalMs);
        report->setProperty("sample_count", samples.size());
        report->setProperty("scope", "UI timer observations and JUCE device counters; not compositor frame times or per-callback maxima. Driver xrun -1 means unsupported; unavailable device fields are null.");
        report->setProperty("samples", juce::var(samples));
        const juce::var payload(report);
        if (outputFile.getParentDirectory().createDirectory().wasOk() && !outputFile.exists())
            writeSucceeded = outputFile.replaceWithText(juce::JSON::toString(payload));
    }

    static constexpr int intervalMs = 200;
    juce::File outputFile;
    Reader read;
    int requestedDurationMs;
    double startedAt, previousTickAt;
    juce::String startedUtc;
    juce::Array<juce::var> samples;
    bool finished = false, writeSucceeded = false;
};
} // namespace singleeditor
