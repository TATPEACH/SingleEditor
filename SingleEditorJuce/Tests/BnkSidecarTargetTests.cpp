#include "Bnk/BnkParser.h"
#include "Bnk/BnkWemPlaybackBackend.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

void runBnkSidecarTargetTests()
{
    using namespace singleeditor;
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-SidecarTargets-" + juce::Uuid().toString());
    check(root.createDirectory().wasOk(), "create private target fixture");
    std::cout << "Sidecar target fixture: " << root.getFullPathName() << '\n';
    const auto bank = root.getChildFile("targets.bnk");
    juce::MemoryOutputStream header;
    header.write("BKHD", 4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    check(bank.replaceWithData(header.getData(), header.getDataSize()), "write actual target bank");
    const auto events = root.getChildFile("Events"), actors = root.getChildFile("Actor-Mixer Hierarchy");
    check(events.createDirectory().wasOk() && actors.createDirectory().wasOk(), "create actual event and actor work units");
    const auto wave = root.getChildFile("media.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(wave.createOutputStream());
        auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(1).withBitsPerSample(16);
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
        juce::AudioBuffer<float> pcm(1, 64);
        for (int i = 0; i < 64; ++i) pcm.setSample(0, i, 0.25f);
        check(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 64), "write actual complete source PCM");
    }
    const auto mediaXml = [](int id)
    {
        return "<File Id=\"" + juce::String(id) + "\"><ShortName>media.wav</ShortName><Path>media.wav</Path></File>";
    };
    const auto writeBankEvent = [&](const juce::String& attributes, bool twoMedia = false)
    {
        check(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
            "<SoundBanksInfo><SoundBanks><SoundBank><Path>targets.bnk</Path><IncludedEvents><Event Id=\"10\" "
            + attributes + "><ReferencedStreamedFiles>" + mediaXml(100) + (twoMedia ? mediaXml(101) : juce::String())
            + "</ReferencedStreamedFiles></Event></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"), "write actual bank event/media metadata");
    };
    const auto stateInfo = [](const char* gain)
    {
        return juce::String("<StateInfo><StateGroupList><StateGroupInfo><StateGroupRef Name=\"Weather\"/>"
            "</StateGroupInfo></StateGroupList><CustomStateList><CustomState><StateRef ID=\"{state-rain}\" Name=\"Rain\"/>"
            "<CustomState><PropertyList><Property Name=\"Volume\" Value=\"") + gain
            + "\"/></PropertyList></CustomState></CustomState></CustomStateList></StateInfo>";
    };
    check(actors.getChildFile("actors.wwu").replaceWithText(
        "<WwiseDocument><Sound ID=\"{target-a}\" Name=\"A\">" + stateInfo("-6")
        + "</Sound><Sound ID=\"{target-b}\" Name=\"B\">" + stateInfo("3") + "</Sound></WwiseDocument>"), "write actual distinct target states");
    writeBankEvent("GUID=\"{event}\" Name=\"Multi\"");
    const auto action = [](const char* id, const char* name)
    {
        return juce::String("<Action><ReferenceList><Reference Name=\"Target\"><ObjectRef ID=\"") + id
            + "\" Name=\"" + name + "\"/></Reference></ReferenceList></Action>";
    };
    check(events.getChildFile("events.wwu").replaceWithText(
        "<WwiseDocument><Event ID=\"{event}\" Name=\"Multi\"><ChildrenList>"
        + action("{target-a}", "A") + action("{target-b}", "B")
        + "</ChildrenList></Event></WwiseDocument>"), "write actual multi-action event");
    auto result = BnkParser::parse(bank, root);
    check(result && result.sidecar && result.state->events.size() == 1, "actual core and optional multi-action metadata parsed");
    check(result.state->events[0].targetObjectId.isEmpty() && result.state->events[0].stateModifiers.empty(),
        "distinct action targets must not publish the first target's global state gain");
    const auto unavailable = [](const BnkParser::Result& parsed)
    {
        return parsed && parsed.sidecar && parsed.state->events.size() == 1
            && parsed.state->events[0].stateTargetScope == BnkDocument::EventDescriptor::StateTargetScope::unavailable
            && parsed.state->events[0].targetObjectId.isEmpty() && parsed.state->events[0].stateModifiers.empty();
    };
    check(unavailable(result) && result.state->info.diagnostics.joinIntoString(" ").contains("media remains accessible"),
        "ambiguous target scope is retained with an explicit diagnostic");
    BnkDocument document;
    check(document.adoptParsedState(result.state), "adopt actual parsed ambiguous bank in document");
    document.selectEventById(10);
    check(document.getSelectedEventId() == 10 && document.getAvailableStateValues().empty(),
        "actual document exposes no wrong State choice for ambiguous owner scope");
    document.clear();
    BnkWemPlaybackBackend backend;
    backend.setMediaResolveMode(BnkMediaResolveMode::sourceMedia);
    BnkPlaybackContext rain; rain.stateValueToken = "{state-rain}";
    check(!backend.preparePreview(*result.state, result.state->events[0], rain).isValid(),
        "explicit ambiguous State request cannot render the wrong first-target gain");
    const auto checkPcm = [](const BnkPreparedPlayback& preview, float gain)
    {
        check(preview.isValid() && preview.state->audioBuffer->getNumSamples() == 64, "actual complete PCM preview survives");
        for (int i = 0; i < 64; ++i)
            check(std::abs(preview.state->audioBuffer->getSample(0, i) - 0.25f * gain) < 0.00001f,
                "every actual preview sample uses only the proven target gain");
    };
    checkPcm(backend.preparePreview(*result.state, result.state->events[0], {}), 1.0f);

    const auto writeEvents = [&](const juce::String& body)
    { check(events.getChildFile("events.wwu").replaceWithText("<WwiseDocument>" + body + "</WwiseDocument>"), "replace own event fixture"); };
    const auto event = [&](const juce::String& attributes, const juce::String& actions)
    { return "<Event " + attributes + "><ChildrenList>" + actions + "</ChildrenList></Event>"; };
    const auto writeActors = [&](const juce::String& body)
    { check(actors.getChildFile("actors.wwu").replaceWithText("<WwiseDocument>" + body + "</WwiseDocument>"), "replace own actor fixture"); };
    const auto actor = [&](const char* id, const char* name, const char* volume)
    { return juce::String("<Sound ID=\"") + id + "\" Name=\"" + name + "\">" + (volume ? stateInfo(volume) : juce::String()) + "</Sound>"; };
    const auto read = [&] { return BnkParser::parse(bank, root); };
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-b}", "B") + action("{target-a}", "A")));
    check(unavailable(read()), "reversing action order does not change ambiguous scope");
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A") + action("{target-a}", "A")));
    result = read();
    check(result && result.sidecar && result.state->events[0].stateModifiers.size() == 1
        && result.state->events[0].targetObjectId == "{target-a}", "repeated references to the same actual object resolve once");
    checkPcm(backend.preparePreview(*result.state, result.state->events[0], rain), std::pow(10.0f, -6.0f / 20.0f));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A") + "<Action/>"));
    check(unavailable(read()), "an additional unresolved action cannot be silently skipped");

    writeActors(actor("{target-a}", "A", "-6") + actor("{target-b}", "{target-a}", "3"));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A")));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}", "an object name cannot overwrite another object's GUID index");
    checkPcm(backend.preparePreview(*result.state, result.state->events[0], rain), std::pow(10.0f, -6.0f / 20.0f));
    writeActors(actor("{target-a}", "Duplicate", "-6") + actor("{target-b}", "Duplicate", "3"));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "Duplicate")));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}", "exact GUID remains usable among duplicate names");
    checkPcm(backend.preparePreview(*result.state, result.state->events[0], rain), std::pow(10.0f, -6.0f / 20.0f));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("", "Duplicate")));
    check(unavailable(read()), "name-only reference cannot choose an arbitrary duplicate");
    writeActors(actor("{target-a}", "Duplicate", "-6") + actor("{target-b}", "Duplicate", nullptr));
    check(unavailable(read()), "objects without StateInfo still participate in name ambiguity");
    writeActors(actor("{target-a}", "A", "-6") + actor("{target-b}", "B", "3"));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{missing}", "A")));
    check(unavailable(read()), "a missing explicit GUID never falls back to a matching display name");
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("", "A")));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}", "unique legacy name-only object reference remains supported");

    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A"))
        + event("ID=\"{other-event}\" Name=\"Multi\"", action("{target-b}", "B")));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}", "XML GUID selects the correct same-name WWU event");
    writeBankEvent("Name=\"Multi\"");
    check(unavailable(read()), "ambiguous event name cannot select the last definition");
    writeEvents(event("ID=\"{event}\" ShortID=\"10\" Name=\"Multi\"", action("{target-a}", "A"))
        + event("ID=\"{other-event}\" ShortID=\"11\" Name=\"Multi\"", action("{target-b}", "B")));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}", "declared event ShortID resolves the correct authoring identity");
    writeEvents(event("ID=\"{event}\" ShortID=\"11\" Name=\"Multi\"", action("{target-a}", "A")));
    check(unavailable(read()), "a contradictory declared ShortID cannot authorize name fallback");
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A"))
        + event("ID=\"{event}\" Name=\"Other\"", action("{target-b}", "B")));
    result = read();
    check(result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
        && result.state->events.empty(), "duplicate authoring GUID rejects the whole metadata transaction");

    writeEvents({});
    writeActors("<WorkUnit ID=\"{unit}\" Name=\"Unit\"><ChildrenList>"
        + actor("{target-a}", "A", "-6") + actor("{target-b}", "B", "3") + "</ChildrenList></WorkUnit>");
    writeBankEvent("Name=\"Multi\"", true);
    const auto txt = root.getChildFile("targets.txt");
    const auto row = [](int id, const char* owner)
    { return juce::String(id) + "\tmedia\tmedia.wav\t" + owner + "\n"; };
    const char* aPath = "\\Actor-Mixer Hierarchy\\Unit\\A";
    const char* bPath = "\\Actor-Mixer Hierarchy\\Unit\\B";
    const auto writeTxt = [&](const juce::String& rows)
    { check(txt.replaceWithText("In Memory Audio\tID\tName\tSource\tOwner\n" + rows), "write actual text media owner relations"); };
    writeTxt(row(100, aPath) + row(101, bPath));
    check(unavailable(read()), "multiple text media owners never select the first owner");
    writeTxt(row(100, aPath));
    check(unavailable(read()), "missing owner coverage cannot turn a partial relation into event-wide authority");
    writeTxt(row(100, aPath) + row(101, aPath));
    result = read();
    check(result && result.sidecar && result.state->events[0].targetObjectId == "{target-a}"
        && result.state->events[0].stateModifiers.size() == 1, "complete identical text owner paths resolve the exact WWU object");
    checkPcm(backend.preparePreview(*result.state, result.state->events[0], rain), std::pow(10.0f, -6.0f / 20.0f));
    writeTxt(row(100, aPath) + row(100, bPath) + row(101, aPath));
    result = read();
    check(result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
        && result.state->events.empty(), "conflicting text owners of one media ID reject instead of overwriting");
    writeTxt(row(100, aPath) + row(101, aPath));
    writeEvents(event("ID=\"{event}\" Name=\"Multi\"", action("{target-a}", "A") + action("{target-b}", "B")));
    check(unavailable(read()), "text fallback cannot bypass a known multi-target authoring boundary");
    result.inputLease.reset(); result.state.reset();
    check(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-SidecarTargets-")
        && root.deleteRecursively(), "remove only successful owned target fixture");
    std::cout << "BnkSidecarTargetTests passed (actual multi-action/identity/TXT owner scope, document choices and full PCM gain)\n";
}
