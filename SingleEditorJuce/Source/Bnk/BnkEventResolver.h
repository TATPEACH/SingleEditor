#pragma once

#include <memory>
#include <functional>

#include "Bnk/BnkDocument.h"
#include "Bnk/BnkRuntimeGraph.h"

namespace singleeditor
{
class BnkEventResolver final
{
public:
    // The projection is bounded independently of the number of paths through
    // shared objects. Cancellation publishes no partial graph.
    using Checkpoint = std::function<bool()>;
    static std::shared_ptr<BnkRuntimeGraph::EventGraph> resolve(const BnkDocument::State& state, uint32_t eventId,
                                                              Checkpoint checkpoint = {});
};
} // namespace singleeditor
