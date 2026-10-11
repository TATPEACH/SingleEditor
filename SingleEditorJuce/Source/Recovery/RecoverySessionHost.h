#pragma once

#include "RecoveryProtectionSession.h"
#include "RecoveryOwnerLifecycle.h"
#include "Document/DocumentSaveCoordinator.h"

namespace singleeditor::recovery
{
// Message-thread lifetime owner. No scan, worker, timer or filesystem access in
// construction/PostCore clean admission. A departing document's accepted work
// stays owned until it actually settles; selecting another row is not a drain.
// The native service owns scheduling returned retirement tickets, quota policy,
// health presentation and the user's bounded-close choices.
class RecoverySessionHost final : private DocumentSaveCoordinator::Listener
{
public:
    struct Config final
    {
        RecoveryProtectionSession::Config session;
        size_t residentSessions = 8, metadataEvents = 128, savedTickets = 32;
        size_t retainedBytes = 256 * 1024 * 1024;
    };
    struct SessionHealth final
    {
        juce::Uuid entry { nullptr };
        bool active = false;
        RecoveryProtectionSession::Health protection;
    };
    struct ReleasedEpoch final
    {
        juce::Uuid entry { nullptr };
        RecoveryMutationProducer::Health protection;
    };
    struct Health final
    {
        std::vector<SessionHealth> sessions;
        Status failure;
        size_t pendingPackets = 0, retainedBytes = 0, pendingRetirements = 0, pendingMetadata = 0;
        bool closing = false, settled = false;
    };
    RecoverySessionHost(SingleTrackDocument&, DocumentSaveCoordinator&, Config);
    ~RecoverySessionHost() override;
    Status attachPostCore(const juce::Uuid& entry);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
    {
        // Only a later admission copies this context. Existing sessions and
        // retired epochs keep the generation in which their work was admitted.
        config.session.producer.queue.optionalLifecycleContext = context;
    }
#endif
    void leaveDocument();
    void poll(); // Zero-time drain checks; does not execute storage retirement.
    Health health() const;
    Status retry(const juce::Uuid& owner, bool collectOrphans = false);
    Status checkpointCurrent();
    void beginClose();
    Status cancelClose(const juce::Uuid& currentEntry);
    std::vector<std::unique_ptr<SavedOwnerRetirement>> takeSavedRetirements();
    std::vector<ReleasedEpoch> takeReleasedEpochs();

private:
    struct OwnedSession final
    {
        juce::Uuid entry { nullptr };
        std::unique_ptr<RecoveryProtectionSession> session;
    };
    void saveCompleted(const SingleTrackDocument::SaveSnapshot&, const DocumentSaveCoordinator::Result&, bool adopted) override;
    void fail(Status, RecoveryDiagnosticState::Stage, const juce::Uuid& owner = juce::Uuid(nullptr), uint64_t saveSerial = 0);
    void note(RecoveryDiagnosticState::Stage, const Status&, const juce::Uuid& owner = juce::Uuid(nullptr), uint64_t saveSerial = 0) const noexcept;
    bool releaseEpochs(OwnedSession&);
    SingleTrackDocument& document;
    DocumentSaveCoordinator& saves;
    Config config;
    std::optional<OwnedSession> active;
    std::vector<OwnedSession> departed;
    std::vector<std::unique_ptr<SavedOwnerRetirement>> savedRetirements;
    std::vector<ReleasedEpoch> releasedEpochs;
    Status failure;
    Status admissionFailure;
    bool metadataBacklog = false;
    bool metadataFailureReported = false;
    bool closing = false;
};
}
