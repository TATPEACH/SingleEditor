#include "RecoveryCheckpointCodec.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include <bit>
#include <cmath>
#include <limits>
#include <map>

namespace singleeditor::recovery
{
namespace
{
using State = SingleTrackDocument::State;
constexpr int64_t maxRevision = std::numeric_limits<int64_t>::max() - 1;
constexpr size_t maxMetadataEntries = 512, maxEnvelopePoints = 4096;
constexpr const char* checkpointPlan = "{\"kind\":\"document-checkpoint\",\"schema\":1}";
constexpr const char* checkpointSourcePlan = "{\"kind\":\"document-checkpoint\",\"schema\":2}";
constexpr const char* mutationPlan = "{\"kind\":\"document-mutation\",\"schema\":1}";
juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> fields)
{
    auto result = std::make_unique<juce::DynamicObject>();
    for (const auto& field : fields) result->setProperty(field.first, field.second);
    return juce::var(result.release());
}
bool keys(const juce::var& value, std::initializer_list<const char*> names)
{
    const auto* obj = value.getDynamicObject();
    if (!obj || obj->getProperties().size() != static_cast<int>(names.size())) return false;
    for (const auto* name : names) if (!value.hasProperty(name)) return false;
    return true;
}
bool integer(const juce::var& value, int64_t min, int64_t max)
{
    return (value.isInt() || value.isInt64()) && static_cast<int64_t>(value) >= min && static_cast<int64_t>(value) <= max;
}
bool number(const juce::var& value)
{
    return (value.isDouble() || value.isInt() || value.isInt64()) && std::isfinite(static_cast<double>(value));
}
bool string(const juce::var& value, size_t maximum = 16384)
{
    return value.isString() && value.toString().getNumBytesAsUTF8() <= maximum;
}
bool hex(const juce::var& value, int digits)
{
    if (!value.isString() || value.toString().length() != digits) return false;
    for (const auto c : value.toString()) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool uuid(const juce::var& value)
{
    return string(value, 32) && value.toString().length() == 32 && !juce::Uuid(value.toString()).isNull()
        && juce::Uuid(value.toString()).toString() == value.toString();
}
bool path(const juce::var& value)
{
    return string(value, 32768) && juce::File::isAbsolutePath(value.toString());
}
bool boundedJson(const juce::String& text, size_t maximum)
{
    if (maximum == 0 || maximum > 256 * 1024 || text.getNumBytesAsUTF8() > maximum) return false;
    bool quoted = false, escaped = false;
    int depth = 0;
    for (const auto ch : text)
    {
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        }
        else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 12) return false; }
        else if (ch == '}' || ch == ']') { if (--depth < 0) return false; }
    }
    return !quoted && depth == 0;
}
juce::var strings(const juce::StringArray& values)
{
    juce::Array<juce::var> result;
    for (const auto& value : values) result.add(value);
    return juce::var(result);
}
bool readStrings(const juce::var& value, juce::StringArray& result)
{
    const auto* entries = value.getArray();
    if (!entries || entries->size() > static_cast<int>(maxMetadataEntries)) return false;
    for (const auto& entry : *entries)
    {
        if (!string(entry, 256) || result.contains(entry.toString())) return false;
        result.add(entry.toString());
    }
    return true;
}
juce::var encodeLayout(const ChannelLayoutState& layout)
{
    juce::Array<juce::var> roles;
    for (const auto role : layout.selectedRoles) roles.add(static_cast<int>(role));
    return object({ { "schema", ChannelLayoutState::interpretationSchemaVersion }, { "channels", layout.sourceChannelCount },
        { "preset", layout.selectedPresetId }, { "name", layout.displayName }, { "order_text", layout.orderText },
        { "roles", juce::var(roles) }, { "detected", layout.detectedFromMetadata }, { "discrete", layout.isDiscreteFallback },
        { "ambisonic", layout.isAmbisonic }, { "ambi_order", layout.ambisonicOrder }, { "normalization", layout.ambisonicNormalization },
        { "origin", static_cast<int>(layout.interpretationOrigin) }, { "wave_mask", static_cast<int64_t>(layout.waveChannelMask) } });
}
bool decodeLayout(const juce::var& json, int channels, ChannelLayoutState& layout)
{
    if (!keys(json, { "schema", "channels", "preset", "name", "order_text", "roles", "detected", "discrete",
                      "ambisonic", "ambi_order", "normalization", "origin", "wave_mask" })
        || !integer(json["schema"], ChannelLayoutState::interpretationSchemaVersion, ChannelLayoutState::interpretationSchemaVersion)
        || !integer(json["channels"], channels, channels) || !string(json["preset"], 256) || !string(json["name"], 1024)
        || !string(json["order_text"], 1024) || !json["roles"].isArray() || json["roles"].size() != channels
        || !json["detected"].isBool() || !json["discrete"].isBool() || !json["ambisonic"].isBool()
        || !integer(json["ambi_order"], 0, 3) || !string(json["normalization"], 64)
        || !integer(json["origin"], 0, static_cast<int>(ChannelInterpretationOrigin::discreteFallback))
        || !integer(json["wave_mask"], 0, std::numeric_limits<uint32_t>::max())) return false;
    layout.sourceChannelCount = channels;
    layout.selectedPresetId = json["preset"].toString(); layout.displayName = json["name"].toString();
    layout.orderText = json["order_text"].toString();
    for (const auto& role : *json["roles"].getArray())
    {
        if (!integer(role, 0, static_cast<int>(SpeakerRole::ambiACN15))) return false;
        layout.selectedRoles.push_back(static_cast<SpeakerRole>(static_cast<int>(role)));
    }
    layout.detectedFromMetadata = static_cast<bool>(json["detected"]);
    layout.isDiscreteFallback = static_cast<bool>(json["discrete"]);
    layout.isAmbisonic = static_cast<bool>(json["ambisonic"]);
    layout.ambisonicOrder = static_cast<int>(json["ambi_order"]);
    layout.ambisonicNormalization = json["normalization"].toString();
    layout.interpretationOrigin = static_cast<ChannelInterpretationOrigin>(static_cast<int>(json["origin"]));
    layout.waveChannelMask = static_cast<uint32_t>(static_cast<int64_t>(json["wave_mask"]));
    // Choices are derived UI data; preserve selected interpretation verbatim.
    layout.candidates = ChannelLayoutCatalog::candidatesForChannelCount(channels);
    return !layout.isAmbisonic || AmbisonicMonitorDecoder::statusFor(layout) == AmbisonicMonitorDecodeStatus::ready;
}
juce::var encodeState(const State& state)
{
    const auto& info = state.info;
    juce::Array<juce::var> metadata, envelope;
    for (int index = 0; index < info.metadata.size(); ++index)
        metadata.add(juce::var(juce::Array<juce::var> { info.metadata.getAllKeys()[index], info.metadata.getAllValues()[index] }));
    for (const auto& point : state.envelopePoints)
        envelope.add(juce::var(juce::Array<juce::var> {
            juce::String::toHexString(static_cast<juce::int64>(std::bit_cast<uint64_t>(point.timeSeconds))).paddedLeft('0', 16),
            juce::String::toHexString(std::bit_cast<uint32_t>(point.gain)).paddedLeft('0', 8) }));
    return object({ { "file", info.file.getFullPathName() }, { "format", info.formatName }, { "metadata", juce::var(metadata) },
        { "rate", info.sampleRate }, { "channels", info.numChannels }, { "bits", info.bitsPerSample }, { "frames", info.numSamples },
        { "layout", encodeLayout(info.channelLayout) }, { "envelope_format", "f64-f32-hex" },
        { "envelope", juce::var(envelope) }, { "timeline_edits", state.hasTimelineEdits },
        { "safe_keys", strings(state.safeTimelineMetadataKeys) }, { "transformed_keys", strings(state.transformedMetadataKeys) },
        { "audio_revision", state.audioRevision }, { "envelope_revision", state.envelopeRevision }, { "generation", state.changeGeneration } });
}
bool decodeState(const juce::var& json, State& state)
{
    if (!keys(json, { "file", "format", "metadata", "rate", "channels", "bits", "frames", "layout", "envelope_format", "envelope", "timeline_edits",
                     "safe_keys", "transformed_keys", "audio_revision", "envelope_revision", "generation" })
        || !path(json["file"]) || !string(json["format"], 256) || !json["metadata"].isArray()
        || json["metadata"].size() > static_cast<int>(maxMetadataEntries) || !number(json["rate"])
        || !integer(json["channels"], 1, 32) || !integer(json["bits"], 1, 64)
        || !integer(json["frames"], 1, std::numeric_limits<int>::max()) || !json["timeline_edits"].isBool()
        || !integer(json["audio_revision"], 1, maxRevision) || !integer(json["envelope_revision"], 1, maxRevision)
        || !integer(json["generation"], 1, maxRevision) || json["envelope_format"] != juce::var("f64-f32-hex")
        || !json["envelope"].isArray() || json["envelope"].size() < 2 || json["envelope"].size() > static_cast<int>(maxEnvelopePoints)) return false;
    auto& info = state.info;
    info.file = juce::File(json["file"].toString()); info.formatName = json["format"].toString();
    info.sampleRate = static_cast<double>(json["rate"]); info.numChannels = static_cast<int>(json["channels"]);
    info.bitsPerSample = static_cast<int>(json["bits"]); info.numSamples = static_cast<int64_t>(json["frames"]);
    if (info.sampleRate < 1 || info.sampleRate > 768000 || !decodeLayout(json["layout"], info.numChannels, info.channelLayout)) return false;
    info.durationSeconds = static_cast<double>(info.numSamples) / info.sampleRate;
    for (const auto& pair : *json["metadata"].getArray())
    {
        if (!pair.isArray() || pair.size() != 2 || !string(pair[0], 256) || pair[0].toString().isEmpty()
            || !string(pair[1]) || info.metadata.containsKey(pair[0].toString())) return false;
        info.metadata.set(pair[0].toString(), pair[1].toString());
    }
    double previous = -1;
    for (const auto& pair : *json["envelope"].getArray())
    {
        if (!pair.isArray() || pair.size() != 2 || !hex(pair[0], 16) || !hex(pair[1], 8)) return false;
        const auto time = std::bit_cast<double>(static_cast<uint64_t>(pair[0].toString().getHexValue64()));
        const auto gain = std::bit_cast<float>(static_cast<uint32_t>(pair[1].toString().getHexValue32()));
        if (!std::isfinite(time) || !std::isfinite(gain) || time <= previous || time < 0 || time > info.durationSeconds || gain < 0 || gain > 4) return false;
        state.envelopePoints.push_back({ time, gain });
        previous = time;
    }
    if (state.envelopePoints.front().timeSeconds != 0 || state.envelopePoints.back().timeSeconds != info.durationSeconds
        || !readStrings(json["safe_keys"], state.safeTimelineMetadataKeys)
        || !readStrings(json["transformed_keys"], state.transformedMetadataKeys)) return false;
    state.hasTimelineEdits = static_cast<bool>(json["timeline_edits"]);
    state.audioRevision = static_cast<int64_t>(json["audio_revision"]);
    state.envelopeRevision = static_cast<int64_t>(json["envelope_revision"]);
    state.changeGeneration = static_cast<int64_t>(json["generation"]);
    state.audioBuffer.reset();
    return true;
}
bool stateInputBounded(const State& state)
{
    if (state.info.metadata.size() > static_cast<int>(maxMetadataEntries) || state.envelopePoints.size() > maxEnvelopePoints
        || state.safeTimelineMetadataKeys.size() > static_cast<int>(maxMetadataEntries)
        || state.transformedMetadataKeys.size() > static_cast<int>(maxMetadataEntries)
        || state.info.channelLayout.selectedRoles.size() > 32) return false;
    size_t bytes = 0;
    const auto addString = [&] (const juce::String& value, size_t maximum)
    {
        const auto count = value.getNumBytesAsUTF8();
        if (count > maximum || count > 256 * 1024 - bytes) return false;
        bytes += count;
        return true;
    };
    if (!addString(state.info.file.getFullPathName(), 32768) || !addString(state.info.formatName, 256)
        || !addString(state.info.channelLayout.selectedPresetId, 256) || !addString(state.info.channelLayout.displayName, 1024)
        || !addString(state.info.channelLayout.orderText, 1024) || !addString(state.info.channelLayout.ambisonicNormalization, 64)) return false;
    for (int index = 0; index < state.info.metadata.size(); ++index)
        if (!addString(state.info.metadata.getAllKeys()[index], 256) || !addString(state.info.metadata.getAllValues()[index], 16384)) return false;
    for (const auto& key : state.safeTimelineMetadataKeys) if (!addString(key, 256)) return false;
    for (const auto& key : state.transformedMetadataKeys) if (!addString(key, 256)) return false;
    const auto json = encodeState(state);
    if (juce::JSON::toString(json, true).getNumBytesAsUTF8() > 256 * 1024) return false;
    State checked;
    return decodeState(json, checked) && state.info.durationSeconds == checked.info.durationSeconds;
}
bool readIdentity(const juce::var& record, media::IdentitySnapshot& identity)
{
    const auto doc = record["document"], source = record["source"];
    if (!keys(doc, { "version", "lineage", "runtime_instance", "generation" }) || !integer(doc["version"], 1, 1)
        || !uuid(doc["lineage"]) || !uuid(doc["runtime_instance"]) || !integer(doc["generation"], 1, maxRevision)
        || !keys(source, { "binding", "path", "size", "modified", "fast_stamp_valid" })
        || !uuid(source["binding"]) || !path(source["path"]) || !integer(source["size"], -1, std::numeric_limits<int64_t>::max())
        || !integer(source["modified"], 0, std::numeric_limits<int64_t>::max()) || !source["fast_stamp_valid"].isBool()) return false;
    identity.document = { juce::Uuid(doc["lineage"].toString()), juce::Uuid(doc["runtime_instance"].toString()), static_cast<int64_t>(doc["generation"]) };
    identity.source = { juce::Uuid(source["binding"].toString()), { juce::File(source["path"].toString()) }, static_cast<int64_t>(source["size"]),
                        static_cast<int64_t>(source["modified"]), static_cast<bool>(source["fast_stamp_valid"]) };
    return true;
}
bool assetClosure(const AudioGraphDescriptor& audio, const juce::var& recorded)
{
    if (!recorded.isArray() || static_cast<size_t>(recorded.size()) > maximumCommitAssets) return false;
    std::set<juce::String> expected, actual;
    for (const auto& sequence : audio.sequences)
        for (const auto& chunk : sequence.chunks) expected.insert(chunk.id);
    for (const auto& id : *recorded.getArray()) if (!id.isString() || !actual.insert(id.toString()).second) return false;
    return actual == expected;
}
bool sameSource(const media::SourceIdentity& a, const media::SourceIdentity& b)
{
    return a.sameBinding(b) && a.location.file == b.location.file && a.sizeBytes == b.sizeBytes
        && a.modifiedMilliseconds == b.modifiedMilliseconds && a.fastStampValid == b.fastStampValid;
}
SingleTrackDocument::CommittedStateSnapshot metadataSnapshot(const PreparedRecoveryCheckpoint& prepared)
{
    SingleTrackDocument::CommittedStateSnapshot result;
    result.identity = prepared.getIdentity();
    const auto& states = prepared.getStates();
    if (states.size() != 1 + prepared.getUndoCount() + prepared.getRedoCount()) return result;
    result.state = states.front();
    result.undo.assign(states.begin() + 1, states.begin() + 1 + prepared.getUndoCount());
    result.redo.assign(states.begin() + 1 + prepared.getUndoCount(), states.end());
    return result;
}
juce::String audioJson(const AudioGraphDescriptor& graph)
{
    juce::Array<juce::var> states, sequences;
    for (const auto index : graph.states) states.add(static_cast<int>(index));
    for (const auto& sequence : graph.sequences)
    {
        juce::Array<juce::var> chunks;
        for (const auto& chunk : sequence.chunks) chunks.add(object({ { "id", chunk.id }, { "frames", chunk.frames } }));
        sequences.add(object({ { "rate", sequence.sampleRate }, { "channels", sequence.channels },
            { "frames", sequence.frames }, { "chunks", juce::var(chunks) } }));
    }
    return juce::JSON::toString(object({ { "kind", "audio-graph" }, { "schema", 2 },
        { "states", juce::var(states) }, { "sequences", juce::var(sequences) } }), true);
}
std::set<juce::String> dependencies(const AudioGraphDescriptor& graph)
{
    std::set<juce::String> result;
    for (const auto& sequence : graph.sequences)
        for (const auto& chunk : sequence.chunks) result.insert(chunk.id);
    return result;
}
// Local semantic validation before publication. This value is never exposed as
// a Store receipt or used as proof that a disk commit exists.
juce::var localRecord(const CommitPacket& packet, const AudioGraphDescriptor& graph)
{
    const auto& source = packet.identity.source;
    juce::Array<juce::var> assets;
    for (const auto& id : dependencies(graph)) assets.add(id);
    return object({ { "document", packet.identity.document.toJson() },
        { "source", object({ { "binding", source.binding.toString() }, { "path", source.location.file.getFullPathName() },
            { "size", source.sizeBytes }, { "modified", source.modifiedMilliseconds }, { "fast_stamp_valid", source.fastStampValid } }) },
        { "operation", packet.operationId }, { "plan_schema", packet.planSchema }, { "plan", packet.planJson },
        { "state", packet.stateJson }, { "assets", juce::var(assets) } });
}
} // namespace

Status RecoveryCheckpointCodec::encode(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
    const std::set<juce::String>& known, CommitPacket& output, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel,
    AudioGraphReuseCache* reuse)
{
    if (reuse) reuse->discardPending();
    if (!snapshot.state || snapshot.undo.size() > 10 || snapshot.redo.size() > 10 || snapshot.identity.asset.has_value()
        || snapshot.identity.savedGeneration < 0 || snapshot.identity.savedGeneration > maxRevision)
        return { Error::invalidPacket, "checkpoint_capture_contract" };
    try
    {
        std::vector<SingleTrackDocument::StatePtr> states { snapshot.state };
        states.insert(states.end(), snapshot.undo.begin(), snapshot.undo.end());
        states.insert(states.end(), snapshot.redo.begin(), snapshot.redo.end());
        juce::Array<juce::var> metadata;
        for (const auto& state : states)
        {
            if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
            if (!state || !stateInputBounded(*state)) return { Error::invalidPacket, "checkpoint_state_contract" };
            metadata.add(encodeState(*state));
        }
        CapturedAudioGraph audio;
        const auto status = RecoveryAudioGraph::capture(snapshot, known, audio, limits, cancel, reuse);
        if (!status) return status;
        CommitPacket candidate;
        candidate.identity = snapshot.identity;
        candidate.operationId = "recovery.checkpoint";
        candidate.planJson = checkpointPlan;
        candidate.timestampMilliseconds = juce::Time::currentTimeMillis();
        candidate.stateJson = juce::JSON::toString(object({ { "kind", "document-checkpoint" }, { "schema", 1 },
            { "saved_generation", snapshot.identity.savedGeneration }, { "undo_count", static_cast<int>(snapshot.undo.size()) },
            { "redo_count", static_cast<int>(snapshot.redo.size()) }, { "states", juce::var(metadata) }, { "audio", audio.json } }), true);
        if (!boundedJson(candidate.stateJson, limits.jsonBytes)) return { Error::budgetExceeded, "checkpoint_json_budget" };
        if (reuse)
        {
            const auto staged = reuse->stage(snapshot, audio, candidate.stateJson, limits);
            if (!staged) return staged;
        }
        candidate.newAudio = std::move(audio.newAudio); candidate.existingAudio = std::move(audio.existingAudio);
        candidate.streamedAudio = std::move(audio.streamedAudio);
        output = std::move(candidate);
        return {};
    }
    catch (...) { if (reuse) reuse->discardPending(); return { Error::resourceOrEnvironmentFault, "checkpoint_encode_exception" }; }
}

Status RecoveryCheckpointCodec::encodeWithSourceEvidence(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
    const RecoverySourceEvidence& evidence, const std::set<juce::String>& known, CommitPacket& output,
    AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel, AudioGraphReuseCache* reuse)
{
    if (reuse) reuse->discardPending();
    try
    {
        if (!evidence.bindsTo(snapshot.identity.source)) return { Error::invalidPacket, "checkpoint_source_binding" };
        CommitPacket candidate;
        const auto encoded = encode(snapshot, known, candidate, limits, cancel, reuse);
        if (!encoded) return encoded;
        auto graph = juce::JSON::parse(candidate.stateJson);
        graph.getDynamicObject()->setProperty("schema", 2);
        graph.getDynamicObject()->setProperty("source_evidence", evidence.toJson());
        candidate.stateJson = juce::JSON::toString(graph, true);
        candidate.planSchema = 2; candidate.planJson = checkpointSourcePlan;
        if (!boundedJson(candidate.stateJson, limits.jsonBytes))
        { if (reuse) reuse->discardPending(); return { Error::budgetExceeded, "checkpoint_json_budget" }; }
        if (cancel && cancel())
        { if (reuse) reuse->discardPending(); return { Error::cancelled, "checkpoint_cancelled" }; }
        if (reuse) reuse->pendingState = candidate.stateJson;
        output = std::move(candidate);
        return {};
    }
    catch (...) { if (reuse) reuse->discardPending(); return { Error::resourceOrEnvironmentFault, "checkpoint_encode_exception" }; }
}

Status RecoveryMutationCodec::replay(const juce::String& json, const SingleTrackDocument::CommittedStateSnapshot& base,
                                    SingleTrackDocument::CommittedStateSnapshot& output, RecoveryAudioGraph::Cancel cancel)
{
    using Mutation = SingleTrackDocument::CanonicalMutation;
    if (!boundedJson(json, 256 * 1024)) return { Error::budgetExceeded, "mutation_json_budget" };
    if (!base.state || !base.identity.document.isValid() || base.state->changeGeneration != base.identity.document.generation
        || base.undo.size() > 10 || base.redo.size() > 10 || base.identity.asset.has_value()
        || base.identity.savedGeneration < 0 || base.identity.savedGeneration > maxRevision)
        return { Error::invalidPacket, "mutation_base_contract" };
    try
    {
        if (cancel && cancel()) return { Error::cancelled, "mutation_cancelled" };
        const auto value = juce::JSON::parse(json);
        const auto kind = value["kind"].toString();
        const bool envelope = kind == "envelope.replace";
        if ((!envelope && kind != "history.undo" && kind != "history.redo")
            || !integer(value["schema"], 1, 1) || juce::JSON::toString(value, true) != json
            || !(envelope ? keys(value, { "kind", "schema", "before", "after", "source_binding", "push_undo", "envelope_revision", "envelope" })
                         : keys(value, { "kind", "schema", "before", "after", "source_binding" })))
            return { Error::unsupportedSchema, "mutation_schema" };
        const auto identity = [] (const juce::var& record, media::DocumentIdentity& result)
        {
            if (!keys(record, { "version", "lineage", "runtime_instance", "generation" })
                || !integer(record["version"], 1, 1) || !uuid(record["lineage"]) || !uuid(record["runtime_instance"])
                || !integer(record["generation"], 1, maxRevision)) return false;
            result = { juce::Uuid(record["lineage"].toString()), juce::Uuid(record["runtime_instance"].toString()),
                       static_cast<int64_t>(record["generation"]) };
            return true;
        };
        media::DocumentIdentity before, after;
        if (!identity(value["before"], before) || !identity(value["after"], after) || !uuid(value["source_binding"])
            || !before.sameVersion(base.identity.document) || !after.sameDocument(before)
            || juce::Uuid(value["source_binding"].toString()) != base.identity.source.binding)
            return { Error::staleHead, "mutation_base_identity" };
        auto candidate = base;
        if (envelope)
        {
            if (!value["push_undo"].isBool() || !integer(value["envelope_revision"], 1, maxRevision)
                || after.generation <= before.generation
                || static_cast<int64_t>(value["envelope_revision"]) <= base.state->envelopeRevision)
                return { Error::invalidPacket, "mutation_envelope_revision" };
            if (!stateInputBounded(*base.state)) return { Error::invalidPacket, "mutation_base_metadata" };
            auto metadata = encodeState(*base.state);
            metadata.getDynamicObject()->setProperty("envelope", value["envelope"]);
            metadata.getDynamicObject()->setProperty("envelope_revision", value["envelope_revision"]);
            metadata.getDynamicObject()->setProperty("generation", after.generation);
            State validated;
            if (!decodeState(metadata, validated)) return { Error::invalidPacket, "mutation_envelope_contract" };
            auto next = std::make_shared<State>(*base.state);
            next->envelopePoints = std::move(validated.envelopePoints);
            next->envelopeRevision = validated.envelopeRevision;
            next->changeGeneration = after.generation;
            candidate.state = std::move(next);
            if (static_cast<bool>(value["push_undo"]))
            {
                candidate.undo.push_back(base.state);
                if (candidate.undo.size() > 10) candidate.undo.erase(candidate.undo.begin());
                candidate.redo.clear();
            }
        }
        else if (kind == "history.undo")
        {
            if (candidate.undo.empty() || candidate.redo.size() >= 10) return { Error::invalidPacket, "mutation_undo_boundary" };
            candidate.redo.push_back(candidate.state);
            candidate.state = candidate.undo.back(); candidate.undo.pop_back();
        }
        else
        {
            if (candidate.redo.empty() || candidate.undo.size() >= 10) return { Error::invalidPacket, "mutation_redo_boundary" };
            candidate.undo.push_back(candidate.state);
            candidate.state = candidate.redo.back(); candidate.redo.pop_back();
        }
        if (!candidate.state || candidate.state->changeGeneration != after.generation)
            return { Error::invalidPacket, "mutation_target_generation" };
        candidate.identity.document = after;
        candidate.mutation = Mutation { envelope ? Mutation::Kind::envelopeReplace
            : kind == "history.undo" ? Mutation::Kind::undo : Mutation::Kind::redo,
            before, after, base.identity.source.binding, envelope && static_cast<bool>(value["push_undo"]) };
        if (cancel && cancel()) return { Error::cancelled, "mutation_cancelled" };
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "mutation_replay_exception" }; }
}

Status RecoveryMutationCodec::encode(const SingleTrackDocument::CommittedStateSnapshot& base,
                                    const SingleTrackDocument::CommittedStateSnapshot& committed,
                                    juce::String& output, RecoveryAudioGraph::Cancel cancel)
{
    using Kind = SingleTrackDocument::CanonicalMutation::Kind;
    if (!committed.mutation) return { Error::unsupportedSchema, "mutation_requires_checkpoint" };
    const auto& mutation = *committed.mutation;
    if (mutation.kind != Kind::envelopeReplace && mutation.pushesUndo)
        return { Error::invalidPacket, "mutation_history_contract" };
    if (!committed.state || !mutation.before.sameVersion(base.identity.document)
        || !mutation.after.sameVersion(committed.identity.document) || mutation.sourceBinding != committed.identity.source.binding
        || !committed.identity.source.sameBinding(base.identity.source)
        || committed.identity.source.location.file != base.identity.source.location.file
        || committed.identity.source.sizeBytes != base.identity.source.sizeBytes
        || committed.identity.source.modifiedMilliseconds != base.identity.source.modifiedMilliseconds
        || committed.identity.source.fastStampValid != base.identity.source.fastStampValid
        || committed.identity.savedGeneration != base.identity.savedGeneration || committed.identity.asset.has_value()
        || committed.undo.size() > 10 || committed.redo.size() > 10)
        return { Error::staleHead, "mutation_capture_identity" };
    try
    {
        const auto kind = mutation.kind == Kind::envelopeReplace ? "envelope.replace"
            : mutation.kind == Kind::undo ? "history.undo" : mutation.kind == Kind::redo ? "history.redo" : "unsupported";
        auto record = object({ { "kind", kind }, { "schema", 1 }, { "before", mutation.before.toJson() },
            { "after", mutation.after.toJson() }, { "source_binding", mutation.sourceBinding.toString() } });
        if (mutation.kind == Kind::envelopeReplace)
        {
            if (!stateInputBounded(*committed.state)) return { Error::invalidPacket, "mutation_capture_metadata" };
            record.getDynamicObject()->setProperty("push_undo", mutation.pushesUndo);
            record.getDynamicObject()->setProperty("envelope_revision", committed.state->envelopeRevision);
            record.getDynamicObject()->setProperty("envelope", encodeState(*committed.state)["envelope"]);
        }
        const auto json = juce::JSON::toString(record, true);
        SingleTrackDocument::CommittedStateSnapshot replayed;
        const auto status = replay(json, base, replayed, cancel);
        if (!status) return status;
        const auto sameOwner = [] (const auto& a, const auto& b)
        { return a == b && !a.owner_before(b) && !b.owner_before(a); };
        const auto sameState = [&] (const auto& actual, const auto& wanted)
        {
            return actual && wanted && stateInputBounded(*actual) && stateInputBounded(*wanted)
                && juce::JSON::toString(encodeState(*actual), true) == juce::JSON::toString(encodeState(*wanted), true)
                && actual->audioBackingStore == wanted->audioBackingStore
                && actual->readerSourceFile == wanted->readerSourceFile && actual->info.recoveryBacked == wanted->info.recoveryBacked
                && sameOwner(actual->audioBuffer, wanted->audioBuffer)
                && sameOwner(actual->memoryMappedReader, wanted->memoryMappedReader)
                && sameOwner(actual->streamingReader, wanted->streamingReader)
                && sameOwner(actual->sourceFileWitness, wanted->sourceFileWitness);
        };
        if (!sameState(replayed.state, committed.state) || replayed.undo.size() != committed.undo.size()
            || replayed.redo.size() != committed.redo.size()) return { Error::invalidPacket, "mutation_replay_mismatch" };
        for (size_t i = 0; i < replayed.undo.size(); ++i)
            if (!sameState(replayed.undo[i], committed.undo[i])) return { Error::invalidPacket, "mutation_replay_mismatch" };
        for (size_t i = 0; i < replayed.redo.size(); ++i)
            if (!sameState(replayed.redo[i], committed.redo[i])) return { Error::invalidPacket, "mutation_replay_mismatch" };
        if (cancel && cancel()) return { Error::cancelled, "mutation_cancelled" };
        output = json;
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "mutation_encode_exception" }; }
}

Status RecoveryCheckpointCodec::prepare(const ManifestCommit& commit, PreparedRecoveryCheckpoint& output,
    AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    const auto& record = commit.record;
    if (!record["state"].isString() || record["operation"] != juce::var("recovery.checkpoint")
        || !integer(record["plan_schema"], 1, 2)
        || record["plan"] != juce::var(static_cast<int>(record["plan_schema"]) == 1 ? checkpointPlan : checkpointSourcePlan)
        || !boundedJson(record["state"].toString(), limits.jsonBytes)) return { Error::unsupportedSchema, "checkpoint_record_contract" };
    try
    {
        PreparedRecoveryCheckpoint candidate;
        candidate.head = commit.head;
        if (!readIdentity(record, candidate.identity)) return { Error::invalidPacket, "checkpoint_identity" };
        const auto graph = juce::JSON::parse(record["state"].toString());
        const auto schema = static_cast<int>(record["plan_schema"]);
        const auto shape = schema == 1
            ? keys(graph, { "kind", "schema", "saved_generation", "undo_count", "redo_count", "states", "audio" })
            : keys(graph, { "kind", "schema", "saved_generation", "undo_count", "redo_count", "states", "audio", "source_evidence" });
        if (!shape || graph["kind"] != juce::var("document-checkpoint") || !integer(graph["schema"], schema, schema)
            || !integer(graph["saved_generation"], 0, maxRevision) || !integer(graph["undo_count"], 0, 10)
            || !integer(graph["redo_count"], 0, 10) || !graph["states"].isArray() || !graph["audio"].isString()
            || juce::JSON::toString(graph, true) != record["state"].toString()) return { Error::unsupportedSchema, "checkpoint_schema" };
        if (schema == 2)
        {
            candidate.sourceEvidence = RecoverySourceEvidence::fromJson(graph["source_evidence"]);
            if (!candidate.sourceEvidence) return { Error::unsupportedSchema, "checkpoint_source_evidence" };
            if (!candidate.sourceEvidence->bindsTo(candidate.identity.source)) return { Error::invalidPacket, "checkpoint_source_binding" };
        }
        const auto undoCount = static_cast<int>(graph["undo_count"]), redoCount = static_cast<int>(graph["redo_count"]);
        if (graph["states"].size() != 1 + undoCount + redoCount) return { Error::invalidPacket, "checkpoint_stack_count" };
        candidate.identity.savedGeneration = static_cast<int64_t>(graph["saved_generation"]);
        auto& states = candidate.states;
        for (const auto& metadata : *graph["states"].getArray())
        {
            if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
            auto state = std::make_shared<State>();
            if (!decodeState(metadata, *state)) return { Error::invalidPacket, "checkpoint_state_contract" };
            state->audioBuffer.reset();
            states.push_back(std::move(state));
        }
        if (states.front()->changeGeneration != candidate.identity.document.generation)
            return { Error::invalidPacket, "checkpoint_current_generation" };
        if (!boundedJson(graph["audio"].toString(), limits.jsonBytes)) return { Error::budgetExceeded, "checkpoint_audio_budget" };
        auto& audioGraph = candidate.audio;
        const auto described = RecoveryAudioGraph::describe(graph["audio"].toString(), audioGraph, limits);
        if (!described) return described;
        if (!assetClosure(audioGraph, record["assets"])) return { Error::invalidPacket, "checkpoint_asset_closure" };
        // Cross-bind metadata before any sample allocation or reads. The audio
        // codec below independently validates every sequence and chunk schema.
        if (audioGraph.states.size() != states.size()) return { Error::invalidPacket, "checkpoint_audio_bindings" };
        for (size_t index = 0; index < states.size(); ++index)
        {
            const auto& sequence = audioGraph.sequences[audioGraph.states[index]];
            const auto& info = states[index]->info;
            if (sequence.sampleRate != info.sampleRate || sequence.channels != info.numChannels
                || sequence.frames != info.numSamples) return { Error::invalidPacket, "checkpoint_audio_format" };
        }
        candidate.audioJson = graph["audio"].toString();
        candidate.undoCount = static_cast<size_t>(undoCount); candidate.redoCount = static_cast<size_t>(redoCount);
        if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "checkpoint_prepare_exception" }; }
}

Status RecoveryCheckpointCodec::applyMutationMetadata(const juce::String& delta, const PreparedRecoveryCheckpoint& base,
    PreparedRecoveryCheckpoint& output, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    const auto old = metadataSnapshot(base);
    if (!old.state || base.audio.states.size() != base.states.size()) return { Error::invalidPacket, "mutation_prepared_base" };
    SingleTrackDocument::CommittedStateSnapshot replayed;
    const auto status = RecoveryMutationCodec::replay(delta, old, replayed, cancel);
    if (!status) return status;
    PreparedRecoveryCheckpoint candidate;
    candidate.identity = replayed.identity;
    candidate.states.push_back(replayed.state);
    candidate.states.insert(candidate.states.end(), replayed.undo.begin(), replayed.undo.end());
    candidate.states.insert(candidate.states.end(), replayed.redo.begin(), replayed.redo.end());
    candidate.undoCount = replayed.undo.size(); candidate.redoCount = replayed.redo.size();
    candidate.sourceEvidence = base.sourceEvidence;
    AudioGraphDescriptor remapped;
    std::map<size_t, size_t> sequenceIndexes;
    for (size_t i = 0; i < candidate.states.size(); ++i)
    {
        if (cancel && cancel()) return { Error::cancelled, "mutation_cancelled" };
        const auto found = std::find(base.states.begin(), base.states.end(), candidate.states[i]);
        size_t oldState = 0;
        if (found != base.states.end()) oldState = static_cast<size_t>(std::distance(base.states.begin(), found));
        else if (i != 0 || !replayed.mutation
            || replayed.mutation->kind != SingleTrackDocument::CanonicalMutation::Kind::envelopeReplace)
            return { Error::invalidPacket, "mutation_audio_binding" };
        const auto oldSequence = base.audio.states[oldState];
        if (oldSequence >= base.audio.sequences.size()) return { Error::invalidPacket, "mutation_audio_binding" };
        const auto [entry, inserted] = sequenceIndexes.emplace(oldSequence, remapped.sequences.size());
        if (inserted) remapped.sequences.push_back(base.audio.sequences[oldSequence]);
        remapped.states.push_back(entry->second);
        const auto& sequence = remapped.sequences[entry->second];
        const auto& info = candidate.states[i]->info;
        if (sequence.sampleRate != info.sampleRate || sequence.channels != info.numChannels || sequence.frames != info.numSamples)
            return { Error::invalidPacket, "mutation_audio_dimensions" };
    }
    candidate.audioJson = audioJson(remapped);
    const auto described = RecoveryAudioGraph::describe(candidate.audioJson, candidate.audio, limits);
    if (!described) return described;
    output = std::move(candidate);
    return {};
}

Status RecoveryCheckpointCodec::prepareMutation(const ManifestCommit& commit, const PreparedRecoveryCheckpoint& base,
    PreparedRecoveryCheckpoint& output, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    const auto& record = commit.record;
    if (record["operation"] != juce::var("recovery.mutation") || !integer(record["plan_schema"], 1, 1)
        || record["plan"] != juce::var(mutationPlan) || !record["state"].isString())
        return { Error::unsupportedSchema, "mutation_record_contract" };
    if (base.head.sequence < 1 || base.head.sequence >= std::numeric_limits<int64_t>::max()
        || commit.head.sequence != base.head.sequence + 1 || record["parent"].toString() != base.head.hash
        || !integer(record["sequence"], commit.head.sequence, commit.head.sequence))
        return { Error::staleHead, "mutation_parent_head" };
    if (!boundedJson(record["state"].toString(), limits.jsonBytes)) return { Error::budgetExceeded, "mutation_json_budget" };
    try
    {
        media::IdentitySnapshot identity;
        if (!readIdentity(record, identity) || !sameSource(identity.source, base.identity.source))
            return { Error::staleHead, "mutation_record_identity" };
        PreparedRecoveryCheckpoint candidate;
        const auto status = applyMutationMetadata(record["state"].toString(), base, candidate, limits, cancel);
        if (!status) return status;
        if (!identity.document.sameVersion(candidate.identity.document) || !assetClosure(candidate.audio, record["assets"]))
            return { Error::invalidPacket, "mutation_record_bindings" };
        candidate.head = commit.head;
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "mutation_prepare_exception" }; }
}

Status RecoveryCheckpointCodec::prepareThrough(const ManifestView& manifest, const SafeHead& selected,
    PreparedRecoveryCheckpoint& output, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    if (!manifest.status) return manifest.status;
    if (manifest.commits.size() > 1024) return { Error::budgetExceeded, "checkpoint_chain_budget" };
    const auto found = std::find_if(manifest.commits.begin(), manifest.commits.end(),
        [&] (const auto& commit) { return commit.head == selected; });
    if (found == manifest.commits.end()) return { Error::staleHead, "checkpoint_not_on_chain" };
    try
    {
        auto first = found;
        while (first->record["operation"] != juce::var("recovery.checkpoint"))
        {
            if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
            if (first == manifest.commits.begin()) return { Error::unsupportedSchema, "checkpoint_chain_missing_base" };
            --first;
        }
        PreparedRecoveryCheckpoint current;
        auto status = prepare(*first, current, limits, cancel);
        if (!status) return status;
        for (auto next = std::next(first); next <= found; ++next)
        {
            if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
            PreparedRecoveryCheckpoint advanced;
            status = prepareMutation(*next, current, advanced, limits, cancel);
            if (!status) return status;
            current = std::move(advanced);
        }
        output = std::move(current);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "checkpoint_chain_exception" }; }
}

Status RecoveryCheckpointCodec::compact(RecoveryStore& store, const SafeHead& expected,
    OrphanCollectionReport& report, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    const auto manifest = store.inspectManifest(cancel);
    if (!manifest.status) return manifest.status;
    if (manifest.head != expected || manifest.retirement || manifest.commits.empty())
        return { Error::staleHead, "compaction_manifest_changed" };
    PreparedRecoveryCheckpoint current;
    Status status;
    // A later known checkpoint must not grant deletion of an earlier unknown
    // required operation/schema. Certify the entire chain being superseded too.
    for (const auto& commit : manifest.commits)
    {
        PreparedRecoveryCheckpoint next;
        status = commit.record["operation"] == juce::var("recovery.checkpoint")
            ? prepare(commit, next, limits, cancel) : prepareMutation(commit, current, next, limits, cancel);
        if (!status) return status;
        current = std::move(next);
    }
    auto floor = manifest.commits.rbegin();
    while (floor != manifest.commits.rend() && floor->record["operation"] != juce::var("recovery.checkpoint")) ++floor;
    if (floor == manifest.commits.rend()) return { Error::unsupportedSchema, "compaction_checkpoint_missing" };
    // Every supported base/delta includes complete current/Undo/Redo metadata
    // and the exact declared audio dependency set.
    RecoveryCompactionProof proof(store.rootDirectory(), store.ownerId(), expected, floor->head);
    if (floor->head != manifest.commits.front().head)
    {
        // Retire an interrupted unpublished cut while its references are still
        // known. Its control file alone is collectible; it conveys no authority.
        status = store.collectOrphans(expected, report, {}, cancel);
        if (!status || report.more) return status;
        if (report.filesRemoved != 0) { report.more = true; return {}; }
    }
    // An already published cut may be swept while readers hold its retained
    // graph. Only advancing that floor requires the exclusive usage lease.
    status = floor->head == manifest.commits.front().head ? Status{} : store.compact(proof, cancel);
    if (!status && status.code != "compaction_pending_collection") return status;
    const bool retryCut = !status;
    status = store.collectOrphans(expected, report, {}, cancel);
    if (status && retryCut) report.more = true;
    return status;
}

Status RecoveryCheckpointWriter::prepare(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
                                         CommitPacket& output, RecoveryAudioGraph::Cancel cancel)
{
    pending.reset(); expectedState.clear(); expectedOperation.clear(); expectedPlan.clear(); expectedSchema = 0;
    try
    {
        CommitPacket packet;
        if (!sourceProbeAttempted)
        {
            sourceProbeAttempted = true;
            // Source identity is optional for a self-contained detached copy.
            // Never block real audio/history protection or manufacture Exact
            // when original source admission facts are unavailable or stale.
            if (snapshot.sourceVersion && !(cancel && cancel()))
            {
                auto probe = RecoverySourceIdentity::captureAdmittedSource(snapshot.identity.source, *snapshot.sourceVersion);
                if (probe.status) admittedSourceEvidence = std::move(probe.evidence);
            }
        }
        const auto evidence = committed && committed->sourceEvidence ? committed->sourceEvidence : admittedSourceEvidence;
        auto status = evidence && evidence->bindsTo(snapshot.identity.source)
            ? RecoveryCheckpointCodec::encodeWithSourceEvidence(snapshot, *evidence, {}, packet, bounds, cancel, &audioReuse)
            : RecoveryCheckpointCodec::encode(snapshot, {}, packet, bounds, cancel, &audioReuse);
        if (!status) return status;
        AudioGraphDescriptor graph;
        status = RecoveryAudioGraph::describe(juce::JSON::parse(packet.stateJson)["audio"].toString(), graph, bounds);
        if (!status) return status;
        PreparedRecoveryCheckpoint prepared;
        status = RecoveryCheckpointCodec::prepare({ {}, localRecord(packet, graph) }, prepared, bounds, cancel);
        if (!status) return status;
        if (committed && snapshot.mutation && deltaDepth < deltaLimit)
        {
            auto target = metadataSnapshot(prepared);
            target.mutation = snapshot.mutation;
            juce::String delta;
            status = RecoveryMutationCodec::encode(metadataSnapshot(*committed), target, delta, cancel);
            if (!status) return status;
            PreparedRecoveryCheckpoint replayed;
            status = RecoveryCheckpointCodec::applyMutationMetadata(delta, *committed, replayed, bounds, cancel);
            if (!status) return status;
            if (replayed.audioJson != prepared.audioJson || !packet.newAudio.empty() || !packet.streamedAudio.empty())
                return { Error::invalidPacket, "mutation_changed_audio_graph" };
            packet.operationId = "recovery.mutation"; packet.planSchema = 1; packet.planJson = mutationPlan;
            packet.stateJson = delta;
        }
        if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
        audioReuse.pendingState = packet.stateJson;
        expectedState = packet.stateJson; expectedOperation = packet.operationId;
        expectedPlan = packet.planJson; expectedSchema = packet.planSchema;
        pending = std::move(prepared);
        output = std::move(packet);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "checkpoint_writer_prepare_exception" }; }
}

Status RecoveryCheckpointWriter::published(const std::shared_ptr<const PublishedRecoveryCommit>& receipt)
{
    if (!receipt || !pending || !receipt->documentIdentity().sameVersion(pending->identity.document)
        || !sameSource(receipt->sourceIdentity(), pending->identity.source)
        || receipt->stateJson() != expectedState || receipt->operationId() != expectedOperation
        || receipt->planJson() != expectedPlan || receipt->planSchema() != expectedSchema
        || (committed && receipt->head().sequence != committed->head.sequence + 1))
        return { Error::staleHead, "checkpoint_publication_mismatch" };
    const auto status = audioReuse.acknowledge(receipt);
    if (!status) return status;
    pending->head = receipt->head();
    committed = std::move(pending); pending.reset();
    deltaDepth = expectedOperation == "recovery.mutation" ? deltaDepth + 1 : 0;
    expectedState.clear(); expectedOperation.clear(); expectedPlan.clear(); expectedSchema = 0;
    return {};
}

Status RecoveryCheckpointCodec::decode(const VerifiedCommit& commit, const RecoveryStore& store,
    SingleTrackDocument::CommittedStateSnapshot& output, AudioGraphLimits limits, RecoveryAudioGraph::Cancel cancel)
{
    PreparedRecoveryCheckpoint prepared;
    const auto metadata = commit.record["operation"] == juce::var("recovery.mutation")
        ? prepareThrough(store.inspectManifest(cancel), commit.head, prepared, limits, cancel)
        : prepare({ commit.head, commit.record }, prepared, limits, cancel);
    if (!metadata) return metadata;
    try
    {
        std::vector<std::shared_ptr<juce::AudioBuffer<float>>> buffers;
        const auto status = RecoveryAudioGraph::materialise(prepared.audioJson, store, buffers, limits, cancel);
        if (!status) return status;
        SingleTrackDocument::CommittedStateSnapshot candidate;
        candidate.identity = prepared.identity;
        for (size_t index = 0; index < prepared.states.size(); ++index)
        {
            if (cancel && cancel()) return { Error::cancelled, "checkpoint_cancelled" };
            auto state = std::make_shared<State>(*prepared.states[index]);
            state->audioBuffer = std::move(buffers[index]);
            state->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
            state->playbackAvailability = State::PlaybackAvailability::randomAccess();
            if (index == 0) candidate.state = state;
            else if (index <= prepared.undoCount) candidate.undo.push_back(state);
            else candidate.redo.push_back(state);
        }
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "checkpoint_decode_exception" }; }
}
} // namespace singleeditor::recovery
