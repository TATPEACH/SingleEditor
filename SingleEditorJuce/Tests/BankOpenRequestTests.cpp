#include "Bnk/BnkOpenRequest.h"
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
using Request = BnkOpenRequest;
using Policy = Request::Policy;
void require(bool value, const char* message)
{ if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
}

void runBankOpenRequestTests()
{
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-BankOpen-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private bank-open request fixture");
    const auto bank = root.getChildFile("open.bnk"), xml = root.getChildFile("SoundbanksInfo.xml");
    juce::MemoryOutputStream header;
    header.write("BKHD", 4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    const auto goodXml = "<SoundBanksInfo><SoundBanks><SoundBank><Path>open.bnk</Path><IncludedEvents>"
        "<Event Id=\"17\" Name=\"actual event\"/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>";
    const auto session = std::make_shared<Policy>();
    const auto make = [&](Request::Capability target = Request::Capability::core, Policy::Intent intent = Policy::Intent::initial,
                          std::optional<Policy::Key> previous = {})
    { return std::make_shared<Request>(session, bank, root, target, intent, previous); };
    const auto commit = [&](const Request::Ptr& request, BnkParser::Result& result)
    {
        BnkDocument document;
        require(request->canPublish(result) && document.adoptParsedState(result.state), "actual bank caller publishes only the request's pinned candidate");
        request->succeed(); require(request->report(Request::Capability::core).succeeded, "core ticket succeeds only after actual adoption");
        result = {};
    };
    require(bank.replaceWithData(header.getData(), header.getDataSize()) && xml.replaceWithText(goodXml), "create actual core and XML inputs");
    {
        auto request = make(); auto result = request->prepare();
        require(result && result.state->events.size() == 1 && result.state->events[0].id == 17
            && request->policyView(Request::Capability::core).inFlight && request->policyView(Request::Capability::sidecar).inFlight,
            "both admitted real parser stages stay pending until caller adoption");
        require(!xml.appendText("x") && !bank.appendData("x", 1), "prepared metadata and core retain actual deny-write leases");
        require(request->report(Request::Capability::core).key->module == Policy::Module::bank
            && request->report(Request::Capability::sidecar).key->module == Policy::Module::sidecar,
            "core index and sidecar have independent capability keys");
        auto other = BnkParser::parse(bank, root);
        require(other && !request->canPublish(other), "a different actual parse result cannot borrow this request's publication ticket");
        other = {}; commit(request, result);
        require(xml.replaceWithText(goodXml), "adopted request releases additional metadata input leases");
    }
    require(xml.replaceWithText("<!DOCTYPE SoundBanksInfo><SoundBanksInfo/>"), "create actual rejected sidecar");
    Policy::Key sidecarKey;
    {
        auto first = make(); auto result = first->prepare();
        require(result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
            && result.sidecar.inputs.size() == 1 && result.state->events.empty(), "real sidecar failure preserves valid core with no metadata prefix");
        sidecarKey = *first->report(Request::Capability::sidecar).key;
        require(first->policyView(Request::Capability::sidecar).failures == 1, "actual sidecar failure enters its session policy");
        const auto firstId = first->diagnosticHandle()->snapshot()->operationId;
        require(!firstId.isNull() && session->latestFailure()->failure.operationId == firstId
            && session->latestFailure()->failure.source == Policy::FailureSource::bankSidecar
            && session->latestFailure()->failure.detailCode == static_cast<int64_t>(BnkParser::Failure::invalidData),
            "actual sidecar failure stores original request ID, source and typed parser outcome");
        commit(first, result);
        auto repeated = make(); result = repeated->prepare();
        require(result && result.sidecar.admissionDenied && result.sidecar.inputs.empty()
            && repeated->report(Request::Capability::sidecar).denied == Policy::Denied::retryRequired,
            "ordinary reopen cannot rerun a failed sidecar without explicit retry");
        commit(repeated, result);
        require(repeated->diagnosticHandle()->snapshot()->operationId != firstId
            && session->latestFailure()->failure.operationId == firstId,
            "refused sidecar repeat and successful core do not relabel or replace the original failure");
        auto retry = make(Request::Capability::sidecar, Policy::Intent::retryOnce, sidecarKey); result = retry->prepare();
        require(result && result.sidecar.failure == BnkParser::Failure::invalidData && retry->policyView(Request::Capability::sidecar).circuitOpen,
            "one explicit actual metadata retry opens the independent fuse after its second failure");
        commit(retry, result);
        const auto retryId = retry->diagnosticHandle()->snapshot()->operationId;
        require(retryId != firstId && session->latestFailure()->failure.operationId == retryId
            && retry->diagnosticHandle()->snapshot()->sidecar.circuitOpen,
            "actual retry owns its new failure and recorded fuse state");
        auto fused = make(); result = fused->prepare();
        require(result && fused->report(Request::Capability::sidecar).denied == Policy::Denied::circuitOpen && result.state->events.empty(),
            "fused sidecar keeps the proven bank core usable without parsing metadata again");
        commit(fused, result);
        require(session->latestFailure()->failure.operationId == retryId,
            "fused refusal preserves the last actual failure rather than inventing a new one");
    }
    require(xml.replaceWithText(goodXml), "change actual sidecar physical version");
    {
        auto staleRetry = make(Request::Capability::sidecar, Policy::Intent::retryOnce, sidecarKey);
        auto result = staleRetry->prepare();
        require(result && staleRetry->report(Request::Capability::sidecar).denied == Policy::Denied::invalidKey,
            "reviewed retry cannot authorize a changed metadata input set");
        commit(staleRetry, result);
        auto fresh = make(); result = fresh->prepare();
        require(result && result.sidecar && result.state->events.size() == 1
            && fresh->report(Request::Capability::sidecar).key->sourceFingerprint != sidecarKey.sourceFingerprint,
            "changed actual metadata receives a fresh capability identity and parses normally");
        commit(fresh, result);
    }
    require(bank.replaceWithText("invalid-core-bytes"), "create malformed actual bank core");
    Policy::Key coreKey;
    {
        auto first = make(); auto result = first->prepare(); coreKey = *first->report(Request::Capability::core).key;
        require(!result && first->policyView(Request::Capability::core).failures == 1
            && !first->report(Request::Capability::sidecar).key, "actual core corruption fails whole open without sidecar admission");
        auto repeated = make(); size_t bodySteps = 0;
        result = repeated->prepare([&](const auto& progress) { if (progress.stage == BnkParser::Stage::coreRead) ++bodySteps; return true; });
        require(!result && result.admissionDenied && bodySteps == 0
            && repeated->report(Request::Capability::core).denied == Policy::Denied::retryRequired,
            "core gate refuses automatic repeat before reading any table body");
        auto retry = make(Request::Capability::core, Policy::Intent::retryOnce, coreKey); result = retry->prepare();
        require(!result && retry->policyView(Request::Capability::core).circuitOpen, "actual repeated core parse failure opens its own fuse");
    }
    require(bank.replaceWithData(header.getData(), header.getDataSize()), "restore a new actual core version");
    {
        auto frozen = BoundedSidecarInput::freeze(bank, root);
        require(frozen && frozen->failure == BnkParser::Failure::none, "freeze actual candidate inputs before body parsing");
        const auto switches = root.getChildFile("Switches");
        require(switches.createDirectory().wasOk() && switches.getChildFile("late.wwu").replaceWithText(
            "<Root><SwitchGroup Name=\"Late group\"><ChildrenList><Switch ID=\"late\" Name=\"Late value\"/></ChildrenList></SwitchGroup></Root>"),
            "add a real WWU after the input set was frozen");
        BnkDocument::State before;
        const auto bounded = BnkSidecarParser::enrichStateFromSidecars(bank, before, root, {}, BnkParser::Profile::conservative, frozen);
        require(bounded && before.switchValues.empty(), "frozen directory query never admits a late WWU into the original parse");
        frozen.reset();
        auto fresh = make(); auto after = fresh->prepare();
        require(after && after.state->switchValues.size() == 1 && after.state->switchValues[0].name == "Late value",
            "a fresh real input snapshot includes the newly added WWU");
        commit(fresh, after);
    }
    {
        auto request = make(); bool reached = false;
        auto result = request->prepare([&](const auto& progress)
        { if (progress.stage == BnkParser::Stage::sidecar) { reached = true; request->requestCancel(); } return true; });
        require(reached && !result && request->wasCancelled() && request->policyView(Request::Capability::core).failures == 0,
            "whole cancellation after proven core drains admission without inventing a failure");
        auto pending = make(); result = pending->prepare(); require(pending->canPublish(result), "real candidate reaches publication boundary");
        pending->requestCancel(); require(!pending->canPublish(result), "late cancellation fences the completed candidate");
        pending->abandon(); result = {};
    }
    {
        auto owner = std::make_shared<Policy>();
        auto request = std::make_shared<Request>(owner, bank, root);
        const auto handle = request->diagnosticHandle();
        std::weak_ptr<Request> weakRequest = request;
        std::weak_ptr<Policy> weakOwner = owner;
        auto prepared = request->prepare();
        require(prepared && owner->retainedEntries() > 0, "prepare real owner before observation lifetime check");
        prepared = {}; request.reset(); owner.reset();
        require(weakRequest.expired() && weakOwner.expired() && handle->snapshot()->abandoned
            && handle->snapshot()->core.terminal && !handle->snapshot()->core.succeeded,
            "passive bank handle retains only bounded outcome; request destructor cancels and session releases");
        require(bank.appendData("x", 1), "observation does not retain source lease after work drains");
    }
    {
        auto owner = std::make_shared<Policy>();
        auto missing = std::make_shared<Request>(owner, root.getChildFile("not-present.bnk"), root);
        const auto result = missing->prepare();
        std::optional<Policy::Capsule> captured;
        require(!result && owner->retainedEntries() == 0 && owner->tryLatestFailure(captured) && captured
            && captured->attempt == 0 && captured->failure.operationId == missing->diagnosticHandle()->snapshot()->operationId
            && captured->failure.source == Policy::FailureSource::bankIndex,
            "real pre-admission input failure is observable without creating a policy entry or retry ticket");
    }
    require(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-BankOpen-") && root.deleteRecursively(),
        "remove only this test's private input tree after all leases release");
    std::cout << "BankOpenRequestTests passed (actual core/sidecar input admission, separate retry/fuse, changed-input fences, physical leases and publication cancellation)\n";
}
