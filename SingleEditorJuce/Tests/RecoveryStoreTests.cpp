#include "Recovery/RecoveryStore.h"
#include "Recovery/AsyncRecoveryWriter.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <limits>
#include <map>
#include <type_traits>
#include <stdexcept>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#endif

using namespace singleeditor;
using namespace singleeditor::recovery;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void good(const Status& status, const char* message)
{
    if (!status) std::cerr << "Status: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
struct Fixture final
{
    juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    juce::File root = parent.getChildFile("SingleEditor-RecoveryTest-" + juce::Uuid().toString());
    juce::Uuid owner;
    juce::File directory() const { return root.getChildFile(owner.toString()); }
    ~Fixture()
    {
        // Only the uniquely owned, resolved temporary fixture may be deleted.
        require(root.isAChildOf(parent) && root.getParentDirectory() == parent
                    && root.getFileName().startsWith("SingleEditor-RecoveryTest-"), "fixture cleanup stays in owned temp directory");
        require(root.deleteRecursively(), "owned temporary fixture cleanup");
    }
};
AudioBlock audio(double rate = 48000, int channels = 2)
{
    AudioBlock result { rate, channels, 8, {} };
    juce::MemoryOutputStream samples;
    for (int i = 0; i < 8 * channels; ++i) samples.writeFloat(static_cast<float>(i - 8) / 32.0f);
    result.samples = samples.getMemoryBlock();
    return result;
}
CommitPacket packet()
{
    CommitPacket result;
    result.identity.document = { juce::Uuid(), juce::Uuid(), 1 };
    result.identity.source.binding = juce::Uuid();
    result.operationId = "test.committed-edit";
    result.planJson = R"({"version":1,"gain":-3})";
    result.stateJson = R"({"undo_parent":null,"graph":{"version":1}})";
    result.timestampMilliseconds = 1234;
    result.newAudio.push_back(audio());
    return result;
}
SafeHead append(RecoveryStore& store, const SafeHead& old, const CommitPacket& edit)
{
    SafeHead result;
    const auto status = store.append(old, edit, result);
    if (!status) std::cerr << "Append fixture root=" << store.rootDirectory().getFullPathName()
        << " expectedSequence=" << old.sequence << '\n';
    good(status, "publish complete recovery commit");
    return result;
}
juce::MemoryBlock load(const juce::File& file)
{
    juce::MemoryBlock result;
    require(file.loadFileAsData(result), "read owned test file");
    return result;
}
void replace(const juce::File& file, const juce::MemoryBlock& data)
{
    require(file.replaceWithData(data.getData(), data.getSize()), "write owned fault fixture");
}
juce::var sortObject(const juce::var& value)
{
    if (auto* input = value.getDynamicObject())
    {
        juce::StringArray names;
        for (const auto& entry : input->getProperties()) names.add(entry.name.toString());
        names.sort(false);
        auto* output = new juce::DynamicObject();
        for (const auto& name : names) output->setProperty(name, sortObject(input->getProperty(name)));
        return juce::var(output);
    }
    if (auto* input = value.getArray())
    {
        juce::Array<juce::var> output;
        for (const auto& item : *input) output.add(sortObject(item));
        return juce::var(output);
    }
    return value;
}
juce::MemoryBlock frameText(const juce::String& text)
{
    const juce::MemoryBlock payload(text.toRawUTF8(), text.getNumBytesAsUTF8());
    juce::MemoryOutputStream output;
    output.write("SER1", 4);
    output.writeInt(static_cast<int>(payload.getSize()));
    const auto hash = juce::SHA256(payload).getRawData();
    output.write(hash.getData(), hash.getSize());
    output.write(payload.getData(), payload.getSize());
    return output.getMemoryBlock();
}
juce::var jsonFile(const juce::File& file)
{
    const auto data = load(file);
    return juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data.getData()) + 40,
                                                 static_cast<int>(data.getSize() - 40)));
}
void jsonReplace(const juce::File& file, const juce::var& value)
{
    replace(file, frameText(juce::JSON::toString(sortObject(value), true)));
}

void audioPublicationCompatibility()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet(); edit.newAudio.clear();
    for (const auto& format : std::vector<AudioBlock> {
            { 44100.5, 1, 1, {} }, { 48000, 2, 4097, {} },
            { 96000, 8, 65537, {} }, { 192000, 64, 257, {} } })
    {
        auto block = format;
        juce::MemoryOutputStream pcm;
        for (int64_t frame = 0; frame < block.frames; ++frame)
            for (int channel = 0; channel < block.channels; ++channel)
                pcm.writeFloat(static_cast<float>((frame * 17 + channel * 31) % 8191 - 4095) / 8192.0f);
        block.samples = pcm.getMemoryBlock(); edit.newAudio.push_back(std::move(block));
    }
    // Independent contiguous schema-1 encoder, matching the previous shipped
    // representation. The production path must emit these exact bytes and IDs.
    std::map<juce::String, juce::MemoryBlock> legacyFiles;
    for (const auto& block : edit.newAudio)
    {
        auto* properties = new juce::DynamicObject();
        properties->setProperty("kind", "audio"); properties->setProperty("schema", 1);
        properties->setProperty("format", "interleaved-f32le"); properties->setProperty("rate", block.sampleRate);
        properties->setProperty("channels", block.channels); properties->setProperty("frames", block.frames);
        const auto json = juce::JSON::toString(sortObject(juce::var(properties)), true);
        juce::MemoryOutputStream payload;
        payload.writeInt(static_cast<int>(json.getNumBytesAsUTF8()));
        payload.write(json.toRawUTF8(), json.getNumBytesAsUTF8());
        payload.write(block.samples.getData(), block.samples.getSize());
        const juce::SHA256 hash(payload.getMemoryBlock());
        const auto id = hash.toHexString();
        juce::String identified;
        good(RecoveryStore::identifyAudio(block, Limits{}.assetBytes, identified), "identify borrowed canonical audio");
        require(identified == id, "segmented hash keeps the previous contiguous audio ID");
        juce::MemoryOutputStream framed;
        framed.write("SER1", 4); framed.writeInt(static_cast<int>(payload.getDataSize()));
        const auto checksum = hash.getRawData(); framed.write(checksum.getData(), checksum.getSize());
        framed.write(payload.getData(), payload.getDataSize());
        legacyFiles.emplace(id, framed.getMemoryBlock());
    }
    edit.newAudio.push_back(edit.newAudio.front());
    const auto first = append(store, {}, edit);
    require(fixture.directory().getChildFile("assets").getNumberOfChildFiles(juce::File::findFiles) == 4,
        "duplicate input audio retains content deduplication");
    for (const auto& [id, legacy] : legacyFiles)
    {
        require(load(fixture.directory().getChildFile("assets").getChildFile(id)) == legacy,
            "segmented durable publication is byte-identical to contiguous schema-1 framing");
        AudioBlock restored; good(store.readAudio(id, restored), "read published multichannel audio");
        require(std::any_of(edit.newAudio.begin(), edit.newAudio.end(), [&](const auto& original)
            { return restored.sampleRate == original.sampleRate && restored.channels == original.channels
                && restored.frames == original.frames && restored.samples == original.samples; }),
            "every canonical sample, channel and frame survives publication");
    }
    const auto headFile = fixture.directory().getChildFile("head");
    require(load(headFile) == frameText(juce::JSON::toString(sortObject(jsonFile(headFile)), true)),
        "metadata-only segmented publication preserves exact frame bytes");
    edit.identity.document.generation = 2;
    const auto second = append(store, first, edit);
    RecoveryStore reopened(fixture.root, fixture.owner);
    const auto view = reopened.inspect(); good(view.status, "cold Store fully verifies segmented publication");
    require(view.head == second && view.commits.size() == 2, "duplicate append and cold complete history remain readable");
    std::cout << "Recovery audio publication byte compatibility passed\n";
}

void manifestDiscovery()
{
    static_assert(!std::is_convertible_v<ManifestCommit, VerifiedCommit>);
    static_assert(!std::is_convertible_v<ManifestView, ReadView>);
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    const auto empty = store.inspectManifest();
    good(empty.status, "empty explicit manifest discovery succeeds");
    require(empty.commits.empty() && empty.head.sequence == 0 && !fixture.root.exists(), "empty discovery creates no owner or service");
    require(store.inspectManifest([] { return true; }).status.error == Error::cancelled && !fixture.root.exists(), "cancel before discovery has no filesystem effect");
    auto edit = packet();
    edit.newAudio.front().frames = 16384;
    edit.newAudio.front().samples.setSize(16384 * 2 * 4, true);
    const auto first = append(store, {}, edit);
    edit.identity.document.generation = 2;
    const auto second = append(store, first, edit);
    const auto captured = store.inspectManifest();
    const auto full = store.inspect();
    good(captured.status, "verify record chain without requiring audio preparation");
    good(full.status, "full certification still verifies all referenced audio");
    require(captured.head == second && captured.commits.size() == 2 && captured.commits.front().head == first
        && captured.verifiedRecordBytes == full.verifiedRecordBytes && full.verifiedBytes > captured.verifiedRecordBytes,
        "manifest bytes count only the authenticated journal while full view counts audio too");

    Limits discoveryBounds; discoveryBounds.recordBytes = discoveryBounds.readBytes = discoveryBounds.journalReadBytes = 8192;
    RecoveryStore bounded(fixture.root, fixture.owner, discoveryBounds);
    require(static_cast<bool>(bounded.inspectManifest().status) && !bounded.inspect().status,
        "bounded manifest succeeds without charging or reading a larger audio body; full validation does not weaken its budget");
    discoveryBounds.records = 1;
    require(RecoveryStore(fixture.root, fixture.owner, discoveryBounds).inspectManifest().status.error == Error::budgetExceeded,
        "manifest retains chain-count budget before replay");
    int polls = 0;
    const auto cancelled = store.inspectManifest([&] { return ++polls == 3; });
    require(cancelled.status.error == Error::cancelled && cancelled.commits.empty() && cancelled.head.sequence == 0,
        "mid-chain cancellation never returns a partial usable manifest");
    const auto throwing = store.inspectManifest([] () -> bool { throw 1; });
    require(throwing.status.error == Error::resourceOrEnvironmentFault && throwing.commits.empty(), "worker exceptions remain typed and unpublished");

    const auto assets = fixture.directory().getChildFile("assets");
    const auto heldAssets = fixture.directory().getChildFile("held-assets");
    require(assets.moveFileTo(heldAssets), "temporarily hide only this UUID fixture's asset directory");
    const auto missing = store.inspectManifest();
    require(static_cast<bool>(missing.status) && missing.head == second && missing.verifiedRecordBytes == captured.verifiedRecordBytes,
        "manifest discovery does not even require the asset directory");
    require(!store.inspect().status, "missing audio can never become a fully verified view");
    SafeHead rejected;
    require(!store.append(second, edit, rejected) && rejected == second,
        "publication still requires the complete existing asset closure, not just its manifest");
    require(heldAssets.moveFileTo(assets), "restore only the hidden owned fixture directory");

    const auto headFile = fixture.directory().getChildFile("head");
    const auto headBytes = load(headFile);
    auto alteredHead = jsonFile(headFile);
    alteredHead.getDynamicObject()->setProperty("owner", juce::Uuid().toString());
    jsonReplace(headFile, alteredHead);
    const auto foreign = store.inspectManifest();
    require(!foreign.status && foreign.commits.empty() && foreign.head.sequence == 0, "checksum-valid foreign owner is refused by discovery");
    replace(headFile, headBytes);
    const auto recordFile = fixture.directory().getChildFile("commits").getChildFile(first.hash);
    const auto recordBytes = load(recordFile);
    require(recordFile.replaceWithText("torn owned journal fixture"), "tear only an older private record");
    const auto torn = store.inspectManifest();
    require(!torn.status && torn.commits.empty() && torn.head.sequence == 0, "valid newest record never hides a corrupt ancestor");
    replace(recordFile, recordBytes);
    edit.identity.document.generation = 3;
    const auto third = append(store, second, edit);
    require(store.inspectManifest().head == third && captured.head == second && captured.commits.size() == 2,
        "discovery freezes the captured head rather than following later publications");
    std::cout << "RecoveryManifestTests passed\n";
}

void headFence()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    good(store.checkHead({}), "absent expected head remains absent without activation");
    require(!fixture.root.exists() && store.checkHead({ -1, {} }).error == Error::invalidPacket
        && store.checkHead({ 0, "invalid" }).error == Error::invalidPacket, "head fence validates tokens without filesystem mutation");
    auto edit = packet();
    const auto first = append(store, {}, edit);
    good(store.checkHead(first), "frozen head matches publication");
    require(store.checkHead({}).error == Error::staleHead, "new publication invalidates an empty observation");
    const auto commits = fixture.directory().getChildFile("commits");
    const auto assets = fixture.directory().getChildFile("assets");
    const auto heldCommits = fixture.directory().getChildFile("held-commits");
    const auto heldAssets = fixture.directory().getChildFile("held-assets");
    require(commits.moveFileTo(heldCommits) && assets.moveFileTo(heldAssets), "hide only this fixture's record and audio directories");
    good(store.checkHead(first), "head fence never requires or reads the record/audio directories");
    require(!store.inspectManifest().status, "head fence does not certify a missing chain");
    require(heldCommits.moveFileTo(commits) && heldAssets.moveFileTo(assets), "restore only this fixture's hidden directories");
    const auto file = fixture.directory().getChildFile("head");
    const auto saved = load(file);
    require(file.deleteFile(), "remove only private head to test disappearance");
    require(store.checkHead(first).error == Error::staleHead, "disappeared head invalidates candidate");
    replace(file, saved);
    auto altered = jsonFile(file);
    altered.getDynamicObject()->setProperty("owner", juce::Uuid().toString());
    jsonReplace(file, altered);
    require(store.checkHead(first).error == Error::corruptJournal, "valid checksum cannot hide foreign head ownership");
    replace(file, saved);
    altered = jsonFile(file); altered.getDynamicObject()->setProperty("schema", 99);
    jsonReplace(file, altered);
    require(store.checkHead(first).error == Error::unsupportedSchema, "head fence preserves typed unsupported schema");
    replace(file, saved);
    require(file.replaceWithText("torn private head"), "tear private head");
    require(!store.checkHead(first), "head fence refuses torn publication");
    replace(file, saved);
    edit.identity.document.generation = 2;
    const auto second = append(store, first, edit);
    require(store.checkHead(first).error == Error::staleHead, "later publication invalidates earlier frozen head");
    good(store.checkHead(second), "new explicit attempt may use the new verified head");
    std::cout << "RecoveryHeadFenceTests passed\n";
}

void identityAndRoundtrip()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    require(!fixture.root.exists(), "store construction has no filesystem side effect");
    const auto empty = store.inspect();
    good(empty.status, "explicit empty inspect succeeds");
    require(empty.head.sequence == 0 && !fixture.root.exists(), "read-only inspect does not create owner");
    auto edit = packet();
    edit.newAudio.push_back(edit.newAudio.front());
    const auto first = append(store, {}, edit);
    auto view = store.inspect();
    good(view.status, "verify first head");
    require(view.head == first && view.commits.size() == 1, "head references one verified commit");
    require(view.commits.front().record["plan"].toString() == edit.planJson
                && view.commits.front().record["state"].toString() == edit.stateJson,
            "immutable processor plan and Undo graph bytes are retained exactly");
    const auto assets = view.commits.front().record["assets"];
    require(assets.getArray()->size() == 1, "identical canonical sample blocks deduplicate");
    const auto assetId = assets[0].toString();
    AudioBlock decoded;
    good(store.readAudio(assetId, decoded), "asset read validates frame, format and checksum");
    require(decoded.samples == edit.newAudio.front().samples && decoded.sampleRate == 48000 && decoded.channels == 2,
            "canonical PCM bytes and format roundtrip");
    edit.newAudio.clear();
    edit.existingAudio = { assetId };
    edit.identity.document.generation = 2;
    edit.operationId = "test.delete";
    const auto second = append(store, first, edit);
    edit.identity.document.generation = 1; // Undo is not a monotonic content generation.
    edit.identity.source.binding = juce::Uuid(); // Explicit Save As keeps the document lineage.
    edit.operationId = "test.undo-after-save-as";
    const auto third = append(store, second, edit);
    view = store.inspect();
    good(view.status, "multi-commit chain verifies");
    require(view.head == third && view.commits.size() == 3 && view.commits[1].head == second
                && static_cast<int64_t>(view.commits.back().record["document"]["generation"]) == 1,
            "sequence remains monotonic across Undo and source rebind");
    SafeHead ignored;
    require(store.append(first, edit, ignored).error == Error::staleHead, "stale writer cannot replace a newer head");
    edit.identity.document.lineage = juce::Uuid();
    require(store.append(third, edit, ignored).error == Error::staleHead, "different document cannot reuse recovery owner");
    require(store.inspect().head == third, "failed writes preserve complete safe head");

    Fixture changed;
    RecoveryStore other(changed.root, changed.owner);
    auto differentlyFormatted = packet();
    differentlyFormatted.newAudio.front().sampleRate = 96000;
    append(other, {}, differentlyFormatted);
    require(other.inspect().commits.front().record["assets"][0].toString() != assetId,
            "CAS identity includes sample format facts, not only bytes");
    std::cout << "Recovery identity/PCM/chain roundtrip passed\n";
}

void hostileFramesAndReferences()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    const auto first = append(store, {}, edit);
    const auto headFile = fixture.directory().getChildFile("head");
    const auto original = load(headFile);
    auto damaged = original;
    static_cast<char*>(damaged.getData())[damaged.getSize() - 1] ^= 1;
    replace(headFile, damaged);
    require(store.inspect().status.error == Error::corruptJournal, "changed frame bytes fail checksum");
    replace(headFile, juce::MemoryBlock(original.getData(), original.getSize() - 3));
    require(store.inspect().status.error == Error::corruptJournal, "torn head rejected");
    replace(headFile, original);
    auto header = jsonFile(headFile);
    header.getDynamicObject()->setProperty("schema", 2);
    jsonReplace(headFile, header);
    require(store.inspect().status.error == Error::unsupportedSchema, "unknown required schema fails closed");
    replace(headFile, original);
    header = jsonFile(headFile);
    header.getDynamicObject()->setProperty("unexpected_required", true);
    jsonReplace(headFile, header);
    require(!store.inspect().status, "unknown head field rejected");
    replace(headFile, original);
    header = jsonFile(headFile);
    header.getDynamicObject()->setProperty("hash", "../../outside");
    jsonReplace(headFile, header);
    require(!store.inspect().status, "untrusted path token is never resolved");
    replace(headFile, original);
    header = jsonFile(headFile);
    header.getDynamicObject()->setProperty("sequence", true);
    jsonReplace(headFile, header);
    require(!store.inspect().status, "boolean cannot masquerade as integer sequence");
    replace(headFile, original);
    const auto view = store.inspect();
    const auto id = view.commits.front().record["assets"][0].toString();
    const auto assetFile = fixture.directory().getChildFile("assets").getChildFile(id);
    const auto asset = load(assetFile);
    require(assetFile.deleteFile(), "remove only owned asset fixture");
    require(store.inspect().status.error == Error::missingOrCorruptAsset, "missing asset prevents partial restore view");
    replace(assetFile, juce::MemoryBlock(asset.getData(), asset.getSize() - 1));
    require(store.inspect().status.error == Error::missingOrCorruptAsset, "torn asset prevents restore");
    replace(assetFile, asset);
    const auto recordFile = fixture.directory().getChildFile("commits").getChildFile(first.hash);
    const auto record = load(recordFile);
    replace(recordFile, juce::MemoryBlock(record.getData(), 8));
    require(!store.inspect().status && store.inspect().commits.empty(), "incomplete commit yields no usable restore state");
    replace(recordFile, record);
    replace(fixture.directory().getChildFile("commits").getChildFile("orphan-tail"), juce::MemoryBlock("partial", 7));
    good(store.inspect().status, "unpublished orphan tail is ignored");
    AudioBlock sentinel = audio();
    const auto prior = sentinel.samples;
    require(!store.readAudio("../outside", sentinel) && sentinel.samples == prior,
            "invalid asset access cannot partially overwrite caller output");
    std::cout << "Recovery hostile frames/references passed\n";
}

void streamedAudioPublication()
{
    Fixture eagerFixture, streamedFixture;
    RecoveryStore eager(eagerFixture.root, eagerFixture.owner), streamed(streamedFixture.root, streamedFixture.owner);
    const auto original = packet();
    append(eager, {}, original);
    auto edit = original;
    const auto block = edit.newAudio.front();
    juce::String id; good(RecoveryStore::identifyAudio(block, Limits{}.assetBytes, id), "identify independently prepared source audio");
    edit.newAudio.clear();
    int reads = 0;
    edit.streamedAudio.push_back({ id, block.sampleRate, block.channels, block.frames,
        [block, &reads](AudioBlock& output) { ++reads; output = block; return Status{}; } });
    const auto first = append(streamed, {}, edit);
    require(reads == 1 && load(eagerFixture.directory().getChildFile("assets").getChildFile(id))
        == load(streamedFixture.directory().getChildFile("assets").getChildFile(id)),
        "streamed and eager publication produce identical existing schema asset bytes");
    const auto second = append(streamed, first, edit);
    require(reads == 2, "streamed declarations remain checked when the destination asset already exists");
    const auto oldHead = load(streamedFixture.directory().getChildFile("head"));
    edit.streamedAudio.front().read = [block](AudioBlock& output)
    { output = block; static_cast<unsigned char*>(output.samples.getData())[0] ^= 1; return Status{}; };
    SafeHead refused;
    require(streamed.append(second, edit, refused).code == "streamed_audio_changed" && refused == second,
        "regenerated audio must still match its captured identity even for an existing asset");
    edit.streamedAudio.front().read = [](AudioBlock&) { return Status { Error::cancelled, "capture_cancelled" }; };
    require(streamed.append(second, edit, refused).error == Error::cancelled && refused == second,
        "cancelled streaming cannot publish a head");
    edit.streamedAudio.front().read = [](AudioBlock&) -> Status { throw std::runtime_error("owned read failure"); };
    require(streamed.append(second, edit, refused).code == "streamed_audio_read_exception" && refused == second,
        "source read exceptions are contained before head publication");
    require(load(streamedFixture.directory().getChildFile("head")) == oldHead && streamed.inspect().head == second,
        "all refused streaming attempts keep the actual old protected head and data");
    edit.streamedAudio.front().read = [block](AudioBlock& output) { output = block; return Status{}; };
    append(streamed, second, edit);
    std::cout << "Recovery streamed publication passed (identical bytes, bounded regeneration, real old-head preservation and Retry)\n";
}

void budgetsAndPublishFailure()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    const auto first = append(store, {}, edit);
    Limits low;
    low.ownerBytes = 1;
    RecoveryStore quota(fixture.root, fixture.owner, low);
    SafeHead unchanged;
    require(quota.append(first, edit, unchanged).error == Error::budgetExceeded && store.inspect().head == first,
            "quota failure retains previous protected head");
    low = {};
    low.freeSpaceReserve = std::numeric_limits<int64_t>::max();
    RecoveryStore reserve(fixture.root, fixture.owner, low);
    require(reserve.append(first, edit, unchanged).error == Error::budgetExceeded && store.inspect().head == first,
            "low-space reserve rejects publication without removing live data");
    const auto headFile = fixture.directory().getChildFile("head");
    require(headFile.setReadOnly(true), "make owned head replacement fail");
    const auto failed = store.append(first, edit, unchanged);
    require(headFile.setReadOnly(false), "restore owned fixture access");
    require(failed.error == Error::resourceOrEnvironmentFault && unchanged == first && store.inspect().head == first,
            "replace failure cannot advance claimed or durable safe head");
    const auto second = append(store, first, edit);
    require(store.inspect().head == second, "explicit retry safely reuses complete orphan commit");

    Fixture invalid;
    RecoveryStore fresh(invalid.root, invalid.owner);
    edit.planJson = "{";
    require(!fresh.append({}, edit, unchanged) && !invalid.root.exists(), "malformed packet creates no storage");
    edit = packet();
    edit.planJson = "{}";
    for (int i = 0; i < 30; ++i) edit.planJson = "{\"nested\":" + edit.planJson + "}";
    require(!fresh.append({}, edit, unchanged) && !invalid.root.exists(), "depth bound applies before recursive parser");
    edit = packet();
    edit.newAudio.front().frames = std::numeric_limits<int64_t>::max();
    require(!fresh.append({}, edit, unchanged) && !invalid.root.exists(), "frame multiplication overflow is rejected");
    edit = packet();
    static_cast<unsigned char*>(edit.newAudio.front().samples.getData())[3] = 0x7f;
    static_cast<unsigned char*>(edit.newAudio.front().samples.getData())[2] = 0x80;
    require(!fresh.append({}, edit, unchanged) && !invalid.root.exists(), "nonfinite sample is rejected");
    std::cout << "Recovery budgets/publish failure passed\n";
}

void semanticCorruptionAndReadBudgets()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    const auto first = append(store, {}, edit);
    edit.identity.document.generation = 2;
    const auto second = append(store, first, edit);
    const auto headFile = fixture.directory().getChildFile("head");
    const auto protectedHead = load(headFile);
    const auto original = jsonFile(fixture.directory().getChildFile("commits").getChildFile(second.hash));
    const auto tamper = [&] (const std::function<void(juce::var&)>& change)
    {
        auto record = original.clone();
        change(record);
        const auto text = juce::JSON::toString(sortObject(record), true);
        const juce::MemoryBlock payload(text.toRawUTF8(), text.getNumBytesAsUTF8());
        const auto hash = juce::SHA256(payload).toHexString();
        replace(fixture.directory().getChildFile("commits").getChildFile(hash), frameText(text));
        auto head = jsonFile(headFile);
        head.getDynamicObject()->setProperty("hash", hash);
        jsonReplace(headFile, head);
        const auto result = store.inspect();
        replace(headFile, protectedHead);
        return result.status;
    };
    require(tamper([] (juce::var& record) { record["document"].getDynamicObject()->setProperty("lineage", juce::Uuid().toString()); }).code
                == "document_chain", "valid checksums do not permit a foreign document in parent chain");
    require(tamper([] (juce::var& record)
        {
            const auto parent = record["parent"].toString();
            record.getDynamicObject()->setProperty("parent", parent.replaceSection(0, 1, parent.startsWithChar('0') ? "1" : "0"));
        }).error
                == Error::corruptJournal, "missing parent is a corrupt chain, not an accepted partial replay");
    require(tamper([] (juce::var& record) { record.getDynamicObject()->setProperty("plan", "{\"changed\":true}"); }).code
                == "plan_or_state_bytes", "independent immutable plan hash detects plan substitution");
    require(tamper([] (juce::var& record) { record.getDynamicObject()->setProperty("unknown_required", 1); }).error
                == Error::unsupportedSchema, "unknown required commit fields are not silently ignored");
    require(store.inspect().head == second, "semantic fault inspection never changes protected files");

    for (const bool journalOnly : { false, true })
    {
        Fixture bounded;
        Limits limits;
        limits.recordBytes = 4096;
        if (journalOnly) limits.journalReadBytes = 4096;
        else limits.readBytes = 4096;
        RecoveryStore limited(bounded.root, bounded.owner, limits);
        auto largePlan = packet();
        largePlan.planJson = "{\"description\":\"" + juce::String::repeatedString("x", 2200) + "\"}";
        const auto one = append(limited, {}, largePlan);
        SafeHead unchanged;
        require(limited.append(one, largePlan, unchanged).error == Error::budgetExceeded,
                "writer cannot publish a chain its bounded reader could not inspect");
        require(limited.inspect().head == one, "read budget failure preserves previous complete prefix");
    }
    std::cout << "Recovery semantic corruption and aggregate read budgets passed\n";
}

void competingWriters()
{
    Fixture fixture;
    RecoveryStore left(fixture.root, fixture.owner), right(fixture.root, fixture.owner);
    const auto edit = packet();
    std::atomic<int> ready { 0 };
    Status a, b;
    auto writer = [&] (RecoveryStore& store, Status& status)
    {
        ++ready;
        while (ready.load() != 2) std::this_thread::yield();
        SafeHead head;
        status = store.append({}, edit, head);
    };
    std::thread first(writer, std::ref(left), std::ref(a)), second(writer, std::ref(right), std::ref(b));
    first.join(); second.join();
    require(static_cast<bool>(a) != static_cast<bool>(b), "exclusive owner write plus expected head allows one publisher");
    const auto view = left.inspect();
    good(view.status, "competing writer leaves one valid chain");
    require(view.head.sequence == 1 && view.commits.size() == 1, "concurrent writer cannot branch or overwrite safe head");
    std::cout << "Recovery competing writers passed\n";
}

void appendReadWorkload()
{
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    edit.newAudio.front().frames = 16384;
    edit.newAudio.front().samples.setSize(16384 * 2 * 4, true);
    SafeHead head;
    AppendWork work;
    good(store.append({}, edit, head, nullptr, &work), "first workload publication");
    require(work.hashedAudioFiles == 1 && work.audioPayloadBytesRead > 131072,
            "first publication verifies the actual canonical CAS bytes");
    const auto full = store.inspect();
    good(full.status, "independent workload certification");
    edit.existingAudio.push_back(full.commits.front().record["assets"][0].toString());
    edit.newAudio.clear();
    size_t audioBytes = 0, recordBytes = 0, entries = 0;
    for (int i = 0; i < 20; ++i)
    {
        ++edit.identity.document.generation;
        const auto expected = head;
        good(store.append(expected, edit, head, nullptr, &work), "metadata-only workload append");
        require(work.audioPayloadBytesRead == 0 && work.hashedAudioFiles == 0 && work.reusedAudioFiles == 1,
                "twenty real later appends reuse only the unchanged leased CAS proof");
        audioBytes += work.audioPayloadBytesRead;
        recordBytes += work.journalPayloadBytesRead;
        entries += work.ownerEntriesScanned;
    }
    const auto verified = store.inspect();
    good(verified.status, "workload remains fully inspectable");
    require(verified.head == head && head.sequence == 21, "all workload edits are actually published");
    std::cout << "Recovery append workload: edits=20 audio_payload_bytes=" << audioBytes
              << " journal_payload_bytes=" << recordBytes << " owner_entries=" << entries << '\n';
}

void appendProofFaults()
{
#if JUCE_WINDOWS
    Fixture fixture;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    edit.newAudio.front().frames = 16384;
    edit.newAudio.front().samples.setSize(16384 * 2 * 4, true);
    SafeHead head;
    AppendWork work;
    std::shared_ptr<const PublishedRecoveryCommit> receipt;
    good(store.append({}, edit, head, &receipt, &work), "publish initial CAS proof");
    const auto first = head;
    const auto id = *receipt->assetIds().begin();
    const auto asset = fixture.directory().getChildFile("assets").getChildFile(id);
    const auto assetBytes = load(asset);
    const auto payloadBytes = assetBytes.getSize() - 40;
    edit.newAudio.clear();
    edit.existingAudio = { id };

    const auto next = [&]
    {
        const auto expected = head;
        ++edit.identity.document.generation;
        good(store.append(expected, edit, head, &receipt, &work), "append current owned proof fixture");
    };
    bool checkedLease = false;
    store.onBoundary = [&] (RecoveryStore::Boundary point)
    {
        if (point != RecoveryStore::Boundary::beforeHeadReplace) return;
        const auto writer = CreateFileW(asset.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto deniedWrite = writer == INVALID_HANDLE_VALUE;
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        require(deniedWrite && !DeleteFileW(asset.getFullPathName().toWideCharPointer()),
                "leased CAS cannot be overwritten or removed before publication");
        const auto destination = asset.getSiblingFile("owned-lease-rename");
        require(!MoveFileExW(asset.getFullPathName().toWideCharPointer(), destination.getFullPathName().toWideCharPointer(), 0),
                "leased CAS name remains bound through head replacement");
        checkedLease = true;
    };
    next();
    require(checkedLease && work.audioPayloadBytesRead == 0, "real cached append retains its short dependency lease");
    store.onBoundary = {};
    const auto released = CreateFileW(asset.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(released != INVALID_HANDLE_VALUE, "successful append retains no CAS handle between edits");
    CloseHandle(released);

    RecoveryStore restarted(fixture.root, fixture.owner);
    auto expected = head;
    good(restarted.append(expected, edit, head, nullptr, &work), "cold writer revalidates actual existing closure");
    require(work.audioPayloadBytesRead == payloadBytes && work.hashedAudioFiles == 1 && work.reusedAudioFiles == 0,
            "another Store does not inherit publication proof from a manifest or file name");
    SafeHead rejected;
    const auto oldReceipt = receipt;
    require(store.append(expected, edit, rejected, &receipt, &work).error == Error::staleHead
        && rejected == expected && receipt == oldReceipt, "external publication rejects stale write without replacing receipt");
    next();
    require(work.audioPayloadBytesRead == payloadBytes && work.reusedAudioFiles == 0,
            "externally advanced head invalidates old in-memory publication frontier");
    next();
    require(work.audioPayloadBytesRead == 0 && work.reusedAudioFiles == 1, "successful cold verification admits next exact version");

    struct FileFacts final { FILE_ID_INFO id {}; FILE_BASIC_INFO basic {}; LARGE_INTEGER size {}; };
    const auto facts = [&]
    {
        const auto handle = CreateFileW(asset.getFullPathName().toWideCharPointer(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        FileFacts result;
        require(handle != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(handle, FileIdInfo, &result.id, sizeof(result.id))
            && GetFileInformationByHandleEx(handle, FileBasicInfo, &result.basic, sizeof(result.basic))
            && GetFileSizeEx(handle, &result.size), "observe actual private CAS physical version");
        CloseHandle(handle);
        return result;
    };
    const auto beforeRewrite = facts();
    const auto writer = CreateFileW(asset.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "open own CAS for in-place restamped corruption");
    LARGE_INTEGER offset; offset.QuadPart = static_cast<LONGLONG>(assetBytes.getSize() - 1);
    unsigned char corrupt = static_cast<const unsigned char*>(assetBytes.getData())[assetBytes.getSize() - 1] ^ 1;
    DWORD written = 0;
    FILETIME originalTime { static_cast<DWORD>(beforeRewrite.basic.LastWriteTime.QuadPart),
                           static_cast<DWORD>(static_cast<uint64_t>(beforeRewrite.basic.LastWriteTime.QuadPart) >> 32) };
    require(SetFilePointerEx(writer, offset, nullptr, FILE_BEGIN) && WriteFile(writer, &corrupt, 1, &written, nullptr)
        && written == 1 && FlushFileBuffers(writer) && SetFileTime(writer, nullptr, nullptr, &originalTime),
        "change actual sample byte while restoring public modification time");
    CloseHandle(writer);
    const auto afterRewrite = facts();
    require(std::memcmp(&beforeRewrite.id.FileId, &afterRewrite.id.FileId, sizeof(FILE_ID_128)) == 0
        && beforeRewrite.size.QuadPart == afterRewrite.size.QuadPart
        && beforeRewrite.basic.LastWriteTime.QuadPart == afterRewrite.basic.LastWriteTime.QuadPart
        && beforeRewrite.basic.ChangeTime.QuadPart != afterRewrite.basic.ChangeTime.QuadPart,
        "fixture proves same physical file, size and mtime with changed Windows ChangeTime");
    expected = head;
    const auto protectedReceipt = receipt;
    require(store.append(expected, edit, rejected, &receipt, &work).error == Error::missingOrCorruptAsset
        && rejected == head && receipt == protectedReceipt && work.hashedAudioFiles == 1,
        "restamped in-place mutation is rehashed and cannot advance protected publication");
    good(store.checkHead(head), "in-place corruption does not modify safe head");

    const auto replaceAtSameName = [&] (const juce::MemoryBlock& bytes)
    {
        const auto prior = facts();
        const auto temporary = asset.getSiblingFile("owned-replacement-" + juce::Uuid().toString());
        replace(temporary, bytes);
        require(MoveFileExW(temporary.getFullPathName().toWideCharPointer(), asset.getFullPathName().toWideCharPointer(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "replace only private CAS name with another physical file");
        const auto handle = CreateFileW(asset.getFullPathName().toWideCharPointer(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE && SetFileTime(handle, nullptr, nullptr, &originalTime), "restore replacement's public mtime");
        CloseHandle(handle);
        const auto after = facts();
        require(std::memcmp(&prior.id.FileId, &after.id.FileId, sizeof(FILE_ID_128)) != 0
            && after.size.QuadPart == beforeRewrite.size.QuadPart
            && after.basic.LastWriteTime.QuadPart == beforeRewrite.basic.LastWriteTime.QuadPart,
            "replacement fixture has a new file ID despite identical path, size and mtime");
    };
    auto changed = assetBytes;
    static_cast<unsigned char*>(changed.getData())[changed.getSize() - 1] ^= 2;
    replaceAtSameName(changed);
    require(store.append(head, edit, rejected, nullptr, &work).error == Error::missingOrCorruptAsset && rejected == head,
            "same-name replacement never reuses old content proof");
    replaceAtSameName(assetBytes);
    next();
    require(work.audioPayloadBytesRead == payloadBytes && work.hashedAudioFiles == 1,
            "byte-identical repaired file must earn a new physical-version proof");
    next();
    require(work.audioPayloadBytesRead == 0, "repaired and published exact version is reusable");

    const auto busy = CreateFileW(asset.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(busy != INVALID_HANDLE_VALUE, "hold writable CAS fixture before append");
    const auto busyResult = store.append(head, edit, rejected, nullptr, &work);
    CloseHandle(busy);
    require(busyResult.error == Error::resourceOrEnvironmentFault && rejected == head,
            "busy writable file cannot bypass exclusive read lease using cached facts");
    next();

    // New audio published before a failed head replacement is an orphan, not an
    // admitted cache entry. Its retry must read and validate the actual body again.
    auto secondAudio = audio(96000);
    secondAudio.frames = 16384;
    secondAudio.samples.setSize(16384 * 2 * 4, true);
    edit.newAudio = { secondAudio };
    edit.existingAudio.clear();
    juce::String secondId;
    good(RecoveryStore::identifyAudio(secondAudio, Limits{}.assetBytes, secondId), "identify generated recovery PCM");
    HANDLE heldHead = INVALID_HANDLE_VALUE;
    store.onBoundary = [&] (RecoveryStore::Boundary point)
    {
        if (point != RecoveryStore::Boundary::beforeHeadReplace) return;
        heldHead = CreateFileW(fixture.directory().getChildFile("head").getFullPathName().toWideCharPointer(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(heldHead != INVALID_HANDLE_VALUE, "hold old head to fail actual atomic replacement");
    };
    expected = head;
    const auto receiptBeforeFailure = receipt;
    const auto failed = store.append(expected, edit, rejected, &receipt, &work);
    if (heldHead != INVALID_HANDLE_VALUE) CloseHandle(heldHead);
    store.onBoundary = {};
    require(!failed && rejected == head && receipt == receiptBeforeFailure && work.hashedAudioFiles == 1,
            "failed durable head does not promote newly verified orphan audio or receipt");
    good(store.checkHead(head), "failed head replacement preserves published prefix");
    next();
    require(work.hashedAudioFiles == 1 && work.audioPayloadBytesRead > 131072 && work.reusedAudioFiles == 1,
            "retry rehashes orphan audio while reusing still-published historical asset");
    edit.newAudio.clear();
    edit.existingAudio = { secondId };
    next();
    require(work.audioPayloadBytesRead == 0 && work.reusedAudioFiles == 2,
            "historical-only and current CAS versions are both leased without rereading");
    replaceAtSameName(changed);
    require(store.append(head, edit, rejected).error == Error::missingOrCorruptAsset && rejected == head,
            "corrupt historical-only audio still prevents publishing a falsely healthy chain");
    replaceAtSameName(assetBytes);
    next();
    const auto record = fixture.directory().getChildFile("commits").getChildFile(first.hash);
    const auto savedRecord = load(record);
    require(record.replaceWithText("torn historical record"), "tear only private old record");
    require(store.append(head, edit, rejected).error == Error::corruptJournal && rejected == head,
            "asset proof never skips old journal hash and schema validation");
    replace(record, savedRecord);
    next();
    const auto certified = store.inspect();
    good(certified.status, "independent full content certification after proof fault and repair matrix");
    require(certified.head == head && certified.verifiedBytes > 2 * 131072,
            "full inspect still reads historical and current audio regardless of append cache");
    {
        Fixture bounded;
        Limits limits; limits.ownerBytes = 1024 * 1024;
        RecoveryStore quota(bounded.root, bounded.owner, limits);
        auto quotaEdit = packet();
        const auto safe = append(quota, {}, quotaEdit);
        const auto view = quota.inspect();
        good(view.status, "quota fixture baseline is complete");
        quotaEdit.existingAudio = { view.commits.front().record["assets"][0].toString() };
        quotaEdit.newAudio.clear();
        const auto orphan = bounded.directory().getChildFile("staging-owned-quota-fixture");
        replace(orphan, juce::MemoryBlock(800 * 1024, true));
        const auto over = quota.append(safe, quotaEdit, rejected, nullptr, &work);
        require(over.error == Error::budgetExceeded && rejected == safe && work.audioPayloadBytesRead == 0
            && work.ownerEntriesScanned > 0, "warm reuse still accounts real orphan files and enforces owner quota");
        require(orphan.deleteFile(), "remove only explicitly owned quota fixture");
        good(quota.append(safe, quotaEdit, rejected), "quota repair allows retry from same complete head");
    }
    std::cout << "Recovery append asset proofs passed (leases, cold/head changes, restamped rewrite, replacement, busy, failed publication, old audio/records)\n";
#endif
}

void asynchronousWriter()
{
    using namespace std::chrono_literals;
    {
        Fixture sharedFixture;
        AsyncRecoveryWriter::Budget sharedBudget; sharedBudget.retainedBytes = 4500; sharedBudget.maximumAge = 10s;
        AsyncRecoveryWriter sharedWriter(sharedFixture.root, sharedFixture.owner, {}, sharedBudget);
        auto backing = std::make_shared<juce::MemoryBlock>(4096, true);
        std::weak_ptr<const void> lifetime = backing;
        juce::WaitableEvent firstEntered, firstRelease, secondEntered, secondRelease;
        const auto base = packet();
        const auto submission = [&](int64_t generation, std::shared_ptr<const void> identity, std::function<void()> gate = {})
        {
            AsyncRecoveryWriter::Submission result;
            result.retainedBytes = 4224; // 4096 actual shared bytes plus 128 capture metadata.
            result.sharedBackings.push_back({ std::move(identity), 4096 });
            result.prepare = [base, generation, gate = std::move(gate)](CommitPacket& output)
            { if (gate) gate(); output = base; output.identity.document.generation = generation; return Status{}; };
            return result;
        };
        good(sharedWriter.submit(submission(1, backing, [&]
            { firstEntered.signal(); require(firstRelease.wait(5000), "release first shared capture"); })), "accept first shared backing");
        require(firstEntered.wait(5000), "first shared capture is actually retained by worker");
        good(sharedWriter.submit(submission(2, backing, [&]
            { secondEntered.signal(); require(secondRelease.wait(5000), "release second shared capture"); })), "accept same live backing without duplicate PCM charge");
        require(sharedWriter.progress().pendingPackets == 2 && sharedWriter.progress().retainedBytes == 4352,
            "two metadata captures retain one actual immutable backing");
        size_t projected = 42;
        require(sharedWriter.projectedRetainedBytes(submission(3, std::make_shared<juce::MemoryBlock>(4096, true)), projected).code == "queue_budget"
            && projected == 42, "equal contents in a different allocation do not share the memory charge");
        good(sharedWriter.projectedRetainedBytes(submission(3, backing), projected), "same-backing preflight preserves independent metadata charge");
        require(projected == 4480, "third metadata capture is still charged");
        AsyncRecoveryWriter::Submission metadata; metadata.retainedBytes = 200;
        const auto memoryRefusal = sharedWriter.projectedRetainedBytes(metadata, projected);
        require(memoryRefusal.code == "queue_budget"
            && sharedWriter.progress().pendingPackets == 2 && !sharedWriter.progress().checkpointRequired,
            "metadata remains bounded and a read-only projection cannot admit or break continuity");
        require(memoryRefusal.capacity.resource == ByteCapacity::Resource::retainedMemory
            && memoryRefusal.capacity.required == 4552 && memoryRefusal.capacity.limit == 4500
            && memoryRefusal.capacity.lowerBound, "writer reports the observed shared queue plus refused metadata requirement");
        firstRelease.signal(); require(secondEntered.wait(5000), "first shared packet publishes before second prepares");
        require(sharedWriter.progress().pendingPackets == 1 && sharedWriter.progress().retainedBytes == 4224,
            "dequeue of the first reference keeps the backing charged to the remaining capture");
        secondRelease.signal(); require(sharedWriter.drain(5s), "shared captures publish the complete ordered prefix");
        backing.reset();
        require(sharedWriter.progress().retainedBytes == 0 && sharedWriter.progress().safeHead.sequence == 2 && lifetime.expired(),
            "last publication releases both retained accounting and the actual backing lifetime");
    }
    Fixture fixture;
    AsyncRecoveryWriter::Budget budget;
    budget.maximumAge = 10s;
    AsyncRecoveryWriter writer(fixture.root, fixture.owner, {}, budget);
    require(!writer.progress().workerRunning && !fixture.root.exists(), "async writer is inert before mutation");
    juce::WaitableEvent entered, release;
    const auto callerThread = std::this_thread::get_id();
    auto edit = packet();
    auto first = edit;
    auto second = edit;
    second.identity.document.generation = 2;
    good(writer.submit({ 128, [&, first] (CommitPacket& output)
    {
        require(std::this_thread::get_id() != callerThread, "preparation executes off caller thread");
        entered.signal();
        require(release.wait(5000), "bounded test preparation wait");
        output = first;
        return Status {};
    } }), "enqueue first immutable mutation");
    require(entered.wait(5000), "worker entered first preparation");
    good(writer.submit({ 128, [second] (CommitPacket& output) { output = second; return Status {}; } }),
         "caller can enqueue while writer is blocked");
    require(writer.progress().pendingPackets == 2 && writer.progress().safeHead.sequence == 0,
            "queued or active mutation is never claimed durable");
    require(!writer.progress().protectedIdentity.isValid(), "blocked preparation cannot acknowledge a protected Document version");
    require(!writer.drain(1ms), "drain returns within caller timeout while I/O is blocked");
    release.signal();
    require(writer.drain(5s), "ordered queue drains after worker is released");
    const auto view = RecoveryStore(fixture.root, fixture.owner).inspect();
    good(view.status, "async head verifies through real storage");
    require(view.head.sequence == 2 && static_cast<int64_t>(view.commits.back().record["document"]["generation"]) == 2,
            "async writer preserves order and immutable captures");
    require(!writer.progress().workerRunning && writer.progress().retainedBytes == 0,
            "idle writer releases thread ownership and retained captures");
    require(writer.progress().protectedIdentity.sameVersion(second.identity.document), "published head acknowledges its exact Document identity");
    writer.close();
    require(!writer.submit({ 1, [] (CommitPacket&) { return Status {}; } }), "closed owner rejects new submissions");

    Fixture retryFixture;
    AsyncRecoveryWriter retry(retryFixture.root, retryFixture.owner, {}, budget);
    std::atomic<int> attempts { 0 };
    good(retry.submit({ 1, [edit, &attempts] (CommitPacket& output)
    {
        if (++attempts == 1) return Status { Error::resourceOrEnvironmentFault, "injected_prepare_failure" };
        output = edit;
        return Status {};
    } }), "accept a mutation that later fails preparation");
    require(!retry.drain(5s) && attempts == 1 && retry.progress().safeHead.sequence == 0
                && retry.progress().pendingPackets == 1, "failure retains front mutation without automatic retry");
    good(retry.retry(), "explicit retry accepted");
    require(retry.drain(5s) && attempts == 2 && retry.progress().safeHead.sequence == 1,
            "explicit retry protects the retained mutation exactly once");

    Fixture gapFixture;
    budget.packets = 1;
    AsyncRecoveryWriter gap(gapFixture.root, gapFixture.owner, {}, budget);
    juce::WaitableEvent gapEntered, gapRelease;
    good(gap.submit({ 1, [&] (CommitPacket& output)
    {
        gapEntered.signal();
        require(gapRelease.wait(5000), "bounded gap fixture wait");
        output = edit;
        return Status {};
    } }), "accept prefix before overflow");
    require(gapEntered.wait(5000), "gap fixture entered");
    require(!gap.submit({ 1, [edit] (CommitPacket& output) { output = edit; return Status {}; } })
                && gap.progress().checkpointRequired, "rejected committed mutation marks discontinuity");
    gapRelease.signal();
    require(!gap.drain(5s) && gap.progress().safeHead.sequence == 1,
            "accepted prefix remains protected while missing tail stays degraded");
    require(!gap.retry() && !gap.submit({ 1, [] (CommitPacket&) { return Status {}; } }),
            "cannot silently resume after a missing mutation without a new checkpoint");

    Fixture lifetimeFixture;
    juce::WaitableEvent detachedEntered, detachedRelease, detachedDone;
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weakLifetime = lifetime;
    auto detached = std::make_unique<AsyncRecoveryWriter>(lifetimeFixture.root, lifetimeFixture.owner, Limits {}, budget);
    good(detached->submit({ 1, [&, lifetime] (CommitPacket& output)
    {
        detachedEntered.signal();
        require(detachedRelease.wait(5000) && *lifetime == 42, "captured state survives timed-out controller shutdown");
        output = edit;
        detachedDone.signal();
        return Status {};
    } }), "submit retained state for controller lifetime test");
    require(detachedEntered.wait(5000), "detached worker entered");
    detached.reset();
    lifetime.reset();
    require(!weakLifetime.expired(), "nonblocking destructor preserves in-flight immutable captures");
    detachedRelease.signal();
    require(detachedDone.wait(5000), "detached capture released by caller");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!weakLifetime.expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(weakLifetime.expired(), "finished detached worker releases capture ownership");
    require(RecoveryStore(lifetimeFixture.root, lifetimeFixture.owner).inspect().head.sequence == 1,
            "controller destruction does not corrupt accepted mutation publication");

    Fixture ageFixture;
    budget.maximumAge = 1s;
    AsyncRecoveryWriter aged(ageFixture.root, ageFixture.owner, {}, budget);
    std::atomic<int> ageAttempts { 0 };
    good(aged.submit({ 1, [edit, &ageAttempts] (CommitPacket& output)
    {
        if (++ageAttempts == 1) std::this_thread::sleep_for(1100ms);
        output = edit;
        return Status {};
    } }), "submit bounded-age mutation");
    require(!aged.drain(5s) && aged.progress().failure.code == "writer_lag" && aged.progress().safeHead.sequence == 0,
            "over-age preparation cannot advance protected head silently");
    good(aged.retry(), "explicit retry acknowledges delayed protection");
    require(aged.drain(5s) && ageAttempts == 2 && aged.progress().safeHead.sequence == 1,
            "retry preserves the same delayed mutation");
    Fixture publicationFixture;
    budget = AsyncRecoveryWriter::Budget {};
    budget.maximumAge = 10s;
    budget.packets = 2;
    AsyncRecoveryWriter publication(publicationFixture.root, publicationFixture.owner, {}, budget);
    juce::WaitableEvent publicationEntered, publicationRelease;
    std::atomic<int> publicationCalls { 0 };
    good(publication.submit({ 1, [&] (CommitPacket& output)
    {
        publicationEntered.signal();
        require(publicationRelease.wait(5000), "bounded publication fault preparation wait");
        output = first; return Status {};
    }, [&] (std::shared_ptr<const PublishedRecoveryCommit> receipt) -> Status
    {
        ++publicationCalls;
        require(receipt && receipt->ownerId() == publicationFixture.owner
            && receipt->documentIdentity().sameVersion(first.identity.document), "publication callback receives real Store receipt");
        throw std::runtime_error("owned publication consumer failure");
    } }), "submit real publication followed by consumer failure");
    require(publicationEntered.wait(5000), "hold first actual packet while queueing its tail");
    good(publication.submit({ 1, [second] (CommitPacket& output) { output = second; return Status {}; } }), "enqueue tail before publication consumer failure");
    require(publication.submit({ 1, [second] (CommitPacket& output) { output = second; return Status {}; } }).error == Error::budgetExceeded,
        "actual third submission creates an admission gap before publication consumer fails");
    publicationRelease.signal();
    require(!publication.drain(5s) && publicationCalls == 1 && publication.progress().safeHead.sequence == 1
        && publication.progress().protectedIdentity.sameVersion(first.identity.document)
        && publication.progress().pendingPackets == 1 && publication.progress().checkpointRequired
        && publication.progress().failure.code == "publication_consumer_exception",
        "consumer exception preserves actual published prefix but poisons and stops the unacknowledged tail");
    require(publication.retry().code == "checkpoint_required"
        && RecoveryStore(publicationFixture.root, publicationFixture.owner).inspect().head.sequence == 1,
        "consumer failure cannot duplicate the durable commit or skip to its tail");
    publication.close();
    require(publication.retryDraining().code == "checkpoint_required" && publication.progress().pendingPackets == 1,
        "publication failure revokes accepted-prefix retry even when an earlier admission gap existed");
    std::cout << "Recovery asynchronous order, lifetime, retry, publication and queue-gap tests passed\n";
}

juce::String createOrphan(RecoveryStore& store, const SafeHead& head, CommitPacket edit)
{
    juce::String id;
    good(RecoveryStore::identifyAudio(edit.newAudio.front(), Limits{}.assetBytes, id), "identify private future orphan");
    store.onBoundary = [] (RecoveryStore::Boundary boundary)
    { if (boundary == RecoveryStore::Boundary::beforeHeadReplace) throw std::runtime_error("owned publication interruption"); };
    bool interrupted = false;
    try { SafeHead unused; store.append(head, edit, unused); }
    catch (const std::runtime_error&) { interrupted = true; }
    store.onBoundary = {};
    require(interrupted && store.inspect().head == head, "actual failed publication leaves orphan CAS and complete protected head");
    return id;
}

void orphanCollection()
{
    {
        Fixture fixture;
        RecoveryStore store(fixture.root, fixture.owner);
        OrphanCollectionReport report;
        good(store.collectOrphans({}, report), "empty explicit maintenance has no storage side effect");
        require(!fixture.root.exists(), "no owner, marker or quarantine from empty maintenance");
        require(store.collectOrphans({}, report, {}, [] { return true; }).error == Error::cancelled && !fixture.root.exists(),
                "cancelled maintenance is inert before any I/O");
        auto edit = packet();
        const auto first = append(store, {}, edit);
        edit.identity.document.generation = 2; edit.newAudio.front().sampleRate = 96000;
        const auto second = append(store, first, edit);
        const auto protectedView = store.inspect(); good(protectedView.status, "freeze all historical/current protection references");
        const auto headFile = fixture.directory().getChildFile("head");
        const auto headBytes = load(headFile);
        require(headFile.deleteFile(), "remove only private head to exercise uncertain reference ownership");
        require(store.collectOrphans({}, report).code == "collection_headless_records" && report.filesRemoved == 0,
                "missing head never makes retained historical audio look unreferenced");
        replace(headFile, headBytes);
        std::map<juce::String, juce::MemoryBlock> originals;
        for (const auto& record : protectedView.commits)
            for (const auto& id : *record.record["assets"].getArray())
                originals[id.toString()] = load(fixture.directory().getChildFile("assets").getChildFile(id.toString()));
        edit.identity.document.generation = 3; edit.newAudio.front().sampleRate = 192000;
        const auto orphan = createOrphan(store, second, edit);
        const auto orphanFile = fixture.directory().getChildFile("assets").getChildFile(orphan);
        OrphanCollectionBudget insufficient; insufficient.bytes = Limits{}.recordBytes - 1;
        require(store.collectOrphans(second, report, insufficient).error == Error::budgetExceeded
            && !report.more && report.filesRemoved == 0 && report.bytesRead == 0
            && orphanFile.existsAsFile() && store.inspect().head == second,
            "insufficient control-scan budget stops without requesting an endless no-progress maintenance retry");
        const auto failedStage = fixture.directory().getChildFile("staging-" + juce::Uuid().toString());
        require(failedStage.replaceWithText("owned partial staging bytes"), "make a recognized failed stage beside live owner");
        const auto unknown = fixture.directory().getChildFile("user-unknown-file");
        require(unknown.replaceWithText("must retain unknown data"), "create an unrecognized private sentinel");
        bool checkedLock = false;
        store.onCollectionBoundary = [&] (RecoveryStore::CollectionBoundary boundary)
        {
            if (boundary != RecoveryStore::CollectionBoundary::marked) return;
            RecoveryStore competing(fixture.root, fixture.owner);
            SafeHead unchanged;
            require(competing.append(second, edit, unchanged).code == "global_budget_busy" && unchanged == second,
                    "GC excludes competing mutation admission at the shared root lock before its owner lock");
            checkedLock = true;
        };
        OrphanCollectionBudget one; one.files = 1;
        good(store.collectOrphans(second, report, one), "first bounded maintenance pass");
        require(checkedLock && report.filesRemoved == 1 && report.more && !failedStage.exists() && orphanFile.existsAsFile(),
                "failed staging is collected before orphan CAS and file budget yields explicitly");
        store.onCollectionBoundary = {};
        good(store.collectOrphans(second, report), "next bounded pass collects actual orphan CAS");
        require(report.filesRemoved == 1 && !orphanFile.exists() && unknown.loadFileAsString() == "must retain unknown data",
                "only recognized manifest-unreferenced recovery objects are removed");
        for (const auto& [id, saved] : originals)
            require(load(fixture.directory().getChildFile("assets").getChildFile(id)) == saved, "all historical/current PCM remains bit-exact");
        require(store.inspect().head == second && store.inspect().commits.size() == 2, "GC preserves exact safe head and every canonical historical record");
        const auto third = append(store, second, edit);
        require(store.inspect().head == third && orphanFile.existsAsFile(), "original failed packet can regenerate cleaned CAS and publish normally");
    }
    {
        Fixture fixture;
        RecoveryStore store(fixture.root, fixture.owner);
        auto edit = packet(); const auto head = append(store, {}, edit);
        edit.newAudio.front().sampleRate = 96000;
        const auto id = createOrphan(store, head, edit);
        bool cancelled = false;
        store.onCollectionBoundary = [&] (RecoveryStore::CollectionBoundary boundary)
        { if (boundary == RecoveryStore::CollectionBoundary::quarantined) cancelled = true; };
        OrphanCollectionReport report;
        require(store.collectOrphans(head, report, {}, [&] { return cancelled; }).error == Error::cancelled
            && report.filesRemoved == 0, "cancel after quarantine retains durable plan and complete object");
        store.onCollectionBoundary = {};
        require(!fixture.directory().getChildFile("assets").getChildFile(id).exists()
            && fixture.directory().getChildFile("quarantine").findChildFiles(juce::File::findFiles, false).size() == 2,
            "cancelled GC leaves an independently resumable plan/item pair");
        const auto next = append(store, head, edit); // Recreates same CAS name with a new live reference.
        good(store.collectOrphans(next, report), "new generation revalidates old plan against current manifest");
        require(report.filesRemoved == 0 && report.retained == 1 && store.inspect().head == next
            && fixture.directory().getChildFile("assets").getChildFile(id).existsAsFile(),
            "newly referenced CAS name is retained even when an older GC plan selected it");
    }
    {
        Fixture fixture;
        RecoveryStore store(fixture.root, fixture.owner);
        auto edit = packet(); const auto first = append(store, {}, edit);
        const auto oldHeadBytes = load(fixture.directory().getChildFile("head"));
        edit.identity.document.generation = 2; const auto head = append(store, first, edit);
        const auto headBytes = load(fixture.directory().getChildFile("head"));
        edit.newAudio.front().sampleRate = 96000;
        createOrphan(store, head, edit);
        store.onCollectionBoundary = [&] (RecoveryStore::CollectionBoundary boundary)
        { if (boundary == RecoveryStore::CollectionBoundary::beforeDelete) replace(fixture.directory().getChildFile("head"), oldHeadBytes); };
        OrphanCollectionReport report;
        require(store.collectOrphans(head, report).error == Error::staleHead && report.filesRemoved == 0,
                "final generation fence refuses deletion after actual head substitution");
        store.onCollectionBoundary = {};
        replace(fixture.directory().getChildFile("head"), headBytes);
        const auto plans = fixture.directory().getChildFile("quarantine").findChildFiles(juce::File::findFiles, false, "plan-*");
        require(plans.size() == 1, "stale generation retains a persisted resume plan");
        const auto originalPlan = load(plans[0]);
        auto future = jsonFile(plans[0]); future.getDynamicObject()->setProperty("schema", 99); jsonReplace(plans[0], future);
        require(store.collectOrphans(head, report).error == Error::unsupportedSchema && report.filesRemoved == 0,
                "unknown collection schema is never guessed into a delete operation");
        replace(plans[0], originalPlan);
        auto traversal = jsonFile(plans[0]); traversal.getDynamicObject()->setProperty("name", "../../outside"); jsonReplace(plans[0], traversal);
        require(store.collectOrphans(head, report).error == Error::corruptJournal && report.filesRemoved == 0,
                "canonical framed collection plan cannot escape its owner with a forged source path");
        replace(plans[0], originalPlan);
        good(store.collectOrphans(head, report), "repaired exact plan resumes after stale head fault");
        require(report.filesRemoved == 1 && store.inspect().head == head,
                "resume deletes the verified quarantined object and preserves active head");
    }
#if JUCE_WINDOWS
    {
        Fixture fixture, outside;
        RecoveryStore store(fixture.root, fixture.owner);
        auto edit = packet(); const auto head = append(store, {}, edit);
        edit.newAudio.front().sampleRate = 96000;
        createOrphan(store, head, edit);
        bool cancelled = false;
        store.onCollectionBoundary = [&] (RecoveryStore::CollectionBoundary point)
        { if (point == RecoveryStore::CollectionBoundary::quarantined) cancelled = true; };
        OrphanCollectionReport report;
        require(store.collectOrphans(head, report, {}, [&] { return cancelled; }).error == Error::cancelled,
                "prepare interrupted quarantine for independent content revalidation");
        store.onCollectionBoundary = {};
        const auto items = fixture.directory().getChildFile("quarantine").findChildFiles(juce::File::findFiles, false, "item-*");
        require(items.size() == 1, "one immutable plan identifies one quarantined file");
        const auto saved = load(items[0]);
        const auto rewrite = [&] (bool damage)
        {
            const auto handle = CreateFileW(items[0].getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            FILETIME modified {};
            require(handle != INVALID_HANDLE_VALUE && GetFileTime(handle, nullptr, nullptr, &modified), "open owned quarantine for restamped in-place fault");
            auto data = saved;
            if (damage) static_cast<unsigned char*>(data.getData())[data.getSize() - 1] ^= 1;
            DWORD written = 0;
            require(WriteFile(handle, data.getData(), static_cast<DWORD>(data.getSize()), &written, nullptr)
                && written == data.getSize() && FlushFileBuffers(handle) && SetFileTime(handle, nullptr, nullptr, &modified),
                "change quarantined bytes while preserving its physical file and public modification time");
            CloseHandle(handle);
        };
        rewrite(true);
        good(store.collectOrphans(head, report), "uncertain quarantined object is retained with diagnostics");
        size_t inspectedControls = 0;
        for (const auto& file : fixture.directory().getChildFile("commits").findChildFiles(juce::File::findFiles, false))
            if (file.getFileName() != head.hash)
                inspectedControls += load(file).getSize() - 40; // SER1 framing; unpublished records remain preserved.
        require(report.filesRemoved == 0 && report.retained == 1 && report.bytesRead == saved.getSize() + inspectedControls && items[0].existsAsFile(),
                "resume hashes actual quarantined content and accounts for separate unpublished-control inspection");
        rewrite(false);
        good(store.collectOrphans(head, report), "exact original quarantined content may be collected on explicit retry");
        require(report.filesRemoved == 1 && !items[0].exists(), "repaired exact object completes original persisted plan");
        require(outside.root.createDirectory().wasOk(), "create independent owned sentinel directory");
        const auto external = outside.root.getChildFile("external-data");
        require(external.replaceWithText("outside bytes must survive"), "create outside sentinel before shared-link fault");
        const auto linked = fixture.directory().getChildFile("staging-" + juce::Uuid().toString());
        require(CreateHardLinkW(linked.getFullPathName().toWideCharPointer(), external.getFullPathName().toWideCharPointer(), nullptr),
                "create real shared physical file under recognizable staging name");
        require(store.collectOrphans(head, report).error == Error::resourceOrEnvironmentFault
            && linked.existsAsFile() && external.loadFileAsString() == "outside bytes must survive",
            "collector refuses shared physical ownership instead of trusting an owner-local name");
        require(linked.deleteFile(), "remove only explicitly created test hardlink before independent fixture cleanup");
    }
#endif
    std::cout << "Recovery orphan collection passed (bounded order, full history, writer exclusion, resume, changed references, stale/future plans)\n";
}

int collectionCrashChild(const juce::File& root, juce::Uuid owner, int stage)
{
    RecoveryStore store(root, owner);
    const auto view = store.inspect(); good(view.status, "collection child verifies active prefix");
    store.onCollectionBoundary = [stage] (RecoveryStore::CollectionBoundary point)
    {
        if (static_cast<int>(point) == stage)
        {
#if JUCE_WINDOWS
            TerminateProcess(GetCurrentProcess(), 86);
#else
            std::_Exit(86);
#endif
        }
    };
    OrphanCollectionReport report;
    good(store.collectOrphans(view.head, report), "collection child reaches selected fault boundary");
    return 2;
}
void collectionCrashMatrix()
{
    for (int stage = 0; stage < 5; ++stage)
    {
        Fixture fixture;
        RecoveryStore store(fixture.root, fixture.owner);
        auto edit = packet(); const auto head = append(store, {}, edit);
        edit.newAudio.front().sampleRate = 96000;
        const auto id = createOrphan(store, head, edit);
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--gc-crash", fixture.root.getFullPathName(), fixture.owner.toString(), juce::String(stage) }), "start owned GC fault child");
        require(child.waitForProcessToFinish(10000) && child.getExitCode() == 86, "GC child terminates at real durability boundary");
        const auto view = store.inspect(); good(view.status, "GC process crash leaves complete original playable prefix");
        require(view.head == head, "GC crash cannot change published recovery head");
        OrphanCollectionReport report;
        good(store.collectOrphans(head, report), "new invocation resumes only persisted owned collection plan");
        require(!fixture.directory().getChildFile("assets").getChildFile(id).exists()
            && fixture.directory().getChildFile("quarantine").findChildFiles(juce::File::findFiles, false).isEmpty(),
            "all five crash boundaries converge without losing protected data or leaving planned garbage");
    }
    std::cout << "Recovery orphan collection abrupt-process matrix passed (5 boundaries)\n";
}

void directoryContainment()
{
#if JUCE_WINDOWS
    Fixture fixture, outside;
    RecoveryStore store(fixture.root, fixture.owner);
    auto edit = packet();
    bool attemptedSwap = false, blockedSwap = false;
    store.onBoundary = [&] (RecoveryStore::Boundary point)
    {
        if (point != RecoveryStore::Boundary::assetPublished) return;
        attemptedSwap = true;
        blockedSwap = !MoveFileExW(fixture.directory().getChildFile("assets").getFullPathName().toWideCharPointer(),
                                   fixture.directory().getChildFile("old-assets").getFullPathName().toWideCharPointer(), 0);
    };
    const auto head = append(store, {}, edit);
    require(attemptedSwap && blockedSwap, "pinned ancestors prevent directory swap during publication");
    const auto id = store.inspect().commits.front().record["assets"][0].toString();
    const auto assets = fixture.directory().getChildFile("assets");
    const auto saved = fixture.directory().getChildFile("saved-assets");
    require(assets.moveFileTo(saved), "save only owned asset directory for junction fixture");
    require(assets.createDirectory().wasOk() && outside.root.createDirectory().wasOk(), "create owned junction directories");
    const auto target = outside.root.getChildFile("untouched.txt");
    require(target.replaceWithText("outside sentinel"), "create outside sentinel");
    const auto substitute = juce::String("\\??\\") + outside.root.getFullPathName();
    const auto display = outside.root.getFullPathName();
    const auto substituteBytes = static_cast<USHORT>(substitute.length() * 2);
    const auto displayBytes = static_cast<USHORT>(display.length() * 2);
    // Mount-point reparse buffer, including both UTF-16 null terminators.
    juce::MemoryBlock buffer(static_cast<size_t>(20 + substituteBytes + displayBytes), true);
    auto* raw = static_cast<unsigned char*>(buffer.getData());
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT dataLength = static_cast<USHORT>(12 + substituteBytes + displayBytes);
    const USHORT displayOffset = static_cast<USHORT>(substituteBytes + 2);
    std::memcpy(raw, &tag, 4);
    std::memcpy(raw + 4, &dataLength, 2);
    std::memcpy(raw + 10, &substituteBytes, 2);
    std::memcpy(raw + 12, &displayOffset, 2);
    std::memcpy(raw + 14, &displayBytes, 2);
    std::memcpy(raw + 16, substitute.toWideCharPointer(), substituteBytes);
    std::memcpy(raw + 16 + displayOffset, display.toWideCharPointer(), displayBytes);
    const auto handle = CreateFileW(assets.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open owned directory for junction fixture");
    DWORD returned = 0;
    const auto installed = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.getData(),
                                           static_cast<DWORD>(buffer.getSize()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    require(installed != FALSE, "install real directory-junction fixture");
    const auto inspected = store.inspect();
    SafeHead unchanged;
    const auto written = store.append(head, edit, unchanged);
    OrphanCollectionReport report;
    const auto collected = store.collectOrphans(head, report);
    AudioBlock output;
    const auto read = store.readAudio(id, output);
    // Remove the junction itself before any recursive fixture cleanup.
    require(RemoveDirectoryW(assets.getFullPathName().toWideCharPointer()) != FALSE, "remove owned junction, not its target");
    require(saved.moveFileTo(assets), "restore owned asset directory");
    require(!inspected.status && !written && !read && !collected, "junction cannot redirect inspect, publish, collection or asset reads");
    require(target.loadFileAsString() == "outside sentinel" && store.inspect().head == head,
            "outside data and protected recovery head remain untouched");
    std::cout << "Recovery real-junction and directory-swap containment passed\n";
#endif
}

Limits globalLimits()
{
    Limits result; result.recordBytes = 4096; result.globalBytes = 100000;
    return result;
}
CommitPacket globalPacket()
{
    auto result = packet();
    result.newAudio.front().frames = 8192;
    result.newAudio.front().samples.setSize(8192 * 2 * 4, true);
    return result;
}
int globalQuotaChild(const juce::File& root, const juce::Uuid& owner, const juce::File& signals, bool crash)
{
    RecoveryStore store(root, owner, globalLimits());
    store.onBoundary = [&] (auto boundary)
    {
        if (boundary != RecoveryStore::Boundary::assetPublished) return;
        if (crash) std::_Exit(91);
        require(signals.getChildFile("ready").replaceWithText("asset published; root lock held"), "signal only owned global quota fixture");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
        while (!signals.getChildFile("release").existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline)
            juce::Thread::sleep(2);
        require(signals.getChildFile("release").existsAsFile(), "parent releases real competing quota writer");
    };
    append(store, {}, globalPacket());
    return 0;
}
void globalQuotaTests()
{
    {
        Fixture fixture; const auto limits = globalLimits();
        RecoveryStore first(fixture.root, fixture.owner, limits);
        require(!fixture.root.exists(), "global budget construction never creates or scans recovery root");
        const auto edit = globalPacket(); AppendWork work; SafeHead head;
        good(first.append({}, edit, head, nullptr, &work), "first actual owner fits global root budget");
        require(work.globalEntriesScanned > 0 && fixture.root.getChildFile("quota-policy").existsAsFile(), "actual append establishes bounded root policy and accounting");
        const auto savedHead = load(fixture.directory().getChildFile("head"));
        const juce::Uuid secondOwner; RecoveryStore second(fixture.root, secondOwner, limits);
        SafeHead unchanged; work = {};
        const auto capacityRefusal = second.append({}, globalPacket(), unchanged, nullptr, &work);
        require(capacityRefusal.code == "global_byte_budget"
            && unchanged.sequence == 0 && work.globalBytesObserved > 65536,
            "different owner cannot pass independent per-owner budgets beyond actual aggregate storage");
        require(capacityRefusal.capacity.resource == ByteCapacity::Resource::globalStorage
            && capacityRefusal.capacity.required > static_cast<uint64_t>(work.globalBytesObserved)
            && capacityRefusal.capacity.limit == static_cast<uint64_t>(limits.globalBytes)
            && !capacityRefusal.capacity.lowerBound,
            "global refusal reports observed total including reserved new work and the actual root cap");
        auto higher = limits; higher.globalBytes = 1000000;
        RecoveryStore largerCaller(fixture.root, secondOwner, higher);
        require(largerCaller.append({}, globalPacket(), unchanged).code == "global_byte_budget",
            "another process configuration cannot silently raise existing physical root policy");
        require(first.inspect().head == head && load(fixture.directory().getChildFile("head")) == savedHead
            && !fixture.root.getChildFile(secondOwner.toString()).getChildFile("head").exists(),
            "global refusal preserves existing protected bytes and never invents a new safe head");
        require(!fixture.root.getChildFile(secondOwner.toString()).exists(),
            "global refusal admits no empty owner directories or writer infrastructure");
        const auto originalEntries = fixture.root.findChildFiles(juce::File::findFilesAndDirectories, true).size();
        for (int i = 0; i < 24; ++i)
        {
            const juce::Uuid rejectedOwner;
            RecoveryStore rejected(fixture.root, rejectedOwner, limits);
            require(rejected.append({}, globalPacket(), unchanged).code == "global_byte_budget"
                && unchanged.sequence == 0 && !fixture.root.getChildFile(rejectedOwner.toString()).exists()
                && fixture.root.findChildFiles(juce::File::findFilesAndDirectories, true).size() == originalEntries,
                "repeated distinct rejected owners cannot grow empty infrastructure under global pressure");
        }
    }
    for (int mode = 0; mode < 4; ++mode)
    {
        Fixture fixture; auto limits = globalLimits();
        require(fixture.root.createDirectory().wasOk(), "create owned global accounting boundary fixture");
        const auto unknown = fixture.root.getChildFile("preserve-unknown");
        if (mode == 0) { juce::MemoryBlock bytes(40000, true); replace(unknown, bytes); }
        if (mode == 1) limits.globalEntries = 7;
        if (mode == 2) require(fixture.root.getChildFile("quota.lock").replaceWithText("unknown lock data"), "inject nonempty quota lock without deleting it");
        if (mode == 3) require(fixture.root.getChildFile("quota-policy").replaceWithText("torn future policy"), "inject unusable policy");
        RecoveryStore store(fixture.root, fixture.owner, limits); SafeHead head;
        const auto result = store.append({}, globalPacket(), head);
        require(!result && head.sequence == 0 && !fixture.directory().getChildFile("head").exists(), "unknown/entry/lock/policy failure prevents all safe-head advancement");
        if (mode == 0) require(result.code == "global_byte_budget" && unknown.getSize() == 40000, "unknown root data consumes budget and is never deleted");
        if (mode == 1) require(result.code == "global_entry_budget", "reserve new assets record head and staging within bounded entry count");
        if (mode < 2) require(!fixture.directory().exists(), "byte and entry refusal both precede new owner directory creation");
        if (mode == 2) require(result.code == "global_lock_invalid" && fixture.root.getChildFile("quota.lock").loadFileAsString() == "unknown lock data", "invalid existing quota lock is preserved");
        if (mode == 3) require(fixture.root.getChildFile("quota-policy").loadFileAsString() == "torn future policy", "unknown policy is never reset to caller defaults");
    }
    for (int crash = 0; crash < 2; ++crash)
    {
        Fixture fixture; require(fixture.root.createDirectory().wasOk(), "create only owned cross-process quota signals directory");
        const auto root = fixture.root.getChildFile("storage"); const juce::Uuid childOwner, otherOwner;
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--global-quota-child", root.getFullPathName(), childOwner.toString(), fixture.root.getFullPathName(), juce::String(crash) }),
            "start real independent writer process in private fixture");
        RecoveryStore other(root, otherOwner, globalLimits()); SafeHead head;
        if (crash == 0)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
            while (!fixture.root.getChildFile("ready").existsAsFile() && child.isRunning() && juce::Time::getMillisecondCounterHiRes() < deadline)
                juce::Thread::sleep(2);
            require(fixture.root.getChildFile("ready").existsAsFile() && child.isRunning(), "child really holds root quota lock after asset publication");
            require(other.append({}, globalPacket(), head).code == "global_budget_busy" && head.sequence == 0,
                "different process cannot admit against a concurrent writer's pre-publication capacity snapshot");
            require(fixture.root.getChildFile("release").replaceWithText("release own writer"), "release only the owned child write");
        }
        require(child.waitForProcessToFinish(15000) && child.getExitCode() == (crash ? 91u : 0u), "owned writer completes or really dies with quota lock held");
        require(other.append({}, globalPacket(), head).code == "global_byte_budget" && head.sequence == 0,
            "root lock releases after completion/death but published or orphan asset still consumes capacity");
        RecoveryStore original(root, childOwner);
        const auto view = original.inspect(); good(view.status, "original owner remains readable after competing global admission");
        require(view.head.sequence == (crash ? 0 : 1), "only actual published original head is recoverable after quota contention or crash");
    }
    for (int mode = 0; mode < 7; ++mode)
    {
        Fixture fixture; auto limits = globalLimits();
        const auto root = fixture.root.getChildFile("storage");
        require(root.createDirectory().wasOk(), "create private unusual global inventory fixture");
        RecoveryStore store(root, fixture.owner, limits); SafeHead head; AppendWork work;
        if (mode == 0)
        {
            auto directory = root;
            for (int i = 0; i < 10; ++i) { directory = directory.getChildFile("nested"); require(directory.createDirectory().wasOk(), "create bounded-depth fault fixture"); }
            require(store.append({}, globalPacket(), head, nullptr, &work).code == "global_depth_budget" && directory.isDirectory(),
                "deep unknown tree is preserved and cannot cause unbounded recursive inventory");
        }
        if (mode == 1)
        {
            for (int i = 0; i < 9; ++i) require(root.getChildFile("unknown-" + juce::String(i)).replaceWithText("retain"), "create counted unknown fixture entry");
            limits.globalEntries = 7; RecoveryStore bounded(root, fixture.owner, limits);
            require(bounded.append({}, globalPacket(), head, nullptr, &work).code == "global_entry_budget" && work.globalEntriesScanned == 8,
                "inventory stops at entry bound without reading entire unknown tree");
        }
        if (mode == 2)
        {
            append(store, {}, globalPacket());
            const auto policy = root.getChildFile("quota-policy"); auto future = jsonFile(policy);
            future.getDynamicObject()->setProperty("schema", 99); jsonReplace(policy, future); const auto original = load(policy);
            const juce::Uuid anotherOwner; RecoveryStore another(root, anotherOwner, limits);
            require(another.append({}, globalPacket(), head).error == Error::unsupportedSchema && load(policy) == original,
                "valid-checksum future root policy remains unchanged and cannot fall back to permissive defaults");
        }
#if JUCE_WINDOWS
        if (mode == 3)
        {
            const auto outside = fixture.root.getChildFile("outside-original"); require(outside.replaceWithText("external owned sentinel"), "create only owned external hardlink sentinel");
            const auto inside = root.getChildFile("unknown-link");
            require(CreateHardLinkW(inside.getFullPathName().toWideCharPointer(), outside.getFullPathName().toWideCharPointer(), nullptr), "install actual cross-root hardlink fixture");
            require(store.append({}, globalPacket(), head).code == "global_inventory_file"
                && outside.loadFileAsString() == "external owned sentinel" && inside.existsAsFile(),
                "hardlinked unknown content blocks inventory without changing either path or external data");
        }
        if (mode == 4)
        {
            const auto unknown = root.getChildFile("active-unknown"); require(unknown.replaceWithText("held external writer"), "create owned concurrent unknown writer fixture");
            const auto writer = CreateFileW(unknown.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            require(writer != INVALID_HANDLE_VALUE, "hold real unknown file writer");
            const auto status = store.append({}, globalPacket(), head); CloseHandle(writer);
            require(status.code == "global_inventory_entry" && head.sequence == 0 && unknown.loadFileAsString() == "held external writer",
                "concurrent ungoverned file writer prevents a false stable capacity claim");
            append(store, {}, globalPacket());
        }
        if (mode == 5 || mode == 6)
        {
            const auto directory = root.getChildFile(fixture.owner.toString());
            require(directory.createDirectory().wasOk(), "create only owned lock validation fixture directory");
            const auto ownerLock = directory.getChildFile("writer.lock");
            const auto outside = fixture.root.getChildFile("outside-lock");
            if (mode == 5) require(ownerLock.replaceWithText("unknown pre-existing lock data"), "create nonempty owner lock fixture");
            else
            {
                require(outside.create().wasOk() && CreateHardLinkW(ownerLock.getFullPathName().toWideCharPointer(),
                    outside.getFullPathName().toWideCharPointer(), nullptr), "create real empty hardlinked owner lock");
            }
            require(store.append({}, globalPacket(), head).code == "owner_lock_invalid" && ownerLock.existsAsFile()
                && (mode == 5 ? ownerLock.loadFileAsString() == "unknown pre-existing lock data" : outside.existsAsFile()),
                "untrusted existing lock is preserved before any delete disposition or quota exemption");
        }
#endif
    }
    std::cout << "Recovery global quota tests passed (actual aggregate capacity, fixed policy, bounded entries, unknown preservation, competing processes and crash unlock)\n";
}

int crashChild(const juce::File& root, const juce::Uuid& owner, int stage)
{
    RecoveryStore store(root, owner);
    const auto view = store.inspect();
    good(view.status, "child verifies baseline");
    auto edit = packet();
    const auto doc = view.commits.back().record["document"];
    edit.identity.document = { juce::Uuid(doc["lineage"].toString()), juce::Uuid(doc["runtime_instance"].toString()), 2 };
    edit.newAudio.front().sampleRate = 96000;
    store.onBoundary = [stage] (RecoveryStore::Boundary boundary)
    {
        if (static_cast<int>(boundary) == stage)
        {
#if JUCE_WINDOWS
            TerminateProcess(GetCurrentProcess(), 86); // No destructor/normal drain.
#else
            std::_Exit(86);
#endif
        }
    };
    append(store, view.head, edit);
    return 2;
}

void crashMatrix()
{
    for (int stage = 0; stage < 4; ++stage)
    {
        Fixture fixture;
        RecoveryStore store(fixture.root, fixture.owner);
        append(store, {}, packet());
        juce::ChildProcess child;
        const juce::StringArray args { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--crash", fixture.root.getFullPathName(), fixture.owner.toString(), juce::String(stage) };
        require(child.start(args), "start only owned console fault child without window");
        require(child.waitForProcessToFinish(10000), "fault child terminates within bound");
        require(child.getExitCode() == 86, "fault boundary terminates process without cleanup");
        const auto recovered = store.inspect();
        good(recovered.status, "abrupt exit preserves fully verified safe head");
        require(recovered.head.sequence == (stage == 3 ? 2 : 1), "only atomically published head is recoverable");
    }
    std::cout << "Recovery abrupt-process-termination matrix passed (4 boundaries)\n";
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && juce::String(argv[1]) == "--publication-only")
    {
        audioPublicationCompatibility(); identityAndRoundtrip(); streamedAudioPublication(); budgetsAndPublishFailure();
        appendReadWorkload(); crashMatrix();
        std::cout << "Recovery publication tests passed\n";
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--writer-only")
    {
        asynchronousWriter();
        std::cout << "Recovery asynchronous writer tests passed\n";
        return 0;
    }
    if (argc == 6 && juce::String(argv[1]) == "--global-quota-child")
        return globalQuotaChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])),
            juce::File(juce::String(argv[4])), juce::String(argv[5]).getIntValue() != 0);
    if (argc == 2 && juce::String(argv[1]) == "--global-quota-only")
    {
        globalQuotaTests(); return 0;
    }
    if (argc == 5 && juce::String(argv[1]) == "--gc-crash")
        return collectionCrashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])), juce::String(argv[4]).getIntValue());
    if (argc == 2 && juce::String(argv[1]) == "--orphan-only")
    {
        orphanCollection(); collectionCrashMatrix(); return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--append-reuse-only")
    {
        appendReadWorkload();
        appendProofFaults();
        return 0;
    }
    if (argc == 5 && juce::String(argv[1]) == "--crash")
        return crashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])), juce::String(argv[4]).getIntValue());
    audioPublicationCompatibility();
    streamedAudioPublication();
    identityAndRoundtrip();
    manifestDiscovery();
    headFence();
    hostileFramesAndReferences();
    budgetsAndPublishFailure();
    globalQuotaTests();
    semanticCorruptionAndReadBudgets();
    competingWriters();
    appendReadWorkload();
    appendProofFaults();
    asynchronousWriter();
    orphanCollection();
    collectionCrashMatrix();
    directoryContainment();
    crashMatrix();
    std::cout << "RecoveryStoreTests passed\n";
    return 0;
}
