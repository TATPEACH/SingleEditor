#pragma once

#include "RecoveryStore.h"
#include "Document/SingleTrackDocument.h"

namespace singleeditor::recovery
{
class RecoveryMutationProducer;
// No caller-writable success bit can authorize saved-owner retirement. This
// packet is prepared from the actual native receipt and clean Document version;
// it retains metadata, not State, PCM, playback readers or source input handles.
class SavedOwnerRetirement final
{
public:
    const juce::File& rootDirectory() const noexcept { return root; }
    juce::Uuid ownerId() const noexcept { return owner; }
    const SafeHead& frozenHead() const noexcept { return head; }
private:
    friend class RecoveryOwnerLifecycle;
    SavedOwnerRetirement() = default;
    media::DocumentIdentity document;
    juce::File root;
    juce::Uuid owner { nullptr };
    SafeHead head;
    media::SourceFileWitness::Ptr target;
    juce::String nonce;
};

class RecoveryOwnerLifecycle final
{
public:
    // Owner-thread-only, no I/O. Requires the exact clean native save and its
    // retired, fully drained producer; binds that root/owner/head. State version
    // numbers are not time order (Undo can lower them).
    static Status prepareSaved(const SingleTrackDocument&, const SingleTrackDocument::SaveSnapshot&,
        const SingleTrackDocument::SaveWriteResult&, const RecoveryMutationProducer&, std::unique_ptr<SavedOwnerRetirement>&);
    // Independent runtime-only authority: actual current clean Save may cancel
    // the closed queue of this same Document's exact producer, even if an older
    // prefix failed or an Undo lowered its state number. Worker rechecks/holds
    // the saved target, finishes any in-flight item, then releases the tail.
    // No disk deletion or SavedOwnerRetirement ticket is issued by cancellation.
    static Status cancelSavedQueue(const SingleTrackDocument&, const SingleTrackDocument::SaveSnapshot&,
        const SingleTrackDocument::SaveWriteResult&, RecoveryMutationProducer&);
    // Explicit worker calls. Saved target is held and revalidated through marker
    // publication. Both operations require the exact frozen head and no writer
    // or live recovered dependency. They publish a terminal marker, not deletion.
    static Status retireSaved(RecoveryStore&, const SafeHead&, const SavedOwnerRetirement&, OwnerRetirement&);
    // Invoke only for the user's explicit selected-owner Discard action. Current
    // implementation requires a valid frozen chain; corrupt-owner disposal is separate.
    static Status discard(RecoveryStore&, const SafeHead&, OwnerRetirement&);
    // Second-confirmed selected-owner operation. Revalidates the reviewed
    // physical directories and exact head inside the retirement transaction.
    static Status discard(RecoveryStore&, const OwnerDiscardReview&, OwnerRetirement&);
private:
    static bool currentSave(const SingleTrackDocument&, const SingleTrackDocument::SaveSnapshot&,
        const SingleTrackDocument::SaveWriteResult&);
};
}
