#include "App/RuntimePlaybackProbe.h"
#include "App/ShutdownDiagnostics.h"
#include "UI/Commands/NativeFocusTrace.h"

#include <cstdlib>
#include <iostream>

namespace
{
class ScopedTestEnvironment
{
public:
    ScopedTestEnvironment(const char* name, const juce::String& value) : variable(name)
    {
        if (const auto* old = std::getenv(name)) { previous = old; hadPrevious = true; }
        set(value.toRawUTF8());
    }
    ~ScopedTestEnvironment() { set(hadPrevious ? previous.toRawUTF8() : nullptr); }
private:
    void set(const char* value)
    {
#if JUCE_WINDOWS
        _putenv_s(variable, value != nullptr ? value : "");
#else
        if (value != nullptr) setenv(variable, value, 1); else unsetenv(variable);
#endif
    }
    const char* variable;
    juce::String previous;
    bool hadPrevious = false;
};

void check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "RuntimePlaybackProbeTests failed: " << message << '\n';
    std::exit(1);
}

void pumpUntil(const std::function<bool()>& done)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    check(done(), "actual diagnostic timer completes within its bounded test deadline");
}
}

void runRuntimePlaybackProbeTests()
{
    using singleeditor::RuntimePlaybackProbe;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("singleeditor-runtime-probe-test", "", false);
    check(directory.createDirectory().wasOk(), "temporary report directory is available");
    const auto focusFile = directory.getChildFile("focus.txt");
    const auto shutdownFile = directory.getChildFile("shutdown.txt");
    ScopedTestEnvironment focusPath("SINGLEEDITOR_NATIVE_FOCUS_TRACE", focusFile.getFullPathName());
    ScopedTestEnvironment shutdownPath("SINGLE_EDITOR_SHUTDOWN_LOG", shutdownFile.getFullPathName());
    juce::Component receiver;
    receiver.setComponentID("observer-test-receiver");
    const juce::KeyPress key(juce::KeyPress::tabKey);
    singleeditor::traceNativeFocus("test.focus", receiver, &key);
    singleeditor::traceShutdown("test.shutdown");
    check(focusFile.loadFileAsString().contains("test.focus")
              && focusFile.loadFileAsString().contains("observer-test-receiver"),
          "test-only focus trace writes actual receiver context");
    check(shutdownFile.loadFileAsString().contains("test.shutdown"),
          "test-only shutdown trace writes its actual teardown marker");
    const auto file = directory.getChildFile("complete.json");
    int reads = 0;
    auto reader = [&reads]
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("sequence", ++reads);
        row->setProperty("driver_xruns", -1); // Unknown must remain unknown.
        row->setProperty("device", juce::var());
        return juce::var(row);
    };
    check(RuntimePlaybackProbe::create({}, 400, reader) == nullptr && reads == 0,
          "disabled probe allocates no observer and never reads its source");
    check(RuntimePlaybackProbe::create("relative.json", 400, reader) == nullptr,
          "diagnostic output requires an explicit absolute path");
    auto probe = RuntimePlaybackProbe::create(file.getFullPathName(), 400, reader);
    check(probe != nullptr && !file.exists(), "no report write occurs at start");
    pumpUntil([&] { return probe->isFinished(); });
    check(probe->didWriteReport(), "completed capture writes a report");
    const auto report = juce::JSON::parse(file);
    const auto* samples = report["samples"].getArray();
    check(static_cast<bool>(report["duration_complete"]) && samples != nullptr && samples->size() == reads && reads >= 2,
          "completion reports exactly the timer observations that actually occurred");
    for (const auto& sample : *samples)
        check(static_cast<int>(sample["driver_xruns"]) == -1 && sample["device"].isVoid()
                  && static_cast<double>(sample["ui_tick_ms"]) > 0.0,
              "unknown fields and observed UI-tick durations are retained");
    const auto completedReads = reads;
    juce::Thread::sleep(230);
    juce::Timer::callPendingTimersSynchronously();
    check(reads == completedReads, "completed probe stops sampling rather than polling while idle");
    const auto saved = file.loadFileAsString();
    probe.reset();
    check(file.loadFileAsString() == saved, "destruction never rewrites completed evidence");
    check(RuntimePlaybackProbe::create(file.getFullPathName(), 400, reader) == nullptr,
          "a later probe cannot overwrite the completed report");

    const auto partialFile = directory.getChildFile("partial.json");
    probe = RuntimePlaybackProbe::create(partialFile.getFullPathName(), 2000, reader);
    const auto beforePartial = reads;
    pumpUntil([&] { return reads > beforePartial; });
    check(!partialFile.exists(), "samples remain in memory until capture ends");
    probe.reset();
    const auto partial = juce::JSON::parse(partialFile);
    check(!static_cast<bool>(partial["duration_complete"]) && static_cast<int>(partial["sample_count"]) > 0,
          "early owner destruction preserves partial evidence without claiming duration completion");
    check(directory.isAChildOf(juce::File::getSpecialLocation(juce::File::tempDirectory)),
          "cleanup stays in the test-owned temporary directory");
    check(directory.deleteRecursively(), "test-owned reports are removed");
    std::cout << "RuntimePlaybackProbeTests passed: opt-in, real timer, unknown counters, bounded completion, partial shutdown and no overwrite\n";
}
