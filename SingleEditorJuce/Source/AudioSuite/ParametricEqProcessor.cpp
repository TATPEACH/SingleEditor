#include "AudioSuite/ParametricEqProcessor.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace singleeditor
{
std::atomic<int> ParametricEqProcessor::liveInstanceCount { 0 };

namespace
{
constexpr double pi = 3.1415926535897932384626433832795;

ParametricEqBiquadCoefficients normalize(double b0,
                                         double b1,
                                         double b2,
                                         double a0,
                                         double a1,
                                         double a2) noexcept
{
    if (!std::isfinite(a0) || std::abs(a0) < 1.0e-12)
        return {};
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

ParametricEqBiquadCoefficients makeCoefficients(ParametricEqFilterType type,
                                                double frequencyHz,
                                                double gainDb,
                                                double q,
                                                double sampleRate) noexcept
{
    const auto omega = 2.0 * pi * frequencyHz / sampleRate;
    const auto cosine = std::cos(omega);
    const auto sine = std::sin(omega);
    const auto alpha = sine / (2.0 * q);
    const auto a = std::pow(10.0, gainDb / 40.0);
    const auto sqrtA = std::sqrt(a);

    switch (type)
    {
        case ParametricEqFilterType::highPass:
            return normalize((1.0 + cosine) * 0.5,
                             -(1.0 + cosine),
                             (1.0 + cosine) * 0.5,
                             1.0 + alpha,
                             -2.0 * cosine,
                             1.0 - alpha);
        case ParametricEqFilterType::lowPass:
            return normalize((1.0 - cosine) * 0.5,
                             1.0 - cosine,
                             (1.0 - cosine) * 0.5,
                             1.0 + alpha,
                             -2.0 * cosine,
                             1.0 - alpha);
        case ParametricEqFilterType::notch:
            return normalize(1.0,
                             -2.0 * cosine,
                             1.0,
                             1.0 + alpha,
                             -2.0 * cosine,
                             1.0 - alpha);
        case ParametricEqFilterType::lowShelf:
            return normalize(a * ((a + 1.0) - (a - 1.0) * cosine
                                  + 2.0 * sqrtA * alpha),
                             2.0 * a * ((a - 1.0) - (a + 1.0) * cosine),
                             a * ((a + 1.0) - (a - 1.0) * cosine
                                  - 2.0 * sqrtA * alpha),
                             (a + 1.0) + (a - 1.0) * cosine
                                 + 2.0 * sqrtA * alpha,
                             -2.0 * ((a - 1.0) + (a + 1.0) * cosine),
                             (a + 1.0) + (a - 1.0) * cosine
                                 - 2.0 * sqrtA * alpha);
        case ParametricEqFilterType::highShelf:
            return normalize(a * ((a + 1.0) + (a - 1.0) * cosine
                                  + 2.0 * sqrtA * alpha),
                             -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine),
                             a * ((a + 1.0) + (a - 1.0) * cosine
                                  - 2.0 * sqrtA * alpha),
                             (a + 1.0) - (a - 1.0) * cosine
                                 + 2.0 * sqrtA * alpha,
                             2.0 * ((a - 1.0) - (a + 1.0) * cosine),
                             (a + 1.0) - (a - 1.0) * cosine
                                 - 2.0 * sqrtA * alpha);
        case ParametricEqFilterType::bell:
        default:
            return normalize(1.0 + alpha * a,
                             -2.0 * cosine,
                             1.0 - alpha * a,
                             1.0 + alpha / a,
                             -2.0 * cosine,
                             1.0 - alpha / a);
    }
}

std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount>
resolveBands(const ParametricEqParameters& parameters, double sampleRate)
{
    std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> result;
    const auto maximumFrequency = juce::jmax(10.0, sampleRate * 0.49);
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        auto& resolved = result[index];
        resolved.requested = parameters.bands[index];
        resolved.effectiveFrequencyHz = juce::jlimit(10.0,
                                                     maximumFrequency,
                                                     resolved.requested.frequencyHz);
        resolved.effectiveGainDb = juce::jlimit(-24.0,
                                                24.0,
                                                resolved.requested.gainDb);
        resolved.effectiveQ = juce::jlimit(0.1, 18.0, resolved.requested.q);
        resolved.stageCount = resolved.requested.enabled
            ? (resolved.requested.slope == ParametricEqSlope::db24 ? 2 : 1)
            : 0;
        if (resolved.stageCount > 0)
        {
            const auto coefficients = makeCoefficients(resolved.requested.type,
                                                       resolved.effectiveFrequencyHz,
                                                       resolved.effectiveGainDb,
                                                       resolved.effectiveQ,
                                                       sampleRate);
            resolved.stages[0] = coefficients;
            resolved.stages[1] = coefficients;
        }
    }
    return result;
}

bool parametersAreValid(const ParametricEqParameters& parameters) noexcept
{
    for (const auto& band : parameters.bands)
    {
        if (!std::isfinite(band.frequencyHz)
            || !std::isfinite(band.gainDb)
            || !std::isfinite(band.q)
            || band.frequencyHz <= 0.0
            || band.q <= 0.0
            || std::abs(band.gainDb) > 48.0)
        {
            return false;
        }
    }
    return true;
}

struct BiquadState
{
    double z1 = 0.0;
    double z2 = 0.0;

    void reset() noexcept { z1 = z2 = 0.0; }

    double process(double input,
                   const ParametricEqBiquadCoefficients& coefficients) noexcept
    {
        auto output = coefficients.b0 * input + z1;
        z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
        z2 = coefficients.b2 * input - coefficients.a2 * output;
        if (!std::isfinite(output) || !std::isfinite(z1) || !std::isfinite(z2))
        {
            reset();
            return 0.0;
        }
        return juce::jlimit(-32.0, 32.0, output);
    }
};

class ParametricEqRenderSession final : public AudioSuiteRenderSession
{
public:
    ParametricEqRenderSession(
        const AudioSuitePlan& plan,
        std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> bandsToUse)
        : selection(plan.getSelection()),
          analysisReadRange(plan.getAnalysisReadRange()),
          boundaryCrossfadeSamples(plan.getBoundaryCrossfadeSamples()),
          bands(std::move(bandsToUse)),
          channelStates(static_cast<std::size_t>(selection.sourceChannelCount))
    {
    }

    void reset() noexcept override
    {
        for (auto& channel : channelStates)
            for (auto& band : channel)
                for (auto& stage : band)
                    stage.reset();
        expectedNextSample.reset();
    }

    void renderBlockInPlace(juce::AudioBuffer<float>& buffer,
                            int64_t absoluteStartSample,
                            juce::Range<int64_t> activeRange) noexcept override
    {
        if (expectedNextSample.has_value() && *expectedNextSample != absoluteStartSample)
            reset();
        expectedNextSample = absoluteStartSample + buffer.getNumSamples();

        const auto channels = juce::jmin(buffer.getNumChannels(),
                                         selection.sourceChannelCount);
        for (int channel = 0; channel < channels; ++channel)
        {
            if (!selection.containsSourceChannel(channel))
                continue;

            auto* samples = buffer.getWritePointer(channel);
            auto& states = channelStates[static_cast<std::size_t>(channel)];
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto absoluteSample = absoluteStartSample + sample;
                if (!analysisReadRange.contains(absoluteSample))
                    continue;

                const auto dry = static_cast<double>(samples[sample]);
                auto wet = dry;
                for (std::size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex)
                {
                    for (int stage = 0; stage < bands[bandIndex].stageCount; ++stage)
                    {
                        wet = states[bandIndex][static_cast<std::size_t>(stage)].process(
                            wet,
                            bands[bandIndex].stages[static_cast<std::size_t>(stage)]);
                    }
                }

                const auto mix = boundaryWetForPlan(absoluteSample, activeRange);
                if (mix > 0.0)
                    samples[sample] = static_cast<float>(dry + (wet - dry) * mix);
            }
        }
    }

private:
    double boundaryWetForPlan(int64_t absoluteSample,
                              juce::Range<int64_t> activeRange) const noexcept
    {
        const auto writeRange = selection.sampleRange.getIntersectionWith(activeRange);
        if (!writeRange.contains(absoluteSample))
            return 0.0;
        const auto fadeSamples = juce::jmin<int64_t>(boundaryCrossfadeSamples,
                                                     writeRange.getLength() / 2);
        if (fadeSamples <= 0)
            return 1.0;
        const auto edgeDistance = juce::jmin(absoluteSample - writeRange.getStart(),
                                             writeRange.getEnd() - 1 - absoluteSample);
        return juce::jlimit(0.0,
                            1.0,
                            static_cast<double>(edgeDistance)
                                / static_cast<double>(fadeSamples));
    }

    using BandStates = std::array<std::array<BiquadState, 2>,
                                  ParametricEqParameters::bandCount>;
    ResolvedAudioSelection selection;
    juce::Range<int64_t> analysisReadRange;
    int boundaryCrossfadeSamples = 0;
    std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> bands;
    std::vector<BandStates> channelStates;
    std::optional<int64_t> expectedNextSample;
};

class ParametricEqRenderFactory final : public AudioSuiteRenderFactory
{
public:
    explicit ParametricEqRenderFactory(
        std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> bandsToUse)
        : bands(std::move(bandsToUse))
    {
    }

    std::unique_ptr<AudioSuiteRenderSession> createRenderSession(
        const AudioSuitePlan& plan) const override
    {
        return std::make_unique<ParametricEqRenderSession>(plan, bands);
    }

private:
    std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> bands;
};

ParametricEqParameters makeFlatParameters()
{
    ParametricEqParameters parameters;
    const std::array<ParametricEqFilterType, ParametricEqParameters::bandCount> types {
        ParametricEqFilterType::highPass,
        ParametricEqFilterType::lowShelf,
        ParametricEqFilterType::bell,
        ParametricEqFilterType::bell,
        ParametricEqFilterType::bell,
        ParametricEqFilterType::bell,
        ParametricEqFilterType::highShelf,
        ParametricEqFilterType::lowPass
    };
    const std::array<double, ParametricEqParameters::bandCount> frequencies {
        30.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 8000.0, 20000.0
    };
    for (std::size_t index = 0; index < parameters.bands.size(); ++index)
    {
        parameters.bands[index].type = types[index];
        parameters.bands[index].frequencyHz = frequencies[index];
    }
    return parameters;
}
} // namespace

ParametricEqParameters ParametricEqParameters::flat()
{
    return makeFlatParameters();
}

bool ParametricEqBiquadCoefficients::isFinite() const noexcept
{
    return std::isfinite(b0) && std::isfinite(b1) && std::isfinite(b2)
        && std::isfinite(a1) && std::isfinite(a2);
}

double ParametricEqBiquadCoefficients::magnitudeAt(double frequencyHz,
                                                  double sampleRate) const noexcept
{
    if (!isFinite() || sampleRate <= 0.0 || !std::isfinite(frequencyHz))
        return 0.0;
    const auto omega = 2.0 * pi * juce::jlimit(0.0, sampleRate * 0.5, frequencyHz)
                     / sampleRate;
    const auto z1 = std::polar(1.0, -omega);
    const auto z2 = z1 * z1;
    const auto numerator = b0 + b1 * z1 + b2 * z2;
    const auto denominator = 1.0 + a1 * z1 + a2 * z2;
    if (std::abs(denominator) < 1.0e-15)
        return 0.0;
    return std::abs(numerator / denominator);
}

double ParametricEqPlan::magnitudeAt(double frequencyHz) const noexcept
{
    auto magnitude = 1.0;
    for (const auto& band : resolvedBands)
        for (int stage = 0; stage < band.stageCount; ++stage)
            magnitude *= band.stages[static_cast<std::size_t>(stage)]
                             .magnitudeAt(frequencyHz, sampleRate);
    return magnitude;
}

std::unique_ptr<ParametricEqProcessor>
ParametricEqProcessor::CapabilityDescriptor::create() const
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto request = OptionalLifecycleToken::request(currentOptionalLifecycleContext(),
        OptionalLifecycleOwner::parametricEq, OptionalLifecycleResourceKind::processor);
    try
    {
        auto processor = std::unique_ptr<ParametricEqProcessor>(new ParametricEqProcessor(*this));
        request.created();
        processor->lifecycleRelease.token = std::move(request);
        return processor;
    }
    catch (...)
    {
        request.failed();
        throw;
    }
#else
    return std::unique_ptr<ParametricEqProcessor>(new ParametricEqProcessor(*this));
#endif
}

ParametricEqProcessor::CapabilityDescriptor ParametricEqProcessor::describe() noexcept
{
    return {};
}

std::vector<juce::String> ParametricEqProcessor::automationSafeParameterIds()
{
    std::vector<juce::String> ids;
    ids.reserve(1 + ParametricEqParameters::bandCount * 6);
    ids.push_back("eq.preset");
    for (std::size_t band = 0; band < ParametricEqParameters::bandCount; ++band)
    {
        const auto prefix = "eq.band" + juce::String(static_cast<int>(band + 1)) + ".";
        ids.push_back(prefix + "enabled");
        ids.push_back(prefix + "type");
        ids.push_back(prefix + "frequency-hz");
        ids.push_back(prefix + "gain-db");
        ids.push_back(prefix + "q");
        ids.push_back(prefix + "slope");
    }
    return ids;
}

std::vector<ParametricEqProcessor::Preset> ParametricEqProcessor::builtInPresets()
{
    auto flat = ParametricEqParameters::flat();

    auto vocal = flat;
    vocal.presetId = "vocal-clarity";
    vocal.bands[0].enabled = true;
    vocal.bands[0].frequencyHz = 80.0;
    vocal.bands[2].enabled = true;
    vocal.bands[2].frequencyHz = 250.0;
    vocal.bands[2].gainDb = -2.0;
    vocal.bands[2].q = 1.0;
    vocal.bands[5].enabled = true;
    vocal.bands[5].frequencyHz = 3500.0;
    vocal.bands[5].gainDb = 2.5;
    vocal.bands[5].q = 0.9;

    auto bass = flat;
    bass.presetId = "bass-control";
    bass.bands[0].enabled = true;
    bass.bands[0].frequencyHz = 30.0;
    bass.bands[0].slope = ParametricEqSlope::db24;
    bass.bands[1].enabled = true;
    bass.bands[1].frequencyHz = 90.0;
    bass.bands[1].gainDb = -2.0;

    return {
        { "flat", "Flat", std::move(flat) },
        { "vocal-clarity", "Vocal Clarity", std::move(vocal) },
        { "bass-control", "Bass Control", std::move(bass) }
    };
}

std::optional<ParametricEqParameters> ParametricEqProcessor::parametersForPreset(
    const juce::String& stableId)
{
    for (const auto& preset : builtInPresets())
        if (preset.stableId == stableId)
            return preset.parameters;
    return std::nullopt;
}

std::vector<float> ParametricEqProcessor::magnitudeResponse(
    const ParametricEqParameters& parameters, double sampleRate,
    double minimumHz, double maximumHz, int pointCount)
{
    if (!parametersAreValid(parameters) || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(minimumHz) || !std::isfinite(maximumHz) || minimumHz <= 0.0
        || maximumHz <= minimumHz || maximumHz > sampleRate * 0.5
        || pointCount < 2 || pointCount > 4096)
        return {};
    ParametricEqPlan coefficients;
    coefficients.sampleRate = sampleRate;
    coefficients.resolvedBands = resolveBands(parameters, sampleRate);
    for (const auto& band : coefficients.resolvedBands)
        for (int stage = 0; stage < band.stageCount; ++stage)
            if (!band.stages[static_cast<std::size_t>(stage)].isFinite())
                return {};
    std::vector<float> response;
    response.reserve(static_cast<std::size_t>(pointCount));
    const auto logMinimum = std::log(minimumHz);
    const auto logRange = std::log(maximumHz) - logMinimum;
    for (int point = 0; point < pointCount; ++point)
    {
        const auto frequency = std::exp(logMinimum + logRange * point / (pointCount - 1));
        response.push_back(static_cast<float>(juce::Decibels::gainToDecibels(
            coefficients.magnitudeAt(frequency), -120.0)));
    }
    return response;
}

ParametricEqProcessor::AnalyzeResult ParametricEqProcessor::analyze(
    SingleTrackDocument::StatePtr state,
    int64_t documentGeneration,
    const AudioSelectionRequest& selection,
    const ParametricEqParameters& parameters,
    double maximumPreviewSeconds) const
{
    AnalyzeResult result;
    if (state == nullptr || !state->info.isValid())
    {
        result.errorMessage = "No valid source audio is available.";
        return result;
    }
    if (!parametersAreValid(parameters))
    {
        result.errorMessage = "One or more EQ parameters are invalid.";
        return result;
    }

    const auto resolvedBands = resolveBands(parameters, state->info.sampleRate);
    for (const auto& band : resolvedBands)
    {
        for (int stage = 0; stage < band.stageCount; ++stage)
        {
            if (!band.stages[static_cast<std::size_t>(stage)].isFinite())
            {
                result.errorMessage = "EQ coefficient resolution failed.";
                return result;
            }
        }
    }

    AudioSuitePlanBuildRequest request;
    request.state = state;
    request.documentGeneration = documentGeneration;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    request.lifecycleContext = observedContext();
    request.lifecycleOwner = OptionalLifecycleOwner::parametricEq;
    request.processorResourceId = observedResourceId();
#endif
    request.selection = selection;
    request.processorId = descriptor.processorId;
    request.processorVersion = descriptor.processorVersion;
    request.renderFactory = std::make_shared<ParametricEqRenderFactory>(resolvedBands);
    request.maximumPreviewSeconds = maximumPreviewSeconds;
    const auto transactionPlan = AudioSuitePlanBuilder::build(request);
    if (!transactionPlan.succeeded())
    {
        result.errorMessage = transactionPlan.errorMessage;
        return result;
    }

    auto plan = std::make_shared<ParametricEqPlan>();
    plan->audioSuitePlan = transactionPlan.plan;
    plan->parameters = parameters;
    plan->resolvedBands = resolvedBands;
    plan->sampleRate = state->info.sampleRate;
    result.plan = std::move(plan);
    return result;
}

AudioSuitePreviewHandle ParametricEqProcessor::createPreview(
    const ParametricEqPlanPtr& plan) const
{
    if (plan == nullptr
        || plan->getAudioSuitePlan() == nullptr
        || plan->getAudioSuitePlan()->getProcessorId() != descriptor.processorId
        || plan->getAudioSuitePlan()->getProcessorVersion() != descriptor.processorVersion)
    {
        return {};
    }
    return AudioSuiteTransactionExecutor::createPreview(plan->getAudioSuitePlan());
}

AudioSuiteApplyResult ParametricEqProcessor::apply(
    SingleTrackDocument& document,
    const ParametricEqPlanPtr& plan,
    int64_t currentDocumentGeneration,
    const AudioSuiteCancellationToken& cancellation,
    int renderBlockSize) const
{
    if (plan == nullptr
        || plan->getAudioSuitePlan() == nullptr
        || plan->getAudioSuitePlan()->getProcessorId() != descriptor.processorId
        || plan->getAudioSuitePlan()->getProcessorVersion() != descriptor.processorVersion)
    {
        return {};
    }
    return AudioSuiteTransactionExecutor::apply(document,
                                                plan->getAudioSuitePlan(),
                                                currentDocumentGeneration,
                                                cancellation,
                                                renderBlockSize);
}

bool ParametricEqProcessor::renderPlanForDiagnostics(
    const ParametricEqPlan& plan,
    juce::AudioBuffer<float>& buffer,
    int64_t absoluteStartSample,
    juce::Range<int64_t> activeRange)
{
    if (plan.getAudioSuitePlan() == nullptr)
        return false;
    auto session = plan.getAudioSuitePlan()->createRenderSession();
    if (session == nullptr)
        return false;
    session->renderBlockInPlace(buffer, absoluteStartSample, activeRange);
    return true;
}

int ParametricEqProcessor::getLiveInstanceCountForDiagnostics() noexcept
{
    return liveInstanceCount.load(std::memory_order_acquire);
}

ParametricEqProcessor::ParametricEqProcessor(CapabilityDescriptor descriptorToUse)
    : descriptor(std::move(descriptorToUse))
{
    liveInstanceCount.fetch_add(1, std::memory_order_acq_rel);
}

ParametricEqProcessor::~ParametricEqProcessor()
{
    liveInstanceCount.fetch_sub(1, std::memory_order_acq_rel);

}
} // namespace singleeditor
