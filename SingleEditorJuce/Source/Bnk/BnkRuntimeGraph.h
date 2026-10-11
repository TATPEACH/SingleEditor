#pragma once

#include <vector>

#include <juce_core/juce_core.h>

namespace singleeditor::BnkRuntimeGraph
{
enum class NodeType
{
    event,
    eventAction,
    sound,
    randomSequenceContainer,
    switchContainer,
    actorMixer,
    blendContainer,
    musicSegment,
    musicTrack,
    unknown
};

struct Node
{
    NodeType type = NodeType::unknown;
    uint32_t objectId = 0;
    juce::String title;
    juce::String detail;
    std::vector<int> children;
};

struct EventGraph
{
    uint32_t eventId = 0;
    juce::String eventName;
    std::vector<Node> nodes;
    juce::StringArray diagnostics;
    int soundNodeCount = 0;
    bool hasMultiNodeGraph = false;
    bool usesHeuristicReferenceScan = true;
};

juce::String describeNodeType(NodeType type);
} // namespace singleeditor::BnkRuntimeGraph
