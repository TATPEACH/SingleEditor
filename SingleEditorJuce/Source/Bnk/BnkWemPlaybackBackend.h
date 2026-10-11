#pragma once

#include "Bnk/BnkPlaybackBackend.h"

namespace singleeditor
{
class BnkWemPlaybackBackend final : public IBnkPlaybackBackend
{
public:
    void setMediaResolveMode(BnkMediaResolveMode mode) override { mediaResolveMode = mode; }
    BnkPreparedPlayback preparePreview(const BnkDocument::State& state,
                                       const BnkDocument::EventDescriptor& event,
                                       const BnkPlaybackContext& context,
                                       media::OptionalMediaOperation::Ptr operation = {}) override;
    BnkPreparedPlayback prepareMediaPreview(const BnkDocument::State& state,
                                            const BnkDocument::PlayableMediaDescriptor& media,
                                            media::OptionalMediaOperation::Ptr operation = {}) override;

private:
    friend class BnkPreviewRequest;
    bool resolvedInputs = false;
    BnkMediaResolveMode mediaResolveMode = BnkMediaResolveMode::decodePackedMedia;
};
} // namespace singleeditor
