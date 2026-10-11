#include "Bnk/BnkParser.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

#include "Bnk/BnkEventResolver.h"
#include "Bnk/BnkSidecarParser.h"
#include "Bnk/WaveDurationHint.h"
#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
namespace
{
constexpr uint32_t fourCC(char a, char b, char c, char d) noexcept
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
         | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint32_t bkhdChunk = fourCC('B', 'K', 'H', 'D');
constexpr uint32_t didxChunk = fourCC('D', 'I', 'D', 'X');
constexpr uint32_t dataChunk = fourCC('D', 'A', 'T', 'A');
constexpr uint32_t hircChunk = fourCC('H', 'I', 'R', 'C');
constexpr size_t maximumTableBytes = 16 * 1024 * 1024;
constexpr size_t maximumObjects = 65536;
constexpr size_t maximumEvents = 4096;
constexpr size_t maximumActionLinks = 4096;
constexpr size_t maximumTotalActionLinks = 65536;

struct ParseControl final
{
    BnkParser::Checkpoint checkpoint;
    BnkParser::Profile profile;
    BnkParser::Progress progress;
    BnkParser::Failure failure = BnkParser::Failure::none;
    size_t actionLinks = 0;
    bool unavailableDurationHints = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool check()
    {
        if (failure != BnkParser::Failure::none) return false;
        if (checkpoint && !checkpoint(progress)) { failure = BnkParser::Failure::cancelled; return false; }
        // One outer deadline includes all stages. The optional sidecar retains
        // its smaller 5/20 s capability budget and can degrade independently.
        const auto limit = profile == BnkParser::Profile::expanded ? std::chrono::seconds(120) : std::chrono::seconds(30);
        if (std::chrono::steady_clock::now() - started > limit) { failure = BnkParser::Failure::deadline; return false; }
        return true;
    }
    bool check(BnkParser::Stage stage) { progress.stage = stage; return check(); }
    bool reject(BnkParser::Failure reason) { failure = reason; return false; }
};

uint32_t readU32LE(const juce::MemoryBlock& bytes, size_t offset) noexcept
{
    if (offset + 4 > bytes.getSize())
        return 0;

    const auto* src = static_cast<const uint8_t*>(bytes.getData()) + offset;
    return static_cast<uint32_t>(src[0])
         | (static_cast<uint32_t>(src[1]) << 8)
         | (static_cast<uint32_t>(src[2]) << 16)
         | (static_cast<uint32_t>(src[3]) << 24);
}

uint8_t readU8(const juce::MemoryBlock& bytes, size_t offset) noexcept
{
    if (offset >= bytes.getSize())
        return 0;

    return *(static_cast<const uint8_t*>(bytes.getData()) + offset);
}

juce::String formatId(uint32_t id)
{
    return "0x" + juce::String::toHexString(static_cast<juce::int64>(id)).paddedLeft('0', 8).toUpperCase();
}


double estimateLongestMediaDurationSeconds(const BnkDocument::EventDescriptor& event, int& remainingProbes, ParseControl& control)
{
    double longest = 0.0;

    for (const auto& media : event.media)
    {
        if (!control.check() || remainingProbes <= 0)
            break;
        --remainingProbes;

        const auto hint = WaveDurationHint::inspect(media.mediaFile, [&] { return control.check(); });
        if (!hint)
        {
            control.unavailableDurationHints = true;
            continue;
        }

        longest = juce::jmax(longest,
                             hint.seconds);
    }

    return longest;
}

bool looksLikeSequenceEvent(const BnkDocument::EventDescriptor& event, int& remainingProbes, ParseControl& control)
{
    if (event.media.size() < 2)
        return false;

    const auto eventDuration = juce::jmax(event.durationMinSeconds, event.durationMaxSeconds);
    if (eventDuration <= 0.0)
        return false;

    const auto longestCandidate = estimateLongestMediaDurationSeconds(event, remainingProbes, control);
    if (longestCandidate <= 0.0)
        return false;

    return eventDuration >= longestCandidate * 1.75;
}

bool parseDidxChunk(const juce::MemoryBlock& bytes, BnkDocument::State& state, ParseControl& control)
{
    if (bytes.getSize() % 12 != 0) return false;
    if (bytes.getSize() / 12 > maximumObjects) return control.reject(BnkParser::Failure::hardLimit);
    const auto entryCount = static_cast<int>(bytes.getSize() / 12);
    state.mediaEntries.reserve(static_cast<std::size_t>(entryCount));
    std::unordered_set<uint32_t> ids;

    for (int i = 0; i < entryCount; ++i)
    {
        if (!control.check(BnkParser::Stage::coreObjects)) return false;
        ++control.progress.objectsVisited;
        const auto base = static_cast<size_t>(i * 12);
        BnkDocument::MediaEntry entry;
        entry.id = readU32LE(bytes, base);
        entry.offset = readU32LE(bytes, base + 4);
        entry.size = readU32LE(bytes, base + 8);
        if (!ids.insert(entry.id).second) return false;
        state.mediaEntries.push_back(entry);
    }

    state.info.mediaCount = static_cast<int>(state.mediaEntries.size());
    return true;
}

bool parseEventObject(uint32_t objectId, const juce::MemoryBlock& payload, BnkDocument::State& state, ParseControl& control)
{
    if (state.events.size() >= maximumEvents) return control.reject(BnkParser::Failure::hardLimit);
    BnkDocument::EventDescriptor event;
    event.id = objectId;
    event.displayName = "Event " + formatId(objectId);

    auto parsedActionList = false;
    if (payload.getSize() >= 4)
    {
        const auto count = readU32LE(payload, 0);
        if (static_cast<size_t>(count) == (payload.getSize() - 4) / 4 && (payload.getSize() - 4) % 4 == 0)
        {
            if (static_cast<size_t>(count) > maximumActionLinks || count > maximumTotalActionLinks - control.actionLinks)
                return control.reject(BnkParser::Failure::hardLimit);
            control.actionLinks += count;
            event.actionIds.reserve(static_cast<std::size_t>(count));
            for (uint32_t i = 0; i < count; ++i)
            {
                if (!control.check()) return false;
                event.actionIds.push_back(readU32LE(payload, 4 + static_cast<size_t>(i) * 4));
            }
            parsedActionList = true;
        }
    }

    if (!parsedActionList && payload.getSize() >= 1)
    {
        const auto count = static_cast<int>(readU8(payload, 0));
        if (1 + static_cast<size_t>(count) * 4 == payload.getSize())
        {
            if (static_cast<size_t>(count) > maximumTotalActionLinks - control.actionLinks)
                return control.reject(BnkParser::Failure::hardLimit);
            control.actionLinks += static_cast<size_t>(count);
            event.actionIds.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                if (!control.check()) return false;
                event.actionIds.push_back(readU32LE(payload, 1 + static_cast<size_t>(i) * 4));
            }
            parsedActionList = true;
        }
    }

    if (parsedActionList)
    {
        event.summary = juce::String(event.actionIds.size()) + " action link(s)";
        event.mayResolveToMultipleNodes = event.actionIds.size() > 1;
    }
    else
    {
        // Neither supported width consumes the payload. Do not silently turn
        // malformed/trailing bytes into different action IDs or an empty graph.
        return control.reject(BnkParser::Failure::invalidData);
    }

    state.events.push_back(std::move(event));
    return true;
}

bool parseHircChunk(const juce::MemoryBlock& bytes, BnkDocument::State& state, ParseControl& control)
{
    if (bytes.getSize() < 4)
    {
        state.info.diagnostics.add("HIRC chunk too small.");
        return false;
    }

    const auto objectCount = readU32LE(bytes, 0);
    if (objectCount > maximumObjects) return control.reject(BnkParser::Failure::hardLimit);
    if (objectCount > (bytes.getSize() - 4) / 9) return false;
    state.objects.reserve(objectCount);
    std::unordered_set<uint32_t> ids;
    size_t cursor = 4;

    for (uint32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
    {
        if (!control.check(BnkParser::Stage::coreObjects)) return false;
        ++control.progress.objectsVisited;
        if (cursor + 9 > bytes.getSize())
        {
            state.info.diagnostics.add("HIRC object table truncated.");
            return false;
        }

        const auto typeValue = readU8(bytes, cursor);
        cursor += 1;
        const auto objectSize = readU32LE(bytes, cursor);
        cursor += 4;
        if (objectSize > 1024 * 1024) return control.reject(BnkParser::Failure::hardLimit);
        if (objectSize < 4 || objectSize > bytes.getSize() - cursor)
        {
            state.info.diagnostics.add("HIRC object payload is malformed.");
            return false;
        }

        const auto objectId = readU32LE(bytes, cursor);
        if (!ids.insert(objectId).second) return false;
        cursor += 4;

        const auto payloadSize = static_cast<size_t>(objectSize - 4);
        juce::MemoryBlock payload(payloadSize);
        if (payloadSize > 0)
            payload.copyFrom(static_cast<const uint8_t*>(bytes.getData()) + cursor, 0, payloadSize);

        BnkDocument::HircObject object;
        object.type = static_cast<BnkDocument::HircObjectType>(typeValue);
        object.id = objectId;
        object.payloadSize = static_cast<uint32_t>(payloadSize);
        if (object.type == BnkDocument::HircObjectType::event && !parseEventObject(objectId, payload, state, control))
            return false;
        object.payload = std::move(payload);
        state.objects.push_back(std::move(object));

        cursor += payloadSize;
    }

    state.info.hircObjectCount = static_cast<int>(state.objects.size());
    state.info.eventCount = static_cast<int>(state.events.size());
    return cursor == bytes.getSize();
}

void refinePlaybackHintsFromGraphs(BnkDocument::State& state, ParseControl& control)
{
    uint64_t payloadBytes = 0;
    for (const auto& object : state.objects) payloadBytes += object.payload.getSize();
    if (state.events.size() * state.objects.size() > 4 * 1024 * 1024
        || payloadBytes * state.events.size() > 64 * 1024 * 1024)
    {
        state.info.diagnostics.add("Optional playback-shape hints exceed the graph work budget; explicit event data remains available.");
        return;
    }
    int remainingProbes = 8;
    for (auto& event : state.events)
    {
        if (!control.check(BnkParser::Stage::playbackHints)) return;
        auto graph = BnkEventResolver::resolve(state, event.id, [&] { return control.check(); });
        if (graph == nullptr)
            continue;

        auto hasRandomSequence = false;
        auto hasSwitchContainer = false;
        auto hasLayerStyleNode = false;

        for (const auto& node : graph->nodes)
        {
            if (!control.check()) return;
            switch (node.type)
            {
                case BnkRuntimeGraph::NodeType::randomSequenceContainer:
                    hasRandomSequence = true;
                    break;
                case BnkRuntimeGraph::NodeType::switchContainer:
                    hasSwitchContainer = true;
                    break;
                case BnkRuntimeGraph::NodeType::actorMixer:
                case BnkRuntimeGraph::NodeType::blendContainer:
                    hasLayerStyleNode = true;
                    break;
                default:
                    break;
            }
        }

        if (hasSwitchContainer && event.playbackHint == BnkDocument::PlaybackHint::switchDefault)
            continue;

        if (hasLayerStyleNode && graph->soundNodeCount > 1 && event.media.size() > 1)
        {
            event.playbackHint = BnkDocument::PlaybackHint::layered;
            event.playbackSummary = "Layered  |  " + juce::String(static_cast<int>(event.media.size())) + " media";
            event.mayResolveToMultipleNodes = true;
            continue;
        }

        if (hasRandomSequence && event.media.size() > 1
            && (event.playbackHint == BnkDocument::PlaybackHint::single
                || event.playbackHint == BnkDocument::PlaybackHint::random))
        {
            if (looksLikeSequenceEvent(event, remainingProbes, control))
            {
                event.playbackHint = BnkDocument::PlaybackHint::sequence;
                event.playbackSummary = "Sequence (heuristic)  |  " + juce::String(static_cast<int>(event.media.size())) + " step(s)";
            }
            else
            {
                event.playbackHint = BnkDocument::PlaybackHint::random;
                event.playbackSummary = "Random  |  " + juce::String(static_cast<int>(event.media.size())) + " candidate(s)";
            }
            event.mayResolveToMultipleNodes = true;
        }
    }
}
std::shared_ptr<BnkDocument::State> parseCandidate(const juce::File& file, const juce::File& projectRootOverride,
    ParseControl& control, BnkParser::Result& result, const BnkParser::Hooks& hooks)
{
    if (!file.existsAsFile())
        return {};

    juce::FileInputStream input(file);
    if (!input.openedOk()) { control.failure = BnkParser::Failure::ioFailure; return {}; }
    if (input.getTotalLength() > 8LL * 1024 * 1024 * 1024)
    { control.failure = BnkParser::Failure::hardLimit; return {}; }
    if (input.getTotalLength() < 16)
        return {};

    auto state = std::make_shared<BnkDocument::State>();
    state->info.file = file;
    state->sourceFileSize = input.getTotalLength();
    state->sourceModifiedMs = file.getLastModificationTime().toMilliseconds();
    state->sourceFileIdentifier = file.getFileIdentifier();
    std::unordered_set<uint32_t> structuralChunks;
    int chunks = 0;

    while (!input.isExhausted())
    {
        if (!control.check(BnkParser::Stage::coreRead)) return {};
        if (++chunks > 4096) { control.failure = BnkParser::Failure::hardLimit; return {}; }
        if (input.getNumBytesRemaining() < 8)
            return {};

        const auto chunkId = static_cast<uint32_t>(input.readInt());
        const auto chunkSize = static_cast<uint32_t>(input.readInt());
        if ((chunks == 1 && chunkId != bkhdChunk) || input.getNumBytesRemaining() < static_cast<int64_t>(chunkSize))
            return {};
        const auto dataStart = input.getPosition();
        const auto known = chunkId == bkhdChunk || chunkId == didxChunk || chunkId == hircChunk || chunkId == dataChunk;
        if (known && !structuralChunks.insert(chunkId).second) return {};
        if (chunkId == dataChunk)
        {
            state->dataChunkStart = dataStart;
            state->dataChunkSize = chunkSize;
        }
        // DATA and unknown payloads are never materialised merely to enumerate
        // a bank. Wwise chunks advance by their declared size, without RIFF padding.
        if (chunkId == dataChunk || !known)
        {
            if (!input.setPosition(dataStart + chunkSize)) return {};
            continue;
        }
        if (chunkSize > maximumTableBytes) { control.failure = BnkParser::Failure::hardLimit; return {}; }

        juce::MemoryBlock chunkBytes(chunkSize);
        for (uint32_t offset = 0; offset < chunkSize;)
        {
            if (!control.check(BnkParser::Stage::coreRead)) return {};
            const auto count = static_cast<int>(juce::jmin(uint32_t{65536}, chunkSize - offset));
            if (input.read(static_cast<char*>(chunkBytes.getData()) + offset, count) != count)
            { control.failure = BnkParser::Failure::ioFailure; return {}; }
            offset += static_cast<uint32_t>(count);
            control.progress.bytesRead += static_cast<uint64_t>(count);
        }
        if (!control.check()) return {};

        if (chunkId == bkhdChunk)
        {
            if (chunkBytes.getSize() >= 8)
            {
                state->info.version = readU32LE(chunkBytes, 0);
                state->info.bankId = readU32LE(chunkBytes, 4);
            }
            else return {};
        }
        else if (chunkId == didxChunk)
        {
            if (!parseDidxChunk(chunkBytes, *state, control)) return {};
        }
        else if (chunkId == hircChunk)
        {
            if (!parseHircChunk(chunkBytes, *state, control)) return {};
        }

    }

    if (state->info.version == 0 && state->mediaEntries.empty() && state->objects.empty())
        return {};
    for (const auto& media : state->mediaEntries)
    {
        if (!control.check()) return {};
        if (static_cast<uint64_t>(media.offset) + media.size > static_cast<uint64_t>(state->dataChunkSize))
            return {};
    }

    std::sort(state->events.begin(),
              state->events.end(),
              [](const BnkDocument::EventDescriptor& a, const BnkDocument::EventDescriptor& b)
              {
                  return a.id < b.id;
              });

    state->info.mediaCount = static_cast<int>(state->mediaEntries.size());
    state->info.hircObjectCount = static_cast<int>(state->objects.size());
    state->info.eventCount = static_cast<int>(state->events.size());
    if (state->events.empty())
        state->info.diagnostics.add("No Event objects were resolved in this BNK yet.");

    if (!control.check(BnkParser::Stage::sidecar)) return {};
    const std::function<bool()> sidecarCheckpoint = [&] { return control.check(BnkParser::Stage::sidecar); };
    result.sidecar = hooks.sidecar ? hooks.sidecar(*state, sidecarCheckpoint)
        : BnkSidecarParser::enrichStateFromSidecars(file, *state, projectRootOverride, sidecarCheckpoint, control.profile);
    if (!control.check(BnkParser::Stage::playbackHints)) return {};
    refinePlaybackHintsFromGraphs(*state, control);
    if (control.unavailableDurationHints)
        state->info.diagnostics.add("Some optional duration hints are unavailable; explicit event metadata remains available.");

    return control.check(BnkParser::Stage::complete) && state->isSourceCurrent() ? state : nullptr;
}
} // namespace

BnkParser::Result BnkParser::parse(const juce::File& file, const juce::File& projectRootOverride,
                                  Checkpoint checkpoint, Profile profile, const Hooks& hooks)
{
    Result result;
    ParseControl control { std::move(checkpoint), profile };
    try
    {
        if (control.check() && media::SourceFileLease::open(file, result.inputLease).wasOk() && result.inputLease)
        {
            if (hooks.admitCore && !hooks.admitCore(*result.inputLease->witness()))
            { result.admissionDenied = true; result.inputLease.reset(); return result; }
            result.state = parseCandidate(file, projectRootOverride, control, result, hooks);
            if (result.state && result.inputLease->recheck().failed())
            { result.state.reset(); control.failure = Failure::ioFailure; }
            result.failure = control.failure;
            if (!result.state && result.failure == Failure::none) result.failure = Failure::invalidData;
        }
        else result.failure = control.failure == Failure::none ? Failure::ioFailure : control.failure;
    }
    catch (const std::bad_alloc&) { result.state.reset(); result.failure = Failure::memoryQuota; }
    catch (...) { result.state.reset(); result.failure = Failure::ioFailure; }
    if (!result) result.inputLease.reset();
    return result;
}

std::shared_ptr<BnkDocument::State> BnkParser::parseFile(const juce::File& file, const juce::File& projectRootOverride)
{ return parse(file, projectRootOverride).state; }
} // namespace singleeditor
