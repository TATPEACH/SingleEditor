#include "DiagnosticBundle.h"
#include "DiagnosticTestActivity.h"
#include "BoundedJson.h"
#include <array>
#include <map>
#include <set>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace singleeditor::diagnostics
{
namespace
{
using Error = BundleError;
bool stopped(const std::atomic<bool>* c) { return c != nullptr && c->load(std::memory_order_relaxed); }
juce::var object() { return juce::var(new juce::DynamicObject()); }
void put(juce::var& v, const char* key, const juce::var& value) { v.getDynamicObject()->setProperty(key, value); }
bool integer(const juce::var& v, int64_t lo, int64_t hi)
{ return (v.isInt() || v.isInt64()) && static_cast<int64_t>(v) >= lo && static_cast<int64_t>(v) <= hi; }
bool hex(const juce::String& s, int length)
{ return s.length() == length && s.containsOnly("0123456789abcdef"); }
bool safeToken(const juce::String& s, int limit = 128)
{ return s.isNotEmpty() && s.length() <= limit && s.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"); }
const char* tierName(PrivacyTier tier)
{
    switch (tier) { case PrivacyTier::supportSafe: return "support-safe"; case PrivacyTier::localIdentifiers: return "local-identifiers";
        case PrivacyTier::reproductionMaterial: return "reproduction-material"; }
    return "";
}
bool safePath(const juce::File& path)
{
    std::vector<juce::File> chain;
    for (auto f = path;; f = f.getParentDirectory())
    {
        chain.push_back(f);
        if (f == f.getParentDirectory()) break;
        if (chain.size() > 128) return false;
    }
    for (auto i = chain.rbegin(); i != chain.rend(); ++i)
    {
#if JUCE_WINDOWS
        const auto attributes = GetFileAttributesW(i->getFullPathName().toWideCharPointer());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
#else
        if (i->isSymbolicLink()) return false;
#endif
    }
    return true;
}

#if JUCE_WINDOWS
// A single read handle denies concurrent write/delete, so preflight and payload
// validation see the same bytes. JUCE FileInputStream permits both by default.
class PinnedInput final : public juce::InputStream
{
public:
    explicit PinnedInput(const juce::File& file)
    {
        handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        good = handle != INVALID_HANDLE_VALUE && GetFileSizeEx(handle, &size);
        length = good ? size.QuadPart : 0;
    }
    ~PinnedInput() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    juce::int64 getTotalLength() override { return length; }
    juce::int64 getPosition() override { return position; }
    bool isExhausted() override { return position >= length || !good; }
    bool setPosition(juce::int64 next) override
    {
        if (!good || next < 0 || next > length) return false;
        LARGE_INTEGER value{}; value.QuadPart = next;
        if (!SetFilePointerEx(handle, value, nullptr, FILE_BEGIN)) { good = false; return false; }
        position = next; return true;
    }
    int read(void* dest, int requested) override
    {
        if (!good || requested <= 0) return 0;
        DWORD count = 0;
        if (!ReadFile(handle, dest, static_cast<DWORD>(requested), &count, nullptr)) { good = false; return 0; }
        position += count; return static_cast<int>(count);
    }
    bool good = false;
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
    int64_t length = 0, position = 0;
};
class PartialOutput final : public juce::OutputStream
{
public:
    PartialOutput(const juce::File& file, const std::atomic<bool>* cancel, int64_t failAt) : cancelled(cancel), failAfter(failAt)
    {
        handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0,
                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        created = good = handle != INVALID_HANDLE_VALUE;
    }
    ~PartialOutput() override { close(); }
    void close() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
    void flush() override { if (good && !FlushFileBuffers(handle)) good = false; }
    juce::int64 getPosition() override { return position; }
    bool setPosition(juce::int64 next) override { return next == position; } // append-only ZIP writer
    bool write(const void* data, size_t bytes) override
    {
        if (!good || bytes > static_cast<size_t>(DiagnosticBundle::maximumArchiveBytes - position)
            || (failAfter >= 0 && bytes > static_cast<size_t>(std::max<int64_t>(0, failAfter - position)))) { good = false; return false; }
        auto* p = static_cast<const uint8_t*>(data);
        while (bytes > 0)
        {
            if (stopped(cancelled)) { good = false; return false; }
            const auto step = static_cast<DWORD>(std::min<size_t>(bytes, 65536)); DWORD done = 0;
            if (!WriteFile(handle, p, step, &done, nullptr) || done != step) { good = false; return false; }
            p += done; bytes -= done; position += done;
        }
        return true;
    }
    bool good = false, created = false;
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
    const std::atomic<bool>* cancelled;
    const int64_t failAfter;
    int64_t position = 0;
};
#endif

uint16_t u16(const uint8_t* p) { return juce::ByteOrder::littleEndianShort(p); }
uint32_t u32(const uint8_t* p) { return juce::ByteOrder::littleEndianInt(p); }
struct ZipItem final
{
    juce::String id;
    uint32_t plain = 0, packed = 0, crc = 0, offset = 0;
    uint64_t end = 0;
};
bool readAt(juce::InputStream& in, uint64_t pos, void* bytes, int count)
{ return pos <= static_cast<uint64_t>(in.getTotalLength()) && in.setPosition(static_cast<int64_t>(pos)) && in.read(bytes, count) == count; }

// A deliberately narrow ZIP profile: one disk, stored/deflate, no extras,
// descriptors, comments, encryption, links, ZIP64, executable prefix or gaps.
// Preflight all allocations/offsets before JUCE constructs its entry holders.
BundleStatus preflight(juce::InputStream& in, std::vector<ZipItem>& items)
{
    const auto length = in.getTotalLength();
    if (length < 22) return { Error::malformedArchive };
    if (length > DiagnosticBundle::maximumArchiveBytes) return { Error::budget };
    std::array<uint8_t, 22> end{};
    if (!readAt(in, static_cast<uint64_t>(length - 22), end.data(), 22) || u32(end.data()) != 0x06054b50
        || u16(end.data() + 4) != 0 || u16(end.data() + 6) != 0 || u16(end.data() + 20) != 0
        || u16(end.data() + 8) != u16(end.data() + 10)) return { Error::malformedArchive };
    const auto count = u16(end.data() + 10);
    const uint64_t directoryBytes = u32(end.data() + 12), directoryStart = u32(end.data() + 16);
    if (count == 0 || count > DiagnosticBundle::maximumEntries || directoryBytes > 65536) return { Error::budget };
    if (directoryStart + directoryBytes != static_cast<uint64_t>(length - 22)) return { Error::malformedArchive };
    std::vector<uint8_t> directory(static_cast<size_t>(directoryBytes));
    if (!readAt(in, directoryStart, directory.data(), static_cast<int>(directoryBytes))) return { Error::malformedArchive };
    size_t pos = 0; uint64_t totalPlain = 0;
    std::set<juce::String> names;
    for (int i = 0; i < count; ++i)
    {
        if (directory.size() - pos < 46) return { Error::malformedArchive };
        const auto* p = directory.data() + pos;
        const auto flags = u16(p + 8), method = u16(p + 10), nameBytes = u16(p + 28);
        const auto attributes = u32(p + 38), mode = (attributes >> 16) & 0170000u;
        if (u32(p) != 0x02014b50 || u16(p + 6) > 20 || (flags & ~0x800u) != 0 || (method != 0 && method != 8)
            || u16(p + 30) != 0 || u16(p + 32) != 0 || u16(p + 34) != 0
            || (attributes & 0xffffu & ~0x20u) != 0 || (mode != 0 && mode != 0100000u)
            || nameBytes == 0 || nameBytes > 96 || 46u + nameBytes > directory.size() - pos) return { Error::malformedArchive };
        ZipItem item;
        item.id = juce::String::fromUTF8(reinterpret_cast<const char*>(p + 46), nameBytes);
        if (item.id.getNumBytesAsUTF8() != nameBytes || (item.id != "manifest.json"
            && !DiagnosticBundle::logicalId(item.id, EntryKind::provider) && !DiagnosticBundle::logicalId(item.id, EntryKind::reproduction))
            || !names.insert(item.id).second) return { Error::malformedArchive };
        item.crc = u32(p + 16); item.packed = u32(p + 20); item.plain = u32(p + 24); item.offset = u32(p + 42);
        totalPlain += item.plain;
        if (item.plain > DiagnosticBundle::maximumEntryBytes || totalPlain > DiagnosticBundle::maximumArchiveBytes
            || ((item.id == "manifest.json" || item.id.startsWith("providers/")) && item.plain > BoundedJson::maximumBytes)
            || item.plain > std::max<uint64_t>(1, item.packed) * 256) return { Error::budget };
        if (method == 0 && item.plain != item.packed) return { Error::malformedArchive };
        std::array<uint8_t, 30> local{};
        if (static_cast<uint64_t>(item.offset) + 30u + nameBytes + item.packed > directoryStart
            || !readAt(in, item.offset, local.data(), 30) || u32(local.data()) != 0x04034b50
            || u16(local.data() + 4) != u16(p + 6) || u16(local.data() + 6) != flags || u16(local.data() + 8) != method
            || u32(local.data() + 14) != item.crc || u32(local.data() + 18) != item.packed || u32(local.data() + 22) != item.plain
            || u16(local.data() + 26) != nameBytes || u16(local.data() + 28) != 0) return { Error::malformedArchive };
        std::array<uint8_t, 96> localName{};
        if (in.read(localName.data(), nameBytes) != nameBytes || std::memcmp(localName.data(), p + 46, nameBytes) != 0)
            return { Error::malformedArchive };
        item.end = static_cast<uint64_t>(item.offset) + 30u + nameBytes + item.packed;
        items.push_back(item); pos += 46u + nameBytes;
    }
    if (pos != directory.size() || names.count("manifest.json") != 1) return { Error::malformedArchive };
    auto spans = items;
    std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    uint64_t next = 0;
    for (const auto& span : spans) { if (span.offset != next) return { Error::malformedArchive }; next = span.end; }
    return { next == directoryStart ? Error::none : Error::malformedArchive };
}
constexpr auto crcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1u) != 0 ? 0xedb88320u : 0u);
        table[i] = c;
    }
    return table;
}
constexpr auto checksums = crcTable();
class PayloadInput final : public juce::InputStream
{
public:
    PayloadInput(juce::InputStream& s, uint32_t limit, bool keep, const std::atomic<bool>* c)
        : source(s), size(limit), retain(keep), cancel(c) {}
    juce::int64 getTotalLength() override { return size; }
    juce::int64 getPosition() override { return consumed; }
    bool setPosition(juce::int64 n) override { return n == consumed; }
    bool isExhausted() override { return consumed >= size || stopped(cancel); }
    int read(void* data, int maximum) override
    {
        if (maximum <= 0 || stopped(cancel)) return 0;
        const auto count = source.read(data, std::min<int>(maximum, static_cast<int>(size - consumed)));
        if (count <= 0) return 0;
        const auto* p = static_cast<const uint8_t*>(data);
        for (int i = 0; i < count; ++i) crc = checksums[(crc ^ p[i]) & 255u] ^ (crc >> 8);
        consumed += static_cast<uint32_t>(count);
        if (retain) bytes.append(data, static_cast<size_t>(count));
        return count;
    }
    juce::MemoryBlock bytes;
    uint32_t consumed = 0, crc = 0xffffffffu;
private:
    juce::InputStream& source;
    const uint32_t size;
    const bool retain;
    const std::atomic<bool>* cancel;
};
BundleStatus readPayload(juce::ZipFile& zip, const ZipItem& item, juce::String& hash,
                         juce::var* json, const std::atomic<bool>* cancel)
{
    std::unique_ptr<juce::InputStream> input(zip.createStreamForEntry(zip.getIndexOfFileName(item.id)));
    if (!input) return { Error::malformedArchive };
    PayloadInput measured(*input, item.plain, json != nullptr, cancel);
    hash = juce::SHA256(measured, item.plain).toHexString();
    if (stopped(cancel)) return { Error::cancelled };
    char extra{};
    if (measured.consumed != item.plain || input->read(&extra, 1) != 0 || (measured.crc ^ 0xffffffffu) != item.crc)
        return { Error::malformedArchive };
    if (json != nullptr && !BoundedJson::parse(measured.bytes, *json)) return { Error::malformedJson };
    return {};
}
bool validIdentity(const juce::var& identity)
{
    if (!identity.isObject()) return false;
    for (const auto* field : { "version", "commit", "configuration", "distribution", "processArchitecture", "osArchitecture", "osBuild" })
        if (!identity[field].isString() || !safeToken(identity[field].toString())) return false;
    return identity["binarySha256"].isString() && hex(identity["binarySha256"].toString(), 64) && identity["timingInstrumentation"].isBool();
}
juce::var makeIdentity(const BundleIdentity& b)
{
    auto v = object();
    put(v, "version", b.version); put(v, "commit", b.commit); put(v, "binarySha256", b.binarySha256);
    put(v, "configuration", b.configuration); put(v, "distribution", b.distribution);
    put(v, "processArchitecture", b.processArchitecture); put(v, "osArchitecture", b.osArchitecture);
    put(v, "osBuild", b.osBuild); put(v, "timingInstrumentation", b.timingInstrumentation);
    return v;
}
}

bool DiagnosticBundle::logicalId(const juce::String& id, EntryKind kind)
{
    const juce::String prefix = kind == EntryKind::provider ? "providers/" : "reproduction/";
    const juce::String suffix = kind == EntryKind::provider ? ".json" : ".bin";
    if ((kind != EntryKind::provider && kind != EntryKind::reproduction) || !id.startsWith(prefix) || !id.endsWith(suffix)) return false;
    const auto slug = id.substring(prefix.length(), id.length() - suffix.length());
    return slug.length() >= 1 && slug.length() <= 48 && slug.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789-")
        && slug[0] != '-' && !slug.endsWithChar('-');
}

bool DiagnosticBundle::identityValid(const BundleIdentity& identity) { return validIdentity(makeIdentity(identity)); }

BundleView DiagnosticBundle::validate(const juce::File& file, const std::atomic<bool>* cancel)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(validation);
    BundleView view;
    const auto fail = [&](Error error) { view.status.error = error; view.manifest = juce::var(); view.summary.clear(); return view; };
    try
    {
        if (stopped(cancel)) return fail(Error::cancelled);
        if (!safePath(file)) return fail(Error::invalidInput);
#if JUCE_WINDOWS
        PinnedInput input(file);
        if (!input.good) return fail(Error::ioFailure);
#else
        juce::FileInputStream input(file);
        if (input.failedToOpen()) return fail(Error::ioFailure);
#endif
        std::vector<ZipItem> items;
        auto status = preflight(input, items);
        if (!status) return fail(status.error);
        juce::ZipFile zip(input);
        if (zip.getNumEntries() != static_cast<int>(items.size())) return fail(Error::malformedArchive);
        const auto manifest = std::find_if(items.begin(), items.end(), [](const auto& e) { return e.id == "manifest.json"; });
        juce::String unused;
        status = readPayload(zip, *manifest, unused, &view.manifest, cancel);
        if (!status) return fail(status.error);
        const auto& m = view.manifest;
        if (!m.isObject() || m["format"].toString() != "single-editor-diagnostics"
            || !integer(m["coreSchemaMajor"], 0, 65535)) return fail(Error::inconsistentManifest);
        if (static_cast<int>(m["coreSchemaMajor"]) != currentCoreMajor) return fail(Error::unsupportedSchema);
        const auto tier = m["privacyTier"].toString(), origin = m["origin"].toString();
        if (!hex(m["snapshotId"].toString(), 32) || juce::Uuid(m["snapshotId"].toString()).isNull()
            || !integer(m["timestampMs"], 1, INT64_MAX) || !validIdentity(m["identity"])
            || (tier != "support-safe" && tier != "local-identifiers" && tier != "reproduction-material")
            || (origin != "gui" && origin != "standalone-cli" && origin != "test-fixture")) return fail(Error::inconsistentManifest);
        const auto* list = m["entries"].getArray();
        if (list == nullptr || static_cast<size_t>(list->size()) + 1 != items.size()) return fail(Error::inconsistentManifest);
        std::set<juce::String> seen;
        for (const auto& entry : *list)
        {
            if (stopped(cancel)) return fail(Error::cancelled);
            const auto id = entry["id"].toString(), type = entry["kind"].toString();
            const auto item = std::find_if(items.begin(), items.end(), [&](const auto& e) { return e.id == id; });
            if (!entry.isObject() || !seen.insert(id).second || item == items.end() || id == "manifest.json"
                || !integer(entry["schemaMajor"], 1, 65535) || !integer(entry["length"], 0, maximumEntryBytes)
                || static_cast<int64_t>(entry["length"]) != item->plain || !hex(entry["sha256"].toString(), 64)
                || (type != "provider" && type != "reproduction")
                || !logicalId(id, type == "provider" ? EntryKind::provider : EntryKind::reproduction)
                || (type == "reproduction" && tier != "reproduction-material")) return fail(Error::inconsistentManifest);
            juce::String hash; juce::var json;
            // Provider JSON receives only structural bounds here. No concrete
            // provider parser/factory is instantiated, including unknown schemas.
            status = readPayload(zip, *item, hash, type == "provider" ? &json : nullptr, cancel);
            if (!status) return fail(status.error);
            if (hash != entry["sha256"].toString()) return fail(Error::hashMismatch);
            if (type == "provider" && !json.isObject()) return fail(Error::malformedJson);
            ++view.verifiedEntries; view.verifiedBytes += item->plain;
        }
        view.summary = "Single Editor diagnostics v" + juce::String(currentCoreMajor)
            + "\nSnapshot: " + m["snapshotId"].toString() + "\nOrigin: " + origin + "\nPrivacy: " + tier
            + "\nBinary SHA-256: " + m["identity"]["binarySha256"].toString()
            + "\nVerified payloads: " + juce::String(static_cast<int>(view.verifiedEntries))
            + "\nVerified bytes: " + juce::String(static_cast<juce::int64>(view.verifiedBytes)) + "\n";
        return view;
    }
    catch (const std::bad_alloc&) { return fail(Error::budget); }
    catch (...) { return fail(Error::ioFailure); }
}

BundleStatus DiagnosticBundle::publish(const BundleInput& bundle, const juce::File& destination, const std::atomic<bool>* cancel
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
                                      , const TestHooks* hooks
#endif
                                      )
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(publication);
#if ! JUCE_WINDOWS
    juce::ignoreUnused(bundle, destination, cancel);
    return { Error::ioFailure }; // Publication ownership/atomicity pilot is Windows-only.
#else
    juce::File partial;
    bool ownsPartial = false;
    const auto perform = [&]() -> BundleStatus
    {
    try
    {
        if (stopped(cancel)) return { Error::cancelled };
        const auto identity = makeIdentity(bundle.identity);
        if (bundle.snapshotId.isNull() || bundle.timestampMs <= 0 || !validIdentity(identity) || *tierName(bundle.tier) == '\0'
            || (bundle.origin != "gui" && bundle.origin != "standalone-cli" && bundle.origin != "test-fixture")
            || !safePath(destination) || !destination.getParentDirectory().isDirectory()) return { Error::invalidInput };
        if (destination.exists()) return { Error::destinationExists };
        if (bundle.entries.size() + 1 > maximumEntries) return { Error::budget };
        auto manifest = object(); auto list = juce::var(juce::Array<juce::var>());
        put(manifest, "format", "single-editor-diagnostics"); put(manifest, "coreSchemaMajor", currentCoreMajor);
        put(manifest, "snapshotId", bundle.snapshotId.toString()); put(manifest, "timestampMs", static_cast<juce::int64>(bundle.timestampMs));
        put(manifest, "identity", identity); put(manifest, "privacyTier", tierName(bundle.tier)); put(manifest, "origin", bundle.origin);
        std::set<juce::String> names; uint64_t total = 0;
        for (const auto& entry : bundle.entries)
        {
            if (stopped(cancel)) return { Error::cancelled };
            if (!logicalId(entry.id, entry.kind) || !names.insert(entry.id).second || entry.schemaMajor < 1 || entry.schemaMajor > 65535
                || (entry.kind == EntryKind::reproduction && bundle.tier != PrivacyTier::reproductionMaterial)) return { Error::invalidInput };
            total += entry.bytes.getSize();
            if (entry.bytes.getSize() > maximumEntryBytes || total > maximumArchiveBytes - 1024 * 1024) return { Error::budget };
            juce::var provider;
            if (entry.kind == EntryKind::provider && (!BoundedJson::parse(entry.bytes, provider) || !provider.isObject())) return { Error::malformedJson };
            auto item = object(); put(item, "id", entry.id); put(item, "kind", entry.kind == EntryKind::provider ? "provider" : "reproduction");
            put(item, "schemaMajor", entry.schemaMajor); put(item, "length", static_cast<juce::int64>(entry.bytes.getSize()));
            put(item, "sha256", juce::SHA256(entry.bytes).toHexString()); list.append(item);
        }
        put(manifest, "entries", list);
        const auto text = juce::JSON::toString(manifest, true);
        const juce::MemoryBlock manifestBytes(text.toRawUTF8(), text.getNumBytesAsUTF8());
        juce::var checked;
        if (!BoundedJson::parse(manifestBytes, checked)) return { Error::budget };
        partial = destination.getSiblingFile("." + destination.getFileName() + ".single-editor-diagnostics-" + juce::Uuid().toString() + ".partial");
        int64_t failAfter = -1;
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
        if (hooks != nullptr) failAfter = hooks->failOutputAfterBytes;
#endif
        PartialOutput output(partial, cancel, failAfter); ownsPartial = output.created;
        if (!output.good) return { Error::ioFailure };
        juce::ZipFile::Builder writer;
        const juce::Time time(bundle.timestampMs);
        writer.addEntry(new juce::MemoryInputStream(manifestBytes, false), 0, "manifest.json", time);
        for (const auto& entry : bundle.entries) writer.addEntry(new juce::MemoryInputStream(entry.bytes, false), 0, entry.id, time);
        const bool written = writer.writeToStream(output, nullptr);
        output.flush(); const bool good = written && output.good; output.close();
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
        if (hooks != nullptr && hooks->boundary) hooks->boundary(Boundary::partialClosed, partial);
#endif
        if (stopped(cancel)) return { Error::cancelled };
        if (!good) return { Error::ioFailure };
        const auto validated = validate(partial, cancel);
        if (!validated.status) return validated.status;
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
        if (hooks != nullptr && hooks->boundary) hooks->boundary(Boundary::validated, partial);
#endif
        if (stopped(cancel)) return { Error::cancelled };
#if SINGLE_EDITOR_DIAGNOSTICS_TESTING
        if (hooks != nullptr && hooks->boundary) hooks->boundary(Boundary::beforePublish, partial);
#endif
        // No replace/copy fallback: a destination created during export wins.
        if (!MoveFileExW(partial.getFullPathName().toWideCharPointer(), destination.getFullPathName().toWideCharPointer(), MOVEFILE_WRITE_THROUGH))
            return { destination.exists() ? Error::destinationExists : Error::ioFailure };
        ownsPartial = false;
        return {};
    }
    catch (const std::bad_alloc&) { return { Error::budget }; }
    catch (...) { return { Error::ioFailure }; }
    };
    auto status = perform(); // all file/ZIP handles released before partial cleanup
    if (ownsPartial && !partial.deleteFile())
    {
        status.partialRetained = true;
        if (status) status.error = Error::ioFailure;
    }
    return status;
#endif
}
}
