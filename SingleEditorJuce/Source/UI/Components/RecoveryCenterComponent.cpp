#include "RecoveryCenterComponent.h"
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor
{
namespace
{
juce::String timeLabel(int64_t milliseconds)
{ return milliseconds > 0 ? juce::Time(milliseconds).formatted("%Y-%m-%d %H:%M:%S") : "Unavailable"; }
bool terminal(recovery::RecoveryAttemptStage stage)
{ return stage == recovery::RecoveryAttemptStage::ready || stage == recovery::RecoveryAttemptStage::failed || stage == recovery::RecoveryAttemptStage::cancelled; }
juce::String confidenceLabel(recovery::SourceConfidence confidence)
{
    using Confidence = recovery::SourceConfidence;
    switch (confidence)
    {
        case Confidence::exact: return "Exact: original identity and sampled bytes match.";
        case Confidence::relocated: return "Relocated: matching file at a different path; confirmation is required.";
        case Confidence::changed: return "Changed: this file conflicts with the recorded original. Restore Copy keeps it separate.";
        case Confidence::missing: return "Missing: locate the original, or restore the self-contained copy.";
        case Confidence::ambiguous: return "Ambiguous: insufficient original evidence. Restore Copy remains separate and requires Save As.";
    }
    return "Source identity unavailable.";
}
juce::String stageLabel(const recovery::RecoveryRestoreQueue::JobStatus& job)
{
    using Stage = recovery::RecoveryAttemptStage;
    if (job.waitingForAdmission) return "Adding recovered copy to playlist";
    switch (job.stage)
    {
        case Stage::queued: return "Queued";
        case Stage::validating: return "Checking recovery history";
        case Stage::resolvingSource: return "Checking original file";
        case Stage::replayingCheckpoint: return "Preparing audio and history";
        case Stage::preparingCore: return "Preparing first waveform and playback";
        case Stage::coreReady: return "First waveform and playback ready";
        case Stage::materializingFull: return "Copy added; preparing remaining audio and history";
        case Stage::ready: return "Ready; full audio and history available";
        case Stage::failed: return "Restore failed";
        case Stage::cancelled: return "Restore cancelled";
    }
    return "Unknown restore stage";
}
}
RecoveryCenterComponent::RecoveryCenterComponent(juce::File journalRoot, Submit submitToUse, std::function<void()> retryToUse, RestoreActions restoreToUse)
    : root(std::move(journalRoot)), submit(std::move(submitToUse)), retry(std::move(retryToUse)), restoreActions(std::move(restoreToUse))
{
    setName("Recovery Center");
    heading.setText("Recovery Center", juce::dontSendNotification);
    later.setTooltip("Close Recovery Center and keep recovery records. Restore and cleanup tasks continue in the background.");
    later.onClick = [this] { if (onLater) onLater(); };
    owners.setName("Recovery records"); owners.setTextWhenNothingSelected("No record selected");
    details.setName("Recovery record details"); details.setMultiLine(true); details.setReadOnly(true);
    details.setScrollbarsShown(true); details.setCaretVisible(false);
    storageDetails.setName("Recovery storage details"); storageDetails.setMultiLine(true); storageDetails.setReadOnly(true);
    storageDetails.setScrollbarsShown(true); storageDetails.setCaretVisible(false);
    storageButton.setTooltip("View recovery storage usage and limits. Scans only when requested.");
    storageButton.onClick = [this] { toggleStorage(); };
    refreshStorage.onClick = [this] { readStorage(); };
    retentionButton.onClick = [this] { chooseRetention(); };
    retentionButton.setTooltip("Choose how long to keep inactive crash recovery records before automatic cleanup.");
    cancelStorage.onClick = [this]
    { cancelled->store(true); cancelStorage.setEnabled(false); storageDetails.setText("Stopping the storage scan. Recovery files are preserved."); };
    jobHeading.setText("Restore tasks", juce::dontSendNotification);
    jobs.setName("Recovery restore tasks"); jobs.setTextWhenNothingSelected("No restore task selected");
    jobDetails.setName("Recovery restore task details"); jobDetails.setMultiLine(true); jobDetails.setReadOnly(true);
    jobDetails.setScrollbarsShown(true); jobDetails.setCaretVisible(false);
    safePoints.setName("Recovery safe points"); safePoints.setTextWhenNothingSelected("Safe Inspect lists verified history points");
    inspectButton.setTooltip("Read recovery metadata and attempt history. Does not load audio or start a restore.");
    deepVerify.setTooltip("Explicitly hash the selected source file. Does not verify stored recovery audio.");
    verifyAudio.setTooltip("Verify stored audio for the selected point and all its Undo/Redo history. Does not create a recovered copy or play audio.");
    exportButton.setTooltip("Review the selected inspection in Local Diagnostics, then save the approved recovery section.");
    for (auto* component : std::initializer_list<juce::Component*>{ &heading, &later, &storageButton, &storageDetails, &refreshStorage, &cancelStorage, &retentionButton,
        &owners, &details, &refresh, &next, &reviewButton, &discard, &retryButton, &expiryButton,
        &restoreButton, &jobHeading, &jobs, &jobDetails, &cancelJob, &retryJob, &forgetJob, &jobDiagnostics, &safePoints,
        &inspectButton, &checkSource, &locateButton, &deepVerify, &verifyAudio, &cancelInspection, &exportButton })
        addAndMakeVisible(component);
    owners.onChange = [this] { selected(); };
    refresh.onClick = [this] { readPage(true); };
    next.onClick = [this] { readPage(false); };
    reviewButton.onClick = [this] { reviewSelected(); };
    expiryButton.onClick = [this] { chooseExpiry(); };
    expiryButton.setTooltip("Review this record against an age you choose. Active records stay protected; cleanup still needs confirmation.");
    discard.onClick = [this] { confirmDiscard(); };
    retryButton.onClick = [this] { retryCleanup(); };
    restoreButton.onClick = [this] { restoreSelected(); };
    inspectButton.onClick = [this] { inspectSelected(recovery::RecoveryInspection::SourceAction::none); };
    safePoints.onChange = [this] { sourceReview = {}; inspectSelected(recovery::RecoveryInspection::SourceAction::none); };
    checkSource.onClick = [this] { inspectSelected(recovery::RecoveryInspection::SourceAction::locateOrCheck); };
    locateButton.onClick = [this] { locateSource(); };
    deepVerify.onClick = [this] { inspectSelected(recovery::RecoveryInspection::SourceAction::deepVerify); };
    verifyAudio.onClick = [this] { inspectSelected(recovery::RecoveryInspection::SourceAction::none, true); };
    cancelInspection.onClick = [this] { cancelled->store(true); cancelInspection.setEnabled(false); details.setText("Stopping inspection at the next bounded read. The recovery record is preserved."); };
    exportButton.onClick = [this] { exportDiagnostics(); };
    jobs.onChange = [this] { selectedJob(); };
    jobDiagnostics.onClick = [this]
    { if (!selectedRestoreAttempt().isNull() && restoreActions.showDiagnostics) restoreActions.showDiagnostics(); };
    cancelJob.onClick = [this] { actOnJob(0); }; retryJob.onClick = [this] { actOnJob(1); }; forgetJob.onClick = [this] { actOnJob(2); };
    setSize(780, 660);
    updateRestoreJobs();
    readPage(true);
}
RecoveryCenterComponent::~RecoveryCenterComponent()
{
    cancelled->store(true);
    // The worker has value/shared ownership of its root, cursor and token;
    // no UI object remains borrowed while an OS read is still in progress.
    if (worker.joinable()) worker.detach();
}
void RecoveryCenterComponent::joinWorker() { if (worker.joinable()) worker.join(); }
void RecoveryCenterComponent::resized()
{
    auto area = getLocalBounds().reduced(18);
    auto tasks = area.removeFromBottom(208);
    jobHeading.setBounds(tasks.removeFromTop(28)); jobs.setBounds(tasks.removeFromTop(30)); tasks.removeFromTop(6);
    auto taskActions = tasks.removeFromBottom(34);
    cancelJob.setBounds(taskActions.removeFromLeft(135).reduced(2)); retryJob.setBounds(taskActions.removeFromLeft(130).reduced(2));
    forgetJob.setBounds(taskActions.removeFromLeft(170).reduced(2));
    jobDiagnostics.setBounds(taskActions.removeFromLeft(170).reduced(2)); tasks.removeFromBottom(6); jobDetails.setBounds(tasks);
    area.removeFromBottom(14);
    auto title = area.removeFromTop(32);
    later.setBounds(title.removeFromRight(90).reduced(2)); storageButton.setBounds(title.removeFromRight(100).reduced(2)); heading.setBounds(title);
    for (auto* component : std::initializer_list<juce::Component*>{ &owners, &details, &refresh, &next, &reviewButton, &discard,
        &retryButton, &restoreButton, &safePoints, &inspectButton, &checkSource, &locateButton, &deepVerify, &verifyAudio, &expiryButton,
        &cancelInspection, &exportButton }) component->setVisible(!storageView);
    storageDetails.setVisible(storageView); refreshStorage.setVisible(storageView); cancelStorage.setVisible(storageView);
    retentionButton.setVisible(storageView);
    if (storageView)
    {
        auto actions = area.removeFromBottom(34);
        refreshStorage.setBounds(actions.removeFromLeft(145).reduced(2)); cancelStorage.setBounds(actions.removeFromLeft(145).reduced(2));
        retentionButton.setBounds(actions.removeFromLeft(145).reduced(2));
        area.removeFromBottom(8); storageDetails.setBounds(area); return;
    }
    owners.setBounds(area.removeFromTop(32)); area.removeFromTop(10);
    auto actions = area.removeFromBottom(34);
    restoreButton.setBounds(actions.removeFromLeft(120).reduced(2));
    refresh.setBounds(actions.removeFromLeft(75).reduced(2)); next.setBounds(actions.removeFromLeft(90).reduced(2));
    reviewButton.setBounds(actions.removeFromLeft(130).reduced(2)); discard.setBounds(actions.removeFromLeft(100).reduced(2));
    retryButton.setBounds(actions.removeFromLeft(125).reduced(2));
    auto audioActions = area.removeFromBottom(34);
    verifyAudio.setBounds(audioActions.removeFromLeft(165).reduced(2)); exportButton.setBounds(audioActions.removeFromLeft(180).reduced(2));
    expiryButton.setBounds(audioActions.removeFromLeft(150).reduced(2));
    auto sourceActions = area.removeFromBottom(34);
    checkSource.setBounds(sourceActions.removeFromLeft(130).reduced(2)); locateButton.setBounds(sourceActions.removeFromLeft(155).reduced(2));
    deepVerify.setBounds(sourceActions.removeFromLeft(165).reduced(2));
    auto inspectionActions = area.removeFromBottom(34);
    inspectButton.setBounds(inspectionActions.removeFromLeft(120).reduced(2)); cancelInspection.setBounds(inspectionActions.removeFromRight(155).reduced(2));
    safePoints.setBounds(inspectionActions.reduced(4, 2));
    area.removeFromBottom(10); details.setBounds(area);
}
void RecoveryCenterComponent::busy(bool value)
{
    working = value;
    storageButton.setEnabled(!value); refreshStorage.setEnabled(!value); cancelStorage.setEnabled(value && storageView && !writingRetention);
    retentionButton.setEnabled(!value && retentionKnown && retentionStatus && configuredStorage.has_value());
    refresh.setEnabled(!value); owners.setEnabled(!value);
    next.setEnabled(!value && page.more); retryButton.setEnabled(!value);
    const int index = owners.getSelectedItemIndex();
    const bool retired = index >= 0 && static_cast<size_t>(index) < page.rows.size()
        && ((page.rows[static_cast<size_t>(index)].summary.status && page.rows[static_cast<size_t>(index)].summary.retired)
            || page.rows[static_cast<size_t>(index)].summary.status.code == "owner_collecting");
    retryButton.setButtonText(retired && onResumeCleanup ? "Resume cleanup" : "Retry cleanup");
    retryButton.setTooltip(retired && onResumeCleanup
        ? "Continue cleanup already authorized by this record's saved or discarded marker. Does not discard a recoverable record."
        : "Retry unfinished cleanup jobs in this session.");
    const bool usable = index >= 0 && static_cast<size_t>(index) < page.rows.size()
        && page.rows[static_cast<size_t>(index)].summary.status
        && !page.rows[static_cast<size_t>(index)].summary.retired
        && page.rows[static_cast<size_t>(index)].summary.head.sequence > 0;
    reviewButton.setEnabled(!value && usable); discard.setEnabled(!value && review != nullptr);
    expiryButton.setEnabled(!value && usable);
    restoreButton.setEnabled(!value && usable && static_cast<bool>(restoreActions.start));
    inspectButton.setEnabled(!value && index >= 0 && static_cast<size_t>(index) < page.rows.size());
    safePoints.setEnabled(!value && inspection && !inspection->points.empty());
    const bool inspected = inspection && inspection->metadataReady;
    checkSource.setEnabled(!value && inspected); locateButton.setEnabled(!value && inspected && !chooser);
    deepVerify.setEnabled(!value && inspected && inspection->checkpoint.getSourceEvidence().has_value());
    verifyAudio.setEnabled(!value && inspected);
    exportButton.setEnabled(!value && inspection && !chooser);
    cancelInspection.setEnabled(value);
}
void RecoveryCenterComponent::toggleStorage()
{
    if (working) return;
    storageView = !storageView; storageButton.setButtonText(storageView ? "Records" : "Storage");
    storageButton.setTooltip(storageView ? "Return to recovery records and their restore or cleanup actions."
        : "View recovery storage usage and limits. Scans only when requested.");
    busy(false); resized();
    if (storageView && !storageUsage) readStorage();
}

void RecoveryCenterComponent::readStorage()
{
    if (working || !storageView) return;
    joinWorker();
    retentionKnown = false;
    configuredStorage = restoreActions.storageSettings ? restoreActions.storageSettings() : std::nullopt;
    cancelled = std::make_shared<std::atomic<bool>>(false);
    busy(true); storageDetails.setText("Reading recovery storage usage... Editing and playback can continue.");
    const auto directory = root == juce::File() ? root : root.getParentDirectory();
    const auto cancellation = cancelled;
    const auto diagnostics = restoreActions.diagnostics;
    const juce::Uuid operation;
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    try
    {
        struct RetentionResult final
        { std::optional<recovery::RecoveryStorageLayout::RetentionPolicy> policy; recovery::Status status; };
        auto retention = std::make_shared<RetentionResult>();
        auto result = std::make_shared<recovery::RecoveryStorageLayout::Usage>();
        worker = std::thread([safeThis, result, retention, directory, cancellation, diagnostics, operation
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
            , boundary = beforeStorageRead
#endif
        ]
        {
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
            if (boundary) boundary();
#endif
#if JUCE_WINDOWS
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
                result->status = { recovery::Error::resourceOrEnvironmentFault, "storage_usage_priority" };
#endif
            if (result->status)
            {
                const recovery::RecoveryStorageLayout layout(directory);
                *result = layout.inspectUsage({}, [cancellation] { return cancellation->load(); });
                retention->status = cancellation->load() ? recovery::Status { recovery::Error::cancelled, "retention_read_cancelled" }
                    : layout.readRetention(retention->policy);
            }
            const auto failure = !result->status ? result->status : result->policyStatus;
            if (!failure && diagnostics)
                diagnostics->record({ operation, juce::Uuid(nullptr), recovery::RecoveryDiagnosticState::Stage::centerStorage, failure.error,
                    { result->bytes, static_cast<int64_t>(result->entries), static_cast<int64_t>(result->files),
                      result->present ? 1 : 0, result->complete ? 1 : 0, result->freeBytes } });
            juce::MessageManager::callAsync([safeThis, result, retention]
            {
                if (!safeThis) return;
                safeThis->joinWorker(); safeThis->storageUsage = result;
                safeThis->retentionPolicy = retention->policy; safeThis->retentionStatus = retention->status;
                safeThis->retentionKnown = result->status && retention->status;
                safeThis->busy(false); safeThis->showStorageUsage();
            });
        });
    }
    catch (...) { busy(false); storageDetails.setText("Storage scan could not start. Refresh usage to retry."); }
}

void RecoveryCenterComponent::showStorageUsage()
{
    if (!storageUsage) return;
    const auto& report = *storageUsage;
    const auto size = [] (int64_t value)
    { return value < 0 ? juce::String("Unavailable") : juce::File::descriptionOfSizeInBytes(value) + " (" + juce::String(value) + " bytes)"; };
    juce::String text = "Recovery storage\n\n";
    if (!retentionKnown) text += "Automatic cleanup: settings unavailable. Refresh usage to retry.\n\n";
    else if (retentionPolicy) text += "Automatic cleanup: keep inactive recovery records for at least "
        + juce::String(retentionPolicy->days) + (retentionPolicy->days == 1 ? " day.\n" : " days.\n")
        + "Records still in use or with unverified activity are kept.\n\n";
    else text += "Automatic cleanup: not configured. Choose Retention to set a period.\n\n";
    if (report.complete && !report.present) text += "No recovery storage has been created yet.\n";
    else text += (report.complete ? "Observed use: " : "Partial scan; counted at least: ") + size(report.bytes) + "\n"
        + "Audio and recovery history: " + size(report.journalBytes) + "\nRestore attempts: " + size(report.attemptBytes)
        + "\nOther files in the recovery folder: " + size(report.otherBytes)
        + "\nFiles counted: " + juce::String(static_cast<int64_t>(report.files)) + "\n";
    if (!report.status)
    {
        text += report.status.error == recovery::Error::cancelled ? "Scan cancelled. Recovery files are unchanged.\n"
            : report.status.error == recovery::Error::budgetExceeded ? "The scan limit was reached; usage is incomplete.\n"
            : "Some storage could not be read; usage is incomplete.\n";
        text += "Status: " + report.status.code + "\n";
    }
    text += "Available on disk: " + size(report.freeBytes) + "\n";
    const auto* policy = report.policy ? &*report.policy : configuredStorage ? &configuredStorage->service : nullptr;
    if (policy)
    {
        text += report.policy ? "\nStored protection limits\n" : "\nConfigured limits (stored policy unavailable)\n";
        text += "Total recovery space: " + size(policy->bytes) + "\nCleanup reserve: " + size(policy->maintenanceBytes)
            + "\nFile/folder limit: " + juce::String(static_cast<int64_t>(policy->entries)) + " ("
            + juce::String(static_cast<int64_t>(policy->maintenanceEntries)) + " reserved for cleanup)"
            + "\nAudio and history limit: " + size(policy->journalBytes)
            + "\nRestore attempt limit: " + size(policy->attemptBytes) + "\nMinimum free disk space: " + size(policy->freeSpaceReserve) + "\n";
    }
    if (configuredStorage) text += "Current per-document recovery budget: " + size(configuredStorage->ownerBytes) + "\n";
    if (!report.policyStatus) text += "Stored limits could not be verified: " + report.policyStatus.code + "\n";
    if (report.policy && configuredStorage && *report.policy != configuredStorage->service)
        text += "Stored limits differ from this session's configuration. New protection may need attention.\n";
    text += "\nChecked: " + timeLabel(report.observedAtMilliseconds)
        + ". Usage may change while editing or cleaning up.\nReturn to Records to review a recovery copy or resume cleanup. Checking usage does not remove files.";
    storageDetails.setText(text);
}

void RecoveryCenterComponent::chooseRetention()
{
    if (working || !retentionKnown || !retentionStatus || !configuredStorage) return;
    const auto expectedHash = retentionPolicy ? retentionPolicy->hash : juce::String();
    auto* dialog = new juce::AlertWindow("Automatic recovery retention", "Keep inactive crash recovery records for at least how many days?"
        "\nOlder records may then be deleted automatically. Active or unverified records are kept.",
        juce::MessageBoxIconType::QuestionIcon, this);
    dialog->addTextEditor("days", retentionPolicy ? juce::String(retentionPolicy->days) : juce::String(), "Days to keep:");
    dialog->getTextEditor("days")->setInputRestrictions(5, "0123456789");
    dialog->addButton("Review policy", 1); dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    const juce::Component::SafePointer<juce::AlertWindow> safeDialog(dialog);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, safeDialog, expectedHash](int result)
    {
        if (!safeThis || !safeDialog || result != 1) return;
        const auto text = safeDialog->getTextEditorContents("days").trim();
        const int days = text.getIntValue();
        if (text.isEmpty() || !text.containsOnly("0123456789") || days < 1 || days > 99999)
        { safeThis->storageDetails.setText("Enter a positive whole number of days. Retention settings are unchanged."); return; }
        auto* confirmation = new juce::AlertWindow("Apply automatic recovery cleanup?", "Keep inactive recovery records for at least "
            + juce::String(days) + (days == 1 ? " day." : " days.")
            + "\n\nThis applies to existing and future recovery records in this recovery folder. Older eligible copies can be permanently deleted in the background."
              "\nOriginal audio files are unchanged. Records still in use or with unverified activity are kept.",
            juce::MessageBoxIconType::WarningIcon, safeThis.getComponent());
        confirmation->addButton("Apply automatic cleanup", 1);
        confirmation->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        confirmation->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, days, expectedHash](int answer)
        { if (safeThis && answer == 1) safeThis->writeRetention(days, expectedHash); }), true);
    }), true);
}

void RecoveryCenterComponent::writeRetention(int days, juce::String expectedHash)
{
    if (working || !configuredStorage) return;
    joinWorker(); writingRetention = true; busy(true);
    storageDetails.setText("Saving retention settings... Editing and playback can continue.");
    const auto directory = root.getParentDirectory();
    const auto budget = configuredStorage->service;
    const auto notifyChanged = restoreActions.retentionChanged;
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    struct Result final { recovery::Status status; recovery::RecoveryStorageLayout::RetentionPolicy policy; };
    try
    {
        auto result = std::make_shared<Result>();
        worker = std::thread([safeThis, directory, budget, days, expectedHash, notifyChanged, result]
        {
#if JUCE_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            result->status = recovery::RecoveryStorageLayout(directory).configureRetention(days, expectedHash, result->policy, budget);
            juce::MessageManager::callAsync([safeThis, result, notifyChanged]
            {
                // The approved operation outlives this optional window. Notify
                // the service even if the user closed Recovery Center meanwhile.
                if (result->status && notifyChanged) notifyChanged();
                if (!safeThis) return;
                safeThis->joinWorker(); safeThis->writingRetention = false;
                if (result->status)
                {
                    safeThis->retentionPolicy = result->policy; safeThis->retentionKnown = true; safeThis->retentionStatus = {};
                    safeThis->busy(false); safeThis->showStorageUsage();
                }
                else
                {
                    safeThis->retentionKnown = false; safeThis->busy(false);
                    safeThis->storageDetails.setText("Retention settings could not be confirmed: " + result->status.code
                        + ". Refresh usage to read the current policy before trying again.");
                }
            });
        });
    }
    catch (...) { writingRetention = false; busy(false); storageDetails.setText("Retention settings could not be saved. Refresh usage to retry."); }
}

void RecoveryCenterComponent::restoreSelected()
{
    if (working || !restoreActions.start) return;
    const auto index = owners.getSelectedItemIndex();
    if (index < 0 || static_cast<size_t>(index) >= page.rows.size()) return;
    const auto& row = page.rows[static_cast<size_t>(index)];
    if (!row.summary.status || row.summary.retired || row.summary.head.sequence <= 0) return;
    recovery::RecoveryRestoreQueue::Request request;
    request.owner = row.owner; request.observedHead = row.summary.head; request.selectedHead = row.summary.head;
    if (inspection && inspection->metadataReady && inspection->owner == row.owner && inspection->observedHead == row.summary.head)
    {
        request.selectedHead = inspection->selectedHead; request.source = sourceReview;
        if (inspection->sourceChecked) request.source.expectedObservation = inspection->observation;
    }
    const bool quarantined = inspection && inspection->quarantinedFor(restoreActions.appBuild);
    const bool relocated = inspection && inspection->sourceChecked && inspection->resolution.confidence == recovery::SourceConfidence::relocated;
    if (quarantined || relocated)
    {
        juce::String message = "Restore a separate copy of the selected history point. The original file stays unchanged; Save As is required.";
        if (quarantined) message += "\n\nThis point has repeated interrupted attempts in this build. Retry Once permits one new attempt; it does not enable automatic retries.";
        if (relocated) message += "\n\nUse this matching relocated file?\n" + inspection->observation.location.getFullPathName();
        auto* dialog = new juce::AlertWindow(quarantined ? "Retry quarantined recovery once?" : "Use relocated original?", message, juce::MessageBoxIconType::WarningIcon, this);
        dialog->addButton(quarantined ? "Retry Once" : "Use relocated file", 1);
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        request.retryQuarantinedOnce = quarantined; request.source.relocationConfirmed = relocated;
        const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
        dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, request](int result)
        { if (safeThis && result == 1) safeThis->submitRestore(request); }), true);
        return;
    }
    submitRestore(std::move(request));
}
void RecoveryCenterComponent::submitRestore(recovery::RecoveryRestoreQueue::Request request)
{
    if (!restoreActions.start) return;
    juce::Uuid attempt { nullptr };
    const auto status = restoreActions.start(std::move(request), attempt);
    details.setText(status ? "Restore Copy accepted. The copy will appear in the playlist when its first waveform and playback are ready. Select it there when you want to use it.\n\nClosing this window leaves the task running. Use Cancel restore to stop the task. Save As is required to save the recovered copy."
        : "Restore was not accepted: " + status.code + "\nThe recovery record is preserved.");
    updateRestoreJobs();
    for (size_t i = 0; i < restoreHealth.jobs.size(); ++i) if (restoreHealth.jobs[i].attempt == attempt)
        jobs.setSelectedItemIndex(static_cast<int>(i), juce::sendNotificationSync);
}
void RecoveryCenterComponent::inspectSelected(recovery::RecoveryInspection::SourceAction action, bool verifyStoredAudio)
{
    if (working) return;
    const auto index = owners.getSelectedItemIndex();
    if (index < 0 || static_cast<size_t>(index) >= page.rows.size()) return;
    const auto& row = page.rows[static_cast<size_t>(index)];
    recovery::RecoveryInspection::Request request; request.observedHead = row.summary.head;
    const auto point = safePoints.getSelectedItemIndex();
    if (inspection && point >= 0 && static_cast<size_t>(point) < inspection->points.size())
        request.selectedHead = inspection->points[static_cast<size_t>(point)].head;
    request.source = sourceReview; request.source.expectedObservation.reset(); request.action = action;
    request.verifyStoredAudio = verifyStoredAudio;
    joinWorker(); review.reset(); cancelled = std::make_shared<std::atomic<bool>>(false); busy(true);
    details.setText(verifyStoredAudio ? "Verifying stored audio and complete Undo/Redo history. No recovered copy or playback is started. You can cancel this inspection."
        : action == recovery::RecoveryInspection::SourceAction::deepVerify
        ? "Deep Verify is reading the source file. It does not read stored recovery audio. You can cancel this inspection."
        : "Inspecting the selected history point. No restore, decoder or audio playback is started.");
    const auto owner = row.owner; const auto journal = root; const auto attempts = restoreActions.attemptRoot;
    const auto cancellation = cancelled;
    const auto diagnostics = restoreActions.diagnostics;
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    try
    {
        auto report = std::make_shared<recovery::RecoveryInspection::Report>();
        worker = std::thread([safeThis, owner, journal, attempts, cancellation, request, report, diagnostics]
        {
            *report = recovery::RecoveryInspection::read(recovery::RecoveryStore(journal, owner), attempts, request, cancellation.get(),
                [safeThis, cancellation](size_t verified, size_t total, size_t bytes)
                {
                    juce::MessageManager::callAsync([safeThis, cancellation, verified, total, bytes]
                    {
                        if (!safeThis || safeThis->cancelled != cancellation || cancellation->load() || !safeThis->working) return;
                        safeThis->details.setText("Verifying stored audio and complete Undo/Redo history...\nChecked "
                            + juce::String(static_cast<int>(verified)) + " of " + juce::String(static_cast<int>(total)) + " audio blocks; "
                            + juce::File::descriptionOfSizeInBytes(static_cast<int64_t>(bytes)) + " read.\nYou can cancel. Existing data and the current document remain unchanged.");
                    });
                }, diagnostics);
            juce::MessageManager::callAsync([safeThis, report]
            {
                if (!safeThis) return;
                safeThis->joinWorker(); safeThis->inspection = report;
                safeThis->safePoints.clear(juce::dontSendNotification);
                int id = 1, selectedId = 0;
                for (const auto& point : report->points)
                {
                    safeThis->safePoints.addItem(timeLabel(point.milliseconds) + " - point " + juce::String(point.head.sequence), id);
                    if (point.head == report->selectedHead) selectedId = id;
                    ++id;
                }
                safeThis->safePoints.setSelectedId(selectedId, juce::dontSendNotification);
                safeThis->busy(false); safeThis->showInspection();
            });
        });
    }
    catch (...) { inspection.reset(); busy(false); details.setText("Inspection could not start. Refresh to retry; existing records are preserved."); }
}
void RecoveryCenterComponent::showInspection()
{
    if (!inspection) return;
    const auto& report = *inspection;
    juce::String text = (report.audioCheckRequested ? "Stored audio verification\nOwner: " : "Safe Inspect\nOwner: ")
        + report.owner.toString() + "\nSelected point: " + juce::String(report.selectedHead.sequence);
    if (!report.status) text += "\nInspection stopped: " + report.status.code + "\nRefresh to review the current record. You can export available diagnostics.";
    if (report.metadataReady)
    {
        text += "\nMetadata and history references checked."
            "\nUndo steps: " + juce::String(static_cast<int>(report.checkpoint.getUndoCount()))
            + "; Redo steps: " + juce::String(static_cast<int>(report.checkpoint.getRedoCount()))
            + "\nOriginal path: " + report.checkpoint.getIdentity().source.location.file.getFullPathName();
        if (report.audioVerified) text += "\nStored audio verified for this point and its complete Undo/Redo history. Restore Copy rechecks before use.";
        else text += "\nStored audio bodies have NOT been fully verified.";
        if (report.sourceChecked) text += "\n" + confidenceLabel(report.resolution.confidence);
        else text += "\nSource identity has not been verified.";
        if (!report.sourceStatus) text += "\nSource check stopped: " + report.sourceStatus.code + ". Check again or locate the original. Restore Copy can still use stored recovery audio.";
        if (!report.checkpoint.getSourceEvidence()) text += "\nThis record has no historical source fingerprint. Source Deep Verify is unavailable; a new hash cannot establish the original identity. Stored audio can still be checked.";
        if (report.sourceChecked) text += "\nSource windows read: " + juce::String(static_cast<juce::int64>(report.sourceWindowBytesRead))
            + " bytes; full file read: " + juce::String(report.sourceFullBytesRead) + " bytes.";
        if (report.sourceFullHash.isNotEmpty()) text += "\nFull source SHA-256: " + report.sourceFullHash
            + "\nA full hash describes the current file. Original confidence still uses the recorded evidence.";
    }
    if (report.audioCheckRequested)
    {
        text += "\nAudio blocks checked: " + juce::String(static_cast<int>(report.audioAssetsVerified)) + " of "
            + juce::String(static_cast<int>(report.assets.size())) + "; " + juce::File::descriptionOfSizeInBytes(static_cast<int64_t>(report.audioPayloadBytesRead)) + " read.";
        if (!report.audioStatus)
        {
            text += "\nAudio verification stopped: " + report.audioStatus.code + ". ";
            switch (report.audioStatus.error)
            {
                case recovery::Error::missingOrCorruptAsset: text += "Some stored audio is missing or failed validation. Inspect an earlier point or export diagnostics."; break;
                case recovery::Error::resourceOrEnvironmentFault: text += "Storage is unavailable or busy. Resolve access and retry; this does not establish damaged audio."; break;
                case recovery::Error::unsupportedSchema: text += "This build cannot interpret the stored format. Preserve the record and export diagnostics."; break;
                case recovery::Error::budgetExceeded: text += "The read budget was reached. Remaining audio is unchecked; export diagnostics to review the limit."; break;
                case recovery::Error::cancelled: text += "Only the listed blocks were checked. Run Deep Verify audio again to check the complete point."; break;
                default: text += "Refresh the record and inspect it again, or export diagnostics."; break;
            }
            text += " The recovery record is preserved.";
        }
        if (!report.audioVerified) text += "\nThis result does not certify complete stored audio.";
    }
    if (!report.attempts.status) text += "\nAttempt history unavailable: " + report.attempts.status.code;
    text += "\nAttempt records: " + juce::String(static_cast<int>(report.attempts.attempts.size()));
    if (report.quarantinedFor(restoreActions.appBuild))
        text += "\nQuarantined for this point and build. Restore Copy will ask before Retry Once. You may inspect an earlier point instead.";
    text += "\n\nInspection leaves the current audio and its complete Undo/Redo unchanged. Recovered copies require Save As.";
    details.setText(text);
}
void RecoveryCenterComponent::locateSource()
{
    if (working || chooser || !inspection || !inspection->metadataReady) return;
    const auto reviewed = inspection;
    chooser = std::make_unique<juce::FileChooser>("Locate the original audio file", juce::File(), "*", true);
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    busy(false);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, reviewed](const juce::FileChooser& selectedFile)
        {
            const auto file = selectedFile.getResult();
            if (!safeThis) return;
            safeThis->chooser.reset(); safeThis->busy(safeThis->working);
            if (file == juce::File() || safeThis->inspection != reviewed || safeThis->working) return;
            safeThis->sourceReview = {}; safeThis->sourceReview.locatedFile = file;
            safeThis->inspectSelected(recovery::RecoveryInspection::SourceAction::locateOrCheck);
        });
}
void RecoveryCenterComponent::exportDiagnostics()
{
    if (working || chooser || !inspection || !restoreActions.showDiagnostics) return;
    restoreActions.showDiagnostics();
}
void RecoveryCenterComponent::updateRestoreJobs()
{
    juce::Uuid selected { nullptr };
    const auto old = jobs.getSelectedItemIndex();
    if (old >= 0 && static_cast<size_t>(old) < restoreHealth.jobs.size()) selected = restoreHealth.jobs[static_cast<size_t>(old)].attempt;
    restoreHealth = restoreActions.health ? restoreActions.health() : recovery::RecoveryRestoreQueue::Health {};
    jobs.clear(juce::dontSendNotification);
    int chosen = 0, id = 1;
    for (const auto& job : restoreHealth.jobs)
    {
        jobs.addItem(job.attempt.toString().substring(0, 8) + " - " + stageLabel(job), id);
        if (job.attempt == selected) chosen = id;
        ++id;
    }
    if (!restoreHealth.jobs.empty()) jobs.setSelectedId(chosen > 0 ? chosen : 1, juce::dontSendNotification);
    selectedJob();
}
void RecoveryCenterComponent::selectedJob()
{
    const auto index = jobs.getSelectedItemIndex();
    const auto* job = index >= 0 && static_cast<size_t>(index) < restoreHealth.jobs.size() ? &restoreHealth.jobs[static_cast<size_t>(index)] : nullptr;
    const bool done = job && terminal(job->stage);
    cancelJob.setEnabled(job && !done);
    retryJob.setEnabled(job && ((done && job->stage != recovery::RecoveryAttemptStage::ready) || restoreHealth.failure.code == "restore_worker_launch"));
    forgetJob.setEnabled(done);
    jobDiagnostics.setEnabled(job && static_cast<bool>(restoreActions.showDiagnostics));
    juce::String text = job ? "Attempt: " + job->attempt.toString() + "\n" + stageLabel(*job) : "No restore tasks. Choose a recovery record and select Restore Copy.";
    if (job && !job->failure) text += "\nReason: " + job->failure.code;
    if (job && !job->attemptCloseFailure) text += "\nAttempt record could not close: " + job->attemptCloseFailure.code;
    if (job && job->coreAdmitted && job->stage != recovery::RecoveryAttemptStage::ready)
        text += "\nThe playlist copy retains its prepared audio. Remaining audio and history may be unavailable until a new restore completes.";
    if (job && done) text += "\nClearing this task keeps the playlist copy and recovery record.";
    if (!restoreHealth.failure) text += "\nTask scheduling: " + restoreHealth.failure.code;
    if (job) text += "\nAudio held by recovered copies: " + juce::File::descriptionOfSizeInBytes(static_cast<int64_t>(restoreHealth.residentMappingBytes));
    if (jobDetails.getText() != text) jobDetails.setText(text);
}
void RecoveryCenterComponent::actOnJob(int action)
{
    const auto index = jobs.getSelectedItemIndex();
    if (index < 0 || static_cast<size_t>(index) >= restoreHealth.jobs.size()) return;
    const auto attempt = restoreHealth.jobs[static_cast<size_t>(index)].attempt;
    recovery::Status status { recovery::Error::resourceOrEnvironmentFault, "restore_action_unavailable" };
    juce::Uuid replacement { nullptr };
    if (action == 0 && restoreActions.cancel) status = restoreActions.cancel(attempt);
    if (action == 1 && restoreActions.retry) status = restoreActions.retry(attempt, replacement);
    if (action == 2 && restoreActions.forget) status = restoreActions.forget(attempt);
    updateRestoreJobs();
    if (!status) jobDetails.setText("Action was not accepted: " + status.code + "\nThe recovery record is preserved.\n" + jobDetails.getText());
}
void RecoveryCenterComponent::readPage(bool restart)
{
    if (working) return;
    joinWorker(); review.reset(); inspection.reset(); sourceReview = {}; safePoints.clear(juce::dontSendNotification);
    cancelled = std::make_shared<std::atomic<bool>>(false);
    if (restart || !discovery) discovery = std::make_shared<recovery::RecoveryDiscovery>(root);
    busy(true); details.setText("Reading recovery record summaries...");
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    try
    {
        auto result = std::make_shared<recovery::RecoveryDiscovery::Page>();
        const auto discoveryCursor = discovery;
        const auto cancellation = cancelled;
        worker = std::thread([safeThis, result, discoveryCursor, cancellation]
        {
            *result = discoveryCursor->nextPage({}, [cancellation] { return cancellation->load(); });
            juce::MessageManager::callAsync([safeThis, result]
            {
                if (!safeThis) return;
                safeThis->joinWorker(); safeThis->page = std::move(*result);
                safeThis->owners.clear(juce::dontSendNotification);
                int id = 1;
                for (const auto& row : safeThis->page.rows)
                {
                    const auto path = row.summary.sourcePath;
                    const auto label = path.isNotEmpty() ? juce::File(path).getFileName() : row.owner.toString();
                    safeThis->owners.addItem(label + (row.summary.retired ? " (retired)" : ""), id++);
                }
                if (!safeThis->page.rows.empty()) safeThis->owners.setSelectedId(1, juce::dontSendNotification);
                safeThis->busy(false); safeThis->selected();
            });
        });
    }
    catch (...) { busy(false); details.setText("Discovery could not start. Refresh to retry."); }
}
void RecoveryCenterComponent::selected()
{
    review.reset(); inspection.reset(); sourceReview = {}; safePoints.clear(juce::dontSendNotification); busy(working);
    juce::String text;
    if (!page.status) text = "Discovery status: " + page.status.code + "\n";
    const int index = owners.getSelectedItemIndex();
    if (index >= 0 && static_cast<size_t>(index) < page.rows.size())
    {
        const auto& row = page.rows[static_cast<size_t>(index)]; const auto& summary = row.summary;
        text += "Owner: " + row.owner.toString() + "\nOriginal path: " + (summary.sourcePath.isEmpty() ? "Unavailable" : summary.sourcePath)
            + "\nLast safe time: " + timeLabel(summary.lastSafeMilliseconds)
            + "\nObserved head sequence: " + juce::String(summary.head.sequence)
            + "\n" + (summary.retired ? "Retired; cleanup may remain." : summary.status.code == "owner_collecting"
                ? "Cleanup was interrupted; it can be resumed." : summary.status
                ? "Summary only; source identity and complete history have not been verified." : "Record needs attention: " + summary.status.code)
            + (summary.retired || summary.status.code == "owner_collecting"
                ? "\nResume cleanup continues removal already authorized by Save or Discard."
                : "\nSize: measured when you select Review Discard.")
            + "\n\nReviewing a record does not change the current audio or its Undo/Redo history.";
    }
    else if (!page.status) text += "Recovery records could not be listed. Existing data has not been changed. Refresh to retry.";
    else text += page.more ? "No owner on this page. Next page reads more entries." : "No recovery records found.";
    if (page.more) text += "\n\nMore entries remain. Use Next page to continue.";
    details.setText(text);
}
void RecoveryCenterComponent::retryCleanup()
{
    if (working) return;
    const int index = owners.getSelectedItemIndex();
    if (index >= 0 && static_cast<size_t>(index) < page.rows.size())
    {
        const auto& row = page.rows[static_cast<size_t>(index)];
        if (onResumeCleanup && ((row.summary.status && row.summary.retired) || row.summary.status.code == "owner_collecting"))
        {
            const auto status = onResumeCleanup(row.owner, row.summary.head);
            details.setText(status
                ? "Cleanup resume requested. It continues in the background when this window closes. The main window shows cleanup status; Refresh updates this list."
                : "Cleanup could not resume: " + status.code + ". Existing recovery data is kept.");
            return;
        }
    }
    if (retry) retry();
    details.setText("Cleanup retry requested. The main window shows protection and cleanup status.");
}

void RecoveryCenterComponent::chooseExpiry()
{
    if (working) return;
    review.reset(); busy(false);
    const int index = owners.getSelectedItemIndex();
    if (index < 0 || static_cast<size_t>(index) >= page.rows.size()) return;
    const auto owner = page.rows[static_cast<size_t>(index)].owner;
    auto* dialog = new juce::AlertWindow("Review recovery expiry", "Keep at least how many days of recovery?"
        "\nThis checks only the selected record without changing automatic retention settings."
        "\nOlder active or unverified records are preserved.", juce::MessageBoxIconType::QuestionIcon, this);
    dialog->addTextEditor("days", {}, "Days to keep:");
    dialog->getTextEditor("days")->setInputRestrictions(5, "0123456789");
    dialog->addButton("Review", 1); dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    const juce::Component::SafePointer<juce::AlertWindow> safeDialog(dialog);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, safeDialog, owner](int result)
    {
        if (!safeThis || !safeDialog || result != 1) return;
        const int index = safeThis->owners.getSelectedItemIndex();
        if (index < 0 || static_cast<size_t>(index) >= safeThis->page.rows.size()
            || safeThis->page.rows[static_cast<size_t>(index)].owner != owner) return;
        const auto text = safeDialog->getTextEditorContents("days").trim();
        const auto days = text.getLargeIntValue();
        if (text.isEmpty() || !text.containsOnly("0123456789") || days < 1 || days > 99999)
        { safeThis->details.setText("Enter a positive whole number of days. The recovery record is preserved."); return; }
        safeThis->reviewSelected(days * int64_t(24 * 60 * 60 * 1000));
    }), true);
}
void RecoveryCenterComponent::reviewSelected(int64_t retentionMilliseconds)
{
    if (working) return;
    const int index = owners.getSelectedItemIndex();
    if (index < 0 || static_cast<size_t>(index) >= page.rows.size()) return;
    joinWorker(); review.reset(); busy(true); details.setText("Reviewing the selected record and its stored size...");
    cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto owner = page.rows[static_cast<size_t>(index)].owner;
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    struct Result final { recovery::Status status; std::unique_ptr<recovery::OwnerDiscardReview> review; };
    try
    {
        auto result = std::make_shared<Result>();
        const auto cancellation = cancelled;
        const auto reviewedRoot = root;
        worker = std::thread([safeThis, owner, result, cancellation, reviewedRoot, retentionMilliseconds]
        {
            try
            {
#if JUCE_WINDOWS
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
                recovery::RecoveryStore store(reviewedRoot, owner);
                const auto cancel = [cancellation] { return cancellation->load(); };
                result->status = retentionMilliseconds > 0 ? store.reviewExpiry(retentionMilliseconds, result->review, cancel)
                                                          : store.reviewDiscard(result->review, cancel);
            }
            catch (...) { result->status = { recovery::Error::resourceOrEnvironmentFault, "review_worker_exception" }; }
            juce::MessageManager::callAsync([safeThis, result]
            {
                if (!safeThis) return;
                safeThis->joinWorker(); safeThis->review = std::move(result->review); safeThis->busy(false);
                if (!result->status || !safeThis->review)
                {
                    const auto explanation = result->status.code == "owner_not_expired" ? "This record is within the chosen retention period."
                        : result->status.code == "expiry_activity_unproven" ? "This older record cannot prove writer inactivity. Restore or review Discard explicitly."
                        : "Review failed: " + result->status.code;
                    safeThis->details.setText(explanation + "\nThe recovery record is preserved. Refresh to review its current state."); return;
                }
                const auto& review = *safeThis->review;
                safeThis->details.setText("Review before Discard\n\nOwner: " + review.ownerId().toString()
                    + "\nOriginal path: " + review.sourcePath() + "\nLast safe time: " + timeLabel(review.lastSafeMilliseconds())
                    + "\nObserved stored bytes: " + juce::String(review.observedBytes())
                    + (review.expiryRetentionMilliseconds() > 0 ? "\nExpiry review: keep at least "
                        + juce::String(review.expiryRetentionMilliseconds() / (24 * 60 * 60 * 1000)) + " days." : "")
                    + "\n\nDiscard removes this recovery record after a second confirmation. It leaves the original audio file unchanged."
                      "\nIf the owner changes or is in use, cleanup will wait or be refused.");
            });
        });
    }
    catch (...) { busy(false); details.setText("Review could not start. The recovery record is preserved."); }
}
void RecoveryCenterComponent::confirmDiscard()
{
    if (working || !review) return;
    auto approved = std::make_shared<std::unique_ptr<recovery::OwnerDiscardReview>>(std::move(review));
    busy(false);
    auto* dialog = new juce::AlertWindow("Discard recovery record?", "Owner: " + (*approved)->ownerId().toString()
        + "\nLast safe time: " + timeLabel((*approved)->lastSafeMilliseconds())
        + "\nObserved stored bytes: " + juce::String((*approved)->observedBytes())
        + ((*approved)->expiryRetentionMilliseconds() > 0 ? "\nExpired under the chosen retention period; active use is checked again." : "")
        + "\n\nThis removes the selected recovery copy. The original audio file remains unchanged.", juce::MessageBoxIconType::WarningIcon, this);
    dialog->addButton("Discard recovery record", 1); dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const juce::Component::SafePointer<RecoveryCenterComponent> safeThis(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, approved](int result)
    {
        if (!safeThis) return;
        if (result != 1) { safeThis->details.setText("Discard cancelled. The recovery record is preserved."); return; }
        const auto status = safeThis->submit ? safeThis->submit(*approved) : recovery::Status { recovery::Error::resourceOrEnvironmentFault, "recovery_service_unavailable" };
        safeThis->details.setText(status ? "Discard accepted. Cleanup continues in the background; the main window shows its status."
            : "Discard was not accepted: " + status.code + "\nThe recovery record is preserved. Review again before retrying.");
    }), true);
}
}
