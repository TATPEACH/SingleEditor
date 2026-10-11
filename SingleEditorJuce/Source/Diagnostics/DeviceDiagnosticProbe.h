#pragma once
#include "DiagnosticSnapshot.h"
#include "Audio/BoundedDecodeProcess.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace singleeditor::diagnostics
{
struct DeviceProbeRequest final
{
    juce::String backend, outputDevice;
    int sampleRate = 0, bufferSamples = 0, outputChannels = 2;
    int pingChannel = 0; // zero = silent negotiation; otherwise one-based physical output
    juce::BigInteger physicalOutputs; // empty = first outputChannels; otherwise exact physical mask
    juce::String expectedOutputEndpoint; // private GUI binding, never emitted in diagnostic fields
    juce::BigInteger requestedOutputs() const
    {
        if (!physicalOutputs.isZero()) return physicalOutputs;
        juce::BigInteger result; result.setRange(0, outputChannels, true); return result;
    }
};
struct DeviceProbeResult final : LoadedDiagnosticProvider
{
    ProviderEvidence evidence;
    bool succeeded = false, cancelled = false, temporaryRemoved = true, workerReleased = true;
    ProviderEvidence snapshot() const override { return evidence; }
};
class DeviceDiagnosticProbe final
{
public:
    static constexpr int durationMs = 250;
    static constexpr int peakDbfs = -40;
    static constexpr double toneHz = 100.0;
    static bool valid(const DeviceProbeRequest&) noexcept;
    static media::BoundedDecodeProcess::Limits limits();
    static DeviceProbeResult run(const juce::File& workerExecutable, const DeviceProbeRequest&,
                                const juce::Uuid&, const std::atomic<bool>&,
                                media::BoundedDecodeProcess::Limits = limits());
    // The optional factory is an in-process dependency seam for actual IODevice
    // lifecycle tests. It is never selected by a product command-line option.
    using BackendFactory = std::function<std::unique_ptr<juce::AudioIODeviceType>()>;
    static std::optional<int> tryWorker(const juce::StringArray&, const BackendFactory& = {});
};
}
