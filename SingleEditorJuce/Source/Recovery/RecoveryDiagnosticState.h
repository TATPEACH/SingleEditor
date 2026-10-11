#pragma once
#include "RecoveryStore.h"
#include <array>
#include <mutex>
#include <optional>

namespace singleeditor::recovery
{
// One result slot shared by this loaded service's actual workers. Publication
// order under this mutex defines latest; Document versions never order events.
// No root, ticket, reader, capture, callback or host is retained here.
class RecoveryDiagnosticState final
{
public:
    enum class Stage { admission, launch, inspect, collectOrphans, prepare, append, publication, savedCancellation,
                       maintenanceLaunch, retireSaved, discard, collectTerminal,
                       centerInspect, centerAttempts, centerSource, centerAudio,
                       restoreLaunch, restoreStorage, restoreQueued, restoreValidation, restoreSource, restoreReplay,
                       restoreCore, restoreCorePublication, restoreFull, restoreFinalize, restoreAdmission, restoreAttemptClose,
                       sessionAdmission, sessionCapture, sessionTransition, sessionCheckpoint,
                       hostConfiguration, hostAdmission, hostMetadata, hostPoll, hostSavedDisposition, hostRetry, hostCheckpoint,
                       maintenanceConfiguration, serviceConfiguration, servicePoll, serviceTransfer,
                       serviceDiscard, serviceRetry, serviceCancel, maintenanceDiscovery, centerStorage };
    struct Failure final
    {
        juce::Uuid operationId { nullptr }, owner { nullptr };
        Stage stage = Stage::admission;
        Error error = Error::none;
        // Writer: pending packets, retained bytes, published sequence, cancelled
        // packets, 0, 0. Maintenance: batches, removed files, removed bytes,
        // frozen sequence, 0, 0. Inspection: metadata bytes, source window bytes,
        // source full bytes, audio bytes, verified assets, selected sequence.
        // Source byte counters are -1 when a failed resolver did not report them.
        // Restore: job count, resident bytes (-1 if busy), selected sequence,
        // core admitted, waiting for admission, attempt-close Error value.
        // Queue launch has no selected owner/sequence or admission flags.
        // Session: identity event sequence, retired epochs, current actor present,
        // checkpoint required, closed, 0. Static admission has no session counts.
        // Host: departed sessions, released metadata, saved tickets, active actor
        // present, closing, actual Save request serial (0 outside Save).
        // Service: waiting tickets, released epochs, completed owners,
        // cancelled packets, removed files, removed bytes.
        // Maintenance discovery: pages, entries, terminal owners admitted,
        // preserved owners, 0, 0. No source path or audio bytes are retained.
        // Storage inspection: observed bytes, entries, files, root present,
        // complete enumeration, available disk bytes (-1 if unavailable).
        // These are evidence, never lifecycle authority.
        std::array<int64_t, 6> counters {};
    };
    void record(Failure failure) noexcept
    {
        if (failure.operationId.isNull() || failure.error == Error::none || failure.error == Error::cancelled) return;
        try { const std::lock_guard lock(mutex); latest = failure; } catch (...) {}
    }
    bool trySnapshot(std::optional<Failure>& result) const
    {
        result.reset();
        const std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        result = latest;
        return true;
    }
private:
    mutable std::mutex mutex;
    std::optional<Failure> latest;
};
}
