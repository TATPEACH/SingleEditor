#include "AudioSuite/TruePeakLimiterProcessor.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>

namespace singleeditor
{
std::atomic<int> TruePeakLimiterProcessor::liveInstanceCount { 0 };

namespace
{
constexpr double internalGuardDb = 0.10;

double dbToGain(double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

double gainToDb(double gain) noexcept
{
    return gain > 0.0 && std::isfinite(gain)
         ? 20.0 * std::log10(gain)
         : -std::numeric_limits<double>::infinity();
}

void atomicMaximum(std::atomic<double>& target, double value) noexcept
{
    if (!std::isfinite(value))
        return;
    auto current = target.load(std::memory_order_relaxed);
    while (value > current
           && !target.compare_exchange_weak(current,
                                            value,
                                            std::memory_order_release,
                                            std::memory_order_relaxed))
    {
    }
}

double cubicInterpolate(double p0,
                        double p1,
                        double p2,
                        double p3,
                        double position) noexcept
{
    const auto x2 = position * position;
    const auto x3 = x2 * position;
    return 0.5 * ((2.0 * p1)
                + (-p0 + p2) * position
                + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * x2
                + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * x3);
}

double sampleAt(const juce::AudioBuffer<float>& buffer,
                int channel,
                int sample,
                double previousSample) noexcept
{
    if (sample < 0)
        return previousSample;
    if (sample >= buffer.getNumSamples())
        sample = buffer.getNumSamples() - 1;
    if (!juce::isPositiveAndBelow(channel, buffer.getNumChannels())
        || !juce::isPositiveAndBelow(sample, buffer.getNumSamples()))
    {
        return 0.0;
    }
    const auto value = static_cast<double>(buffer.getSample(channel, sample));
    return std::isfinite(value) ? value : 0.0;
}

double oversampledFramePeak(const juce::AudioBuffer<float>& buffer,
                            int channel,
                            int sample,
                            int factor,
                            double previousSample) noexcept
{
    const auto p0 = sampleAt(buffer, channel, sample - 1, previousSample);
    const auto p1 = sampleAt(buffer, channel, sample, previousSample);
    if (factor <= 1)
        return std::abs(p1);

    const auto p2 = sampleAt(buffer, channel, sample + 1, previousSample);
    const auto p3 = sampleAt(buffer, channel, sample + 2, previousSample);
    auto peak = std::abs(p1);
    for (int phase = 1; phase < factor; ++phase)
    {
        const auto value = cubicInterpolate(p0,
                                            p1,
                                            p2,
                                            p3,
                                            static_cast<double>(phase)
                                                / static_cast<double>(factor));
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

bool parametersAreValid(const TruePeakLimiterParameters& parameters) noexcept
{
    return parameters.enabled
        && std::isfinite(parameters.thresholdDb)
        && parameters.thresholdDb >= -36.0
        && parameters.thresholdDb <= 0.0
        && std::isfinite(parameters.ceilingDbtp)
        && parameters.ceilingDbtp >= -12.0
        && parameters.ceilingDbtp <= 0.0
        && std::isfinite(parameters.lookAheadMs)
        && std::abs(parameters.lookAheadMs - 5.0) <= 1.0e-9
        && std::isfinite(parameters.releaseMs)
        && parameters.releaseMs >= 20.0
        && parameters.releaseMs <= 2000.0
        && parameters.oversampling
            == TruePeakLimiterOversampling::sampleRateAwareFixed
        && parameters.channelsLinked;
}

class TruePeakLimiterRenderSession final : public AudioSuiteRenderSession
{
public:
    TruePeakLimiterRenderSession(
        const AudioSuitePlan& plan,
        TruePeakLimiterParameters parametersToUse,
        double sampleRateToUse,
        int oversamplingFactorToUse,
        int lookAheadSamplesToUse,
        std::shared_ptr<TruePeakLimiterFeedbackState> feedbackToUse)
        : selection(plan.getSelection()),
          parameters(std::move(parametersToUse)),
          sampleRate(sampleRateToUse),
          oversamplingFactor(oversamplingFactorToUse),
          lookAheadSamples(lookAheadSamplesToUse),
          inputGain(dbToGain(-parameters.thresholdDb)),
          guardedCeilingLinear(dbToGain(parameters.ceilingDbtp - internalGuardDb)),
          feedback(std::move(feedbackToUse)),
          previousInputSamples(static_cast<std::size_t>(selection.sourceChannelCount), 0.0)
    {
    }

    void reset() noexcept override
    {
        linkedGain = 1.0;
        expectedNextSample.reset();
        previousProcessedSample.reset();
        std::fill(previousInputSamples.begin(), previousInputSamples.end(), 0.0);
    }

    int getRequiredLookAheadSamples() const noexcept override
    {
        return lookAheadSamples + 2;
    }

    void renderBlockInPlace(juce::AudioBuffer<float>& buffer,
                            int64_t absoluteStartSample,
                            juce::Range<int64_t> activeRange) noexcept override
    {
        renderBlockWithLookAheadInPlace(buffer,
                                        buffer.getNumSamples(),
                                        absoluteStartSample,
                                        activeRange);
    }

    void renderBlockWithLookAheadInPlace(juce::AudioBuffer<float>& buffer,
                                         int validOutputSamples,
                                         int64_t absoluteStartSample,
                                         juce::Range<int64_t> activeRange) noexcept override
    {
        validOutputSamples = juce::jlimit(0, buffer.getNumSamples(), validOutputSamples);
        if (validOutputSamples <= 0)
            return;
        if (expectedNextSample.has_value() && *expectedNextSample != absoluteStartSample)
            reset();
        expectedNextSample = absoluteStartSample + validOutputSamples;

        const auto channels = juce::jmin(buffer.getNumChannels(),
                                         selection.sourceChannelCount);
        const auto writeRange = selection.sampleRange.getIntersectionWith(activeRange);
        if (channels <= 0 || writeRange.isEmpty())
            return;

        auto lastInputSamples = previousInputSamples;
        for (int channel = 0; channel < channels; ++channel)
        {
            if (selection.containsSourceChannel(channel))
                lastInputSamples[static_cast<std::size_t>(channel)] =
                    sampleAt(buffer,
                             channel,
                             validOutputSamples - 1,
                             previousInputSamples[static_cast<std::size_t>(channel)]);
        }

        const auto detectorCount = juce::jmin(
            buffer.getNumSamples(), validOutputSamples + lookAheadSamples + 1);
        std::vector<double> detectorPeaks(static_cast<std::size_t>(detectorCount), 0.0);
        for (int sample = 0; sample < detectorCount; ++sample)
        {
            const auto absoluteSample = absoluteStartSample + sample;
            if (!writeRange.contains(absoluteSample))
                continue;
            auto peak = 0.0;
            for (int channel = 0; channel < channels; ++channel)
            {
                if (!selection.containsSourceChannel(channel))
                    continue;
                peak = std::max(
                    peak,
                    oversampledFramePeak(
                        buffer,
                        channel,
                        sample,
                        oversamplingFactor,
                        previousInputSamples[static_cast<std::size_t>(channel)]));
            }
            detectorPeaks[static_cast<std::size_t>(sample)] = peak * inputGain;
        }

        std::vector<double> futurePeaks(static_cast<std::size_t>(validOutputSamples), 0.0);
        std::deque<int> maxima;
        for (int sample = detectorCount - 1; sample >= 0; --sample)
        {
            while (!maxima.empty() && maxima.front() > sample + lookAheadSamples)
                maxima.pop_front();
            while (!maxima.empty()
                   && detectorPeaks[static_cast<std::size_t>(maxima.back())]
                        <= detectorPeaks[static_cast<std::size_t>(sample)])
            {
                maxima.pop_back();
            }
            maxima.push_back(sample);
            if (sample < validOutputSamples)
            {
                futurePeaks[static_cast<std::size_t>(sample)] =
                    detectorPeaks[static_cast<std::size_t>(maxima.front())];
            }
        }

        auto blockMaximumInputPeak = 0.0;
        auto blockMaximumGainReduction = 0.0;
        int64_t processedSamples = 0;
        for (int sample = 0; sample < validOutputSamples; ++sample)
        {
            const auto absoluteSample = absoluteStartSample + sample;
            if (!writeRange.contains(absoluteSample))
                continue;
            if (!previousProcessedSample.has_value()
                || *previousProcessedSample + 1 != absoluteSample)
            {
                linkedGain = 1.0;
            }
            previousProcessedSample = absoluteSample;

            const auto peak = futurePeaks[static_cast<std::size_t>(sample)];
            const auto requiredGain = peak > guardedCeilingLinear
                ? guardedCeilingLinear / peak
                : 1.0;
            if (requiredGain < linkedGain)
            {
                linkedGain = requiredGain;
            }
            else
            {
                auto releaseMs = parameters.releaseMs;
                if (parameters.releaseMode == TruePeakLimiterReleaseMode::automatic)
                {
                    const auto reduction = juce::jlimit(0.0, 1.0, 1.0 - linkedGain);
                    releaseMs = juce::jlimit(40.0,
                                             800.0,
                                             60.0 + 360.0 * reduction);
                }
                const auto coefficient = std::exp(
                    -1.0 / (juce::jmax(0.001, releaseMs * 0.001) * sampleRate));
                linkedGain = requiredGain + coefficient * (linkedGain - requiredGain);
            }
            linkedGain = juce::jlimit(0.0, 1.0, linkedGain);

            // A dry boundary crossfade cannot preserve a brickwall ceiling when
            // the unprocessed edge already exceeds it. Limiter selections are
            // therefore fully wet inside the exact write range; samples outside
            // the range remain byte-for-byte untouched by the transaction.
            constexpr auto wet = 1.0;
            for (int channel = 0; channel < channels; ++channel)
            {
                if (!selection.containsSourceChannel(channel))
                    continue;
                const auto dry = sampleAt(
                    buffer,
                    channel,
                    sample,
                    previousInputSamples[static_cast<std::size_t>(channel)]);
                const auto limited = dry * inputGain * linkedGain;
                const auto output = dry + (limited - dry) * wet;
                buffer.setSample(channel,
                                 sample,
                                 static_cast<float>(std::isfinite(output) ? output : 0.0));
            }
            blockMaximumInputPeak = std::max(blockMaximumInputPeak, peak);
            blockMaximumGainReduction = std::max(blockMaximumGainReduction,
                                                  -gainToDb(juce::jmax(linkedGain,
                                                                      1.0e-12)));
            ++processedSamples;
        }

        previousInputSamples = std::move(lastInputSamples);
        if (feedback != nullptr)
            feedback->observe(blockMaximumInputPeak,
                              blockMaximumGainReduction,
                              processedSamples);
    }

    bool requiresPostRenderVerification() const noexcept override { return true; }
    int getPostRenderVerificationLookAheadSamples() const noexcept override { return 2; }

    void beginPostRenderVerification(int numChannels,
                                     int64_t numSamples,
                                     double verificationSampleRate) noexcept override
    {
        verification = {};
        verification.performed = true;
        verification.verifiedChannels = numChannels;
        verification.sampleRate = verificationSampleRate;
        expectedVerificationChannels = numChannels;
        expectedVerificationSamples = numSamples;
        verificationPreviousSamples.assign(
            static_cast<std::size_t>(juce::jmax(0, numChannels)), 0.0);
        verification.passed = numChannels == selection.sourceChannelCount
            && numSamples > 0
            && std::abs(verificationSampleRate - sampleRate) <= 1.0e-6;
        if (!verification.passed)
            verification.failureMessage = "Rendered duration, channel count or sample rate changed.";
    }

    bool verifyRenderedBlock(const juce::AudioBuffer<float>& buffer,
                             int validOutputSamples,
                             int64_t absoluteStartSample,
                             juce::Range<int64_t> activeRange) noexcept override
    {
        validOutputSamples = juce::jlimit(0, buffer.getNumSamples(), validOutputSamples);
        if (buffer.getNumChannels() != expectedVerificationChannels
            || validOutputSamples <= 0)
        {
            verification.passed = false;
            verification.failureMessage = "Rendered block shape changed during verification.";
            return false;
        }

        const auto writeRange = selection.sampleRange.getIntersectionWith(activeRange);
        auto maximumPeak = std::isfinite(verification.maximumTruePeakDbtp)
            ? dbToGain(verification.maximumTruePeakDbtp)
            : 0.0;
        const auto channels = juce::jmin(buffer.getNumChannels(),
                                         selection.sourceChannelCount);
        for (int sample = 0; sample < validOutputSamples; ++sample)
        {
            const auto absoluteSample = absoluteStartSample + sample;
            if (!writeRange.contains(absoluteSample))
                continue;
            for (int channel = 0; channel < channels; ++channel)
            {
                if (!selection.containsSourceChannel(channel))
                    continue;
                const auto value = static_cast<double>(buffer.getSample(channel, sample));
                if (!std::isfinite(value))
                {
                    ++verification.nonFiniteSamples;
                    continue;
                }
                if (std::abs(value) > 1.0 + 1.0e-7)
                    ++verification.clippedSamples;
                maximumPeak = std::max(
                    maximumPeak,
                    oversampledFramePeak(buffer,
                                         channel,
                                         sample,
                                         oversamplingFactor,
                                         verificationPreviousSamples[
                                             static_cast<std::size_t>(channel)]));
            }
        }
        for (int channel = 0; channel < channels; ++channel)
        {
            verificationPreviousSamples[static_cast<std::size_t>(channel)] =
                sampleAt(buffer,
                         channel,
                         validOutputSamples - 1,
                         verificationPreviousSamples[static_cast<std::size_t>(channel)]);
        }
        verification.maximumTruePeakDbtp = gainToDb(maximumPeak);
        verification.verifiedSamples += validOutputSamples;
        return true;
    }

    AudioSuiteRenderVerification finishPostRenderVerification() noexcept override
    {
        const auto shapeMatches = verification.verifiedChannels
                == expectedVerificationChannels
            && verification.verifiedSamples == expectedVerificationSamples
            && verification.sampleRate > 0.0;
        const auto peakPasses = !std::isfinite(verification.maximumTruePeakDbtp)
            || verification.maximumTruePeakDbtp <= parameters.ceilingDbtp + 0.02;
        verification.passed = verification.passed
            && shapeMatches
            && peakPasses
            && verification.clippedSamples == 0
            && verification.nonFiniteSamples == 0;
        if (!verification.passed && verification.failureMessage.isEmpty())
        {
            if (!shapeMatches)
                verification.failureMessage = "Rendered duration or channel count verification failed.";
            else if (verification.nonFiniteSamples > 0)
                verification.failureMessage = "Rendered output contains NaN or Inf.";
            else if (verification.clippedSamples > 0)
                verification.failureMessage = "Rendered output contains clipped samples.";
            else
                verification.failureMessage = "Rendered output exceeds the true-peak ceiling.";
        }
        return verification;
    }

private:
    ResolvedAudioSelection selection;
    TruePeakLimiterParameters parameters;
    double sampleRate = 0.0;
    int oversamplingFactor = 1;
    int lookAheadSamples = 0;
    double inputGain = 1.0;
    double guardedCeilingLinear = 1.0;
    std::shared_ptr<TruePeakLimiterFeedbackState> feedback;
    std::vector<double> previousInputSamples;
    double linkedGain = 1.0;
    std::optional<int64_t> expectedNextSample;
    std::optional<int64_t> previousProcessedSample;
    AudioSuiteRenderVerification verification;
    std::vector<double> verificationPreviousSamples;
    int expectedVerificationChannels = 0;
    int64_t expectedVerificationSamples = 0;
};

class TruePeakLimiterRenderFactory final : public AudioSuiteRenderFactory
{
public:
    TruePeakLimiterRenderFactory(
        TruePeakLimiterParameters parametersToUse,
        double sampleRateToUse,
        int oversamplingFactorToUse,
        int lookAheadSamplesToUse,
        std::shared_ptr<TruePeakLimiterFeedbackState> feedbackToUse)
        : parameters(std::move(parametersToUse)),
          sampleRate(sampleRateToUse),
          oversamplingFactor(oversamplingFactorToUse),
          lookAheadSamples(lookAheadSamplesToUse),
          feedback(std::move(feedbackToUse))
    {
    }

    std::unique_ptr<AudioSuiteRenderSession> createRenderSession(
        const AudioSuitePlan& plan) const override
    {
        return std::make_unique<TruePeakLimiterRenderSession>(plan,
                                                              parameters,
                                                              sampleRate,
                                                              oversamplingFactor,
                                                              lookAheadSamples,
                                                              feedback);
    }

private:
    TruePeakLimiterParameters parameters;
    double sampleRate = 0.0;
    int oversamplingFactor = 1;
    int lookAheadSamples = 0;
    std::shared_ptr<TruePeakLimiterFeedbackState> feedback;
};
} // namespace

TruePeakLimiterParameters TruePeakLimiterParameters::masteringDefault()
{
    return {};
}

void TruePeakLimiterFeedbackState::reset() noexcept
{
    maximumInputTruePeakLinear.store(0.0, std::memory_order_release);
    maximumGainReductionDb.store(0.0, std::memory_order_release);
    processedSamples.store(0, std::memory_order_release);
}

void TruePeakLimiterFeedbackState::observe(double inputTruePeakLinear,
                                           double gainReductionDb,
                                           int64_t sampleCount) noexcept
{
    atomicMaximum(maximumInputTruePeakLinear, inputTruePeakLinear);
    atomicMaximum(maximumGainReductionDb, gainReductionDb);
    processedSamples.fetch_add(juce::jmax<int64_t>(0, sampleCount),
                               std::memory_order_acq_rel);
}

TruePeakLimiterFeedbackSnapshot TruePeakLimiterFeedbackState::snapshot() const noexcept
{
    TruePeakLimiterFeedbackSnapshot result;
    result.inputTruePeakDbtp = gainToDb(
        maximumInputTruePeakLinear.load(std::memory_order_acquire));
    result.maximumGainReductionDb = maximumGainReductionDb.load(
        std::memory_order_acquire);
    result.processedSamples = processedSamples.load(std::memory_order_acquire);
    return result;
}

TruePeakLimiterFeedbackSnapshot TruePeakLimiterPlan::getFeedbackSnapshot() const noexcept
{
    return feedback != nullptr ? feedback->snapshot() : TruePeakLimiterFeedbackSnapshot {};
}

void TruePeakLimiterPlan::resetFeedback() const noexcept
{
    if (feedback != nullptr)
        feedback->reset();
}

std::unique_ptr<TruePeakLimiterProcessor>
TruePeakLimiterProcessor::CapabilityDescriptor::create() const
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto request = OptionalLifecycleToken::request(currentOptionalLifecycleContext(),
        OptionalLifecycleOwner::truePeakLimiter, OptionalLifecycleResourceKind::processor);
    try
    {
        auto processor = std::unique_ptr<TruePeakLimiterProcessor>(new TruePeakLimiterProcessor(*this));
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
    return std::unique_ptr<TruePeakLimiterProcessor>(
        new TruePeakLimiterProcessor(*this));
#endif
}

TruePeakLimiterProcessor::CapabilityDescriptor
TruePeakLimiterProcessor::describe() noexcept
{
    return {};
}

std::vector<juce::String> TruePeakLimiterProcessor::automationSafeParameterIds()
{
    return {
        "limiter.enabled",
        "limiter.threshold-db",
        "limiter.ceiling-dbtp",
        "limiter.lookahead-ms-fixed",
        "limiter.release-mode",
        "limiter.release-ms",
        "limiter.oversampling-fixed",
        "limiter.channels-linked-fixed"
    };
}

int TruePeakLimiterProcessor::oversamplingFactorForSampleRate(double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 1;
    if (sampleRate <= 96000.0)
        return 4;
    if (sampleRate <= 192000.0)
        return 2;
    return 1;
}

TruePeakLimiterProcessor::AnalyzeResult TruePeakLimiterProcessor::analyze(
    SingleTrackDocument::StatePtr state,
    int64_t documentGeneration,
    const AudioSelectionRequest& selection,
    const TruePeakLimiterParameters& parameters,
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
        result.errorMessage = parameters.enabled
            ? "One or more limiter parameters violate the locked conformance profile."
            : "The limiter is Off.";
        return result;
    }

    const auto factor = oversamplingFactorForSampleRate(state->info.sampleRate);
    const auto lookAhead = juce::jmax(
        1, static_cast<int>(std::ceil(state->info.sampleRate * 0.005)));
    auto feedback = std::make_shared<TruePeakLimiterFeedbackState>();
    AudioSuitePlanBuildRequest request;
    request.state = state;
    request.documentGeneration = documentGeneration;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    request.lifecycleContext = observedContext();
    request.lifecycleOwner = OptionalLifecycleOwner::truePeakLimiter;
    request.processorResourceId = observedResourceId();
#endif
    request.selection = selection;
    request.processorId = descriptor.processorId;
    request.processorVersion = descriptor.processorVersion;
    request.renderFactory = std::make_shared<TruePeakLimiterRenderFactory>(
        parameters,
        state->info.sampleRate,
        factor,
        lookAhead,
        feedback);
    request.maximumPreviewSeconds = maximumPreviewSeconds;
    const auto transactionPlan = AudioSuitePlanBuilder::build(request);
    if (!transactionPlan.succeeded())
    {
        result.errorMessage = transactionPlan.errorMessage;
        return result;
    }

    auto plan = std::make_shared<TruePeakLimiterPlan>();
    plan->audioSuitePlan = transactionPlan.plan;
    plan->parameters = parameters;
    plan->sampleRate = state->info.sampleRate;
    plan->oversamplingFactor = factor;
    plan->lookAheadSamples = lookAhead;
    plan->guardedCeilingDbtp = parameters.ceilingDbtp - internalGuardDb;
    plan->feedback = std::move(feedback);
    result.plan = std::move(plan);
    return result;
}

AudioSuitePreviewHandle TruePeakLimiterProcessor::createPreview(
    const TruePeakLimiterPlanPtr& plan) const
{
    if (plan == nullptr
        || plan->getAudioSuitePlan() == nullptr
        || plan->getAudioSuitePlan()->getProcessorId() != descriptor.processorId
        || plan->getAudioSuitePlan()->getProcessorVersion() != descriptor.processorVersion)
    {
        return {};
    }
    plan->resetFeedback();
    return AudioSuiteTransactionExecutor::createPreview(plan->getAudioSuitePlan());
}

AudioSuiteApplyResult TruePeakLimiterProcessor::apply(
    SingleTrackDocument& document,
    const TruePeakLimiterPlanPtr& plan,
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
    plan->resetFeedback();
    return AudioSuiteTransactionExecutor::apply(document,
                                                plan->getAudioSuitePlan(),
                                                currentDocumentGeneration,
                                                cancellation,
                                                renderBlockSize);
}

bool TruePeakLimiterProcessor::renderPlanForDiagnostics(
    const TruePeakLimiterPlan& plan,
    juce::AudioBuffer<float>& buffer,
    int validOutputSamples,
    int64_t absoluteStartSample,
    juce::Range<int64_t> activeRange)
{
    if (plan.getAudioSuitePlan() == nullptr)
        return false;
    auto session = plan.getAudioSuitePlan()->createRenderSession();
    if (session == nullptr)
        return false;
    session->renderBlockWithLookAheadInPlace(buffer,
                                             validOutputSamples,
                                             absoluteStartSample,
                                             activeRange);
    return true;
}

double TruePeakLimiterProcessor::estimateTruePeakLinearForDiagnostics(
    const float* samples,
    int numSamples,
    int factor) noexcept
{
    if (samples == nullptr || numSamples <= 0)
        return 0.0;
    juce::AudioBuffer<float> buffer(1, numSamples);
    buffer.copyFrom(0, 0, samples, numSamples);
    auto peak = 0.0;
    for (int sample = 0; sample < numSamples; ++sample)
        peak = std::max(peak,
                        oversampledFramePeak(buffer,
                                             0,
                                             sample,
                                             juce::jlimit(1, 4, factor),
                                             samples[0]));
    return peak;
}

int TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() noexcept
{
    return liveInstanceCount.load(std::memory_order_acquire);
}

TruePeakLimiterProcessor::TruePeakLimiterProcessor(CapabilityDescriptor descriptorToUse)
    : descriptor(std::move(descriptorToUse))
{
    liveInstanceCount.fetch_add(1, std::memory_order_acq_rel);
}

TruePeakLimiterProcessor::~TruePeakLimiterProcessor()
{
    liveInstanceCount.fetch_sub(1, std::memory_order_acq_rel);
}
} // namespace singleeditor
