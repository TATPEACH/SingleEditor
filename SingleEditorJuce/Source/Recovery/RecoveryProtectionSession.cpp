#include "RecoveryProtectionSession.h"
#include "RecoveryOwnerLifecycle.h"
#include <algorithm>
#include <limits>

namespace singleeditor::recovery
{
RecoveryProtectionSession::RecoveryProtectionSession(SingleTrackDocument& doc, Config settings,
    media::IdentitySnapshot identity) : document(doc), config(std::move(settings)), observed(std::move(identity)) {}

Status RecoveryProtectionSession::attachPostCore(SingleTrackDocument& doc, Config settings,
    std::unique_ptr<RecoveryProtectionSession>& output, RecoveryMutationProducer::Admission admission)
{
    const auto diagnostics = settings.producer.queue.diagnostics;
    const auto operation = diagnostics ? juce::Uuid() : juce::Uuid(nullptr);
    const auto finish = [&](Status status)
    {
        if (diagnostics && !status)
            diagnostics->record({ operation, juce::Uuid(nullptr), RecoveryDiagnosticState::Stage::sessionAdmission, status.error, {} });
        return status;
    };
    try
    {
        const auto identity = doc.getMediaIdentity();
        if (output || !identity.document.isValid() || identity.source.binding.isNull()
            || settings.producer.root == juce::File() || settings.retiredEpochs == 0 || settings.retiredEpochs > 128
            || settings.retainedBytes == 0)
            return finish({ Error::invalidPacket, "session_admission" });
        const bool dirty = identity.document.generation != identity.savedGeneration;
        if (admission == RecoveryMutationProducer::Admission::observeClean && dirty)
            return finish({ Error::staleHead, "session_dirty_admission" });
        if ((admission != RecoveryMutationProducer::Admission::observeClean
                && admission != RecoveryMutationProducer::Admission::checkpointDirty)
            || (admission == RecoveryMutationProducer::Admission::checkpointDirty && !dirty))
            return finish({ Error::invalidPacket, "session_admission_mode" });
        std::optional<SingleTrackDocument::CommittedStateSnapshot> initial;
        if (dirty)
        {
            initial = doc.captureCommittedState(identity.document);
            if (!initial || !initial->identity.source.sameBinding(identity.source)
                || initial->identity.savedGeneration != identity.savedGeneration)
                return finish({ Error::staleHead, "session_admission_changed" });
        }
        auto candidate = std::unique_ptr<RecoveryProtectionSession>(new RecoveryProtectionSession(doc, std::move(settings), identity));
        candidate->observationId = operation;
        auto status = candidate->startEpoch(identity, std::move(initial));
        if (!status) return finish(status);
        candidate->capture = std::make_unique<DocumentRecoveryCapture>(doc,
            [session = candidate.get()] (auto event) { session->consume(std::move(event)); },
            [session = candidate.get()] (const Status& failure, uint64_t sequence)
            { session->note(session->observationId, RecoveryDiagnosticState::Stage::sessionCapture, failure, sequence); });
        output = std::move(candidate);
        return {};
    }
    catch (...) { return finish({ Error::resourceOrEnvironmentFault, "session_admission_exception" }); }
}

RecoveryProtectionSession::~RecoveryProtectionSession() { close(); }

Status RecoveryProtectionSession::checkRetained(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
    RecoveryMutationProducer& producer) const
{
    size_t incoming = 0;
    auto status = producer.projectedRetained(snapshot, incoming);
    if (!status) return status;
    const auto refusal = [&]() -> Status
    {
        return byteCapacityFailure("session_retained_budget", ByteCapacity::Resource::retainedMemory,
            incoming, config.retainedBytes, true);
    };
    // The projection already includes this producer's current queue. Other
    // source epochs remain fully charged even if they reference the same audio.
    const auto include = [&](const RecoveryMutationProducer* other)
    {
        if (!other || other == &producer) return true;
        const auto bytes = other->health().writer.retainedBytes;
        if (bytes > std::numeric_limits<size_t>::max() - incoming) return false;
        incoming += bytes; return true;
    };
    if (incoming > config.retainedBytes) return refusal();
    if (!include(current.get())) return { Error::budgetExceeded, "session_retained_budget" };
    if (incoming > config.retainedBytes) return refusal();
    for (const auto& epoch : retired)
    {
        if (!include(epoch.get())) return { Error::budgetExceeded, "session_retained_budget" };
        if (incoming > config.retainedBytes) return refusal();
    }
    return {};
}

Status RecoveryProtectionSession::startEpoch(const media::IdentitySnapshot& identity,
    std::optional<SingleTrackDocument::CommittedStateSnapshot> initial)
{
    // A clean observer has no owner to retire. Failed/pending owners are never
    // silently evicted to make room for a replacement.
    const bool retainPrevious = current && !current->health().owner.isNull();
    if (retainPrevious && retired.size() >= config.retiredEpochs)
        return { Error::budgetExceeded, "session_epoch_budget" };
    auto next = std::unique_ptr<RecoveryMutationProducer>(new RecoveryMutationProducer(document, config.producer, identity, false));
    if (initial)
    {
        const auto allowed = checkRetained(*initial, *next);
        if (!allowed) return allowed;
    }
    // Reserve before accepting any work or retiring the current observer.
    if (retainPrevious) retired.reserve(retired.size() + 1);
    if (initial)
    {
        const auto submitted = next->enqueue(std::move(*initial));
        if (!submitted) return submitted;
    }
    if (current) current->close();
    if (retainPrevious) retired.push_back(std::move(current));
    current = std::move(next);
    return {};
}

void RecoveryProtectionSession::fail(Status reason)
{
    if (!checkpointRequired)
    {
        note(observationId, RecoveryDiagnosticState::Stage::sessionTransition, reason, eventSequence);
        failure = std::move(reason);
    }
    checkpointRequired = true;
    if (current && current->retirementReason.isEmpty()) current->rejectGap(failure);
}

void RecoveryProtectionSession::note(const juce::Uuid& operation, RecoveryDiagnosticState::Stage stage,
    const Status& status, uint64_t sequence) const noexcept
{
    if (!config.producer.queue.diagnostics || status) return;
    config.producer.queue.diagnostics->record({ operation, current ? current->owner : juce::Uuid(nullptr), stage, status.error,
        { static_cast<int64_t>(sequence), static_cast<int64_t>(retired.size()), current ? 1 : 0,
          checkpointRequired ? 1 : 0, closed ? 1 : 0, 0 } });
}

void RecoveryProtectionSession::consume(DocumentRecoveryCapture::Capture event)
{
    if (closed) return;
    eventSequence = event.event.sequence;
    const auto& change = event.event;
    if (!change.before.document.sameVersion(observed.document)
        || !change.before.source.sameBinding(observed.source))
    { fail({ Error::staleHead, "session_event_discontinuity" }); return; }
    observed = change.after;
    if (change.kind != media::IdentityEventKind::generationChanged
        && change.kind != media::IdentityEventKind::saveCheckpoint
        && change.kind != media::IdentityEventKind::sourceRebound)
    {
        if (current) current->consume(std::move(event));
        closed = true;
        if (capture) capture->stop(); // Do not destroy a callback's own object.
        return; // A different document needs its own post-Core admission.
    }
    if (!change.after.document.sameDocument(change.before.document) || !event.document)
    { fail({ Error::staleHead, "session_capture_changed" }); return; }
    if (checkpointRequired) return;
    try
    {
        if (change.kind == media::IdentityEventKind::generationChanged)
        {
            const auto allowed = checkRetained(*event.document, *current);
            if (!allowed) { fail(allowed); return; }
            current->consume(std::move(event));
            const auto state = current->health();
            if (state.checkpointRequired) fail(state.failure);
            return;
        }
        // The same immutable notification supplies the next epoch's checkpoint;
        // no listener replacement, asynchronous gap or invented edit is involved.
        current->consume(event);
        auto initial = change.after.document.generation != change.after.savedGeneration
            ? std::move(event.document) : std::nullopt;
        const auto admitted = startEpoch(change.after, std::move(initial));
        if (!admitted) fail(admitted);
    }
    catch (...) { fail({ Error::resourceOrEnvironmentFault, "session_transition_exception" }); }
}

RecoveryProtectionSession::Health RecoveryProtectionSession::health() const
{
    Health result;
    result.observed = observed; result.failure = failure;
    result.checkpointRequired = checkpointRequired; result.closed = closed;
    const auto include = [&] (const RecoveryMutationProducer::Health& value)
    {
        result.pendingPackets += value.writer.pendingPackets;
        result.retainedBytes += value.writer.retainedBytes;
        if (result.failure && !value.failure) result.failure = value.failure;
        result.checkpointRequired = result.checkpointRequired || value.checkpointRequired;
    };
    if (current)
    {
        result.current = current->health(); include(result.current);
        result.currentProtected = !closed && !checkpointRequired && !result.current.checkpointRequired && result.current.failure
            && result.current.writer.cancellation == AsyncRecoveryWriter::Cancellation::none
            && result.current.writer.pendingPackets == 0
            && result.current.writer.protectedIdentity.sameVersion(observed.document);
    }
    for (const auto& epoch : retired)
    {
        result.retired.push_back(epoch->health()); include(result.retired.back());
    }
    if (capture && !capture->latestFailure())
    { if (result.failure) result.failure = capture->latestFailure(); result.checkpointRequired = true; result.currentProtected = false; }
    return result;
}

Status RecoveryProtectionSession::retry(const juce::Uuid& owner, bool collectOrphans)
{
    if (owner.isNull()) return { Error::invalidPacket, "session_retry_owner" };
    const auto attempt = [&] (RecoveryMutationProducer& producer)
    {
        if (!producer.writer) return Status { Error::invalidPacket, "session_retry_no_writer" };
        // A capture gap does not invalidate its earlier accepted prefix. It can
        // drain, but only a fresh full checkpoint repairs the missed generation.
        return producer.writer->progress().accepting ? producer.retry(collectOrphans)
            : producer.writer->retryDraining(collectOrphans);
    };
    if (current && current->health().owner == owner) return attempt(*current);
    for (auto& epoch : retired) if (epoch->health().owner == owner) return attempt(*epoch);
    return { Error::staleHead, "session_retry_unknown_owner" };
}

Status RecoveryProtectionSession::checkpointCurrent()
{
    const auto operation = config.producer.queue.diagnostics ? juce::Uuid() : juce::Uuid(nullptr);
    const auto finish = [&](Status status)
    { note(operation, RecoveryDiagnosticState::Stage::sessionCheckpoint, status, eventSequence); return status; };
    try
    {
        if (closed || !capture || !capture->latestFailure()) return finish({ Error::staleHead, "session_observer_unavailable" });
        const auto identity = document.getMediaIdentity();
        if (!identity.document.sameVersion(observed.document) || !identity.source.sameBinding(observed.source)
            || identity.document.generation == identity.savedGeneration)
            return finish({ Error::staleHead, "session_checkpoint_identity" });
        auto state = document.captureCommittedState(identity.document);
        if (!state || !state->identity.source.sameBinding(identity.source)) return finish({ Error::staleHead, "session_checkpoint_changed" });
        const auto admitted = startEpoch(identity, std::move(state));
        if (admitted) { failure = {}; checkpointRequired = false; }
        return finish(admitted);
    }
    catch (...) { return finish({ Error::resourceOrEnvironmentFault, "session_checkpoint_exception" }); }
}

Status RecoveryProtectionSession::resumePostCore(size_t maximumRetainedBytes)
{
    const auto operation = config.producer.queue.diagnostics ? juce::Uuid() : juce::Uuid(nullptr);
    const auto finish = [&](Status status)
    { note(operation, RecoveryDiagnosticState::Stage::sessionAdmission, status); return status; };
    try
    {
        const auto identity = document.getMediaIdentity();
        if (!closed || !identity.document.isValid() || identity.source.binding.isNull()
            || !identity.document.sameDocument(observed.document))
            return finish({ Error::staleHead, "session_resume_identity" });
        const auto retainedLimit = std::min(config.retainedBytes, maximumRetainedBytes);
        if (retainedLimit == 0) return finish({ Error::budgetExceeded, "session_resume_budget" });
        std::optional<SingleTrackDocument::CommittedStateSnapshot> initial;
        if (identity.document.generation != identity.savedGeneration)
        {
            initial = document.captureCommittedState(identity.document);
            if (!initial || !initial->identity.source.sameBinding(identity.source)
                || initial->identity.savedGeneration != identity.savedGeneration)
                return finish({ Error::staleHead, "session_resume_changed" });
        }
        // Allocate the new observer before accepting work. The owner thread
        // cannot dispatch a Document edit during this synchronous admission.
        auto nextCapture = std::make_unique<DocumentRecoveryCapture>(document,
            [this](auto event) { consume(std::move(event)); },
            [this](const Status& status, uint64_t sequence)
            { note(observationId, RecoveryDiagnosticState::Stage::sessionCapture, status, sequence); });
        Status admitted;
        {
            const juce::ScopedValueSetter<size_t> limit(config.retainedBytes, retainedLimit);
            admitted = startEpoch(identity, std::move(initial));
        }
        if (!admitted) return finish(admitted);
        // The remaining Host budget constrains all future captures too. A
        // failed admission preserves the old configuration and exact actors.
        config.retainedBytes = retainedLimit;
        capture = std::move(nextCapture);
        observed = identity; observationId = operation; eventSequence = 0;
        failure = {}; checkpointRequired = false; closed = false;
        return {};
    }
    catch (...) { return finish({ Error::resourceOrEnvironmentFault, "session_resume_exception" }); }
}

Status RecoveryProtectionSession::prepareSavedRetirement(const juce::Uuid& owner,
    const SingleTrackDocument::SaveSnapshot& snapshot, const SingleTrackDocument::SaveWriteResult& saved,
    std::unique_ptr<SavedOwnerRetirement>& output) const
{
    if (owner.isNull()) return { Error::invalidPacket, "session_retirement_owner" };
    // A failed epoch transition can leave the retired actor in current. Always
    // use the original producer and the existing exact settled-epoch predicate.
    if (current && current->health().owner == owner)
        return RecoveryOwnerLifecycle::prepareSaved(document, snapshot, saved, *current, output);
    for (const auto& epoch : retired)
        if (epoch->health().owner == owner)
            return RecoveryOwnerLifecycle::prepareSaved(document, snapshot, saved, *epoch, output);
    return { Error::staleHead, "session_retirement_unknown_owner" };
}

Status RecoveryProtectionSession::cancelSavedQueue(const juce::Uuid& owner,
    const SingleTrackDocument::SaveSnapshot& snapshot, const SingleTrackDocument::SaveWriteResult& saved)
{
    if (owner.isNull()) return { Error::invalidPacket, "session_cancellation_owner" };
    if (current && current->health().owner == owner)
        return RecoveryOwnerLifecycle::cancelSavedQueue(document, snapshot, saved, *current);
    for (auto& epoch : retired)
        if (epoch->health().owner == owner)
            return RecoveryOwnerLifecycle::cancelSavedQueue(document, snapshot, saved, *epoch);
    return { Error::staleHead, "session_cancellation_unknown_owner" };
}

std::vector<RecoveryMutationProducer::Health> RecoveryProtectionSession::takeDrainedEpochs()
{
    std::vector<RecoveryMutationProducer::Health> completed;
    completed.reserve(retired.size()); // Allocate before erasing any owned actor.
    const auto latest = current ? current->health() : RecoveryMutationProducer::Health{};
    // A closed observer is no longer protecting live edits, but its exact final
    // checkpoint can still prove that an earlier runtime gap was superseded.
    // Use the actual last actor (not the replacement Document or version order).
    // This predicate releases runtime ownership only; it grants no disk rights.
    const bool finalCheckpointProtected = current && !checkpointRequired
        && (!capture || capture->latestFailure()) && !latest.owner.isNull()
        && !latest.checkpointRequired && latest.failure
        && latest.writer.cancellation == AsyncRecoveryWriter::Cancellation::none
        && !latest.writer.workerRunning && latest.writer.pendingPackets == 0
        && latest.writer.safeHead.sequence > 0
        && latest.writer.protectedIdentity.sameVersion(latest.observedIdentity);
    for (auto it = retired.begin(); it != retired.end();)
    {
        const auto old = (*it)->health();
        const bool cancelled = old.writer.cancellation == AsyncRecoveryWriter::Cancellation::completed
            && !old.writer.workerRunning && old.writer.pendingPackets == 0 && old.writer.retainedBytes == 0;
        const bool supersededGap = old.checkpointRequired && !old.writer.workerRunning
            && old.writer.pendingPackets == 0 && finalCheckpointProtected
            && old.observedIdentity.sameDocument(latest.observedIdentity);
        if (!cancelled && !supersededGap && !(*it)->drain(std::chrono::milliseconds(0))) { ++it; continue; }
        completed.push_back((*it)->health());
        it = retired.erase(it);
    }
    return completed;
}

bool RecoveryProtectionSession::drain(std::chrono::milliseconds budget)
{
    const auto start = std::chrono::steady_clock::now();
    budget = std::max(budget, std::chrono::milliseconds(0));
    bool drained = true;
    const auto one = [&] (RecoveryMutationProducer& epoch)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const auto remaining = elapsed >= budget ? std::chrono::milliseconds(0) : budget - elapsed;
        if (!epoch.drain(remaining)) drained = false;
    };
    for (auto& epoch : retired) one(*epoch);
    if (current) one(*current);
    return drained && !health().checkpointRequired;
}

void RecoveryProtectionSession::close()
{
    if (capture && !capture->latestFailure()) fail(capture->latestFailure());
    capture.reset(); closed = true;
    if (current) current->close();
    for (auto& epoch : retired) epoch->close();
}
}
