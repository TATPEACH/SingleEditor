#include "Bnk/BnkParser.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
void checkPath(bool value,const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

void runBnkDerivedPathTests()
{
    using namespace singleeditor;
    const auto parent=juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root=parent.getChildFile("SingleEditor-DerivedPaths-"+juce::Uuid().toString());
    checkPath(root.createDirectory().wasOk(),"create bounded derived-path fixture");
    std::cout << "Derived path fixture: " << root.getFullPathName() << '\n';
    const auto bank=root.getChildFile("paths.bnk"), actors=root.getChildFile("Actor-Mixer Hierarchy"), events=root.getChildFile("Events");
    checkPath(actors.createDirectory().wasOk() && events.createDirectory().wasOk(),"create owned authoring directories");
    juce::MemoryOutputStream header; header.write("BKHD",4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    checkPath(bank.replaceWithData(header.getData(),header.getDataSize()),"write actual core bank");
    const auto writeCatalog=[&](bool includeEvent)
    { checkPath(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
        "<SoundBanksInfo><SoundBanks><SoundBank><Path>paths.bnk</Path><GameParameters>"
        "<GameParameter Id=\"1\" GUID=\"parameter\" Name=\"Amount\"/></GameParameters><IncludedEvents>"
        +juce::String(includeEvent?"<Event Id=\"10\" GUID=\"event\" Name=\"Paths\"/>":"")
        +"</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),"write actual parameter/event catalog"); };
    writeCatalog(true);
    checkPath(events.getChildFile("event.wwu").replaceWithText(
        "<WwiseDocument><Event ID=\"event\" Name=\"Paths\"><ChildrenList><Action><ReferenceList>"
        "<Reference Name=\"Target\"><ObjectRef ID=\"object-0\"/></Reference></ReferenceList></Action>"
        "</ChildrenList></Event></WwiseDocument>"),"write actual event target");
    const auto writeHierarchy=[&](const juce::String& ancestor,int childrenPerFile,bool curves,bool folder=false)
    {
        int64_t inputBytes=0;
        for (int file=0;file<8;++file)
        {
            juce::String body="<WwiseDocument><WorkUnit Name=\""+ancestor+"\"><ChildrenList>";
            if (folder) body+="<Folder Name=\"Folder\"><ChildrenList>";
            for (int child=0;child<childrenPerFile;++child)
            {
                const auto id=juce::String(file*childrenPerFile+child);
                body+="<Sound ID=\"object-"+id+"\" Name=\"Sound-"+id+"\">";
                if (curves) body+="<PropertyList><Property Name=\"Volume\"><RTPCList>"
                    "<RTPC><ReferenceList><Reference Name=\"ControlInput\"><ObjectRef ID=\"parameter\"/></Reference></ReferenceList>"
                    "<Curve><PointList><Point><XPos>0</XPos><YPos>-6</YPos></Point></PointList></Curve>"
                    "</RTPC></RTPCList></Property></PropertyList>";
                body+="</Sound>";
            }
            if (folder) body+="</ChildrenList></Folder>";
            body+="</ChildrenList></WorkUnit></WwiseDocument>";
            const auto filePath=actors.getChildFile(juce::String(file)+".wwu");
            checkPath(filePath.replaceWithText(body),"write bounded actual amplification input");
            inputBytes+=filePath.getSize();
        }
        return inputBytes;
    };
    const auto inputBytes=writeHierarchy(juce::String::repeatedString("x",8192),512,true);
    auto parsed=BnkParser::parse(bank,root);
    checkPath(parsed && parsed.state->info.bankId==7,"actual core bank survives path metadata admission");
    int64_t publishedPathBytes=0; size_t bindings=0;
    for (const auto& parameter:parsed.state->rtpcs) for (const auto& binding:parameter.bindings)
    { publishedPathBytes+=static_cast<int64_t>(binding.objectPath.getNumBytesAsUTF8()); ++bindings; }
    std::cout << "Derived paths input_bytes=" << inputBytes << " published_path_bytes=" << publishedPathBytes
        << " bindings=" << bindings << " sidecar_ok=" << static_cast<bool>(parsed.sidecar) << '\n';
    checkPath(!parsed.sidecar && parsed.sidecar.failure==BnkParser::Failure::hardLimit
        && parsed.state->rtpcs.empty() && parsed.state->events.empty(),
        "replicated authoring paths must hit a derived allocation budget before optional publication");

    auto expanded=BnkParser::parse(bank,root,{},BnkParser::Profile::expanded);
    checkPath(expanded && !expanded.sidecar && expanded.sidecar.failure==BnkParser::Failure::hardLimit,
        "expanded wall-time profile cannot bypass fixed derived-text budget");
    expanded.inputLease.reset(); expanded.state.reset();

    // This hierarchy fits its first path traversal. Adding real Event lookup
    // consumes the same transaction budget, rather than a fresh per-pass quota.
    writeHierarchy(juce::String::repeatedString("x",4096),64,false);
    writeCatalog(false); parsed=BnkParser::parse(bank,root);
    checkPath(parsed && parsed.sidecar && parsed.state->events.empty(),"bounded hierarchy-only first pass remains supported");
    writeCatalog(true); parsed=BnkParser::parse(bank,root);
    checkPath(parsed && !parsed.sidecar && parsed.sidecar.failure==BnkParser::Failure::hardLimit,
        "actual Event/Object index traversal shares the prior pass's derived-text budget");

    const auto unicode=juce::String::charToString(0x8def)+juce::String::charToString(0x5f84);
    const auto normalAncestor=juce::String::repeatedString(unicode,32);
    writeHierarchy(normalAncestor,16,true,true); parsed=BnkParser::parse(bank,root);
    checkPath(parsed && parsed.sidecar && parsed.state->rtpcs.size()==1 && parsed.state->rtpcs[0].bindings.size()==128
        && parsed.state->events[0].targetObjectId=="object-0","ordinary Unicode hierarchy retains every binding and exact target");
    const auto expectedPath="\\Actor-Mixer Hierarchy\\"+normalAncestor+"\\Folder\\Sound-0";
    checkPath(parsed.state->rtpcs[0].bindings[0].objectPath==expectedPath,
        "RTPC and authoring traversal include the same real Folder hierarchy without losing Unicode");
    const auto priorBindings=parsed.state->rtpcs[0].bindings;
    writeHierarchy(juce::String::repeatedString("x",8192),512,true);
    const auto failedRefresh=BnkSidecarParser::enrichStateFromSidecars(bank,*parsed.state,root);
    checkPath(!failedRefresh && failedRefresh.failure==BnkParser::Failure::hardLimit
        && parsed.state->rtpcs[0].bindings.size()==priorBindings.size()
        && parsed.state->rtpcs[0].bindings[0].objectPath==priorBindings[0].objectPath
        && parsed.state->events[0].targetObjectId=="object-0","budget failure preserves the caller's actual prior metadata, not a partial prefix");
    int checkpoints=0;
    const auto cancelled=BnkSidecarParser::enrichStateFromSidecars(bank,*parsed.state,root,[&]{return ++checkpoints<20;});
    checkPath(!cancelled && cancelled.failure==BnkParser::Failure::cancelled
        && parsed.state->rtpcs[0].bindings.size()==128,"cancelled bounded traversal retains cancellation and prior metadata");
    {
        BoundedSidecarInput hostileCapacity;
        checkPath(!BoundedSidecarInput::admitDerivedText(std::numeric_limits<size_t>::max())
            && hostileCapacity.failureReason()==BoundedSidecarInput::Failure::hardLimit,
            "hostile capacity arithmetic rejects before multiplication or allocation");
    }
    writeHierarchy(normalAncestor,16,true,true);
    const auto refreshed=BnkSidecarParser::enrichStateFromSidecars(bank,*parsed.state,root);
    checkPath(refreshed && parsed.state->rtpcs[0].bindings.size()==128
        && parsed.state->rtpcs[0].bindings[0].objectPath==expectedPath,"new optional transaction gets its own finite budget and can recover");
    parsed.inputLease.reset(); parsed.state.reset();
    checkPath(root.getParentDirectory()==parent && root.getFileName().startsWith("SingleEditor-DerivedPaths-")
        && root.deleteRecursively(),"remove only successful private derived-path fixture after releasing leases");
    std::cout << "BnkDerivedPathTests passed (actual multi-WWU/cross-pass budget, fixed expanded profile, Unicode Folder paths, failed/cancelled refresh, checked capacity and preserved core)\n";
}
