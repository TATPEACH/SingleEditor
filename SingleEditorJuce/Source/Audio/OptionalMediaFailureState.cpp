#include "Audio/OptionalMediaFailureState.h"
#include <limits>

namespace singleeditor::media
{
bool OptionalMediaFailureState::valid(const Key& key)
{
    const auto hash = [](const juce::String& value)
    { return value.length() == 64 && value.containsOnly("0123456789abcdef"); };
    return hash(key.sourceFingerprint) && hash(key.policyFingerprint)
        && key.module >= Module::packedAudio && key.module <= Module::sidecar
        && key.profile >= Profile::conservative && key.profile <= Profile::expanded;
}
bool OptionalMediaFailureState::budget(Reason reason)
{
    return reason == Reason::deadline || reason == Reason::memoryQuota || reason == Reason::outputQuota;
}
OptionalMediaFailureState::Admission OptionalMediaFailureState::begin(
    const Key& key, Intent intent, const std::optional<Key>& expandedFrom)
{
    const std::scoped_lock lock(mutex);
    if (!valid(key) || intent < Intent::initial || intent > Intent::expandBudget
        || (intent != Intent::expandBudget && expandedFrom))
        return { Denied::invalidKey, {} };
    Entry* origin = nullptr;
    if (intent == Intent::expandBudget)
    {
        if (!expandedFrom || !valid(*expandedFrom) || key.profile != Profile::expanded
            || expandedFrom->profile != Profile::conservative || key.module != expandedFrom->module
            || key.sourceFingerprint != expandedFrom->sourceFingerprint
            || key.policyFingerprint == expandedFrom->policyFingerprint)
            return { Denied::expansionNotAllowed, {} };
        const auto found = entries.find(*expandedFrom);
        if (found == entries.end() || found->second.inFlight != 0 || !found->second.unresolved
            || !budget(found->second.lastReason) || found->second.expandedGranted)
            return { Denied::expansionNotAllowed, {} };
        origin = &found->second;
    }
    auto found = entries.find(key);
    if (found == entries.end())
    {
        if (intent == Intent::retryOnce || (key.profile == Profile::expanded && origin == nullptr))
            return { Denied::retryRequired, {} };
        if (entries.size() >= 128) return { Denied::stateBudget, {} };
        found = entries.emplace(key, Entry()).first;
    }
    auto& entry = found->second;
    if (entry.inFlight != 0) return { Denied::busy, {} };
    if (entry.failures >= 2) return { Denied::circuitOpen, {} };
    if (intent == Intent::initial && entry.unresolved) return { Denied::retryRequired, {} };
    if (intent == Intent::expandBudget && entry.failures != 0) return { Denied::expansionNotAllowed, {} };
    if (intent == Intent::retryOnce && (!entry.unresolved || entry.retryUsed)) return { Denied::retryUsed, {} };
    if (sequence == std::numeric_limits<uint64_t>::max()) return { Denied::stateBudget, {} };
    if (origin != nullptr) origin->expandedGranted = true;
    if (intent == Intent::retryOnce) entry.retryUsed = true;
    entry.inFlight = ++sequence;
    entry.intent = intent;
    entry.expandedFrom = expandedFrom;
    return { Denied::none, Ticket { key, sequence, intent, expandedFrom } };
}
bool OptionalMediaFailureState::succeed(const Ticket& ticket)
{
    const std::scoped_lock lock(mutex);
    const auto found = entries.find(ticket.key);
    if (ticket.sequence == 0 || found == entries.end() || found->second.inFlight != ticket.sequence) return false;
    found->second.inFlight = 0;
    found->second.unresolved = false;
    found->second.expandedFrom.reset();
    // Success does not create a retained log, nor erase earlier failure counts.
    if (found->second.failures == 0 && ticket.key.profile == Profile::conservative)
        entries.erase(found);
    return true;
}
bool OptionalMediaFailureState::fail(const Ticket& ticket, const Failure& failure)
{
    const std::scoped_lock lock(mutex);
    const auto found = entries.find(ticket.key);
    if (ticket.sequence == 0 || found == entries.end() || found->second.inFlight != ticket.sequence
        || failure.reason < Reason::decoderExit || failure.reason > Reason::ioFailure
        || failure.stage < Stage::admission || failure.stage > Stage::cleanup) return false;
    auto& entry = found->second;
    entry.inFlight = 0; entry.unresolved = true; ++entry.failures; entry.lastReason = failure.reason;
    entry.expandedFrom.reset();
    latest = Capsule { ticket.key, failure, ticket.sequence };
    return true;
}
bool OptionalMediaFailureState::cancel(const Ticket& ticket)
{
    const std::scoped_lock lock(mutex);
    const auto found = entries.find(ticket.key);
    if (ticket.sequence == 0 || found == entries.end() || found->second.inFlight != ticket.sequence) return false;
    found->second.inFlight = 0;
    const auto intent = found->second.intent;
    if (intent == Intent::retryOnce) found->second.retryUsed = false;
    if (found->second.expandedFrom)
    {
        const auto origin = entries.find(*found->second.expandedFrom);
        if (origin != entries.end()) origin->second.expandedGranted = false;
    }
    found->second.expandedFrom.reset();
    if (found->second.failures == 0
        && (ticket.key.profile == Profile::conservative || intent == Intent::expandBudget)) entries.erase(found);
    return true;
}
std::optional<OptionalMediaFailureState::Capsule> OptionalMediaFailureState::latestFailure() const
{
    const std::scoped_lock lock(mutex); return latest;
}
size_t OptionalMediaFailureState::retainedEntries() const
{
    const std::scoped_lock lock(mutex); return entries.size();
}
bool OptionalMediaFailureState::tryLatestFailure(std::optional<Capsule>& result) const
{
    result.reset();
    const std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    result = latest; return true;
}
void OptionalMediaFailureState::noteUnadmittedFailure(const Failure& failure)
{
    if (failure.operationId.isNull()) return;
    const std::scoped_lock lock(mutex);
    latest = Capsule { {}, failure, 0 }; // No valid capability key or attempt ticket.
}
OptionalMediaFailureState::View OptionalMediaFailureState::inspect(const Key& key) const
{
    const std::scoped_lock lock(mutex);
    const auto found = entries.find(key);
    if (found == entries.end()) return {};
    const auto& e = found->second;
    return { e.failures, e.inFlight != 0,
        e.inFlight == 0 && e.unresolved && e.failures < 2 && !e.retryUsed,
        e.inFlight == 0 && e.unresolved && key.profile == Profile::conservative
            && budget(e.lastReason) && !e.expandedGranted,
        e.failures >= 2 };
}
}
