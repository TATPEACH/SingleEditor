#include "Audio/DecodedWaveCache.h"
#include "Audio/ReaderAdmission.h"
#include <array>
#include <cstring>

namespace singleeditor::media
{
namespace
{
constexpr int sealBytes = 184;
constexpr char magic[8] { 'S', 'E', 'W', 'A', 'V', 'C', '1', 0 };
juce::String digest(const juce::String& value)
{
    return juce::SHA256(value.toRawUTF8(), value.getNumBytesAsUTF8()).toHexString();
}
// This is a physical-file stamp, not a full source-content digest. The current
// user and program/cache files are trusted by the M3 threat model. A pathname
// hash alone is never accepted as source identity.
juce::String stamp(const juce::File& file)
{
    if (!file.existsAsFile()) return {};
    return digest(file.getFullPathName() + "|" + juce::String(file.getFileIdentifier()) + "|"
                  + juce::String(file.getSize()) + "|"
                  + juce::String(file.getLastModificationTime().toMilliseconds()));
}
bool current(const DecodedWaveCache::Context& context)
{
    return context.sourceStamp.isNotEmpty() && context.producerStamp.isNotEmpty()
        && stamp(context.source) == context.sourceStamp && stamp(context.producer) == context.producerStamp;
}
uint32_t le32(const unsigned char* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
juce::MemoryBlock hashBytes(const juce::String& hex)
{
    juce::MemoryBlock result;
    result.loadFromHexString(hex);
    return result;
}
std::optional<juce::MemoryBlock> bodyHash(const juce::File& file, int64_t expectedSize, const WorkCheckpoint& checkpoint)
{
    juce::FileInputStream input(file);
    if (!input.openedOk() || input.getTotalLength() != expectedSize || expectedSize < 44
        || expectedSize > DecodedWaveCache::maximumBytes) return std::nullopt;
    const auto hash = checkedStreamHash(input, expectedSize, checkpoint);
    if (!hash || input.getPosition() != expectedSize || file.getSize() != expectedSize) return std::nullopt;
    return hash->getRawData();
}
}

std::optional<DecodedWaveCache::Context> DecodedWaveCache::capture(
    const juce::File& source, const juce::File& producer, const juce::String& policy)
{
    if (policy.isEmpty() || policy.getNumBytesAsUTF8() > 256) return std::nullopt;
    Context context { source, producer, policy, stamp(source), stamp(producer) };
    if (!current(context)) return std::nullopt;
    return context;
}

std::optional<DecodedWaveCache::WaveFacts> DecodedWaveCache::inspectComplete(const juce::File& wave, const WorkCheckpoint& checkpoint)
{
    if (checkpoint && !checkpoint()) return std::nullopt;
    juce::FileInputStream input(wave);
    const auto size = input.getTotalLength();
    if (!input.openedOk() || size < 44 || size > maximumBytes) return std::nullopt;
    std::array<unsigned char, 12> header {};
    if (input.read(header.data(), 12) != 12 || std::memcmp(header.data(), "RIFF", 4) != 0
        || std::memcmp(header.data() + 8, "WAVE", 4) != 0
        || static_cast<uint64_t>(le32(header.data() + 4)) + 8 != static_cast<uint64_t>(size))
        return std::nullopt;
    WaveFacts facts;
    bool formatSeen = false, dataSeen = false;
    uint64_t inspectedHeaderBytes = 12;
    while (input.getPosition() < size)
    {
        if (checkpoint && !checkpoint()) return std::nullopt;
        if (size - input.getPosition() < 8 || inspectedHeaderBytes > 1024 * 1024) return std::nullopt;
        std::array<unsigned char, 8> chunk {};
        if (input.read(chunk.data(), 8) != 8) return std::nullopt;
        const auto bytes = static_cast<uint64_t>(le32(chunk.data() + 4));
        const auto start = static_cast<uint64_t>(input.getPosition());
        const auto end = start + bytes + (bytes & 1);
        if (end > static_cast<uint64_t>(size)) return std::nullopt;
        if (std::memcmp(chunk.data(), "fmt ", 4) == 0)
        {
            if (formatSeen || bytes < 16 || bytes > 64) return std::nullopt;
            std::array<unsigned char, 64> format {};
            if (input.read(format.data(), static_cast<int>(bytes)) != static_cast<int>(bytes)) return std::nullopt;
            auto tag = le16(format.data());
            if (tag == 0xfffe)
            {
                constexpr unsigned char guidTail[14] { 0,0, 0,0, 0x10,0, 0x80,0, 0,0xaa,0,0x38,0x9b,0x71 };
                if (bytes < 40 || le16(format.data() + 16) < 22
                    || std::memcmp(format.data() + 26, guidTail, 14) != 0) return std::nullopt;
                tag = le16(format.data() + 24);
            }
            const auto bits = le16(format.data() + 14);
            if ((tag != 1 && tag != 3) || bits == 0 || bits > 32 || bits % 8 != 0
                || (tag == 3 && bits != 32)) return std::nullopt;
            facts.channels = le16(format.data() + 2);
            facts.rate = le32(format.data() + 4);
            facts.bytesPerFrame = le16(format.data() + 12);
            if (facts.bytesPerFrame != facts.channels * (bits / 8)
                || static_cast<uint64_t>(facts.rate) * facts.bytesPerFrame != le32(format.data() + 8))
                return std::nullopt;
            formatSeen = true;
        }
        else if (std::memcmp(chunk.data(), "data", 4) == 0)
        {
            if (dataSeen || !formatSeen || facts.bytesPerFrame == 0 || bytes == 0
                || bytes % facts.bytesPerFrame != 0) return std::nullopt;
            facts.dataStart = start; facts.dataBytes = bytes; facts.frames = bytes / facts.bytesPerFrame;
            if (!admitReader(facts.rate, facts.channels, static_cast<int64_t>(facts.frames))) return std::nullopt;
            dataSeen = true;
        }
        else if (bytes > 1024 * 1024) return std::nullopt;
        inspectedHeaderBytes += 8 + (std::memcmp(chunk.data(), "data", 4) == 0 ? 0 : bytes);
        if (!input.setPosition(static_cast<int64_t>(end))) return std::nullopt;
    }
    return dataSeen ? std::optional<WaveFacts>(facts) : std::nullopt;
}

juce::File DecodedWaveCache::sealFile(const juce::File& wave)
{
    return wave.getSiblingFile(wave.getFileName() + ".seal");
}
bool DecodedWaveCache::seal(const Context& context, const juce::File& wave, const WorkCheckpoint& checkpoint)
{
    if ((checkpoint && !checkpoint()) || !current(context)) return false;
    const auto facts = inspectComplete(wave, checkpoint);
    const auto size = wave.getSize();
    if (!facts) return false;
    const auto hash = bodyHash(wave, size, checkpoint);
    if (!hash || !current(context)) return false;
    juce::MemoryOutputStream bytes;
    bytes.write(magic, 8); bytes.writeInt(1);
    bytes.write(juce::Uuid().getRawData(), 16);
    for (const auto& value : { hashBytes(context.sourceStamp), hashBytes(context.producerStamp),
                              hashBytes(digest(context.policy)), *hash })
        bytes.write(value.getData(), 32);
    bytes.writeInt64(size); bytes.writeDouble(static_cast<double>(facts->rate));
    bytes.writeInt(static_cast<int>(facts->channels)); bytes.writeInt64(static_cast<int64_t>(facts->frames));
    if (bytes.getDataSize() != sealBytes) return false;
    const auto destination = sealFile(wave);
    const auto staging = destination.getSiblingFile(destination.getFileName() + "." + juce::Uuid().toString() + ".tmp");
    const auto written = staging.replaceWithData(bytes.getData(), bytes.getDataSize());
    const auto published = written && (!checkpoint || checkpoint()) && current(context) && staging.replaceFileIn(destination);
    if (!published) staging.deleteFile();
    return published;
}
const char* DecodedWaveCache::lookupReasonName(LookupReason reason) noexcept
{
    switch (reason)
    {
        case LookupReason::notObserved: return "not-observed";
        case LookupReason::reused: return "reused";
        case LookupReason::contextUnavailable: return "context-unavailable";
        case LookupReason::contextChanged: return "context-changed";
        case LookupReason::workEnded: return "work-ended";
        case LookupReason::incompleteMarker: return "incomplete-marker";
        case LookupReason::entryUnavailable: return "entry-unavailable";
        case LookupReason::entrySizeRejected: return "entry-size-rejected";
        case LookupReason::sealUnavailable: return "seal-unavailable";
        case LookupReason::sealInvalid: return "seal-invalid";
        case LookupReason::sourceChanged: return "source-changed";
        case LookupReason::producerChanged: return "producer-changed";
        case LookupReason::policyChanged: return "policy-changed";
        case LookupReason::sizeChanged: return "size-changed";
        case LookupReason::waveIncomplete: return "wave-incomplete";
        case LookupReason::dimensionsChanged: return "dimensions-changed";
        case LookupReason::bodyReadIncomplete: return "body-read-incomplete";
        case LookupReason::bodyChanged: return "body-changed";
        case LookupReason::readerUnavailable: return "reader-unavailable";
        case LookupReason::readerRejected: return "reader-rejected";
    }
    return "not-observed";
}
bool DecodedWaveCache::reusable(const Context& context, const juce::File& wave, const WorkCheckpoint& checkpoint,
                              LookupReason* observation)
{
    bool workEnded = false;
    const WorkCheckpoint checked = checkpoint ? WorkCheckpoint([&]
    { if (!checkpoint()) { workEnded = true; return false; } return true; }) : WorkCheckpoint{};
    const auto finish = [&](LookupReason reason)
    {
        if (workEnded) reason = LookupReason::workEnded;
        if (observation) *observation = reason;
        return reason == LookupReason::reused;
    };
    if (checked && !checked()) return finish(LookupReason::workEnded);
    if (!current(context)) return finish(LookupReason::contextChanged);
    juce::FileInputStream input(sealFile(wave));
    std::array<unsigned char, sealBytes> bytes {};
    if (!input.openedOk()) return finish(LookupReason::sealUnavailable);
    if (input.getTotalLength() != sealBytes || input.read(bytes.data(), sealBytes) != sealBytes)
        return finish(LookupReason::sealInvalid);
    juce::MemoryInputStream seal(bytes.data(), bytes.size(), false);
    std::array<char, 8> readMagic {};
    seal.read(readMagic.data(), 8);
    if (std::memcmp(readMagic.data(), magic, 8) != 0 || seal.readInt() != 1) return finish(LookupReason::sealInvalid);
    std::array<unsigned char, 16> generation {};
    seal.read(generation.data(), 16);
    if (juce::Uuid(generation.data()).isNull()) return finish(LookupReason::sealInvalid);
    for (const auto& [expected, reason] : std::array<std::pair<juce::String, LookupReason>, 3> {{
             { context.sourceStamp, LookupReason::sourceChanged },
             { context.producerStamp, LookupReason::producerChanged },
             { digest(context.policy), LookupReason::policyChanged } }})
    {
        juce::MemoryBlock readHash(32);
        if (seal.read(readHash.getData(), 32) != 32) return finish(LookupReason::sealInvalid);
        if (readHash != hashBytes(expected)) return finish(reason);
    }
    juce::MemoryBlock expectedBody(32);
    seal.read(expectedBody.getData(), 32);
    const auto size = seal.readInt64(); const auto rate = seal.readDouble();
    const auto channels = seal.readInt(); const auto frames = seal.readInt64();
    if (size != wave.getSize() || size < 44 || size > maximumBytes) return finish(LookupReason::sizeChanged);
    const auto facts = inspectComplete(wave, checked);
    if (!facts) return finish(LookupReason::waveIncomplete);
    if (rate != facts->rate || channels != static_cast<int>(facts->channels)
        || frames != static_cast<int64_t>(facts->frames)) return finish(LookupReason::dimensionsChanged);
    const auto hash = bodyHash(wave, size, checked);
    if (!hash) return finish(LookupReason::bodyReadIncomplete);
    if (*hash != expectedBody) return finish(LookupReason::bodyChanged);
    if (checked && !checked()) return finish(LookupReason::workEnded);
    return finish(current(context) ? LookupReason::reused : LookupReason::contextChanged);
}
}
