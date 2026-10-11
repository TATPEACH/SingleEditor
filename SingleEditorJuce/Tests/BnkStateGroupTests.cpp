#include "Bnk/BnkParser.h"
#include "Bnk/BnkWemPlaybackBackend.h"
#include "Bnk/BnkStateSelection.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void checkGroup(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

void runBnkStateGroupTests()
{
    using namespace singleeditor;
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-StateGroups-" + juce::Uuid().toString());
    checkGroup(root.createDirectory().wasOk(), "create group fixture");
    std::cout << "State group fixture: " << root.getFullPathName() << '\n';
    const auto bank = root.getChildFile("groups.bnk");
    juce::MemoryOutputStream header;
    header.write("BKHD", 4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    checkGroup(bank.replaceWithData(header.getData(), header.getDataSize()), "write actual bank");
    const auto events = root.getChildFile("Events"), actors = root.getChildFile("Actor-Mixer Hierarchy");
    checkGroup(events.createDirectory().wasOk() && actors.createDirectory().wasOk(), "create authoring directories");
    checkGroup(events.getChildFile("events.wwu").replaceWithText(
        "<WwiseDocument><Event ID=\"{event}\" Name=\"Preview\"><ChildrenList><Action><ReferenceList>"
        "<Reference Name=\"Target\"><ObjectRef ID=\"{target}\" Name=\"Sound\"/></Reference>"
        "</ReferenceList></Action></ChildrenList></Event></WwiseDocument>"), "write event target");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> output(root.getChildFile("media.wav").createOutputStream());
        const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(1).withBitsPerSample(16);
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(output, options));
        juce::AudioBuffer<float> pcm(1, 128); for (int i=0;i<128;++i) pcm.setSample(0,i,0.25f);
        checkGroup(writer && writer->writeFromAudioSampleBuffer(pcm,0,128), "write complete source PCM");
    }
    const auto group = [](const char* id, const char* name, const char* value)
    {
        return juce::String("<StateGroup GUID=\"") + id + "\" Name=\"" + name + "\"><States>"
            "<State Id=\"1\" GUID=\"{none-" + value + "}\" Name=\"None\"/>"
            "<State Id=\"2\" GUID=\"{" + value + "}\" Name=\"Rain\"/></States></StateGroup>";
    };
    const auto writeCatalog = [&](const juce::String& groups)
    {
        checkGroup(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
            "<SoundBanksInfo><SoundBanks><SoundBank><Path>groups.bnk</Path><StateGroups>" + groups
            + "</StateGroups><IncludedEvents><Event Id=\"10\" GUID=\"{event}\" Name=\"Preview\">"
            "<ReferencedStreamedFiles><File Id=\"100\"><Path>media.wav</Path></File></ReferencedStreamedFiles>"
            "</Event></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"), "write group catalog");
    };
    const auto groupRef = [](const char* id, const char* name)
    { return juce::String("<StateGroupInfo><StateGroupRef ID=\"") + id + "\" Name=\"" + name + "\"/></StateGroupInfo>"; };
    const auto writeState = [&](const juce::String& groups, const char* stateId, const char* stateName)
    {
        checkGroup(actors.getChildFile("actors.wwu").replaceWithText(
            "<WwiseDocument><Sound ID=\"{target}\" Name=\"Sound\"><StateInfo><StateGroupList>" + groups
            + "</StateGroupList><CustomStateList><CustomState><StateRef ID=\"" + stateId + "\" Name=\"" + stateName
            + "\"/><CustomState><PropertyList><Property Name=\"Volume\" Value=\"-6\"/></PropertyList></CustomState>"
            "</CustomState></CustomStateList></StateInfo></Sound></WwiseDocument>"), "write object State reference");
    };
    writeCatalog(group("{weather}","Weather","rain-a") + group("{mood}","Mood","rain-b"));
    writeState(groupRef("{weather}","Weather") + groupRef("{mood}","Mood"), "{rain-b}", "Rain");
    auto parsed = BnkParser::parse(bank, root);
    checkGroup(parsed && parsed.sidecar && parsed.state->events.size()==1
        && parsed.state->events[0].stateModifiers.size()==1, "actual core, sidecar and State modifier parsed");
    checkGroup(parsed.state->events[0].stateModifiers[0].stateGroupName == "Mood",
        "exact State identity must select its own group, not the first StateGroupRef");
    BnkDocument document;
    BnkWemPlaybackBackend backend;
    backend.setMediaResolveMode(BnkMediaResolveMode::sourceMedia);
    const auto read = [&] { return BnkParser::parse(bank,root); };
    const auto options = [&](const BnkParser::Result& input)
    {
        checkGroup(input && input.sidecar && document.adoptParsedState(input.state), "adopt actual parsed State groups");
        const auto available = document.getAvailableStateValues(); document.clear(); return available;
    };
    const auto pcm = [&](const BnkParser::Result& input, const juce::String& selection, float gain)
    {
        BnkPlaybackContext context; context.stateValueToken=selection;
        const auto preview=backend.preparePreview(*input.state,input.state->events[0],context);
        checkGroup(preview.isValid() && preview.state->audioBuffer->getNumSamples()==128, "complete actual State PCM");
        for (int sample=0;sample<128;++sample)
            checkGroup(std::abs(preview.state->audioBuffer->getSample(0,sample)-0.25f*gain)<0.00001f,
                "every sample has only the selected exact group gain");
    };
    const auto denied = [&](const BnkParser::Result& input, const juce::String& selection)
    {
        BnkPlaybackContext context; context.stateValueToken=selection;
        checkGroup(!backend.preparePreview(*input.state,input.state->events[0],context).isValid(), "unresolved or foreign State cannot render guessed gain");
    };
    const auto unresolved = [&](const BnkParser::Result& input)
    {
        checkGroup(input && input.sidecar && input.state->events.size()==1 && !input.state->events[0].stateModifiersResolved,
            "ambiguous State reference degrades only the State capability");
        checkGroup(options(input).empty(), "unresolved reference exposes no misleading State options");
        pcm(input,{},1.0f); denied(input,"{rain-b}");
    };
    auto available=options(parsed);
    checkGroup(available.size()==2 && available[0].groupToken=="{mood}" && available[1].groupToken=="{mood}",
        "same-name value in another group does not enter actual document choices");
    const auto rainGain=std::pow(10.0f,-6.0f/20.0f);
    pcm(parsed,bnkstate::token(available[1]),rainGain); pcm(parsed,bnkstate::token(available[0]),1.0f);
    pcm(parsed,"{RAIN-B}",rainGain);
    denied(parsed,"Rain"); denied(parsed,"{rain-a}"); denied(parsed,"{missing}");
    denied(parsed,bnkstate::token(parsed.state->stateValues[1]));
    writeCatalog(group("{mood}","Mood","rain-b")+group("{weather}","Weather","rain-a"));
    parsed=read(); checkGroup(options(parsed).size()==2 && parsed.state->events[0].stateModifiers[0].stateGroupToken=="{mood}",
        "reversing catalog group order preserves exact membership");

    writeState(groupRef("{mood}","Mood"),"{missing}","Rain"); unresolved(read());
    writeState(groupRef("{missing-group}","Mood"),"{rain-b}","Rain"); unresolved(read());
    writeState(groupRef("{mood}","Mood"),"{rain-a}","Rain"); unresolved(read());
    writeState(groupRef("{mood}","Mood"),"","Rain");
    parsed=read(); available=options(parsed); checkGroup(available.size()==2,"name-only State resolves only inside its proven group");
    pcm(parsed,bnkstate::token(available[1]),rainGain);
    writeState(groupRef("{weather}","Weather")+groupRef("{mood}","Mood"),"","Rain"); unresolved(read());

    writeCatalog(group("{weather}","Same","rain-a")+group("{mood}","Same","rain-b"));
    writeState(groupRef("{mood}","Same"),"{rain-b}","Rain");
    parsed=read(); available=options(parsed); checkGroup(available.size()==2 && available[1].groupToken=="{mood}",
        "duplicate group names do not replace exact group GUID");
    pcm(parsed,bnkstate::token(available[1]),rainGain);
    writeState(groupRef("","Same"),"{rain-b}","Rain"); unresolved(read());
    writeCatalog("<StateGroup GUID=\"{empty}\" Name=\"Same\"><States/></StateGroup>"+group("{mood}","Same","rain-b"));
    unresolved(read());

    const auto rejected = [&]
    {
        const auto input=read();
        checkGroup(input && !input.sidecar && input.sidecar.failure==BoundedSidecarInput::Failure::invalidData
            && input.state->events.empty() && input.state->stateGroups.empty(), "conflicting catalog rejects private metadata and preserves core");
    };
    writeCatalog(group("{weather}","Weather","rain-b")+group("{mood}","Mood","rain-b")); rejected();
    writeCatalog(group("{mood}","Mood","rain-b")+group("{mood}","Mood","rain-c")); rejected();
    writeCatalog("<StateGroup GUID=\"{mood}\" Name=\"Mood\"><States><State Id=\"2\" GUID=\"{first}\" Name=\"First\"/>"
        "<State Id=\"2\" GUID=\"{second}\" Name=\"Second\"/></States></StateGroup>"); rejected();

    // The project-only path must preserve same-name values too; it formerly
    // deduplicated every State globally by display name.
    writeCatalog({});
    const auto statesDir=root.getChildFile("States"); checkGroup(statesDir.createDirectory().wasOk(),"create State work units");
    checkGroup(statesDir.getChildFile("states.wwu").replaceWithText(
        "<WwiseDocument><StateGroup ID=\"{weather}\" Name=\"Weather\"><ChildrenList>"
        "<State ID=\"{rain-a}\" Name=\"Rain\"/></ChildrenList></StateGroup>"
        "<StateGroup ID=\"{mood}\" Name=\"Mood\"><ChildrenList><State ID=\"{rain-b}\" Name=\"Rain\"/>"
        "</ChildrenList></StateGroup></WwiseDocument>"),"write same-name WWU State definitions");
    writeState(groupRef("{weather}","Weather")+groupRef("{mood}","Mood"),"{rain-b}","Rain");
    parsed=read(); available=options(parsed); checkGroup(parsed.state->stateValues.size()==2 && available.size()==1
        && available[0].groupToken=="{mood}","WWU catalog keeps distinct identities despite equal names");
    pcm(parsed,bnkstate::token(available[0]),rainGain);
    checkGroup(statesDir.getChildFile("states.wwu").deleteFile(),"remove owned State input before text-only scenario");

    writeState(groupRef("","Weather"),"","Rain");
    checkGroup(bank.withFileExtension("txt").replaceWithText("State\tID\tName\tGroup\n1\tNone\tWeather\n2\tRain\tWeather\n"
        "1\tNone\tMood\n2\tRain\tMood\n"),"write group-scoped TXT State IDs");
    parsed=read(); available=options(parsed); checkGroup(parsed && parsed.sidecar && available.size()==2
        && parsed.state->stateValues.size()==4 && available[1].token.isEmpty(),"TXT short IDs stay scoped and never become GUIDs");
    pcm(parsed,bnkstate::token(available[1]),rainGain);
    denied(parsed,bnkstate::token(parsed.state->stateValues[3]));
    writeState(groupRef("","Weather"),"{2}","Rain"); unresolved(read());
    parsed.inputLease.reset(); parsed.state.reset();
    checkGroup(root.getParentDirectory()==parent && root.getFileName().startsWith("SingleEditor-StateGroups-")
        && root.deleteRecursively(), "remove only released owned group fixture");
    std::cout << "BnkStateGroupTests passed (actual XML/WWU/TXT group identity, exact document/backend selection and complete PCM)\n";
}
