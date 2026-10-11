#include "Bnk/BnkRuntimeSession.h"
#include <thread>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor
{
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
namespace
{
std::atomic<int> sessionFactoryRequests { 0 }, sessionLiveInstances { 0 };
}
#endif
struct BnkRuntimeSession::Work final
{
    PreviewIntent intent;
    BnkPreparedPlayback prepared;
    std::atomic<bool> finished { false };
    bool handled = false;
};

BnkRuntimeSession::BnkRuntimeSession(BnkDocument& bank, uint64_t generation)
    : document(bank), bankLease(bank.getState()), documentGeneration(generation)
{
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    ++sessionLiveInstances;
#endif
}

BnkRuntimeSession::Ptr BnkRuntimeSession::create(BnkDocument& bank, uint64_t generation)
{
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    ++sessionFactoryRequests;
#endif
    if (bank.getBankInfo().file == juce::File()) return {};
    return Ptr(new BnkRuntimeSession(bank, generation));
}

BnkRuntimeSession::~BnkRuntimeSession()
{
    release();
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    --sessionLiveInstances;
#endif
}

void BnkRuntimeSession::bind(uint64_t generation)
{
    if (released) return;
    if (generation != documentGeneration)
    {
        cancel(); active.reset();
        selectedContext = {}; switches.clear(); states.clear();
        documentGeneration = generation;
    }
    bankLease = document.getState();
}

void BnkRuntimeSession::release()
{
    if (released) return;
    released = true;
    cancel(); active.reset();
    selectedContext = {}; switches.clear(); states.clear();
    // Clear even a moved source, but never a newer bank installed by another
    // owner. An in-flight worker retains only its frozen request until drained.
    if (bankLease && document.getState() == bankLease) document.clear();
    bankLease.reset();
}

void BnkRuntimeSession::setContext(BnkPlaybackContext value, std::optional<uint32_t> event)
{
    if (released) return;
    selectedContext = std::move(value);
    if (!event) return;
    if (selectedContext.switchValueToken.isNotEmpty()) switches[*event] = selectedContext.switchValueToken;
    if (selectedContext.stateValueToken.isNotEmpty()) states[*event] = selectedContext.stateValueToken;
}

juce::String BnkRuntimeSession::preferredSwitch(std::optional<uint32_t> event) const
{
    const auto found = event ? switches.find(*event) : switches.end();
    return found == switches.end() ? selectedContext.switchValueToken : found->second;
}

juce::String BnkRuntimeSession::preferredState(std::optional<uint32_t> event) const
{
    const auto found = event ? states.find(*event) : states.end();
    return found == states.end() ? selectedContext.stateValueToken : found->second;
}

bool BnkRuntimeSession::selectionMatches(const PreviewIntent& intent, BnkMediaResolveMode mode) const
{
    if (released || !intent.request || document.getState() != intent.request->bankState()
        || document.getSelectedEventId() != intent.event || document.getSelectedMediaId() != intent.media
        || mode != intent.request->resolveMode()) return false;
    if (intent.request->mediaId()) return true;
    const auto& expected = intent.request->playbackContext();
    return selectedContext.switchValueToken == expected.switchValueToken
        && selectedContext.stateValueToken == expected.stateValueToken
        && selectedContext.rtpcToken == expected.rtpcToken && selectedContext.rtpcValue == expected.rtpcValue;
}

void BnkRuntimeSession::queue(PreviewIntent intent)
{
    if (released) { intent.operation->cancelled(); return; }
    if (queued) queued->operation->cancelled();
    if (active && !active->finished.load(std::memory_order_acquire)) active->intent.operation->requestCancel();
    queued = std::move(intent);
}

bool BnkRuntimeSession::cancel(int64_t currentToken)
{
    const bool owned = (queued && queued->token == currentToken) || (active && active->intent.token == currentToken);
    if (queued) { queued->operation->cancelled(); queued.reset(); }
    if (active)
    {
        active->intent.operation->requestCancel();
        if (active->finished.load(std::memory_order_acquire))
        { active->intent.operation->cancelled(); active.reset(); }
    }
    return owned;
}

bool BnkRuntimeSession::isPreparing() const noexcept
{
    return queued.has_value() || (active && !active->finished.load(std::memory_order_acquire));
}

const BnkRuntimeSession::PreviewIntent* BnkRuntimeSession::lastPreview() const noexcept
{ return active ? &active->intent : nullptr; }

void BnkRuntimeSession::poll(const Admission& admit, const Delivery& deliver)
{
    if (released) return;
    if (active && !active->handled)
    {
        const auto work = active;
        if (!admit(work->intent, false)) work->intent.operation->requestCancel();
        if (!work->finished.load(std::memory_order_acquire)) return;
        work->handled = true;
        // The local Work also protects this value across a re-entrant GUI action
        // which replaces/releases the active session during delivery.
        deliver(work->intent, std::move(work->prepared), true);
        work->intent.previousWorkspace.reset(); work->intent.previousState.reset();
        if (released) return;
    }
    if (!queued || (active && !active->finished.load(std::memory_order_acquire))) return;
    auto intent = std::move(*queued); queued.reset();
    if (intent.operation->cancelFlag().load(std::memory_order_acquire) || !admit(intent, true))
    {
        intent.operation->cancelled();
        deliver(intent, {}, false);
        return;
    }
    auto work = std::make_shared<Work>();
    work->intent = std::move(intent); active = work;
    try
    {
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
        const auto prepare = prepareForTesting;
        std::thread([work, prepare]
#else
        std::thread([work]
#endif
        {
#if JUCE_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            try
            {
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
                if (prepare) work->prepared = prepare(work->intent.request, work->intent.operation);
                else
#endif
                    work->prepared = work->intent.request->prepare(work->intent.operation);
            }
            catch (...)
            {
                using S = media::OptionalMediaFailureState;
                if (!work->intent.operation->report().key) work->intent.operation->beginBankPreparation();
                work->intent.operation->fail(S::Reason::ioFailure, S::Stage::admission);
            }
            // Release can remove the UI owner while this value-owned job is
            // finishing. Its cancellation must still acquire a terminal report.
            if (work->intent.operation->cancelFlag().load(std::memory_order_acquire)
                && !work->intent.operation->report().terminal) work->intent.operation->cancelled();
            work->finished.store(true, std::memory_order_release);
        }).detach();
    }
    catch (...)
    {
        using S = media::OptionalMediaFailureState;
        work->intent.operation->beginBankPreparation();
        work->intent.operation->fail(S::Reason::ioFailure, S::Stage::admission);
        work->finished.store(true, std::memory_order_release);
    }
}

#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
int BnkRuntimeSession::factoryRequestsForTesting() noexcept { return sessionFactoryRequests.load(); }
int BnkRuntimeSession::liveInstancesForTesting() noexcept { return sessionLiveInstances.load(); }
BnkRuntimeSession::Ptr BnkRuntimeSession::createForTesting(BnkDocument& bank, uint64_t generation, Prepare prepare)
{
    auto result = create(bank, generation);
    if (result) result->prepareForTesting = std::move(prepare);
    return result;
}
std::weak_ptr<const void> BnkRuntimeSession::workerForTesting() const
{ return active ? std::weak_ptr<const void>(active) : std::weak_ptr<const void>(); }
bool BnkRuntimeSession::workerFinishedForTesting() const
{ return active && active->finished.load(std::memory_order_acquire); }
#endif
}
