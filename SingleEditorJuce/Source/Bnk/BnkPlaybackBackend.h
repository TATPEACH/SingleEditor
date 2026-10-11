#pragma once

#include <memory>

#include "Bnk/BnkDocument.h"
#include "Document/SingleTrackDocument.h"
#include "Audio/OptionalMediaOperation.h"

namespace singleeditor
{
enum class BnkMediaResolveMode
{
    decodePackedMedia = 0,
    sourceMedia
};

struct BnkPreparedPlayback
{
    std::shared_ptr<SingleTrackDocument::State> state;
    juce::String detail;
    juce::StringArray diagnostics;

    bool isValid() const noexcept { return state != nullptr && state->info.isValid() && state->audioBuffer != nullptr; }
};

struct BnkPlaybackContext
{
    juce::String switchValueToken;
    juce::String stateValueToken;
    juce::String rtpcToken;
    float rtpcValue = 0.0f;
};

class IBnkPlaybackBackend
{
public:
    virtual ~IBnkPlaybackBackend() = default;

    virtual void setMediaResolveMode(BnkMediaResolveMode mode) = 0;

    virtual BnkPreparedPlayback preparePreview(const BnkDocument::State& state,
                                               const BnkDocument::EventDescriptor& event,
                                               const BnkPlaybackContext& context,
                                               media::OptionalMediaOperation::Ptr operation = {}) = 0;

    virtual BnkPreparedPlayback prepareMediaPreview(const BnkDocument::State& state,
                                                    const BnkDocument::PlayableMediaDescriptor& media,
                                                    media::OptionalMediaOperation::Ptr operation = {}) = 0;
};
} // namespace singleeditor
