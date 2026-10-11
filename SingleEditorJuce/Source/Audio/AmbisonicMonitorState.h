#pragma once

namespace singleeditor
{
// Passive monitor state carried by document snapshots. Keeping these values
// separate lets ordinary waveform/save clients read state without importing
// decoder, speaker-matrix or HRTF resource implementations.
struct AmbisonicOrientation
{
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float rollDegrees = 0.0f;

    bool isIdentity() const noexcept;
};

enum class AmbisonicMonitorDecodeStatus
{
    notRequired = 0,
    ready,
    unsupportedOrder,
    unsupportedNormalization,
    unsupportedChannelOrdering,
    interpretationRequired
};

enum class AmbisonicMonitorMode
{
    speakerProjection = 0,
    binauralHrtf
};
} // namespace singleeditor
