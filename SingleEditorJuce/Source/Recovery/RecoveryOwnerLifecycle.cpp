#include "RecoveryOwnerLifecycle.h"
#include "RecoveryMutationProducer.h"

namespace singleeditor::recovery
{
bool RecoveryOwnerLifecycle::currentSave(const SingleTrackDocument& document, const SingleTrackDocument::SaveSnapshot& snapshot,
    const SingleTrackDocument::SaveWriteResult& result)
{
    const auto identity = document.getMediaIdentity();
    return result.succeeded && result.publication && result.publication->matches(snapshot)
        && result.targetFile == snapshot.targetFile && result.changeGeneration == snapshot.changeGeneration
        && snapshot.intent != SingleTrackDocument::SaveIntent::exportCopy && !document.hasUnsavedChanges()
        && identity.document.sameVersion(snapshot.documentIdentity)
        && identity.savedGeneration == snapshot.changeGeneration && identity.source.location.file == snapshot.targetFile;
}
Status RecoveryOwnerLifecycle::prepareSaved(const SingleTrackDocument& document,
    const SingleTrackDocument::SaveSnapshot& snapshot, const SingleTrackDocument::SaveWriteResult& result,
    const RecoveryMutationProducer& producer, std::unique_ptr<SavedOwnerRetirement>& output) try
{
    const auto identity = document.getMediaIdentity();
    const auto protection = producer.health();
    if (output || !currentSave(document, snapshot, result))
        return { Error::staleHead, "retirement_save_not_current" };
    if (protection.owner.isNull() || protection.phase != RecoveryMutationProducer::Phase::retired
        || (protection.retirementReason != "saved" && protection.retirementReason != "source_rebound")
        || !protection.failure || protection.checkpointRequired || protection.writer.accepting
        || protection.writer.workerRunning || protection.writer.pendingPackets != 0 || protection.writer.safeHead.sequence < 1
        || protection.writer.cancellation != AsyncRecoveryWriter::Cancellation::none
        || !protection.observedIdentity.sameVersion(identity.document)
        || !protection.writer.protectedIdentity.sameVersion(identity.document))
        return { Error::staleHead, "retirement_owner_not_settled" };
    auto candidate = std::unique_ptr<SavedOwnerRetirement>(new SavedOwnerRetirement());
    candidate->document = identity.document;
    candidate->root = producer.config.root; candidate->owner = protection.owner; candidate->head = protection.writer.safeHead;
    // The actual published target has no retained original-reader lease.
    candidate->target = result.publication->targetVersion();
    if (!candidate->target || candidate->target->retainedInput()) return { Error::invalidPacket, "retirement_save_target" };
    candidate->nonce = juce::Uuid().toString();
    output = std::move(candidate);
    return {};
}
catch (...) { return { Error::resourceOrEnvironmentFault, "retirement_save_prepare_exception" }; }

Status RecoveryOwnerLifecycle::cancelSavedQueue(const SingleTrackDocument& document,
    const SingleTrackDocument::SaveSnapshot& snapshot, const SingleTrackDocument::SaveWriteResult& result,
    RecoveryMutationProducer& producer) try
{
    if (!currentSave(document, snapshot, result)) return { Error::staleHead, "cancellation_save_not_current" };
    const auto protection = producer.health();
    if (!producer.writer || protection.owner.isNull() || protection.writer.accepting
        || !protection.observedIdentity.sameDocument(snapshot.documentIdentity))
        return { Error::staleHead, "cancellation_owner_scope" };
    const auto target = result.publication->targetVersion();
    if (!target || target->retainedInput()) return { Error::invalidPacket, "cancellation_save_target" };
    // Capture only physical metadata, never SaveSnapshot/State/producer/Document.
    // Its guard is returned to the worker and held until queued captures release.
    return producer.writer->cancelAfterSaved([target] (std::shared_ptr<void>& guard) -> Status
    {
        std::unique_ptr<media::SourceFileLease> lease;
        const auto opened = media::SourceFileLease::open(target->file, lease);
        if (opened.failed() || !lease->witness()->sameFileVersion(*target))
            return { Error::sourceConflict, "cancellation_saved_target_changed" };
        guard = std::shared_ptr<media::SourceFileLease>(std::move(lease));
        return {};
    });
}
catch (...) { return { Error::resourceOrEnvironmentFault, "cancellation_prepare_exception" }; }

Status RecoveryOwnerLifecycle::retireSaved(RecoveryStore& store, const SafeHead& head,
    const SavedOwnerRetirement& saved, OwnerRetirement& output) try
{
    if (store.rootDirectory() != saved.root || store.ownerId() != saved.owner || head != saved.head)
        return { Error::staleHead, "retirement_owner_scope" };
    std::unique_ptr<media::SourceFileLease> target;
    const auto opened = media::SourceFileLease::open(saved.target->file, target);
    if (opened.failed() || !target->witness()->sameFileVersion(*saved.target))
        return { Error::sourceConflict, "retirement_saved_target_changed" };
    auto* facts = new juce::DynamicObject();
    facts->setProperty("path", saved.target->file.getFullPathName());
    facts->setProperty("volume", saved.target->volumeId); facts->setProperty("file_id", saved.target->fileId);
    facts->setProperty("size", saved.target->sizeBytes); facts->setProperty("modified", saved.target->modifiedTicks);
    facts->setProperty("changed", saved.target->changedTicks);
    auto* evidence = new juce::DynamicObject();
    evidence->setProperty("reason", "saved"); evidence->setProperty("document", saved.document.toJson());
    evidence->setProperty("target", juce::var(facts));
    // A normal source lease denies write/delete sharing for the whole atomic
    // retirement transaction, including its crash-injection boundaries.
    return store.publishRetirement(head, saved.nonce, juce::var(evidence), output);
}
catch (...) { return { Error::resourceOrEnvironmentFault, "retirement_save_exception" }; }

Status RecoveryOwnerLifecycle::discard(RecoveryStore& store, const SafeHead& head, OwnerRetirement& output)
{
    const auto view = store.inspectManifest();
    if (!view.status) return view.status;
    if (view.head != head || view.commits.empty()) return { Error::staleHead, "retirement_discard_head" };
    auto* evidence = new juce::DynamicObject();
    evidence->setProperty("reason", "discarded"); evidence->setProperty("document", view.commits.back().record["document"]);
    evidence->setProperty("target", juce::var());
    return store.publishRetirement(head, juce::Uuid().toString(), juce::var(evidence), output);
}

Status RecoveryOwnerLifecycle::discard(RecoveryStore& store, const OwnerDiscardReview& review, OwnerRetirement& output) try
{
    if (store.rootDirectory() != review.root || store.ownerId() != review.owner)
        return { Error::staleHead, "discard_review_owner_scope" };
    const auto view = store.inspectManifest();
    if (!view.status) return view.status;
    if (view.head != review.head || view.commits.empty()) return { Error::staleHead, "discard_review_head_changed" };
    auto* evidence = new juce::DynamicObject();
    evidence->setProperty("reason", review.retention > 0 ? "expired" : "discarded");
    evidence->setProperty("document", view.commits.back().record["document"]);
    if (review.retention > 0)
    {
        auto* expiry = new juce::DynamicObject();
        expiry->setProperty("retention_ms", review.retention); expiry->setProperty("reviewed_at", review.reviewedAt);
        if (review.retentionPolicyHash.isNotEmpty()) expiry->setProperty("policy_hash", review.retentionPolicyHash);
        expiry->setProperty("last_activity", review.latestActivity); evidence->setProperty("target", juce::var(expiry));
    }
    else evidence->setProperty("target", juce::var());
    return store.publishRetirement(review.head, juce::Uuid().toString(), juce::var(evidence), output,
                                   review.rootId, review.directoryId);
}
catch (...) { return { Error::resourceOrEnvironmentFault, "discard_review_retirement_exception" }; }
}
