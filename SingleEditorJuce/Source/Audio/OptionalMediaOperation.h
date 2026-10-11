#pragma once
#include "Audio/OptionalMediaFailureState.h"
#include "Audio/BoundedDecodeProcess.h"
#include "Audio/DecodedWaveCache.h"
#include <memory>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

namespace singleeditor::media
{
// Value-owned optional operation. Main owns the session policy lazily; worker
// requests retain it independently. Never instantiated for ordinary audio.
class OptionalMediaOperation final
{
public:
    using State = OptionalMediaFailureState;
    using Ptr = std::shared_ptr<OptionalMediaOperation>;
    enum class CacheLookup { notObserved, unavailable, miss, reused };
    enum class ReaderKind { notReported, videoCache, packedProgressiveWave, bankDecodedWave };
    struct CacheReport final
    {
        CacheLookup lastLookup = CacheLookup::notObserved;
        DecodedWaveCache::LookupReason lookupReason = DecodedWaveCache::LookupReason::notObserved;
        int schema = 0;
        uint32_t lookups = 0, hits = 0, publications = 0, publicationFailures = 0;
        std::optional<bool> lastPublication;
    };
    struct Report final
    {
        juce::Uuid operationId;
        State::Module module = State::Module::videoAudio;
        std::optional<State::Key> key;
        juce::String dependencyFingerprint;
        std::optional<State::Failure> failure;
        std::optional<BoundedDecodeProcess::Result> process;
        State::Denied denied = State::Denied::none;
        bool terminal = false, succeeded = false, cancelled = false;
        bool decoderComplete = false, readerAdmitted = false;
        int64_t readyFrames = 0, totalFrames = 0;
        CacheReport cache;
        ReaderKind reader = ReaderKind::notReported;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        // Correlates the existing operation UUID with the non-owning channel.
        std::uint64_t observedResourceId = 0;
#endif
    };
    // A read-only observation handle retains bounded values, not the operation,
    // session policy, source reader or worker. Observation must not delay teardown.
    class DiagnosticState final
    {
    public:
        std::optional<Report> snapshot() const;
    private:
        friend class OptionalMediaOperation;
        mutable std::mutex mutex;
        Report value;
    };
    using DiagnosticHandle = std::shared_ptr<const DiagnosticState>;
    DiagnosticHandle diagnosticHandle() const noexcept { return diagnosticState; }
    OptionalMediaOperation(std::shared_ptr<State>, State::Intent = State::Intent::initial,
                           std::optional<State::Key> reviewed = {}, State::Module = State::Module::videoAudio);
    ~OptionalMediaOperation();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext observedContext() const noexcept { return observedLifetime.token.creationContext(); }
    std::uint64_t observedResourceId() const noexcept { return observedLifetime.token.resourceId(); }
#endif
    bool admitVideo(const juce::File& source, const juce::File& producer);
    bool admitPacked(const juce::File& source, const juce::File& producer);
    // Starts only bounded bank source resolution on its worker. No decoder or
    // retry permission exists until admitBank binds the actual resolved scope.
    bool beginBankPreparation();
    // Explicit worker admission for one frozen bank selection and its resolved
    // dependency identities. The scope contains no mutable UI object/pointer.
    bool admitBank(const juce::File& source, const juce::File& producer, const juce::String& scopeFingerprint);
    BoundedDecodeProcess::Limits videoLimits() const;
    juce::String videoPolicy() const;
    BoundedDecodeProcess::Limits packedLimits() const;
    juce::String packedPolicy() const;
    BoundedDecodeProcess::Limits bankLimits() const;
    juce::String bankPolicy() const;
    BoundedDecodeProcess::Limits operationLimits() const;
    int remainingWallTimeMs() const;
    bool continueWork(State::Stage);
    State::Module module() const noexcept { return requestModule; }
    void noteProgress(int64_t readyFrames, int64_t totalFrames);
    void completeDecodedAudio();
    void processFailed(const BoundedDecodeProcess::Result&);
    void observeProcess(const BoundedDecodeProcess::Result&);
    void noteCacheLookup(CacheLookup, DecodedWaveCache::LookupReason);
    void noteCachePublication(bool);
    void noteReader(ReaderKind);
    void fail(State::Reason, State::Stage);
    void succeed();
    void requestCancel() noexcept { cancellation.store(true, std::memory_order_release); }
    void cancelled();
    const std::atomic<bool>& cancelFlag() const noexcept { return cancellation; }
    Report report() const;
    State::View policyView() const;
private:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedLifetime final
    {
        explicit ObservedLifetime(OptionalLifecycleContext context) noexcept
            : token(OptionalLifecycleToken::request(context, OptionalLifecycleOwner::optionalMedia,
                OptionalLifecycleResourceKind::optionalMediaOperation)) {}
        ~ObservedLifetime() { if (created) token.release(); else token.failed(); }
        OptionalLifecycleToken token;
        bool created = false;
    } observedLifetime; // Last member destroyed, after policy/captures/diagnostic ownership.
#endif
    bool admit(const juce::File&, const juce::File&, State::Module, const juce::String& scopeFingerprint = {});
    std::shared_ptr<State> session;
    const State::Module requestModule;
    State::Intent intent;
    std::optional<State::Key> reviewed;
    std::shared_ptr<DiagnosticState> diagnosticState = std::make_shared<DiagnosticState>();
    // Existing operation state and diagnostic observation share one authority.
    std::mutex& mutex = diagnosticState->mutex;
    Report& result = diagnosticState->value;
    std::optional<State::Ticket> ticket;
    double admittedAtMs = 0;
    bool bankPreparing = false;
    std::atomic<bool> cancellation { false };
};
}
