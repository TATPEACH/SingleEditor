#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "Recovery/RecoveryDiscovery.h"
#include "Recovery/RecoveryRestoreQueue.h"
#include "Recovery/RecoveryInspection.h"
#include "Recovery/RecoveryStorageLayout.h"
#include <atomic>
#include <thread>

namespace singleeditor
{
// Created only by the user's explicit Recovery Center command. Read-only work
// has its own bounded worker; confirmed disposal transfers to RecoveryService.
class RecoveryCenterComponent final : public juce::Component
{
public:
    using Submit = std::function<recovery::Status(std::unique_ptr<recovery::OwnerDiscardReview>&)>;
    struct StorageSettings final { recovery::ServiceStorageBudget service; int64_t ownerBytes = 0; };
    struct RestoreActions final
    {
        juce::File attemptRoot;
        juce::String appBuild;
        std::shared_ptr<recovery::RecoveryDiagnosticState> diagnostics;
        std::function<void()> showDiagnostics;
        std::function<std::optional<StorageSettings>()> storageSettings;
        std::function<void()> retentionChanged;
        std::function<recovery::Status(recovery::RecoveryRestoreQueue::Request, juce::Uuid&)> start;
        std::function<recovery::RecoveryRestoreQueue::Health()> health;
        std::function<recovery::Status(juce::Uuid)> cancel, forget;
        std::function<recovery::Status(juce::Uuid, juce::Uuid&)> retry;
    };
    RecoveryCenterComponent(juce::File journalRoot, Submit, std::function<void()> retry, RestoreActions);
    ~RecoveryCenterComponent() override;
    std::function<void()> onLater;
    std::function<recovery::Status(juce::Uuid, const recovery::SafeHead&)> onResumeCleanup;
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    void setStorageReadBoundaryForTesting(std::function<void()> value) { beforeStorageRead = std::move(value); }
#endif
    void resized() override;
    void updateRestoreJobs();
    juce::Uuid selectedRestoreAttempt() const
    {
        const auto index = jobs.getSelectedItemIndex();
        return index >= 0 && static_cast<size_t>(index) < restoreHealth.jobs.size()
            ? restoreHealth.jobs[static_cast<size_t>(index)].attempt : juce::Uuid(nullptr);
    }
    std::optional<recovery::RecoveryInspection::DiagnosticObservation> diagnosticObservation() const
    {
        if (working || !inspection || inspection->operationId.isNull()) return {};
        return inspection->diagnosticObservation(restoreActions.appBuild);
    }
private:
    void readPage(bool restart);
    void toggleStorage();
    void readStorage();
    void showStorageUsage();
    void chooseRetention();
    void writeRetention(int days, juce::String expectedHash);
    void selected();
    void reviewSelected(int64_t retentionMilliseconds = 0);
    void chooseExpiry();
    void confirmDiscard();
    void retryCleanup();
    void busy(bool);
    void joinWorker();
    void restoreSelected();
    void submitRestore(recovery::RecoveryRestoreQueue::Request);
    void inspectSelected(recovery::RecoveryInspection::SourceAction, bool verifyStoredAudio = false);
    void showInspection();
    void locateSource();
    void exportDiagnostics();
    void selectedJob();
    void actOnJob(int action);
    juce::File root;
    Submit submit;
    std::function<void()> retry;
    RestoreActions restoreActions;
    juce::ComboBox owners;
    juce::TextEditor details;
    juce::Label heading;
    juce::TextButton storageButton { "Storage" }, refreshStorage { "Refresh usage" }, cancelStorage { "Cancel scan" },
                     retentionButton { "Retention..." };
    juce::TextEditor storageDetails;
    std::shared_ptr<recovery::RecoveryStorageLayout::Usage> storageUsage;
    std::optional<StorageSettings> configuredStorage;
    std::optional<recovery::RecoveryStorageLayout::RetentionPolicy> retentionPolicy;
    recovery::Status retentionStatus;
    bool storageView = false, retentionKnown = false, writingRetention = false;
    juce::TextButton later { "Later" };
    juce::TextButton refresh { "Refresh" }, next { "Next page" }, reviewButton { "Review Discard" },
                     discard { "Discard..." }, retryButton { "Retry cleanup" };
    juce::TextButton expiryButton { "Review expiry..." };
    juce::TextButton restoreButton { "Restore Copy" }, cancelJob { "Cancel restore" }, retryJob { "Retry restore" }, forgetJob { "Clear finished task" }, jobDiagnostics { "Task diagnostics..." };
    juce::Label jobHeading;
    juce::ComboBox jobs;
    juce::TextEditor jobDetails;
    recovery::RecoveryRestoreQueue::Health restoreHealth;
    juce::ComboBox safePoints;
    juce::TextButton inspectButton { "Safe Inspect" }, checkSource { "Check original" }, locateButton { "Locate original..." },
                     deepVerify { "Deep Verify source" }, verifyAudio { "Deep Verify audio" }, cancelInspection { "Cancel inspection" }, exportButton { "Export diagnostics..." };
    std::shared_ptr<recovery::RecoveryInspection::Report> inspection;
    recovery::RecoverySourceBinding::Request sourceReview;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<recovery::RecoveryDiscovery> discovery;
    recovery::RecoveryDiscovery::Page page;
    std::unique_ptr<recovery::OwnerDiscardReview> review;
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    bool working = false;
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    std::function<void()> beforeStorageRead;
#endif
};
}
