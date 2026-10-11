#include "Recovery/RecoveryService.h"
#include "Recovery/RecoveryDiscovery.h"
#include "Recovery/RecoveryRestoreCandidate.h"
#include "Recovery/RecoveryRestoreQueue.h"
#include "Document/RecoveredPlaylistDocument.h"
#include "Audio/AudioEngine.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "Core/NativeFilePath.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cstdlib>
#include <iostream>
#include <map>
#include <atomic>
#include <stdexcept>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using namespace std::chrono_literals;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Recovery service: " << status.code << '\n'; require(static_cast<bool>(status), message); }
template <typename Predicate> void await(RecoveryService& service, Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        // Exercise the actual native scheduling contract: no unconditional
        // safety-net polling after the service says there is no work.
        if (service.needsPoll()) service.poll();
        if (predicate()) return;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    const auto health = service.health();
    std::cerr << "Service wait expired: needsPoll=" << service.needsPoll()
        << " worker=" << health.maintenance.workerRunning << " failure=" << health.maintenance.failure.code
        << " sweepActive=" << health.maintenance.sweep.active << " sweepComplete=" << health.maintenance.sweep.completed
        << " sweepFailure=" << health.maintenance.sweep.failure.code << " pages=" << health.maintenance.sweep.pages
        << " completedOwners=" << health.completedOwners << '\n';
    for (const auto& job : health.maintenance.jobs)
        std::cerr << "Maintenance wait job: root=" << job.root.getFullPathName() << " owner=" << job.owner.toString()
            << " phase=" << static_cast<int>(job.phase) << " failure=" << job.failure.code << '\n';
    require(false, message);
}
void makeSource(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output(files::nativeFile(file).createOutputStream());
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create service audio fixture");
    juce::AudioBuffer<float> pcm(2, 12000);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < pcm.getNumSamples(); ++i)
        pcm.setSample(c, i, ((i * 19 + c) % 127 - 63) / 140.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write real service audio");
}
RecoveryService::Config config(const juce::File& root)
{
    RecoveryService::Config settings; settings.root = root;
    settings.sessions.session.producer.queue.maximumAge = 30s; // Fault gates, not a latency measurement.
    settings.maintenance.batch.files = 1;
    return settings;
}
RecoveryMutationProducer::Health current(const RecoveryService& service)
{
    for (const auto& item : service.health().protection.sessions) if (item.active) return item.protection.current;
    require(false, "service has a current actual Document"); return {};
}
RecoveryDiagnosticState::Failure diagnosticFailure(const RecoveryService& service)
{
    std::optional<RecoveryDiagnosticState::Failure> result;
    require(service.tryDiagnosticFailure(result) && result.has_value(), "actual service failure has bounded diagnostics");
    return *result;
}
void serviceControlDiagnostics(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load real control-diagnostic source");
    DocumentSaveCoordinator saves(document);
    const auto image = document.captureWorkspaceSnapshot();
    {
        auto settings = config(juce::File());
        RecoveryService service(document, saves, settings);
        const auto failure = diagnosticFailure(service);
        require(failure.stage == RecoveryDiagnosticState::Stage::serviceConfiguration && failure.error == Error::invalidPacket
            && failure.owner.isNull() && !failure.operationId.isNull(), "missing root records actual setup failure without invented owner");
        require(service.attachPostCore(juce::Uuid()).code == "service_root", "missing root rejects actual PostCore admission");
        service.poll(); (void) service.health(); service.poll();
        require(diagnosticFailure(service).operationId == failure.operationId, "repeated root refusal and observation retain one setup failure");
    }
    {
        auto settings = config(root.getChildFile("invalid-maintenance-config")); settings.maintenance.jobs = 0;
        RecoveryService service(document, saves, settings);
        const auto failure = diagnosticFailure(service);
        require(failure.stage == RecoveryDiagnosticState::Stage::maintenanceConfiguration && failure.owner.isNull()
            && failure.error == Error::invalidPacket, "actual invalid maintenance capacity has its own setup failure");
        good(service.attachPostCore(juce::Uuid()), "valid clean observer can coexist with unavailable cleanup configuration");
        require(diagnosticFailure(service).operationId == failure.operationId && !settings.root.exists(),
            "successful clean admission preserves actual maintenance failure and stays storage-inert");
    }
    {
        const auto settings = config(root.getChildFile("explicit-control-diagnostics"));
        RecoveryService service(document, saves, settings); const juce::Uuid owner;
        require(!service.retryMaintenance(owner), "real explicit Retry rejects unknown maintenance owner");
        const auto retry = diagnosticFailure(service);
        require(retry.stage == RecoveryDiagnosticState::Stage::serviceRetry && retry.owner == owner,
            "Retry refusal records the actual requested owner without making a job");
        std::unique_ptr<OwnerDiscardReview> absent;
        require(!service.submitConfirmedDiscard(absent) && !absent, "missing confirmed review is not accepted");
        const auto discard = diagnosticFailure(service);
        require(discard.stage == RecoveryDiagnosticState::Stage::serviceDiscard && discard.owner.isNull()
            && discard.operationId != retry.operationId, "actual distinct Discard refusal has no fabricated owner");
        require(!service.cancelDiscard(owner), "Cancel rejects unknown Discard owner");
        const auto cancelled = diagnosticFailure(service);
        require(cancelled.stage == RecoveryDiagnosticState::Stage::serviceCancel && cancelled.owner == owner
            && cancelled.operationId != discard.operationId, "real refused Cancel is distinct from successful user cancellation");
        good(service.attachPostCore(juce::Uuid()), "normal clean admission still succeeds after explicit control refusals");
        require(diagnosticFailure(service).operationId == cancelled.operationId && !settings.root.exists()
            && service.health().maintenance.jobs.empty(), "success preserves last actual refusal without creating work or storage");
    }
    require(document.captureWorkspaceSnapshot()->sameContents(*image), "all setup/control diagnostic observations preserve actual Document history");
}
void protect(RecoveryService& service)
{
    await(service, [&]
    {
        const auto value = current(service);
        if (!value.failure && !value.writer.workerRunning)
        { std::cerr << "Actual service protection failed: " << value.failure.code << '\n'; require(false, "real protection succeeded"); }
        return value.phase == RecoveryMutationProducer::Phase::protectedState && !service.needsPoll();
    }, "native scheduling reaches durable protection and goes idle");
}
void save(DocumentSaveCoordinator& coordinator, SingleTrackDocument& document, const juce::File& file,
          std::function<void()> completed = {})
{
    bool done = false;
    const auto snapshot = document.createSaveSnapshot(file, SingleTrackDocument::SaveIntent::saveAs);
    require(coordinator.requestSave(snapshot, [&](const auto& result)
    {
        if (!result.succeeded) std::cerr << result.errorMessage << '\n';
        require(result.succeeded && result.publication && !document.hasUnsavedChanges(), "actual native Save publishes and adopts before completion");
        done = true; if (completed) completed();
    }), "service Save request accepted");
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while ((!done || coordinator.isSaveInProgress()) && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    require(done && !coordinator.isSaveInProgress(), "real Save callback and worker finish");
}

void lightweightDiscovery(const juce::File& root, const juce::File& source)
{
    const auto absent = root.getChildFile("discovery-absent");
    RecoveryDiscovery empty(absent);
    require(!absent.exists(), "discovery construction is inert");
    const auto nothing = empty.nextPage({});
    good(nothing.status, "absent presence check succeeds");
    require(!nothing.presence && !nothing.more && nothing.rows.empty() && nothing.entriesRead == 0 && !absent.exists(),
        "absent root does not create, enumerate, or invent a recovery owner");
    SingleTrackDocument document; require(document.loadFile(source), "load discovery source");
    DocumentSaveCoordinator saves(document); const auto settings = config(root.getChildFile("discovery"));
    RecoveryService service(document, saves, settings); good(service.attachPostCore(juce::Uuid()), "admit discovery source");
    require(document.moveEnvelopePoint(0, 0, 0.41f), "first discovery mutation"); protect(service);
    const auto first = current(service);
    require(document.moveEnvelopePoint(0, 0, 0.63f), "second discovery mutation"); protect(service);
    const auto latest = current(service);
    const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    RecoveryStore store(journal, latest.owner); const auto manifest = store.inspectManifest();
    good(manifest.status, "prepare real discovery chain");
    require(manifest.commits.size() == 2 && latest.owner == first.owner, "real two-record chain");
    const auto image = document.captureWorkspaceSnapshot();
    service.beginClose(); await(service, [&] { return service.health().settled; }, "release discovery producer before payload-denial probes");
#if JUCE_WINDOWS
    struct Block final
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        explicit Block(const juce::File& file)
        {
            handle = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) std::cerr << "Read-denial fixture: " << file.getFullPathName() << " Win32=" << GetLastError() << '\n';
            require(handle != INVALID_HANDLE_VALUE, "hold actual payload read-denial fixture");
        }
        ~Block() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    };
    const auto directory = journal.getChildFile(latest.owner.toString());
    Block oldRecord(directory.getChildFile("commits").getChildFile(first.writer.safeHead.hash));
    const auto asset = manifest.commits.back().record["assets"][0].toString();
    require(asset.isNotEmpty(), "real checkpoint has actual CAS audio");
    Block audio(directory.getChildFile("assets").getChildFile(asset));
    require(!store.inspectManifest().status, "full history inspection actually encounters the denied ancestor");
#endif
    const auto summary = store.inspectSummary(); good(summary.status, "head summary ignores locked ancestors and CAS");
    require(summary.head == latest.writer.safeHead && summary.sourcePath == source.getFullPathName()
        && summary.lastSafeMilliseconds > 0 && summary.recordBytesRead > 0 && !summary.retired,
        "summary reports only actual latest metadata");
    require(store.inspectSummary([] { return true; }).status.error == Error::cancelled, "summary cancellation is explicit");
    std::set<juce::String> expected { latest.owner.toString() }, seen;
    for (int i = 0; i < 7; ++i)
    {
        const juce::Uuid owner; expected.insert(owner.toString());
        require(journal.getChildFile(owner.toString()).createDirectory().wasOk(), "owned empty discovery row");
    }
    const juce::Uuid corrupt; expected.insert(corrupt.toString());
    require(journal.getChildFile(corrupt.toString()).createDirectory().wasOk()
        && journal.getChildFile(corrupt.toString()).getChildFile("head").replaceWithText("invalid"), "owned corrupt head row");
    RecoveryDiscovery discovery(journal);
    bool more = true; size_t pages = 0; bool observedCorrupt = false;
    while (more && pages++ < 32)
    {
        const auto page = discovery.nextPage({ 1, Limits{}.recordBytes });
        good(page.status, "bounded discovery page survives a corrupt sibling");
        require(page.presence && page.entriesRead <= 1 && page.rows.size() <= 1
            && page.recordBytesRead <= Limits{}.recordBytes, "real cursor obeys per-page entry and payload budgets");
        for (const auto& row : page.rows)
        {
            seen.insert(row.owner.toString());
            if (row.owner == latest.owner) require(row.summary.status && row.summary.head == latest.writer.safeHead,
                "discovery page uses the lightweight head reader");
            if (row.owner == corrupt) observedCorrupt = !row.summary.status;
        }
        more = page.more;
    }
    require(!more && seen == expected && observedCorrupt, "all pages preserve empty, valid and corrupt owner distinctions");
    require(document.captureWorkspaceSnapshot()->sameContents(*image), "discovery leaves real Document and Undo/Redo untouched");
    service.beginClose(); await(service, [&] { return service.health().settled; }, "discovery owner drains normally");
}

void automaticHandoff(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load service clean input");
    DocumentSaveCoordinator saves(document); const auto settings = config(root.getChildFile("automatic"));
    RecoveryService service(document, saves, settings); const juce::Uuid entry, nextEntry;
    require(!service.needsPoll() && !settings.root.exists(), "service construction starts no scheduler, writer or directory");
    good(service.attachPostCore(entry), "admit actual clean post-Core Document");
    std::optional<RecoveryDiagnosticState::Failure> diagnostic;
    require(service.tryDiagnosticFailure(diagnostic) && !diagnostic, "clean service has no fabricated failure or request");
    require(!service.needsPoll() && current(service).owner.isNull() && !settings.root.exists(), "clean PostCore finishes inert");
    require(!document.moveEnvelopePoint(0, 0, 1.0f) && !service.needsPoll(), "no-op keeps native service asleep");
    require(document.moveEnvelopePoint(0, 0, 0.45f), "real edit wakes automatic protection");
    protect(service);
    const auto original = current(service); const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    require(original.writer.safeHead.sequence == 1 && document.hasUnsavedChanges() && document.canUndo(),
        "one actual mutation creates one protected checkpoint without changing editing history");
    save(saves, document, root.getChildFile("automatic-saved.wav"), [&]
    {
        require(service.health().protection.pendingRetirements == 1, "Host signs exact ticket before the native user completion switches rows");
        document.clear(); require(document.loadFile(source), "native completion opens an independent next Document");
        good(service.attachPostCore(nextEntry), "admit next Document without losing old real ticket");
    });
    await(service, [&] { return !service.needsPoll() && service.health().completedOwners == 1; }, "service transfers and collects the exact saved owner");
    require(!files::nativeFile(journal.getChildFile(original.owner.toString())).exists()
        && root.getChildFile("automatic-saved.wav").existsAsFile() && current(service).owner.isNull(),
        "metadata service collects only the saved owner and leaves next clean Document inert");
    service.beginClose(); require(service.health().settled, "clean service closes with no remaining work");
    good(service.cancelClose(nextEntry), "Cancel reopens clean observation without sealing maintenance admission");
    require(!service.health().protection.closing && !service.health().settled && !service.needsPoll(), "cancelled close is a live idle service");
    require(document.moveEnvelopePoint(0, 0, 0.63f), "editing after cancelled close uses the resumed observer");
    protect(service); require(current(service).writer.safeHead.sequence == 1, "resumption installs one observer and one initial checkpoint");
    save(saves, document, root.getChildFile("automatic-resumed.wav"));
    await(service, [&] { return !service.needsPoll() && service.health().completedOwners == 2; }, "maintenance still admits a later genuine Save after close cancellation");
    service.beginClose(); require(service.health().settled && document.canUndo(), "service close preserves the saved Document's full history");
    document.clear();
    good(service.cancelClose(juce::Uuid(nullptr)), "Cancel on an empty workspace resumes admission without inventing a Document");
    require(service.health().protection.sessions.empty() && !service.health().protection.closing,
        "empty workspace has no active recovery observer or owner");
}

void backpressure(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load pressure source");
    DocumentSaveCoordinator saves(document); auto settings = config(root.getChildFile("backpressure"));
    settings.maintenance.jobs = 1; settings.sessions.savedTickets = 1;
    RecoveryService service(document, saves, settings); const juce::Uuid entry;
    good(service.attachPostCore(entry), "admit pressure Document");
    const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    std::vector<RecoveryMutationProducer::Health> owners;
    std::shared_ptr<RecoveryOwnerLease> live;
    for (int i = 0; i < 3; ++i)
    {
        require(document.moveEnvelopePoint(0, 0, 0.7f - i * 0.15f), "create next genuine source epoch");
        protect(service); owners.push_back(current(service));
        if (i == 0) good(RecoveryStore(journal, owners.back().owner).acquireOwnerLease(live), "actual reader lease blocks first cleanup");
        save(saves, document, root.getChildFile("pressure-save-" + juce::String(i) + ".wav"));
        await(service, [&] { return !service.needsPoll(); }, "failed cleanup/backpressure becomes idle and waits for an explicit action");
        if (i == 0)
        {
            const auto failure = diagnosticFailure(service);
            require(failure.stage == RecoveryDiagnosticState::Stage::retireSaved && failure.owner == owners.front().owner
                && failure.operationId == service.health().maintenance.jobs.front().operationId,
                "first actual saved retirement failure retains its real worker operation ID");
        }
    }
    const auto blocked = service.health();
    require(blocked.maintenance.jobs.size() == 1 && !blocked.maintenance.failure
        && blocked.waitingRetirements == 1 && blocked.protection.pendingRetirements == 1,
        "one-slot queue, service and Host each retain their bounded original private ticket");
    require(blocked.dispositionFailure.code == "maintenance_job_budget", "service exposes admission backpressure without pretending cleanup completed");
    std::optional<RecoveryDiagnosticState::Failure> diagnostic;
    require(service.tryDiagnosticFailure(diagnostic) && diagnostic && diagnostic->owner == owners[1].owner
        && diagnostic->operationId != blocked.maintenance.jobs.front().operationId
        && diagnostic->stage == RecoveryDiagnosticState::Stage::serviceTransfer && diagnostic->error == Error::budgetExceeded
        && diagnostic->counters[0] == 1,
        "later actual ticket transfer refusal has a distinct operation and exact waiting owner without inventing a maintenance job");
    const auto failureId = diagnostic->operationId;
    service.poll(); service.poll(); (void) service.health();
    require(diagnosticFailure(service).operationId == failureId, "same held ticket and refusal do not mint repeated failure IDs on poll");
    for (const auto& owner : owners) require(RecoveryStore(journal, owner.owner).inspect().head == owner.writer.safeHead,
        "every backpressured original durable head remains intact");
    service.beginClose(); require(!service.health().settled, "failed cleanup and waiting authority prevent a false settled close");
    good(service.cancelClose(entry), "cancel close with a full queue preserves maintenance jobs and observations");
    require(diagnosticFailure(service).operationId == failureId, "close and cancel against the same held ticket retain one refusal");
    auto lastTransfer = *diagnostic;
    live.reset(); good(service.retryMaintenance(owners.front().owner), "explicitly retry actual blocked saved-owner cleanup");
    await(service, [&]
    {
        const auto latest = diagnosticFailure(service);
        if (latest.operationId != lastTransfer.operationId)
            require(latest.stage == RecoveryDiagnosticState::Stage::serviceTransfer && latest.owner == owners[2].owner
                && latest.error == Error::budgetExceeded && latest.counters[0] == 1,
                "only the next actual held ticket's capacity refusal may replace the prior transfer failure");
        lastTransfer = latest;
        return !service.needsPoll() && service.health().completedOwners == 3;
    },
        "native polling transfers every retained ticket as capacity becomes available");
    for (const auto& owner : owners) require(!files::nativeFile(journal.getChildFile(owner.owner.toString())).exists(),
        "each exact saved owner was actually collected");
    const auto done = service.health();
    service.poll();
    require(service.tryDiagnosticFailure(diagnostic) && diagnostic && diagnostic->operationId == lastTransfer.operationId,
        "successful maintenance retry and completed job release preserve the latest actual failure capsule");
    require(done.waitingRetirements == 0 && done.protection.pendingRetirements == 0 && done.maintenance.jobs.empty()
        && done.removedFiles > 0 && done.removedBytes > 0 && done.dispositionFailure,
        "completion clears transient backpressure and retains actual collection evidence");
    service.beginClose(); require(service.health().settled, "all accepted service work settles after retry");
}

void degradedClose(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load degraded source");
    DocumentSaveCoordinator saves(document); auto settings = config(root.getChildFile("blocked-layout"));
    require(settings.root.replaceWithText("private test obstruction"), "place actual storage obstruction");
    RecoveryService service(document, saves, settings); const juce::Uuid entry;
    good(service.attachPostCore(entry), "clean PostCore never probes obstructed recovery directory");
    require(document.moveEnvelopePoint(0, 0, 0.57f), "degraded recovery does not block the real edit");
    await(service, [&] { return !service.needsPoll() && !current(service).owner.isNull(); }, "real preparation failure leaves the service idle");
    const auto failed = current(service);
    std::optional<RecoveryDiagnosticState::Failure> diagnostic;
    require(service.tryDiagnosticFailure(diagnostic) && diagnostic && diagnostic->owner == failed.owner
        && diagnostic->operationId == failed.writer.operationId && !diagnostic->operationId.isNull()
        && diagnostic->stage == RecoveryDiagnosticState::Stage::prepare && diagnostic->error == failed.failure.error
        && diagnostic->counters[0] == 1 && diagnostic->counters[1] > 0 && diagnostic->counters[2] == 0,
        "real storage preparation failure freezes the actual owner, attempt, stage and pending facts");
    const auto firstFailure = *diagnostic;
    service.health();
    require(service.tryDiagnosticFailure(diagnostic) && diagnostic->operationId == firstFailure.operationId,
        "reading aggregate Health does not manufacture or replace a failure event");
    require(!failed.failure && failed.writer.pendingPackets == 1 && failed.writer.safeHead.sequence == 0
        && document.canUndo() && document.hasUnsavedChanges(), "failed preparation preserves accepted owner, latest edit and complete Undo");
    service.beginClose(); require(!service.health().settled && service.health().protection.pendingPackets == 1,
        "degraded close reports outstanding accepted work instead of dropping the queue");
    good(service.cancelClose(entry), "Cancel can re-admit current dirty Document while retaining its older failed queue");
    await(service, [&] { return !service.needsPoll(); }, "new dirty checkpoint also honestly reports the real obstruction");
    const auto resumed = current(service); require(resumed.owner != failed.owner, "resumption owns a distinct checkpoint without impersonating the failed actor");
    require(service.tryDiagnosticFailure(diagnostic) && diagnostic && diagnostic->owner == resumed.owner
        && diagnostic->operationId == resumed.writer.operationId && diagnostic->operationId != firstFailure.operationId,
        "a second owner's actual failed attempt replaces the one service result slot");
    const auto lastFailure = *diagnostic;
    require(settings.root.getParentDirectory() == root && settings.root.deleteFile(), "remove only the owned obstruction");
    good(service.retryProtection(failed.owner), "retry exact departed owner after fixing storage");
    good(service.retryProtection(resumed.owner), "retry exact current owner through the same prepared service root");
    protect(service);
    require(current(service).writer.operationId != lastFailure.operationId && service.tryDiagnosticFailure(diagnostic)
        && diagnostic && diagnostic->operationId == lastFailure.operationId && diagnostic->counters == lastFailure.counters,
        "successful explicit retry owns a fresh attempt while retaining the immutable previous failure and counters");
    await(service, [&] { return !service.needsPoll() && service.health().protection.sessions.size() == 1; }, "closed prior queue drains and releases its runtime after real retry");
    const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    require(RecoveryStore(journal, failed.owner).inspect().head.sequence == 1, "runtime drain preserves the older durable recovery point without inventing Save authority");
    save(saves, document, root.getChildFile("degraded-resumed.wav"));
    await(service, [&] { return !service.needsPoll() && service.health().completedOwners == 1; }, "current real Save retires its exact resumed owner");
    require(RecoveryStore(journal, failed.owner).inspect().head.sequence == 1,
        "a released older prefix is preserved for separate disposition, never cleaned from returned Health");
    service.beginClose(); require(service.health().settled, "all accepted runtime work is settled without claiming older-prefix storage disposition");
    const auto savedImage = document.captureWorkspaceSnapshot();
    std::unique_ptr<OwnerDiscardReview> review;
    good(RecoveryStore(journal, failed.owner).reviewDiscard(review), "review the actually released older prefix without manufacturing Save permission");
    good(service.submitConfirmedDiscard(review), "explicit second-confirmed Discard enters the real service maintenance worker");
    await(service, [&] { return service.health().settled && service.health().completedOwners == 2; }, "separately confirmed old-prefix Discard completes");
    require(!files::nativeFile(journal.getChildFile(failed.owner.toString())).exists()
        && document.captureWorkspaceSnapshot()->sameContents(*savedImage), "old-prefix Discard preserves the live saved Document and complete history");
}

void reviewedDiscard(const juce::File& root, const juce::File& source)
{
    const auto sourceHash = juce::SHA256(source).toHexString();
    SingleTrackDocument document; require(document.loadFile(source), "load selected-owner review input");
    DocumentSaveCoordinator saves(document); auto settings = config(root.getChildFile("reviewed-discard"));
    settings.maintenance.jobs = 1;
    const auto failCollection = std::make_shared<std::atomic<bool>>(false);
    const auto failPublication = std::make_shared<std::atomic<bool>>(false);
    settings.maintenance.afterRetirementPublished = [failPublication](auto)
    { if (failPublication->exchange(false)) throw std::runtime_error("owned post-publication observation fault"); };
    settings.maintenance.onBoundary = [failCollection](auto phase, auto)
    {
        if (phase == RecoveryMaintenanceQueue::Phase::collecting && failCollection->exchange(false))
            throw std::runtime_error("owned terminal collection fault");
    };
    RecoveryService service(document, saves, settings); const juce::Uuid entry;
    good(service.attachPostCore(entry), "admit review fixture only after core");
    const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    std::unique_ptr<OwnerDiscardReview> absent;
    require(!RecoveryStore(journal, juce::Uuid()).reviewDiscard(absent) && !absent && !settings.root.exists(),
        "review of missing storage does not create it");
    require(document.moveEnvelopePoint(0, 0, 0.45f), "first actual reviewed mutation"); protect(service);
    const auto initial = current(service);
    RecoveryStore store(journal, initial.owner);
    std::unique_ptr<OwnerDiscardReview> review;
    good(store.reviewDiscard(review), "read bounded selected owner metadata");
    require(review->ownerId() == initial.owner && review->frozenHead() == initial.writer.safeHead
        && review->sourcePath() == source.getFullPathName() && review->lastSafeMilliseconds() > 0
        && review->observedBytes() > 0 && review->observedEntries() > 0,
        "review exposes selected owner, original source, observed size and actual last-safe timestamp");
    const auto firstVersion = document.getMediaIdentity().document.generation;
    require(document.moveEnvelopePoint(0, 0, 0.62f), "append after the user saw an earlier review"); protect(service);
    require(document.undo() && document.getMediaIdentity().document.generation == firstVersion, "Undo returns to the older numeric version"); protect(service);
    good(service.submitConfirmedDiscard(review), "worker accepts the second-confirmed frozen review");
    await(service, [&] { return !service.needsPoll() && !service.health().maintenance.failure; }, "stale reviewed head fails without repeated background work");
    require(service.health().maintenance.jobs.front().failure.code == "discard_review_head_changed",
        "matching old Document generation never substitutes for exact journal head");
    std::optional<OwnerRetirement> marker; good(store.inspectRetirement(marker), "read unchanged disposition");
    require(!marker, "stale confirmation published no terminal marker");
    std::unique_ptr<OwnerDiscardReview> fresh;
    good(store.reviewDiscard(fresh), "prepare a fresh review after stale failure");
    require(!service.submitConfirmedDiscard(fresh) && fresh, "full failed queue retains the caller's new review");
    const auto refused = diagnosticFailure(service);
    require(refused.stage == RecoveryDiagnosticState::Stage::serviceDiscard && refused.owner == initial.owner
        && refused.error == Error::budgetExceeded, "actual confirmed Discard capacity refusal keeps exact owner and caller review");
    good(service.cancelDiscard(initial.owner), "explicitly cancel only the failed unpublished Discard job");
    require(diagnosticFailure(service).operationId == refused.operationId, "successful Discard cancellation does not invent a new failure");
    require(service.health().maintenance.jobs.empty(), "cancelled stale review frees bounded capacity without data deletion");
    service.leaveDocument();
    await(service, [&] { return !service.needsPoll() && service.health().protection.sessions.empty(); }, "release actual closed producer before selected-owner collection");
    const auto image = document.captureWorkspaceSnapshot();
    const auto directory = journal.getChildFile(initial.owner.toString());
    const auto archived = journal.getChildFile("review-original-" + juce::Uuid().toString());
    require(directory.isAChildOf(settings.root) && archived.isAChildOf(settings.root), "directory replacement experiment remains in owned recovery fixture");
    require(files::nativeFile(directory).moveFileTo(files::nativeFile(archived))
        && files::nativeFile(archived).copyDirectoryTo(files::nativeFile(directory)), "replace only the owned directory with a byte-identical copy");
    OwnerRetirement rejected;
    const auto replaced = RecoveryOwnerLifecycle::discard(store, *fresh, rejected);
    require(!replaced && replaced.code == "retirement_review_directory_changed", "same head in a different physical owner is not the user's reviewed object");
    require(files::nativeFile(directory).deleteRecursively() && files::nativeFile(archived).moveFileTo(files::nativeFile(directory)),
        "restore the exact owned original directory after replacement check");
    std::unique_ptr<OwnerDiscardReview> cancelled;
    require(store.reviewDiscard(cancelled, [] { return true; }).error == Error::cancelled && !cancelled,
        "cancelled review produces no packet or mutation");
    auto limited = settings.sessions.session.producer.store; limited.globalEntries = 1;
    require(RecoveryStore(journal, initial.owner, limited).reviewDiscard(cancelled).error == Error::budgetExceeded && !cancelled,
        "review enumeration obeys a real metadata entry budget");
#if JUCE_WINDOWS
    const auto assets = files::nativeFile(directory.getChildFile("assets")).findChildFiles(juce::File::findFiles, false);
    require(!assets.isEmpty(), "real protection produced CAS assets");
    const auto blockRead = CreateFileW(files::nativePath(assets[0]).toWideCharPointer(), GENERIC_WRITE,
        FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(blockRead != INVALID_HANDLE_VALUE, "deny actual CAS payload reads for the metadata review");
    std::unique_ptr<OwnerDiscardReview> noAudio;
    const auto metadataOnly = store.reviewDiscard(noAudio);
    CloseHandle(blockRead);
    good(metadataOnly, "Discard review reads metadata without opening CAS payloads");
#endif
    RecoveryService other(document, saves, config(root.getChildFile("different-review-root")));
    require(!other.submitConfirmedDiscard(fresh) && fresh, "another service root cannot consume the reviewed owner");
    require(diagnosticFailure(other).stage == RecoveryDiagnosticState::Stage::serviceDiscard
        && diagnosticFailure(other).owner == initial.owner, "actual different-root refusal reports its reviewed owner without consuming authority");
    std::shared_ptr<RecoveryOwnerLease> live; good(store.acquireOwnerLease(live), "hold actual restored-reader owner lease");
    good(service.submitConfirmedDiscard(fresh), "confirmed current review enters bounded worker");
    await(service, [&] { return !service.needsPoll() && !service.health().maintenance.failure; }, "live restored dependency blocks Discard retirement");
    good(store.inspectRetirement(marker), "inspect while live data is pinned"); require(!marker, "active dependency prevents even the terminal marker");
    live.reset(); failPublication->store(true);
    good(service.retryMaintenance(initial.owner), "explicit Retry after restored reader releases");
    await(service, [&] { return !service.needsPoll() && !service.health().maintenance.failure; }, "failed publication observation is reported after actual marker write");
    good(store.inspectRetirement(marker), "inspect marker after uncertain success observation");
    require(marker && !service.cancelDiscard(initial.owner), "uncertain publication cannot be cancelled from an unset in-memory success flag");
    failCollection->store(true);
    good(service.retryMaintenance(initial.owner), "revalidate published marker before resuming physical cleanup");
    await(service, [&] { return !service.needsPoll() && !service.health().maintenance.failure; }, "fault after terminal publication keeps the cleanup task");
    std::optional<OwnerRetirement> duringCollection;
    const auto collecting = store.inspectRetirement(duringCollection);
    require(!collecting && collecting.code == "owner_collecting" && !duringCollection
        && marker && marker->evidence["reason"].toString() == "discarded"
        && !service.cancelDiscard(initial.owner), "published terminal intent cannot be falsely cancelled");
    good(service.retryMaintenance(initial.owner), "resume exact terminal collection without another confirmation");
    await(service, [&] { return !service.needsPoll() && service.health().completedOwners == 1; }, "confirmed Discard completes bounded resumable collection");
    require(!files::nativeFile(directory).exists() && source.existsAsFile() && juce::SHA256(source).toHexString() == sourceHash
        && document.captureWorkspaceSnapshot()->sameContents(*image), "selected-owner Discard leaves source and current complete history unchanged");
    service.beginClose(); require(service.health().settled, "all Discard metadata and worker ownership retires");
}

void storageUsage(const juce::File& root, const juce::File& source)
{
    const auto directory = root.getChildFile("storage-usage");
    RecoveryStorageLayout layout(directory);
    const auto absent = layout.inspectUsage({});
    require(absent.status && absent.complete && !absent.present && absent.bytes == 0 && !absent.policy
        && absent.freeBytes >= 0 && !directory.exists(), "absent usage inspection reports zero without creating storage");
    require(!RecoveryStorageLayout(juce::File()).inspectUsage({}).status, "usage requires an explicit absolute service root");
    ServiceStorageBudget configured;
    configured.bytes = configured.journalBytes = 64 * 1024 * 1024;
    configured.attemptBytes = 1024 * 1024;
    good(layout.prepare(configured), "initialize actual non-default stored limits");
    SingleTrackDocument document; require(document.loadFile(source), "load usage source");
    require(document.moveEnvelopePoint(0, 0, 0.43f) && document.moveEnvelopePoint(0, 0, 0.71f) && document.undo(),
        "usage fixture contains complete current Undo and Redo");
    const auto workspace = document.captureWorkspaceSnapshot();
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encode(*document.captureCommittedState(document.getMediaIdentity().document), {}, packet),
        "encode actual audio and history for measured usage");
    RecoveryStore store(layout.journalDirectory(), juce::Uuid()); SafeHead head;
    good(store.append({}, packet, head), "publish actual counted recovery files");
    const juce::MemoryBlock extra(8192, true);
    require(layout.attemptDirectory().getChildFile("counted-extra.bin").replaceWithData(extra.getData(), extra.getSize())
        && directory.getChildFile("unrelated-file.bin").replaceWithData(extra.getData(), extra.getSize()),
        "include every file in usage regardless of whether it is a known recovery record");
    int64_t expectedBytes = 0, expectedJournal = 0, expectedAttempts = 0, expectedOther = 0;
    const auto nativeRoot = files::nativeFile(directory);
    const auto allFiles = nativeRoot.findChildFiles(juce::File::findFiles, true);
    for (const auto& file : allFiles)
    {
        const auto relative = file.getRelativePathFrom(nativeRoot).replaceCharacter('\\', '/');
        const auto size = file.getSize(); expectedBytes += size;
        (relative.startsWith("journals/") ? expectedJournal : relative.startsWith("attempts/") ? expectedAttempts : expectedOther) += size;
    }
    const auto fingerprint = [&]
    {
        std::map<juce::String, juce::String> hashes;
        for (const auto& file : nativeRoot.findChildFiles(juce::File::findFiles, true))
            hashes.emplace(file.getRelativePathFrom(nativeRoot), juce::SHA256(file).toHexString());
        return hashes;
    };
    const auto before = fingerprint();
    const auto report = layout.inspectUsage({});
    good(report.status, "read actual complete storage metadata"); good(report.policyStatus, "verify persisted quota identity and format");
    require(report.present && report.complete && report.policy && *report.policy == configured
        && report.bytes == expectedBytes && report.journalBytes == expectedJournal && report.attemptBytes == expectedAttempts
        && report.otherBytes == expectedOther && report.files == static_cast<size_t>(allFiles.size()) && report.freeBytes >= 0,
        "usage and category totals equal an independent filesystem inventory and display stored non-default limits");
    auto limitedBudget = RecoveryStorageLayout::UsageBudget{}; limitedBudget.entries = 1;
    const auto limited = layout.inspectUsage(limitedBudget);
    require(limited.status.code == "storage_usage_entry_budget" && !limited.complete && limited.entries == 1
        && limited.bytes <= report.bytes, "bounded partial observation never claims a complete total");
    int polls = 0;
    const auto cancelled = layout.inspectUsage({}, [&] { return ++polls == 8; });
    require(cancelled.status.error == Error::cancelled && !cancelled.complete && cancelled.bytes <= report.bytes,
        "explicit cancellation stops actual metadata enumeration and retains an honest partial observation");
    require(fingerprint() == before && store.inspectManifest().head == head
        && document.captureWorkspaceSnapshot()->sameContents(*workspace),
        "all usage paths preserve every physical recovery file, safe head and current full history");
}

void automaticTerminalSweep(const juce::File& root, const juce::File& source)
{
    SingleTrackDocument document; require(document.loadFile(source), "load automatic cleanup current source");
    require(document.moveEnvelopePoint(0, 0, 0.41f) && document.moveEnvelopePoint(0, 0, 0.63f) && document.undo(),
        "current document has real Undo and Redo while old terminal data is discovered");
    const auto workspace = document.captureWorkspaceSnapshot();
    const auto sourceHash = juce::SHA256(source).toHexString();
    const auto snapshot = *document.captureCommittedState(document.getMediaIdentity().document);
    auto settings = config(root.getChildFile("automatic-terminal-sweep")); settings.maintenance.jobs = 1;
    const RecoveryStorageLayout layout(settings.root); good(layout.prepare(), "prepare isolated service storage");
    CommitPacket packet; good(RecoveryCheckpointCodec::encode(snapshot, {}, packet), "capture actual old-owner fixture graph");
    std::vector<juce::Uuid> terminalOwners;
    std::shared_ptr<RecoveryOwnerLease> liveReader;
    for (int i = 0; i < 7; ++i)
    {
        const juce::Uuid owner; terminalOwners.push_back(owner);
        RecoveryStore store(layout.journalDirectory(), owner); SafeHead head;
        good(store.append({}, packet, head), "publish complete isolated owner before terminal disposition");
        OwnerRetirement retirement;
        good(RecoveryOwnerLifecycle::discard(store, head, retirement), "fixture explicitly Discards an old owner before the new service exists");
        if (i == 1)
        {
            TerminalCollectionReport report;
            for (int pass = 0; pass < 8 && report.filesRemoved == 0; ++pass)
                good(store.collectTerminal(head, report, { 1, 32 * 1024 * 1024 }), "prepare genuinely interrupted terminal sweep");
            require(report.filesRemoved > 0 && report.more, "automatic fixture retains a partial terminal intent");
        }
    }
    const juce::Uuid preservedOwner;
    RecoveryStore preserved(layout.journalDirectory(), preservedOwner); SafeHead preservedHead;
    good(preserved.append({}, packet, preservedHead), "publish recoverable crash owner without a terminal marker");
    good(preserved.acquireOwnerLease(liveReader), "keep actual live reader ownership of the nonterminal recovery data");
    const auto unknown = layout.journalDirectory().getChildFile("unrelated-note.txt");
    require(unknown.replaceWithText("preserve unrelated data"), "create unrelated owned root fixture");
    DocumentSaveCoordinator saves(document);
    auto quotaBlock = std::make_unique<juce::FileOutputStream>(files::nativeFile(settings.root.getChildFile("quota.lock")));
    require(quotaBlock->openedOk(), "hold the actual private service transaction file");
    RecoveryService service(document, saves, settings);
    require(!service.needsPoll() && !service.health().maintenance.sweep.active, "service construction never scans old recovery data");
    good(service.sweepTerminalOwnersAfterPresence(), "post-Core presence schedules automatic existing-terminal cleanup");
    await(service, [&] { return !service.needsPoll() && !service.health().maintenance.failure; },
        "real quota contention pauses the one-slot queue without busy polling or dropping pending discovery");
    const auto blocked = service.health();
    require(blocked.maintenance.sweep.active && !blocked.maintenance.workerRunning && blocked.maintenance.jobs.size() == 1
        && preserved.inspectManifest().head == preservedHead,
        "actual quota refusal preserves queued data and the live nonterminal owner while discovery is paused");
    quotaBlock.reset(); service.poll();
    require(!service.health().maintenance.failure, "releasing contention does not implicitly retry failed cleanup");
    good(service.retryMaintenance(blocked.maintenance.jobs.front().owner), "explicit Retry frees capacity for automatic remaining pages");
    await(service, [&] { return !service.needsPoll() && service.health().maintenance.sweep.completed; },
        "automatic sweep resumes after completion consumes the bounded queue slot");
    const auto completed = service.health();
    require(completed.completedOwners == terminalOwners.size() && completed.maintenance.sweep.pages > 1
        && completed.maintenance.sweep.terminalOwners == terminalOwners.size() && completed.maintenance.jobs.empty(),
        "automatic sweep covers multiple pages and more owners than queue capacity");
    for (const auto& owner : terminalOwners)
        require(!layout.journalDirectory().getChildFile(owner.toString()).exists()
            && !layout.journalDirectory().getChildFile("terminal-" + owner.toString()).exists(), "automatic cleanup completes existing marker and intent owners");
    require(preserved.inspectManifest().head == preservedHead && unknown.loadFileAsString() == "preserve unrelated data"
        && document.captureWorkspaceSnapshot()->sameContents(*workspace) && juce::SHA256(source).toHexString() == sourceHash,
        "recoverable owner, unrelated data, current full history and source survive automatic reclamation");
    service.beginClose(); require(service.health().settled, "completed sweep has no cursor, worker or pending ownership");

    juce::WaitableEvent entered, release;
    settings.maintenance.beforeSweepPage = [&] { entered.signal(); require(release.wait(5000), "release only the owned discovery gate"); };
    RecoveryService cancelled(document, saves, settings);
    good(cancelled.sweepTerminalOwnersAfterPresence(), "start a separately cancellable discovery pass");
    require(entered.wait(5000), "actual worker reaches its first bounded page");
    cancelled.beginClose(); release.signal();
    await(cancelled, [&] { return cancelled.health().maintenance.sweep.cancelled && cancelled.health().settled; },
        "close cancels discovery before admission and releases its worker normally");
    require(cancelled.health().maintenance.sweep.terminalOwners == 0 && preserved.inspectManifest().head == preservedHead,
        "cancelled discovery grants no cleanup authority or false completion");
}

void resumePersistedCleanup(const juce::File& root, const juce::File& source, bool partial)
{
    SingleTrackDocument document; require(document.loadFile(source), "load real restart-cleanup document");
    DocumentSaveCoordinator saves(document);
    const auto settings = config(root.getChildFile(partial ? "resume-terminal-intent" : "resume-terminal-marker"));
    RecoveryMutationProducer::Health original;
    {
        RecoveryService previous(document, saves, settings);
        good(previous.attachPostCore(juce::Uuid()), "attach previous session without storage activity");
        require(document.moveEnvelopePoint(0, 0, 0.47f), "capture an actual recoverable mutation before restart");
        protect(previous); original = current(previous);
        previous.leaveDocument();
        await(previous, [&] { return !previous.needsPoll() && previous.health().protection.sessions.empty(); },
            "drain and release the original runtime producer before persisted disposition");
        previous.beginClose(); require(previous.health().settled, "previous runtime is fully settled");
    }
    const auto journal = RecoveryStorageLayout(settings.root).journalDirectory();
    RecoveryStore store(journal, original.owner);
    const auto before = document.captureWorkspaceSnapshot();
    const auto sourceHash = juce::SHA256(source).toHexString();
    {
        RecoveryService rejected(document, saves, settings);
        good(rejected.resumeTerminalCleanup(original.owner, original.writer.safeHead), "accept only a selector for worker validation");
        await(rejected, [&] { return !rejected.needsPoll() && !rejected.health().maintenance.failure; },
            "nonterminal owner is rejected by the actual collection worker");
        require(rejected.health().maintenance.failure.code == "terminal_marker_required"
            && store.inspectManifest().head == original.writer.safeHead,
            "Resume cannot turn a recoverable owner into a terminal owner or remove its head");
        std::optional<OwnerRetirement> marker;
        good(store.inspectRetirement(marker), "check that failed Resume did not publish a marker");
        require(!marker, "Resume never grants Discard or Saved authority");
    }
    OwnerRetirement retirement;
    good(RecoveryOwnerLifecycle::discard(store, original.writer.safeHead, retirement),
        "fixture explicitly authorizes the selected owner's durable Discard before restart");
    if (partial)
    {
        TerminalCollectionReport report;
        for (int pass = 0; pass < 8 && report.filesRemoved == 0; ++pass)
            good(store.collectTerminal(original.writer.safeHead, report, { 1, 32 * 1024 * 1024 }),
                "perform bounded physical cleanup before simulating interrupted runtime");
        require(report.filesRemoved > 0 && report.more && !report.completed,
            "fixture has a real partial sweep and a remaining durable intent");
        require(store.inspectSummary().status.code == "owner_collecting", "discovery retains the interrupted intent without enlarging its read budget");
        SafeHead discovered;
        good(store.inspectTerminalHead(discovered), "explicit control lookup recovers the persisted exact head");
        require(discovered == original.writer.safeHead, "persisted intent resolves the original head without numeric ordering");
    }
    else
    {
        const auto summary = store.inspectSummary();
        good(summary.status, "discover previously published terminal marker");
        require(summary.retired && summary.head == original.writer.safeHead, "marker summary carries the selected terminal head");
    }
    RecoveryService restarted(document, saves, settings);
    require(restarted.health().maintenance.jobs.empty() && !restarted.health().maintenance.workerRunning,
        "fresh service does not scan or start cleanup before explicit Resume");
    const auto selector = partial ? SafeHead{} : original.writer.safeHead;
    good(restarted.resumeTerminalCleanup(original.owner, selector), "new service resumes the persisted terminal owner without a new Save or Discard ticket");
    await(restarted, [&] { return !restarted.needsPoll() && restarted.health().completedOwners == 1; },
        "new service finishes bounded cleanup of the previous runtime's terminal owner");
    require(!files::nativeFile(journal.getChildFile(original.owner.toString())).exists()
        && !files::nativeFile(journal.getChildFile("terminal-" + original.owner.toString())).exists()
        && restarted.health().removedFiles > 0 && document.captureWorkspaceSnapshot()->sameContents(*before)
        && juce::SHA256(source).toHexString() == sourceHash,
        "resumed cleanup removes only the terminated recovery data and preserves current document, complete history and source");
    SafeHead alreadyGone = original.writer.safeHead;
    good(store.inspectTerminalHead(alreadyGone), "a stale selected intent may already have been collected by another runtime");
    require(alreadyGone.sequence == 0 && alreadyGone.hash.isEmpty(), "absent owner and intent produce explicit no-work completion");
    restarted.beginClose(); require(restarted.health().settled, "resumed cleanup retires its queue and worker normally");
}
}

void runRecoveryServiceTests(const juce::File& parent)
{
    const auto root = parent.getChildFile("recovery-service-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create unique service test directory");
    const auto source = root.getChildFile("source.wav"); makeSource(source);
    serviceControlDiagnostics(root, source);
    automaticHandoff(root, source); backpressure(root, source); degradedClose(root, source); reviewedDiscard(root, source);
    lightweightDiscovery(root, source);
    resumePersistedCleanup(root, source, false); resumePersistedCleanup(root, source, true);
    automaticTerminalSweep(root, source); storageUsage(root, source);
    require(root.getParentDirectory() == parent && root.getFileName().startsWith("recovery-service-")
        && files::nativeFile(root).deleteRecursively(), "remove only owned service fixtures after all lifetime owners release");
    std::cout << "RecoveryServiceTests passed (inert admission, real Save handoff, bounded tickets, actual locks, Retry, cancelled close, reviewed Discard scope and resumable retirement)\n";
}

void runRecoveryCapacityPilot(const juce::File& root)
{
    for (const int minutes : { 10, 60 })
    {
        const auto source = root.getChildFile("source-" + juce::String(minutes) + "min.wav");
        const auto receipt = juce::JSON::parse(source.getSiblingFile(source.getFileName() + ".json"));
        require(source.existsAsFile() && receipt.isObject(), "generated capacity source and receipt exist");
        const auto originalHash = juce::SHA256(source).toHexString();
        require(originalHash == receipt["sha256"].toString(), "full PCM source matches generated receipt");
        SingleTrackDocument document;
        require(document.loadFile(source), "real multichannel source opens through Document");
        const auto initial = document.captureCommittedState(document.getMediaIdentity().document);
        const auto frames = static_cast<int64_t>(48000) * minutes * 60;
        require(initial && initial->state->info.numChannels == 8 && initial->state->info.numSamples == frames,
            "actual 7.1 dimensions match the pilot");
        DocumentSaveCoordinator saves(document);
        RecoveryService::Config settings; settings.root = root.getChildFile("pilot-" + juce::String(minutes) + "min-recovery");
        settings.sessions.session.producer.graph.materialisedBytes = 256 * 1024 * 1024;
        require(!settings.root.exists(), "capacity pilot never reuses prior recovery data");
        RecoveryService service(document, saves, settings);
        good(service.attachPostCore(juce::Uuid()), "clean long source attaches without recovery allocation");
        for (const float value : { 0.41f, 0.62f, 0.73f })
            require(document.moveEnvelopePoint(0, 0, value), "editing continues after capacity refusal");
        require(document.undo(), "Undo remains usable while protection is degraded");
        const auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
        const auto workspace = document.captureWorkspaceSnapshot();
        require(snapshot && snapshot->undo.size() == 2 && snapshot->redo.size() == 1 && workspace,
            "complete current Undo and Redo branches remain");
        auto health = service.health();
        const auto refusal = health.protection.failure;
        const auto& capacity = refusal.capacity;
        const auto pcmBytes = static_cast<uint64_t>(frames) * 8 * sizeof(float);
        require(!refusal && refusal.code == "producer_audio_graph_budget"
            && capacity.resource == ByteCapacity::Resource::audioGraph && capacity.lowerBound
            && capacity.required >= pcmBytes && capacity.required < pcmBytes + 65536
            && capacity.limit == settings.sessions.session.producer.graph.materialisedBytes,
            "explicit smaller graph limit reports the actual logical PCM requirement and configured cap");
        const auto retry = service.checkpointCurrent();
        require(!retry && retry.capacity.required >= pcmBytes && !settings.root.exists()
            && document.captureWorkspaceSnapshot()->sameContents(*workspace),
            "Retry above the same limit preserves history, creates no false head and reports capacity again");
        for (size_t i = 0; i < snapshot->undo.size(); ++i) require(document.undo(), "all current Undo states remain available");
        for (size_t i = 0; i < snapshot->undo.size(); ++i) require(document.redo(), "all current Redo states return to the edit");
        require(document.redo() && document.undo(), "original Redo branch remains reversible");
        require(juce::SHA256(source).toHexString() == originalHash, "capacity handling does not modify the generated source");
        service.beginClose();
        std::cout << "Capacity pilot: minutes=" << minutes << "; channels=8; frames=" << frames
            << "; required_lower_bound=" << capacity.required << "; limit=" << capacity.limit
            << "; failure=" << refusal.code << "; protected=false; source_unchanged=true; current_history=complete\n";
    }
    std::cout << "RecoveryCapacityPilot passed (real 10/60 minute 7.1 readers; explicitly configured smaller graph limit, current history preserved)\n";
}

static void verifyLargeDspStaging(const juce::File& source)
{
    SingleTrackDocument document;
    require(document.loadFile(source), "large staging fixture uses actual source admission");
    const auto original = document.getState();
    juce::AudioBuffer<float> block, rendered;
    {
        auto cancelled = document.beginEqualLengthAudioMutation(original);
        require(cancelled && cancelled->readSourceBlock(0, 4096, block)
            && cancelled->writeRenderedBlock(0, block, 0, 4096)
            && !cancelled->readRenderedBlock(0, 64, rendered),
            "partial large output remains unpublished and unavailable for verification");
    }
    require(document.getState() == original && !document.canUndo(),
        "abandoning a partially written transaction preserves the document and history");
    auto transaction = document.beginEqualLengthAudioMutation(original);
    require(transaction != nullptr, "create large file-backed verification transaction");
    const auto frames = original->info.numSamples;
    for (int64_t start = 0; start < frames; start += 65536)
    {
        const auto count = static_cast<int>(juce::jmin<int64_t>(65536, frames - start));
        require(transaction->readSourceBlock(start, count, block)
            && transaction->writeRenderedBlock(start, block, 0, count),
            "write complete staged audio in bounded blocks");
    }
    for (const auto start : { int64_t(32), frames / 2, frames - 128 })
    {
        require(transaction->readSourceBlock(start, 64, block)
            && transaction->readRenderedBlock(start, 64, rendered),
            "post-render verification reads the completed private WAV");
        for (int c = 0; c < block.getNumChannels(); ++c) for (int i = 0; i < 64; ++i)
            require(block.getSample(c, i) == rendered.getSample(c, i),
                "post-render reads preserve all channel PCM exactly");
    }
    require(document.moveEnvelopePoint(0, 0, 0.8f), "create a newer real document edit before publication");
    const auto newer = document.getState();
    require(!document.commitEqualLengthAudioMutation(std::move(transaction)) && document.getState() == newer,
        "completed stale large output cannot replace the newer document");
    std::cout << "Large DSP staging passed: partial abandonment, post-render reads, stale publication" << std::endl;
}

static juce::File runRecoveryLargeAudioPilotImpl(const juce::File& source, const juce::File& root, bool applyDsp)
{
    juce::File ownedDspFile;
    const auto receipt = juce::JSON::parse(source.getSiblingFile(source.getFileName() + ".json"));
    require(source.existsAsFile() && receipt.isObject() && !root.exists(), "large pilot uses a generated source and fresh evidence directory");
    require(root.createDirectory().wasOk(), "create isolated large pilot output");
    std::cout << "Large pilot: hashing source " << source.getFileName() << std::endl;
    const auto sourceHash = juce::SHA256(source).toHexString();
    require(sourceHash == receipt["sha256"].toString(), "entire source matches generator receipt");
    SingleTrackDocument document;
    require(document.loadFile(source), "large source opens through actual Document");
    const auto frames = document.getState()->info.numSamples;
    require(document.getState()->info.numChannels == 8 && document.getState()->info.sampleRate == 48000
        && (frames == int64_t(48000) * 600 || frames == int64_t(48000) * 3600), "actual 10/60 minute 7.1 dimensions");
    const auto pcmBytes = static_cast<size_t>(frames) * 8 * sizeof(float);
    if (applyDsp) verifyLargeDspStaging(source);
    DocumentSaveCoordinator saves(document);
    RecoveryService::Config settings; settings.root = root.getChildFile("recovery");
    RecoveryService service(document, saves, settings);
    good(service.attachPostCore(juce::Uuid()), "large source uses default post-Core Service admission");
    const auto started = std::chrono::steady_clock::now();
    if (applyDsp)
    {
        require(pcmBytes > 256ULL * 1024 * 1024 && pcmBytes <= media::maximumMaterialisedBytes,
            "large DSP pilot exercises the actual spill threshold without widening edit admission");
        auto processor = AudioSuiteReferenceProcessor::describe().create();
        const auto original = document.getState();
        const auto analyzed = processor->analyze(original, 501, {}, 0.5f, 1.0);
        require(analyzed.succeeded(), "large DSP plan uses the actual processor");
        AudioSuiteCancellationToken cancelled; cancelled.cancel();
        require(processor->apply(document, analyzed.plan, 501, cancelled).status == AudioSuiteApplyStatus::cancelled
            && document.getState() == original && !document.canUndo(), "cancelled large Apply preserves source and history");
        require(processor->apply(document, analyzed.plan, 501, {}, 16384).succeeded(), "large block-rendered DSP Apply commits");
        const auto rendered = document.getState();
        ownedDspFile = rendered->readerSourceFile;
        require(rendered->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::streamingReader
            && rendered->streamingReader && !rendered->audioBuffer
            && rendered->readerSourceFile != source && rendered->sourceFileWitness,
            "large DSP publishes an owned immutable reader instead of a full PCM buffer");
        auto before = SingleTrackDocument::createReaderForFile(source);
        auto after = SingleTrackDocument::createReaderForFile(rendered->readerSourceFile);
        require(before && after && after->usesFloatingPointData && after->bitsPerSample == 32,
            "actual private output is lossless floating-point WAV");
        AudioEngine renderedEngine(document);
        SingleTrackDocument unprocessed;
        require(unprocessed.adoptLoadedState(std::make_shared<SingleTrackDocument::State>(*original)),
            "playback oracle retains original PCM and source channel roles");
        AudioEngine unprocessedEngine(unprocessed);
        for (const auto start : { int64_t(1024), frames / 2, frames - 2048 })
        {
            juce::AudioBuffer<float> a(8, 64), b(8, 64), played(8, 64), unprocessedPlayback(8, 64);
            require(before->read(&a, 0, 64, start, true, true) && after->read(&b, 0, 64, start, true, true)
                && renderedEngine.renderDocumentBlockForDiagnostics(start, played)
                && unprocessedEngine.renderDocumentBlockForDiagnostics(start, unprocessedPlayback),
                "read real source and DSP output across all channels");
            for (int c = 0; c < 8; ++c) for (int i = 0; i < 64; ++i)
            {
                require(std::abs(b.getSample(c, i) - a.getSample(c, i) * 0.5f) < 1.0e-7f,
                    "actual DSP changes PCM without quantization or channel loss");
                require(std::abs(played.getSample(c, i) - unprocessedPlayback.getSample(c, i) * 0.5f) < 1.0e-7f,
                    "actual AudioEngine plays processed PCM with unchanged source-to-output routing");
            }
        }
        require(document.undo() && document.getState() == original && document.redo()
            && document.getState() == rendered, "large DSP keeps exact original and rendered Undo/Redo states");
        // These two real history transitions are also protected by the service.
        std::cout << "Large DSP applied: owned_float_wave=" << rendered->readerSourceFile.getFullPathName()
            << "; pcm_bytes=" << pcmBytes << std::endl;
    }
    const size_t undoCount = applyDsp ? 3u : 2u;
    const auto expectedHead = applyDsp ? 7u : 4u; // DSP Apply, Undo, Redo plus the existing four edits.
    const auto mappingBytes = pcmBytes * (applyDsp ? 2u : 1u);
    for (const float value : { 0.41f, 0.62f, 0.73f })
        require(document.moveEnvelopePoint(0, 0, value), "large source commits real envelope edits");
    require(document.undo(), "large source keeps a real Redo branch while capture is pending");
    const auto expected = *document.captureCommittedState(document.getMediaIdentity().document);
    const auto workspace = document.captureWorkspaceSnapshot();
    require(expected.undo.size() == undoCount && expected.redo.size() == 1, "known complete large fixture history");
    auto announced = started - 30s;
    while (std::chrono::steady_clock::now() < started + 10min)
    {
        if (service.needsPoll()) service.poll();
        const auto value = current(service);
        good(value.failure, "large reader stays admitted throughout actual protection");
        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= 15s)
        {
            std::cout << "Protect: elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()
                << "; pending=" << value.writer.pendingPackets << "; retained=" << value.writer.retainedBytes
                << "; head=" << value.writer.safeHead.sequence << std::endl;
            announced = now;
        }
        if (!value.writer.workerRunning && value.phase == RecoveryMutationProducer::Phase::protectedState) break;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    const auto protectedHead = current(service);
    require(protectedHead.phase == RecoveryMutationProducer::Phase::protectedState
        && protectedHead.writer.safeHead.sequence == expectedHead, "all real edits reach the exact durable head");
    const auto protectionMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "Protected: elapsed_ms=" << protectionMs << "; inspecting manifest" << std::endl;
    RecoveryStore store(RecoveryStorageLayout(settings.root).journalDirectory(), protectedHead.owner);
    const auto manifest = store.inspectManifest(); good(manifest.status, "actual large journal manifest is readable");
    PreparedRecoveryCheckpoint prepared;
    good(RecoveryCheckpointCodec::prepareThrough(manifest, protectedHead.writer.safeHead, prepared), "replay exact protected history without PCM allocation");
    size_t chunks = 0;
    for (const auto& sequence : prepared.getAudioGraph().sequences)
        for (const auto& chunk : sequence.chunks)
        {
            ++chunks;
            require(static_cast<uint64_t>(chunk.frames) * sequence.channels * sizeof(float)
                <= AudioGraphLimits{}.chunkSampleBytes, "large capture respects actual per-block bound");
        }
    require(chunks > 64 && chunks <= maximumCommitAssets && prepared.getUndoCount() == undoCount && prepared.getRedoCount() == 1,
        "bounded large graph retains every current and history reference");
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> eager;
    require(RecoveryAudioGraph::materialise(prepared.getAudioJson(), store, eager).code == "audio_graph_eager_budget" && eager.empty(),
        "eager full-PCM path still refuses before allocating a large graph");

    RecoveryRestoreQueue::Config restoreConfig;
    restoreConfig.root = settings.root; restoreConfig.scratch = root.getChildFile("scratch");
    restoreConfig.appBuild = "large-audio-capacity-pilot";
    RecoveryRestoreQueue queue(restoreConfig);
    RecoveryRestoreQueue::Request request;
    request.owner = protectedHead.owner; request.observedHead = protectedHead.writer.safeHead; request.selectedHead = protectedHead.writer.safeHead;
    juce::Uuid attempt; good(queue.start(request, attempt), "start actual production restore queue");
    std::shared_ptr<const RecoveredCandidateDocument> candidate;
    std::shared_ptr<RecoveredPlaylistDocument> playlist;
    bool fullPublished = false;
    const auto restoreStarted = std::chrono::steady_clock::now(); announced = restoreStarted - 30s;
    while (std::chrono::steady_clock::now() < restoreStarted + 10min)
    {
        const auto health = queue.health(); good(health.failure, "restore queue remains healthy");
        require(health.jobs.size() == 1 && health.jobs.front().attempt == attempt, "exact large restore attempt remains observable");
        const auto& job = health.jobs.front(); good(job.failure, "large progressive restore stays within production limits");
        for (const auto& publication : queue.takePublications())
        {
            require(publication.attempt == attempt, "only selected attempt publishes");
            if (publication.core)
            {
                candidate = publication.core; playlist = RecoveredPlaylistDocument::fromCandidate(*candidate);
                require(playlist != nullptr && candidate->document->requiresRecoverySaveAs(), "real playlist accepts recovered full workspace");
                good(queue.acknowledgeCore(attempt, true), "acknowledge actual recovered playlist admission");
                std::cout << "Restore: actual Core admission; logical_mapping_bytes=" << health.residentMappingBytes << std::endl;
            }
            if (publication.fullPeaks)
            {
                require(candidate && publication.originalCore == candidate->coreState && !publication.fullPeaks->isPreview
                    && publication.fullPeaks->loadedSampleRange.getEnd() == frames, "full waveform publication covers actual large tail");
                fullPublished = true;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - announced >= 15s)
        {
            std::cout << "Restore: elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(now - restoreStarted).count()
                << "; stage=" << static_cast<int>(job.stage) << "; logical_mapping_bytes=" << health.residentMappingBytes << std::endl;
            announced = now;
        }
        if (job.stage == RecoveryAttemptStage::ready && !health.workerRunning && fullPublished) break;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    require(candidate && playlist && fullPublished && queue.health().jobs.front().stage == RecoveryAttemptStage::ready
        && queue.health().residentMappingBytes == mappingBytes, "full queue restoration completes and retains exact logical mapping reservation");
    const auto restoreMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restoreStarted).count();
    SingleTrackDocument recovered; juce::String error;
    require(recovered.adoptWorkspaceSnapshot(*playlist->snapshot(), error), "select recovered workspace without reopening source or losing history");
    auto history = recovered.captureCommittedState(recovered.getMediaIdentity().document);
    require(history && history->undo.size() == undoCount && history->redo.size() == 1, "selected recovered Document retains both history branches");
    AudioEngine recoveredEngine(recovered);
    const auto compare = [&](const SingleTrackDocument::StatePtr& state)
    {
        SingleTrackDocument oracle;
        require(oracle.adoptLoadedState(std::make_shared<SingleTrackDocument::State>(*state)),
            "independent expected-state oracle retains the exact historical audio backing");
        AudioEngine oracleEngine(oracle);
        for (const auto start : { int64_t(32), frames / 2, frames - 128 })
        {
            juce::AudioBuffer<float> originalAudio(8, 64), recoveredAudio(8, 64);
            require(oracleEngine.renderDocumentBlockForDiagnostics(start, originalAudio)
                && recoveredEngine.renderDocumentBlockForDiagnostics(start, recoveredAudio), "all eight recovered channels play prefix, middle and tail");
            for (int c = 0; c < 8; ++c) for (int i = 0; i < 64; ++i)
            {
                const auto difference = std::abs(recoveredAudio.getSample(c, i) - originalAudio.getSample(c, i));
                if (difference >= 1.0e-6f)
                    std::cerr << "History PCM mismatch: sample=" << start + i << "; channel=" << c
                        << "; expected=" << originalAudio.getSample(c, i) << "; actual=" << recoveredAudio.getSample(c, i) << '\n';
                require(difference < 1.0e-6f,
                    "restored current and every historical DSP/envelope state produce the expected PCM");
            }
        }
    };
    compare(expected.state);
    require(recovered.redo(), "large restored original Redo works"); compare(expected.redo.back());
    require(recovered.undo(), "large restored current returns");
    for (auto it = expected.undo.rbegin(); it != expected.undo.rend(); ++it) { require(recovered.undo(), "every large restored Undo works"); compare(*it); }
    for (size_t i = 0; i < expected.undo.size(); ++i) require(recovered.redo(), "every large restored Redo returns current");
    compare(expected.state);
    std::cout << "Restore complete: elapsed_ms=" << restoreMs << "; Save As started" << std::endl;
    DocumentSaveCoordinator recoveredSaves(recovered); bool saved = false;
    const auto output = root.getChildFile("recovered-save.wav");
    require(recoveredSaves.requestSave(recovered.createSaveSnapshot(output, SingleTrackDocument::SaveIntent::saveAs), [&](const auto& result)
    { require(result.succeeded && result.publication && !recovered.hasUnsavedChanges(), "large recovered Save As publishes actual clean Document"); saved = true; }), "large recovered Save As starts");
    const auto saveDeadline = std::chrono::steady_clock::now() + 10min;
    while ((!saved || recoveredSaves.isSaveInProgress()) && std::chrono::steady_clock::now() < saveDeadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    require(saved && !recoveredSaves.isSaveInProgress() && output.getSize() > 0, "large Save As finishes with an actual file");
    auto savedReader = SingleTrackDocument::createReaderForFile(output);
    auto expectedReader = SingleTrackDocument::createReaderForFile(expected.state->readerSourceFile != juce::File()
        ? expected.state->readerSourceFile : source);
    require(savedReader && expectedReader && savedReader->numChannels == 8
        && savedReader->sampleRate == 48000 && savedReader->lengthInSamples == frames && savedReader->bitsPerSample == 16,
        "saved physical file preserves large source dimensions and selected encoding");
    for (const auto start : { int64_t(1024), frames / 2, frames - 2048 })
    {
        juce::AudioBuffer<float> expectedPcm(8, 64), savedPcm(8, 64);
        require(expectedReader->read(&expectedPcm, 0, 64, start, true, true)
            && savedReader->read(&savedPcm, 0, 64, start, true, true), "read actual saved and expected PCM");
        for (int c = 0; c < 8; ++c) for (int i = 0; i < 64; ++i)
        {
            const auto envelope = SingleTrackDocument::evaluateEnvelope(expected.state->envelopePoints,
                static_cast<double>(start + i) / 48000.0);
            require(std::abs(savedPcm.getSample(c, i) - expectedPcm.getSample(c, i) * envelope) < 1.1f / 32768.0f,
                "Save As encodes recovered DSP PCM even while the different historical source remains present");
        }
    }
    history = recovered.captureCommittedState(recovered.getMediaIdentity().document);
    require(history && history->undo.size() == undoCount && history->redo.size() == 1 && document.captureWorkspaceSnapshot()->sameContents(*workspace),
        "Save As preserves both complete histories and original workspace");
    std::cout << "Save As complete; verifying source hash" << std::endl;
    require(juce::SHA256(source).toHexString() == sourceHash, "large source remains byte-for-byte unchanged");
    service.beginClose(); queue.close();
    std::cout << "RecoveryLargeAudioPilot passed: frames=" << frames << "; channels=8; chunks=" << chunks
        << "; logical_mapping_bytes=" << mappingBytes << "; dsp=" << applyDsp << "; protection_ms=" << protectionMs << "; restore_ms=" << restoreMs
        << "; full_history=true; saved=true; saved_pcm_verified=true; source_unchanged=true" << std::endl;
    return ownedDspFile;
}

void runRecoveryLargeAudioPilot(const juce::File& source, const juce::File& root, bool applyDsp)
{
    const auto owned = runRecoveryLargeAudioPilotImpl(source, root, applyDsp);
    if (applyDsp)
    {
        require(owned != juce::File() && !owned.exists(),
            "last Document/history/reader release removes only its private DSP output");
        std::cout << "Large DSP normal-lifetime cleanup passed" << std::endl;
    }
}

void runRecoveryOrdinaryAudioTests(const juce::File& root, bool burst)
{
    // The caller supplies an isolated copy of the ordinary Core corpus. Keep
    // all files as reviewable evidence; never save to or remove the real corpus.
    const auto source = root.getChildFile("source.wav");
    require(source.existsAsFile(), "ordinary recovery fixture source exists");
    const auto sourceHash = juce::SHA256(source).toHexString();
    SingleTrackDocument document;
    require(document.loadFile(source), "load actual ordinary corpus through Document reader admission");
    DocumentSaveCoordinator saves(document);
    RecoveryService::Config settings; settings.root = root.getChildFile("recovery");
    auto entered = std::make_shared<juce::WaitableEvent>(), release = std::make_shared<juce::WaitableEvent>();
    auto held = std::make_shared<std::atomic<bool>>(false);
    if (burst) settings.sessions.session.producer.queue.onBoundary = [entered, release, held](AsyncRecoveryWriter::Boundary boundary)
    {
        if (boundary == AsyncRecoveryWriter::Boundary::beforePrepare && !held->exchange(true))
        { entered->signal(); (void) release->wait(5000); }
    };
    RecoveryService service(document, saves, settings);
    good(service.attachPostCore(juce::Uuid()), "attach ordinary source using production default budgets");
    const auto started = std::chrono::steady_clock::now();
    require(document.moveEnvelopePoint(0, 0, 0.41f), "edit actual ordinary source");
    if (burst)
    {
        require(entered->wait(5000), "first actual preparation waits while subsequent edits enter the real queue");
        for (const float value : { 0.52f, 0.56f, 0.59f })
            require(document.moveEnvelopePoint(0, 0, value), "rapid envelope edits share the immutable original audio");
        require(document.undo(), "rapid Undo keeps the exact Redo branch before protection catches up");
        const auto pending = current(service);
        std::cout << "Burst admission: failure=" << pending.failure.code << "; pending=" << pending.writer.pendingPackets
            << "; retained=" << pending.writer.retainedBytes << '\n';
        release->signal();
        require(pending.failure && !pending.checkpointRequired && pending.writer.pendingPackets == 5
            && pending.writer.retainedBytes < settings.sessions.session.producer.queue.retainedBytes,
            "five real edits sharing one large backing fit the unchanged default queue budget without a continuity gap");
    }
    const auto deadline = started + 60s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (service.needsPoll()) service.poll();
        const auto value = current(service);
        if (!value.writer.workerRunning && (!value.failure || value.phase == RecoveryMutationProducer::Phase::protectedState))
        {
            std::cout << "Ordinary recovery result: " << value.failure.code << "; head=" << value.writer.safeHead.sequence
                      << "; elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << '\n';
            good(value.failure, "production defaults protect actual ordinary audio");
            require(value.phase == RecoveryMutationProducer::Phase::protectedState, "only actual durable publication protects the edit");
            break;
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    require(current(service).phase == RecoveryMutationProducer::Phase::protectedState,
        "ordinary recovery completes within fixture deadline");
    require(document.moveEnvelopePoint(0, 0, 0.63f), "second ordinary envelope edit"); protect(service);
    require(document.moveEnvelopePoint(0, 0, 0.72f), "third ordinary envelope edit"); protect(service);
    require(document.undo(), "ordinary Undo retains a real Redo branch"); protect(service);
    const auto expected = *document.captureCommittedState(document.getMediaIdentity().document);
    const auto workspace = document.captureWorkspaceSnapshot();
    require(expected.undo.size() == (burst ? 4 : 2) && expected.redo.size() == 1, "ordinary fixture retains complete known history");
    CapturedAudioGraph rejected;
    AudioGraphLimits smaller; smaller.chunks = 64;
    require(RecoveryAudioGraph::capture(expected, {}, rejected, smaller).code == "audio_graph_chunk_budget"
        && rejected.newAudio.empty(), "explicit smaller chunk budget still rejects before PCM allocation");
    smaller = {}; smaller.materialisedBytes = 128 * 1024 * 1024;
    require(!RecoveryAudioGraph::capture(expected, {}, rejected, smaller) && rejected.newAudio.empty(),
        "independent total PCM byte bound still rejects the larger ordinary source");
    const auto protectedHead = current(service);
    RecoveryStore store(RecoveryStorageLayout(settings.root).journalDirectory(), protectedHead.owner);
    const auto manifest = store.inspectManifest(); good(manifest.status, "read actual large-asset manifest");
    PreparedRecoveryCheckpoint prepared;
    good(RecoveryCheckpointCodec::prepareThrough(manifest, protectedHead.writer.safeHead, prepared),
        "replay actual checkpoint plus envelope and Undo records with more than 64 audio assets");
    size_t chunks = 0;
    for (const auto& sequence : prepared.getAudioGraph().sequences) chunks += sequence.chunks.size();
    require(chunks > 64 && chunks <= maximumCommitAssets && prepared.getUndoCount() == expected.undo.size()
        && prepared.getRedoCount() == expected.redo.size(), "all large-source chunks and history survive record preparation");
    RecoveryRestoreCandidate::Request request;
    request.observedOwnerHead = protectedHead.writer.safeHead; request.checkpointHead = protectedHead.writer.safeHead;
    request.scratchDirectory = root.getChildFile("restore-scratch");
    request.attemptDirectory = root.getChildFile("restore-attempts"); request.appBuild = "ordinary-capacity-tests";
    require(request.scratchDirectory.createDirectory().wasOk(), "create isolated progressive reader scratch");
    RecoveryRestoreCandidate candidate(store, request);
    std::shared_ptr<const RecoveredCandidateDocument> restored;
    for (size_t step = 0; step < 1024 && candidate.getStage() != RecoveryAttemptStage::ready; ++step)
    {
        good(candidate.advance(), "advance actual ordinary progressive restore");
        if (candidate.getStage() == RecoveryAttemptStage::coreReady && !restored)
            good(candidate.acquireCore(candidate.getAttemptId(), restored), "acquire actual playable and visible recovery Document");
    }
    require(restored && candidate.getStage() == RecoveryAttemptStage::ready && restored->document->hasUnsavedChanges(),
        "full ordinary recovery finishes with an independent dirty Document");
    auto& recovered = *restored->document;
    auto history = recovered.captureCommittedState(recovered.getMediaIdentity().document);
    require(history && history->undo.size() == expected.undo.size() && history->redo.size() == expected.redo.size(),
        "progressive recovered Document retains complete Undo and Redo");
    AudioEngine originalEngine(document), recoveredEngine(recovered);
    for (const auto start : { int64_t(32), expected.state->info.numSamples / 2, expected.state->info.numSamples - 128 })
    {
        juce::AudioBuffer<float> originalAudio(2, 64), recoveredAudio(2, 64);
        require(originalEngine.renderDocumentBlockForDiagnostics(start, originalAudio)
            && recoveredEngine.renderDocumentBlockForDiagnostics(start, recoveredAudio), "ordinary source and full restored tail both play");
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 64; ++i)
            require(originalAudio.getSample(c, i) == recoveredAudio.getSample(c, i), "actual restored playback equals original at prefix, middle and tail");
    }
    const auto compareEnvelope = [&] (const SingleTrackDocument::StatePtr& expectedState)
    {
        require(recovered.getState()->info.numSamples == expectedState->info.numSamples
            && SingleTrackDocument::evaluateEnvelope(recovered.getState()->envelopePoints, 0)
                == SingleTrackDocument::evaluateEnvelope(expectedState->envelopePoints, 0), "restored history has the exact original edit state");
    };
    require(recovered.redo(), "recovered original Redo works"); compareEnvelope(expected.redo.back());
    require(recovered.undo(), "return recovered current"); compareEnvelope(expected.state);
    for (auto it = expected.undo.rbegin(); it != expected.undo.rend(); ++it)
    { require(recovered.undo(), "every recovered Undo works"); compareEnvelope(*it); }
    for (size_t i = 0; i < expected.undo.size(); ++i) require(recovered.redo(), "every recovered Redo returns current");
    compareEnvelope(expected.state);
    DocumentSaveCoordinator recoveredSaves(recovered);
    save(recoveredSaves, recovered, root.getChildFile("recovered-save.wav"));
    history = recovered.captureCommittedState(recovered.getMediaIdentity().document);
    require(history && history->undo.size() == expected.undo.size() && history->redo.size() == expected.redo.size()
        && document.captureWorkspaceSnapshot()->sameContents(*workspace) && juce::SHA256(source).toHexString() == sourceHash,
        "real recovered Save As preserves both full histories and original source bytes");
    service.beginClose();
    std::cout << "RecoveryOrdinaryAudioTests passed (" << chunks << " chunks, default Service, progressive full restore, all Undo/Redo, real Save As)\n";
}
