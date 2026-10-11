#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "Audio/AmbisonicBinauralFilterBank.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/ChannelRouter.h"

namespace singleeditor
{
class AmbisonicBinauralRenderer final
{
public:
    static std::unique_ptr<AmbisonicBinauralRenderer> create(
        std::shared_ptr<const AmbisonicBinauralFilterBank> filterBank,
        int ambisonicOrder,
        AmbisonicOrientation orientation,
        double sampleRate,
        int maximumBlockSize,
        juce::String& errorMessage);

    void process(const juce::AudioBuffer<float>& source,
                 int sourceStartSample,
                 juce::AudioBuffer<float>& destination,
                 int destinationStartSample,
                 int numSamples,
                 const ChannelPlaybackState& playbackState) noexcept;
    void setOrientation(AmbisonicOrientation orientation);
    void reset() noexcept;

    int getLatencySamples() const noexcept { return 0; }
    int getAmbisonicOrder() const noexcept { return ambisonicOrder; }
    const juce::String& getResourceIdentity() const noexcept { return resourceIdentity; }
    bool isCompatible(const std::shared_ptr<const AmbisonicBinauralFilterBank>& candidateFilterBank,
                      int candidateOrder,
                      double candidateSampleRate,
                      int candidateMaximumBlockSize) const noexcept;

private:
    using Complex = std::complex<float>;
    static constexpr int maximumAmbisonicChannels = 16;

    struct RotationState
    {
        std::array<std::array<float, maximumAmbisonicChannels>, maximumAmbisonicChannels> matrix {};
        AmbisonicOrientation orientation;
        std::uint64_t revision = 0;
    };

    AmbisonicBinauralRenderer(std::shared_ptr<const AmbisonicBinauralFilterBank> filterBank,
                              int ambisonicOrder,
                              AmbisonicOrientation orientation,
                              double sampleRate,
                              int maximumBlockSize);

    void processChunk(const juce::AudioBuffer<float>& source,
                      int sourceStartSample,
                      juce::AudioBuffer<float>& destination,
                      int destinationStartSample,
                      int numSamples,
                      const ChannelPlaybackState& playbackState) noexcept;
    std::size_t filterIndex(int ear, int sourceChannel, int bin) const noexcept;

    std::shared_ptr<const AmbisonicBinauralFilterBank> filterBank;
    int ambisonicOrder = 0;
    int sourceChannels = 0;
    int tapCount = 0;
    int historySamples = 0;
    int maximumBlockSize = 0;
    int fftSize = 0;
    double renderSampleRate = 0.0;
    juce::String resourceIdentity;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<Complex> filterSpectra;
    std::vector<Complex> inputSpectrum;
    std::vector<Complex> outputSpectrum;
    std::vector<Complex> fftInput;
    std::vector<Complex> fftOutput;
    std::vector<float> inputHistory;
    std::vector<float> chunkInput;
    std::vector<float> rotatedChunkInput;
    std::atomic<std::shared_ptr<const RotationState>> publishedRotationState;
    std::vector<std::shared_ptr<const RotationState>> retiredRotationStates;
    juce::CriticalSection rotationPublicationLock;
    std::array<std::array<float, maximumAmbisonicChannels>, maximumAmbisonicChannels> currentRotation {};
    std::uint64_t currentRotationRevision = 0;
    std::uint64_t nextRotationRevision = 1;
};
} // namespace singleeditor
