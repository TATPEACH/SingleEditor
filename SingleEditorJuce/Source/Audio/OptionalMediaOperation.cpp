#include "Audio/OptionalMediaOperation.h"
#include "Audio/DecodedWaveCache.h"
#include <juce_cryptography/juce_cryptography.h>

namespace singleeditor::media
{
namespace
{
juce::String hash(const juce::String& text)
{ return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString(); }
// Physical stamp on an explicit optional worker, not a full content hash or
// source-write credential. Missing dependencies get a stable failure identity.
juce::String stamp(const juce::File& file)
{
    return hash(file.getFullPathName() + "|" + juce::String(file.getFileIdentifier()) + "|"
        + juce::String(file.getSize()) + "|" + juce::String(file.getLastModificationTime().toMilliseconds()));
}
BoundedDecodeProcess::Limits limitsFor(bool expanded, OptionalMediaFailureState::Module module = OptionalMediaFailureState::Module::videoAudio)
{
    BoundedDecodeProcess::Limits limits;
    limits.wallTimeMs = expanded ? 600000 : 60000;
    if (module == OptionalMediaFailureState::Module::packedAudio || module == OptionalMediaFailureState::Module::bank)
        limits.wallTimeMs = expanded ? 120000 : 30000;
    limits.memoryBytes = (expanded ? 4ULL : 2ULL) * 1024 * 1024 * 1024;
    limits.outputBytes = (expanded ? 4ULL : 1ULL) * 1024 * 1024 * 1024;
    return limits;
}
juce::String policyFor(const BoundedDecodeProcess::Limits& limits, OptionalMediaFailureState::Module module = OptionalMediaFailureState::Module::videoAudio)
{
    return juce::String(module == OptionalMediaFailureState::Module::packedAudio ? "vgmstream-pcm/v2/"
        : module == OptionalMediaFailureState::Module::bank ? "bnk-preview/v1/layer8-step16-pcm256MiB/" : "ffmpeg-pcm16/file-only/v2/")
        + juce::String(limits.wallTimeMs) + "/"
        + juce::String(limits.memoryBytes) + "/" + juce::String(limits.outputBytes) + "/1";
}
}
OptionalMediaOperation::OptionalMediaOperation(std::shared_ptr<State> owner, State::Intent action,
                                               std::optional<State::Key> previous, State::Module module)
    :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
      observedLifetime(currentOptionalLifecycleContext()),
#endif
      session(std::move(owner)), requestModule(module), intent(action), reviewed(std::move(previous))
{
    result.module = module;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    observedLifetime.token.created(); observedLifetime.created = true;
    result.observedResourceId = observedResourceId();
#endif
}
OptionalMediaOperation::~OptionalMediaOperation() { cancelled(); }
BoundedDecodeProcess::Limits OptionalMediaOperation::videoLimits() const
{
    const bool expanded = intent == State::Intent::expandBudget
        || (reviewed && reviewed->profile == State::Profile::expanded);
    return limitsFor(expanded);
}
juce::String OptionalMediaOperation::videoPolicy() const
{
    return policyFor(videoLimits());
}
bool OptionalMediaOperation::admitVideo(const juce::File& source, const juce::File& producer)
{ return admit(source, producer, State::Module::videoAudio); }
bool OptionalMediaOperation::admitPacked(const juce::File& source, const juce::File& producer)
{ return admit(source, producer, State::Module::packedAudio); }
bool OptionalMediaOperation::beginBankPreparation()
{
    const std::scoped_lock lock(mutex);
    if (requestModule != State::Module::bank || !session || ticket || result.terminal) return false;
    if (cancellation.load(std::memory_order_acquire))
    { result.cancelled = result.terminal = true; return false; }
    if (!bankPreparing)
    { bankPreparing = true; admittedAtMs = juce::Time::getMillisecondCounterHiRes(); }
    return true;
}
bool OptionalMediaOperation::admitBank(const juce::File& source, const juce::File& producer, const juce::String& scope)
{ return admit(source, producer, State::Module::bank, scope); }
BoundedDecodeProcess::Limits OptionalMediaOperation::packedLimits() const
{
    return limitsFor(intent == State::Intent::expandBudget || (reviewed && reviewed->profile == State::Profile::expanded), State::Module::packedAudio);
}
juce::String OptionalMediaOperation::packedPolicy() const
{ return policyFor(packedLimits(), State::Module::packedAudio); }
BoundedDecodeProcess::Limits OptionalMediaOperation::bankLimits() const
{
    return limitsFor(intent == State::Intent::expandBudget || (reviewed && reviewed->profile == State::Profile::expanded), State::Module::bank);
}
juce::String OptionalMediaOperation::bankPolicy() const
{ return policyFor(bankLimits(), State::Module::bank); }
BoundedDecodeProcess::Limits OptionalMediaOperation::operationLimits() const
{ return requestModule == State::Module::bank ? bankLimits() : requestModule == State::Module::packedAudio ? packedLimits() : videoLimits(); }
int OptionalMediaOperation::remainingWallTimeMs() const
{
    const std::scoped_lock lock(mutex);
    if (admittedAtMs <= 0 || result.terminal) return 0;
    // A reviewed expansion is not yet authority for resolving a different scope.
    const auto allowed = bankPreparing && !ticket ? limitsFor(false, State::Module::bank).wallTimeMs : operationLimits().wallTimeMs;
    const auto remaining = allowed - (juce::Time::getMillisecondCounterHiRes() - admittedAtMs);
    return static_cast<int>(juce::jmax(0.0, remaining));
}
bool OptionalMediaOperation::continueWork(State::Stage stage)
{
    if (cancellation.load(std::memory_order_acquire)) { cancelled(); return false; }
    const auto current = report();
    if (current.terminal) return false;
    if (!current.key)
    {
        const std::scoped_lock lock(mutex);
        if (!bankPreparing || stage != State::Stage::admission) return false;
    }
    if (remainingWallTimeMs() <= 0) { fail(State::Reason::deadline, stage); return false; }
    return true;
}
bool OptionalMediaOperation::admit(const juce::File& source, const juce::File& producer, State::Module module, const juce::String& scope)
{
    const std::scoped_lock lock(mutex);
    if (result.terminal || ticket) return false;
    if (requestModule != module || (module == State::Module::bank && (scope.isEmpty() || scope.getNumBytesAsUTF8() > 256)))
    { result.denied = State::Denied::invalidKey; result.terminal = true; return false; }
    if (cancellation.load(std::memory_order_acquire))
    { result.terminal = result.cancelled = true; return false; }
    const auto sourceStamp = stamp(source);
    const auto policy = module == State::Module::bank ? bankPolicy() : module == State::Module::packedAudio ? packedPolicy() : videoPolicy();
    State::Key key { scope.isEmpty() ? sourceStamp : hash(sourceStamp + "|" + scope), hash(policy + "|" + stamp(producer)), module,
        (intent == State::Intent::expandBudget || (reviewed && reviewed->profile == State::Profile::expanded))
            ? State::Profile::expanded : State::Profile::conservative };
    result.key = key;
    result.dependencyFingerprint = stamp(producer);
    // Both retry and expansion must refer to the reviewed source/dependency.
    if (!session || (reviewed && (reviewed->sourceFingerprint != key.sourceFingerprint
        || reviewed->module != key.module || (intent != State::Intent::expandBudget
            && reviewed->policyFingerprint != key.policyFingerprint)
            || (intent == State::Intent::expandBudget && reviewed->policyFingerprint
                != hash(policyFor(limitsFor(false, module), module) + "|" + result.dependencyFingerprint)))))
    { result.denied = State::Denied::invalidKey; result.terminal = true; return false; }
    if (!bankPreparing) admittedAtMs = juce::Time::getMillisecondCounterHiRes();
    auto admission = session->begin(key, intent, intent == State::Intent::expandBudget ? reviewed : std::nullopt);
    result.denied = admission.denied;
    ticket = std::move(admission.ticket);
    if (!ticket)
    {
        result.terminal = true;
        const auto previous = session->latestFailure();
        if (previous && !(previous->key < key) && !(key < previous->key)) result.failure = previous->failure;
    }
    if (ticket && juce::Time::getMillisecondCounterHiRes() - admittedAtMs >= operationLimits().wallTimeMs)
    {
        State::Failure failure; failure.reason = State::Reason::deadline; failure.stage = State::Stage::admission;
        failure.operationId = result.operationId;
        failure.source = State::FailureSource::decoder;
        session->fail(*ticket, failure); result.failure = failure; result.terminal = true;
        return false;
    }
    return ticket.has_value();
}
void OptionalMediaOperation::observeProcess(const BoundedDecodeProcess::Result& process)
{ const std::scoped_lock lock(mutex); result.process = process; }
void OptionalMediaOperation::noteCacheLookup(CacheLookup lookup, DecodedWaveCache::LookupReason reason)
{
    const std::scoped_lock lock(mutex);
    result.cache.lastLookup = lookup;
    result.cache.lookupReason = reason;
    result.cache.schema = DecodedWaveCache::schemaVersion;
    if (lookup == CacheLookup::miss || lookup == CacheLookup::reused) ++result.cache.lookups;
    if (lookup == CacheLookup::reused) ++result.cache.hits;
}
void OptionalMediaOperation::noteCachePublication(bool published)
{
    const std::scoped_lock lock(mutex);
    result.cache.schema = DecodedWaveCache::schemaVersion;
    result.cache.lastPublication = published;
    if (published) ++result.cache.publications; else ++result.cache.publicationFailures;
}
void OptionalMediaOperation::noteReader(ReaderKind reader)
{ const std::scoped_lock lock(mutex); result.reader = reader; }
void OptionalMediaOperation::noteProgress(int64_t readyFrames, int64_t totalFrames)
{
    const std::scoped_lock lock(mutex);
    if (result.terminal || totalFrames <= 0 || readyFrames < 0 || readyFrames > totalFrames) return;
    result.readyFrames = readyFrames; result.totalFrames = totalFrames;
}
void OptionalMediaOperation::completeDecodedAudio()
{
    if (cancellation.load(std::memory_order_acquire)) { cancelled(); return; }
    const std::scoped_lock lock(mutex);
    if (result.terminal || !ticket) return;
    result.decoderComplete = true;
    if (result.readerAdmitted) { result.succeeded = session->succeed(*ticket); result.terminal = true; }
}
void OptionalMediaOperation::processFailed(const BoundedDecodeProcess::Result& process)
{
    observeProcess(process);
    if (process.failure == BoundedDecodeProcess::Failure::cancelled) { cancelled(); return; }
    State::Failure failure;
    failure.exitCode = process.exitCode;
    failure.observedBytes = process.observedOutputBytes;
    failure.allowedBytes = operationLimits().outputBytes;
    failure.elapsedMilliseconds = process.elapsedMilliseconds;
    failure.jobCleanupConfirmed = process.jobCleanupConfirmed;
    using F = BoundedDecodeProcess::Failure;
    switch (process.failure)
    {
        case F::deadline: failure.reason = State::Reason::deadline; break;
        case F::outputLimit: failure.reason = State::Reason::outputQuota; break;
        case F::invalidOutput: failure.reason = State::Reason::cacheInvalid; failure.stage = State::Stage::cache; break;
        case F::cleanup: failure.reason = State::Reason::containment; failure.stage = State::Stage::cleanup; break;
        case F::containment: failure.reason = State::Reason::containment; failure.stage = State::Stage::launch; break;
        case F::invalidRequest: case F::unsupportedPlatform: case F::launch:
            failure.reason = State::Reason::ioFailure; failure.stage = State::Stage::launch; break;
        default: failure.reason = State::Reason::decoderExit; break;
    }
    const std::scoped_lock lock(mutex);
    if (result.terminal || !ticket) return;
    failure.operationId = result.operationId;
    failure.source = State::FailureSource::decoder;
    session->fail(*ticket, failure); result.failure = failure; result.terminal = true;
}
void OptionalMediaOperation::fail(State::Reason reason, State::Stage stage)
{
    if (cancellation.load(std::memory_order_acquire)) { cancelled(); return; }
    const std::scoped_lock lock(mutex);
    if (result.terminal || (!ticket && (!bankPreparing || stage != State::Stage::admission))) return;
    State::Failure failure; failure.reason = reason; failure.stage = stage;
    failure.operationId = result.operationId;
    failure.source = State::FailureSource::decoder;
    if (ticket) session->fail(*ticket, failure);
    else if (session) session->noteUnadmittedFailure(failure);
    result.failure = failure; result.terminal = true;
}
void OptionalMediaOperation::succeed()
{
    if (cancellation.load(std::memory_order_acquire)) { cancelled(); return; }
    const std::scoped_lock lock(mutex);
    if (result.terminal || !ticket) return;
    result.readerAdmitted = true;
    if (requestModule == State::Module::videoAudio) result.reader = ReaderKind::videoCache;
    if (requestModule == State::Module::packedAudio && !result.decoderComplete) return;
    result.succeeded = session->succeed(*ticket); result.terminal = true;
}
void OptionalMediaOperation::cancelled()
{
    const std::scoped_lock lock(mutex);
    if (result.terminal) return;
    if (ticket && session) session->cancel(*ticket);
    result.cancelled = result.terminal = true;
}
OptionalMediaOperation::Report OptionalMediaOperation::report() const
{ const std::scoped_lock lock(mutex); return result; }
std::optional<OptionalMediaOperation::Report> OptionalMediaOperation::DiagnosticState::snapshot() const
{
    const std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    return value;
}
OptionalMediaOperation::State::View OptionalMediaOperation::policyView() const
{ const auto r = report(); return session && r.key ? session->inspect(*r.key) : State::View {}; }
}
