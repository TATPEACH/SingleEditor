#pragma once

#include <juce_core/juce_core.h>

#include "Bnk/BnkDocument.h"
#include "Bnk/BoundedSidecarInput.h"

namespace singleeditor
{
class BnkSidecarParser final
{
public:
    struct Result final
    {
        BoundedSidecarInput::Failure failure = BoundedSidecarInput::Failure::none;
        bool admissionDenied = false;
        std::vector<media::SourceFileWitness::Ptr> inputs;
        explicit operator bool() const noexcept { return failure == BoundedSidecarInput::Failure::none && !admissionDenied; }
    };
    // Only metadata fields are staged. HIRC payloads and the proven core model
    // stay in place; a failed/cancelled optional capability publishes no prefix.
    static Result enrichStateFromSidecars(const juce::File& bankFile,
        BnkDocument::State& state, const juce::File& projectRootOverride = {},
        std::function<bool()> checkpoint = {},
        BoundedSidecarInput::Profile profile = BoundedSidecarInput::Profile::conservative,
        std::shared_ptr<const BoundedSidecarInput::Catalog> catalog = {});
};
} // namespace singleeditor
