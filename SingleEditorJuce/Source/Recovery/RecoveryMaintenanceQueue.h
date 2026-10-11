#pragma once
#include "RecoveryOwnerLifecycle.h"
#include "RecoveryDiagnosticState.h"

namespace singleeditor::recovery
{
// Service-owned, bounded metadata queue. No thread or filesystem access until
// an actual saved ticket, second-confirmed Discard review, explicit resume, or
// post-Core sweep of terminal owners and configured retention is accepted.
// Never borrows Document, Host or UI.
class RecoveryMaintenanceQueue final
{
public:
    enum class Phase { queued, retiring, collecting, completed, failed };
    enum class Disposition { saved, discarded, previouslyRetired, expired };
    struct Config final
    {
        size_t jobs = 32;
        OrphanCollectionBudget batch { 4, 32 * 1024 * 1024 };
        Limits store;
        std::shared_ptr<RecoveryDiagnosticState> diagnostics;
#if SINGLE_EDITOR_RECOVERY_TESTING
        std::function<void(Phase, juce::Uuid)> onBoundary;
        std::function<void(juce::Uuid)> afterRetirementPublished;
        std::function<void()> beforeSweepPage;
#endif
    };
    struct JobStatus final
    {
        juce::Uuid operationId; // Renewed only when an actual retry is accepted.
        juce::File root;
        juce::Uuid owner { nullptr };
        SafeHead head;
        Phase phase = Phase::queued;
        Disposition disposition = Disposition::saved;
        Status failure;
        size_t batches = 0, filesRemoved = 0, bytesRemoved = 0;
        bool preserved = false; // An automatic candidate became ineligible; no retirement permission was issued.
    };
    struct Progress final
    {
        struct Sweep final
        {
            size_t pages = 0, entries = 0, terminalOwners = 0, preservedOwners = 0, expiryCandidates = 0;
            bool active = false, completed = false, cancelled = false;
            Status failure;
        } sweep;
        std::vector<JobStatus> jobs;
        Status failure;
        bool accepting = true, workerRunning = false, settled = false;
    };
    explicit RecoveryMaintenanceQueue(Config);
    ~RecoveryMaintenanceQueue();
    // Rejection preserves caller ownership. Completion/failure still occupies
    // queue capacity until consumed/retried; no ticket is silently evicted.
    Status submit(std::unique_ptr<SavedOwnerRetirement>&);
    // Call only after explicit second confirmation of this selected owner's
    // displayed review. Rejection retains it; Later/Cancel never submit a job.
    Status submitConfirmedDiscard(std::unique_ptr<OwnerDiscardReview>&);
    // Explicitly resume a selected persisted terminal owner after a restart.
    // These are selectors, not retirement authority: collectTerminal rechecks
    // the existing durable marker/intent and exact head on its worker. An empty
    // head requests bounded intent lookup when the old head is already gone.
    Status resumeTerminal(const juce::File& root, const juce::Uuid& owner, const SafeHead&);
    // Post-Core, after a presence hint. Bounded terminal pass, then a configured
    // expiry pass on service roots. Expiry independently reviews the current
    // policy/head and must publish a real retirement before terminal collection.
    // Ordinary repeated calls do not restart a completed sweep or retry failed
    // jobs. Explicit policy change cancels the old cursor and starts a new pass.
    Status sweepTerminals(const juce::File& root, bool policyChanged = false);
    void cancelTerminalSweep();
    // Explicit cancellation before any terminal marker was published. In-flight
    // retirement and published intents cannot be revoked; their jobs stay owned.
    Status cancelDiscard(const juce::File& root, const juce::Uuid& owner);
    Status retry(const juce::File& root, const juce::Uuid& owner);
    Progress progress() const;
    std::vector<JobStatus> takeCompleted();
    // Stop admission and drain accepted jobs. A failed job remains unresolved;
    // destruction is nonblocking and running work owns its detached state.
    void close();
private:
    struct State;
    Status admit(std::unique_ptr<SavedOwnerRetirement>&, std::unique_ptr<OwnerDiscardReview>&);
    static void launch(const std::shared_ptr<State>&);
    static void run(const std::shared_ptr<State>&);
    static void discover(const std::shared_ptr<State>&);
    std::shared_ptr<State> state;
};
}
