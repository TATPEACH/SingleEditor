#include "Document/DocumentSaveCoordinator.h"

namespace singleeditor
{
class DocumentSaveCoordinator::SaveJob final : public juce::ThreadPoolJob
{
public:
    SaveJob(DocumentSaveCoordinator& ownerToUse, Request requestToUse)
        : juce::ThreadPoolJob("Single Editor audio save"),
          owner(ownerToUse),
          request(std::move(requestToUse))
    {
    }

    JobStatus runJob() override
    {
        auto result = owner.snapshotWriter(request.snapshot);
        owner.finishFromWorker(std::move(request), std::move(result));
        return jobHasFinished;
    }

private:
    DocumentSaveCoordinator& owner;
    Request request;
};

DocumentSaveCoordinator::DocumentSaveCoordinator(SingleTrackDocument& documentToUse,
                                                 SnapshotWriter writerToUse)
    : document(documentToUse),
      snapshotWriter(std::move(writerToUse))
{
    if (!snapshotWriter)
    {
        snapshotWriter = [](const SingleTrackDocument::SaveSnapshot& snapshot)
        {
            return SingleTrackDocument::writeSaveSnapshot(snapshot);
        };
    }
}

DocumentSaveCoordinator::~DocumentSaveCoordinator()
{
    if (workerPool != nullptr)
        workerPool->removeAllJobs(false, -1);
    cancelPendingUpdate();
}

bool DocumentSaveCoordinator::requestSave(SingleTrackDocument::SaveSnapshot snapshot,
                                          Completion completion)
{
    if (!snapshot.isValid())
        return false;

    Request request;
    request.snapshot = std::move(snapshot);
    if (completion)
        request.completions.push_back(std::move(completion));

    auto shouldStart = false;
    {
        const juce::ScopedLock sl(stateLock);
        if (active)
        {
            if (pendingRequest.has_value())
            {
                auto& callbacks = pendingRequest->completions;
                request.completions.insert(request.completions.begin(),
                                           std::make_move_iterator(callbacks.begin()),
                                           std::make_move_iterator(callbacks.end()));
            }

            pendingRequest = std::move(request);
            return true;
        }

        active = true;
        shouldStart = true;
    }

    if (shouldStart)
        startRequest(std::move(request));

    return true;
}

bool DocumentSaveCoordinator::isSaveInProgress() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return active || pendingRequest.has_value();
}

bool DocumentSaveCoordinator::hasPendingSave() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return pendingRequest.has_value();
}

void DocumentSaveCoordinator::whenIdle(std::function<void(bool)> completion)
{
    if (!completion)
        return;

    auto callImmediately = false;
    auto succeeded = true;
    {
        const juce::ScopedLock sl(stateLock);
        if (active || pendingRequest.has_value())
        {
            idleCallbacks.push_back(std::move(completion));
            return;
        }

        callImmediately = true;
        succeeded = latestSaveSucceeded;
    }

    if (callImmediately)
        completion(succeeded);
}

void DocumentSaveCoordinator::startRequest(Request request)
{
    if (workerPool == nullptr)
    {
        workerPool = std::make_unique<juce::ThreadPool>(1,
                                                        juce::Thread::osDefaultStackSize,
                                                        juce::Thread::Priority::background);
    }

    workerPool->addJob(new SaveJob(*this, std::move(request)), true);
}

void DocumentSaveCoordinator::finishFromWorker(Request request, Result result)
{
    {
        const juce::ScopedLock sl(stateLock);
        completedRequest = CompletedRequest { std::move(request), std::move(result) };
    }

    triggerAsyncUpdate();
}

void DocumentSaveCoordinator::handleAsyncUpdate()
{
    std::optional<CompletedRequest> finished;
    {
        const juce::ScopedLock sl(stateLock);
        finished = std::move(completedRequest);
        completedRequest.reset();
    }

    if (!finished.has_value())
        return;

    bool adopted = false;
    if (finished->result.succeeded)
    {
        const auto& snapshot = finished->request.snapshot;
        const bool targetsCurrent = document.getMediaIdentity().document.sameDocument(snapshot.documentIdentity)
            && snapshot.intent != SingleTrackDocument::SaveIntent::exportCopy;
        adopted = document.markSaveSnapshotCommitted(snapshot, finished->result);
        if (!adopted && targetsCurrent)
        {
            finished->result.succeeded = false;
            finished->result.failureReason = SingleTrackDocument::SaveFailureReason::validationFailed;
            finished->result.errorMessage = "The saved output could not be verified for the current document. Unsaved edits remain.";
        }
    }

    listeners.call([&] (Listener& listener)
    {
        listener.saveCompleted(finished->request.snapshot, finished->result, adopted);
    });

    for (auto& completion : finished->request.completions)
        if (completion)
            completion(finished->result);

    std::optional<Request> nextRequest;
    std::vector<std::function<void(bool)>> callbacksToNotify;
    auto idleSucceeded = finished->result.succeeded;
    {
        const juce::ScopedLock sl(stateLock);
        latestSaveSucceeded = finished->result.succeeded;
        if (pendingRequest.has_value())
        {
            nextRequest = std::move(pendingRequest);
            pendingRequest.reset();
        }
        else
        {
            active = false;
            callbacksToNotify = std::move(idleCallbacks);
            idleCallbacks.clear();
        }
    }

    if (nextRequest.has_value())
    {
        startRequest(std::move(*nextRequest));
        return;
    }

    for (auto& callback : callbacksToNotify)
        if (callback)
            callback(idleSucceeded);
}
} // namespace singleeditor
