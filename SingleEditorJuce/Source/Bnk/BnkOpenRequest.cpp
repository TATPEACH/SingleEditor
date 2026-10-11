#include "Bnk/BnkOpenRequest.h"
#include <juce_cryptography/juce_cryptography.h>

namespace singleeditor
{
namespace
{
juce::String hash(const juce::String& value)
{ return juce::SHA256(value.toRawUTF8(), value.getNumBytesAsUTF8()).toHexString(); }
void field(juce::String& output, const juce::String& value)
{ output += juce::String(value.length()) + ":" + value; }
juce::String identity(const media::SourceFileWitness& witness)
{
    juce::String result;
    field(result, witness.volumeId); field(result, witness.fileId);
    field(result, juce::String(witness.sizeBytes)); field(result, juce::String(witness.modifiedTicks));
    field(result, juce::String(witness.changedTicks));
    return result;
}
juce::String catalogIdentity(const juce::String& bank, const BoundedSidecarInput::Catalog& catalog)
{
    juce::String result("bnk-sidecar-inputs/v1"); field(result, bank);
    field(result, catalog.projectRoot.getFullPathName().toLowerCase());
    field(result, juce::String(static_cast<int>(catalog.failure)));
    for (const auto& query : catalog.queries)
    {
        field(result, query.root.getFullPathName().toLowerCase()); field(result, query.wildcard);
        field(result, query.recursive ? "recursive" : "direct");
        for (const auto& file : query.matches) field(result, file.getFullPathName().toLowerCase());
    }
    std::vector<std::pair<juce::String, juce::String>> inputs;
    for (const auto& input : catalog.inputs)
        inputs.emplace_back(input->witness()->file.getFullPathName().toLowerCase(), identity(*input->witness()));
    std::sort(inputs.begin(), inputs.end());
    for (const auto& input : inputs) { field(result, input.first); field(result, input.second); }
    return hash(result);
}
}

BnkOpenRequest::BnkOpenRequest(std::shared_ptr<Policy> owner, juce::File file, juce::File project,
    Capability capability, Policy::Intent action, std::optional<Policy::Key> previous)
    : session(std::move(owner)), bank(std::move(file)), root(std::move(project)), target(capability), intent(action), reviewed(std::move(previous)) {}
BnkOpenRequest::~BnkOpenRequest() { abandon(); }
BnkOpenRequest::Slot& BnkOpenRequest::slot(Capability capability) { return capability == Capability::core ? core : sidecar; }
const BnkOpenRequest::Slot& BnkOpenRequest::slot(Capability capability) const { return capability == Capability::core ? core : sidecar; }
BnkParser::Profile BnkOpenRequest::profile(Capability capability) const
{
    return capability == target && (intent == Policy::Intent::expandBudget || (reviewed && reviewed->profile == Policy::Profile::expanded))
        ? BnkParser::Profile::expanded : BnkParser::Profile::conservative;
}
juce::String BnkOpenRequest::policy(Capability capability, BnkParser::Profile budget) const
{
    const bool expanded = budget == BnkParser::Profile::expanded;
    return hash(capability == Capability::core
        ? juce::String("bnk-index/v1/file8GiB-chunks4096-table16MiB-objects65536-events4096-actions4096/") + (expanded ? "120s" : "30s")
        : juce::String("bnk-sidecar/v2/discovery5s-inputs128-file4MiB-read32MiB-derivedText32MiB-nodes32768-depth64/") + (expanded ? "20s" : "5s"));
}
bool BnkOpenRequest::admit(Capability capability, const juce::String& fingerprint)
{
    const std::scoped_lock lock(mutex);
    auto& current = slot(capability);
    if (!session) { current.report.denied = Policy::Denied::invalidKey; current.report.terminal = true; return false; }
    if (wasCancelled() || current.ticket || current.report.terminal) return false;
    Policy::Key key { fingerprint, policy(capability, profile(capability)), capability == Capability::core ? Policy::Module::bank : Policy::Module::sidecar,
        profile(capability) == BnkParser::Profile::expanded ? Policy::Profile::expanded : Policy::Profile::conservative };
    current.report.key = key;
    const auto action = capability == target ? intent : Policy::Intent::initial;
    if (capability == target && action != Policy::Intent::initial
        && (!reviewed || reviewed->sourceFingerprint != key.sourceFingerprint || reviewed->module != key.module
            || reviewed->policyFingerprint != policy(capability, action == Policy::Intent::expandBudget ? BnkParser::Profile::conservative : profile(capability))))
    { current.report.denied = Policy::Denied::invalidKey; current.report.terminal = true; return false; }
    const auto admission = session->begin(key, action, action == Policy::Intent::expandBudget ? reviewed : std::nullopt);
    current.report.denied = admission.denied; current.ticket = admission.ticket;
    current.report.terminal = !current.ticket;
    current.report.circuitOpen = session->inspect(key).circuitOpen;
    return current.ticket.has_value();
}
void BnkOpenRequest::failed(Capability capability, BnkParser::Failure reason, Policy::Stage stage)
{
    const std::scoped_lock lock(mutex);
    auto& current = slot(capability);
    if (current.report.terminal) return;
    current.report.failure = reason;
    current.report.stage = current.ticket ? stage : Policy::Stage::admission;
    current.report.terminal = true;
    if (!session) return;
    if (wasCancelled() || reason == BnkParser::Failure::cancelled)
    { if (current.ticket) session->cancel(*current.ticket); }
    else
    {
        Policy::Failure failure; failure.stage = current.report.stage;
        failure.operationId = diagnosticState->value.operationId;
        failure.source = capability == Capability::core ? Policy::FailureSource::bankIndex : Policy::FailureSource::bankSidecar;
        failure.detailCode = static_cast<int64_t>(reason);
        switch (reason)
        {
            // Discovery is a fixed admission ceiling, not an expanded parser budget.
            case BnkParser::Failure::deadline: failure.reason = stage == Policy::Stage::admission ? Policy::Reason::parserBounds : Policy::Reason::deadline; break;
            // This policy expands time only. Allocation failure cannot promise
            // a larger memory allowance on an otherwise identical retry.
            case BnkParser::Failure::memoryQuota: failure.reason = Policy::Reason::parserBounds; break;
            case BnkParser::Failure::ioFailure: failure.reason = Policy::Reason::ioFailure; break;
            default: failure.reason = Policy::Reason::parserBounds; break;
        }
        if (current.ticket) session->fail(*current.ticket, failure);
        else session->noteUnadmittedFailure(failure);
    }
    if (current.report.key) current.report.circuitOpen = session->inspect(*current.report.key).circuitOpen;
    current.ticket.reset();
}
BnkParser::Result BnkOpenRequest::prepare(BnkParser::Checkpoint checkpoint)
{
    if (started.exchange(true, std::memory_order_acq_rel)) return {};
    BnkParser::Hooks hooks;
    juce::String coreIdentity;
    hooks.admitCore = [&](const media::SourceFileWitness& input)
    { coreIdentity = identity(input); return admit(Capability::core, hash(coreIdentity)); };
    hooks.sidecar = [&](BnkDocument::State& state, const std::function<bool()>& progress)
    {
        BnkSidecarParser::Result result;
        try
        {
        catalog = BoundedSidecarInput::freeze(bank, root, progress);
        if (!admit(Capability::sidecar, catalogIdentity(coreIdentity, *catalog)))
        {
            result.admissionDenied = true;
            state.info.diagnostics.add("Optional sidecar work was not admitted by its session policy; proven core bank data was kept.");
            catalog.reset(); return result;
        }
        if (catalog->failure != BnkParser::Failure::none)
        {
            result.failure = catalog->failure;
            failed(Capability::sidecar, result.failure, Policy::Stage::admission);
            state.info.diagnostics.add(BoundedSidecarInput::describe(result.failure));
            catalog.reset(); return result;
        }
        result = BnkSidecarParser::enrichStateFromSidecars(bank, state, root, progress, profile(Capability::sidecar), catalog);
        if (!result)
        { failed(Capability::sidecar, result.failure, Policy::Stage::parse); catalog.reset(); }
        return result;
        }
        catch (const std::bad_alloc&) { result.failure = BnkParser::Failure::memoryQuota; }
        catch (...) { result.failure = BnkParser::Failure::ioFailure; }
        failed(Capability::sidecar, result.failure, Policy::Stage::admission);
        catalog.reset();
        try { state.info.diagnostics.add(BoundedSidecarInput::describe(result.failure)); } catch (...) {}
        return result;
    };
    auto result = BnkParser::parse(bank, root, [&](const BnkParser::Progress& progress)
        { return !wasCancelled() && (!checkpoint || checkpoint(progress)); }, profile(Capability::core), hooks);
    if (!result)
    {
        if (!result.admissionDenied) failed(Capability::core, result.failure, Policy::Stage::parse);
        abandon(); catalog.reset();
    }
    else preparedState = result.state;
    return result;
}
bool BnkOpenRequest::canPublish(const BnkParser::Result& result) const
{
    if (wasCancelled() || !result || !result.inputLease || result.inputLease->recheck().failed()) return false;
    if (preparedState.lock() != result.state || result.state->info.file != bank) return false;
    if (catalog) for (const auto& input : catalog->inputs) if (input->recheck().failed()) return false;
    const auto coreReport = report(Capability::core);
    return !coreReport.terminal && coreReport.key.has_value()
        && coreReport.key->sourceFingerprint == hash(identity(*result.inputLease->witness()));
}
void BnkOpenRequest::succeed()
{
    const std::scoped_lock lock(mutex);
    for (auto* current : { &core, &sidecar })
        if (current->ticket && session)
        {
            current->report.succeeded = !wasCancelled() && session->succeed(*current->ticket);
            if (!current->report.succeeded) session->cancel(*current->ticket);
            current->report.terminal = true; current->ticket.reset();
        }
    catalog.reset();
}
void BnkOpenRequest::failPublication()
{
    failed(Capability::core, BnkParser::Failure::ioFailure, Policy::Stage::admission);
    abandon(); catalog.reset();
}
void BnkOpenRequest::abandon()
{
    const std::scoped_lock lock(mutex);
    diagnosticState->value.abandoned = true;
    for (auto* current : { &core, &sidecar })
        if (current->ticket && session)
        { session->cancel(*current->ticket); current->ticket.reset(); current->report.terminal = true; }
    catalog.reset(); preparedState.reset();
}
BnkOpenRequest::Report BnkOpenRequest::report(Capability capability) const
{ const std::scoped_lock lock(mutex); return slot(capability).report; }
std::optional<BnkOpenRequest::DiagnosticReport> BnkOpenRequest::DiagnosticState::snapshot() const
{
    const std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    auto copy = value;
    copy.started = started.load(std::memory_order_acquire);
    copy.cancelled = cancellation.load(std::memory_order_acquire);
    return copy;
}
BnkOpenRequest::Policy::View BnkOpenRequest::policyView(Capability capability) const
{ const auto value = report(capability); return session && value.key ? session->inspect(*value.key) : Policy::View{}; }
}
