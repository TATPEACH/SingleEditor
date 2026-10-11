#include "Audio/AmbisonicBinauralRenderer.h"

#include <array>
#include <cmath>
#include <cstring>

namespace singleeditor
{
namespace
{
constexpr int maximumAmbisonicChannels = 16;

struct Direction
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Direction rotateAroundX(Direction direction, double radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { direction.x,
             cosine * direction.y - sine * direction.z,
             sine * direction.y + cosine * direction.z };
}

Direction rotateAroundY(Direction direction, double radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { cosine * direction.x + sine * direction.z,
             direction.y,
             -sine * direction.x + cosine * direction.z };
}

Direction rotateAroundZ(Direction direction, double radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { cosine * direction.x - sine * direction.y,
             sine * direction.x + cosine * direction.y,
             direction.z };
}

Direction inverseRotate(Direction direction, AmbisonicOrientation orientation) noexcept
{
    direction = rotateAroundZ(direction, juce::degreesToRadians(-orientation.yawDegrees));
    direction = rotateAroundY(direction, juce::degreesToRadians(orientation.pitchDegrees));
    return rotateAroundX(direction, juce::degreesToRadians(-orientation.rollDegrees));
}

int factorial(int value) noexcept
{
    auto result = 1;
    for (auto factor = 2; factor <= value; ++factor)
        result *= factor;
    return result;
}

double associatedLegendreWithoutCondonShortley(int degree, int order, double z) noexcept
{
    auto pmm = 1.0;
    const auto radial = std::sqrt(juce::jmax(0.0, 1.0 - z * z));
    for (auto index = 1; index <= order; ++index)
        pmm *= static_cast<double>((2 * index) - 1) * radial;
    if (degree == order)
        return pmm;

    auto previousPrevious = pmm;
    auto previous = static_cast<double>((2 * order) + 1) * z * pmm;
    if (degree == order + 1)
        return previous;
    for (auto currentDegree = order + 2; currentDegree <= degree; ++currentDegree)
    {
        const auto current = (static_cast<double>((2 * currentDegree) - 1) * z * previous
                            - static_cast<double>(currentDegree + order - 1) * previousPrevious)
                           / static_cast<double>(currentDegree - order);
        previousPrevious = previous;
        previous = current;
    }
    return previous;
}

void fillSn3dBasis(Direction direction, int channelCount, double* destination) noexcept
{
    const auto azimuth = std::atan2(direction.y, direction.x);
    for (auto acn = 0; acn < channelCount; ++acn)
    {
        const auto degree = static_cast<int>(std::floor(std::sqrt(static_cast<double>(acn))));
        const auto signedOrder = acn - degree * (degree + 1);
        const auto absoluteOrder = std::abs(signedOrder);
        const auto normalization = std::sqrt(
            static_cast<double>(signedOrder == 0 ? 1 : 2)
            * static_cast<double>(factorial(degree - absoluteOrder))
            / static_cast<double>(factorial(degree + absoluteOrder)));
        const auto azimuthTerm = signedOrder < 0
                               ? std::sin(static_cast<double>(absoluteOrder) * azimuth)
                               : std::cos(static_cast<double>(absoluteOrder) * azimuth);
        destination[acn] = normalization
                         * associatedLegendreWithoutCondonShortley(degree, absoluteOrder, direction.z)
                         * azimuthTerm;
    }
}

using RotationMatrix = std::array<std::array<float, maximumAmbisonicChannels>, maximumAmbisonicChannels>;

RotationMatrix buildRotationMatrix(int channelCount, AmbisonicOrientation orientation) noexcept
{
    RotationMatrix result {};
    if (orientation.isIdentity())
    {
        for (auto channel = 0; channel < channelCount; ++channel)
            result[static_cast<std::size_t>(channel)][static_cast<std::size_t>(channel)] = 1.0f;
        return result;
    }

    constexpr int directionCount = 128;
    constexpr auto goldenAngle = juce::MathConstants<double>::pi * (3.0 - 2.2360679774997896964);
    std::array<std::array<double, maximumAmbisonicChannels * 2>, maximumAmbisonicChannels> augmented {};
    std::array<double, maximumAmbisonicChannels> basis {};
    std::array<double, maximumAmbisonicChannels> rotatedBasis {};

    for (auto index = 0; index < directionCount; ++index)
    {
        const auto z = 1.0 - (2.0 * (static_cast<double>(index) + 0.5) / directionCount);
        const auto azimuth = static_cast<double>(index) * goldenAngle;
        const auto radius = std::sqrt(juce::jmax(0.0, 1.0 - z * z));
        const Direction direction { radius * std::cos(azimuth), radius * std::sin(azimuth), z };
        fillSn3dBasis(direction, channelCount, basis.data());
        fillSn3dBasis(inverseRotate(direction, orientation), channelCount, rotatedBasis.data());
        for (auto row = 0; row < channelCount; ++row)
        {
            for (auto column = 0; column < channelCount; ++column)
            {
                augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] +=
                    basis[static_cast<std::size_t>(row)] * basis[static_cast<std::size_t>(column)];
                augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(channelCount + column)] +=
                    basis[static_cast<std::size_t>(row)] * rotatedBasis[static_cast<std::size_t>(column)];
            }
        }
    }

    for (auto pivot = 0; pivot < channelCount; ++pivot)
    {
        auto bestRow = pivot;
        for (auto row = pivot + 1; row < channelCount; ++row)
        {
            if (std::abs(augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(pivot)])
                > std::abs(augmented[static_cast<std::size_t>(bestRow)][static_cast<std::size_t>(pivot)]))
                bestRow = row;
        }
        if (bestRow != pivot)
            std::swap(augmented[static_cast<std::size_t>(bestRow)], augmented[static_cast<std::size_t>(pivot)]);

        const auto divisor = augmented[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(pivot)];
        if (std::abs(divisor) < 1.0e-10)
            continue;
        for (auto column = 0; column < channelCount * 2; ++column)
            augmented[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)] /= divisor;

        for (auto row = 0; row < channelCount; ++row)
        {
            if (row == pivot)
                continue;
            const auto factor = augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(pivot)];
            for (auto column = 0; column < channelCount * 2; ++column)
                augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] -=
                    factor * augmented[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(column)];
        }
    }

    for (auto row = 0; row < channelCount; ++row)
    {
        for (auto column = 0; column < channelCount; ++column)
            result[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] = static_cast<float>(
                augmented[static_cast<std::size_t>(row)][static_cast<std::size_t>(channelCount + column)]);
    }
    return result;
}

std::vector<float> resampleImpulse(const AmbisonicBinauralFilterBank& bank,
                                   int order,
                                   int ear,
                                   int sourceChannel,
                                   double targetSampleRate)
{
    const auto sourceTapCount = bank.getTapCount();
    const auto ratio = targetSampleRate / static_cast<double>(bank.getSampleRate());
    const auto targetTapCount = juce::jmax(1, static_cast<int>(std::ceil(sourceTapCount * ratio)));
    std::vector<float> result(static_cast<std::size_t>(targetTapCount));
    for (auto targetTap = 0; targetTap < targetTapCount; ++targetTap)
    {
        const auto sourcePosition = static_cast<double>(targetTap) / ratio;
        const auto first = juce::jlimit(0, sourceTapCount - 1, static_cast<int>(std::floor(sourcePosition)));
        const auto second = juce::jmin(sourceTapCount - 1, first + 1);
        const auto fraction = static_cast<float>(sourcePosition - first);
        result[static_cast<std::size_t>(targetTap)] =
            bank.getSample(order, ear, sourceChannel, first) * (1.0f - fraction)
          + bank.getSample(order, ear, sourceChannel, second) * fraction;
    }
    return result;
}
} // namespace

std::unique_ptr<AmbisonicBinauralRenderer> AmbisonicBinauralRenderer::create(
    std::shared_ptr<const AmbisonicBinauralFilterBank> filterBank,
    int ambisonicOrder,
    AmbisonicOrientation orientation,
    double sampleRate,
    int maximumBlockSize,
    juce::String& errorMessage)
{
    errorMessage.clear();
    if (filterBank == nullptr || !juce::isPositiveAndBelow(ambisonicOrder, 4)
        || filterBank->getSourceChannelCount(ambisonicOrder) != (ambisonicOrder + 1) * (ambisonicOrder + 1)
        || sampleRate < 8000.0 || sampleRate > 384000.0 || maximumBlockSize <= 0)
    {
        errorMessage = "Binaural renderer configuration is invalid.";
        return {};
    }

    return std::unique_ptr<AmbisonicBinauralRenderer>(
        new AmbisonicBinauralRenderer(std::move(filterBank),
                                      ambisonicOrder,
                                      AmbisonicMonitorDecoder::canonicalizeOrientation(orientation),
                                      sampleRate,
                                      maximumBlockSize));
}

AmbisonicBinauralRenderer::AmbisonicBinauralRenderer(
    std::shared_ptr<const AmbisonicBinauralFilterBank> filterBankToUse,
    int orderToUse,
    AmbisonicOrientation orientation,
    double sampleRate,
    int maximumBlockSizeToUse)
    : filterBank(std::move(filterBankToUse)),
      ambisonicOrder(orderToUse),
      sourceChannels((orderToUse + 1) * (orderToUse + 1)),
      maximumBlockSize(maximumBlockSizeToUse),
      renderSampleRate(sampleRate),
      resourceIdentity(filterBank->getResourceIdentity())
{
    const auto sampleRateRatio = sampleRate / static_cast<double>(filterBank->getSampleRate());
    tapCount = juce::jmax(1, static_cast<int>(std::ceil(filterBank->getTapCount() * sampleRateRatio)));
    historySamples = tapCount - 1;
    auto fftOrder = 0;
    while ((1 << fftOrder) < maximumBlockSize + historySamples)
        ++fftOrder;
    fftSize = 1 << fftOrder;
    fft = std::make_unique<juce::dsp::FFT>(fftOrder);
    filterSpectra.resize(static_cast<std::size_t>(2 * sourceChannels * fftSize));
    inputSpectrum.resize(static_cast<std::size_t>(sourceChannels * fftSize));
    outputSpectrum.resize(static_cast<std::size_t>(fftSize));
    fftInput.resize(static_cast<std::size_t>(fftSize));
    fftOutput.resize(static_cast<std::size_t>(fftSize));
    inputHistory.assign(static_cast<std::size_t>(sourceChannels * historySamples), 0.0f);
    chunkInput.assign(static_cast<std::size_t>(sourceChannels * maximumBlockSize), 0.0f);
    rotatedChunkInput.assign(static_cast<std::size_t>(sourceChannels * maximumBlockSize), 0.0f);

    for (auto ear = 0; ear < 2; ++ear)
    {
        for (auto sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel)
        {
            std::fill(fftInput.begin(), fftInput.end(), Complex {});
            const auto impulse = resampleImpulse(*filterBank,
                                                 ambisonicOrder,
                                                 ear,
                                                 sourceChannel,
                                                 sampleRate);
            for (auto tap = 0; tap < juce::jmin(tapCount, static_cast<int>(impulse.size())); ++tap)
                fftInput[static_cast<std::size_t>(tap)] = Complex(impulse[static_cast<std::size_t>(tap)], 0.0f);
            fft->perform(fftInput.data(),
                         filterSpectra.data() + filterIndex(ear, sourceChannel, 0),
                         false);
        }
    }

    auto initialRotation = std::make_shared<RotationState>();
    initialRotation->matrix = buildRotationMatrix(sourceChannels, orientation);
    initialRotation->orientation = orientation;
    initialRotation->revision = nextRotationRevision++;
    currentRotation = initialRotation->matrix;
    currentRotationRevision = initialRotation->revision;
    publishedRotationState.store(std::move(initialRotation), std::memory_order_release);
}

void AmbisonicBinauralRenderer::setOrientation(AmbisonicOrientation orientation)
{
    orientation = AmbisonicMonitorDecoder::canonicalizeOrientation(orientation);
    const juce::ScopedLock sl(rotationPublicationLock);
    if (const auto current = publishedRotationState.load(std::memory_order_acquire);
        current != nullptr
            && std::abs(current->orientation.yawDegrees - orientation.yawDegrees) < 0.001f
            && std::abs(current->orientation.pitchDegrees - orientation.pitchDegrees) < 0.001f
            && std::abs(current->orientation.rollDegrees - orientation.rollDegrees) < 0.001f)
    {
        return;
    }

    auto next = std::make_shared<RotationState>();
    next->matrix = buildRotationMatrix(sourceChannels, orientation);
    next->orientation = orientation;
    next->revision = nextRotationRevision++;
    auto previous = publishedRotationState.exchange(std::move(next), std::memory_order_acq_rel);
    if (previous != nullptr)
        retiredRotationStates.push_back(std::move(previous));
    if (retiredRotationStates.size() > 16)
    {
        retiredRotationStates.erase(retiredRotationStates.begin(),
                                    retiredRotationStates.end() - 16);
    }
}

bool AmbisonicBinauralRenderer::isCompatible(
    const std::shared_ptr<const AmbisonicBinauralFilterBank>& candidateFilterBank,
    int candidateOrder,
    double candidateSampleRate,
    int candidateMaximumBlockSize) const noexcept
{
    return filterBank == candidateFilterBank
        && ambisonicOrder == candidateOrder
        && std::abs(renderSampleRate - candidateSampleRate) < 0.001
        && maximumBlockSize == candidateMaximumBlockSize;
}

void AmbisonicBinauralRenderer::process(const juce::AudioBuffer<float>& source,
                                        int sourceStartSample,
                                        juce::AudioBuffer<float>& destination,
                                        int destinationStartSample,
                                        int numSamples,
                                        const ChannelPlaybackState& playbackState) noexcept
{
    auto processed = 0;
    while (processed < numSamples)
    {
        const auto chunk = juce::jmin(maximumBlockSize, numSamples - processed);
        processChunk(source,
                     sourceStartSample + processed,
                     destination,
                     destinationStartSample + processed,
                     chunk,
                     playbackState);
        processed += chunk;
    }
}

void AmbisonicBinauralRenderer::reset() noexcept
{
    std::fill(inputHistory.begin(), inputHistory.end(), 0.0f);
    if (const auto target = publishedRotationState.load(std::memory_order_acquire);
        target != nullptr)
    {
        currentRotation = target->matrix;
        currentRotationRevision = target->revision;
    }
}

void AmbisonicBinauralRenderer::processChunk(const juce::AudioBuffer<float>& source,
                                             int sourceStartSample,
                                             juce::AudioBuffer<float>& destination,
                                             int destinationStartSample,
                                             int numSamples,
                                             const ChannelPlaybackState& playbackState) noexcept
{
    const auto readableChannels = juce::jmin(sourceChannels, source.getNumChannels());
    for (auto sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel)
    {
        auto* chunk = chunkInput.data() + static_cast<std::size_t>(sourceChannel * maximumBlockSize);
        if (sourceChannel < readableChannels
            && ChannelRouter::sourceChannelIsAudible(sourceChannel, playbackState))
        {
            juce::FloatVectorOperations::copy(chunk,
                                              source.getReadPointer(sourceChannel, sourceStartSample),
                                              numSamples);
        }
        else
        {
            juce::FloatVectorOperations::clear(chunk, numSamples);
        }
    }

    const auto targetRotation = publishedRotationState.load(std::memory_order_acquire);
    const auto rotationChanged = targetRotation != nullptr
                              && targetRotation->revision != currentRotationRevision;
    for (auto rotatedChannel = 0; rotatedChannel < sourceChannels; ++rotatedChannel)
    {
        auto* rotated = rotatedChunkInput.data()
                      + static_cast<std::size_t>(rotatedChannel * maximumBlockSize);
        juce::FloatVectorOperations::clear(rotated, numSamples);
        for (auto sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel)
        {
            const auto* chunk = chunkInput.data()
                              + static_cast<std::size_t>(sourceChannel * maximumBlockSize);
            const auto startGain = currentRotation[static_cast<std::size_t>(rotatedChannel)]
                                                  [static_cast<std::size_t>(sourceChannel)];
            const auto endGain = targetRotation != nullptr
                               ? targetRotation->matrix[static_cast<std::size_t>(rotatedChannel)]
                                                       [static_cast<std::size_t>(sourceChannel)]
                               : startGain;
            if (!rotationChanged || numSamples <= 1)
            {
                juce::FloatVectorOperations::addWithMultiply(rotated, chunk, endGain, numSamples);
                continue;
            }

            const auto increment = (endGain - startGain) / static_cast<float>(numSamples - 1);
            auto gain = startGain;
            for (auto sample = 0; sample < numSamples; ++sample)
            {
                rotated[sample] += chunk[sample] * gain;
                gain += increment;
            }
        }

        std::fill(fftInput.begin(), fftInput.end(), Complex {});
        if (historySamples > 0)
        {
            const auto* history = inputHistory.data()
                                + static_cast<std::size_t>(rotatedChannel * historySamples);
            for (auto sample = 0; sample < historySamples; ++sample)
                fftInput[static_cast<std::size_t>(sample)] = Complex(history[sample], 0.0f);
        }
        for (auto sample = 0; sample < numSamples; ++sample)
            fftInput[static_cast<std::size_t>(historySamples + sample)] = Complex(rotated[sample], 0.0f);
        fft->perform(fftInput.data(),
                     inputSpectrum.data() + static_cast<std::size_t>(rotatedChannel * fftSize),
                     false);
    }

    if (rotationChanged)
    {
        currentRotation = targetRotation->matrix;
        currentRotationRevision = targetRotation->revision;
    }

    const auto writableEars = juce::jmin(2, destination.getNumChannels());
    for (auto ear = 0; ear < writableEars; ++ear)
    {
        std::fill(outputSpectrum.begin(), outputSpectrum.end(), Complex {});
        for (auto sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel)
        {
            const auto* input = inputSpectrum.data() + static_cast<std::size_t>(sourceChannel * fftSize);
            const auto* filter = filterSpectra.data() + filterIndex(ear, sourceChannel, 0);
            for (auto bin = 0; bin < fftSize; ++bin)
                outputSpectrum[static_cast<std::size_t>(bin)] += input[bin] * filter[bin];
        }
        fft->perform(outputSpectrum.data(), fftOutput.data(), true);
        auto* output = destination.getWritePointer(ear, destinationStartSample);
        for (auto sample = 0; sample < numSamples; ++sample)
            output[sample] = fftOutput[static_cast<std::size_t>(historySamples + sample)].real();
    }
    for (auto channel = writableEars; channel < destination.getNumChannels(); ++channel)
        destination.clear(channel, destinationStartSample, numSamples);

    if (historySamples <= 0)
        return;
    for (auto sourceChannel = 0; sourceChannel < sourceChannels; ++sourceChannel)
    {
        auto* history = inputHistory.data() + static_cast<std::size_t>(sourceChannel * historySamples);
        const auto* chunk = rotatedChunkInput.data()
                          + static_cast<std::size_t>(sourceChannel * maximumBlockSize);
        if (numSamples >= historySamples)
        {
            juce::FloatVectorOperations::copy(history, chunk + numSamples - historySamples, historySamples);
        }
        else
        {
            std::memmove(history,
                         history + numSamples,
                         static_cast<std::size_t>(historySamples - numSamples) * sizeof(float));
            juce::FloatVectorOperations::copy(history + historySamples - numSamples, chunk, numSamples);
        }
    }
}

std::size_t AmbisonicBinauralRenderer::filterIndex(int ear,
                                                   int sourceChannel,
                                                   int bin) const noexcept
{
    return static_cast<std::size_t>(((ear * sourceChannels) + sourceChannel) * fftSize + bin);
}

} // namespace singleeditor
