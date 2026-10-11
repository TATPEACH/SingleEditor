#include "Audio/OptionalMediaFailureState.h"
#include "Audio/OptionalMediaOperation.h"
#include <iostream>

using State = singleeditor::media::OptionalMediaFailureState;
namespace
{
void require(bool good, const char* message)
{
    if (!good) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
State::Key key(int source = 1)
{
    return { juce::String::toHexString(source).paddedLeft('0', 64),
             juce::String::repeatedString("a", 64), State::Module::packedAudio, State::Profile::conservative };
}
}
int main()
{
    State state;
    require(state.retainedEntries() == 0 && !state.latestFailure(), "inert owner starts with no log or work");
    for (int i = 1; i < 301; ++i)
    {
        auto admitted = state.begin(key(i));
        require(admitted && state.succeed(*admitted.ticket), "successful operations require no retained global history");
    }
    require(state.retainedEntries() == 0 && !state.latestFailure(), "success retains no log or failure entry");
    const State::Failure quota { State::Reason::outputQuota, State::Stage::decode, 0, 8192, 4096, 12, true };
    auto first = state.begin(key());
    require(first && state.begin(key()).denied == State::Denied::busy, "one in-flight request per fingerprint");
    require(state.fail(*first.ticket, quota), "record first typed failure");
    require(state.begin(key()).denied == State::Denied::retryRequired, "failure never starts an automatic retry");
    auto retry = state.begin(key(), State::Intent::retryOnce);
    require(retry && !state.fail(*first.ticket, quota), "stale completion cannot alter a retry");
    require(state.fail(*retry.ticket, quota), "record explicit retry failure");
    require(state.begin(key()).denied == State::Denied::circuitOpen
            && state.begin(key(), State::Intent::retryOnce).denied == State::Denied::circuitOpen,
            "two same-fingerprint failures fuse this policy for the session");
    auto fresh = state.begin(key(2));
    require(fresh && state.cancel(*fresh.ticket), "changed identity gets one fresh attempt; cancellation is not failure");
    State restarted;
    require(static_cast<bool>(restarted.begin(key())), "application-session replacement resets the circuit");
    auto expanded = key(); expanded.profile = State::Profile::expanded;
    expanded.policyFingerprint = juce::String::repeatedString("b", 64);
    auto expansion = state.begin(expanded, State::Intent::expandBudget, key());
    require(expansion && state.cancel(*expansion.ticket), "explicit bounded expansion is permitted only after a budget failure");
    expansion = state.begin(expanded, State::Intent::expandBudget, key());
    require(expansion && state.fail(*expansion.ticket, quota), "cancelled expansion does not consume the user attempt");
    require(state.begin(expanded, State::Intent::expandBudget, key()).denied == State::Denied::expansionNotAllowed,
            "expanded profile cannot be repeatedly granted");
    auto expandedRetry = state.begin(expanded, State::Intent::retryOnce);
    require(expandedRetry && state.fail(*expandedRetry.ticket, quota), "expanded policy also gets at most one same-policy retry");
    require(state.begin(expanded).denied == State::Denied::circuitOpen, "expanded failures remain fused too");
    auto parseRequest = state.begin(key(3));
    State::Failure malformed { State::Reason::parserBounds, State::Stage::parse };
    require(parseRequest && state.fail(*parseRequest.ticket, malformed), "record structural failure");
    auto wrongExpansion = expanded; wrongExpansion.sourceFingerprint = key(3).sourceFingerprint;
    require(state.begin(wrongExpansion, State::Intent::expandBudget, key(3)).denied == State::Denied::expansionNotAllowed,
            "larger resource limits cannot bypass malformed structure");
    auto cancelledRetry = state.begin(key(3), State::Intent::retryOnce);
    require(cancelledRetry && state.cancel(*cancelledRetry.ticket), "cancel pending retry without consuming failure allowance");
    auto recovered = state.begin(key(3), State::Intent::retryOnce);
    require(recovered && state.succeed(*recovered.ticket), "successful explicit retry restores capability");
    auto later = state.begin(key(3));
    require(later && state.fail(*later.ticket, malformed)
            && state.begin(key(3)).denied == State::Denied::circuitOpen, "later success never erases the session failure count");
    require(state.latestFailure()->failure.reason == State::Reason::parserBounds, "only latest structured capsule is retained");
    State healthyExpanded;
    auto base = healthyExpanded.begin(key());
    require(base && healthyExpanded.fail(*base.ticket, quota), "budget failure before healthy expansion");
    auto grant = healthyExpanded.begin(expanded, State::Intent::expandBudget, key());
    require(grant && healthyExpanded.succeed(*grant.ticket), "expanded profile recovers successfully");
    auto normal = healthyExpanded.begin(expanded);
    require(normal && healthyExpanded.cancel(*normal.ticket) && healthyExpanded.begin(expanded),
            "cancelling a later request retains a previously granted healthy expanded profile");
    State saturated;
    for (int i = 1; i <= 128; ++i)
    {
        auto request = saturated.begin(key(i));
        require(request && saturated.fail(*request.ticket, malformed), "retain bounded failure identities");
    }
    require(saturated.retainedEntries() == 128 && saturated.begin(key(129)).denied == State::Denied::stateBudget,
            "saturation fails closed instead of evicting circuit history");
    require(!state.begin(State::Key()), "invalid fingerprint is inert");
    {
        using Operation = singleeditor::media::OptionalMediaOperation;
        auto session = std::make_shared<State>();
        const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto root = parent.getChildFile("optional-operation-" + juce::Uuid().toString());
        require(root.createDirectory().wasOk(), "create private physical admission fixture");
        const auto source = root.getChildFile("private-source.mp4"), producer = root.getChildFile("private-decoder.bin");
        require(source.replaceWithText("source") && producer.replaceWithText("producer"), "create actual source/dependency identities");
        Operation original(session);
        require(original.admitVideo(source, producer), "actual physical source gets initial admission");
        original.fail(State::Reason::outputQuota, State::Stage::decode);
        const auto reviewed = original.report().key;
        require(original.policyView().retryAvailable && original.policyView().expansionAvailable,
            "typed budget failure exposes exact read-only retry and expansion choices");
        Operation cancelledExpansion(session, State::Intent::expandBudget, reviewed);
        require(cancelledExpansion.admitVideo(source, producer), "reviewed source/dependency accepts bounded expansion");
        cancelledExpansion.requestCancel(); cancelledExpansion.succeed();
        require(cancelledExpansion.report().cancelled && original.policyView().expansionAvailable,
            "cancel at result publication does not spend the uncompleted expansion grant");
        require(producer.appendText(" changed dependency"), "replace actual producer stamp");
        Operation changedDependency(session, State::Intent::expandBudget, reviewed);
        require(!changedDependency.admitVideo(source, producer) && changedDependency.report().denied == State::Denied::invalidKey,
            "old expansion button cannot grant changed dependency identity");
        Operation fresh(session);
        require(fresh.admitVideo(source, producer), "fresh explicit open may admit a changed decoder policy identity");
        fresh.fail(State::Reason::decoderExit, State::Stage::decode);
        require(source.appendText(" changed source"), "change actual source identity");
        Operation staleRetry(session, State::Intent::retryOnce, fresh.report().key);
        require(!staleRetry.admitVideo(source, producer) && staleRetry.report().denied == State::Denied::invalidKey,
            "old retry button cannot grant changed source identity");
        const auto diagnostic = fresh.diagnosticHandle()->snapshot();
        require(diagnostic && diagnostic->failure && diagnostic->failure->operationId == fresh.report().operationId,
            "actual failure remains observable through the shared typed report used by the common exporter");
        Operation prefix(session, State::Intent::initial, {}, State::Module::packedAudio);
        require(prefix.admitPacked(source, producer), "packed module uses its own physical policy key");
        prefix.noteProgress(48000, 144000); prefix.succeed();
        require(!prefix.report().terminal && prefix.report().readerAdmitted && !prefix.report().decoderComplete,
            "reader/prefix admission cannot credit decoder completion");
        prefix.fail(State::Reason::deadline, State::Stage::decode); prefix.completeDecodedAudio();
        require(prefix.report().failure && !prefix.report().succeeded && !prefix.report().decoderComplete,
            "late completion cannot overwrite a terminal packed failure");
        Operation retryPrefix(session, State::Intent::retryOnce, prefix.report().key, State::Module::packedAudio);
        require(retryPrefix.admitPacked(source, producer), "packed explicit retry uses reviewed exact identity");
        retryPrefix.completeDecodedAudio();
        require(!retryPrefix.report().terminal && retryPrefix.report().decoderComplete && !retryPrefix.report().readerAdmitted,
            "sealed bytes still wait for actual reader admission");
        retryPrefix.requestCancel(); retryPrefix.succeed();
        require(retryPrefix.report().cancelled && prefix.policyView().retryAvailable,
            "cancel between seal and reader does not spend unfinished retry");
        Operation bank(session, State::Intent::initial, {}, State::Module::bank);
        require(bank.admitBank(source, producer, "media5-dependencies-a") && bank.continueWork(State::Stage::decode)
            && bank.remainingWallTimeMs() > 0 && bank.remainingWallTimeMs() <= 30000,
            "frozen bank selection has one bounded operation deadline");
        bank.fail(State::Reason::outputQuota, State::Stage::decode);
        const auto bankKey = bank.report().key;
        Operation wrongSelection(session, State::Intent::retryOnce, bankKey, State::Module::bank);
        require(!wrongSelection.admitBank(source, producer, "media6-dependencies-a")
            && wrongSelection.report().denied == State::Denied::invalidKey, "bank retry cannot cross a selected media identity");
        Operation wrongDependencies(session, State::Intent::expandBudget, bankKey, State::Module::bank);
        require(!wrongDependencies.admitBank(source, producer, "media5-dependencies-b"), "bank expansion cannot cross resolved dependency identity");
        Operation expandedBank(session, State::Intent::expandBudget, bankKey, State::Module::bank);
        require(expandedBank.admitBank(source, producer, "media5-dependencies-a") && expandedBank.bankLimits().wallTimeMs == 120000
            && expandedBank.bankLimits().processes == 1 && expandedBank.bankLimits().outputBytes == 4ULL * 1024 * 1024 * 1024,
            "exact bank budget ticket permits only the fixed finite expanded profile");
        expandedBank.requestCancel();
        require(!expandedBank.continueWork(State::Stage::parse) && expandedBank.report().cancelled && bank.policyView().expansionAvailable,
            "cancelled composite work cannot keep running or consume an unfinished expansion");
        Operation badScope(session, State::Intent::initial, {}, State::Module::bank);
        require(!badScope.admitBank(source, producer, {}) && badScope.report().denied == State::Denied::invalidKey,
            "bank operations require an explicit frozen selection/dependency scope");
        require(bank.diagnosticHandle()->snapshot()->module == State::Module::bank,
            "bank diagnostic module remains typed for common provider capture");
        Operation preparation(session, State::Intent::initial, {}, State::Module::bank);
        const auto entriesBeforePreparation = session->retainedEntries();
        require(preparation.beginBankPreparation() && preparation.continueWork(State::Stage::admission)
            && !preparation.continueWork(State::Stage::decode) && !preparation.report().key
            && session->retainedEntries() == entriesBeforePreparation,
            "bank resolution has a clock and cancellation, but no decoder ticket or session entry");
        juce::Thread::sleep(25);
        const auto beforeAdmission = preparation.remainingWallTimeMs();
        require(preparation.admitBank(source, producer, "resolved-actual-inputs")
            && preparation.remainingWallTimeMs() <= beforeAdmission && beforeAdmission < 30000,
            "binding actual resolved inputs does not restart the operation deadline");
        preparation.succeed();
        Operation cancelledPreparation(session, State::Intent::initial, {}, State::Module::bank);
        const auto beforeCancelled = session->retainedEntries();
        require(cancelledPreparation.beginBankPreparation(), "start another bounded bank resolution");
        cancelledPreparation.requestCancel();
        require(!cancelledPreparation.continueWork(State::Stage::admission) && cancelledPreparation.report().cancelled
            && !cancelledPreparation.admitBank(source, producer, "cancelled-resolution")
            && session->retainedEntries() == beforeCancelled,
            "cancelled resolution cannot mint a later source/decoder ticket");
        Operation invalidPreparation(session, State::Intent::initial, {}, State::Module::bank);
        require(invalidPreparation.beginBankPreparation(), "start structural admission fixture");
        invalidPreparation.fail(State::Reason::parserBounds, State::Stage::admission);
        require(invalidPreparation.report().terminal && invalidPreparation.report().failure
            && !invalidPreparation.report().key && session->retainedEntries() == beforeCancelled,
            "pre-admission structural failure is typed without inventing source authority");
        require(root.getParentDirectory() == parent && root.getFileName().startsWith("optional-operation-")
            && root.deleteRecursively(), "remove only this known private physical fixture");
    }
    std::cout << "OptionalMediaFailureStateTests passed (explicit retry, policy expansion, stale completion, cancellation, session circuit, bounded capsule)\n";
}
