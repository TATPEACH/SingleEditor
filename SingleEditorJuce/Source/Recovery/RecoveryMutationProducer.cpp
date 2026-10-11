#include "RecoveryMutationProducer.h"
#include <limits>
#include <set>

namespace singleeditor::recovery
{
RecoveryMutationProducer::RecoveryMutationProducer(SingleTrackDocument& doc, Config settings,
                                                     media::IdentitySnapshot identity, bool observe)
    : config(std::move(settings)), observed(std::move(identity))
{
    if (observe)
        capture = std::make_unique<DocumentRecoveryCapture>(doc,
            [this] (DocumentRecoveryCapture::Capture captured) { consume(std::move(captured)); });
}

Status RecoveryMutationProducer::attachPostCore(SingleTrackDocument& document, Config settings,
                                                 std::unique_ptr<RecoveryMutationProducer>& output, Admission admission) try
{
    const auto identity = document.getMediaIdentity();
    if (output || !identity.document.isValid() || identity.source.binding.isNull()
        || settings.root == juce::File())
        return { Error::invalidPacket, "producer_admission" };
    const bool dirty = identity.savedGeneration != identity.document.generation;
    if (admission == Admission::observeClean && dirty)
        return { Error::staleHead, "producer_dirty_admission" };
    if (admission != Admission::observeClean && admission != Admission::checkpointDirty)
        return { Error::invalidPacket, "producer_admission_mode" };
    if (admission == Admission::checkpointDirty && !dirty)
        return { Error::invalidPacket, "producer_checkpoint_not_dirty" };
    std::optional<SingleTrackDocument::CommittedStateSnapshot> initial;
    if (admission == Admission::checkpointDirty)
    {
        initial = document.captureCommittedState(identity.document);
        if (!initial || !initial->identity.source.sameBinding(identity.source)
            || initial->identity.savedGeneration != identity.savedGeneration)
            return { Error::staleHead, "producer_checkpoint_capture_changed" };
    }
    auto candidate = std::unique_ptr<RecoveryMutationProducer>(new RecoveryMutationProducer(document, std::move(settings), identity));
    if (initial)
    {
        const auto accepted = candidate->enqueue(std::move(*initial));
        if (!accepted) return accepted;
    }
    output = std::move(candidate);
    return {};
}
catch (...) { return { Error::resourceOrEnvironmentFault, "producer_admission_exception" }; }

RecoveryMutationProducer::~RecoveryMutationProducer() { close(); }

Status RecoveryMutationProducer::estimate(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
                                          size_t& result, std::vector<AsyncRecoveryWriter::RetainedBacking>* sharedBackings,
                                          size_t* logicalAudioBytes) const
{
    if (snapshot.undo.size() >= config.graph.states
        || snapshot.redo.size() >= config.graph.states - snapshot.undo.size())
        return { Error::budgetExceeded, "producer_state_budget" };
    size_t bytes = 0, refusedRequirement = 0, logicalBytes = 0;
    const auto capacityFailure = [&]() -> Status
    {
        if (refusedRequirement == 0) return { Error::budgetExceeded, "producer_retained_budget" };
        return byteCapacityFailure("producer_retained_budget", ByteCapacity::Resource::retainedMemory,
            refusedRequirement, config.queue.retainedBytes, true);
    };
    // In-memory DSP remains fully retained. Reader-backed sources reserve bounded
    // block work instead of their entire logical decoded length. Existing restore
    // mappings retain their separate allocation lease; this is not a RAM meter.
    const auto add = [&] (size_t amount)
    {
        if (amount > config.queue.retainedBytes || bytes > config.queue.retainedBytes - amount)
        {
            if (amount <= std::numeric_limits<size_t>::max() - bytes) refusedRequirement = bytes + amount;
            return false;
        }
        bytes += amount;
        return true;
    };
    if (snapshot.sourceVersion)
    {
        const auto& source = *snapshot.sourceVersion;
        if (!add(sizeof(media::SourceFileWitness))) return capacityFailure();
        for (const auto& text : { source.file.getFullPathName(), source.volumeId, source.fileId, source.readerFormat })
            if (!add(text.getNumBytesAsUTF8() + 1)) return capacityFailure();
    }
    std::set<const void*> backings;
    std::vector<AsyncRecoveryWriter::RetainedBacking> retainedBackings;
    const auto include = [&] (const SingleTrackDocument::StatePtr& state) -> Status
    {
        if (!state || state->info.numChannels <= 0 || state->info.numSamples <= 0)
            return { Error::invalidPacket, "producer_state_dimensions" };
        if (state->envelopePoints.size() > config.queue.retainedBytes / sizeof(SingleTrackDocument::EnvelopePoint)
            || !add(sizeof(SingleTrackDocument::State))
            || !add(state->envelopePoints.size() * sizeof(SingleTrackDocument::EnvelopePoint)))
            return capacityFailure();
        const auto addString = [&] (const juce::String& value) { return add(value.getNumBytesAsUTF8() + 1); };
        if (!addString(state->info.file.getFullPathName()) || !addString(state->info.formatName)
            || !addString(state->readerSourceFile.getFullPathName()) || !addString(state->ambisonicBinauralStatus))
            return capacityFailure();
        for (const auto* strings : { &state->info.metadata.getAllKeys(), &state->info.metadata.getAllValues(),
                                    &state->safeTimelineMetadataKeys, &state->transformedMetadataKeys })
            for (const auto& value : *strings)
                if (!addString(value)) return capacityFailure();
        std::shared_ptr<const void> backing;
        if (state->audioBuffer && state->audioBuffer->getNumSamples() > 0) backing = state->audioBuffer;
        else if (state->sourceFileWitness) backing = state->sourceFileWitness;
        else if (state->memoryMappedReader) backing = state->memoryMappedReader;
        else backing = state->streamingReader;
        if (!backing) return { Error::invalidPacket, "producer_backing_identity" };
        if (backings.insert(backing.get()).second)
        {
            const auto channels = static_cast<uint64_t>(state->info.numChannels);
            const auto frames = static_cast<uint64_t>(state->info.numSamples);
            if (frames > std::numeric_limits<size_t>::max() / sizeof(float) / channels)
                return capacityFailure();
            const auto pcmBytes = static_cast<size_t>(frames * channels * sizeof(float));
            if (pcmBytes > config.graph.materialisedBytes - logicalBytes)
                return byteCapacityFailure("producer_audio_graph_budget", ByteCapacity::Resource::audioGraph,
                    static_cast<uint64_t>(logicalBytes) + pcmBytes, config.graph.materialisedBytes, true);
            logicalBytes += pcmBytes;
            if (config.graph.chunkSampleBytes > std::numeric_limits<size_t>::max() / 4)
                return { Error::invalidPacket, "producer_chunk_budget" };
            const bool memory = state->audioBuffer && state->audioBuffer->getNumSamples() > 0;
            const auto reservation = memory ? pcmBytes : std::min(pcmBytes, 4 * config.graph.chunkSampleBytes);
            if (!add(reservation) || !add(sizeof(AsyncRecoveryWriter::RetainedBacking))) return capacityFailure();
            retainedBackings.push_back({ std::move(backing), reservation });
        }
        return {};
    };
    auto status = include(snapshot.state);
    for (const auto& state : snapshot.undo) if (status) status = include(state);
    for (const auto& state : snapshot.redo) if (status) status = include(state);
    if (status)
    {
        result = bytes;
        if (sharedBackings) *sharedBackings = std::move(retainedBackings);
        if (logicalAudioBytes) *logicalAudioBytes = logicalBytes;
    }
    return status;
}

Status RecoveryMutationProducer::projectedRetained(const SingleTrackDocument::CommittedStateSnapshot& snapshot, size_t& output) const
{
    AsyncRecoveryWriter::Submission incoming;
    const auto status = estimate(snapshot, incoming.retainedBytes, &incoming.sharedBackings);
    if (!status) return status;
    if (writer) return writer->projectedRetainedBytes(incoming, output);
    output = incoming.retainedBytes; return {};
}

void RecoveryMutationProducer::rejectGap(Status problem)
{
    if (!checkpointRequired) failure = std::move(problem);
    checkpointRequired = true;
    if (capture) capture->stop();
    if (writer) writer->close();
}

void RecoveryMutationProducer::consume(DocumentRecoveryCapture::Capture captured)
{
    const auto& event = captured.event;
    if (retirementReason.isNotEmpty()) return;
    if (event.kind != media::IdentityEventKind::generationChanged)
    {
        if (event.kind == media::IdentityEventKind::saveCheckpoint || event.kind == media::IdentityEventKind::sourceRebound)
        {
            if (!event.before.document.sameVersion(observed.document)
                || !event.before.source.sameBinding(observed.source)
                || !event.after.document.sameDocument(observed.document))
            { rejectGap({ Error::staleHead, "producer_save_discontinuity" }); return; }
            if (event.after.savedGeneration != event.after.document.generation)
                replacementCheckpoint = event.after;
            observed = event.after;
        }
        retire(event.kind == media::IdentityEventKind::saveCheckpoint ? "saved"
            : event.kind == media::IdentityEventKind::sourceRebound ? "source_rebound"
            : event.kind == media::IdentityEventKind::cleared ? "cleared" : "document_replaced");
        return;
    }
    if (checkpointRequired) return;
    if (!event.before.document.sameVersion(observed.document)
        || !event.after.document.sameDocument(observed.document)
        || !event.after.source.sameBinding(observed.source))
    { rejectGap({ Error::staleHead, "producer_event_discontinuity" }); return; }
    observed = event.after;
    // The callback itself is a commit boundary, not a debounced timer: selection,
    // transport, preview changes and failed edits do not produce these events.
    try
    {
        if (!captured.document) { rejectGap({ Error::staleHead, "producer_capture_stale" }); return; }
        const auto submitted = enqueue(std::move(*captured.document));
        if (!submitted) rejectGap(submitted);
    }
    catch (...) { rejectGap({ Error::resourceOrEnvironmentFault, "producer_capture_exception" }); }
}

Status RecoveryMutationProducer::enqueue(SingleTrackDocument::CommittedStateSnapshot snapshot)
{
    size_t retained = 0, logicalAudioBytes = 0;
    std::vector<AsyncRecoveryWriter::RetainedBacking> backings;
    const auto admissible = estimate(snapshot, retained, &backings, &logicalAudioBytes);
    if (!admissible) return admissible;
    if (!writer)
    {
        owner = juce::Uuid();
        auto queue = config.queue;
        if (logicalAudioBytes > 256 * 1024 * 1024)
            queue.maximumAge = std::max(queue.maximumAge, config.largeSourceMaximumAge);
        writer = std::make_unique<AsyncRecoveryWriter>(config.root, owner, config.store, queue);
        auto graph = config.graph;
        graph.streamNewAudio = true;
        const auto deltas = config.store.records > 2 ? std::min<size_t>(32, config.store.records / 2 - 1) : 0;
        checkpointWriter = std::make_shared<RecoveryCheckpointWriter>(config.root, owner, graph, deltas);
    }
    return writer->submit({ retained,
        [snapshot = std::move(snapshot), checkpoints = checkpointWriter, prepareStorage = config.prepareStorage] (CommitPacket& packet)
        {
            if (prepareStorage)
                if (auto prepared = prepareStorage(); !prepared) return prepared;
            return checkpoints->prepare(snapshot, packet);
        },
        [checkpoints = checkpointWriter] (std::shared_ptr<const PublishedRecoveryCommit> receipt)
        { return checkpoints->published(receipt); }, std::move(backings),
        [graph = config.graph] (RecoveryStore& store, const SafeHead& head, OrphanCollectionReport& report,
                               std::function<bool()> cancel)
        { return RecoveryCheckpointCodec::compact(store, head, report, graph, std::move(cancel)); } });
}

RecoveryMutationProducer::Health RecoveryMutationProducer::health() const
{
    Health result;
    result.owner = owner;
    result.observedIdentity = observed.document;
    result.failure = failure;
    result.checkpointRequired = checkpointRequired;
    result.retirementReason = retirementReason;
    result.replacementCheckpoint = replacementCheckpoint;
    if (capture && !capture->latestFailure())
    {
        if (result.failure) result.failure = capture->latestFailure();
        result.checkpointRequired = true;
    }
    if (writer)
    {
        result.writer = writer->progress();
        if (result.failure) result.failure = result.writer.failure;
        if (result.failure) result.failure = result.writer.cancellationFailure;
        result.checkpointRequired = result.checkpointRequired || result.writer.checkpointRequired;
    }
    if (retirementReason.isNotEmpty()) result.phase = Phase::retired;
    else if (!result.failure || result.checkpointRequired) result.phase = Phase::degraded;
    else if (!writer) result.phase = Phase::idle;
    else if (result.writer.cancellation == AsyncRecoveryWriter::Cancellation::completed) result.phase = Phase::retired;
    else if (result.writer.protectedIdentity.sameVersion(observed.document)
        && result.writer.pendingPackets == 0) result.phase = Phase::protectedState;
    else result.phase = Phase::pending;
    return result;
}

Status RecoveryMutationProducer::retry(bool collectOrphans)
{
    if (retirementReason.isNotEmpty()) return { Error::resourceOrEnvironmentFault, "producer_retired" };
    if (health().checkpointRequired) return { Error::staleHead, "checkpoint_required" };
    return writer ? writer->retry(collectOrphans) : Status {};
}

void RecoveryMutationProducer::retire(const char* reason)
{
    if (retirementReason.isEmpty()) retirementReason = reason;
    if (capture) capture->stop();
    if (writer) writer->close();
}
void RecoveryMutationProducer::close()
{
    if (capture && !capture->latestFailure()) rejectGap(capture->latestFailure());
    capture.reset();
    retire("closed");
}
bool RecoveryMutationProducer::drain(std::chrono::milliseconds timeout)
{
    const bool drained = !writer || writer->drain(timeout);
    return drained && !health().checkpointRequired;
}
}
