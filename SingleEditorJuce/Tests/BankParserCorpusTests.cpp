#include "Bnk/BnkParser.h"
#include "Bnk/WaveDurationHint.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>

namespace
{
using namespace singleeditor;
using Bytes = std::vector<uint8_t>;
void corpusCheck(bool ok, const char* message)
{
    if (!ok) { std::cerr << "FAILED: " << message << std::endl; std::exit(1); }
}
Bytes bytes(const juce::MemoryOutputStream& stream)
{
    const auto* begin = static_cast<const uint8_t*>(stream.getData());
    return { begin, begin + stream.getDataSize() };
}
Bytes utf8(const juce::String& value)
{
    const auto* begin = reinterpret_cast<const uint8_t*>(value.toRawUTF8());
    return { begin, begin + value.getNumBytesAsUTF8() };
}
juce::String digest(const Bytes& data) { return juce::SHA256(data.data(), data.size()).toHexString(); }
void write(const juce::File& file, const Bytes& data)
{
    if (data.empty())
    {
        juce::FileOutputStream stream(file);
        corpusCheck(stream.openedOk() && stream.setPosition(0) && stream.truncate().wasOk(), "create actual empty owned input without deleting its identity");
    }
    else corpusCheck(file.replaceWithData(data.data(), data.size()), "write only owned corpus input");
    corpusCheck(file.existsAsFile() && file.getSize() == static_cast<int64_t>(data.size()), "corpus input has the intended physical length");
}
void chunk(juce::MemoryOutputStream& stream, const char* tag, const juce::MemoryOutputStream& body)
{
    stream.write(tag, 4); stream.writeInt(static_cast<int>(body.getDataSize())); stream.write(body.getData(), body.getDataSize());
}
struct Case { int family; juce::String mutation; Bytes data; };

juce::String bankOutcome(const BnkParser::Result& result)
{
    corpusCheck(result.failure != BnkParser::Failure::deadline && result.failure != BnkParser::Failure::ioFailure
        && result.sidecar.failure != BnkParser::Failure::deadline && result.sidecar.failure != BnkParser::Failure::ioFailure,
        "tiny fixed corpus must not hide timeout or IO faults as malformed-input success");
    juce::String key = juce::String(static_cast<int>(result.failure)) + "/" + juce::String(static_cast<int>(result.sidecar.failure));
    if (!result)
    {
        corpusCheck(!result.state && !result.inputLease && result.failure != BnkParser::Failure::none,
            "failed core has a typed failure, no partial State and no retained source lease");
        return key;
    }
    const auto& state = *result.state;
    corpusCheck(result.inputLease && state.isValid() && state.isSourceCurrent(), "accepted bank retains its current physical input");
    corpusCheck(state.objects.size() <= 65536 && state.events.size() <= 4096, "accepted parser respects structural output limits");
    std::set<uint32_t> objectIds, mediaIds;
    key += "/" + juce::String(state.info.bankId) + "/" + juce::String(state.dataChunkStart) + "/" + juce::String(state.dataChunkSize);
    for (const auto& media : state.mediaEntries)
    {
        corpusCheck(mediaIds.insert(media.id).second && static_cast<uint64_t>(media.offset) + media.size <= static_cast<uint64_t>(state.dataChunkSize),
            "accepted media spans stay unique and within the actual DATA body");
        key += "/m" + juce::String(media.id) + ":" + juce::String(media.offset) + ":" + juce::String(media.size);
    }
    for (const auto& object : state.objects)
    {
        corpusCheck(objectIds.insert(object.id).second && object.payloadSize == object.payload.getSize(), "accepted HIRC identity and payload remain exact");
        key += "/o" + juce::String(object.id) + ":" + juce::String(static_cast<int>(object.type))
            + ":" + juce::SHA256(object.payload).toHexString();
    }
    for (const auto& event : state.events)
    {
        key += "/e" + juce::String(event.id) + ":" + event.displayName;
        for (const auto id : event.actionIds) key += ":" + juce::String(id);
    }
    return key;
}
}

// Deterministic mutation tier, not coverage-guided fuzzing or a real Wwise corpus.
// Inputs and progress survive a crash. A fresh directory plus case ID replays it.
void runBankParserCorpusTests(const juce::File& artifacts, int onlyCase)
{
    corpusCheck(!artifacts.exists() && artifacts.getFileName().startsWith("bnk-corpus-") && artifacts.createDirectory().wasOk(),
        "corpus requires a fresh explicitly named artifact directory");
    const auto input = artifacts.getChildFile("inputs"), actors = input.getChildFile("Actor-Mixer Hierarchy");
    corpusCheck(actors.createDirectory().wasOk(), "create owned parser input directory");
    const auto bank = input.getChildFile("case.bnk"), xml = input.getChildFile("SoundbanksInfo.xml"), wwu = actors.getChildFile("case.wwu"), wave = input.getChildFile("hint.wav");
    juce::MemoryOutputStream header, minimal, full, didx, data, hirc, event;
    header.writeInt(128); header.writeInt(7); chunk(minimal, "BKHD", header); chunk(full, "BKHD", header);
    didx.writeInt(99); didx.writeInt(0); didx.writeInt(4); data.writeInt(0); chunk(full, "DIDX", didx); chunk(full, "DATA", data);
    hirc.writeInt(2); event.writeInt(1); event.writeInt(20);
    hirc.writeByte(4); hirc.writeInt(4 + static_cast<int>(event.getDataSize())); hirc.writeInt(10); hirc.write(event.getData(), event.getDataSize());
    hirc.writeByte(3); hirc.writeInt(4); hirc.writeInt(20); chunk(full, "HIRC", hirc);
    const auto catalog = utf8("<SoundBanksInfo><SoundBanks><SoundBank><Path>case.bnk</Path><IncludedEvents>"
        "<Event Id=\"10\" GUID=\"event\" Name=\"Corpus\"/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>");
    const auto authoring = utf8("<WwiseDocument><WorkUnit Name=\"Unit\"><ChildrenList><Folder Name=\"Folder\"><ChildrenList>"
        "<Sound ID=\"sound\" Name=\"Sound\"/></ChildrenList></Folder></ChildrenList></WorkUnit></WwiseDocument>");
    juce::MemoryOutputStream riff, fmt, waveBody, samples;
    fmt.writeShort(1); fmt.writeShort(1); fmt.writeInt(48000); fmt.writeInt(96000); fmt.writeShort(2); fmt.writeShort(16);
    samples.writeInt(0); chunk(waveBody, "fmt ", fmt); chunk(waveBody, "data", samples);
    riff.write("RIFF", 4); riff.writeInt(4 + static_cast<int>(waveBody.getDataSize())); riff.write("WAVE", 4); riff.write(waveBody.getData(), waveBody.getDataSize());
    const std::array<Bytes, 4> seeds { bytes(full), catalog, authoring, bytes(riff) };
    std::vector<Case> corpus;
    uint32_t random = 0x0327c0de;
    const auto nextRandom = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    for (int family = 0; family < 4; ++family)
    {
        const auto& seed = seeds[static_cast<size_t>(family)];
        write(artifacts.getChildFile("seed-" + juce::String(family) + ".bin"), seed);
        corpus.push_back({ family, "original", seed });
        for (size_t offset = 0; offset < seed.size(); ++offset)
        {
            corpus.push_back({ family, "truncate:" + juce::String(static_cast<int>(offset)), Bytes(seed.begin(), seed.begin() + static_cast<ptrdiff_t>(offset)) });
            for (const auto mask : { 1, 128, 255 })
            {
                auto changed = seed; changed[offset] ^= static_cast<uint8_t>(mask);
                corpus.push_back({ family, "xor:" + juce::String(static_cast<int>(offset)) + ":" + juce::String(mask), std::move(changed) });
            }
        }
        for (int mutation = 0; mutation < 128; ++mutation)
        {
            auto changed = seed;
            const auto offset = nextRandom() % changed.size();
            if (mutation % 3 == 0)
            {
                const auto count = static_cast<size_t>(nextRandom() % 17 + 1);
                const auto value = static_cast<uint8_t>(nextRandom());
                changed.insert(changed.begin() + static_cast<ptrdiff_t>(offset), count, value);
            }
            else if (mutation % 3 == 1) changed.erase(changed.begin() + static_cast<ptrdiff_t>(offset));
            else for (int byte = 0; byte < 4 && offset + byte < changed.size(); ++byte) changed[offset + byte] = static_cast<uint8_t>(nextRandom());
            corpus.push_back({ family, "seeded:" + juce::String(mutation), std::move(changed) });
        }
    }
    corpusCheck(onlyCase < static_cast<int>(corpus.size()), "replay case exists in this corpus schema");
    auto rows = artifacts.getChildFile("results.jsonl").createOutputStream(); corpusCheck(rows != nullptr, "open owned result stream");
    int executed = 0, accepted = 0, rejected = 0;
    const auto began = juce::Time::getMillisecondCounterHiRes();
#if defined(__SANITIZE_ADDRESS__)
    constexpr bool instrumented = true;
#else
    constexpr bool instrumented = false;
#endif
    std::cout << "Bank parser corpus v1 cases=" << corpus.size() << " seed=0x0327c0de asan=" << instrumented << std::endl;
    for (size_t index = 0; index < corpus.size(); ++index)
    {
        if (onlyCase >= 0 && index != static_cast<size_t>(onlyCase)) continue;
        const auto& test = corpus[index];
        write(bank, test.family == 0 ? test.data : bytes(minimal));
        write(xml, test.family == 1 ? test.data : (test.family == 2 ? catalog : utf8("<SoundBanksInfo/>")));
        write(wwu, test.family == 2 ? test.data : utf8("<WwiseDocument/>"));
        write(wave, test.family == 3 ? test.data : seeds[3]);
        const auto mutatedFile = test.family == 0 ? bank : (test.family == 1 ? xml : (test.family == 2 ? wwu : wave));
        corpusCheck(juce::SHA256(mutatedFile).toHexString() == digest(test.data), "actual mutated file bytes match the recorded deterministic input");
        juce::ReferenceCountedObjectPtr<juce::DynamicObject> row = new juce::DynamicObject();
        row->setProperty("case", static_cast<int>(index)); row->setProperty("family", test.family); row->setProperty("mutation", test.mutation);
        row->setProperty("bytes", static_cast<int>(test.data.size())); row->setProperty("sha256", digest(test.data));
        corpusCheck(artifacts.getChildFile("current-case.json").replaceWithText(juce::JSON::toString(juce::var(row.get()), true)), "persist current case before native parser admission");
        juce::String outcome; bool ok = false;
        if (test.family == 3)
        {
            const auto first = WaveDurationHint::inspect(wave), second = WaveDurationHint::inspect(wave);
            corpusCheck(first.failure == second.failure && first.seconds == second.seconds && first.bytesRead == second.bytesRead,
                "same hint bytes produce the same typed result and duration");
            corpusCheck(first.failure != WaveDurationHint::Failure::ioFailure && first.bytesRead <= 65536 && first.chunksVisited <= 1025,
                "hint mutations remain bounded and do not hide IO failures");
            corpusCheck(std::isfinite(first.seconds) && first.seconds >= 0 && (first.failure == WaveDurationHint::Failure::none || first.seconds == 0),
                "hint never publishes nonfinite or failed duration");
            outcome = juce::String(static_cast<int>(first.failure)) + "/" + juce::String(first.seconds, 15); ok = static_cast<bool>(first);
        }
        else
        {
            auto first = BnkParser::parse(bank, input); outcome = bankOutcome(first); ok = static_cast<bool>(first) && static_cast<bool>(first.sidecar);
            if (test.family != 0)
            {
                corpusCheck(first && first.state->info.bankId == 7, "mutated optional input preserves independently proved core");
                if (!first.sidecar) corpusCheck(first.state->events.empty() && first.state->rtpcs.empty(), "optional rejection never publishes a prefix");
            }
            first = {};
            auto second = BnkParser::parse(bank, input);
            corpusCheck(outcome == bankOutcome(second), "same bank and sidecar bytes produce deterministic structural output");
            second = {};
            if (test.mutation == "original")
            {
                const auto cancelled = BnkParser::parse(bank, input, [](const auto&) { return false; });
                corpusCheck(!cancelled && cancelled.failure == BnkParser::Failure::cancelled && !cancelled.inputLease,
                    "parser cancellation leaves no partial State or source lease");
            }
        }
        // An identical replacement is a real Windows deny-write lease probe.
        write(bank, test.family == 0 ? test.data : bytes(minimal));
        write(xml, test.family == 1 ? test.data : (test.family == 2 ? catalog : utf8("<SoundBanksInfo/>")));
        write(wwu, test.family == 2 ? test.data : utf8("<WwiseDocument/>"));
        row->setProperty("accepted", ok); row->setProperty("outcome", outcome);
        corpusCheck(rows->writeText(juce::JSON::toString(juce::var(row.get()), true) + "\n", false, false, nullptr), "record actual corpus outcome");
        ++executed; if (ok) ++accepted; else ++rejected;
        if (executed % 128 == 0) { rows->flush(); std::cout << "corpus completed=" << executed << " last=" << index << std::endl; }
    }
    rows->flush(); rows.reset();
    corpusCheck(executed > 0, "at least one corpus input was actually executed");
    auto summary = new juce::DynamicObject();
    summary->setProperty("schema", "singleeditor.bank-parser-corpus.v1"); summary->setProperty("seed", "0x0327c0de");
    summary->setProperty("availableCases", static_cast<int>(corpus.size())); summary->setProperty("executed", executed);
    summary->setProperty("accepted", accepted); summary->setProperty("rejected", rejected); summary->setProperty("asan", instrumented);
    summary->setProperty("elapsedMs", juce::Time::getMillisecondCounterHiRes() - began);
    corpusCheck(artifacts.getChildFile("summary.json").replaceWithText(juce::JSON::toString(juce::var(summary))), "persist completed corpus evidence");
    std::cout << "BankParserCorpusTests passed executed=" << executed << " accepted=" << accepted << " rejected=" << rejected << " asan=" << instrumented << std::endl;
}

int runBankCorpusAsanWitness()
{
#if defined(__SANITIZE_ADDRESS__)
    // Positive control belongs only to this test executable. ASan must terminate
    // it with heap-buffer-overflow; an ordinary successful exit is a failed gate.
    const auto data = std::make_unique<int[]>(4);
    volatile int invalidIndex = 7;
    std::cout << "ASan positive control value=" << data[invalidIndex] << std::endl;
    return 2;
#else
    std::cerr << "ASan positive control requires an instrumented Document test build\n";
    return 2;
#endif
}
