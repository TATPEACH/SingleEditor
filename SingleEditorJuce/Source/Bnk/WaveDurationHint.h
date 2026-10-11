#pragma once

#include <array>
#include <algorithm>
#include <functional>
#include "Audio/ReaderAdmission.h"
#include "Audio/SourceFileLease.h"

namespace singleeditor
{
// Optional BNK duration hint only. No decoder, sample reader, metadata body or
// input-sized allocation. This does not validate a source for actual playback.
class WaveDurationHint final
{
public:
    enum class Failure { none, unsupported, invalidData, hardLimit, cancelled, ioFailure };
    struct Result
    {
        double seconds = 0;
        Failure failure = Failure::none;
        uint32_t chunksVisited = 0, bytesRead = 0;
        explicit operator bool() const noexcept { return failure == Failure::none && seconds > 0; }
    };
    using Checkpoint = std::function<bool()>;
    static Result inspect(const juce::File& file, Checkpoint checkpoint = {})
    {
        Result result;
        const auto fail = [&](Failure failure) { result.seconds = 0; result.failure = failure; return result; };
        if (!file.hasFileExtension("wav;wave;wem")) return fail(Failure::unsupported);
        if (checkpoint && !checkpoint()) return fail(Failure::cancelled);
        std::unique_ptr<media::SourceFileLease> lease;
        if (media::SourceFileLease::open(file, lease).failed() || !lease) return fail(Failure::ioFailure);
        juce::FileInputStream input(file);
        if (!input.openedOk() || input.getTotalLength() < 12) return fail(Failure::invalidData);
        const auto fileSize = static_cast<uint64_t>(input.getTotalLength());
        std::array<uint8_t, 40> bytes {};
        const auto read = [&](int count)
        {
            if (checkpoint && !checkpoint()) { result.failure = Failure::cancelled; return false; }
            if (input.read(bytes.data(), count) != count) { result.failure = Failure::ioFailure; return false; }
            result.bytesRead += static_cast<uint32_t>(count);
            return true;
        };
        const auto u16 = [&](size_t at) { return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8); };
        const auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
        const auto u64 = [&](size_t at) { return static_cast<uint64_t>(u32(at)) | (static_cast<uint64_t>(u32(at + 4)) << 32); };
        const auto tag = [](char a, char b, char c, char d)
        { return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24); };
        if (!read(12)) return fail(result.failure);
        const auto rf64 = u32(0) == tag('R','F','6','4');
        if ((!rf64 && u32(0) != tag('R','I','F','F')) || u32(8) != tag('W','A','V','E'))
            return fail(Failure::unsupported);
        uint64_t end = static_cast<uint64_t>(u32(4)) + 8, rfData = 0, rfFrames = 0, dataSize = 0;
        if (rf64) { if (u32(4) != 0xffffffffu) return fail(Failure::invalidData); end = fileSize; }
        if (end < 12 || end > fileSize) return fail(Failure::invalidData);
        bool haveFormat = false, haveData = false, haveDs64 = false;
        uint32_t rate = 0, channels = 0, align = 0;
        while (static_cast<uint64_t>(input.getPosition()) < end)
        {
            if (++result.chunksVisited > 1024) return fail(Failure::hardLimit);
            const auto position = static_cast<uint64_t>(input.getPosition());
            if (end - position < 8) return fail(Failure::invalidData);
            if (!read(8)) return fail(result.failure);
            const auto id = u32(0);
            uint64_t length = u32(4);
            const auto start = static_cast<uint64_t>(input.getPosition());
            if (rf64 && !haveDs64 && id != tag('d','s','6','4')) return fail(Failure::invalidData);
            if (id == tag('d','a','t','a') && rf64 && length == 0xffffffffu) length = rfData;
            if (length > end - start || (length & 1) > end - start - length) return fail(Failure::invalidData);
            const auto next = start + length + (length & 1);
            if (id == tag('d','s','6','4'))
            {
                if (!rf64 || haveDs64 || length < 28) return fail(Failure::invalidData);
                if (!read(28)) return fail(result.failure);
                const auto riffSize = u64(0);
                rfData = u64(8); rfFrames = u64(16);
                const auto tableEntries = u32(24);
                if (tableEntries > 1024) return fail(Failure::hardLimit);
                if (28 + static_cast<uint64_t>(tableEntries) * 12 != length
                    || riffSize > fileSize - 8 || riffSize < 4 || next > riffSize + 8) return fail(Failure::invalidData);
                end = riffSize + 8; haveDs64 = true;
            }
            else if (id == tag('f','m','t',' '))
            {
                if (haveFormat || length < 16) return fail(Failure::invalidData);
                if (!read(static_cast<int>(juce::jmin(uint64_t{40}, length)))) return fail(result.failure);
                auto format = u16(0);
                channels = u16(2); rate = u32(4); const auto byteRate = u32(8);
                align = u16(12); const auto bits = u16(14);
                if (format == 0xfffe)
                {
                    if (length < 40 || u16(16) < 22 || static_cast<uint64_t>(u16(16)) + 18 > length
                        || u16(18) == 0 || u16(18) > bits) return fail(Failure::invalidData);
                    const auto sub = u32(24);
                    const std::array<uint8_t, 12> standard { 0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71 };
                    const std::array<uint8_t, 12> ambisonic { 0x21,7,0xd3,0x11,0x86,0x44,0xc8,0xc1,0xca,0,0,0 };
                    const auto standardGuid = std::equal(standard.begin(), standard.end(), bytes.begin() + 28);
                    const auto ambisonicGuid = std::equal(ambisonic.begin(), ambisonic.end(), bytes.begin() + 28);
                    if (!((standardGuid && (sub == 1 || sub == 3)) || (ambisonicGuid && sub == 1)))
                        return fail(Failure::unsupported);
                    format = sub;
                }
                if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32))
                    || (format == 3 && (bits == 32 || bits == 64)))) return fail(Failure::unsupported);
                if (!media::admitReader(rate, channels, 1) || align != channels * (bits / 8)
                    || static_cast<uint64_t>(byteRate) != static_cast<uint64_t>(rate) * align)
                    return fail(Failure::invalidData);
                haveFormat = true;
            }
            else if (id == tag('d','a','t','a'))
            {
                if (haveData || (rf64 && length != rfData)) return fail(Failure::invalidData);
                dataSize = length; haveData = true;
            }
            // Unknown and metadata chunks are bounds-checked and skipped, even
            // if their declared bodies are gigabytes. fmt uses at most 40 bytes.
            if (!input.setPosition(static_cast<int64_t>(next))) return fail(Failure::ioFailure);
        }
        if (!haveFormat || !haveData || align == 0 || dataSize % align != 0
            || (rf64 && rfFrames != 0 && rfFrames != dataSize / align)
            || !media::admitReader(rate, channels, static_cast<int64_t>(dataSize / align)))
            return fail(Failure::invalidData);
        if (checkpoint && !checkpoint()) return fail(Failure::cancelled);
        if (lease->recheck().failed()) return fail(Failure::ioFailure);
        result.seconds = static_cast<double>(dataSize / align) / rate;
        return result;
    }
};
} // namespace singleeditor
