#include "OptionalLifecycleTestSupport.h"
#include <array>
#include <functional>
#include <iostream>
#include <optional>

#if !SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
 #error LifecycleViolationTests belongs only to the existing native test target
#endif

namespace singleeditor
{
namespace
{
using namespace lifecycle_test;
using Token = OptionalLifecycleToken;

// Synthetic channel input only: no product factory, window, thread, device,
// timer, sleep or resource-class deadline is created by these drills.
struct Trace
{
    std::array<OptionalLifecycleEventSlot, 64> slots;
    OptionalLifecycleObserver observer {std::span{slots}};
    std::optional<Token> retained;
    OptionalLifecycleContext context() noexcept { return {&observer, 1, 17}; }
};
struct Observation
{
    bool reachedConsumer = false, accepted = false;
    juce::String error;
    std::uint64_t attempted = 0, outstanding = 0;
    std::uint32_t failures = 0;
    std::vector<Event> raw;
};
using Builder = std::function<void(Trace&, bool)>;
using Mutation = std::function<void(std::vector<Event>&)>;

Observation observe(Trace& trace, const Mutation& mutation = {})
{
    Observation result;
    result.attempted = trace.observer.attemptedEventCount();
    result.outstanding = trace.observer.outstandingResourceCount();
    result.failures = trace.observer.failureFlags();
    try
    {
        result.raw = readEvents(trace.observer);
        if (mutation) mutation(result.raw);
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
        return result; // Fixture/read failure is not an expected consumer rejection.
    }
    result.reachedConsumer = true;
    try
    {
        (void) validateResourceStream(trace.observer, result.raw);
        result.accepted = true;
    }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
juce::var json(const Observation& observation)
{
    auto* value = new juce::DynamicObject;
    value->setProperty("consumer_reached", observation.reachedConsumer);
    value->setProperty("actual", observation.reachedConsumer ? (observation.accepted ? "accepted" : "rejected") : "harness-broken");
    value->setProperty("error", observation.error);
    value->setProperty("attempted_events", static_cast<juce::int64>(observation.attempted));
    value->setProperty("observer_failures", static_cast<int>(observation.failures));
    value->setProperty("outstanding_resources", static_cast<juce::int64>(observation.outstanding));
    juce::Array<juce::var> events;
    for (const auto& event : observation.raw) events.add(eventJson(event));
    value->setProperty("raw", events);
    return juce::var(value);
}
Event& unique(std::vector<Event>& raw, Kind kind, Action action)
{
    Event* found = nullptr;
    for (auto& event : raw)
        if (event.kind == kind && event.action == action)
        {
            if (found) throw std::runtime_error("synthetic mutation target is not unique");
            found = &event;
        }
    if (!found) throw std::runtime_error("synthetic mutation target is absent");
    return *found;
}
void processor(Trace& trace, bool)
{
    auto token = Token::request(trace.context(), Owner::parametricEq, Kind::processor);
    token.created(); token.release();
}
void completion(Trace& trace, bool)
{
    auto task = Token::request(trace.context(), Owner::parametricEq, Kind::applyTask);
    task.created(); task.note(Action::taskStarted);
    auto callback = Token::request(trace.context(), Owner::parametricEq, Kind::completionCallback, task.resourceId());
    callback.created(); callback.note(Action::callbackPostRequested);
    callback.note(Action::callbackInvoked); callback.release();
    task.note(Action::taskSucceeded); task.note(Action::taskCompleted); task.release();
}
void device(Trace& trace, bool duplicate)
{
    auto state = Token::request(trace.context(), Owner::audioOutput, Kind::deviceOperationState);
    state.created();
    std::array<std::uint8_t, 16> operation {};
    operation[0] = 1; // Explicit synthetic UUID, not a device request or product identity.
    state.bindOperation(operation);
    if (duplicate) state.bindOperation(operation);
    state.note(Action::taskRejected); state.release();
}

class Drills
{
public:
    void run(const char* id, const char* mode, const char* change, const char* expectedError,
             const Builder& build, const Mutation& mutation = {})
    {
        auto* row = new juce::DynamicObject;
        row->setProperty("id", id); row->setProperty("expected", "baseline-pass then expected-fail");
        row->setProperty("expected_error", expectedError);
        auto* delta = new juce::DynamicObject;
        delta->setProperty("mode", mode); delta->setProperty("single_point", change);
        row->setProperty("mutation", juce::var(delta));
        bool passed = false;
        try
        {
            Trace baseline;
            build(baseline, false);
            const auto normal = observe(baseline);
            row->setProperty("baseline", json(normal));
            if (!normal.reachedConsumer || !normal.accepted)
            {
                row->setProperty("actual", "harness-broken");
                row->setProperty("error", "normal baseline did not pass; negative injection was not run");
            }
            else
            {
                ++baselines;
                Trace injected;
                build(injected, true);
                const auto negative = observe(injected, mutation);
                row->setProperty("injected", json(negative));
                passed = negative.reachedConsumer && !negative.accepted && negative.error == expectedError;
                row->setProperty("actual", passed ? "expected-fail" : "harness-broken");
                row->setProperty("error", negative.error);
                if (!passed) row->setProperty("unexpected_result", negative.accepted ? "violation escaped consumer" : "wrong failure source or exact error");
            }
        }
        catch (const std::exception& error)
        {
            row->setProperty("actual", "harness-broken"); row->setProperty("error", error.what());
        }
        row->setProperty("status", passed ? "pass" : "failed");
        results.add(juce::var(row));
        if (passed) ++rejected; else ++failed;
    }
    void deadline()
    {
        auto* row = new juce::DynamicObject;
        row->setProperty("id", "native-workflow-deadline");
        row->setProperty("expected", "baseline-pass then expected-fail");
        row->setProperty("expected_error", "native scenario timed out");
        row->setProperty("consumer", "lifecycle_test::validateScenarioDeadline");
        row->setProperty("limit_ms", 45000.0);
        row->setProperty("mutation", "Only elapsed input changes from 44999.0 to 45000.0 at the unchanged native workflow predicate; these are explicit synthetic inputs, not event timestamps.");
        row->setProperty("raw", juce::Array<juce::var>());
        bool passed = false;
        try
        {
            validateScenarioDeadline(44999.0, 45000.0);
            ++baselines;
            row->setProperty("baseline", "accepted");
            juce::String error;
            try { validateScenarioDeadline(45000.0, 45000.0); }
            catch (const std::exception& caught) { error = caught.what(); }
            passed = error == "native scenario timed out";
            row->setProperty("actual", passed ? "expected-fail" : "harness-broken");
            row->setProperty("error", error);
        }
        catch (const std::exception& error)
        {
            row->setProperty("baseline", "rejected"); row->setProperty("actual", "harness-broken");
            row->setProperty("error", error.what());
        }
        row->setProperty("status", passed ? "pass" : "failed");
        results.add(juce::var(row));
        if (passed) ++rejected; else ++failed;
    }
    int write(const juce::File& artifacts) const
    {
        auto* report = new juce::DynamicObject;
        report->setProperty("schema", "singleeditor.lifecycle-violation-drills"); report->setProperty("version", 1);
        report->setProperty("synthetic", true); report->setProperty("release_eligible", false);
        report->setProperty("status", failed == 0 && results.size() == 14 && baselines == 14 && rejected == 14 ? "pass" : "failed");
        report->setProperty("cases", results); report->setProperty("baseline_passes", baselines);
        report->setProperty("expected_rejections", rejected); report->setProperty("failures", failed);
        report->setProperty("scope", "Only synthetic observer/token inputs and the existing pure consumers. No product factory, worker, timer, physical device, real runtime matrix or formal Release acceptance is claimed.");
        auto* categories = new juce::DynamicObject;
        categories->setProperty("expected-fail", "An injected single violation reaches the existing consumer and is rejected with the exact expected error after its normal baseline passed.");
        categories->setProperty("harness-broken", "Baseline failure, fixture/read failure, accepted violation or an unrelated exception/error prevents expected-fail credit and makes this entry return nonzero.");
        categories->setProperty("inconclusive", "No applicable evidence or contract is available. Such a boundary remains unproved; it is not a passing drill.");
        categories->setProperty("product-regression", "A real product workflow fails its original contract. Synthetic rejection does not compensate; no product workflow is executed here.");
        report->setProperty("failure_classifications", juce::var(categories));
        auto* boundaries = new juce::DynamicObject;
        boundaries->setProperty("deadline", "The native 45000 ms workflow predicate only. Resource-class asynchronous quiescence deadlines are not defined or proved by this check.");
        boundaries->setProperty("stale_generation", "Changed callback creation identity is rejected. Current-document result adoption and SafePointer/stale-result fencing remain the separate real Main/Mag scenario evidence.");
        boundaries->setProperty("shared_engine", "The generic stream validator has no refcount policy. Actual Optional meter and Mag Spatial/Ambisonic consumer tests supply retained-consumer and last-consumer release evidence; they are not rerun or replaced here.");
        boundaries->setProperty("product_evidence", "Use the separately bound completed Optional, Mag and backend native packages. This receipt does not rerun those workflows or demonstrate every failure classification as a real product failure.");
        boundaries->setProperty("capture", "Raw streams and observer counters describe validation time. A deliberately retained synthetic token remains alive during leak validation; its later fixture destruction cannot turn that observation into pass.");
        report->setProperty("boundaries", juce::var(boundaries));
        const auto file = artifacts.getChildFile("lifecycle-violation-drills.json");
        if (!artifacts.isDirectory() || file.exists() || !file.replaceWithText(juce::JSON::toString(juce::var(report), true)))
            throw std::runtime_error("cannot write fresh lifecycle violation evidence");
        return failed == 0 && results.size() == 14 && baselines == 14 && rejected == 14 ? 0 : 1;
    }
private:
    juce::Array<juce::var> results;
    int baselines = 0, rejected = 0, failed = 0;
};
}

int runLifecycleViolationTests(const juce::File& artifacts)
{
    try
    {
        if (artifacts.exists() || artifacts.createDirectory().failed())
            throw std::runtime_error("lifecycle violation artifacts must be a fresh directory");
        Drills drills;
        constexpr auto channelFailure = "lifecycle channel overflow/failure/uncommitted/open resource";
        drills.run("live-resource-leak", "producer-call", "Omit processor.release and retain the live token through validation.", channelFailure,
            [](Trace& trace, bool inject)
            {
                auto token = Token::request(trace.context(), Owner::parametricEq, Kind::processor); token.created();
                if (inject) trace.retained.emplace(std::move(token)); else token.release();
            });
        drills.run("stale-callback-generation-identity", "raw-field", "Change only completionCallback.callbackInvoked.generation from creation generation 17 to 18.",
            "changed resource identity or post-terminal event", completion,
            [](auto& raw) { ++unique(raw, Kind::completionCallback, Action::callbackInvoked).generation; });
        drills.run("orphan-worker-task", "producer-argument", "Change only worker request parent from the actual State ID to absent ID 999.",
            "missing real parent acquisition", [](Trace& trace, bool inject)
            {
                auto state = Token::request(trace.context(), Owner::recovery, Kind::recoveryWriterState); state.created();
                auto worker = Token::request(trace.context(), Owner::recovery, Kind::recoveryWriterWorker, inject ? 999 : state.resourceId());
                worker.created(); worker.note(Action::taskStarted); worker.note(Action::taskCompleted); worker.release(); state.release();
            });
        drills.run("orphan-timer-callback", "producer-call", "Omit delayed callback.release after its observed delivery; keep the timer identity live through validation.",
            channelFailure, [](Trace& trace, bool inject)
            {
                auto timer = Token::request(trace.context(), Owner::audioOutput, Kind::deviceWarmupCallback); timer.created();
                timer.note(Action::callbackPostRequested); timer.note(Action::callbackInvoked);
                if (inject) trace.retained.emplace(std::move(timer)); else timer.release();
            });
        drills.run("wrong-backend-owner", "producer-argument", "Change only the Recovery State request owner to audio-output; all emitted identity fields remain internally consistent.",
            "backend kind/owner acquisition mismatch", [](Trace& trace, bool inject)
            {
                auto state = Token::request(trace.context(), inject ? Owner::audioOutput : Owner::recovery, Kind::recoveryWriterState);
                state.created(); state.release();
            });
        drills.run("duplicate-terminal", "producer-call", "Call release a second time; the real token records invalidTransition instead of fabricating a second terminal.",
            channelFailure, [](Trace& trace, bool inject)
            {
                auto token = Token::request(trace.context(), Owner::parametricEq, Kind::processor); token.created(); token.release();
                if (inject) token.release();
            });
        drills.run("missing-terminal-on-destruction", "producer-call", "Omit release before the actual token destructor; its missingTerminal flag/event and nonzero balance must be rejected.",
            channelFailure, [](Trace& trace, bool inject)
            {
                auto token = Token::request(trace.context(), Owner::parametricEq, Kind::processor); token.created();
                if (!inject) token.release();
            });
        drills.run("observer-event-loss", "raw-row", "Remove only the committed processor.created row; leave sequence values and actual observer attempt count unchanged.",
            channelFailure, processor, [](auto& raw)
            {
                const auto sequence = unique(raw, Kind::processor, Action::created).sequence;
                raw.erase(std::find_if(raw.begin(), raw.end(), [=](const auto& event) { return event.sequence == sequence; }));
            });
        drills.run("callback-delivery-xor", "producer-call", "Append one callbackDiscarded note after callbackInvoked, before the real token release.",
            "invalid callback discard", [](Trace& trace, bool inject)
            {
                auto callback = Token::request(trace.context(), Owner::audioOutput, Kind::deviceWarmupCallback); callback.created();
                callback.note(Action::callbackPostRequested); callback.note(Action::callbackInvoked);
                if (inject) callback.note(Action::callbackDiscarded);
                callback.release();
            });
        drills.run("task-completion-missing", "producer-call", "Omit the actual taskCompleted note while retaining start and release.",
            "task released before actual completion", [](Trace& trace, bool inject)
            {
                auto worker = Token::request(trace.context(), Owner::spatialEnergy, Kind::spatialWorker); worker.created();
                worker.note(Action::taskStarted); if (!inject) worker.note(Action::taskCompleted); worker.release();
            });
        drills.run("post-terminal-callback", "producer-call", "Append one callbackInvoked note after the callback resource's release.",
            "changed resource identity or post-terminal event", [](Trace& trace, bool inject)
            {
                auto callback = Token::request(trace.context(), Owner::audioOutput, Kind::deviceWarmupCallback); callback.created();
                callback.note(Action::callbackPostRequested); callback.note(Action::callbackInvoked); callback.release();
                if (inject) callback.note(Action::callbackInvoked);
            });
        drills.run("zero-device-operation-uuid", "raw-field", "Zero only the 16 bytes of the device State operationBound UUID.",
            "invalid actual device operation binding", [](Trace& trace, bool) { device(trace, false); },
            [](auto& raw) { unique(raw, Kind::deviceOperationState, Action::operationBound).operationId.fill(0); });
        drills.run("duplicate-device-operation-uuid", "producer-call", "Bind the same real synthetic UUID a second time to the same device State.",
            "invalid actual device operation binding", device);
        drills.deadline();
        const auto result = drills.write(artifacts);
        std::cout << "Lifecycle violation drills " << (result == 0 ? "passed" : "failed")
            << " (synthetic consumer inputs only; no product acceptance)" << std::endl;
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Lifecycle violation harness-broken: " << error.what() << std::endl;
        return 1;
    }
}
}
