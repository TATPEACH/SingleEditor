#include "Audio/AmbisonicMonitorDecoder.h"

#include <cmath>

namespace singleeditor
{
namespace
{
struct SpeakerDirection
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

SpeakerDirection rotateAroundX(SpeakerDirection direction, float radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { direction.x,
             (cosine * direction.y) - (sine * direction.z),
             (sine * direction.y) + (cosine * direction.z),
             direction.valid };
}

SpeakerDirection rotateAroundY(SpeakerDirection direction, float radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { (cosine * direction.x) + (sine * direction.z),
             direction.y,
             (-sine * direction.x) + (cosine * direction.z),
             direction.valid };
}

SpeakerDirection rotateAroundZ(SpeakerDirection direction, float radians) noexcept
{
    const auto cosine = std::cos(radians);
    const auto sine = std::sin(radians);
    return { (cosine * direction.x) - (sine * direction.y),
             (sine * direction.x) + (cosine * direction.y),
             direction.z,
             direction.valid };
}

SpeakerDirection inverseRotateDirection(SpeakerDirection direction,
                                        AmbisonicOrientation orientation) noexcept
{
    // Active scene rotation uses +yaw toward left, +pitch toward up, and
    // +roll from left toward up. Decode samples the inverse-rotated speakers.
    direction = rotateAroundZ(direction, juce::degreesToRadians(-orientation.yawDegrees));
    direction = rotateAroundY(direction, juce::degreesToRadians(orientation.pitchDegrees));
    return rotateAroundX(direction, juce::degreesToRadians(-orientation.rollDegrees));
}

bool sourceChannelIsAudible(int sourceChannel,
                            const ChannelPlaybackState& playbackState) noexcept
{
    return ChannelRouter::sourceChannelIsAudible(sourceChannel, playbackState);
}

int factorial(int value) noexcept
{
    auto result = 1;
    for (auto factor = 2; factor <= value; ++factor)
        result *= factor;
    return result;
}

float associatedLegendreWithoutCondonShortley(int degree, int order, float z) noexcept
{
    if (order < 0 || order > degree)
        return 0.0f;

    auto pmm = 1.0f;
    const auto radial = std::sqrt(juce::jmax(0.0f, 1.0f - (z * z)));
    for (auto index = 1; index <= order; ++index)
        pmm *= static_cast<float>((2 * index) - 1) * radial;

    if (degree == order)
        return pmm;

    auto previousPrevious = pmm;
    auto previous = static_cast<float>((2 * order) + 1) * z * pmm;
    if (degree == order + 1)
        return previous;

    for (auto currentDegree = order + 2; currentDegree <= degree; ++currentDegree)
    {
        const auto current = (static_cast<float>((2 * currentDegree) - 1) * z * previous
                            - static_cast<float>(currentDegree + order - 1) * previousPrevious)
                           / static_cast<float>(currentDegree - order);
        previousPrevious = previous;
        previous = current;
    }

    return previous;
}

float sn3dBasisForAcn(int acn, const SpeakerDirection& direction) noexcept
{
    const auto degree = static_cast<int>(std::floor(std::sqrt(static_cast<float>(acn))));
    const auto signedOrder = acn - (degree * (degree + 1));
    const auto absoluteOrder = std::abs(signedOrder);
    const auto normalization = std::sqrt(
        static_cast<float>(signedOrder == 0 ? 1 : 2)
        * static_cast<float>(factorial(degree - absoluteOrder))
        / static_cast<float>(factorial(degree + absoluteOrder)));
    const auto azimuth = std::atan2(direction.y, direction.x);
    const auto azimuthTerm = signedOrder < 0
                           ? std::sin(static_cast<float>(absoluteOrder) * azimuth)
                           : std::cos(static_cast<float>(absoluteOrder) * azimuth);
    return normalization
         * associatedLegendreWithoutCondonShortley(degree, absoluteOrder, direction.z)
         * azimuthTerm;
}

float legendrePolynomial(int degree, float x) noexcept
{
    if (degree <= 0)
        return 1.0f;
    if (degree == 1)
        return x;

    auto previousPrevious = 1.0f;
    auto previous = x;
    for (auto currentDegree = 2; currentDegree <= degree; ++currentDegree)
    {
        const auto current = (static_cast<float>((2 * currentDegree) - 1) * x * previous
                            - static_cast<float>(currentDegree - 1) * previousPrevious)
                           / static_cast<float>(currentDegree);
        previousPrevious = previous;
        previous = current;
    }
    return previous;
}
} // namespace

bool AmbisonicOrientation::isIdentity() const noexcept
{
    return std::abs(yawDegrees) < 0.0001f
        && std::abs(pitchDegrees) < 0.0001f
        && std::abs(rollDegrees) < 0.0001f;
}

AmbisonicMonitorDecoder::AmbisonicMonitorDecoder(
    int ambisonicOrder,
    AmbisonicOrientation orientationToUse) noexcept
    : sourceChannelCount((ambisonicOrder + 1) * (ambisonicOrder + 1)),
      effectiveOrder(ambisonicOrder),
      fixedReferenceGain(fixedReferenceGainForOrder(ambisonicOrder)),
      orientation(orientationToUse)
{
    const std::array<ChannelLayoutState, standardArtifactSlots> layouts {
        ChannelLayoutCatalog::monoOutputLayout(),
        ChannelLayoutCatalog::stereoOutputLayout(),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(4),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(6),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(8),
        ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(12)
    };
    for (const auto& layout : layouts)
    {
        const auto artifact = AmbisonicStandardMatrixCatalog::find(layout, effectiveOrder);
        if (artifact.has_value())
            standardArtifacts[static_cast<std::size_t>(standardArtifactCount++)] = *artifact;
    }
}

std::shared_ptr<const AmbisonicMonitorDecoder> AmbisonicMonitorDecoder::create(
    const ChannelLayoutState& sourceLayout,
    AmbisonicOrientation orientation)
{
    if (statusFor(sourceLayout) != AmbisonicMonitorDecodeStatus::ready)
        return {};

    auto decoder = std::shared_ptr<AmbisonicMonitorDecoder>(
        new AmbisonicMonitorDecoder(sourceLayout.ambisonicOrder,
                                    canonicalizeOrientation(orientation)));
    if (decoder->standardArtifactCount != standardArtifactSlots)
        return {};
    return decoder;
}

AmbisonicOrientation AmbisonicMonitorDecoder::canonicalizeOrientation(
    AmbisonicOrientation orientation) noexcept
{
    const auto wrapDegrees = [](float degrees) noexcept
    {
        if (!std::isfinite(degrees))
            return 0.0f;
        auto wrapped = std::fmod(degrees, 360.0f);
        if (wrapped > 180.0f)
            wrapped -= 360.0f;
        else if (wrapped < -180.0f)
            wrapped += 360.0f;
        return std::abs(wrapped) < 0.0001f ? 0.0f : wrapped;
    };

    orientation.yawDegrees = wrapDegrees(orientation.yawDegrees);
    orientation.pitchDegrees = std::isfinite(orientation.pitchDegrees)
                                 ? juce::jlimit(-90.0f, 90.0f,
                                                orientation.pitchDegrees)
                                 : 0.0f;
    orientation.rollDegrees = wrapDegrees(orientation.rollDegrees);
    return orientation;
}

void AmbisonicMonitorDecoder::evaluateSn3dBasis(
    int ambisonicOrder,
    float azimuthDegrees,
    float elevationDegrees,
    AmbisonicOrientation orientation,
    std::array<float, 16>& destination) noexcept
{
    destination.fill(0.0f);
    if (!juce::isPositiveAndBelow(ambisonicOrder, 4))
        return;

    const auto azimuth = juce::degreesToRadians(azimuthDegrees);
    const auto elevation = juce::degreesToRadians(elevationDegrees);
    const auto elevationCosine = std::cos(elevation);
    auto direction = SpeakerDirection { elevationCosine * std::cos(azimuth),
                                        elevationCosine * std::sin(azimuth),
                                        std::sin(elevation),
                                        true };
    direction = inverseRotateDirection(direction, canonicalizeOrientation(orientation));
    const auto channelCount = (ambisonicOrder + 1) * (ambisonicOrder + 1);
    for (int acn = 0; acn < channelCount; ++acn)
        destination[static_cast<std::size_t>(acn)] = sn3dBasisForAcn(acn, direction);
}

AmbisonicMonitorDecodeStatus AmbisonicMonitorDecoder::statusFor(
    const ChannelLayoutState& sourceLayout) noexcept
{
    if (!sourceLayout.isAmbisonic)
        return AmbisonicMonitorDecodeStatus::notRequired;
    if (!juce::isPositiveAndBelow(sourceLayout.ambisonicOrder, 4)
        || sourceLayout.sourceChannelCount
               != (sourceLayout.ambisonicOrder + 1) * (sourceLayout.ambisonicOrder + 1))
        return AmbisonicMonitorDecodeStatus::unsupportedOrder;
    if (!sourceLayout.ambisonicNormalization.equalsIgnoreCase("SN3D"))
        return AmbisonicMonitorDecodeStatus::unsupportedNormalization;
    if (static_cast<int>(sourceLayout.selectedRoles.size()) != sourceLayout.sourceChannelCount)
        return AmbisonicMonitorDecodeStatus::unsupportedChannelOrdering;
    for (auto sourceChannel = 0; sourceChannel < sourceLayout.sourceChannelCount; ++sourceChannel)
    {
        if (ChannelLayoutCatalog::ambisonicChannelIndex(
                sourceLayout.selectedRoles[static_cast<std::size_t>(sourceChannel)]) != sourceChannel)
        {
            return AmbisonicMonitorDecodeStatus::unsupportedChannelOrdering;
        }
    }
    if (sourceLayout.interpretationOrigin != ChannelInterpretationOrigin::explicitPreset
        && !sourceLayout.detectedFromMetadata)
    {
        return AmbisonicMonitorDecodeStatus::interpretationRequired;
    }
    return AmbisonicMonitorDecodeStatus::ready;
}

juce::String AmbisonicMonitorDecoder::statusToString(AmbisonicMonitorDecodeStatus status)
{
    switch (status)
    {
        case AmbisonicMonitorDecodeStatus::notRequired: return "not_required";
        case AmbisonicMonitorDecodeStatus::ready: return "ready_on_demand";
        case AmbisonicMonitorDecodeStatus::unsupportedOrder: return "unsupported_order";
        case AmbisonicMonitorDecodeStatus::unsupportedNormalization: return "unsupported_normalization";
        case AmbisonicMonitorDecodeStatus::unsupportedChannelOrdering: return "unsupported_channel_ordering";
        case AmbisonicMonitorDecodeStatus::interpretationRequired: return "interpretation_required";
    }

    return "unsupported";
}

juce::String AmbisonicMonitorDecoder::modeFor(const ChannelLayoutState& sourceLayout)
{
    return statusFor(sourceLayout) == AmbisonicMonitorDecodeStatus::ready
         ? "canonical_stage_graph_max_re"
         : juce::String();
}

juce::String AmbisonicMonitorDecoder::decoderProfileId()
{
    return "max-re-137.9deg-fixed-reference-v1";
}

float AmbisonicMonitorDecoder::maxReWeightForDegree(int ambisonicOrder,
                                                     int degree) noexcept
{
    if (!juce::isPositiveAndBelow(ambisonicOrder, 4)
        || degree < 0 || degree > ambisonicOrder)
    {
        return 0.0f;
    }

    const auto maxReArgument = std::cos(juce::degreesToRadians(137.9f)
                                      / (static_cast<float>(ambisonicOrder) + 1.51f));
    return legendrePolynomial(degree, maxReArgument);
}

float AmbisonicMonitorDecoder::fixedReferenceGainForOrder(int ambisonicOrder) noexcept
{
    // Locked per-order reference gains. They are independent of speaker count,
    // direction, active role count and audio blocks; the small reserve keeps the
    // bounded SN3D component sum below unity through third order.
    switch (ambisonicOrder)
    {
        case 1: return 0.50f;
        case 2: return 0.31f;
        case 3: return 0.22f;
        default: return 0.0f;
    }
}

juce::String AmbisonicMonitorDecoder::capabilityFor(const ChannelLayoutState& sourceLayout)
{
    switch (statusFor(sourceLayout))
    {
        case AmbisonicMonitorDecodeStatus::notRequired: return "not_required";
        case AmbisonicMonitorDecodeStatus::ready: return "supported";
        case AmbisonicMonitorDecodeStatus::unsupportedOrder: return "unsupported_order";
        case AmbisonicMonitorDecodeStatus::unsupportedNormalization:
            return "unsupported_normalization";
        case AmbisonicMonitorDecodeStatus::unsupportedChannelOrdering:
            return "unsupported_channel_ordering";
        case AmbisonicMonitorDecodeStatus::interpretationRequired:
            return "interpretation_required";
    }

    return "unsupported";
}

RoutingMatrix AmbisonicMonitorDecoder::buildRoutingMatrix(
    const ChannelLayoutState& targetLayout,
    const ChannelPlaybackState& playbackState) const noexcept
{
    const auto* artifact = standardArtifactFor(targetLayout);
    if (artifact == nullptr)
        return {};

    RoutingMatrix matrix;
    matrix.sourceChannels = sourceChannelCount;
    matrix.outputChannels = artifact->outputChannels;
    matrix.gains.fill(0.0f);
    if (orientation.isIdentity())
    {
        for (auto output = 0; output < matrix.outputChannels; ++output)
        {
            for (auto source = 0; source < matrix.sourceChannels; ++source)
            {
                if (sourceChannelIsAudible(source, playbackState))
                {
                    matrix.gain(output, source) =
                        artifact->gains[(output * artifact->sourceChannels) + source];
                }
            }
        }
        return matrix;
    }

    std::array<float, 16> basis {};
    for (auto output = 0; output < matrix.outputChannels; ++output)
    {
        const auto& speaker = artifact->speakers[static_cast<std::size_t>(output)];
        if (speaker.role == SpeakerRole::LFE)
            continue;
        if (speaker.role == SpeakerRole::mono)
        {
            if (sourceChannelIsAudible(0, playbackState))
                matrix.gain(output, 0) = fixedReferenceGain;
            continue;
        }
        evaluateSn3dBasis(effectiveOrder,
                          speaker.azimuthDegrees,
                          speaker.elevationDegrees,
                          orientation,
                          basis);
        for (auto source = 0; source < matrix.sourceChannels; ++source)
        {
            if (!sourceChannelIsAudible(source, playbackState))
                continue;
            const auto degree = static_cast<int>(
                std::floor(std::sqrt(static_cast<float>(source))));
            matrix.gain(output, source) = fixedReferenceGain
                * maxReWeightForDegree(effectiveOrder, degree)
                * basis[static_cast<std::size_t>(source)];
        }
    }
    return matrix;
}

RoutingMatrix AmbisonicMonitorDecoder::buildRoutingMatrix(
    const AmbisonicDecodeMatrixArtifact& artifact,
    const ChannelPlaybackState& playbackState) const noexcept
{
    if (artifact.ambisonicOrder != effectiveOrder
        || artifact.decodeMatrix.sourceChannels != sourceChannelCount)
    {
        return {};
    }

    RoutingMatrix matrix;
    matrix.sourceChannels = sourceChannelCount;
    matrix.outputChannels = artifact.decodeMatrix.outputChannels;
    matrix.gains.fill(0.0f);
    if (orientation.isIdentity())
    {
        for (auto output = 0; output < matrix.outputChannels; ++output)
        {
            for (auto source = 0; source < matrix.sourceChannels; ++source)
            {
                if (sourceChannelIsAudible(source, playbackState))
                    matrix.gain(output, source) = artifact.decodeMatrix.gain(output, source);
            }
        }
        return matrix;
    }

    std::array<float, 16> basis {};
    auto output = 0;
    for (const auto& speaker : artifact.geometry.speakers)
    {
        if (!speaker.enabled || !speaker.available)
            continue;
        if (output >= matrix.outputChannels)
            return {};
        if (speaker.role == SpeakerRole::mono)
        {
            if (sourceChannelIsAudible(0, playbackState))
                matrix.gain(output, 0) = fixedReferenceGain;
        }
        else if (speaker.role != SpeakerRole::LFE)
        {
            evaluateSn3dBasis(effectiveOrder,
                              speaker.azimuthDegrees,
                              speaker.elevationDegrees,
                              orientation,
                              basis);
            const auto staticWeight = artifact.geometry.weightPolicy
                                        == SpeakerGeometryWeightPolicy::explicitStatic
                                    ? speaker.staticWeight : 1.0f;
            for (auto source = 0; source < matrix.sourceChannels; ++source)
            {
                if (!sourceChannelIsAudible(source, playbackState))
                    continue;
                const auto degree = static_cast<int>(
                    std::floor(std::sqrt(static_cast<float>(source))));
                matrix.gain(output, source) = staticWeight * fixedReferenceGain
                    * maxReWeightForDegree(effectiveOrder, degree)
                    * basis[static_cast<std::size_t>(source)];
            }
        }
        ++output;
    }
    return output == matrix.outputChannels ? matrix : RoutingMatrix {};
}

const StandardAmbisonicMatrixView* AmbisonicMonitorDecoder::standardArtifactFor(
    const ChannelLayoutState& targetLayout) const noexcept
{
    for (auto artifactIndex = 0; artifactIndex < standardArtifactCount; ++artifactIndex)
    {
        const auto& artifact = standardArtifacts[static_cast<std::size_t>(artifactIndex)];
        if (artifact.outputChannels != targetLayout.sourceChannelCount
            || static_cast<int>(targetLayout.selectedRoles.size()) != artifact.outputChannels)
        {
            continue;
        }
        auto matches = true;
        for (auto output = 0; output < artifact.outputChannels; ++output)
        {
            if (artifact.speakers[static_cast<std::size_t>(output)].role
                != targetLayout.selectedRoles[static_cast<std::size_t>(output)])
            {
                matches = false;
                break;
            }
        }
        if (matches)
            return &artifact;
    }
    return nullptr;
}
} // namespace singleeditor
