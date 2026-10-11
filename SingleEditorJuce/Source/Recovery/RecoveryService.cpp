#include "RecoveryService.h"
#include <algorithm>

namespace singleeditor::recovery
{
namespace
{
bool runningProtection(const RecoverySessionHost::Health& status)
{
    return std::any_of(status.sessions.begin(), status.sessions.end(), [](const auto& session)
    {
        if (session.protection.current.writer.workerRunning) return true;
        return std::any_of(session.protection.retired.begin(), session.protection.retired.end(),
                           [](const auto& epoch) { return epoch.writer.workerRunning; });
    });
}
}
RecoverySessionHost::Config RecoveryService::bindStorage(const Config& config, const std::shared_ptr<RecoveryDiagnosticState>& diagnostics)
{
    auto settings = config.sessions;
    settings.session.producer.queue.diagnostics = diagnostics;
    const RecoveryStorageLayout layout(config.root);
    settings.session.producer.root = layout.journalDirectory();
    // Service owns both namespaces and the preparation contract. The immutable
    // values remain valid even after native Document/service destruction.
    settings.session.producer.prepareStorage = [layout, budget = config.storage] { return layout.prepare(budget); };
    return settings;
}

RecoveryMaintenanceQueue::Config RecoveryService::bindMaintenance(const Config& config, const std::shared_ptr<RecoveryDiagnosticState>& diagnostics)
{
    auto settings = config.maintenance;
    settings.diagnostics = diagnostics;
    settings.store = config.sessions.session.producer.store;
    return settings;
}

RecoveryService::RecoveryService(SingleTrackDocument& doc, DocumentSaveCoordinator& coordinator, Config config)
    : document(doc), saves(coordinator), journalRoot(RecoveryStorageLayout(config.root).journalDirectory()),
      storageBudget(config.storage), ownerByteLimit(config.sessions.session.producer.store.ownerBytes),
      diagnostics(config.diagnostics ? std::move(config.diagnostics) : std::make_shared<RecoveryDiagnosticState>()),
      sessions(doc, coordinator, bindStorage(config, diagnostics)), maintenance(bindMaintenance(config, diagnostics))
{
    if (config.root == juce::File())
    {
        dispositionFailure = { Error::invalidPacket, "service_root" };
        note(RecoveryDiagnosticState::Stage::serviceConfiguration, dispositionFailure);
    }
    // Host's Save listener was registered first: it signs exact producer tickets
    // before our wake-up and before the native Save completion can switch rows.
    document.addListener(this);
    saves.addListener(this);
}

void RecoveryService::note(RecoveryDiagnosticState::Stage stage, const Status& status, const juce::Uuid& owner) const noexcept
{
    if (status || status.error == Error::cancelled) return;
    try
    {
        diagnostics->record({ juce::Uuid(), owner, stage, status.error,
            { static_cast<int64_t>(waiting.size()), static_cast<int64_t>(releasedCount), static_cast<int64_t>(completedCount),
              static_cast<int64_t>(cancelledCount), static_cast<int64_t>(removedFiles), static_cast<int64_t>(removedBytes) } });
    }
    catch (...) {}
}

RecoveryService::~RecoveryService()
{
    saves.removeListener(this);
    document.removeListener(this);
    // Members close admission; accepted workers retain only owned captures.
    // A forced exit does not turn their unfinished work into a clean close.
}

Status RecoveryService::attachPostCore(const juce::Uuid& entry)
{
    if (!dispositionFailure && dispositionFailure.code == "service_root") return dispositionFailure;
    const auto status = sessions.attachPostCore(entry);
    pollRequested = true;
    poll();
    return status;
}

void RecoveryService::leaveDocument() { sessions.leaveDocument(); pollRequested = true; }

void RecoveryService::poll() try
{
    // One extra owner-thread pass if a worker was live on entry. It may finish
    // between Host::poll and the later health read; that transition must not
    // leave a completed retired actor stranded with polling turned off.
    const bool wasRunning = runningProtection(sessions.health());
    sessions.poll();
    for (const auto& epoch : sessions.takeReleasedEpochs())
    {
        ++releasedCount;
        cancelledCount += epoch.protection.writer.cancelledPackets;
        // Released Health is an observation only, never a retirement ticket.
    }
    for (const auto& completed : maintenance.takeCompleted())
    {
        if (!completed.preserved) ++completedCount;
        removedFiles += completed.filesRemoved; removedBytes += completed.bytesRemoved;
    }
    if (waiting.empty()) waiting = sessions.takeSavedRetirements();
    if (dispositionFailure.code != "service_root") dispositionFailure = {};
    while (!waiting.empty() && dispositionFailure)
    {
        // submit moves only on acceptance. A full/failed queue leaves this real
        // ticket here, while the Host retains any subsequent bounded batch.
        const auto submitted = maintenance.submit(waiting.front());
        if (!submitted)
        {
            dispositionFailure = submitted;
            if (transferFailure.error != submitted.error || transferFailure.code != submitted.code)
                note(RecoveryDiagnosticState::Stage::serviceTransfer, submitted,
                     waiting.front() ? waiting.front()->ownerId() : juce::Uuid(nullptr));
            transferFailure = submitted;
            break;
        }
        transferFailure = {};
        waiting.erase(waiting.begin());
    }
    const auto status = sessions.health();
    const auto cleanup = maintenance.progress();
    pollRequested = wasRunning || cleanup.workerRunning || status.pendingMetadata != 0
        || (status.pendingRetirements != 0 && waiting.empty())
        || runningProtection(status)
        || std::any_of(cleanup.jobs.begin(), cleanup.jobs.end(), [](const auto& job)
                       { return job.phase == RecoveryMaintenanceQueue::Phase::completed; });
    // Failed workers stop polling; Retry is explicit. A Save or Document event
    // wakes this same service, including a source transition during close.
}
catch (...)
{
    dispositionFailure = { Error::resourceOrEnvironmentFault, "service_poll_exception" };
    note(RecoveryDiagnosticState::Stage::servicePoll, dispositionFailure);
    pollRequested = false;
}

RecoveryService::Health RecoveryService::health() const
{
    Health result;
    result.protection = sessions.health(); result.maintenance = maintenance.progress();
    result.dispositionFailure = dispositionFailure; result.waitingRetirements = waiting.size();
    result.releasedEpochs = releasedCount; result.cancelledPackets = cancelledCount;
    result.completedOwners = completedCount; result.removedFiles = removedFiles; result.removedBytes = removedBytes;
    result.needsPoll = pollRequested;
    result.settled = result.protection.settled && result.protection.failure && result.maintenance.failure
        && result.dispositionFailure && result.protection.pendingMetadata == 0 && result.protection.pendingRetirements == 0
        && waiting.empty() && result.maintenance.jobs.empty() && !result.maintenance.workerRunning;
    return result;
}

Status RecoveryService::retryProtection(const juce::Uuid& owner, bool collectOrphans)
{
    const auto status = sessions.retry(owner, collectOrphans);
    pollRequested = true; poll();
    return status;
}

Status RecoveryService::retryMaintenance(const juce::Uuid& owner)
{
    const auto status = maintenance.retry(journalRoot, owner);
    if (!status) note(RecoveryDiagnosticState::Stage::serviceRetry, status, owner);
    pollRequested = true; poll();
    return status;
}

Status RecoveryService::checkpointCurrent()
{
    const auto status = sessions.checkpointCurrent();
    pollRequested = true; poll();
    return status;
}

Status RecoveryService::sweepTerminalOwnersAfterPresence(bool policyChanged)
{
    const auto status = maintenance.sweepTerminals(journalRoot, policyChanged);
    if (!status) note(RecoveryDiagnosticState::Stage::maintenanceDiscovery, status);
    pollRequested = true; poll();
    return status;
}

Status RecoveryService::resumeTerminalCleanup(const juce::Uuid& owner, const SafeHead& head)
{
    const auto progress = maintenance.progress();
    Status status;
    const auto found = std::find_if(progress.jobs.begin(), progress.jobs.end(), [&] (const auto& job)
        { return job.root == journalRoot && job.owner == owner; });
    if (found == progress.jobs.end()) status = maintenance.resumeTerminal(journalRoot, owner, head);
    else if (found->head != head && !(head.sequence == 0 && head.hash.isEmpty()))
        status = { Error::staleHead, "maintenance_resume_head_changed" };
    else if (found->phase == RecoveryMaintenanceQueue::Phase::failed
        || (found->phase == RecoveryMaintenanceQueue::Phase::queued && !progress.workerRunning))
        status = maintenance.retry(journalRoot, owner);
    // An identical in-flight/completed job already owns this request.
    if (!status) note(RecoveryDiagnosticState::Stage::serviceRetry, status, owner);
    pollRequested = true; poll();
    return status;
}

Status RecoveryService::submitConfirmedDiscard(std::unique_ptr<OwnerDiscardReview>& review)
{
    if (!review || review->rootDirectory() != journalRoot)
    {
        const Status status { Error::staleHead, "service_discard_root" };
        note(RecoveryDiagnosticState::Stage::serviceDiscard, status, review ? review->ownerId() : juce::Uuid(nullptr));
        return status;
    }
    const auto owner = review->ownerId();
    const auto status = maintenance.submitConfirmedDiscard(review);
    if (!status) note(RecoveryDiagnosticState::Stage::serviceDiscard, status, owner);
    if (status) { pollRequested = true; poll(); }
    return status;
}

void RecoveryService::beginClose()
{
    maintenance.cancelTerminalSweep(); // Stop new discovery; accepted cleanup still drains.
    sessions.beginClose(); pollRequested = true; poll();
}

Status RecoveryService::cancelDiscard(const juce::Uuid& owner)
{
    const auto status = maintenance.cancelDiscard(journalRoot, owner);
    if (!status) note(RecoveryDiagnosticState::Stage::serviceCancel, status, owner);
    if (status) { pollRequested = true; poll(); }
    return status;
}

Status RecoveryService::cancelClose(const juce::Uuid& entry)
{
    const auto status = sessions.cancelClose(entry);
    pollRequested = true; poll();
    return status;
}
}
