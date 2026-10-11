#include "Bnk/BnkEventResolver.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace singleeditor
{
namespace
{
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

juce::String formatId(uint32_t id)
{
    return "0x" + juce::String::toHexString(static_cast<juce::int64>(id)).paddedLeft('0', 8).toUpperCase();
}

BnkRuntimeGraph::NodeType mapNodeType(BnkDocument::HircObjectType type) noexcept
{
    switch (type)
    {
        case BnkDocument::HircObjectType::event: return BnkRuntimeGraph::NodeType::event;
        case BnkDocument::HircObjectType::eventAction: return BnkRuntimeGraph::NodeType::eventAction;
        case BnkDocument::HircObjectType::sound: return BnkRuntimeGraph::NodeType::sound;
        case BnkDocument::HircObjectType::randomSequenceContainer:
        case BnkDocument::HircObjectType::musicRandomSequenceContainer:
            return BnkRuntimeGraph::NodeType::randomSequenceContainer;
        case BnkDocument::HircObjectType::switchContainer:
        case BnkDocument::HircObjectType::musicSwitchContainer:
            return BnkRuntimeGraph::NodeType::switchContainer;
        case BnkDocument::HircObjectType::actorMixer:
            return BnkRuntimeGraph::NodeType::actorMixer;
        case BnkDocument::HircObjectType::blendContainer:
            return BnkRuntimeGraph::NodeType::blendContainer;
        case BnkDocument::HircObjectType::musicSegment:
            return BnkRuntimeGraph::NodeType::musicSegment;
        case BnkDocument::HircObjectType::musicTrack:
            return BnkRuntimeGraph::NodeType::musicTrack;
        case BnkDocument::HircObjectType::unknown:
        case BnkDocument::HircObjectType::settings:
        case BnkDocument::HircObjectType::audioBus:
        default:
            return BnkRuntimeGraph::NodeType::unknown;
    }
}

bool canContainChildren(BnkRuntimeGraph::NodeType type) noexcept
{
    switch (type)
    {
        case BnkRuntimeGraph::NodeType::event:
        case BnkRuntimeGraph::NodeType::eventAction:
        case BnkRuntimeGraph::NodeType::randomSequenceContainer:
        case BnkRuntimeGraph::NodeType::switchContainer:
        case BnkRuntimeGraph::NodeType::actorMixer:
        case BnkRuntimeGraph::NodeType::blendContainer:
        case BnkRuntimeGraph::NodeType::musicSegment:
        case BnkRuntimeGraph::NodeType::musicTrack:
            return true;
        case BnkRuntimeGraph::NodeType::sound:
        case BnkRuntimeGraph::NodeType::unknown:
        default:
            return false;
    }
}

std::vector<uint32_t> collectReferencedObjectIds(const juce::MemoryBlock& payload,
                                                 const std::unordered_map<uint32_t, const BnkDocument::HircObject*>& objectLookup,
                                                 uint32_t selfId, const BnkEventResolver::Checkpoint& checkpoint, bool& cancelled)
{
    std::vector<uint32_t> refs;
    refs.reserve(8);
    std::unordered_set<uint32_t> seen;

    for (size_t offset = 0; offset + 4 <= payload.getSize(); offset += 4)
    {
        if (offset % 65536 == 0 && checkpoint && !checkpoint()) { cancelled = true; return {}; }
        const auto candidate = readU32LE(payload, offset);
        if (candidate == 0 || candidate == selfId)
            continue;

        if (objectLookup.find(candidate) == objectLookup.end())
            continue;

        if (!seen.insert(candidate).second)
            continue;

        refs.push_back(candidate);
        if (refs.size() >= 16)
            break;
    }

    return refs;
}
} // namespace

std::shared_ptr<BnkRuntimeGraph::EventGraph> BnkEventResolver::resolve(const BnkDocument::State& state, uint32_t eventId,
                                                                   Checkpoint checkpoint)
{
    if (state.objects.size() > 65536 || state.events.size() > 4096 || (checkpoint && !checkpoint())) return {};
    const auto eventIt = std::find_if(state.events.begin(),
                                      state.events.end(),
                                      [eventId](const BnkDocument::EventDescriptor& event)
                                      {
                                          return event.id == eventId;
                                      });
    if (eventIt == state.events.end())
        return {};
    if (eventIt->actionIds.size() > 4096) return {};

    std::unordered_map<uint32_t, const BnkDocument::HircObject*> objectLookup;
    objectLookup.reserve(state.objects.size());
    for (const auto& object : state.objects)
    {
        if (object.payload.getSize() > 1024 * 1024 || !objectLookup.emplace(object.id, &object).second
            || (checkpoint && !checkpoint())) return {};
    }

    auto graph = std::make_shared<BnkRuntimeGraph::EventGraph>();
    graph->eventId = eventIt->id;
    graph->eventName = eventIt->displayName;
    graph->diagnostics.add("Graph built from a lightweight heuristic HIRC scan. Playback backend wiring comes next.");

    std::unordered_map<uint32_t, int> nodeIndexById;
    bool truncated = false, cancelled = false;

    std::function<int(uint32_t, BnkRuntimeGraph::NodeType, juce::String, juce::String)> addNode =
        [&graph, &nodeIndexById](uint32_t objectId, BnkRuntimeGraph::NodeType type, juce::String title, juce::String detail)
    {
        if (const auto found = nodeIndexById.find(objectId); found != nodeIndexById.end())
            return found->second;

        BnkRuntimeGraph::Node node;
        node.type = type;
        node.objectId = objectId;
        node.title = std::move(title);
        node.detail = std::move(detail);
        const auto index = static_cast<int>(graph->nodes.size());
        graph->nodes.push_back(std::move(node));
        nodeIndexById.emplace(objectId, index);
        if (type == BnkRuntimeGraph::NodeType::sound)
            ++graph->soundNodeCount;
        return index;
    };

    struct Pending { uint32_t id; int index; int depth; };
    std::vector<Pending> pending;
    pending.reserve(96);
    // Breadth-first discovery reaches each object at its shortest depth, so a
    // shared node is scanned once without losing a later, shallower path.
    const auto wouldCycle = [&graph](int parent, int child)
    {
        std::array<int, 96> todo {};
        std::array<bool, 96> seen {};
        size_t count = 1;
        todo[0] = child; seen[static_cast<size_t>(child)] = true;
        while (count > 0)
        {
            const auto current = todo[--count];
            if (current == parent) return true;
            for (const auto next : graph->nodes[static_cast<size_t>(current)].children)
                if (!seen[static_cast<size_t>(next)]) { seen[static_cast<size_t>(next)] = true; todo[count++] = next; }
        }
        return false;
    };
    const auto connect = [&](int parentIndex, uint32_t objectId, int depth)
    {
        if (checkpoint && !checkpoint()) { cancelled = true; return; }
        if (depth > 8) { truncated = true; return; }
        const auto found = objectLookup.find(objectId);
        if (found == objectLookup.end()) return;
        const auto existing = nodeIndexById.find(objectId);
        int childIndex = -1;
        if (existing != nodeIndexById.end()) childIndex = existing->second;
        else
        {
            if (graph->nodes.size() >= 96) { truncated = true; return; }
            const auto& object = *found->second;
            childIndex = addNode(object.id, mapNodeType(object.type),
                BnkDocument::describeObjectType(object.type) + " " + formatId(object.id),
                "Payload " + juce::String(object.payloadSize) + " B");
            pending.push_back({ objectId, childIndex, depth });
        }
        auto& children = graph->nodes[static_cast<size_t>(parentIndex)].children;
        if (std::find(children.begin(), children.end(), childIndex) == children.end()
            && !wouldCycle(parentIndex, childIndex)) children.push_back(childIndex);
    };

    const auto rootIndex = addNode(eventIt->id,
                                   BnkRuntimeGraph::NodeType::event,
                                   eventIt->displayName,
                                   eventIt->summary);
    if (eventIt->actionIds.empty())
        graph->diagnostics.add("Event exposes no parsed action ids yet.");

    for (const auto actionId : eventIt->actionIds)
    {
        connect(rootIndex, actionId, 0);
        if (cancelled) return {};
    }
    for (size_t cursor = 0; cursor < pending.size(); ++cursor)
    {
        const auto entry = pending[cursor];
        const auto& object = *objectLookup.at(entry.id);
        const auto nodeType = mapNodeType(object.type);
        if (!canContainChildren(nodeType)) continue;
        const auto refs = collectReferencedObjectIds(object.payload, objectLookup, object.id, checkpoint, cancelled);
        if (cancelled) return {};
        if (refs.empty() && nodeType == BnkRuntimeGraph::NodeType::eventAction)
            graph->diagnostics.add("EventAction " + formatId(object.id) + " did not expose a direct target in the current heuristic pass.");
        for (const auto refId : refs)
        {
            connect(entry.index, refId, entry.depth + 1);
            if (cancelled) return {};
        }
    }
    if (truncated) graph->diagnostics.add("Optional graph projection reached its node or depth limit.");
    if (checkpoint && !checkpoint()) return {};

    graph->hasMultiNodeGraph = graph->nodes.size() > 2;
    return graph;
}
} // namespace singleeditor
