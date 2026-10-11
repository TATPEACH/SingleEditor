#include "RecoveryAudioGraph.h"
#include "ProgressiveRecoveryAudio.h"
#include <bit>
#include <cmath>
#include <map>
#include <limits>

namespace singleeditor::recovery
{
namespace
{
// One independent decoder per unique ordinary source, owned by the prepared
// packet through append. Source leases remain held across both read passes.
// Access is serialized on the same recovery worker, never the playback reader.
struct CapturedChunkSource final
{
    SingleTrackDocument::StatePtr state;
    std::shared_ptr<media::SourceFileLease> lease;
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::AudioBuffer<float> scratch;

    Status read(int start, int count, AudioBlock& output,
        const RecoveryAudioGraph::Cancel& cancelled, bool recheckLease)
    {
        if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
        if (recheckLease && lease && lease->recheck().failed())
            return { Error::sourceConflict, "audio_source_changed_during_capture" };
        const auto& info = state->info;
        if (reader && !reader->read(&scratch, 0, count, start, true, info.numChannels > 1))
            return { Error::resourceOrEnvironmentFault, "audio_source_read_failed" };
        const auto& audio = reader ? scratch : *state->audioBuffer;
        const auto offset = reader ? 0 : start;
        AudioBlock block { info.sampleRate, info.numChannels, count, {} };
        block.samples.setSize(static_cast<size_t>(count) * static_cast<size_t>(info.numChannels) * 4);
        auto* bytes = static_cast<unsigned char*>(block.samples.getData());
        for (int frame = 0; frame < count; ++frame)
            for (int channel = 0; channel < info.numChannels; ++channel)
            {
                const auto bits = std::bit_cast<uint32_t>(audio.getSample(channel, offset + frame));
                for (int byte = 0; byte < 4; ++byte) *bytes++ = static_cast<unsigned char>(bits >> (8 * byte));
            }
        if (recheckLease && lease && lease->recheck().failed())
            return { Error::sourceConflict, "audio_source_changed_during_capture" };
        output = std::move(block);
        return {};
    }
};

media::SourceFileWitness::Ptr backingWitness(const SingleTrackDocument::StatePtr& state)
{
    if (!state) return {};
    if (state->sourceFileWitness) return state->sourceFileWitness;
    if (const auto* decoded = dynamic_cast<const media::IndependentAudioBacking*>(state->streamingReader.get()))
        return decoded->validatedBacking();
    return {};
}
juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> fields)
{
    auto result = std::make_unique<juce::DynamicObject>();
    for (const auto& field : fields) result->setProperty(field.first, field.second);
    return juce::var(result.release());
}
bool keys(const juce::var& value, std::initializer_list<const char*> names)
{
    const auto* valueObject = value.getDynamicObject();
    if (valueObject == nullptr || valueObject->getProperties().size() != static_cast<int>(names.size())) return false;
    for (const auto* name : names) if (!value.hasProperty(name)) return false;
    return true;
}
bool integer(const juce::var& value, int64_t minimum, int64_t maximum)
{
    return (value.isInt() || value.isInt64()) && static_cast<int64_t>(value) >= minimum
        && static_cast<int64_t>(value) <= maximum;
}
bool validLimits(const AudioGraphLimits& limits)
{
    return limits.states > 0 && limits.states <= 32 && limits.chunks > 0 && limits.chunks <= maximumCommitAssets
        && limits.chunkSampleBytes >= 256 && limits.assetBytes >= 2048 && limits.assetBytes <= 8 * 1024 * 1024
        && limits.chunkSampleBytes <= limits.assetBytes - 1024
        && limits.materialisedBytes > 0 && limits.materialisedBytes <= maximumRecoveryAudioBytes
        && limits.eagerBytes > 0 && limits.eagerBytes <= 256 * 1024 * 1024
        && limits.jsonBytes > 0 && limits.jsonBytes <= 256 * 1024;
}
bool audioDimensions(double rate, int channels, int64_t frames, size_t maximum, size_t& size)
{
    if (!std::isfinite(rate) || rate < 1 || rate > 768000 || channels < 1 || channels > 32
        || frames < 1 || frames > std::numeric_limits<int>::max()
        || static_cast<uint64_t>(frames) > maximum / (4u * static_cast<size_t>(channels))) return false;
    size = static_cast<size_t>(frames) * static_cast<size_t>(channels) * 4;
    return true;
}
bool boundedJson(const juce::String& json, size_t maximum)
{
    if (json.getNumBytesAsUTF8() > maximum) return false;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (const auto ch : json)
    {
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        }
        else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 8) return false; }
        else if (ch == '}' || ch == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && !quoted;
}
bool assetToken(const juce::var& value)
{
    if (!value.isString() || value.toString().length() != 64) return false;
    for (const auto c : value.toString()) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
} // namespace

std::shared_ptr<const void> AudioGraphReuseCache::backingOf(const SingleTrackDocument::StatePtr& state)
{
    using Backing = SingleTrackDocument::State::AudioBackingStore;
    if (!state) return {};
    if (state->audioBackingStore == Backing::recoveryStream)
        return ProgressiveRecoveryAudio::getDurableReference(state->streamingReader) ? state->streamingReader : nullptr;
    if (!state->playbackAvailability.isRandomAccess()) return {};
    if (state->audioBackingStore == Backing::inMemoryBuffer)
        return state->audioBuffer && state->audioBuffer->getNumChannels() == state->info.numChannels
            && state->audioBuffer->getNumSamples() == state->info.numSamples ? state->audioBuffer : nullptr;
    if ((state->audioBackingStore == Backing::memoryMappedFile && state->memoryMappedReader)
        || (state->audioBackingStore == Backing::streamingReader && state->streamingReader))
        if (const auto witness = backingWitness(state); witness && witness->file == state->readerSourceFile
            && (!state->audioBuffer || state->audioBuffer->getNumSamples() == 0)) return witness;
    return {};
}

const AudioGraphDescriptor::Sequence* AudioGraphReuseCache::lookup(const SingleTrackDocument::StatePtr& state) const
{
    const auto backing = backingOf(state);
    if (!backing) return nullptr;
    for (const auto& entry : committed)
    {
        const auto held = entry.backing.lock();
        if (held && held == backing && !held.owner_before(backing) && !backing.owner_before(held)
            && entry.kind == state->audioBackingStore && entry.audioRevision == state->audioRevision
            && entry.sequence.sampleRate == state->info.sampleRate && entry.sequence.channels == state->info.numChannels
            && entry.sequence.frames == state->info.numSamples) return &entry.sequence;
    }
    return nullptr;
}

Status AudioGraphReuseCache::stage(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
                                  const CapturedAudioGraph& audio, const juce::String& state, AudioGraphLimits limits)
{
    AudioGraphDescriptor graph;
    const auto status = RecoveryAudioGraph::describe(audio.json, graph, limits);
    if (!status) return status;
    std::vector<SingleTrackDocument::StatePtr> states { snapshot.state };
    states.insert(states.end(), snapshot.undo.begin(), snapshot.undo.end());
    states.insert(states.end(), snapshot.redo.begin(), snapshot.redo.end());
    if (graph.states.size() != states.size()) return { Error::invalidPacket, "reuse_stage_bindings" };
    std::vector<Entry> next;
    for (size_t i = 0; i < states.size(); ++i)
    {
        auto backing = backingOf(states[i]);
        if (!backing) return { Error::invalidPacket, "reuse_stage_backing" };
        next.push_back({ backing, states[i]->audioBackingStore, states[i]->audioRevision, graph.sequences[graph.states[i]] });
    }
    pending = std::move(next);
    pendingIdentity = snapshot.identity.document;
    pendingState = state;
    work = audio.work;
    return {};
}

Status AudioGraphReuseCache::acknowledge(const std::shared_ptr<const PublishedRecoveryCommit>& receipt)
{
    if (!receipt || root == juce::File() || owner.isNull() || receipt->rootDirectory() != root
        || receipt->ownerId() != owner || pending.empty()
        || !receipt->documentIdentity().sameVersion(pendingIdentity) || receipt->stateJson() != pendingState
        || receipt->head().sequence <= committedHead.sequence)
        return { Error::staleHead, "reuse_publication_mismatch" };
    std::set<juce::String> dependencies;
    for (const auto& entry : pending)
        for (const auto& chunk : entry.sequence.chunks) dependencies.insert(chunk.id);
    if (dependencies != receipt->assetIds()) return { Error::staleHead, "reuse_publication_assets" };
    committed = std::move(pending);
    committedHead = receipt->head();
    discardPending();
    return {};
}

Status RecoveryAudioGraph::capture(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
                                  const std::set<juce::String>& known, CapturedAudioGraph& output,
                                  AudioGraphLimits limits, Cancel cancelled, const AudioGraphReuseCache* reuse)
{
    if (!validLimits(limits) || !snapshot.identity.document.isValid() || !snapshot.state
        || snapshot.state->changeGeneration != snapshot.identity.document.generation
        || snapshot.undo.size() > limits.states - 1
        || snapshot.redo.size() > limits.states - 1 - snapshot.undo.size())
        return { Error::invalidPacket, "audio_graph_capture_contract" };
    try
    {
        std::vector<SingleTrackDocument::StatePtr> states { snapshot.state };
        states.insert(states.end(), snapshot.undo.begin(), snapshot.undo.end());
        states.insert(states.end(), snapshot.redo.begin(), snapshot.redo.end());
        size_t declaredBytes = 0;
        std::set<const void*> declaredBackings;
        for (const auto& state : states)
        {
            const auto backing = AudioGraphReuseCache::backingOf(state);
            if (!backing) return { Error::invalidPacket, "audio_backing_not_frozen" };
            if (!declaredBackings.insert(backing.get()).second) continue;
            size_t size = 0;
            if (!audioDimensions(state->info.sampleRate, state->info.numChannels, state->info.numSamples,
                    limits.materialisedBytes, size) || size > limits.materialisedBytes - declaredBytes)
                return { Error::budgetExceeded, "audio_graph_materialisation_budget" };
            declaredBytes += size;
        }
        if (!limits.streamNewAudio && declaredBytes > limits.eagerBytes)
            return { Error::budgetExceeded, "audio_graph_eager_budget" };
        const auto captureChunkBytes = declaredBytes <= 256 * 1024 * 1024
            ? std::min<size_t>(limits.chunkSampleBytes, 2 * 1024 * 1024) : limits.chunkSampleBytes;
        CapturedAudioGraph candidate;
        juce::Array<juce::var> bindings, sequences;
        std::map<const void*, int> backingIndexes;
        std::set<juce::String> emitted;
        size_t aggregateBytes = 0, chunkCount = 0;
        for (const auto& state : states)
        {
            if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
            if (const auto* cached = reuse ? reuse->lookup(state) : nullptr)
            {
                const auto backing = AudioGraphReuseCache::backingOf(state);
                if (const auto found = backingIndexes.find(backing.get()); found != backingIndexes.end())
                { bindings.add(found->second); continue; }
                size_t size = 0;
                if (!audioDimensions(cached->sampleRate, cached->channels, cached->frames, limits.materialisedBytes, size)
                    || size > limits.materialisedBytes - aggregateBytes || cached->chunks.size() > limits.chunks - chunkCount)
                    return { Error::budgetExceeded, "reuse_capture_budget" };
                // Cached PCM cannot hide a changed ordinary source still backing
                // the live Document. Metadata/read sharing only; no decoder/read.
                std::shared_ptr<media::SourceFileLease> lease;
                const auto witness = backingWitness(state);
                if (witness && (state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::memoryMappedFile
                    || state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::streamingReader))
                {
                    lease = witness->retainedInput();
                    if (!lease)
                    {
                        std::unique_ptr<media::SourceFileLease> openedLease;
                        const auto opened = media::SourceFileLease::open(witness->file, openedLease);
                        if (opened.failed()) return { Error::resourceOrEnvironmentFault, opened.getErrorMessage() };
                        if (!openedLease->witness()->sameFileVersion(*witness))
                            return { Error::sourceConflict, "audio_source_identity_changed" };
                        lease = std::move(openedLease);
                    }
                    else if (lease->recheck().failed()) return { Error::sourceConflict, "audio_retained_input_changed" };
                }
                juce::Array<juce::var> ids;
                for (const auto& chunk : cached->chunks)
                {
                    if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
                    if (static_cast<uint64_t>(chunk.frames) > limits.chunkSampleBytes / (4u * static_cast<size_t>(cached->channels)))
                        return { Error::budgetExceeded, "reuse_chunk_budget" };
                    ids.add(object({ { "id", chunk.id }, { "frames", chunk.frames } }));
                    if (emitted.insert(chunk.id).second) candidate.existingAudio.push_back(chunk.id);
                }
                if (lease && lease->recheck().failed()) return { Error::sourceConflict, "audio_source_changed_during_capture" };
                aggregateBytes += size; chunkCount += cached->chunks.size();
                const auto index = sequences.size();
                backingIndexes.emplace(backing.get(), index);
                bindings.add(index);
                sequences.add(object({ { "rate", cached->sampleRate }, { "channels", cached->channels },
                    { "frames", cached->frames }, { "chunks", juce::var(ids) } }));
                ++candidate.work.reusedSequences;
                continue;
            }
            if (state && state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::recoveryStream)
            {
                const auto reference = ProgressiveRecoveryAudio::getDurableReference(state->streamingReader);
                if (!reference) return { Error::invalidPacket, "recovery_backing_no_durable_source" };
                const auto& sequence = reference->sequence;
                const auto& info = state->info;
                size_t size = 0;
                if (sequence.sampleRate != info.sampleRate || sequence.channels != info.numChannels || sequence.frames != info.numSamples
                    || !audioDimensions(sequence.sampleRate, sequence.channels, sequence.frames, limits.materialisedBytes, size))
                    return { Error::invalidPacket, "recovery_backing_dimensions" };
                if (const auto found = backingIndexes.find(state->streamingReader.get()); found != backingIndexes.end())
                {
                    bindings.add(found->second);
                    continue;
                }
                if (size > limits.materialisedBytes - aggregateBytes || sequence.chunks.size() > limits.chunks - chunkCount)
                    return { Error::budgetExceeded, "recovery_backing_capture_budget" };
                aggregateBytes += size; chunkCount += sequence.chunks.size();
                juce::Array<juce::var> ids;
                // Recover the entire immutable dependency graph from durable
                // CAS, including history not yet materialized for playback.
                // Never read unprepared mapping bytes or a mutable source path.
                for (const auto& chunk : sequence.chunks)
                {
                    if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
                    if (static_cast<uint64_t>(chunk.frames) > limits.chunkSampleBytes / (4u * static_cast<size_t>(sequence.channels)))
                        return { Error::budgetExceeded, "recovery_backing_chunk_budget" };
                    AudioBlock block;
                    const auto loaded = reference->store.readAudio(chunk.id, block);
                    if (!loaded) return loaded;
                    ++candidate.work.casBlocksRead;
                    candidate.work.sampleBytesReadOrCopied += block.samples.getSize();
                    if (block.sampleRate != sequence.sampleRate || block.channels != sequence.channels || block.frames != chunk.frames
                        || block.samples.getSize() > limits.assetBytes)
                        return { Error::missingOrCorruptAsset, "recovery_backing_chunk_dimensions" };
                    ids.add(object({ { "id", chunk.id }, { "frames", chunk.frames } }));
                    if (emitted.insert(chunk.id).second)
                    {
                        if (known.contains(chunk.id)) candidate.existingAudio.push_back(chunk.id);
                        else if (limits.streamNewAudio)
                            candidate.streamedAudio.push_back({ chunk.id, sequence.sampleRate, sequence.channels, chunk.frames,
                                [reference, id = chunk.id, cancelled](AudioBlock& output)
                                {
                                    if (cancelled && cancelled()) return Status { Error::cancelled, "audio_graph_cancelled" };
                                    return reference->store.readAudio(id, output);
                                } });
                        else candidate.newAudio.push_back(std::move(block));
                    }
                }
                const auto index = sequences.size();
                backingIndexes.emplace(state->streamingReader.get(), index);
                bindings.add(index);
                sequences.add(object({ { "rate", sequence.sampleRate }, { "channels", sequence.channels },
                    { "frames", sequence.frames }, { "chunks", juce::var(ids) } }));
                continue;
            }
            if (!state || !state->playbackAvailability.isRandomAccess())
                return { Error::invalidPacket, "audio_backing_not_frozen" };
            const auto memory = state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::inMemoryBuffer;
            const auto ordinary = state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::memoryMappedFile
                || state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::streamingReader;
            const auto sourceWitness = backingWitness(state);
            if ((!memory && !ordinary) || (memory && !state->audioBuffer)
                || (ordinary && !sourceWitness)) return { Error::invalidPacket, "audio_backing_not_frozen" };
            if (ordinary && ((state->audioBuffer && state->audioBuffer->getNumSamples() > 0)
                || (state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::memoryMappedFile && !state->memoryMappedReader)
                || (state->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::streamingReader && !state->streamingReader)))
                return { Error::invalidPacket, "audio_backing_not_frozen" };
            const auto& info = state->info;
            size_t size = 0;
            if ((memory && (state->audioBuffer->getNumChannels() != info.numChannels || state->audioBuffer->getNumSamples() != info.numSamples))
                || !audioDimensions(info.sampleRate, info.numChannels, info.numSamples, limits.materialisedBytes, size))
                return { Error::invalidPacket, "audio_graph_dimensions" };
            const void* backingKey = memory ? static_cast<const void*>(state->audioBuffer.get()) : sourceWitness.get();
            if (const auto found = backingIndexes.find(backingKey); found != backingIndexes.end())
            {
                const auto& previous = sequences[found->second];
                if (static_cast<double>(previous["rate"]) != info.sampleRate || static_cast<int>(previous["channels"]) != info.numChannels
                    || static_cast<int64_t>(previous["frames"]) != info.numSamples)
                    return { Error::invalidPacket, "audio_backing_format_alias" };
                bindings.add(found->second);
                continue;
            }
            if (size > limits.materialisedBytes - aggregateBytes)
                return { Error::budgetExceeded, "audio_graph_materialisation_budget" };
            aggregateBytes += size;
            const auto framesPerChunk = static_cast<int>(captureChunkBytes / (4u * static_cast<size_t>(info.numChannels)));
            const auto requiredChunks = (static_cast<size_t>(info.numSamples) + static_cast<size_t>(framesPerChunk) - 1)
                                      / static_cast<size_t>(framesPerChunk);
            if (requiredChunks > limits.chunks - chunkCount)
                return { Error::budgetExceeded, "audio_graph_chunk_budget" };
            chunkCount += requiredChunks;
            auto input = std::make_shared<CapturedChunkSource>();
            input->state = state;
            auto& sourceLease = input->lease;
            auto& independent = input->reader;
            auto& scratch = input->scratch;
            if (ordinary)
            {
                const auto& witness = *sourceWitness;
                if (witness.file != state->readerSourceFile)
                    return { Error::sourceConflict, "audio_source_location_changed" };
                sourceLease = witness.retainedInput();
                if (sourceLease)
                {
                    if (sourceLease->recheck().failed()) return { Error::sourceConflict, "audio_retained_input_changed" };
                    independent = SingleTrackDocument::createReaderForRetainedSource(sourceWitness);
                }
                else
                {
                    std::unique_ptr<media::SourceFileLease> openedLease;
                    const auto leased = media::SourceFileLease::open(witness.file, openedLease);
                    if (leased.failed()) return { Error::resourceOrEnvironmentFault, leased.getErrorMessage() };
                    if (!openedLease->witness()->sameFileVersion(witness)) return { Error::sourceConflict, "audio_source_identity_changed" };
                    sourceLease = std::move(openedLease);
                    independent = SingleTrackDocument::createReaderForFile(witness.file);
                }
                ++candidate.work.sourceDecodersOpened;
                if (!independent || independent->sampleRate != info.sampleRate || independent->numChannels != static_cast<unsigned int>(info.numChannels)
                    || independent->lengthInSamples != info.numSamples)
                    return { Error::sourceConflict, "audio_source_reader_changed" };
                scratch.setSize(info.numChannels, std::min(framesPerChunk, static_cast<int>(info.numSamples)));
            }
            juce::Array<juce::var> ids;
            for (int start = 0; start < static_cast<int>(info.numSamples);)
            {
                if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
                const auto count = std::min(framesPerChunk, static_cast<int>(info.numSamples) - start);
                AudioBlock block;
                const auto read = input->read(start, count, block, cancelled, false);
                if (!read) return read;
                candidate.work.sampleBytesReadOrCopied += block.samples.getSize();
                juce::String id;
                const auto status = RecoveryStore::identifyAudio(block, limits.assetBytes, id);
                if (!status) return status;
                ids.add(object({ { "id", id }, { "frames", count } }));
                if (emitted.insert(id).second)
                {
                    if (known.contains(id)) candidate.existingAudio.push_back(id);
                    else if (limits.streamNewAudio)
                        candidate.streamedAudio.push_back({ id, info.sampleRate, info.numChannels, count,
                            [input, start, count, cancelled](AudioBlock& output)
                            { return input->read(start, count, output, cancelled, true); } });
                    else candidate.newAudio.push_back(std::move(block));
                }
                start += count;
            }
            if (sourceLease && sourceLease->recheck().failed()) return { Error::sourceConflict, "audio_source_changed_during_capture" };
            const auto index = sequences.size();
            backingIndexes.emplace(backingKey, index);
            bindings.add(index);
            sequences.add(object({ { "rate", info.sampleRate }, { "channels", info.numChannels },
                                   { "frames", info.numSamples }, { "chunks", juce::var(ids) } }));
        }
        candidate.json = juce::JSON::toString(object({ { "kind", "audio-graph" }, { "schema", 2 },
            { "states", juce::var(bindings) }, { "sequences", juce::var(sequences) } }), true);
        if (!boundedJson(candidate.json, limits.jsonBytes)) return { Error::budgetExceeded, "audio_graph_json_budget" };
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "audio_graph_capture_exception" }; }
}

Status RecoveryAudioGraph::describe(const juce::String& json, AudioGraphDescriptor& output, AudioGraphLimits limits)
{
    if (!validLimits(limits) || !boundedJson(json, limits.jsonBytes))
        return { Error::budgetExceeded, "audio_graph_input_budget" };
    try
    {
        const auto graph = juce::JSON::parse(json);
        if (!keys(graph, { "kind", "schema", "states", "sequences" })
            || graph["kind"] != juce::var("audio-graph") || !integer(graph["schema"], 2, 2)
            || !graph["states"].isArray() || !graph["sequences"].isArray()
            || juce::JSON::toString(graph, true) != json)
            return { Error::unsupportedSchema, "audio_graph_schema" };
        const auto& bindings = *graph["states"].getArray();
        const auto& sequences = *graph["sequences"].getArray();
        if (bindings.isEmpty() || sequences.isEmpty() || static_cast<size_t>(bindings.size()) > limits.states
            || sequences.size() > bindings.size()) return { Error::invalidPacket, "audio_graph_state_count" };
        AudioGraphDescriptor candidate;
        size_t aggregateBytes = 0, chunkCount = 0;
        std::set<int> referenced;
        for (const auto& binding : bindings)
        {
            if (!integer(binding, 0, sequences.size() - 1)) return { Error::invalidPacket, "audio_graph_state_reference" };
            referenced.insert(static_cast<int>(binding));
            candidate.states.push_back(static_cast<size_t>(static_cast<int>(binding)));
        }
        if (referenced.size() != static_cast<size_t>(sequences.size())) return { Error::invalidPacket, "audio_graph_orphan" };
        // Validate the entire allocation plan before allocating any sample buffer.
        for (const auto& sequence : sequences)
        {
            size_t size = 0;
            if (!keys(sequence, { "rate", "channels", "frames", "chunks" })
                || !(sequence["rate"].isDouble() || sequence["rate"].isInt() || sequence["rate"].isInt64())
                || !integer(sequence["channels"], 1, 32) || !integer(sequence["frames"], 1, std::numeric_limits<int>::max())
                || !sequence["chunks"].isArray()
                || !audioDimensions(static_cast<double>(sequence["rate"]), static_cast<int>(sequence["channels"]),
                                    static_cast<int64_t>(sequence["frames"]), limits.materialisedBytes, size))
                return { Error::invalidPacket, "audio_graph_sequence" };
            const auto& ids = *sequence["chunks"].getArray();
            if (size > limits.materialisedBytes - aggregateBytes || ids.isEmpty()
                || static_cast<size_t>(ids.size()) > limits.chunks - chunkCount)
                return { Error::budgetExceeded, "audio_graph_allocation_budget" };
            aggregateBytes += size;
            chunkCount += static_cast<size_t>(ids.size());
            AudioGraphDescriptor::Sequence checked;
            checked.sampleRate = static_cast<double>(sequence["rate"]);
            checked.channels = static_cast<int>(sequence["channels"]);
            checked.frames = static_cast<int64_t>(sequence["frames"]);
            int64_t start = 0;
            for (const auto& chunk : ids)
            {
                if (!keys(chunk, { "id", "frames" }) || !assetToken(chunk["id"])
                    || !integer(chunk["frames"], 1, checked.frames - start))
                    return { Error::invalidPacket, "audio_graph_chunk_reference" };
                const auto frames = static_cast<int64_t>(chunk["frames"]);
                if (static_cast<uint64_t>(frames) > limits.chunkSampleBytes / (4u * static_cast<size_t>(checked.channels)))
                    return { Error::budgetExceeded, "audio_graph_chunk_bytes" };
                checked.chunks.push_back({ chunk["id"].toString(), start, frames });
                start += frames;
            }
            if (start != checked.frames) return { Error::invalidPacket, "audio_graph_chunk_total" };
            candidate.sequences.push_back(std::move(checked));
        }
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "audio_graph_describe_exception" }; }
}

Status RecoveryAudioGraph::materialise(const juce::String& json, const RecoveryStore& store,
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>>& output, AudioGraphLimits limits, Cancel cancelled)
{
    AudioGraphDescriptor graph;
    const auto described = describe(json, graph, limits);
    if (!described) return described;
    size_t eagerBytes = 0;
    for (const auto& sequence : graph.sequences)
    {
        const auto size = static_cast<size_t>(sequence.frames) * static_cast<size_t>(sequence.channels) * 4;
        if (size > limits.eagerBytes - eagerBytes) return { Error::budgetExceeded, "audio_graph_eager_budget" };
        eagerBytes += size;
    }
    try
    {
        std::vector<std::shared_ptr<juce::AudioBuffer<float>>> buffers, candidate;
        for (const auto& sequence : graph.sequences)
        {
            if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
            const auto channels = sequence.channels, frames = static_cast<int>(sequence.frames);
            auto buffer = std::make_shared<juce::AudioBuffer<float>>(channels, frames);
            int position = 0;
            for (const auto& chunk : sequence.chunks)
            {
                if (cancelled && cancelled()) return { Error::cancelled, "audio_graph_cancelled" };
                AudioBlock block;
                const auto status = store.readAudio(chunk.id, block);
                if (!status) return status;
                if (block.sampleRate != sequence.sampleRate || block.channels != channels
                    || block.frames != chunk.frames || block.samples.getSize() > limits.assetBytes)
                    return { Error::missingOrCorruptAsset, "audio_graph_chunk_dimensions" };
                const auto* bytes = static_cast<const char*>(block.samples.getData());
                for (int frame = 0; frame < static_cast<int>(block.frames); ++frame)
                    for (int channel = 0; channel < channels; ++channel, bytes += 4)
                        buffer->setSample(channel, position + frame,
                                          std::bit_cast<float>(juce::ByteOrder::littleEndianInt(bytes)));
                position += static_cast<int>(block.frames);
            }
            if (position != frames) return { Error::missingOrCorruptAsset, "audio_graph_chunk_total" };
            buffers.push_back(std::move(buffer));
        }
        for (const auto binding : graph.states) candidate.push_back(buffers[binding]);
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "audio_graph_materialise_exception" }; }
}
} // namespace singleeditor::recovery
