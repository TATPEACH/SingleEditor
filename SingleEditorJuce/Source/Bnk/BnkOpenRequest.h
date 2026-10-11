#pragma once
#include "Bnk/BnkParser.h"
#include "Audio/OptionalMediaFailureState.h"
#include <atomic>

namespace singleeditor
{
// Explicit optional worker request. No UI pointer, process singleton, decoder,
// timer or scan is created by construction. One prepare call per request.
class BnkOpenRequest final
{
public:
    using Ptr = std::shared_ptr<BnkOpenRequest>;
    using Policy = media::OptionalMediaFailureState;
    enum class Capability { core, sidecar };
    struct Report final
    {
        std::optional<Policy::Key> key;
        Policy::Denied denied = Policy::Denied::none;
        BnkParser::Failure failure = BnkParser::Failure::none;
        Policy::Stage stage = Policy::Stage::admission;
        bool terminal = false, succeeded = false, circuitOpen = false;
    };
    struct DiagnosticReport final
    {
        juce::Uuid operationId;
        Report core, sidecar;
        bool started = false, cancelled = false, abandoned = false;
    };
    class DiagnosticState final
    {
    public:
        std::optional<DiagnosticReport> snapshot() const;
    private:
        friend class BnkOpenRequest;
        mutable std::mutex mutex;
        DiagnosticReport value;
        std::atomic<bool> cancellation { false }, started { false };
    };
    using DiagnosticHandle = std::shared_ptr<const DiagnosticState>;
    DiagnosticHandle diagnosticHandle() const noexcept { return diagnosticState; }
    BnkOpenRequest(std::shared_ptr<Policy>, juce::File bank, juce::File projectRoot = {},
        Capability = Capability::core, Policy::Intent = Policy::Intent::initial, std::optional<Policy::Key> reviewed = {});
    ~BnkOpenRequest();
    BnkParser::Result prepare(BnkParser::Checkpoint = {});
    void requestCancel() noexcept { cancellation.store(true, std::memory_order_release); }
    bool wasCancelled() const noexcept { return cancellation.load(std::memory_order_acquire); }
    // Caller checks under the held inputs, adopts the state, then succeeds.
    bool canPublish(const BnkParser::Result&) const;
    void failPublication();
    void succeed();
    // Settled-worker methods; requestCancel is the concurrent cancellation API.
    void abandon();
    Report report(Capability) const;
    Policy::View policyView(Capability) const;
    const juce::File& file() const noexcept { return bank; }
    const juce::File& projectRoot() const noexcept { return root; }
private:
    struct Slot final { Report& report; std::optional<Policy::Ticket> ticket; };
    Slot& slot(Capability);
    const Slot& slot(Capability) const;
    BnkParser::Profile profile(Capability) const;
    juce::String policy(Capability, BnkParser::Profile) const;
    bool admit(Capability, const juce::String& fingerprint);
    void failed(Capability, BnkParser::Failure, Policy::Stage);
    std::shared_ptr<Policy> session;
    const juce::File bank, root;
    const Capability target;
    const Policy::Intent intent;
    const std::optional<Policy::Key> reviewed;
    // One result authority. Observation retains values, not inputs or work.
    std::shared_ptr<DiagnosticState> diagnosticState = std::make_shared<DiagnosticState>();
    std::mutex& mutex = diagnosticState->mutex;
    Slot core { diagnosticState->value.core, {} }, sidecar { diagnosticState->value.sidecar, {} };
    std::shared_ptr<const BoundedSidecarInput::Catalog> catalog;
    std::weak_ptr<const BnkDocument::State> preparedState;
    std::atomic<bool>& cancellation = diagnosticState->cancellation;
    std::atomic<bool>& started = diagnosticState->started;
};
}
