#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace singleeditor::media
{
enum class ReaderFailure { none, sampleRate, channels, sampleLength, byteOverflow, sampleNarrowing, materialisationLimit };
struct ReaderAdmission final
{
    ReaderFailure failure = ReaderFailure::none;
    uint64_t floatBytes = 0;
    explicit operator bool() const noexcept { return failure == ReaderFailure::none; }
};

// Fixed safety ceilings; source channel support matches canonical routing.
constexpr uint64_t maximumSourceChannels = 32;
constexpr double maximumSourceRate = 768000.0;
constexpr uint64_t maximumMaterialisedBytes = 4ULL * 1024 * 1024 * 1024;

inline ReaderAdmission checkedPcmSize(uint64_t channels, int64_t frames) noexcept
{
    if (channels == 0 || channels > maximumSourceChannels) return { ReaderFailure::channels };
    if (frames <= 0) return { ReaderFailure::sampleLength };
    const auto stride = channels * sizeof(float);
    if (static_cast<uint64_t>(frames) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / stride)
        return { ReaderFailure::byteOverflow };
    return { ReaderFailure::none, static_cast<uint64_t>(frames) * stride };
}

// Constant work over already parsed reader facts. No I/O, allocation, scan,
// digest, registry, optional decoder or helper request.
inline ReaderAdmission admitReader(double rate, uint64_t channels, int64_t frames) noexcept
{
    if (!std::isfinite(rate) || rate < 1.0 || rate > maximumSourceRate) return { ReaderFailure::sampleRate };
    return checkedPcmSize(channels, frames);
}
inline ReaderAdmission admitMaterialisation(uint64_t channels, int64_t frames) noexcept
{
    const auto admitted = checkedPcmSize(channels, frames);
    if (!admitted) return admitted;
    if (frames > std::numeric_limits<int>::max()) return { ReaderFailure::sampleNarrowing };
    if (admitted.floatBytes > maximumMaterialisedBytes) return { ReaderFailure::materialisationLimit };
    return admitted;
}
inline const char* describeReaderFailure(ReaderFailure failure) noexcept
{
    switch (failure)
    {
        case ReaderFailure::none: return "";
        case ReaderFailure::sampleRate: return "Audio sample rate is invalid or exceeds the supported range.";
        case ReaderFailure::channels: return "Audio channel count is invalid or exceeds 32 supported source channels.";
        case ReaderFailure::sampleLength: return "Audio has no valid sample range.";
        case ReaderFailure::byteOverflow: return "Audio sample dimensions exceed safe arithmetic limits.";
        case ReaderFailure::sampleNarrowing: return "This edit requires more samples than a contiguous buffer supports.";
        case ReaderFailure::materialisationLimit: return "This edit exceeds the 4 GiB contiguous-audio buffer limit.";
    }
    return "Audio facts are not supported.";
}
} // namespace singleeditor::media
