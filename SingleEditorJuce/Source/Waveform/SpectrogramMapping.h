#pragma once

#include <algorithm>
#include <cmath>

namespace singleeditor
{
// Persistent spectrogram v1 uses these exact FFT/row coordinates. Share them
// with presentation so a frequency ruler cannot silently assume a log image.
struct SpectrogramMapping final
{
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int imageHeight = 256;
    static constexpr int maximumBin = fftSize / 2 - 2;
    static constexpr float curvePower = 2.6f;

    static float binAtNormalisedY(float y) noexcept
    {
        return (1.0f - std::pow(y, curvePower)) * static_cast<float>(maximumBin);
    }

    static double maximumFrequency(double sampleRate) noexcept
    {
        return (static_cast<double>(maximumBin) + 0.5) * sampleRate / fftSize;
    }

    static float normalisedYForFrequency(double frequency, double sampleRate) noexcept
    {
        if (!std::isfinite(frequency) || !std::isfinite(sampleRate) || sampleRate <= 0) return 1.0f;
        // Each pixel averages floor(bin) and floor(bin)+1. Locate its nominal
        // centre; DC is clamped to the final row, which includes bin zero.
        const auto bin = std::clamp(frequency * fftSize / sampleRate - 0.5,
                                    0.0, static_cast<double>(maximumBin));
        const auto row = std::pow(1.0 - bin / maximumBin, 1.0 / curvePower) * (imageHeight - 1);
        // drawImage stretches complete pixels, so the first/last row centres
        // are half a source pixel inside the destination bounds.
        return static_cast<float>((row + 0.5) / imageHeight);
    }
};
} // namespace singleeditor
