#include "Bnk/BnkRuntimeGraph.h"

namespace singleeditor::BnkRuntimeGraph
{
juce::String describeNodeType(NodeType type)
{
    switch (type)
    {
        case NodeType::event: return "Event";
        case NodeType::eventAction: return "Event Action";
        case NodeType::sound: return "Sound";
        case NodeType::randomSequenceContainer: return "Random/Sequence Container";
        case NodeType::switchContainer: return "Switch Container";
        case NodeType::actorMixer: return "Actor Mixer";
        case NodeType::blendContainer: return "Blend Container";
        case NodeType::musicSegment: return "Music Segment";
        case NodeType::musicTrack: return "Music Track";
        case NodeType::unknown:
        default: return "Unknown";
    }
}
} // namespace singleeditor::BnkRuntimeGraph
