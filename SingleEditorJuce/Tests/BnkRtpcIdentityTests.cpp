#include "Bnk/BnkParser.h"
#include <cstdlib>
#include <iostream>

namespace
{
void checkRtpc(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

void runBnkRtpcIdentityTests()
{
    using namespace singleeditor;
    const auto parent=juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root=parent.getChildFile("SingleEditor-RtpcIdentity-"+juce::Uuid().toString());
    checkRtpc(root.createDirectory().wasOk(),"create RTPC fixture");
    std::cout << "RTPC identity fixture: " << root.getFullPathName() << '\n';
    const auto bank=root.getChildFile("rtpc.bnk");
    juce::MemoryOutputStream header; header.write("BKHD",4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    checkRtpc(bank.replaceWithData(header.getData(),header.getDataSize()),"write real BKHD bank");
    const auto events=root.getChildFile("Events"), actors=root.getChildFile("Actor-Mixer Hierarchy");
    checkRtpc(events.createDirectory().wasOk() && actors.createDirectory().wasOk(),"create authoring inputs");
    const auto parameter=[](const char* guid, int id, const char* name, int max)
    {
        return juce::String("<GameParameter Id=\"")+juce::String(id)+"\" GUID=\""+guid+"\" Name=\""+name
            +"\" Min=\"0\" Max=\""+juce::String(max)+"\" InitialValue=\"0\"/>";
    };
    const auto writeCatalog=[&](const juce::String& parameters)
    {
        checkRtpc(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
            "<SoundBanksInfo><SoundBanks><SoundBank><Path>rtpc.bnk</Path><GameParameters>"+parameters
            +"</GameParameters><IncludedEvents><Event Id=\"10\" GUID=\"{event-a}\" Name=\"A\"/>"
             "<Event Id=\"20\" GUID=\"{event-b}\" Name=\"B\"/><Event Id=\"30\" GUID=\"{event-c}\" Name=\"C\"/>"
             "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),"write actual parameter catalog");
    };
    const auto event=[](const char* id, const char* name, const char* object)
    {
        return juce::String("<Event ID=\"")+id+"\" Name=\""+name+"\"><ChildrenList><Action><ReferenceList>"
            "<Reference Name=\"Target\"><ObjectRef ID=\""+object+"\" Name=\"Voice\"/></Reference>"
            "</ReferenceList></Action></ChildrenList></Event>";
    };
    checkRtpc(events.getChildFile("events.wwu").replaceWithText("<WwiseDocument>"+event("{event-a}","A","{object-a}")
        +event("{event-b}","B","{object-b}")+event("{event-c}","C","{object-c}")+"</WwiseDocument>"),"write actual target references");
    const auto curve=[](const char* token, const char* name, double y)
    {
        return juce::String("<PropertyList><Property Name=\"Volume\"><RTPCList><RTPC><ReferenceList>"
            "<Reference Name=\"ControlInput\"><ObjectRef ID=\"")+token+"\" Name=\""+name+"\"/></Reference>"
            "</ReferenceList><Curve><PointList><Point><XPos>0</XPos><YPos>"+juce::String(y)
            +"</YPos></Point></PointList></Curve></RTPC></RTPCList></Property></PropertyList>";
    };
    const auto object=[](const char* id,const char* name,const juce::String& properties)
    { return juce::String("<Sound ID=\"")+id+"\" Name=\""+name+"\">"+properties+"</Sound>"; };
    const auto writeObjects=[&](const juce::String& body)
    { checkRtpc(actors.getChildFile("actors.wwu").replaceWithText("<WwiseDocument>"+body+"</WwiseDocument>"),"write actual RTPC curves"); };
    writeCatalog(parameter("{parameter-a}",1,"Speed",10)+parameter("{parameter-b}",2,"Speed",20));
    writeObjects(object("{object-a}","Voice",curve("{parameter-b}","Speed",-6.0))
        +object("{object-b}","Voice",{})+object("{object-c}","Voice extra",{}));
    auto parsed=BnkParser::parse(bank,root);
    checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==2 && parsed.state->events.size()==3,
        "actual core, two parameter definitions, event targets and RTPC curve parsed");
    checkRtpc(parsed.state->rtpcs[0].bindings.empty() && parsed.state->rtpcs[1].bindings.size()==1,
        "explicit RTPC GUID must not attach its curve to an earlier equal-name parameter");
    BnkDocument document;
    const auto read=[&] { return BnkParser::parse(bank,root); };
    const auto available=[&](const BnkParser::Result& input,uint32_t eventId)
    {
        checkRtpc(input && input.sidecar && document.adoptParsedState(input.state),"adopt actual RTPC metadata");
        document.selectEventById(eventId); auto result=document.getAvailableRtpcs();
        checkRtpc(document.hasRtpcMetadataForSelectedEvent()==!result.empty(),"metadata presence query agrees without copying the curve graph");
        document.clear(); return result;
    };
    auto selected=available(parsed,10);
    checkRtpc(selected.size()==1 && selected[0].token=="{parameter-b}" && selected[0].maxValue==20
        && selected[0].hasDefinition && selected[0].explicitRangeFields==7,"exact selected parameter retains its own declared range");
    checkRtpc(available(parsed,20).empty() && available(parsed,30).empty(),"equal object names and path substrings cannot authorize bindings or all-parameter fallback");
    checkRtpc(BnkDocument::hasRtpcMetadataForEvent(*parsed.state,parsed.state->events[0])
        && !BnkDocument::hasRtpcMetadataForEvent(*parsed.state,parsed.state->events[1])
        && !BnkDocument::hasRtpcMetadataForEvent(*parsed.state,parsed.state->events[2]),"actual event badge predicate uses exact target identity");
    checkRtpc(document.adoptParsedState(parsed.state),"adopt for no-selection check"); document.selectEventById(std::nullopt);
    checkRtpc(document.getAvailableRtpcs().empty(),"no selected event has no implied global parameter authority"); document.clear();

    writeObjects(object("{object-a}","Voice",curve("{missing}","Speed",-6.0))+object("{object-b}","Voice",{})+object("{object-c}","Voice extra",{}));
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==3
        && !parsed.state->rtpcs.back().hasDefinition && parsed.state->rtpcs.back().explicitRangeFields==0
        && parsed.state->rtpcs.back().bindings.size()==1 && available(parsed,10).empty(),"missing explicit GUID stays reference-only and cannot inherit same-name ranges");
    writeObjects(object("{object-a}","Voice",curve("","Speed",-6.0)));
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==2
        && parsed.state->rtpcs[0].bindings.empty() && parsed.state->rtpcs[1].bindings.empty(),"ambiguous legacy parameter name binds neither declaration");
    writeCatalog(parameter("{parameter-a}",1,"Other",10)+parameter("{parameter-b}",2,"Speed",20));
    parsed=read(); checkRtpc(available(parsed,10).size()==1 && available(parsed,10)[0].token=="{parameter-b}","unambiguous name-only reference resolves its declared parameter");
    writeCatalog(parameter("",2,"Speed",20));
    writeObjects(object("{object-a}","Voice",curve("{2}","Speed",-6.0)));
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==2 && available(parsed,10).empty(),"numeric ShortID cannot masquerade as an authoring GUID");

    writeCatalog(parameter("{parameter-a}",1,"Speed",10)+parameter("{parameter-b}",2,"Speed",20));
    writeObjects(object("{object-a}","Voice",curve("{PARAMETER-B}","Speed",-6.0))
        +object("{object-b}","Voice",curve("{parameter-b}","Speed",3.0))+object("{object-c}","Voice extra",{}));
    parsed=read(); selected=available(parsed,10);
    checkRtpc(parsed.state->rtpcs[1].bindings.size()==2 && selected.size()==1 && selected[0].bindings.size()==1
        && selected[0].bindings[0].objectId=="{object-a}" && available(parsed,20)[0].bindings[0].objectId=="{object-b}",
        "shared parameter returns only the selected object's real curve");
    const auto rejected=[&]
    {
        const auto input=read();
        checkRtpc(input && !input.sidecar && input.sidecar.failure==BoundedSidecarInput::Failure::invalidData
            && input.state->rtpcs.empty() && input.state->events.empty() && input.state->info.bankId==7,
            "conflicting RTPC input rejects private metadata while preserving actual core");
    };
    writeCatalog(parameter("{parameter-a}",1,"Speed",10)+parameter("{parameter-b}",1,"Speed",20)); rejected();
    writeCatalog(parameter("{parameter-a}",1,"Speed",10)+parameter("{parameter-a}",2,"Speed",10)); rejected();
    writeCatalog(parameter("{parameter-a}",1,"Speed",10)+parameter("{parameter-a}",1,"Speed",20)); rejected();
    writeCatalog(parameter("{parameter-b}",2,"Speed",20)+parameter("{parameter-b}",2,"Speed",20));
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==1,"identical repeated definitions merge without changing authority");

    const auto parametersDir=root.getChildFile("Game Parameters"); checkRtpc(parametersDir.createDirectory().wasOk(),"create parameter WWU directory");
    const auto parameterFile=parametersDir.getChildFile("parameters.wwu");
    checkRtpc(parameterFile.replaceWithText("<WwiseDocument><GameParameter ID=\"{parameter-b}\" ShortID=\"2\" Name=\"Speed\"/></WwiseDocument>"),"write definition with omitted range fields");
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==1 && parsed.state->rtpcs[0].maxValue==20,
        "omitted WWU range fields do not overwrite or contradict explicit generated metadata");
    checkRtpc(parameterFile.replaceWithText("<WwiseDocument><GameParameter ID=\"{parameter-b}\" ShortID=\"2\" Name=\"Speed\" Max=\"21\"/></WwiseDocument>"),"write conflicting explicit range"); rejected();
    checkRtpc(parameterFile.replaceWithText("<WwiseDocument><GameParameter ID=\"{parameter-c}\" ShortID=\"3\" Name=\"Speed\" Max=\"30\"/></WwiseDocument>"),"write distinct equal-name WWU declaration");
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs.size()==2,"WWU declaration with another GUID survives an equal generated name");
    checkRtpc(parameterFile.replaceWithText("<WwiseDocument/>"),"remove only own extra parameter definition");

    auto duplicated=curve("{parameter-b}","Speed",-6.0);
    const auto extra=curve("{parameter-b}","Speed",3.0).fromFirstOccurrenceOf("<RTPC>",true,false).upToFirstOccurrenceOf("</RTPC>",true,false);
    writeObjects(object("{object-a}","Voice",duplicated.replace("</RTPCList>",extra+"</RTPCList>"))); rejected();
    const auto same=duplicated.fromFirstOccurrenceOf("<RTPC>",true,false).upToFirstOccurrenceOf("</RTPC>",true,false);
    writeObjects(object("{object-a}","Voice",duplicated.replace("</RTPCList>",same+"</RTPCList>")));
    parsed=read(); checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs[0].bindings.size()==1,"identical duplicate curve is stored once");
    writeObjects(object("{object-a}","Voice",duplicated.replace("</ReferenceList>",
        "<Reference Name=\"ControlInput\"><ObjectRef ID=\"{parameter-b}\" Name=\"Speed\"/></Reference></ReferenceList>"))); rejected();

    writeObjects(object("{object-a}","Voice",curve("{parameter-b}","Speed",-6.0)));
    parsed=read(); writeCatalog(parameter("{parameter-b}",2,"Speed",25));
    auto refresh=BnkSidecarParser::enrichStateFromSidecars(bank,*parsed.state,root);
    checkRtpc(refresh && parsed.state->rtpcs.size()==1 && parsed.state->rtpcs[0].maxValue==25
        && parsed.state->rtpcs[0].bindings.size()==1,"valid refresh replaces old definitions without duplicating old bindings");
    writeCatalog(parameter("{parameter-b}",2,"Speed",25).replace("Max=\"25\"","Max=\"25suffix\""));
    refresh=BnkSidecarParser::enrichStateFromSidecars(bank,*parsed.state,root);
    checkRtpc(!refresh && parsed.state->rtpcs[0].maxValue==25 && parsed.state->rtpcs[0].bindings.size()==1,
        "failed refresh preserves exact prior parameter and real curve");
    writeCatalog(parameter("{parameter-b}",2,"Speed",25));
    const auto onePoint=juce::String("<Point><XPos>0</XPos><YPos>-6</YPos></Point>");
    const auto large=duplicated.upToFirstOccurrenceOf("<PointList>",true,false)
        +juce::String::repeatedString(onePoint,8192)+duplicated.fromLastOccurrenceOf("</PointList>",true,false);
    writeObjects(object("{object-a}","Voice",large)); parsed=read();
    checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs[0].bindings[0].points.size()==8192
        && parsed.state->rtpcs[0].bindings[0].points.back().y==-6.0,"large actual finite curve retains every point within structural budgets");

    writeObjects({});
    for (int file=0;file<8;++file)
    {
        juce::String body="<WwiseDocument>";
        for (int index=0;index<512;++index)
        {
            const auto id="bulk-"+juce::String(file*512+index);
            body+=object(id.toRawUTF8(),id.toRawUTF8(),duplicated);
        }
        body+="</WwiseDocument>";
        checkRtpc(actors.getChildFile("bulk-"+juce::String(file)+".wwu").replaceWithText(body),"write bounded cross-file binding corpus");
    }
    parsed=read();
    checkRtpc(parsed && parsed.sidecar && parsed.state->rtpcs[0].bindings.size()==4096,"all4096 actual cross-file bindings are admitted without quadratic duplicate scanning");
    checkRtpc(actors.getChildFile("bulk-8.wwu").replaceWithText("<WwiseDocument>"+object("overflow","Overflow",duplicated)+"</WwiseDocument>"),"add binding4097");
    {
        const auto over=read(); checkRtpc(over && !over.sidecar && over.sidecar.failure==BoundedSidecarInput::Failure::hardLimit
            && over.state->rtpcs.empty(),"binding4097 fails the aggregate insertion budget before metadata publication");
    }
    parsed.inputLease.reset(); parsed.state.reset();
    checkRtpc(root.getParentDirectory()==parent && root.getFileName().startsWith("SingleEditor-RtpcIdentity-")
        && root.deleteRecursively(),"remove only released owned RTPC fixture");
    std::cout << "BnkRtpcIdentityTests passed (actual parameter/binding identity, event scope, private refresh, large curves and4096/4097 aggregate boundary)\n";
}
