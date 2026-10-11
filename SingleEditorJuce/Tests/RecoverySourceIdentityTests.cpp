#include "Recovery/RecoverySourceIdentity.h"
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#if JUCE_WINDOWS
 #include <windows.h>
#endif
using namespace singleeditor;
using namespace singleeditor::recovery;
namespace
{
void require(bool value, const char* message)
{ if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
media::SourceIdentity stamp(const juce::File& file)
{
    media::SourceIdentity source;
    source.binding = juce::Uuid(); source.location.file = file;
    source.sizeBytes = file.getSize(); source.modifiedMilliseconds = file.getLastModificationTime().toMilliseconds();
    source.fastStampValid = true; return source;
}
SourceAudioFacts createPcmWave(const juce::File& file)
{
    constexpr int payloadBytes = 512 * 1024;
    juce::FileOutputStream output(file);
    require(output.openedOk(), "open uniquely owned synthetic PCM WAV");
    output.write("RIFF", 4); output.writeInt(36 + payloadBytes); output.write("WAVEfmt ", 8); output.writeInt(16);
    output.writeShort(1); output.writeShort(2); output.writeInt(48000); output.writeInt(192000);
    output.writeShort(4); output.writeShort(16); output.write("data", 4); output.writeInt(payloadBytes);
    for (int i = 0; i < payloadBytes / 2; ++i) output.writeShort(static_cast<short>((i * 31) % 32767));
    output.flush(); require(output.getStatus().wasOk(), "write complete PCM WAV fixture");
    return { "WAV-PCM", "stereo", 48000, 2, 16, payloadBytes / 4, 44, payloadBytes };
}
}
int main()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-RecoverySourceTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create source-identity fixture root");
    const auto wave = root.getChildFile("original.wav");
    const auto facts = createPcmWave(wave); auto source = stamp(wave);
    const auto first = RecoverySourceIdentity::capture(source, facts);
    if (!first.status) std::cerr << first.status.code << '\n';
    require(first.status && first.evidence && first.payloadBytesRead == 3 * 65536 && first.fullBytesRead == 0,
            "post-mutation sparse capture reads only three bounded payload windows");
    const auto initial = *first.evidence;
    require(initial.fullFileSha256.isEmpty() && initial.fileId.length() == 32 && initial.volumeId.length() == 16,
            "sparse evidence binds physical file/volume without whole-file hashing");
    const auto second = RecoverySourceIdentity::capture(source, facts);
    require(second.status && RecoverySourceIdentity::compare(initial, second.evidence).confidence == SourceConfidence::exact,
            "same pinned source and facts resolve Exact");
    const auto json = initial.toJson(); const auto decoded = RecoverySourceEvidence::fromJson(json);
    require(decoded && juce::JSON::toString(decoded->toJson(), true) == juce::JSON::toString(json, true), "versioned identity encoding round trips deterministically");
    auto wrongSchema = json.clone(); wrongSchema.getDynamicObject()->setProperty("schema", 2);
    require(!RecoverySourceEvidence::fromJson(wrongSchema), "future identity schema fails closed");
    auto wrongHash = json.clone(); wrongHash.getDynamicObject()->setProperty("structure_sha256", juce::String::repeatedString("0", 64));
    require(!RecoverySourceEvidence::fromJson(wrongHash), "structure digest must agree with canonical parsed facts");
    auto negative = json.clone(); negative["facts"].getDynamicObject()->setProperty("payload_bytes", -1);
    require(!RecoverySourceEvidence::fromJson(negative), "negative/overflow-prone payload spans rejected");
    auto badFacts = facts; badFacts.payloadOffset = INT64_MAX;
    require(!RecoverySourceIdentity::capture(source, badFacts).status, "untrusted payload span rejected before reads");
    std::atomic<bool> cancel{ true };
    const auto cancelled = RecoverySourceIdentity::capture(source, facts, RecoverySourceIdentity::Mode::explicitDeepVerify, &cancel);
    require(!cancelled.status && cancelled.payloadBytesRead == 0 && cancelled.fullBytesRead == 0, "pre-cancel has no source read");
    auto unknownStamp = source; unknownStamp.fastStampValid = false;
    require(!RecoverySourceIdentity::capture(unknownStamp, facts).status, "missing open-time facts never trigger a guessed reparse");

    const auto deep = RecoverySourceIdentity::capture(source, facts, RecoverySourceIdentity::Mode::explicitDeepVerify);
    require(deep.status && deep.fullBytesRead == source.sizeBytes && deep.evidence->fullFileSha256 == juce::SHA256(wave).toHexString(),
            "explicit deep verification hashes exact complete file bytes");
    require(RecoverySourceIdentity::compare(*deep.evidence, first.evidence).confidence == SourceConfidence::ambiguous,
            "a sparse recheck cannot silently downgrade an existing full-hash requirement");

    auto changed = initial; changed.sparseSha256 = juce::String::repeatedString("b", 64);
    require(RecoverySourceIdentity::compare(initial, changed).reason == SourceReason::sparseChanged, "sparse conflict is Changed");
    changed = initial; changed.modifiedTicks += 1;
    require(RecoverySourceIdentity::compare(initial, changed).reason == SourceReason::metadataChanged, "mtime conflict is not silently treated as metadata-only safe");
    changed = initial; changed.fileId = juce::String::repeatedString("c", 32);
    require(RecoverySourceIdentity::compare(initial, changed).reason == SourceReason::fileIdentityChanged, "same-name replacement conflicts with stable identity");
    auto insufficient = initial; insufficient.fileId.clear(); insufficient.volumeId.clear();
    require(RecoverySourceIdentity::compare(insufficient, insufficient).confidence == SourceConfidence::ambiguous,
            "sparse hash alone without stable identity cannot authorize replay");
    require(RecoverySourceIdentity::compare(initial, std::nullopt).confidence == SourceConfidence::missing, "confirmed absent candidate is Missing");
    const auto ambiguous = RecoverySourceIdentity::resolve(initial, { initial, initial });
    require(ambiguous.confidence == SourceConfidence::ambiguous && !ambiguous.permitsReplay(), "multiple candidates never auto-select");
    require(RecoverySourceIdentity::resolve(initial, { changed, initial }).confidence == SourceConfidence::exact,
            "one exact source can be distinguished from a nonmatching candidate without directory scanning");

#if JUCE_WINDOWS
    const auto writer = CreateFileW(wave.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "hold fixture write handle");
    const auto busy = RecoverySourceIdentity::capture(source, facts);
    require(!busy.status && busy.status.code == "source_busy" && busy.payloadBytesRead == 0, "source write ownership prevents TOCTOU-prone capture");
    CloseHandle(writer);
#endif
    const auto moved = root.getChildFile("relocated.wav");
    require(wave.moveFileTo(moved), "rename only the synthetic source on the same volume");
    auto movedStamp = source; movedStamp.location.file = moved;
    const auto relocation = RecoverySourceIdentity::capture(movedStamp, facts);
    require(static_cast<bool>(relocation.status), "capture renamed source using already parsed facts");
    const auto resolution = RecoverySourceIdentity::compare(initial, relocation.evidence);
    require(resolution.confidence == SourceConfidence::relocated && !resolution.permitsReplay() && resolution.permitsReplay(true),
            "real stable-file relocation needs explicit confirmation before replay");
    {
        juce::FileOutputStream output(moved); require(output.openedOk() && output.setPosition(44) && output.writeByte(127), "modify an actual fingerprint window in place");
        output.flush();
    }
    const auto changedProbe = RecoverySourceIdentity::capture(stamp(moved), facts);
    require(changedProbe.status && changedProbe.evidence->sparseSha256 != initial.sparseSha256
            && !RecoverySourceIdentity::compare(initial, changedProbe.evidence).permitsReplay(), "real payload mutation blocks replay");
    auto stale = stamp(moved); stale.sizeBytes += 1;
    const auto mismatch = RecoverySourceIdentity::capture(stale, facts);
    require(!mismatch.status && mismatch.status.code == "source_fast_stamp_changed" && mismatch.payloadBytesRead == 0,
            "stale source facts fail before payload fingerprinting");
    require(root.deleteRecursively(), "remove successful test-owned source fixtures");
    std::cout << "RecoverySourceIdentityTests passed (bounded payload windows, physical identity, encoding, deep verification, confidence, relocation, stale/busy fences)\n";
}
