#pragma once

#include "AudioSuite/AudioSuiteReferenceProcessor.h"

#include <atomic>
#include <memory>
#include <vector>

namespace singleeditor
{
enum class TruePeakLimiterReleaseMode
{
    manual,
    automatic
};

enum class TruePeakLimiterOversampling
{
    sampleRateAwareFixed
};

struct TruePeakLimiterParameters
{
    bool enabled = false;
    double thresholdDb = -6.0;
    double ceilingDbtp = -1.0;
    double lookAheadMs = 5.0;
    TruePeakLimiterReleaseMode releaseMode = TruePeakLimiterReleaseMode::automatic;
    double releaseMs = 120.0;
    TruePeakLimiterOversampling oversampling =
        TruePeakLimiterOversampling::sampleRateAwareFixed;
    bool channelsLinked = true;

    static TruePeakLimiterParameters masteringDefault();
    bool operator==(const TruePeakLimiterParameters&) const noexcept = default;
};

struct TruePeakLimiterFeedbackSnapshot
{
    double inputTruePeakDbtp = -std::numeric_limits<double>::infinity();
    double maximumGainReductionDb = 0.0;
    int64_t processedSamples = 0;
};

class TruePeakLimiterFeedbackState final
{
public:
    void reset() noexcept;
    void observe(double inputTruePeakLinear,
                 double gainReductionDb,
                 int64_t sampleCount) noexcept;
    TruePeakLimiterFeedbackSnapshot snapshot() const noexcept;

private:
    std::atomic<double> maximumInputTruePeakLinear { 0.0 };
    std::atomic<double> maximumGainReductionDb { 0.0 };
    std::atomic<int64_t> processedSamples { 0 };
};

class TruePeakLimiterPlan final
{
public:
    const AudioSuitePlanPtr& getAudioSuitePlan() const noexcept { return audioSuitePlan; }
    const TruePeakLimiterParameters& getParameters() const noexcept { return parameters; }
    double getSampleRate() const noexcept { return sampleRate; }
    int getOversamplingFactor() const noexcept { return oversamplingFactor; }
    int getLookAheadSamples() const noexcept { return lookAheadSamples; }
    double getGuardedCeilingDbtp() const noexcept { return guardedCeilingDbtp; }
    TruePeakLimiterFeedbackSnapshot getFeedbackSnapshot() const noexcept;
    void resetFeedback() const noexcept;

private:
    friend class TruePeakLimiterProcessor;

    AudioSuitePlanPtr audioSuitePlan;
    TruePeakLimiterParameters parameters;
    double sampleRate = 0.0;
    int oversamplingFactor = 1;
    int lookAheadSamples = 0;
    double guardedCeilingDbtp = -1.1;
    std::shared_ptr<TruePeakLimiterFeedbackState> feedback;
};

using TruePeakLimiterPlanPtr = std::shared_ptr<const TruePeakLimiterPlan>;

class TruePeakLimiterProcessor final
{
public:
    struct CapabilityDescriptor
    {
        juce::String processorId = "singleeditor.true-peak-limiter-v1";
        juce::String processorVersion = "1.0.0";
        juce::String displayName = "True-Peak Limiter";

        std::unique_ptr<TruePeakLimiterProcessor> create() const;
    };

    struct AnalyzeResult
    {
        TruePeakLimiterPlanPtr plan;
        juce::String errorMessage;

        bool succeeded() const noexcept { return plan != nullptr; }
    };

    static CapabilityDescriptor describe() noexcept;
    static std::vector<juce::String> automationSafeParameterIds();
    static int oversamplingFactorForSampleRate(double sampleRate) noexcept;

    AnalyzeResult analyze(SingleTrackDocument::StatePtr state,
                          int64_t documentGeneration,
                          const AudioSelectionRequest& selection,
                          const TruePeakLimiterParameters& parameters,
                          double maximumPreviewSeconds = 30.0) const;
    AudioSuitePreviewHandle createPreview(const TruePeakLimiterPlanPtr& plan) const;
    AudioSuiteApplyResult apply(SingleTrackDocument& document,
                                const TruePeakLimiterPlanPtr& plan,
                                int64_t currentDocumentGeneration,
                                const AudioSuiteCancellationToken& cancellation = {},
                                int renderBlockSize = 4096) const;

    static bool renderPlanForDiagnostics(const TruePeakLimiterPlan& plan,
                                         juce::AudioBuffer<float>& buffer,
                                         int validOutputSamples,
                                         int64_t absoluteStartSample,
                                         juce::Range<int64_t> activeRange);
    static double estimateTruePeakLinearForDiagnostics(const float* samples,
                                                       int numSamples,
                                                       int oversamplingFactor) noexcept;
    static int getLiveInstanceCountForDiagnostics() noexcept;

    ~TruePeakLimiterProcessor();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::uint64_t observedResourceId() const noexcept { return lifecycleRelease.token.resourceId(); }
    OptionalLifecycleContext observedContext() const noexcept { return lifecycleRelease.token.creationContext(); }
#endif

private:
    explicit TruePeakLimiterProcessor(CapabilityDescriptor descriptorToUse);

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    AudioSuiteObservedRelease lifecycleRelease;
#endif
    CapabilityDescriptor descriptor;
    static std::atomic<int> liveInstanceCount;
};
} // namespace singleeditor
