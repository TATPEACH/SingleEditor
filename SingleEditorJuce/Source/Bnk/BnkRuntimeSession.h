#pragma once

#include "Bnk/BnkPreviewRequest.h"
#include <functional>
#include <unordered_map>

namespace singleeditor
{
// Created only for an admitted bank or its retained selection. A moved locator
// still needs an owner for release, but grants no fresh source access.
// UI values are frozen at submit;
// detached preparation owns no GUI pointer and cannot publish a Document.
class BnkRuntimeSession final
{
public:
    struct PreviewIntent final
    {
        BnkPreviewRequest::Ptr request;
        media::OptionalMediaOperation::Ptr operation;
        SingleTrackDocument::WorkspaceSnapshot::Ptr previousWorkspace;
        SingleTrackDocument::StatePtr previousState;
        std::optional<juce::Range<double>> previousSelection;
        std::optional<juce::Uuid> previousEntry;
        std::optional<uint32_t> event, media;
        int64_t token = 0;
        bool autoPlay = false, previousWasEmpty = false;
    };
    using Ptr = std::shared_ptr<BnkRuntimeSession>;
    using Admission = std::function<bool(const PreviewIntent&, bool starting)>;
    using Delivery = std::function<void(const PreviewIntent&, BnkPreparedPlayback&&, bool prepared)>;

    static Ptr create(BnkDocument&, uint64_t generation);
    ~BnkRuntimeSession();
    BnkRuntimeSession(const BnkRuntimeSession&) = delete;
    BnkRuntimeSession& operator=(const BnkRuntimeSession&) = delete;

    // A sidecar refresh may replace the parsed state within the same generation.
    // A different generation discards old context and cancels outstanding work.
    void bind(uint64_t generation);
    uint64_t generation() const noexcept { return documentGeneration; }
    void release();

    BnkPlaybackContext context() const { return selectedContext; }
    void setContext(BnkPlaybackContext, std::optional<uint32_t> event = {});
    juce::String preferredSwitch(std::optional<uint32_t>) const;
    juce::String preferredState(std::optional<uint32_t>) const;
    bool selectionMatches(const PreviewIntent&, BnkMediaResolveMode) const;

    void queue(PreviewIntent);
    // All callbacks execute synchronously on the caller (UI) thread. Admission
    // retains the existing UI/workspace fence; delivery never runs on a worker.
    void poll(const Admission&, const Delivery&);
    bool cancel(int64_t currentToken = 0);
    bool isPreparing() const noexcept;
    const PreviewIntent* lastPreview() const noexcept;

#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    using Prepare = std::function<BnkPreparedPlayback(const BnkPreviewRequest::Ptr&,
                                                     const media::OptionalMediaOperation::Ptr&)>;
    static Ptr createForTesting(BnkDocument&, uint64_t, Prepare);
    static int factoryRequestsForTesting() noexcept;
    static int liveInstancesForTesting() noexcept;
    std::weak_ptr<const void> workerForTesting() const;
    bool workerFinishedForTesting() const;
#endif

private:
    struct Work;
    explicit BnkRuntimeSession(BnkDocument&, uint64_t);
    BnkDocument& document;
    BnkDocument::StatePtr bankLease;
    uint64_t documentGeneration = 0;
    BnkPlaybackContext selectedContext;
    std::unordered_map<uint32_t, juce::String> switches, states;
    std::shared_ptr<Work> active;
    std::optional<PreviewIntent> queued;
    bool released = false;
#if SINGLE_EDITOR_BNK_SESSION_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    Prepare prepareForTesting;
#endif
};
}
