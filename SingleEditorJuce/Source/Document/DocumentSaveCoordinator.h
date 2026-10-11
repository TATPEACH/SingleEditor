#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
class DocumentSaveCoordinator final : private juce::AsyncUpdater
{
public:
    using Result = SingleTrackDocument::SaveWriteResult;
    using Completion = std::function<void(const Result&)>;
    using SnapshotWriter = std::function<Result(const SingleTrackDocument::SaveSnapshot&)>;
    class Listener
    {
    public:
        virtual ~Listener() = default;
        // Owner thread, after the real publication has been offered to the
        // Document and before user completions can switch/clear it. A successful
        // export or a saved obsolete document is not an adopted checkpoint.
        virtual void saveCompleted(const SingleTrackDocument::SaveSnapshot&, const Result&, bool adopted) = 0;
    };

    explicit DocumentSaveCoordinator(SingleTrackDocument& documentToUse,
                                     SnapshotWriter writerToUse = {});
    ~DocumentSaveCoordinator() override;

    bool requestSave(SingleTrackDocument::SaveSnapshot snapshot,
                     Completion completion = {});
    bool isSaveInProgress() const noexcept;
    bool hasPendingSave() const noexcept;
    void whenIdle(std::function<void(bool)> completion);
    void addListener(Listener* listener) { listeners.add(listener); }
    void removeListener(Listener* listener) { listeners.remove(listener); }

private:
    struct Request
    {
        SingleTrackDocument::SaveSnapshot snapshot;
        std::vector<Completion> completions;
    };

    struct CompletedRequest
    {
        Request request;
        Result result;
    };

    class SaveJob;

    void startRequest(Request request);
    void finishFromWorker(Request request, Result result);
    void handleAsyncUpdate() override;

    SingleTrackDocument& document;
    SnapshotWriter snapshotWriter;
    std::unique_ptr<juce::ThreadPool> workerPool;
    mutable juce::CriticalSection stateLock;
    bool active = false;
    bool latestSaveSucceeded = true;
    std::optional<Request> pendingRequest;
    std::optional<CompletedRequest> completedRequest;
    std::vector<std::function<void(bool)>> idleCallbacks;
    juce::ListenerList<Listener> listeners;
};
} // namespace singleeditor
