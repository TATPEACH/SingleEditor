#include "RecoverySourceIdentity.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/SourceFileLease.h"
#include <juce_cryptography/juce_cryptography.h>
#include <set>
#include <array>
#include <chrono>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::recovery
{
namespace
{
juce::var object() { return juce::var(new juce::DynamicObject()); }
void put(juce::var& v, const char* key, const juce::var& value) { v.getDynamicObject()->setProperty(key, value); }
bool integer(const juce::var& v, int64_t minimum, int64_t maximum = INT64_MAX)
{ return (v.isInt() || v.isInt64()) && static_cast<int64_t>(v) >= minimum && static_cast<int64_t>(v) <= maximum; }
bool token(const juce::String& s, int count) { return s.length() == count && s.containsOnly("0123456789abcdef"); }
bool factToken(const juce::String& s) { return s.isNotEmpty() && s.length() <= 64 && s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"); }
bool validFacts(const SourceAudioFacts& facts, int64_t fileBytes)
{
    return factToken(facts.format) && factToken(facts.channelLayout) && facts.bitsPerSample >= 0 && facts.bitsPerSample <= 64
        && media::admitReader(facts.sampleRate, static_cast<uint64_t>(facts.channels), facts.frames)
        && facts.payloadOffset >= 0 && facts.payloadBytes > 0 && facts.payloadOffset <= fileBytes
        && facts.payloadBytes <= fileBytes - facts.payloadOffset;
}
juce::var factsJson(const SourceAudioFacts& facts)
{
    auto v = object(); put(v, "format", facts.format); put(v, "channel_layout", facts.channelLayout);
    put(v, "sample_rate", facts.sampleRate); put(v, "channels", facts.channels); put(v, "bits_per_sample", facts.bitsPerSample);
    put(v, "frames", static_cast<juce::int64>(facts.frames)); put(v, "payload_offset", static_cast<juce::int64>(facts.payloadOffset));
    put(v, "payload_bytes", static_cast<juce::int64>(facts.payloadBytes)); return v;
}
juce::String structureHash(const SourceAudioFacts& facts)
{
    const auto text = juce::JSON::toString(factsJson(facts), true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
bool validEvidence(const RecoverySourceEvidence& evidence)
{
    const bool coverage = evidence.algorithm == "payload-3x64k-sha256-v1"
        || (evidence.algorithm == "source-file-3x64k-sha256-v1" && evidence.facts.payloadOffset == 0 && evidence.facts.payloadBytes == evidence.sizeBytes);
    return evidence.schema == 1 && coverage
        && evidence.location.getFullPathName().isNotEmpty() && evidence.location.getFullPathName().length() <= 4096
        && evidence.sizeBytes > 0 && evidence.modifiedTicks > 0 && validFacts(evidence.facts, evidence.sizeBytes)
        && ((evidence.volumeId.isEmpty() && evidence.fileId.isEmpty()) || (token(evidence.volumeId, 16) && token(evidence.fileId, 32)))
        && token(evidence.structureSha256, 64) && evidence.structureSha256 == structureHash(evidence.facts)
        && token(evidence.sparseSha256, 64) && (evidence.fullFileSha256.isEmpty() || token(evidence.fullFileSha256, 64));
}
bool stopped(const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(std::memory_order_relaxed); }
#if JUCE_WINDOWS
struct Handle final { HANDLE h = INVALID_HANDLE_VALUE; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
bool pinParents(juce::File leaf, std::vector<std::unique_ptr<Handle>>& pins, bool* absentParent = nullptr)
{
    if (absentParent != nullptr) *absentParent = false;
    std::vector<juce::File> parents;
    for (auto f = leaf.getParentDirectory();; f = f.getParentDirectory())
    {
        parents.push_back(f); if (f == f.getParentDirectory()) break;
        if (parents.size() > 128) return false;
    }
    for (auto i = parents.rbegin(); i != parents.rend(); ++i)
    {
        auto pin = std::make_unique<Handle>();
        pin->h = CreateFileW(i->getFullPathName().toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (pin->h == INVALID_HANDLE_VALUE && absentParent != nullptr)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { *absentParent = true; return true; }
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (pin->h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(pin->h, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
        pins.push_back(std::move(pin));
    }
    return true;
}
int64_t fileTime(const FILETIME& time) { return static_cast<int64_t>((static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime); }
class DeepInput final : public juce::InputStream
{
public:
    DeepInput(HANDLE source, int64_t count, const std::atomic<bool>* c) : handle(source), bytes(count), cancel(c), start(std::chrono::steady_clock::now()) {}
    juce::int64 getTotalLength() override { return bytes; }
    juce::int64 getPosition() override { return consumed; }
    bool setPosition(juce::int64 n) override { return n == consumed; }
    bool isExhausted() override { return consumed == bytes || interrupted(); }
    bool interrupted() const { return stopped(cancel) || std::chrono::steady_clock::now() - start > std::chrono::seconds(30); }
    int read(void* data, int count) override
    {
        if (interrupted() || count <= 0) return 0;
        const auto wanted = static_cast<DWORD>(std::min<int64_t>({ count, 65536, bytes - consumed })); DWORD got = 0;
        if (!ReadFile(handle, data, wanted, &got, nullptr)) return 0;
        consumed += got; return static_cast<int>(got);
    }
    int64_t consumed = 0;
private:
    HANDLE handle;
    int64_t bytes;
    const std::atomic<bool>* cancel;
    std::chrono::steady_clock::time_point start;
};
#endif
}

juce::var RecoverySourceEvidence::toJson() const
{
    auto v = object(); put(v, "schema", schema); put(v, "algorithm", algorithm);
    put(v, "path", location.getFullPathName()); put(v, "size", static_cast<juce::int64>(sizeBytes));
    put(v, "modified_ticks", static_cast<juce::int64>(modifiedTicks)); put(v, "volume_id", volumeId); put(v, "file_id", fileId);
    put(v, "facts", factsJson(facts)); put(v, "structure_sha256", structureSha256); put(v, "sparse_sha256", sparseSha256);
    put(v, "full_file_sha256", fullFileSha256); return v;
}
std::optional<RecoverySourceEvidence> RecoverySourceEvidence::fromJson(const juce::var& v)
{
    if (!v.isObject() || v.getDynamicObject()->getProperties().size() != 11 || !integer(v["schema"], 1, 1)
        || !v["path"].isString() || v["path"].toString().length() > 4096 || !juce::File::isAbsolutePath(v["path"].toString())
        || !integer(v["size"], 1) || !integer(v["modified_ticks"], 1)) return std::nullopt;
    for (auto* key : { "algorithm", "volume_id", "file_id", "structure_sha256", "sparse_sha256", "full_file_sha256" })
        if (!v[key].isString() || v[key].toString().length() > 64) return std::nullopt;
    const auto& f = v["facts"];
    if (!f.isObject() || f.getDynamicObject()->getProperties().size() != 8 || !f["format"].isString() || !f["channel_layout"].isString()
        || !(f["sample_rate"].isDouble() || f["sample_rate"].isInt() || f["sample_rate"].isInt64())
        || !integer(f["channels"], 1, 32) || !integer(f["bits_per_sample"], 0, 64) || !integer(f["frames"], 1)
        || !integer(f["payload_offset"], 0) || !integer(f["payload_bytes"], 1)) return std::nullopt;
    RecoverySourceEvidence evidence;
    evidence.algorithm = v["algorithm"].toString(); evidence.location = juce::File(v["path"].toString());
    evidence.sizeBytes = static_cast<int64_t>(v["size"]); evidence.modifiedTicks = static_cast<int64_t>(v["modified_ticks"]);
    evidence.volumeId = v["volume_id"].toString(); evidence.fileId = v["file_id"].toString();
    evidence.facts = { f["format"].toString(), f["channel_layout"].toString(), static_cast<double>(f["sample_rate"]),
                       static_cast<int>(f["channels"]), static_cast<int>(f["bits_per_sample"]), static_cast<int64_t>(f["frames"]),
                       static_cast<int64_t>(f["payload_offset"]), static_cast<int64_t>(f["payload_bytes"]) };
    evidence.structureSha256 = v["structure_sha256"].toString(); evidence.sparseSha256 = v["sparse_sha256"].toString();
    evidence.fullFileSha256 = v["full_file_sha256"].toString();
    return validEvidence(evidence) ? std::optional<RecoverySourceEvidence>(std::move(evidence)) : std::nullopt;
}

bool RecoverySourceEvidence::bindsTo(const media::SourceIdentity& source) const
{
    return validEvidence(*this) && !source.binding.isNull() && source.fastStampValid
        && location == source.location.file && sizeBytes == source.sizeBytes
        && modifiedTicks / 10000 - 11644473600000LL == source.modifiedMilliseconds;
}

SourceObservationResult RecoverySourceIdentity::observe(const juce::File& file, const std::atomic<bool>* cancel)
{
    SourceObservationResult result;
    result.observation.location = file;
    const auto fail = [&] (Error error, const char* code)
    {
        result.status = { error, code }; result.observation = {}; return result;
    };
    try
    {
        if (stopped(cancel)) return fail(Error::cancelled, "source_observation_cancelled");
        if (file == juce::File() || file.getFullPathName().length() > 4096)
            return fail(Error::invalidPacket, "source_observation_path");
#if ! JUCE_WINDOWS
        return fail(Error::resourceOrEnvironmentFault, "source_probe_platform_unsupported");
#else
        std::vector<std::unique_ptr<Handle>> pins;
        bool absentParent = false;
        if (!pinParents(file, pins, &absentParent)) return fail(Error::resourceOrEnvironmentFault, "source_parent_unavailable_or_reparse");
        if (GetFileAttributesW(file.getFullPathName().toWideCharPointer()) == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return result;
            return fail(Error::resourceOrEnvironmentFault, "source_observation_access");
        }
        // If a missing branch appeared between observations, pin and validate
        // it before opening the now-present leaf. Never follow a fresh reparse.
        if (absentParent && !pinParents(file, pins)) return fail(Error::sourceConflict, "source_parent_appeared_during_observation");
        Handle source;
        source.h = CreateFileW(file.getFullPathName().toWideCharPointer(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (source.h == INVALID_HANDLE_VALUE) return fail(Error::resourceOrEnvironmentFault, "source_observation_open");
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandle(source.h, &info) || !GetFileInformationByHandleEx(source.h, FileBasicInfo, &basic, sizeof(basic))
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return fail(Error::resourceOrEnvironmentFault, "source_observation_not_regular");
        auto& value = result.observation;
        value.exists = true;
        value.sizeBytes = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
        value.modifiedTicks = fileTime(info.ftLastWriteTime); value.changedTicks = basic.ChangeTime.QuadPart;
        FILE_ID_INFO stable{};
        if (GetFileInformationByHandleEx(source.h, FileIdInfo, &stable, sizeof(stable)))
        {
            value.volumeId = juce::String::toHexString(stable.VolumeSerialNumber).paddedLeft('0', 16);
            value.fileId = juce::String::toHexString(stable.FileId.Identifier, sizeof(stable.FileId.Identifier), 0);
        }
        if (stopped(cancel)) return fail(Error::cancelled, "source_observation_cancelled");
        return result;
#endif
    }
    catch (...) { return fail(Error::resourceOrEnvironmentFault, "source_observation_failed"); }
}

SourceProbe RecoverySourceIdentity::capture(const media::SourceIdentity& expected, const SourceAudioFacts& facts,
                                            Mode mode, const std::atomic<bool>* cancel, Coverage coverage)
{
    SourceProbe probe;
    const auto fail = [&](Error error, const char* reason) { probe.status = { error, reason }; probe.evidence.reset(); return probe; };
    try
    {
        if (stopped(cancel)) return fail(Error::resourceOrEnvironmentFault, "source_probe_cancelled");
        if (expected.binding.isNull() || !expected.fastStampValid || expected.sizeBytes <= 0 || !validFacts(facts, expected.sizeBytes)
            || (mode != Mode::sparseAfterMutation && mode != Mode::explicitDeepVerify)
            || (coverage != Coverage::audioPayload && coverage != Coverage::encodedFile)
            || (coverage == Coverage::encodedFile && (facts.payloadOffset != 0 || facts.payloadBytes != expected.sizeBytes)))
            return fail(Error::invalidPacket, "source_facts_unavailable");
#if ! JUCE_WINDOWS
        return fail(Error::resourceOrEnvironmentFault, "source_probe_platform_unsupported");
#else
        const auto path = expected.location.file;
        if (path.getFullPathName().length() > 4096) return fail(Error::budgetExceeded, "source_path_budget");
        std::vector<std::unique_ptr<Handle>> pins;
        if (!pinParents(path, pins)) return fail(Error::resourceOrEnvironmentFault, "source_parent_unavailable_or_reparse");
        Handle source;
        source.h = CreateFileW(path.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (source.h == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            return fail(Error::resourceOrEnvironmentFault,
                        error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "source_missing"
                        : error == ERROR_SHARING_VIOLATION ? "source_busy"
                        : error == ERROR_ACCESS_DENIED ? "source_access_denied" : "source_unavailable");
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(source.h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return fail(Error::resourceOrEnvironmentFault, "source_not_regular");
        RecoverySourceEvidence evidence; evidence.location = path; evidence.facts = facts;
        if (coverage == Coverage::encodedFile) evidence.algorithm = "source-file-3x64k-sha256-v1";
        evidence.sizeBytes = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
        evidence.modifiedTicks = fileTime(info.ftLastWriteTime);
        const auto milliseconds = evidence.modifiedTicks / 10000 - 11644473600000LL;
        if (evidence.sizeBytes != expected.sizeBytes || milliseconds != expected.modifiedMilliseconds)
            return fail(Error::staleHead, "source_fast_stamp_changed");
        FILE_ID_INFO stable{};
        if (GetFileInformationByHandleEx(source.h, FileIdInfo, &stable, sizeof(stable)))
        {
            evidence.volumeId = juce::String::toHexString(stable.VolumeSerialNumber).paddedLeft('0', 16);
            evidence.fileId = juce::String::toHexString(stable.FileId.Identifier, sizeof(stable.FileId.Identifier), 0);
        }
        evidence.structureSha256 = structureHash(facts);
        juce::MemoryOutputStream fingerprint;
        fingerprint.writeString(evidence.algorithm); fingerprint.writeString(evidence.structureSha256);
        constexpr int64_t window = 65536;
        const auto span = std::min(window, facts.payloadBytes);
        std::set<int64_t> offsets { 0, (facts.payloadBytes - span) / 2, facts.payloadBytes - span };
        std::array<uint8_t, window> buffer{};
        for (auto offset : offsets)
        {
            if (stopped(cancel)) return fail(Error::resourceOrEnvironmentFault, "source_probe_cancelled");
            LARGE_INTEGER pos{}; pos.QuadPart = facts.payloadOffset + offset; DWORD got = 0;
            if (!SetFilePointerEx(source.h, pos, nullptr, FILE_BEGIN) || !ReadFile(source.h, buffer.data(), static_cast<DWORD>(span), &got, nullptr)
                || got != span) return fail(Error::resourceOrEnvironmentFault, "source_window_read_failed");
            probe.payloadBytesRead += got; fingerprint.writeInt64(offset); fingerprint.writeInt64(span); fingerprint.write(buffer.data(), got);
        }
        evidence.sparseSha256 = juce::SHA256(fingerprint.getMemoryBlock()).toHexString();
        if (mode == Mode::explicitDeepVerify)
        {
            if (evidence.sizeBytes > 8LL * 1024 * 1024 * 1024) return fail(Error::budgetExceeded, "source_deep_verify_budget");
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(source.h, start, nullptr, FILE_BEGIN)) return fail(Error::resourceOrEnvironmentFault, "source_deep_seek_failed");
            DeepInput stream(source.h, evidence.sizeBytes, cancel);
            evidence.fullFileSha256 = juce::SHA256(stream, evidence.sizeBytes).toHexString(); probe.fullBytesRead = stream.consumed;
            if (stream.interrupted()) return fail(Error::resourceOrEnvironmentFault, "source_deep_verify_cancelled_or_timeout");
            if (stream.consumed != evidence.sizeBytes) return fail(Error::resourceOrEnvironmentFault, "source_deep_verify_incomplete");
        }
        BY_HANDLE_FILE_INFORMATION after{};
        if (!GetFileInformationByHandle(source.h, &after) || fileTime(after.ftLastWriteTime) != evidence.modifiedTicks
            || after.nFileSizeHigh != info.nFileSizeHigh || after.nFileSizeLow != info.nFileSizeLow)
            return fail(Error::staleHead, "source_changed_during_probe");
        if (!validEvidence(evidence)) return fail(Error::invalidPacket, "source_evidence_invalid");
        probe.evidence = std::move(evidence); return probe;
#endif
    }
    catch (const std::bad_alloc&) { return fail(Error::budgetExceeded, "source_probe_memory_budget"); }
    catch (...) { return fail(Error::resourceOrEnvironmentFault, "source_probe_failed"); }
}

SourceProbe RecoverySourceIdentity::captureAdmittedSource(const media::SourceIdentity& expected,
    const media::SourceFileWitness& witness, const std::atomic<bool>* cancel)
{
    const auto fail = [](Status status) { SourceProbe result; result.status = std::move(status); return result; };
    if (expected.location.file != witness.file || expected.sizeBytes != witness.sizeBytes
        || expected.modifiedMilliseconds != witness.modifiedTicks / 10000 - 11644473600000LL
        || witness.readerFormat.isEmpty()) return fail({ Error::sourceConflict, "source_admission_facts_unavailable" });
    const auto before = observe(witness.file, cancel);
    if (!before.status) return fail(before.status);
    const auto matches = [&](const SourceObservation& observed)
    {
        return observed.exists && observed.location == witness.file && observed.sizeBytes == witness.sizeBytes
            && observed.modifiedTicks == witness.modifiedTicks && observed.changedTicks == witness.changedTicks
            && observed.volumeId == witness.volumeId && observed.fileId == witness.fileId;
    };
    if (!matches(before.observation)) return fail({ Error::sourceConflict, "source_admission_changed" });
    // The span is the encoded file, not a guessed codec payload. Preserve an
    // unambiguous token even for decoder names containing spaces or punctuation.
    const auto format = factToken(witness.readerFormat) ? witness.readerFormat
        : "decoder-" + juce::SHA256(witness.readerFormat.toRawUTF8(), witness.readerFormat.getNumBytesAsUTF8()).toHexString().substring(0, 32);
    SourceAudioFacts facts { format, "reader-channels-" + juce::String(witness.channels), witness.sampleRate,
        witness.channels, witness.bitsPerSample, witness.frames, 0, witness.sizeBytes };
    auto result = capture(expected, facts, Mode::sparseAfterMutation, cancel, Coverage::encodedFile);
    if (!result.status) return result;
    const auto after = observe(witness.file, cancel);
    if (!after.status) return fail(after.status);
    if (after.observation != before.observation || !matches(after.observation))
        return fail({ Error::sourceConflict, "source_changed_during_admission_probe" });
    return result;
}

SourceResolution RecoverySourceIdentity::compare(const RecoverySourceEvidence& original, const std::optional<RecoverySourceEvidence>& candidate)
{
    if (!validEvidence(original)) return { SourceConfidence::ambiguous, SourceReason::unsupportedIdentity, {} };
    if (!candidate) return { SourceConfidence::missing, SourceReason::absent, {} };
    const auto& current = *candidate;
    if (!validEvidence(current)) return { SourceConfidence::ambiguous, SourceReason::unsupportedIdentity, {} };
    if (original.algorithm != current.algorithm) return { SourceConfidence::ambiguous, SourceReason::unsupportedIdentity, {} };
    const auto changed = [](SourceReason why) { return SourceResolution { SourceConfidence::changed, why, {} }; };
    if (original.facts != current.facts || original.structureSha256 != current.structureSha256) return changed(SourceReason::factsChanged);
    if (original.sizeBytes != current.sizeBytes || original.modifiedTicks != current.modifiedTicks) return changed(SourceReason::metadataChanged);
    if (original.sparseSha256 != current.sparseSha256) return changed(SourceReason::sparseChanged);
    if (original.fullFileSha256.isNotEmpty() && current.fullFileSha256.isNotEmpty() && original.fullFileSha256 != current.fullFileSha256)
        return changed(SourceReason::fullHashChanged);
    const bool stable = original.fileId.isNotEmpty() && current.fileId.isNotEmpty();
    if (stable && (original.volumeId != current.volumeId || original.fileId != current.fileId)) return changed(SourceReason::fileIdentityChanged);
    if ((!original.fullFileSha256.isEmpty() && current.fullFileSha256.isEmpty())
        || (!stable && (original.fullFileSha256.isEmpty() || current.fullFileSha256.isEmpty())))
        return { SourceConfidence::ambiguous, SourceReason::insufficientEvidence, {} };
    const auto samePath = original.location == current.location;
    return { samePath ? SourceConfidence::exact : SourceConfidence::relocated, samePath ? SourceReason::matched : SourceReason::pathMoved, 0 };
}
SourceResolution RecoverySourceIdentity::resolve(const RecoverySourceEvidence& original, const std::vector<RecoverySourceEvidence>& candidates)
{
    if (candidates.size() > 32) return { SourceConfidence::ambiguous, SourceReason::multipleCandidates, {} };
    if (!validEvidence(original)) return { SourceConfidence::ambiguous, SourceReason::unsupportedIdentity, {} };
    if (candidates.empty()) return { SourceConfidence::missing, SourceReason::absent, {} };
    std::optional<SourceResolution> match; bool anyChanged = false, anyAmbiguous = false;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        auto result = compare(original, candidates[i]);
        if (result.confidence == SourceConfidence::exact || result.confidence == SourceConfidence::relocated)
        {
            if (match) return { SourceConfidence::ambiguous, SourceReason::multipleCandidates, {} };
            result.candidate = i; match = result;
        }
        anyChanged |= result.confidence == SourceConfidence::changed;
        anyAmbiguous |= result.confidence == SourceConfidence::ambiguous;
    }
    if (match && !anyAmbiguous) return *match;
    if (anyAmbiguous) return { SourceConfidence::ambiguous, SourceReason::insufficientEvidence, {} };
    if (anyChanged && candidates.size() == 1) return compare(original, candidates.front());
    return { SourceConfidence::changed, SourceReason::fileIdentityChanged, {} };
}
}
