#include "Bnk/BnkParser.h"
#include "Bnk/BnkMediaLocator.h"
#include "Bnk/BnkPreviewRequest.h"
#include "Bnk/BnkWemPlaybackBackend.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void checkLocator(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void writeLocatorWave(const juce::File& file, float value)
{
    checkLocator(file.getParentDirectory().createDirectory().wasOk(), "create private PCM parent");
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    const auto options=juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream,options));
    juce::AudioBuffer<float> pcm(2,128);
    for (int c=0;c<2;++c) for (int i=0;i<128;++i) pcm.setSample(c,i,c==0?value:-value);
    checkLocator(writer && writer->writeFromAudioSampleBuffer(pcm,0,128),"write all actual locator PCM samples");
}
void checkLocatorPcm(const singleeditor::BnkPreparedPlayback& preview, float value)
{
    checkLocator(preview.isValid() && preview.state->audioBuffer->getNumChannels()==2
        && preview.state->audioBuffer->getNumSamples()==128,"complete actual locator preview");
    for (int c=0;c<2;++c) for (int i=0;i<128;++i)
    {
        const auto expected=c==0?value:-value;
        const auto actual=preview.state->audioBuffer->getSample(c,i);
        if (std::abs(actual-expected)>=0.00001f)
            std::cerr << "PCM mismatch channel=" << c << " sample=" << i << " actual=" << actual
                << " expected=" << expected << " detail=" << preview.detail << '\n';
        checkLocator(std::abs(actual-expected)<0.00001f,"every actual PCM sample comes from the selected source role");
    }
}
}

void runBnkMediaLocatorTests()
{
    using namespace singleeditor;
    const auto parent=juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root=parent.getChildFile("SingleEditor-MediaLocator-"+juce::Uuid().toString());
    checkLocator(root.createDirectory().wasOk(),"create private media locator fixture");
    std::cout << "Media locator fixture: " << root.getFullPathName() << '\n';
    const auto bank=root.getChildFile("locator.bnk"), txt=root.getChildFile("locator.txt");
    juce::MemoryOutputStream header; header.write("BKHD",4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    checkLocator(bank.replaceWithData(header.getData(),header.getDataSize()),"write actual BKHD locator bank");
    const auto generated=root.getChildFile("packed.wav"), original=root.getChildFile("Originals/exact.wav");
    writeLocatorWave(generated,0.25f); writeLocatorWave(original,0.5f);
    // The locator lives in a separate translation unit but must use the
    // caller's active scope, including nested cancellation and restoration.
    {
        BoundedSidecarInput outer;
        checkLocator(bnkmedia::findOriginal({},root,{},"Originals/exact.wav",{})==original,
            "out-of-line locator observes the caller's active bounded scope");
        {
            BoundedSidecarInput cancelled(nullptr, [] { return false; });
            checkLocator(bnkmedia::findOriginal({},root,{},"Originals/exact.wav",{})==juce::File()
                && cancelled.failureReason()==BoundedSidecarInput::Failure::cancelled,
                "out-of-line locator cancels through the actual nested caller scope");
        }
        checkLocator(!outer.wasLimited()
            && bnkmedia::findOriginal({},root,{},"Originals/exact.wav",{})==original,
            "leaving a cancelled nested scope restores the outer locator transaction");
    }
    checkLocator(bnkmedia::findOriginal({},root,{},"Originals/exact.wav",{})==juce::File(),
        "locator does not inherit a completed caller scope");
    const auto actors=root.getChildFile("Actor-Mixer Hierarchy");
    checkLocator(actors.createDirectory().wasOk() && actors.getChildFile("actors.wwu").replaceWithText(
        "<WwiseDocument><WorkUnit Name=\"Unit\"><ChildrenList><Sound ID=\"{voice}\" Name=\"Voice\"/>"
        "</ChildrenList></WorkUnit></WwiseDocument>"),"write actual authoring owner for empty-column relation");
    const auto writeXml=[&](const juce::String& path={}, const juce::String& name={})
    {
        checkLocator(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
            "<SoundBanksInfo><SoundBanks><SoundBank><Path>locator.bnk</Path><IncludedEvents>"
            "<Event Id=\"10\" Name=\"Locator\"><ReferencedStreamedFiles><File Id=\"100\"><ShortName>"+name
            +"</ShortName><Path>"+path+"</Path></File></ReferencedStreamedFiles></Event>"
            "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),"write actual locator XML");
    };
    const auto writeTxt=[&](const juce::String& rows)
    { checkLocator(txt.replaceWithText(rows),"write actual tab-separated locator rows"); };
    const auto read=[&] { return BnkParser::parse(bank,root); };
    writeXml();
    writeTxt("Streamed Audio\tID\tName\tAudio source file\tGenerated audio file\tWwise Object Path\n"
        "100\t\t\tpacked.wav\t\\Actor-Mixer Hierarchy\\Unit\\Voice\n");
    auto parsed=read();
    checkLocator(parsed && parsed.sidecar && parsed.state->events.size()==1 && parsed.state->events[0].media.size()==1,
        "actual bank/XML/TXT transaction succeeds before checking empty-column interpretation");
    checkLocator(parsed.state->events[0].media[0].mediaFile==generated,
        "empty name and source columns must not shift generated media or owner columns");
    checkLocator(parsed.state->events[0].targetObjectId=="{voice}","empty TXT fields preserve exact actual WWU owner mapping");
    BnkWemPlaybackBackend backend;
    checkLocatorPcm(backend.preparePreview(*parsed.state,parsed.state->events[0],{}),0.25f);

    const juce::String streamedHeader="Streamed Audio\tID\tName\tAudio source file\tGenerated audio file\tWwise Object Path\n";
    const juce::String memoryHeader="In Memory Audio\tID\tName\tAudio source file\tWwise Object Path\n";
    const juce::String owner="\\Actor-Mixer Hierarchy\\Unit\\Voice";
    const auto streamed=[&](const juce::String& source,const juce::String& packed)
    { return "100\t\t"+source+"\t"+packed+"\t"+owner+"\n"; };
    const auto memory=[&](const juce::String& source)
    { return "100\t\t"+source+"\t"+owner+"\n"; };
    const auto playable=[&](const BnkParser::Result& result)
    {
        BnkDocument document;
        checkLocator(result && result.sidecar && document.adoptParsedState(result.state),"adopt actual locator document");
        const auto media=document.getPlayableMediaById(100);
        checkLocator(media.has_value(),"actual raw media descriptor exists");
        return *media;
    };
    const auto checkModes=[&](const BnkParser::Result& result,float sourceValue,float packedValue)
    {
        const auto raw=playable(result);
        std::cout << "Locator modes source=" << raw.sourceFile.getFullPathName() << " generated=" << raw.mediaFile.getFullPathName()
            << " expected=" << sourceValue << "/" << packedValue << '\n';
        std::optional<media::OptionalMediaFailureState::Key> priorKey;
        for (const auto mode:{BnkMediaResolveMode::sourceMedia,BnkMediaResolveMode::decodePackedMedia})
        {
            const auto value=mode==BnkMediaResolveMode::sourceMedia ? sourceValue : packedValue;
            backend.setMediaResolveMode(mode);
            checkLocatorPcm(backend.preparePreview(*result.state,result.state->events[0],{}),value);
            checkLocatorPcm(backend.prepareMediaPreview(*result.state,raw),value);
            for (bool rawRequest:{false,true})
            {
                const auto request=rawRequest ? BnkPreviewRequest::forMedia(result.state,raw,mode)
                    : BnkPreviewRequest::forEvent(result.state,10,{},mode);
                auto policy=std::make_shared<media::OptionalMediaFailureState>();
                auto operation=std::make_shared<media::OptionalMediaOperation>(policy,
                    media::OptionalMediaFailureState::Intent::initial,std::nullopt,media::OptionalMediaFailureState::Module::bank);
                checkLocator(request!=nullptr,"create actual native-path frozen locator request");
                checkLocatorPcm(request->prepare(operation),value);
                checkLocator(operation->report().key.has_value() && !operation->report().failure,
                    "actual request pins admitted physical media without fallback after decode");
                if (!rawRequest)
                {
                    if (priorKey) checkLocator(priorKey->sourceFingerprint!=operation->report().key->sourceFingerprint,
                        "source and packed mode requests have distinct admitted scope identities");
                    priorKey=operation->report().key;
                }
            }
        }
    };
    writeTxt(streamedHeader+streamed("Originals/exact.wav","packed.wav"));
    parsed=read();
    checkLocator(parsed && parsed.sidecar && parsed.state->events[0].media[0].sourceFile==original
        && parsed.state->events[0].media[0].mediaFile==generated,"source and generated locators remain separate in actual event metadata");
    const auto raw=playable(parsed);
    checkLocator(raw.sourceFile==original && raw.mediaFile==generated,"actual Audio list preserves both source roles");
    checkModes(parsed,0.5f,0.25f);

    // Prefetched/streamed descriptions may name the same media in both sections.
    for (bool reversed:{false,true})
    {
        const auto sourceRows=memoryHeader+memory("Originals/exact.wav");
        const auto packedRows=streamedHeader+streamed("Originals/exact.wav","packed.wav");
        writeTxt(reversed ? packedRows+sourceRows : sourceRows+packedRows);
        parsed=read(); checkModes(parsed,0.5f,0.25f);
    }
    const auto reject=[&](const juce::String& rows)
    {
        writeTxt(rows); auto rejected=read();
        checkLocator(rejected && !rejected.sidecar && rejected.state->info.bankId==7
            && rejected.sidecar.failure==BnkParser::Failure::invalidData && rejected.state->events.empty(),
            "conflicting locator declaration rejects the private metadata while preserving actual core bank");
    };
    reject(streamedHeader+streamed("Originals/exact.wav","packed.wav")+streamed("Originals/missing.wav","packed.wav"));
    reject(streamedHeader+streamed("Originals/exact.wav","packed.wav")+streamed("Originals/exact.wav","other.wav"));
    reject(memoryHeader+memory("Originals/exact.wav")+memory("Originals/missing.wav"));
    reject(streamedHeader+streamed("../outside.wav","packed.wav"));
    reject(streamedHeader+streamed("Originals/exact.wav","../outside.wav"));
    writeTxt(streamedHeader+streamed("Originals/exact.wav","packed.wav")+streamed("Originals/exact.wav","packed.wav"));
    parsed=read(); checkModes(parsed,0.5f,0.25f);

    writeTxt(streamedHeader+streamed("Originals/exact.wav",{}));
    parsed=read();
    checkLocator(parsed && parsed.sidecar && parsed.state->events[0].media[0].mediaFile==juce::File()
        && parsed.state->events[0].media[0].sourceFile==original,"empty generated cell never consumes Wwise owner path");
    checkModes(parsed,0.5f,0.5f); // Existing location fallback is retained before admission.
    writeTxt(memoryHeader+memory("Originals/exact.wav"));
    parsed=read();
    const auto sourceOnly=playable(parsed);
    checkLocator(sourceOnly.availability==BnkDocument::PlayableMediaDescriptor::Availability::sourceMedia
        && !sourceOnly.extractableFromBank,"source-only metadata cannot invent embedded bank bytes or extraction availability");
    checkModes(parsed,0.5f,0.5f);

    // XML can resolve both roots; generated and original bytes must stay distinct.
    const auto generatedRoot=root.getChildFile("GeneratedSoundBanks");
    writeLocatorWave(generatedRoot.getChildFile("exact.wav"),0.125f);
    writeXml("exact.wav","exact.wav"); writeTxt({}); parsed=read();
    checkModes(parsed,0.5f,0.125f);
    writeTxt(streamedHeader+streamed("Originals/exact.wav","packed.wav"));
    auto conflict=read();
    checkLocator(conflict && !conflict.sidecar && conflict.sidecar.failure==BnkParser::Failure::invalidData,
        "contradictory XML and TXT generated location cannot silently choose different audio");
    conflict.inputLease.reset(); conflict.state.reset();

    // Shared original-name lookup must not choose the first equal basename.
    const auto a=root.getChildFile("Originals/A/same.wav"), b=root.getChildFile("Originals/B/same.wav");
    writeLocatorWave(a,0.5f); writeLocatorWave(b,0.125f);
    writeXml({},"same.wav"); writeTxt(streamedHeader+streamed({},"packed.wav"));
    parsed=read(); checkLocator(playable(parsed).sourceFile==juce::File(),"ambiguous original basenames have no guessed source identity");
    checkModes(parsed,0.25f,0.25f);
    checkLocator(b.isAChildOf(root) && b.deleteFile(),"remove only one owned ambiguity fixture file");
    parsed=read(); checkLocator(playable(parsed).sourceFile==a,"unique original name still resolves the existing fallback");
    checkModes(parsed,0.5f,0.25f);
    writeTxt(streamedHeader+streamed("Originals/missing.wav","packed.wav"));
    parsed=read(); checkModes(parsed,0.25f,0.25f);

    checkLocator(root.getChildFile("Originals/bad.wav").replaceWithText("not a WAV"),"write actual failed preferred source");
    writeTxt(streamedHeader+streamed("Originals/bad.wav","packed.wav")); parsed=read();
    backend.setMediaResolveMode(BnkMediaResolveMode::sourceMedia);
    checkLocator(!backend.preparePreview(*parsed.state,parsed.state->events[0],{}).isValid(),
        "actual preferred source decode failure cannot silently switch to generated PCM");
    {
        const auto request=BnkPreviewRequest::forEvent(parsed.state,10,{},BnkMediaResolveMode::sourceMedia);
        const auto policy=std::make_shared<media::OptionalMediaFailureState>();
        const auto operation=std::make_shared<media::OptionalMediaOperation>(policy,
            media::OptionalMediaFailureState::Intent::initial,std::nullopt,media::OptionalMediaFailureState::Module::bank);
        checkLocator(request && !request->prepare(operation).isValid() && operation->report().key && operation->report().failure,
            "actual frozen failed source remains a failure for its pinned role, not alternate generated success");
    }
    backend.setMediaResolveMode(BnkMediaResolveMode::decodePackedMedia);
    checkLocatorPcm(backend.preparePreview(*parsed.state,parsed.state->events[0],{}),0.25f);

    parsed.inputLease.reset(); parsed.state.reset();
    checkLocator(root.getParentDirectory()==parent && root.getFileName().startsWith("SingleEditor-MediaLocator-")
        && root.deleteRecursively(),"remove only successful private locator fixture after releasing leases");
    std::cout << "BnkMediaLocatorTests passed (actual XML/TXT roles, empty cells, duplicates, scoped paths, unique originals, Document/raw/event/frozen requests and every PCM sample)\n";
}
