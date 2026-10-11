#include "Diagnostics/DiagnosticBundle.h"
#include "Diagnostics/BoundedJson.h"
#include <iostream>
#include <functional>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

using namespace singleeditor::diagnostics;
namespace
{
void require(bool condition, const char* message)
{ if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
juce::MemoryBlock bytes(const juce::String& text) { return { text.toRawUTF8(), text.getNumBytesAsUTF8() }; }
BundleEntry provider(const juce::String& text = "{\"status\":\"available\",\"sampleRate\":48000}")
{ return { "providers/audio.json", EntryKind::provider, 1, bytes(text) }; }
BundleInput fixture()
{
    BundleInput input;
    input.timestampMs = 1800000000000LL;
    input.identity = { "0.3.0", "c6ef07b8", juce::String::repeatedString("a", 64),
                       "Release", "test", "x64", "x64", "10.0.26100", false };
    input.origin = "test-fixture";
    input.entries.push_back(provider());
    return input;
}
void zip(const juce::File& target, const juce::var& manifest, const std::vector<BundleEntry>& entries, int compression = 0)
{
    juce::ZipFile::Builder writer;
    const auto text = bytes(juce::JSON::toString(manifest, true));
    writer.addEntry(new juce::MemoryInputStream(text, true), 0, "manifest.json", {});
    for (const auto& entry : entries) writer.addEntry(new juce::MemoryInputStream(entry.bytes, true), compression, entry.id, {});
    juce::FileOutputStream stream(target);
    require(stream.openedOk() && writer.writeToStream(stream, nullptr), "construct synthetic hostile archive");
    stream.flush();
}
void patch(const juce::File& original, const juce::File& changed, const std::function<void(juce::MemoryBlock&)>& mutate)
{
    juce::MemoryBlock buffer; require(original.loadFileAsData(buffer), "read tiny fixture for mutation");
    mutate(buffer); require(changed.replaceWithData(buffer.getData(), buffer.getSize()), "write test-owned hostile fixture");
}
size_t signature(const juce::MemoryBlock& b, uint32_t value, size_t first = 0)
{
    const auto* p = static_cast<const uint8_t*>(b.getData());
    for (size_t i = first; i + 4 <= b.getSize(); ++i) if (juce::ByteOrder::littleEndianInt(p + i) == value) return i;
    require(false, "fixture ZIP signature exists"); return 0;
}
void set32(juce::MemoryBlock& b, size_t offset, uint32_t value)
{ auto* p = static_cast<uint8_t*>(b.getData()) + offset; for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i)); }
}
int main(int argc, char* argv[])
{
    if (argc == 3 && juce::String::fromUTF8(argv[1]) == "--write-fixture")
    {
        const auto path = juce::String::fromUTF8(argv[2]);
        require(juce::File::isAbsolutePath(path), "fixture destination must be absolute");
        const auto outcome = DiagnosticBundle::publish(fixture(), juce::File(path));
        require(static_cast<bool>(outcome), "publish explicit synthetic CLI fixture without overwrite");
        std::cout << "Synthetic diagnostic fixture written\n";
        return 0;
    }
    require(argc == 1, "unsupported fixture arguments");
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-DiagnosticTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create uniquely owned fixture root");
    const auto baseline = root.getChildFile("baseline.zip");
    auto input = fixture();
    auto result = DiagnosticBundle::publish(input, baseline);
    if (!result) std::cerr << "publish error=" << static_cast<int>(result.error) << '\n';
    require(static_cast<bool>(result), "publish validated same-volume bundle");
    const auto view = DiagnosticBundle::validate(baseline);
    require(view.status && view.verifiedEntries == 1 && view.verifiedBytes == input.entries.front().bytes.getSize(), "manifest and payload round trip");
    require(view.summary == DiagnosticBundle::validate(baseline).summary, "summary is a deterministic projection");
    require(DiagnosticBundle::publish(input, baseline).error == BundleError::destinationExists, "never overwrite an existing destination");
    const auto baselineHash = juce::SHA256(baseline).toHexString();
    std::atomic<bool> cancel{ true };
    const auto cancelled = root.getChildFile("cancelled.zip");
    require(DiagnosticBundle::publish(input, cancelled, &cancel).error == BundleError::cancelled && !cancelled.exists(), "pre-cancel publishes nothing");
    require(DiagnosticBundle::validate(baseline, &cancel).status.error == BundleError::cancelled, "validation honors cancellation");
    require(!DiagnosticBundle::publish(input, root.getChildFile("absent/child.zip")), "export never creates parent directories");
    DiagnosticBundle::TestHooks diskFault;
    diskFault.failOutputAfterBytes = 128;
    const auto failedWrite = root.getChildFile("failed-write.zip");
    require(DiagnosticBundle::publish(input, failedWrite, nullptr, &diskFault).error == BundleError::ioFailure && !failedWrite.exists(),
            "injected output failure publishes no success");
    DiagnosticBundle::TestHooks cancelAfterWrite;
    cancel.store(false);
    cancelAfterWrite.boundary = [&](auto boundary, const auto&) { if (boundary == DiagnosticBundle::Boundary::validated) cancel.store(true); };
    require(DiagnosticBundle::publish(input, cancelled, &cancel, &cancelAfterWrite).error == BundleError::cancelled && !cancelled.exists(),
            "cancellation after readback still removes partial and publishes nothing");
    DiagnosticBundle::TestHooks corruptPartial;
    corruptPartial.boundary = [](auto boundary, const auto& partial)
    { if (boundary == DiagnosticBundle::Boundary::partialClosed) require(partial.replaceWithText("truncated"), "inject pre-validation corruption"); };
    require(!DiagnosticBundle::publish(input, root.getChildFile("corrupt-partial.zip"), nullptr, &corruptPartial), "readback corruption cannot be published");
    DiagnosticBundle::TestHooks destinationRace;
    const auto raced = root.getChildFile("race.zip");
    destinationRace.boundary = [&](auto boundary, const auto&)
    { if (boundary == DiagnosticBundle::Boundary::beforePublish) require(raced.replaceWithText("other owner"), "inject destination ownership race"); };
    require(DiagnosticBundle::publish(input, raced, nullptr, &destinationRace).error == BundleError::destinationExists
            && raced.loadFileAsString() == "other owner", "atomic publication never replaces a competing owner's file");

    for (const auto& name : { "../audio.json", "providers/../audio.json", "providers/C:/audio.json", "providers/a\\b.json",
                              "providers/a.json/", "providers/Audio.json", "providers/..json", "providers/audio.json:stream" })
        require(!DiagnosticBundle::logicalId(name, EntryKind::provider), "reject path-like entry identifiers");
    auto reproduction = input;
    reproduction.entries.push_back({ "reproduction/sample.bin", EntryKind::reproduction, 1, bytes("<script>not executed</script>\nRIFFopaque") });
    require(!DiagnosticBundle::publish(reproduction, root.getChildFile("no-consent.zip")), "support-safe cannot carry reproduction media");
    reproduction.tier = PrivacyTier::reproductionMaterial;
    const auto attached = root.getChildFile("attached.zip");
    require(DiagnosticBundle::publish(reproduction, attached) && DiagnosticBundle::validate(attached).verifiedEntries == 2,
            "explicit reproduction tier validates attachment as opaque bytes");
    require(!root.getChildFile("reproduction").exists(), "validation never extracts attachments");

    juce::var parsed;
    require(BoundedJson::parse(bytes("{\"a\":[true,false,null,-1.2e3,\"中文\\n\"]}"), parsed), "bounded JSON accepts ordinary structured data");
    for (const auto& malformed : { "{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}", "{\"n\":1e999}",
                                   "{\"n\":01}", "{\"n\":1.}", "{\"n\":+1}", "{\"a\":1,}", "{\"s\":\"\\q\"}" })
        require(!BoundedJson::parse(bytes(malformed), parsed), "reject ambiguous or invalid JSON before DOM parsing");
    const auto deep = juce::String::repeatedString("[", 18) + "0" + juce::String::repeatedString("]", 18);
    require(!BoundedJson::parse(bytes(deep), parsed), "JSON depth budget");
    require(!BoundedJson::parse(bytes("{\"long\":\"" + juce::String::repeatedString("x", 4097) + "\"}"), parsed), "JSON string budget");
    const uint8_t invalidUtf8[] { '{', '"', 'x', '"', ':', '"', 0xff, '"', '}' };
    require(!BoundedJson::parse(juce::MemoryBlock(invalidUtf8, sizeof(invalidUtf8)), parsed), "invalid UTF-8 is not normalized into accepted evidence");

    auto wrongBody = input.entries; wrongBody[0] = provider("{\"status\":\"different-but-valid\"}");
    auto wrongManifest = view.manifest.clone();
    // Preserve length to distinguish the SHA check from the length check.
    wrongManifest["entries"].getArray()->getReference(0).getDynamicObject()->setProperty("length", static_cast<juce::int64>(wrongBody[0].bytes.getSize()));
    zip(root.getChildFile("hash.zip"), wrongManifest, wrongBody);
    require(DiagnosticBundle::validate(root.getChildFile("hash.zip")).status.error == BundleError::hashMismatch, "ZIP CRC cannot substitute for manifest SHA-256");
    for (const int major : { 0, 2, 65535 })
    {
        auto m = view.manifest.clone(); m.getDynamicObject()->setProperty("coreSchemaMajor", major);
        const auto target = root.getChildFile("schema-" + juce::String(major) + ".zip"); zip(target, m, input.entries);
        require(DiagnosticBundle::validate(target).status.error == BundleError::unsupportedSchema, "unknown core major fails closed without rewriting");
    }
    auto extended = view.manifest.clone(); extended.getDynamicObject()->setProperty("optionalFutureField", "ignored");
    extended["entries"].getArray()->getReference(0).getDynamicObject()->setProperty("schemaMajor", 55);
    zip(root.getChildFile("extension.zip"), extended, input.entries, 6);
    require(static_cast<bool>(DiagnosticBundle::validate(root.getChildFile("extension.zip")).status), "same-major fields and unknown provider schema get bounded outer validation");

    const auto rejectPatch = [&](const char* name, const std::function<void(juce::MemoryBlock&)>& mutate)
    {
        const auto target = root.getChildFile(juce::String(name) + ".zip"); patch(baseline, target, mutate);
        require(!DiagnosticBundle::validate(target).status, name);
    };
    rejectPatch("truncated", [](auto& b) { b.setSize(b.getSize() - 3); });
    rejectPatch("entry-count", [](auto& b) { auto* p = static_cast<uint8_t*>(b.getData()) + b.getSize() - 22; p[8] = p[10] = 33; });
    rejectPatch("directory-overflow", [](auto& b) { set32(b, b.getSize() - 22 + 16, 0xfffffff0u); });
    rejectPatch("entry-size", [](auto& b) { set32(b, signature(b, 0x02014b50) + 24, 0x7fffffffu); });
    rejectPatch("symlink", [](auto& b) { set32(b, signature(b, 0x02014b50) + 38, 0120777u << 16); });
    rejectPatch("encrypted", [](auto& b) { static_cast<uint8_t*>(b.getData())[signature(b, 0x02014b50) + 8] |= 1; });
    rejectPatch("local-name-mismatch", [](auto& b) { static_cast<uint8_t*>(b.getData())[30] = 'x'; });
    rejectPatch("corrupt-payload", [](auto& b) { static_cast<uint8_t*>(b.getData())[30 + 13 + 5] ^= 1; });
    rejectPatch("trailing-polyglot", [](auto& b) { const char c = 'x'; b.append(&c, 1); });
    auto duplicate = input.entries; duplicate.push_back(duplicate.front());
    zip(root.getChildFile("duplicate.zip"), view.manifest, duplicate);
    require(!DiagnosticBundle::validate(root.getChildFile("duplicate.zip")).status, "duplicate logical entry rejected");
    auto traversal = input.entries; traversal[0].id = "../escaped.json";
    zip(root.getChildFile("traversal.zip"), view.manifest, traversal);
    require(!DiagnosticBundle::validate(root.getChildFile("traversal.zip")).status && !root.getParentDirectory().getChildFile("escaped.json").exists(),
            "traversal rejected without any extraction");
    auto bomb = input.entries; bomb[0] = provider("{\"zeros\":\"" + juce::String::repeatedString("0", 200000) + "\"}");
    zip(root.getChildFile("bomb.zip"), view.manifest, bomb, 9);
    require(DiagnosticBundle::validate(root.getChildFile("bomb.zip")).status.error == BundleError::budget, "compressed expansion rejected before inflate");
    auto wrongPrivacy = DiagnosticBundle::validate(attached).manifest.clone(); wrongPrivacy.getDynamicObject()->setProperty("privacyTier", "support-safe");
    zip(root.getChildFile("tier-mismatch.zip"), wrongPrivacy, reproduction.entries);
    require(!DiagnosticBundle::validate(root.getChildFile("tier-mismatch.zip")).status, "manifest privacy and attachment category must agree");
    auto hostileJson = input; hostileJson.entries[0] = provider("{\"a\":1,\"a\":2}");
    require(!DiagnosticBundle::publish(hostileJson, root.getChildFile("invalid-json.zip")), "writer validates provider structure before creating a partial");
#if JUCE_WINDOWS
    HANDLE heldPartial = INVALID_HANDLE_VALUE;
    juce::File retained;
    DiagnosticBundle::TestHooks retainedPartial;
    cancel.store(false);
    retainedPartial.boundary = [&](auto boundary, const auto& partial)
    {
        if (boundary != DiagnosticBundle::Boundary::validated) return;
        retained = partial;
        heldPartial = CreateFileW(partial.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(heldPartial != INVALID_HANDLE_VALUE, "hold fixture partial to inject cleanup refusal"); cancel.store(true);
    };
    const auto retainedResult = DiagnosticBundle::publish(input, root.getChildFile("retained.zip"), &cancel, &retainedPartial);
    require(retainedResult.error == BundleError::cancelled && retainedResult.partialRetained && retained.existsAsFile()
            && !root.getChildFile("retained.zip").exists(), "cleanup refusal is explicit and never reported as successful publication");
    CloseHandle(heldPartial);
    require(retained.deleteFile(), "release only the synthetic partial just held by this test");
    const auto locked = CreateFileW(baseline.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "open fixture writer for competing-handle test");
    require(DiagnosticBundle::validate(baseline).status.error == BundleError::ioFailure, "validation refuses concurrent mutable file ownership");
    CloseHandle(locked);
#endif
    require(juce::SHA256(baseline).toHexString() == baselineHash, "validation and refused publication preserve immutable evidence");
    require(root.findChildFiles(juce::File::findFiles, false, "*.partial").isEmpty(), "successful and refused flows retain no partial history");
    require(root.deleteRecursively(), "remove successful test-owned synthetic fixtures");
    std::cout << "DiagnosticBundleTests passed (atomic publish, immutable validation, ZIP/JSON budgets, hashes, schema, opaque attachment, cancellation)\n";
}
