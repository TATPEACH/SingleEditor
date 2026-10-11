#pragma once

#include <memory>

#include <juce_core/juce_core.h>

#include "Bnk/BnkDocument.h"
#include "Bnk/BnkSidecarParser.h"

namespace singleeditor
{
class BnkParser final
{
public:
    using Failure = BoundedSidecarInput::Failure;
    using Profile = BoundedSidecarInput::Profile;
    enum class Stage { input, coreRead, coreObjects, sidecar, playbackHints, complete };
    struct Progress final
    {
        Stage stage = Stage::input;
        uint64_t bytesRead = 0;
        uint32_t objectsVisited = 0;
    };
    using Checkpoint = std::function<bool(const Progress&)>;
    struct Hooks final
    {
        // Runs under the actual bank lease, before any table body is parsed.
        std::function<bool(const media::SourceFileWitness&)> admitCore;
        // A value-owned request can freeze/disallow sidecar work independently.
        std::function<BnkSidecarParser::Result(BnkDocument::State&, const std::function<bool()>&)> sidecar;
    };
    struct Result final
    {
        std::shared_ptr<BnkDocument::State> state;
        Failure failure = Failure::none;
        bool admissionDenied = false;
        BnkSidecarParser::Result sidecar;
        // The caller may keep the actual bank input pinned until publication.
        // This is a read lease, never a retained Save/cleanup credential.
        std::unique_ptr<media::SourceFileLease> inputLease;
        explicit operator bool() const noexcept { return state != nullptr && failure == Failure::none; }
    };
    static Result parse(const juce::File& file, const juce::File& projectRootOverride = {},
                        Checkpoint checkpoint = {}, Profile profile = Profile::conservative, const Hooks& hooks = {});
    static std::shared_ptr<BnkDocument::State> parseFile(const juce::File& file, const juce::File& projectRootOverride = {});
};
} // namespace singleeditor
