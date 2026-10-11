#pragma once
#include "Bnk/BnkPlaybackBackend.h"

namespace singleeditor
{
// Logical selection copied by the UI, resolved and executed only by its worker.
// Keep this same request for a reviewed retry. Its random seed is never a key:
// the key binds the actual chosen member and guarded physical dependencies.
class BnkPreviewRequest final
{
public:
    using Ptr = std::shared_ptr<const BnkPreviewRequest>;
    static Ptr forEvent(BnkDocument::StatePtr, uint32_t eventId, BnkPlaybackContext, BnkMediaResolveMode);
    static Ptr forMedia(BnkDocument::StatePtr, BnkDocument::PlayableMediaDescriptor, BnkMediaResolveMode);
    BnkPreparedPlayback prepare(const media::OptionalMediaOperation::Ptr&) const;
    BnkDocument::StatePtr bankState() const noexcept { return bank; }
    std::optional<uint32_t> eventId() const { return eventIndex ? std::optional<uint32_t>(bank->events[*eventIndex].id) : std::nullopt; }
    std::optional<uint32_t> mediaId() const { return rawMedia ? std::optional<uint32_t>(rawMedia->mediaId) : std::nullopt; }
    const BnkPlaybackContext& playbackContext() const noexcept { return context; }
    BnkMediaResolveMode resolveMode() const noexcept { return mode; }
private:
    BnkDocument::StatePtr bank;
    std::optional<size_t> eventIndex;
    std::optional<BnkDocument::PlayableMediaDescriptor> rawMedia;
    BnkPlaybackContext context;
    BnkMediaResolveMode mode = BnkMediaResolveMode::decodePackedMedia;
    int64_t randomSeed = 0;
};
}
