#include "Bnk/BnkRuntimeSession.h"
#include <juce_events/juce_events.h>
#include <iostream>
#include <stdexcept>
#include <thread>

#if !SINGLE_EDITOR_BNK_SESSION_TESTING
 #error The fake preparer is available only in the existing ToolTest target
#endif

namespace
{
using namespace singleeditor;
void expectSession(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
template<class Predicate> void awaitSession(Predicate predicate)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!predicate() && juce::Time::getMillisecondCounterHiRes() < until) juce::Thread::sleep(2);
    expectSession(predicate(), "session worker did not settle within its test deadline");
}
}

void runBnkRuntimeSessionTests()
{
    const juce::ScopedJuceInitialiser_GUI init;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-BnkSession-" + juce::Uuid().toString());
    expectSession(root.createDirectory().wasOk(), "create private session fixture");
    BnkDocument bank;
    expectSession(!BnkRuntimeSession::create(bank, 1), "empty ordinary document cannot construct a BNK runtime");
    auto state = std::make_shared<BnkDocument::State>();
    state->info.file = root.getChildFile("session.bnk");
    juce::MemoryOutputStream bytes;
    bytes.write("BKHD", 4); bytes.writeInt(8); bytes.writeInt(128); bytes.writeInt(7);
    expectSession(state->info.file.replaceWithData(bytes.getData(), bytes.getDataSize()), "write owned admitted-bank witness");
    state->sourceFileSize = state->info.file.getSize();
    state->sourceModifiedMs = state->info.file.getLastModificationTime().toMilliseconds();
    state->sourceFileIdentifier = state->info.file.getFileIdentifier();
    expectSession(bank.adoptParsedState(state), "adopt only the private bank fixture");
    struct WorkerControl { std::atomic<int> starts { 0 }; std::atomic<bool> gate { false }; };
    const auto control = std::make_shared<WorkerControl>();
    auto& starts = control->starts;
    auto& gate = control->gate;
    auto session = BnkRuntimeSession::createForTesting(bank, 1, [control](const auto&, const auto& operation)
    {
        operation->beginBankPreparation();
        const auto ordinal = control->starts.fetch_add(1);
        if (ordinal == 0) awaitSession([&] { return control->gate.load(); });
        BnkPreparedPlayback result; result.detail = "prepared-" + juce::String(ordinal);
        return result;
    });
    expectSession(session != nullptr, "ToolTest fake factory creates the same generation owner interface");
    BnkPlaybackContext context; context.switchValueToken = "rain"; context.stateValueToken = "mood/quiet";
    session->setContext(context, 10);
    session->setContext({});
    expectSession(session->preferredSwitch(10) == "rain" && session->preferredState(10) == "mood/quiet",
        "same-generation event context survives another event with no controls");
    const auto policy = std::make_shared<media::OptionalMediaFailureState>();
    const auto intent = [&](int token)
    {
        BnkRuntimeSession::PreviewIntent value;
        value.token = token;
        value.operation = std::make_shared<media::OptionalMediaOperation>(policy,
            media::OptionalMediaFailureState::Intent::initial, std::nullopt, media::OptionalMediaFailureState::Module::bank);
        return value;
    };
    const auto admit = [](const auto&, bool) { return true; };
    std::vector<int64_t> delivered;
    const auto deliver = [&](const auto& value, BnkPreparedPlayback&&, bool started)
    {
        expectSession(started, "only admitted work reaches preparation in this case");
        if (value.operation->cancelFlag().load()) value.operation->cancelled();
        else { delivered.push_back(value.token); value.operation->succeed(); }
    };
    auto first = intent(1); const auto firstOperation = first.operation;
    session->queue(std::move(first)); session->poll(admit, deliver);
    awaitSession([&] { return starts.load() == 1; });
    const auto oldWorker = session->workerForTesting();
    auto skipped = intent(2); const auto skippedOperation = skipped.operation;
    session->queue(std::move(skipped)); session->queue(intent(3));
    expectSession(firstOperation->cancelFlag().load() && skippedOperation->report().cancelled,
        "latest selection cancels running work and coalesces the unstarted middle intent");
    gate.store(true);
    awaitSession([&] { session->poll(admit, deliver); return delivered.size() == 1; });
    expectSession(delivered[0] == 3 && starts.load() == 2 && oldWorker.expired()
        && firstOperation->report().cancelled, "only the latest prepared selection is delivered and old work releases");

    // Finish real threaded preparation while deliberately withholding UI delivery.
    session->queue(intent(4)); session->poll(admit, deliver);
    awaitSession([&] { return session->workerFinishedForTesting(); });
    const auto lateWorker = session->workerForTesting();
    session->bind(2); session->poll(admit, deliver);
    expectSession(delivered.size() == 1 && lateWorker.expired() && session->generation() == 2
        && session->preferredSwitch(10).isEmpty() && session->preferredState(10).isEmpty(),
        "new generation refuses completed stale delivery and forgets old event context");

    auto rejected = intent(5); const auto rejectedOperation = rejected.operation;
    session->queue(std::move(rejected));
    bool rejectedDelivery = false;
    session->poll([](const auto&, bool) { return false; }, [&](const auto&, BnkPreparedPlayback&&, bool started)
        { rejectedDelivery = !started; });
    expectSession(rejectedDelivery && rejectedOperation->report().cancelled && starts.load() == 3,
        "failed current-workspace admission cancels without invoking a backend factory");

    // An old owner must not clear a new bank installed before its release.
    const auto replacement = std::make_shared<BnkDocument::State>(*state);
    expectSession(bank.adoptParsedState(replacement), "install a newer explicit parsed-bank identity");
    session->release(); session.reset();
    expectSession(bank.getState() == replacement, "released old owner cannot clear another owner's newer parsed state");

    gate.store(false); starts.store(0);
    auto closing = BnkRuntimeSession::createForTesting(bank, 3, [control](const auto&, const auto& operation)
    {
        operation->beginBankPreparation(); control->starts.fetch_add(1);
        awaitSession([&] { return control->gate.load(); }); operation->cancelled(); return BnkPreparedPlayback{};
    });
    auto closeIntent = intent(6); const auto closingOperation = closeIntent.operation;
    closing->queue(std::move(closeIntent)); closing->poll(admit, deliver);
    awaitSession([&] { return starts.load() == 1; });
    const auto draining = closing->workerForTesting();
    const std::weak_ptr<BnkRuntimeSession> lifetime = closing;
    closing->release(); closing.reset();
    expectSession(lifetime.expired() && !bank.hasBank() && !draining.expired() && closingOperation->cancelFlag().load(),
        "owner/view release does not join or destroy an executing value-owned worker");
    gate.store(true); awaitSession([&] { return draining.expired(); });
    expectSession(closingOperation->report().cancelled && delivered.size() == 1,
        "cancelled detached preparation drains without any late GUI delivery");
    expectSession(root.getFileName().startsWith("SingleEditor-BnkSession-") && root.deleteRecursively(),
        "remove only this test's owned private fixture");
    std::cout << "BnkRuntimeSessionTests passed (generation ownership, coalescing, context, stale results, admission and nonblocking release)\n";
}
