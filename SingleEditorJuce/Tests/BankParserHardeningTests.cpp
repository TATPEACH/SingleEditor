#include "Bnk/BnkParser.h"
#include "Bnk/BnkEventResolver.h"
#include "Bnk/WaveDurationHint.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << "\n"; std::exit(1); }
}
void chunk(juce::MemoryOutputStream& out, const char* tag, const juce::MemoryOutputStream& body)
{
    out.write(tag, 4); out.writeInt(static_cast<int>(body.getDataSize()));
    out.write(body.getData(), body.getDataSize());
}
void object(juce::MemoryOutputStream& out, int type, int id, const juce::MemoryOutputStream& body)
{
    out.writeByte(static_cast<char>(type)); out.writeInt(4 + static_cast<int>(body.getDataSize()));
    out.writeInt(id); out.write(body.getData(), body.getDataSize());
}
}

void runBankParserHardeningTests()
{
    using namespace singleeditor;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-BankParserHardening-" + juce::Uuid().toString());
    check(root.createDirectory().wasOk(), "create private parser fixture");
    const auto bank = root.getChildFile("shared-graph.bnk");
    juce::MemoryOutputStream bytes, header, hirc, event, empty;
    header.writeInt(128); header.writeInt(7); chunk(bytes, "BKHD", header);
    hirc.writeInt(2); event.writeInt(512);
    for (int i = 0; i < 512; ++i) event.writeInt(20);
    object(hirc, 4, 10, event); object(hirc, 3, 20, empty); chunk(bytes, "HIRC", hirc);
    check(bank.replaceWithData(bytes.getData(), bytes.getDataSize()), "write actual repeated-action bank");
    auto parsed = BnkParser::parse(bank, root);
    check(parsed && parsed.state->events[0].actionIds.size() == 512, "preserve explicit event action list");
    const auto graph = BnkEventResolver::resolve(*parsed.state, 10);
    check(graph && graph->nodes.size() == 2 && graph->nodes[0].children.size() == 1,
        "shared action remains one node and one graph edge");
    check(graph->diagnostics.size() == 2, "shared action payload is expanded once, without repeated derived diagnostics");
    int checkpoints = 0;
    check(!BnkEventResolver::resolve(*parsed.state, 10, [&] { return ++checkpoints < 12; }) && checkpoints == 12,
        "shared graph traversal cancellation publishes no partial projection");
    parsed = {};
    // The two supported event count widths must consume the entire payload.
    const auto parseEvent = [&](const juce::MemoryOutputStream& payload)
    {
        juce::MemoryOutputStream container, table;
        chunk(container, "BKHD", header); table.writeInt(1); object(table, 4, 10, payload); chunk(container, "HIRC", table);
        check(bank.replaceWithData(container.getData(), container.getDataSize()), "write exact event-shape fixture");
        return BnkParser::parse(bank, root);
    };
    juce::MemoryOutputStream trailing;
    trailing.writeInt(1); trailing.writeInt(20); trailing.writeByte(0); trailing.writeByte(0);
    check(!parseEvent(trailing), "ambiguous event trailing bytes cannot be silently reinterpreted as another count width");
    juce::MemoryOutputStream narrow;
    narrow.writeByte(1); narrow.writeInt(20);
    { auto value = parseEvent(narrow); check(value && value.state->events[0].actionIds == std::vector<uint32_t>{20},
        "exact one-byte count retains its original action ID"); }
    juce::MemoryOutputStream wide;
    wide.writeInt(1); wide.writeInt(20);
    { auto value = parseEvent(wide); check(value && value.state->events[0].actionIds == std::vector<uint32_t>{20},
        "exact four-byte count retains its original action ID"); }
    // Bound total derived action storage independently of the table byte limit.
    const auto aggregate = [&](int count)
    {
        juce::MemoryOutputStream container, table, links;
        chunk(container, "BKHD", header); table.writeInt(count); links.writeInt(4096);
        for (int i = 0; i < 4096; ++i) links.writeInt(20);
        for (int i = 0; i < count; ++i) object(table, 4, 100 + i, links);
        chunk(container, "HIRC", table);
        check(bank.replaceWithData(container.getData(), container.getDataSize()), "write aggregate action fixture");
        return BnkParser::parse(bank, root);
    };
    { auto boundary = aggregate(16); check(boundary && boundary.state->events.size() == 16, "65536 aggregate action links remain supported"); }
    { auto over = aggregate(17); check(!over && over.failure == BnkParser::Failure::hardLimit && !over.inputLease,
        "aggregate derived allocation rejects before reserve and releases the actual input"); }

    // Shared DAG, shortest-depth reachability and a cycle remain a finite graph.
    BnkDocument::State dag;
    BnkDocument::EventDescriptor descriptor; descriptor.id = 500; descriptor.actionIds = { 1, 9 }; dag.events.push_back(descriptor);
    for (uint32_t id = 1; id <= 12; ++id)
    {
        BnkDocument::HircObject value; value.id = id; value.type = BnkDocument::HircObjectType::eventAction;
        juce::MemoryOutputStream refs;
        if (id < 12) refs.writeInt(static_cast<int>(id + 1));
        if (id == 11) refs.writeInt(9);
        value.payload = refs.getMemoryBlock(); value.payloadSize = static_cast<uint32_t>(value.payload.getSize());
        dag.objects.push_back(std::move(value));
    }
    const auto shared = BnkEventResolver::resolve(dag, 500);
    check(shared && shared->nodes.size() == 13, "shallower shared route reaches descendants beyond a deep first path");
    const auto node9 = std::find_if(shared->nodes.begin(), shared->nodes.end(), [](const auto& node) { return node.objectId == 9; });
    const auto node11 = std::find_if(shared->nodes.begin(), shared->nodes.end(), [](const auto& node) { return node.objectId == 11; });
    check(node9 != shared->nodes.end() && node11 != shared->nodes.end()
        && std::find(node11->children.begin(), node11->children.end(), static_cast<int>(node9 - shared->nodes.begin())) == node11->children.end(),
        "cyclic edge is omitted while the reachable shared branch survives");
    dag.objects[0].payload.setSize(256 * 1024, true); dag.events[0].actionIds = { 1 };
    checkpoints = 0;
    check(!BnkEventResolver::resolve(dag, 500, [&] { return ++checkpoints < 17; }) && checkpoints == 17,
        "large heuristic payload scan observes cancellation at a 64 KiB boundary");

    const auto wave = root.getChildFile("hint.wav");
    const auto makeWave = [&](int format, int bits, bool duplicate, bool rf64, bool metadata, bool extensible)
    {
        juce::MemoryOutputStream fmt, body, audio;
        const auto align = 2 * bits / 8;
        fmt.writeShort(static_cast<short>(extensible ? 0xfffe : format)); fmt.writeShort(2);
        fmt.writeInt(48000); fmt.writeInt(48000 * align); fmt.writeShort(static_cast<short>(align)); fmt.writeShort(static_cast<short>(bits));
        if (extensible)
        {
            fmt.writeShort(22); fmt.writeShort(static_cast<short>(bits)); fmt.writeInt(3); fmt.writeInt(format);
            const uint8_t guid[] { 0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71 }; fmt.write(guid, sizeof(guid));
        }
        if (metadata)
        {
            body.write("bext", 4); body.writeInt(8 * 1024 * 1024);
            juce::MemoryBlock zeros(8 * 1024 * 1024, true); body.write(zeros.getData(), zeros.getSize());
        }
        chunk(body, "fmt ", fmt);
        if (duplicate) chunk(body, "fmt ", fmt);
        juce::MemoryBlock samples(static_cast<size_t>(4800 * align), true);
        audio.write(samples.getData(), samples.getSize());
        if (rf64) { body.write("data", 4); body.writeInt(-1); body.write(audio.getData(), audio.getDataSize()); }
        else chunk(body, "data", audio);
        juce::MemoryOutputStream container;
        container.write(rf64 ? "RF64" : "RIFF", 4);
        container.writeInt(rf64 ? -1 : 4 + static_cast<int>(body.getDataSize())); container.write("WAVE", 4);
        if (rf64)
        {
            juce::MemoryOutputStream ds64;
            ds64.writeInt64(static_cast<juce::int64>(4 + 36 + body.getDataSize()));
            ds64.writeInt64(static_cast<juce::int64>(audio.getDataSize())); ds64.writeInt64(4800); ds64.writeInt(0);
            chunk(container, "ds64", ds64);
        }
        container.write(body.getData(), body.getDataSize());
        check(wave.replaceWithData(container.getData(), container.getDataSize()), "write actual bounded WAV hint fixture");
    };
    for (const auto format : { 1, 3 })
        for (const auto extensible : { false, true })
            for (const auto rf64 : { false, true })
            {
                makeWave(format, format == 1 ? 24 : 32, false, rf64, false, extensible);
                const auto hint = WaveDurationHint::inspect(wave);
                check(hint && std::abs(hint.seconds - 0.1) < 1.0e-12 && hint.bytesRead <= 104,
                    "actual PCM/float RIFF/RF64/extensible duration uses only fixed fields");
            }
    makeWave(1, 16, false, false, true, false);
    { const auto hint = WaveDurationHint::inspect(wave); check(hint && hint.bytesRead == 52,
        "eight MiB BWAV metadata and audio bodies are skipped without materialization"); }
    makeWave(1, 16, true, false, false, false);
    check(WaveDurationHint::inspect(wave).failure == WaveDurationHint::Failure::invalidData, "duplicate fmt cannot change duration interpretation");
    makeWave(1, 16, false, false, false, false);
    checkpoints = 0; bool pinned = false;
    const auto cancelledHint = WaveDurationHint::inspect(wave, [&]
    {
        ++checkpoints;
#if JUCE_WINDOWS
        if (checkpoints == 2) pinned = !wave.appendData("x", 1);
#else
        pinned = true;
#endif
        return checkpoints < 3;
    });
    check(cancelledHint.failure == WaveDurationHint::Failure::cancelled && pinned && wave.appendData("x", 1),
        "actual hint input is pinned during read and released after cancellation");
    makeWave(1, 16, false, false, false, false);
    {
        juce::MemoryOutputStream many;
        many.write("RIFF", 4); many.writeInt(4 + 1025 * 8); many.write("WAVE", 4);
        for (int i = 0; i < 1025; ++i) { many.write("JUNK", 4); many.writeInt(0); }
        check(wave.replaceWithData(many.getData(), many.getDataSize()), "write finite chunk-bomb fixture");
        const auto hint = WaveDurationHint::inspect(wave);
        check(hint.failure == WaveDurationHint::Failure::hardLimit && hint.bytesRead == 8204,
            "chunk budget rejects further work after 1024 headers");
    }
    makeWave(1, 16, false, false, false, false);
    {
        juce::FileOutputStream change(wave);
        check(change.setPosition(40) && change.writeInt(0x7fffffff), "write hostile actual data length");
    }
    check(WaveDurationHint::inspect(wave).failure == WaveDurationHint::Failure::invalidData,
        "declared data cannot escape the actual RIFF or physical file");
    makeWave(1, 16, false, false, false, false);
    {
        juce::MemoryOutputStream container, table, links;
        chunk(container, "BKHD", header); table.writeInt(2); links.writeInt(1); links.writeInt(20);
        object(table, 4, 10, links); object(table, 5, 20, empty); chunk(container, "HIRC", table);
        check(bank.replaceWithData(container.getData(), container.getDataSize()), "write real random-container bank");
        check(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
            "<SoundBanksInfo><SoundBanks><SoundBank><Path>shared-graph.bnk</Path><IncludedEvents>"
            "<Event Id=\"10\" Name=\"hint event\" DurationMax=\"0.3\"><IncludedMemoryFiles>"
            "<File Id=\"1\"><ShortName>hint.wav</ShortName><Path>hint.wav</Path></File>"
            "<File Id=\"2\"><ShortName>hint.wav</ShortName><Path>hint.wav</Path></File>"
            "</IncludedMemoryFiles></Event></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"),
            "write actual sidecar duration and media references");
        { auto sequence = BnkParser::parse(bank, root);
          check(sequence && sequence.sidecar && sequence.state->events[0].playbackHint == BnkDocument::PlaybackHint::sequence,
              "actual bank plus XML plus bounded WAV duration preserves sequence hint"); }
        makeWave(1, 16, true, false, false, false);
        { auto degraded = BnkParser::parse(bank, root);
          check(degraded && degraded.sidecar && degraded.state->events[0].media.size() == 2
              && degraded.state->events[0].playbackHint == BnkDocument::PlaybackHint::random
              && degraded.state->info.diagnostics.joinIntoString(" ").contains("optional duration hints are unavailable"),
              "bad optional WAV hint is visible degradation while explicit event and metadata remain"); }
    }
    {
        // Sidecar identity failures must not publish an earlier valid prefix.
        makeWave(1, 16, false, false, false, false);
        auto retained = BnkParser::parse(bank, root);
        check(retained && retained.state->events.size() == 1 && retained.state->events[0].media.size() == 2,
            "capture actual core and existing metadata before sidecar conflicts");
        const auto xml = root.getChildFile("SoundbanksInfo.xml");
        const auto wrap = [](const juce::String& events)
        { return "<SoundBanksInfo><SoundBanks><SoundBank><Path>shared-graph.bnk</Path><IncludedEvents>"
            + events + "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"; };
        const auto reject = [&](const juce::String& body, const char* message)
        {
            check(xml.replaceWithText(body), "write actual conflicting sidecar");
            const auto result = BnkSidecarParser::enrichStateFromSidecars(bank, *retained.state, root);
            check(!result && result.failure == BnkParser::Failure::invalidData
                && retained.state->events.size() == 1 && retained.state->events[0].displayName == "hint event"
                && retained.state->events[0].media.size() == 2 && retained.state->objects.size() == 2, message);
        };
        reject(wrap("<Event Id=\"10\" Name=\"first\"/><Event Id=\"10\" Name=\"second\"/>"),
            "duplicate event ID rejects the metadata transaction without last-writer publication");
        for (const auto id : { "4294967306", "10suffix", "-4294967286", "1e1", "+10" })
            reject(wrap("<Event Id=\"" + juce::String(id) + "\" Name=\"aliased\"/>"),
                "invalid numeric ID cannot wrap or partially parse into an existing event");
        reject(wrap("<Event Id=\"10\" Name=\"changed\"><IncludedMemoryFiles>"
            "<File Id=\"1\"><Path>hint.wav</Path></File><File Id=\"1\"><Path>other.wav</Path></File>"
            "</IncludedMemoryFiles></Event>"), "one media ID cannot acquire conflicting paths");
        reject("<SoundBanksInfo><SoundBanks><SoundBank><Path>shared-graph.bnk</Path><IncludedEvents><Event Id=\"10\" Name=\"first\"/></IncludedEvents></SoundBank>"
            "<SoundBank><Path>shared-graph.bnk</Path><IncludedEvents><Event Id=\"10\" Name=\"second\"/></IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>",
            "two matching bank definitions cannot be resolved by XML order");
        reject(wrap("<Event Id=\"10\" Name=\"changed\"><IncludedMemoryFiles><File Id=\"1\"><Path>hint.wav</Path><Path>other.wav</Path></File></IncludedMemoryFiles></Event>"),
            "duplicate scalar path does not hide a second media locator");
        reject(wrap("<Event Id=\"10\" Name=\"changed\"><IncludedMemoryFiles><File Id=\"1\"><Path>hint.wav</Path></File></IncludedMemoryFiles>"
            "<IncludedMemoryFiles><File Id=\"2\"><Path>other.wav</Path></File></IncludedMemoryFiles></Event>"),
            "duplicate metadata containers do not hide another definition");
        reject(wrap("<Event Id=\"10\" Name=\"first\"><IncludedMemoryFiles><File Id=\"1\"><Path>hint.wav</Path></File></IncludedMemoryFiles></Event>"
            "<Event Id=\"11\" Name=\"second\"><IncludedMemoryFiles><File Id=\"1\"><Path>other.wav</Path></File></IncludedMemoryFiles></Event>"),
            "conflicting media ID across two events cannot publish a partial first event");
        const auto identical = wrap("<Event Id=\"10\" Name=\"hint event\"><IncludedMemoryFiles>"
            "<File Id=\"4294967295\"><Path>hint.wav</Path></File><File Id=\"4294967295\"><Path>hint.wav</Path></File>"
            "</IncludedMemoryFiles></Event>");
        check(xml.replaceWithText(identical), "write valid uint32 maximum and identical repeated media");
        const auto accepted = BnkSidecarParser::enrichStateFromSidecars(bank, *retained.state, root);
        check(accepted && retained.state->events[0].media.size() == 1
            && retained.state->events[0].media[0].mediaId == UINT32_MAX,
            "uint32 maximum is preserved and identical references do not multiply media");
        const auto txt = bank.withFileExtension(".txt");
        check(txt.replaceWithText("Streamed Audio\tID\tName\tPath\n4294967297\talias\tignored\thint.wav\n"),
            "write actual TXT integer alias");
        const auto textRejected = BnkSidecarParser::enrichStateFromSidecars(bank, *retained.state, root);
        check(!textRejected && textRejected.failure == BnkParser::Failure::invalidData
            && retained.state->events[0].media[0].mediaId == UINT32_MAX,
            "text-sidecar numeric aliases also reject without replacing prior metadata");
        retained = {};
    }
    check(root.deleteRecursively(), "remove only private successful test fixture");
    std::cout << "BankParserHardeningTests passed (actual BNK event shapes/aggregate quota, shared DAG/cycle/cancellation, bounded RIFF/RF64/PCM/float/extensible reads, lease lifetime, metadata skip, chunk budget, actual sidecar hint integration/degradation, XML/TXT integer identity, duplicate definition rejection and identical media deduplication)\n";
}
