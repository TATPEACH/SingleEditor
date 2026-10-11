#pragma once
#include "Core/OptionalLifecycleObserver.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <vector>

#if !SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
 #error Lifecycle test consumer requires the bounded test observer
#endif

namespace singleeditor::lifecycle_test
{
using Action = OptionalLifecycleAction;
using Kind = OptionalLifecycleResourceKind;
using Owner = OptionalLifecycleOwner;
using Event = OptionalLifecycleEvent;
// Same elapsed-time predicate used by the native scenario loop. Synthetic
// boundary checks exercise this gate without moving the observation point or
// sleeping; individual resource deadlines remain separate owner contracts.
inline void validateScenarioDeadline(double elapsed, double limit)
{
    if (!(elapsed < limit)) throw std::runtime_error("native scenario timed out");
}
inline const char* actionName(Action action)
{
    switch (action)
    {
        case Action::factoryRequested: return "factory-requested";
        case Action::created: return "created";
        case Action::factoryFailed: return "factory-failed";
        case Action::activated: return "activated";
        case Action::quiesced: return "quiesced";
        case Action::released: return "released";
        case Action::borrowedAttached: return "borrowed-attached";
        case Action::borrowedDetached: return "borrowed-detached";
        case Action::published: return "published";
        case Action::withdrawn: return "withdrawn";
        case Action::generationBegan: return "generation-began";
        case Action::firstWaveform: return "first-waveform";
        case Action::playable: return "playable";
        case Action::coreReady: return "core-ready";
        case Action::missingTerminal: return "missing-terminal";
        case Action::moduleEnableRequested: return "module-enable-requested";
        case Action::moduleOffRequested: return "module-off-requested";
        case Action::moduleRemoveRequested: return "module-remove-requested";
        case Action::previewRequested: return "preview-requested";
        case Action::previewOffRequested: return "preview-off-requested";
        case Action::taskStarted: return "task-started";
        case Action::cancelRequested: return "cancel-requested";
        case Action::taskCompleted: return "task-completed";
        case Action::callbackPostRequested: return "callback-post-requested";
        case Action::callbackInvoked: return "callback-invoked";
        case Action::callbackDiscarded: return "callback-discarded";
        case Action::taskSucceeded: return "task-succeeded";
        case Action::taskCancelled: return "task-cancelled";
        case Action::taskRejected: return "task-rejected";
        case Action::operationBound: return "operation-bound";
    }
    throw std::runtime_error("unknown lifecycle action");
}
inline const char* kindName(Kind kind)
{
    switch (kind)
    {
        case Kind::ownedView: return "owned-view";
        case Kind::borrowedView: return "borrowed-view";
        case Kind::processor: return "processor";
        case Kind::previewPublication: return "preview-publication";
        case Kind::meterConsumer: return "meter-consumer";
        case Kind::meterState: return "meter-state";
        case Kind::marker: return "marker";
        case Kind::workspaceHost: return "workspace-host";
        case Kind::renderSession: return "render-session";
        case Kind::previewState: return "preview-state";
        case Kind::applyTask: return "apply-task";
        case Kind::completionCallback: return "completion-callback";
        case Kind::ambisonicFieldConsumer: return "ambisonic-field-consumer";
        case Kind::ambisonicFieldAnalyzer: return "ambisonic-field-analyzer";
        case Kind::ambisonicPlaybackConsumer: return "ambisonic-playback-consumer";
        case Kind::ambisonicStageGraph: return "ambisonic-stage-graph";
        case Kind::spatialConsumer: return "spatial-consumer";
        case Kind::spatialWorker: return "spatial-worker";
        case Kind::spatialState: return "spatial-state";
        case Kind::spatialVisualWorker: return "spatial-visual-worker";
        case Kind::viewTimer: return "view-timer";
        case Kind::optionalMediaOperation: return "optional-media-operation";
        case Kind::packedDecodeWork: return "packed-decode-work";
        case Kind::boundedDecodeProcess: return "bounded-decode-process";
        case Kind::recoveryWriterState: return "recovery-writer-state";
        case Kind::recoveryWriterWorker: return "recovery-writer-worker";
        case Kind::deviceOperationState: return "device-operation-state";
        case Kind::deviceWarmupCallback: return "device-warmup-callback";
        case Kind::deviceWarmupWorker: return "device-warmup-worker";
    }
    throw std::runtime_error("unknown lifecycle resource kind");
}
inline bool isBackendKind(Kind kind) noexcept
{
    return kind == Kind::optionalMediaOperation || kind == Kind::packedDecodeWork || kind == Kind::boundedDecodeProcess
        || kind == Kind::recoveryWriterState || kind == Kind::recoveryWriterWorker || kind == Kind::deviceOperationState
        || kind == Kind::deviceWarmupCallback || kind == Kind::deviceWarmupWorker;
}
inline bool terminal(Action action) noexcept
{
    return action == Action::released || action == Action::factoryFailed
        || action == Action::borrowedDetached || action == Action::withdrawn;
}
inline bool hasUuid(const Event& event) noexcept
{ return std::any_of(event.operationId.begin(), event.operationId.end(), [](auto byte) { return byte != 0; }); }
inline juce::String operationId(const Event& event)
{ return hasUuid(event) ? juce::Uuid(event.operationId.data()).toString() : juce::String(); }
inline std::vector<Event> readEvents(const OptionalLifecycleObserver& observer)
{
    std::vector<Event> result;
    for (const auto& slot : observer.events())
        if (const auto committed = slot.committedSequence.load(std::memory_order_acquire); committed != 0)
        {
            if (committed != slot.event.sequence) throw std::runtime_error("uncommitted lifecycle slot");
            result.push_back(slot.event);
        }
    return result;
}
struct Resource
{
    Event first;
    bool created = false, active = false, ended = false;
    std::uint64_t started = 0, completed = 0, outcome = 0, rejected = 0;
    std::uint64_t post = 0, invoked = 0, discarded = 0, bound = 0, terminalSequence = 0;
    std::array<std::uint8_t, 16> operation {};
};
using Resources = std::map<std::uint64_t, Resource>;
inline bool threadTask(Kind kind) noexcept
{
    return kind == Kind::applyTask || kind == Kind::spatialWorker || kind == Kind::spatialVisualWorker
        || kind == Kind::packedDecodeWork || kind == Kind::recoveryWriterWorker || kind == Kind::deviceWarmupWorker;
}
// No product control, waits, timing changes or passed-in outcome. This consumes
// the committed channel after the actual owners have been destroyed.
inline Resources validateResourceStream(const OptionalLifecycleObserver& observer, const std::vector<Event>& raw,
                                       std::uint64_t session = 1)
{
    if (observer.failureFlags() || observer.outstandingResourceCount() || raw.size() != observer.attemptedEventCount())
        throw std::runtime_error("lifecycle channel overflow/failure/uncommitted/open resource");
    Resources resources;
    std::uint64_t sequence = 0;
    for (const auto& event : raw)
    {
        (void) actionName(event.action); (void) kindName(event.kind);
        if (event.sequence != ++sequence || event.session != session) throw std::runtime_error("incomplete lifecycle order/session");
        if (event.action != Action::operationBound && hasUuid(event)) throw std::runtime_error("UUID outside explicit binding event");
        if (event.resourceId == 0)
        {
            const bool milestone = event.action == Action::generationBegan || event.action == Action::firstWaveform
                || event.action == Action::playable || event.action == Action::coreReady;
            const bool command = event.action == Action::moduleEnableRequested || event.action == Action::moduleOffRequested
                || event.action == Action::moduleRemoveRequested || event.action == Action::previewRequested
                || event.action == Action::previewOffRequested;
            if (event.kind != Kind::marker || event.relatedResource || (!milestone && !command)
                || (milestone && event.owner != Owner::coreFile)) throw std::runtime_error("invalid non-owning marker");
            continue;
        }
        if (event.action == Action::factoryRequested || event.action == Action::borrowedAttached || event.action == Action::published)
        {
            if (event.kind == Kind::marker || !resources.emplace(event.resourceId, Resource{event}).second)
                throw std::runtime_error("duplicate acquisition or resource marker");
            if (isBackendKind(event.kind))
            {
                const auto expected = event.kind == Kind::packedDecodeWork ? Owner::vgmstream
                    : event.kind == Kind::optionalMediaOperation || event.kind == Kind::boundedDecodeProcess ? Owner::optionalMedia
                    : event.kind == Kind::recoveryWriterState || event.kind == Kind::recoveryWriterWorker ? Owner::recovery : Owner::audioOutput;
                if (event.action != Action::factoryRequested
                    || (event.owner != expected && !(event.kind == Kind::boundedDecodeProcess && event.owner == Owner::diagnostics)))
                    throw std::runtime_error("backend kind/owner acquisition mismatch");
            }
            if (event.relatedResource)
            {
                const auto found = resources.find(event.relatedResource);
                if (found == resources.end() || event.relatedResource == event.resourceId) throw std::runtime_error("missing real parent acquisition");
                const auto& parent = found->second.first;
                if (parent.session != event.session) throw std::runtime_error("parent session mismatch");
                if (!isBackendKind(event.kind) && parent.owner != event.owner) throw std::runtime_error("product parent owner mismatch");
                if (event.kind == Kind::completionCallback && (parent.kind != Kind::applyTask || parent.generation != event.generation))
                    throw std::runtime_error("completion lacks its actual task/generation");
                if (event.kind == Kind::spatialState && parent.kind != Kind::spatialWorker) throw std::runtime_error("Spatial State parent mismatch");
                if (isBackendKind(event.kind))
                {
                    bool correct = false;
                    if (event.kind == Kind::packedDecodeWork) correct = parent.kind == Kind::optionalMediaOperation;
                    if (event.kind == Kind::boundedDecodeProcess) correct = event.owner == Owner::optionalMedia
                        && (parent.kind == Kind::packedDecodeWork || parent.kind == Kind::optionalMediaOperation);
                    if (event.kind == Kind::recoveryWriterWorker) correct = parent.kind == Kind::recoveryWriterState;
                    if (event.kind == Kind::deviceWarmupWorker) correct = parent.kind == Kind::deviceWarmupCallback;
                    if (!correct || parent.generation != event.generation) throw std::runtime_error("backend parent/context mismatch");
                }
            }
            else if (event.kind == Kind::completionCallback || event.kind == Kind::spatialState
                     || event.kind == Kind::recoveryWriterWorker || event.kind == Kind::deviceWarmupWorker)
                throw std::runtime_error("resource lacks required real parent");
            continue;
        }
        auto found = resources.find(event.resourceId);
        if (found == resources.end()) throw std::runtime_error("event without acquisition");
        auto& resource = found->second;
        const auto& first = resource.first;
        if (resource.ended || first.session != event.session || first.generation != event.generation
            || first.owner != event.owner || first.kind != event.kind || first.relatedResource != event.relatedResource)
            throw std::runtime_error("changed resource identity or post-terminal event");
        const bool task = threadTask(first.kind), deviceState = first.kind == Kind::deviceOperationState;
        const bool callback = first.kind == Kind::completionCallback || first.kind == Kind::deviceWarmupCallback;
        switch (event.action)
        {
            case Action::created:
                if (resource.created || first.action != Action::factoryRequested) throw std::runtime_error("invalid created");
                resource.created = true; break;
            case Action::activated:
                if (isBackendKind(first.kind) || resource.active || (first.action == Action::factoryRequested && !resource.created))
                    throw std::runtime_error("invalid activation");
                resource.active = true; break;
            case Action::quiesced:
                if (!resource.active) throw std::runtime_error("quiesced without active");
                resource.active = false; break;
            case Action::taskStarted:
                if (first.action != Action::factoryRequested || (!task && !deviceState)
                    || resource.started || resource.completed || resource.rejected)
                    throw std::runtime_error("invalid/duplicate task start");
                resource.started = event.sequence; break;
            case Action::taskCompleted:
                if ((!task && !deviceState) || !resource.started || resource.completed || resource.rejected)
                    throw std::runtime_error("completion without unique actual start");
                resource.completed = event.sequence; break;
            case Action::cancelRequested:
                if (first.action != Action::factoryRequested || (!task && !deviceState))
                    throw std::runtime_error("cancel note on unrelated kind");
                break; // Requests can repeat and can follow task completion.
            case Action::taskSucceeded: case Action::taskCancelled: case Action::taskRejected:
                if (deviceState && event.action == Action::taskRejected)
                {
                    if (!resource.created || resource.started || resource.completed || resource.rejected)
                        throw std::runtime_error("device rejection after launch or duplicate");
                    resource.rejected = event.sequence;
                }
                else
                {
                    if (first.kind != Kind::applyTask || !resource.started || resource.completed || resource.outcome)
                        throw std::runtime_error("invalid Apply outcome");
                    resource.outcome = event.sequence;
                }
                break;
            case Action::callbackPostRequested:
                if (!callback || !resource.created || resource.post || resource.invoked || resource.discarded)
                    throw std::runtime_error("invalid callback post");
                resource.post = event.sequence; break;
            case Action::callbackInvoked:
                if (!callback || !resource.post || resource.invoked || resource.discarded) throw std::runtime_error("invalid callback invoke");
                resource.invoked = event.sequence; break;
            case Action::callbackDiscarded:
                // Discard is destruction without invocation. An invoked timer
                // that declines initialization after cancel is still invoked.
                if (!callback || !resource.post || resource.discarded || resource.invoked)
                    throw std::runtime_error("invalid callback discard");
                resource.discarded = event.sequence; break;
            case Action::operationBound:
                if ((!deviceState && first.kind != Kind::deviceWarmupWorker) || resource.bound || !hasUuid(event)
                    || (deviceState && (!resource.created || resource.started || resource.rejected))
                    || (first.kind == Kind::deviceWarmupWorker && (!resource.started || resource.completed)))
                    throw std::runtime_error("invalid actual device operation binding");
                resource.bound = event.sequence; resource.operation = event.operationId; break;
            case Action::factoryFailed:
                if (resource.created || first.action != Action::factoryRequested || resource.started || resource.invoked || resource.bound)
                    throw std::runtime_error("invalid failed construction");
                resource.ended = true; resource.terminalSequence = event.sequence; break;
            case Action::released:
                if (!resource.created || resource.active || first.action != Action::factoryRequested) throw std::runtime_error("invalid release");
                if (task && (resource.completed <= resource.started || !resource.started)) throw std::runtime_error("task released before actual completion");
                if (first.kind == Kind::applyTask && !(resource.started < resource.outcome && resource.outcome < resource.completed))
                    throw std::runtime_error("Apply lacks unique outcome within task");
                if (callback && (!resource.post || (static_cast<bool>(resource.invoked) == static_cast<bool>(resource.discarded))))
                    throw std::runtime_error("completion lacks unique invoked/discarded delivery");
                if (deviceState && (!resource.bound || (static_cast<bool>(resource.rejected) == static_cast<bool>(resource.completed))))
                    throw std::runtime_error("device State lacks UUID and either rejection or actual resource drain");
                resource.ended = true; resource.terminalSequence = event.sequence; break;
            case Action::borrowedDetached:
                if (resource.active || first.action != Action::borrowedAttached) throw std::runtime_error("invalid borrowed detach");
                resource.ended = true; resource.terminalSequence = event.sequence; break;
            case Action::withdrawn:
                if (resource.active || first.action != Action::published) throw std::runtime_error("invalid publication withdrawal");
                resource.ended = true; resource.terminalSequence = event.sequence; break;
            default: throw std::runtime_error("unexpected resource action");
        }
    }
    for (const auto& [id, resource] : resources)
    {
        juce::ignoreUnused(id);
        if (!resource.ended) throw std::runtime_error("resource lacks real terminal");
    }
    return resources;
}
inline juce::var eventJson(const Event& event)
{
    auto* row = new juce::DynamicObject;
    row->setProperty("sequence", static_cast<juce::int64>(event.sequence));
    row->setProperty("resource", static_cast<juce::int64>(event.resourceId));
    row->setProperty("session", static_cast<juce::int64>(event.session));
    row->setProperty("generation", static_cast<juce::int64>(event.generation));
    row->setProperty("related_resource", static_cast<juce::int64>(event.relatedResource));
    row->setProperty("owner", optionalLifecycleOwnerId(event.owner));
    row->setProperty("kind", kindName(event.kind)); row->setProperty("action", actionName(event.action));
    row->setProperty("operation_id", operationId(event));
    return juce::var(row);
}
}
