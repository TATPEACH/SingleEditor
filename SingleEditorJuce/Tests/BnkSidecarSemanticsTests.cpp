#include "Bnk/BnkParser.h"
#include "Bnk/BnkSidecarParser.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

void runBnkSidecarSemanticsTests()
{
    using namespace singleeditor;
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-SidecarSemantics-" + juce::Uuid().toString());
    check(root.createDirectory().wasOk(), "create owned sidecar semantics fixture");
    std::cout << "Sidecar semantics fixture: " << root.getFullPathName() << '\n';
    check(juce::String("12suffix").getDoubleValue() == 12.0, "legacy JUCE scalar API control accepts only the numeric prefix");
    std::cout << "Legacy scalar API control: 12suffix becomes 12; actual parser must reject the whole metadata transaction.\n";
    const auto bank = root.getChildFile("values.bnk"), xml = root.getChildFile("SoundbanksInfo.xml");
    juce::MemoryOutputStream header;
    header.write("BKHD", 4); header.writeInt(8); header.writeInt(128); header.writeInt(7);
    check(bank.replaceWithData(header.getData(), header.getDataSize()), "write actual proven core bank");
    const auto readParameter = [&](const juce::String& attributes)
    {
        check(xml.replaceWithText("<SoundBanksInfo><SoundBanks><SoundBank><Path>values.bnk</Path>"
            "<GameParameters><GameParameter Id=\"3\" GUID=\"{00000000-0000-0000-0000-000000000003}\" Name=\"Speed\" "
            + attributes + "/></GameParameters><IncludedEvents><Event Id=\"10\" Name=\"kept core\"/></IncludedEvents>"
              "</SoundBank></SoundBanks></SoundBanksInfo>"), "write actual numeric sidecar");
        return BnkParser::parse(bank, root);
    };
    for (const auto value : { "12suffix", "1e999", "NaN", "Inf", "0x1p2", "+-1", "" })
    {
        const auto result = readParameter("Min=\"0\" Max=\"" + juce::String(value) + "\" InitialValue=\"0\"");
        if (!(result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
            && result.state->rtpcs.empty() && result.state->events.empty() && result.state->info.bankId == 7))
            std::cerr << "Rejected scalar=" << value << " core=" << static_cast<int>(result.failure)
                << " sidecar=" << static_cast<int>(result.sidecar.failure) << " bankId=" << (result.state ? result.state->info.bankId : 0)
                << " rtpcs=" << (result.state ? result.state->rtpcs.size() : 0)
                << " events=" << (result.state ? result.state->events.size() : 0) << '\n';
        check(result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
            && result.state->rtpcs.empty() && result.state->events.empty() && result.state->info.bankId == 7,
            "numeric text must be complete and finite before metadata publication");
    }
    const auto invalid = [](const BnkParser::Result& result)
    {
        return result && !result.sidecar && result.sidecar.failure == BnkParser::Failure::invalidData
            && result.state->rtpcs.empty() && result.state->events.empty() && result.state->info.bankId == 7;
    };
    for (const auto attributes : {
        "Min=\"10\" Max=\"0\"", "Min=\"0\" Max=\"1\" InitialValue=\"2\"",
        "Min=\"0\" Max=\"1\" DefaultValue=\"-1\"", "Min=\"-1e308\" Max=\"1e308\"",
        "Min=\"0\" MinValue=\"1\"", "Max=\"100\" MaxValue=\"99\"",
        "InitialValue=\"0\" DefaultValue=\"1\"", "Max=\"100\" MaxValue=\"100suffix\"" })
        check(invalid(readParameter(attributes)), "range, default, alias and derived span must agree");
    auto good = readParameter("Min=\" -2.5e1 \" MinValue=\"-25\" MaxValue=\"+5E1\" InitialValue=\"+0.5\" DefaultValue=\".5\"");
    check(good && good.sidecar && good.state->rtpcs.size() == 1
        && good.state->rtpcs[0].minValue == -25.0 && good.state->rtpcs[0].maxValue == 50.0
        && good.state->rtpcs[0].defaultValue == 0.5, "finite decimal, exponent, whitespace and equivalent aliases survive exactly");
    good = readParameter("Min=\"7\" Max=\"7\" InitialValue=\"7\"");
    check(good && good.sidecar && good.state->rtpcs[0].defaultValue == 7.0, "constant parameter range remains supported");

    // A failed refresh must keep prior committed metadata, not just an empty
    // core result. Exercise the public transaction on an already enriched bank.
    const auto previous = good.state->rtpcs;
    readParameter("Min=\"0\" Max=\"1oops\"");
    const auto rejectedRefresh = BnkSidecarParser::enrichStateFromSidecars(bank, *good.state, root);
    check(!rejectedRefresh && good.state->rtpcs.size() == previous.size()
        && good.state->rtpcs[0].minValue == 7.0 && good.state->rtpcs[0].defaultValue == 7.0
        && good.state->events.size() == 1 && good.state->events[0].id == 10,
        "rejected numeric refresh preserves previous committed metadata and core identity");

    const auto readEvent = [&](const juce::String& attributes)
    {
        check(xml.replaceWithText("<SoundBanksInfo><SoundBanks><SoundBank><Path>values.bnk</Path>"
            "<IncludedEvents><Event Id=\"10\" Name=\"kept core\" " + attributes
            + "/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"), "write actual duration sidecar");
        return BnkParser::parse(bank, root);
    };
    for (const auto attributes : { "DurationMin=\"-1\"", "DurationMin=\"3\" DurationMax=\"2\"",
        "DurationMax=\"1e999\"", "DurationMax=\"10oops\"", "DurationType=\"OneShot\" DurationMax=\"Inf\"" })
        check(invalid(readEvent(attributes)), "finite duration requires a valid nonnegative interval");
    good = readEvent("DurationType=\"OneShot\" DurationMin=\"+1e0\" DurationMax=\"2.5\"");
    check(good && good.sidecar && good.state->events[0].durationMinSeconds == 1.0
        && good.state->events[0].durationMaxSeconds == 2.5, "finite event duration survives exactly");
    for (const auto type : { "Infinite", "Unknown", "FutureType", "" })
    {
        good = readEvent("DurationType=\"" + juce::String(type) + "\" DurationMin=\"Inf\" DurationMax=\"99999\"");
        check(good && good.sidecar && good.state->events.size() == 1
            && good.state->events[0].durationMinSeconds == 0.0 && good.state->events[0].durationMaxSeconds == 0.0
            && good.state->info.diagnostics.joinIntoString(" ").contains("finite duration hint is unavailable"),
            "explicit non-finite or unknown duration never becomes a finite sequence hint");
    }
    readEvent({});
    const auto parameterDirectory = root.getChildFile("Game Parameters");
    check(parameterDirectory.createDirectory().wasOk(), "create actual WWU parameter directory");
    const auto parameterFile = parameterDirectory.getChildFile("parameters.wwu");
    const auto writeParameter = [&](const juce::String& attributes)
    {
        check(parameterFile.replaceWithText("<WwiseDocument><GameParameter ID=\"{parameter}\" Name=\"Speed\" "
            + attributes + "/></WwiseDocument>"), "write WWU parameter");
        return BnkParser::parse(bank, root);
    };
    check(invalid(writeParameter("Min=\"0\" Max=\"1suffix\"")), "WWU parameter uses the same strict scalar transaction");
    check(invalid(writeParameter("Min=\"1\" Max=\"0\"")), "WWU parameter range is validated");
    good = writeParameter("Min=\"-25\" Max=\"50\" InitialValue=\"0.5\"");
    check(good && good.sidecar && good.state->rtpcs.size() == 1
        && good.state->rtpcs[0].defaultValue == 0.5, "valid WWU parameter is read from disk");
    check(parameterFile.replaceWithText("<WwiseDocument/>"), "remove fixture parameter contents before curve tests");

    const auto actors = root.getChildFile("Actor-Mixer Hierarchy"), events = root.getChildFile("Events");
    check(actors.createDirectory().wasOk() && events.createDirectory().wasOk(), "create actual actor and event work units");
    check(events.getChildFile("events.wwu").replaceWithText(
        "<WwiseDocument><Event ID=\"{event}\" Name=\"kept core\"><ChildrenList><Action><ReferenceList>"
        "<Reference Name=\"Target\"><ObjectRef ID=\"{target}\" Name=\"Target\"/></Reference>"
        "</ReferenceList></Action></ChildrenList></Event></WwiseDocument>"), "write actual event target work unit");
    const auto readActor = [&](const juce::String& body)
    {
        check(actors.getChildFile("actors.wwu").replaceWithText(
            "<WwiseDocument><Sound ID=\"{target}\" Name=\"Target\">" + body + "</Sound></WwiseDocument>"), "write actual actor work unit");
        return BnkParser::parse(bank, root);
    };
    const auto readCurve = [&](const juce::String& x, const juce::String& y)
    {
        return readActor("<PropertyList><Property Name=\"Volume\"><RTPCList><RTPC><ReferenceList>"
            "<Reference Name=\"ControlInput\"><ObjectRef ID=\"{parameter}\" Name=\"Speed\"/></Reference>"
            "</ReferenceList><Curve><PointList><Point><XPos>" + x + "</XPos><YPos>" + y
            + "</YPos></Point></PointList></Curve></RTPC></RTPCList></Property></PropertyList>");
    };
    for (const auto value : { "", "NaN", "1e999", "0x1p2", "2oops", "<Nested>1</Nested>" })
    {
        check(invalid(readCurve(value, "0")), "curve X requires a complete finite scalar");
        check(invalid(readCurve("0", value)), "curve Y requires a complete finite scalar");
    }
    good = readCurve("-1e2", "+.25");
    check(good && good.sidecar && good.state->rtpcs.size() == 1 && good.state->rtpcs[0].bindings.size() == 1
        && good.state->rtpcs[0].bindings[0].points.size() == 1
        && good.state->rtpcs[0].bindings[0].points[0].x == -100.0
        && good.state->rtpcs[0].bindings[0].points[0].y == 0.25, "actual WWU curve retains finite coordinates");

    const auto readVolume = [&](const juce::String& property)
    {
        return readActor("<StateInfo><StateGroupList><StateGroupInfo><StateGroupRef Name=\"Weather\"/>"
            "</StateGroupInfo></StateGroupList><CustomStateList><CustomState><StateRef ID=\"{state}\" Name=\"Rain\"/>"
            "<CustomState><PropertyList>" + property
            + "</PropertyList></CustomState></CustomState></CustomStateList></StateInfo>");
    };
    for (const auto value : { "", "NaN", "1e999", "1e39", "-1e39", "1suffix", "+-1", "1e-99", "-1e-99", "1000" })
        check(invalid(readVolume("<Property Name=\"Volume\" Value=\"" + juce::String(value) + "\"/>")),
            "volume is finite and float representable before narrowing");
    for (const auto property : {
        "<Property Name=\"Volume\"><ValueList/></Property>",
        "<Property Name=\"Volume\"><ValueList><Value Platform=\"A\">-6</Value><Value Platform=\"B\">3</Value></ValueList></Property>",
        "<Property Name=\"Volume\" Value=\"-6\"><ValueList><Value>-3</Value></ValueList></Property>",
        "<Property Name=\"Volume\" Value=\"-6\"/><Property Name=\"Volume\" Value=\"3\"/>",
        "<Property Name=\"Volume\"><ValueList><Value><Nested>-6</Nested></Value></ValueList></Property>",
        "<Property Name=\"Volume\"><ValueList><Value>-6</Value></ValueList><ValueList><Value>-6</Value></ValueList></Property>" })
        check(invalid(readVolume(property)), "ambiguous or malformed scalar must not choose a platform or definition");
    for (const auto property : {
        "<Property Name=\"Volume\" Value=\"-6\"/>",
        "<Property Name=\"Volume\"><ValueList><Value Platform=\"A\">-6</Value><Value Platform=\"B\">-6.0</Value></ValueList></Property>",
        "<Property Name=\"Volume\" Value=\"-6\"><ValueList><Value>-6e0</Value></ValueList></Property>" })
    {
        good = readVolume(property);
        check(good && good.sidecar && good.state->events.size() == 1 && good.state->events[0].stateModifiers.size() == 1
            && good.state->events[0].stateModifiers[0].hasVolume && good.state->events[0].stateModifiers[0].volumeDb == -6.0f,
            "unambiguous WWU volume reaches its actual event target exactly");
    }
    good.inputLease.reset();
    good.state.reset();
    check(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-SidecarSemantics-")
        && root.deleteRecursively(), "remove only the owned successful semantics fixture");
    std::cout << "BnkSidecarSemanticsTests passed (actual XML/WWU finite scalar, range, curve, volume and duration transactions)\n";
}
