#include "RecoverySessionHost.h"
#include <algorithm>

namespace singleeditor::recovery
{
RecoverySessionHost::RecoverySessionHost(SingleTrackDocument& doc, DocumentSaveCoordinator& coordinator, Config settings)
    : document(doc), saves(coordinator), config(std::move(settings))
{
    if (config.residentSessions == 0 || config.residentSessions > 128 || config.retainedBytes == 0
        || config.savedTickets == 0 || config.savedTickets > 4096
        || config.metadataEvents < config.session.retiredEpochs + 1 || config.metadataEvents > 16384)
        fail({ Error::invalidPacket, "host_configuration" }, RecoveryDiagnosticState::Stage::hostConfiguration);
    if (failure) departed.reserve(config.residentSessions);
    saves.addListener(this);
}

RecoverySessionHost::~RecoverySessionHost()
{
    saves.removeListener(this);
    if (active) active->session->close();
    for (auto& item : departed) item.session->close();
}

void RecoverySessionHost::note(RecoveryDiagnosticState::Stage stage, const Status& status,
    const juce::Uuid& owner, uint64_t saveSerial) const noexcept
{
    if (!config.session.producer.queue.diagnostics || status) return;
    // One real failed Host operation, identified at its result publication.
    // Observation through health() never comes through this path.
    try
    {
        config.session.producer.queue.diagnostics->record({ juce::Uuid(), owner, stage, status.error,
            { static_cast<int64_t>(departed.size()), static_cast<int64_t>(releasedEpochs.size()),
              static_cast<int64_t>(savedRetirements.size()), active ? 1 : 0, closing ? 1 : 0,
              static_cast<int64_t>(saveSerial) } });
    }
    catch (...) {}
}

void RecoverySessionHost::fail(Status status, RecoveryDiagnosticState::Stage stage, const juce::Uuid& owner, uint64_t saveSerial)
{
    note(stage, status, owner, saveSerial);
    if (failure) failure = std::move(status);
}

Status RecoverySessionHost::attachPostCore(const juce::Uuid& entry) try
{
    const auto finish = [&](Status status)
    { note(RecoveryDiagnosticState::Stage::hostAdmission, status); return status; };
    if (closing || entry.isNull()) return finish({ Error::invalidPacket, "host_admission" });
    if (!failure && failure.code == "host_configuration") return failure;
    const auto identity = document.getMediaIdentity();
    if (active)
    {
        const auto existing = active->session->health();
        if (!existing.closed && existing.observed.document.sameDocument(identity.document))
            return finish(active->entry == entry ? Status{} : Status{ Error::staleHead, "host_entry_changed" });
        leaveDocument();
    }
    poll();
    const auto retained = health().retainedBytes;
    for (auto it = departed.begin(); it != departed.end(); ++it)
    {
        const auto previous = it->session->health();
        if (it->entry != entry || !previous.closed || !previous.checkpointRequired
            || !previous.observed.document.sameDocument(identity.document)) continue;
        // A cancelled close must keep the old gap and the new full checkpoint
        // in the same lifetime owner. Replacing the controller strands the gap
        // even after Retry, because its closed producer can never repair it.
        const auto others = retained - std::min(retained, previous.retainedBytes);
        if (others >= config.retainedBytes)
        { admissionFailure = { Error::budgetExceeded, "host_retained_budget" }; return finish(admissionFailure); }
        const auto status = it->session->resumePostCore(config.retainedBytes - others);
        if (!status) { admissionFailure = status; return status; }
        active = std::move(*it); departed.erase(it);
        admissionFailure = {};
        return {};
    }
    if (departed.size() >= config.residentSessions)
    { admissionFailure = { Error::budgetExceeded, "host_session_budget" }; return finish(admissionFailure); }
    if (retained >= config.retainedBytes)
    { admissionFailure = { Error::budgetExceeded, "host_retained_budget" }; return finish(admissionFailure); }
    auto settings = config.session;
    settings.retainedBytes = std::min(settings.retainedBytes, config.retainedBytes - retained);
    settings.producer.queue.retainedBytes = std::min(settings.producer.queue.retainedBytes, settings.retainedBytes);
    OwnedSession next; next.entry = entry;
    const auto status = RecoveryProtectionSession::attachPostCore(document, std::move(settings), next.session,
        document.hasUnsavedChanges() ? RecoveryMutationProducer::Admission::checkpointDirty
                                    : RecoveryMutationProducer::Admission::observeClean);
    if (!status) { admissionFailure = status; return status; }
    active = std::move(next);
    admissionFailure = {};
    return {};
}
catch (...) { fail({ Error::resourceOrEnvironmentFault, "host_admission_exception" }, RecoveryDiagnosticState::Stage::hostAdmission); return failure; }

void RecoverySessionHost::leaveDocument()
{
    if (!active) return;
    // Admission accounts for active + departed. Capacity was reserved before
    // subscription, so detaching cannot allocate or lose a pending queue.
    jassert(departed.size() < config.residentSessions);
    active->session->close();
    departed.push_back(std::move(*active));
    active.reset();
}

bool RecoverySessionHost::releaseEpochs(OwnedSession& item)
{
    const auto before = item.session->health();
    // takeDrainedEpochs erases producers. Reserve the entire possible result
    // and leave one slot for the closed session's final current actor first.
    if (releasedEpochs.size() >= config.metadataEvents
        || before.retired.size() + 1 > config.metadataEvents - releasedEpochs.size())
    {
        metadataBacklog = true;
        if (!metadataFailureReported)
        {
            note(RecoveryDiagnosticState::Stage::hostMetadata, { Error::budgetExceeded, "host_metadata_backlog" });
            metadataFailureReported = true;
        }
        return false;
    }
    releasedEpochs.reserve(releasedEpochs.size() + before.retired.size() + 1);
    auto completed = item.session->takeDrainedEpochs();
    for (auto& value : completed) releasedEpochs.push_back({ item.entry, std::move(value) });
    return true;
}

void RecoverySessionHost::poll() try
{
    metadataBacklog = false;
    if (active && active->session->health().closed) leaveDocument();
    if (active) releaseEpochs(*active);
    for (auto it = departed.begin(); it != departed.end();)
    {
        if (!releaseEpochs(*it)) { ++it; continue; }
        const auto value = it->session->health();
        const auto& current = value.current.writer;
        const bool cancelled = value.retired.empty()
            && current.cancellation == AsyncRecoveryWriter::Cancellation::completed
            && !current.workerRunning && current.pendingPackets == 0 && current.retainedBytes == 0;
        if (!cancelled && !it->session->drain(std::chrono::milliseconds(0))) { ++it; continue; }
        if (!value.current.owner.isNull()) releasedEpochs.push_back({ it->entry, value.current });
        it = departed.erase(it);
    }
    if (!metadataBacklog) metadataFailureReported = false;
}
catch (...) { fail({ Error::resourceOrEnvironmentFault, "host_poll_exception" }, RecoveryDiagnosticState::Stage::hostPoll); }

RecoverySessionHost::Health RecoverySessionHost::health() const
{
    Health result; result.failure = failure; result.closing = closing;
    if (result.failure && !admissionFailure) result.failure = admissionFailure;
    if (result.failure && metadataBacklog) result.failure = { Error::budgetExceeded, "host_metadata_backlog" };
    result.pendingMetadata = releasedEpochs.size(); result.pendingRetirements = savedRetirements.size();
    const auto add = [&] (const OwnedSession& item, bool isActive)
    {
        auto value = item.session->health();
        result.pendingPackets += value.pendingPackets; result.retainedBytes += value.retainedBytes;
        if (result.failure && !value.failure) result.failure = value.failure;
        result.sessions.push_back({ item.entry, isActive, std::move(value) });
    };
    if (active) add(*active, true);
    for (const auto& item : departed) add(item, false);
    // Completed metadata/tickets must still be handed to the service, which
    // accounts for its own storage jobs before authorising a final clean close.
    result.settled = closing && !active && departed.empty();
    return result;
}

Status RecoverySessionHost::retry(const juce::Uuid& owner, bool collectOrphans)
{
    const auto tryOne = [&] (OwnedSession& item) { return item.session->retry(owner, collectOrphans); };
    if (active)
    {
        const auto status = tryOne(*active);
        if (status.code != "session_retry_unknown_owner") return status;
    }
    for (auto& item : departed)
    {
        const auto status = tryOne(item);
        if (status.code != "session_retry_unknown_owner") return status;
    }
    const Status status { Error::staleHead, "host_retry_unknown_owner" };
    note(RecoveryDiagnosticState::Stage::hostRetry, status, owner);
    return status;
}

Status RecoverySessionHost::checkpointCurrent()
{
    if (active && !closing) return active->session->checkpointCurrent();
    const Status status { Error::staleHead, "host_no_active_document" };
    note(RecoveryDiagnosticState::Stage::hostCheckpoint, status);
    return status;
}

void RecoverySessionHost::beginClose()
{
    leaveDocument(); closing = true; poll();
}

Status RecoverySessionHost::cancelClose(const juce::Uuid& entry)
{
    closing = false;
    if (entry.isNull()) return {}; // Empty workspace resumes without inventing a Document.
    return attachPostCore(entry);
}

void RecoverySessionHost::saveCompleted(const SingleTrackDocument::SaveSnapshot& snapshot,
    const DocumentSaveCoordinator::Result& result, bool adopted) try
{
    if (!adopted || !result.succeeded || snapshot.intent == SingleTrackDocument::SaveIntent::exportCopy) return;
    // An adopted Save of an earlier snapshot may leave later edits dirty. That
    // is a normal checkpoint transition, not a failure or disposal authority.
    if (document.hasUnsavedChanges()
        || !document.getMediaIdentity().document.sameVersion(snapshot.documentIdentity)) return;
    const auto consider = [&] (OwnedSession& item)
    {
        const auto status = item.session->health();
        const auto one = [&] (const RecoveryMutationProducer::Health& epoch)
        {
            if (epoch.owner.isNull() || !epoch.observedIdentity.sameDocument(snapshot.documentIdentity)) return;
            if (epoch.writer.cancellation == AsyncRecoveryWriter::Cancellation::requested
                || epoch.writer.cancellation == AsyncRecoveryWriter::Cancellation::completed) return;
            auto queued = std::find_if(savedRetirements.begin(), savedRetirements.end(),
                [&] (const auto& ticket) { return ticket->ownerId() == epoch.owner; });
            if (queued == savedRetirements.end() && savedRetirements.size() >= config.savedTickets)
            { fail({ Error::budgetExceeded, "host_retirement_backlog" }, RecoveryDiagnosticState::Stage::hostSavedDisposition,
                   epoch.owner, snapshot.requestSerial); return; }
            const auto index = static_cast<size_t>(queued - savedRetirements.begin());
            if (index == savedRetirements.size()) savedRetirements.reserve(savedRetirements.size() + 1);
            std::unique_ptr<SavedOwnerRetirement> ticket;
            const auto prepared = item.session->prepareSavedRetirement(epoch.owner, snapshot, result, ticket);
            if (prepared)
            {
                if (index < savedRetirements.size()) savedRetirements[index] = std::move(ticket);
                else savedRetirements.push_back(std::move(ticket));
            }
            else
            {
                const auto cancelled = item.session->cancelSavedQueue(epoch.owner, snapshot, result);
                if (!cancelled) fail(cancelled, RecoveryDiagnosticState::Stage::hostSavedDisposition, epoch.owner, snapshot.requestSerial);
            }
        };
        one(status.current);
        for (const auto& epoch : status.retired) one(epoch);
    };
    if (active) consider(*active);
    for (auto& item : departed) consider(item);
    // No takeDrainedEpochs here until every exact producer has been examined.
}
catch (...) { fail({ Error::resourceOrEnvironmentFault, "host_saved_disposition_exception" },
                  RecoveryDiagnosticState::Stage::hostSavedDisposition, juce::Uuid(nullptr), snapshot.requestSerial); }

std::vector<std::unique_ptr<SavedOwnerRetirement>> RecoverySessionHost::takeSavedRetirements()
{
    std::vector<std::unique_ptr<SavedOwnerRetirement>> result;
    result.swap(savedRetirements);
    return result;
}

std::vector<RecoverySessionHost::ReleasedEpoch> RecoverySessionHost::takeReleasedEpochs()
{
    std::vector<ReleasedEpoch> result;
    result.swap(releasedEpochs);
    return result;
}
}
