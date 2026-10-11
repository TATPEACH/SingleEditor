#pragma once
#include "RecoveryMaintenanceQueue.h"
#include "RecoverySessionHost.h"
#include "RecoveryStorageLayout.h"

namespace singleeditor::recovery
{
// Owner-thread bridge for the native host. Construction/clean PostCore are
// inert. The existing UI scheduler polls only while needsPoll() is true; every
// storage operation remains on an owned writer or maintenance worker.
class RecoveryService final : private SingleTrackDocument::Listener,
                              private DocumentSaveCoordinator::Listener
{
public:
    struct Config final
    {
        juce::File root;
        ServiceStorageBudget storage;
        // Service supplies producer.root/prepareStorage and shares its Store
        // limits with maintenance; the caller supplies the remaining budgets.
        RecoverySessionHost::Config sessions;
        RecoveryMaintenanceQueue::Config maintenance;
        // Native recovery owners may share one data-only result slot.
        std::shared_ptr<RecoveryDiagnosticState> diagnostics;
    };
    struct Health final
    {
        RecoverySessionHost::Health protection;
        RecoveryMaintenanceQueue::Progress maintenance;
        Status dispositionFailure;
        size_t waitingRetirements = 0, releasedEpochs = 0, cancelledPackets = 0;
        size_t completedOwners = 0, removedFiles = 0, removedBytes = 0;
        bool needsPoll = false, settled = false;
    };

    RecoveryService(SingleTrackDocument&, DocumentSaveCoordinator&, Config);
    ~RecoveryService() override;
    Status attachPostCore(const juce::Uuid& entry);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
    { sessions.setOptionalLifecycleContext(context); }
#endif
    void leaveDocument();
    bool needsPoll() const noexcept { return pollRequested; }
    void poll();
    Health health() const;
    const ServiceStorageBudget& configuredStorageBudget() const noexcept { return storageBudget; }
    int64_t configuredOwnerByteLimit() const noexcept { return ownerByteLimit; }
    bool tryDiagnosticFailure(std::optional<RecoveryDiagnosticState::Failure>& result) const
    { return diagnostics->trySnapshot(result); }
    Status retryProtection(const juce::Uuid& owner, bool collectOrphans = false);
    Status retryMaintenance(const juce::Uuid& owner);
    Status resumeTerminalCleanup(const juce::Uuid& owner, const SafeHead&);
    // Native's post-Core presence result, never clean construction/admission.
    Status sweepTerminalOwnersAfterPresence(bool policyChanged = false);
    // Recovery Center's second confirmation, never ordinary close Discard.
    // Review is prepared on a worker; root/capacity refusal keeps caller ownership.
    Status submitConfirmedDiscard(std::unique_ptr<OwnerDiscardReview>&);
    Status cancelDiscard(const juce::Uuid& owner);
    Status checkpointCurrent();
    // Stops observation and drains accepted work. Maintenance admission remains
    // available until destruction, so Cancel can resume without losing jobs.
    void beginClose();
    Status cancelClose(const juce::Uuid& currentEntry);

private:
    static RecoverySessionHost::Config bindStorage(const Config&, const std::shared_ptr<RecoveryDiagnosticState>&);
    static RecoveryMaintenanceQueue::Config bindMaintenance(const Config&, const std::shared_ptr<RecoveryDiagnosticState>&);
    void note(RecoveryDiagnosticState::Stage, const Status&, const juce::Uuid& owner = juce::Uuid(nullptr)) const noexcept;
    void documentChanged(SingleTrackDocument::StatePtr) override { pollRequested = true; }
    void documentIdentityChanged(const media::IdentityEvent&) override { pollRequested = true; }
    void saveCompleted(const SingleTrackDocument::SaveSnapshot&, const DocumentSaveCoordinator::Result&, bool) override
    { pollRequested = true; }

    SingleTrackDocument& document;
    DocumentSaveCoordinator& saves;
    juce::File journalRoot;
    const ServiceStorageBudget storageBudget;
    const int64_t ownerByteLimit;
    std::shared_ptr<RecoveryDiagnosticState> diagnostics;
    RecoverySessionHost sessions;
    RecoveryMaintenanceQueue maintenance;
    // At most Config::sessions.savedTickets: receive the next bounded batch
    // only after every ticket in this one has transferred successfully.
    std::vector<std::unique_ptr<SavedOwnerRetirement>> waiting;
    Status dispositionFailure;
    Status transferFailure;
    size_t releasedCount = 0, cancelledCount = 0, completedCount = 0, removedFiles = 0, removedBytes = 0;
    bool pollRequested = false;
};
}
