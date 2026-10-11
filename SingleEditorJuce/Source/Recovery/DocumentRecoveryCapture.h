#pragma once

#include "Document/SingleTrackDocument.h"
#include "RecoveryStore.h"

namespace singleeditor::recovery
{
// Explicit post-Core subscription. Does not create a writer or read source/audio.
// Owner-thread-only, like Document's ListenerList. The consumer decides when to
// start/retire the owner and how to encode a packet.
class DocumentRecoveryCapture final : private SingleTrackDocument::Listener
{
public:
    struct Capture final
    {
        media::IdentityEvent event;
        std::optional<SingleTrackDocument::CommittedStateSnapshot> document;
    };
    using Sink = std::function<void(Capture)>;
    using FailureSink = std::function<void(const Status&, uint64_t)>;
    DocumentRecoveryCapture(SingleTrackDocument& source, Sink sink, FailureSink failures = {})
        : document(source), consume(std::move(sink)), reportFailure(std::move(failures)) { document.addListener(this); }
    ~DocumentRecoveryCapture() override { stop(); }
    Status latestFailure() const { return failure; }
    // May be called while consuming an event. Keep the capture object alive
    // until the callback returns; ListenerList supports removing its listener.
    void stop()
    {
        if (attached) { document.removeListener(this); attached = false; }
    }

private:
    void fail(Status status, uint64_t sequence)
    {
        failure = std::move(status);
        // Reporting must not alter the observer's original stopped-on-gap rule.
        try { if (reportFailure) reportFailure(failure, sequence); } catch (...) {}
    }
    void documentIdentityChanged(const media::IdentityEvent& event) override
    {
        if (!attached || !failure) return; // A missed event needs a fresh verified checkpoint.
        if (lastSequence != 0 && event.sequence != lastSequence + 1)
        { fail({ Error::staleHead, "identity_event_gap" }, event.sequence); return; }
        lastSequence = event.sequence;
        try
        {
            Capture capture { event, std::nullopt };
            if (event.kind == media::IdentityEventKind::generationChanged
                || event.kind == media::IdentityEventKind::saveCheckpoint
                || event.kind == media::IdentityEventKind::sourceRebound)
            {
                capture.document = document.captureCommittedState(event.after.document);
                if (!capture.document.has_value()
                    || !capture.document->identity.source.sameBinding(event.after.source))
                { fail({ Error::staleHead, "identity_capture_changed" }, event.sequence); return; }
            }
            if (consume) consume(std::move(capture));
        }
        catch (...) { fail({ Error::resourceOrEnvironmentFault, "capture_consumer_exception" }, event.sequence); }
    }
    SingleTrackDocument& document;
    Sink consume;
    FailureSink reportFailure;
    uint64_t lastSequence = 0;
    Status failure;
    bool attached = true;
};
} // namespace singleeditor::recovery
