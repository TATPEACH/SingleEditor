#pragma once

#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "Core/OptionalLifecycleObserver.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace singleeditor
{
enum class ParametricEqFilterType
{
    highPass,
    lowShelf,
    bell,
    notch,
    highShelf,
    lowPass
};

enum class ParametricEqSlope
{
    db12,
    db24
};

struct ParametricEqBandParameters
{
    bool enabled = false;
    ParametricEqFilterType type = ParametricEqFilterType::bell;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.7071067811865476;
    ParametricEqSlope slope = ParametricEqSlope::db12;

    bool operator==(const ParametricEqBandParameters&) const noexcept = default;
};

struct ParametricEqParameters
{
    static constexpr std::size_t bandCount = 8;

    std::array<ParametricEqBandParameters, bandCount> bands;
    juce::String presetId = "flat";

    static ParametricEqParameters flat();
    bool operator==(const ParametricEqParameters&) const noexcept = default;
};

struct ParametricEqBiquadCoefficients
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    bool isFinite() const noexcept;
    double magnitudeAt(double frequencyHz, double sampleRate) const noexcept;
};

struct ParametricEqResolvedBand
{
    ParametricEqBandParameters requested;
    double effectiveFrequencyHz = 1000.0;
    double effectiveGainDb = 0.0;
    double effectiveQ = 0.7071067811865476;
    int stageCount = 0;
    std::array<ParametricEqBiquadCoefficients, 2> stages;
};

class ParametricEqPlan final
{
public:
    const AudioSuitePlanPtr& getAudioSuitePlan() const noexcept { return audioSuitePlan; }
    const ParametricEqParameters& getParameters() const noexcept { return parameters; }
    const std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount>&
        getResolvedBands() const noexcept { return resolvedBands; }
    double getSampleRate() const noexcept { return sampleRate; }
    double magnitudeAt(double frequencyHz) const noexcept;

private:
    friend class ParametricEqProcessor;

    AudioSuitePlanPtr audioSuitePlan;
    ParametricEqParameters parameters;
    std::array<ParametricEqResolvedBand, ParametricEqParameters::bandCount> resolvedBands;
    double sampleRate = 0.0;
};

using ParametricEqPlanPtr = std::shared_ptr<const ParametricEqPlan>;

class ParametricEqProcessor final
{
public:
    struct CapabilityDescriptor
    {
        juce::String processorId = "singleeditor.parametric-eq-v1";
        juce::String processorVersion = "1.0.0";
        juce::String displayName = "Parametric EQ";

        std::unique_ptr<ParametricEqProcessor> create() const;
    };

    struct Preset
    {
        juce::String stableId;
        juce::String displayName;
        ParametricEqParameters parameters;
    };

    struct AnalyzeResult
    {
        ParametricEqPlanPtr plan;
        juce::String errorMessage;

        bool succeeded() const noexcept { return plan != nullptr; }
    };

    static CapabilityDescriptor describe() noexcept;
    static std::vector<juce::String> automationSafeParameterIds();
    static std::vector<Preset> builtInPresets();
    static std::optional<ParametricEqParameters> parametersForPreset(
        const juce::String& stableId);
    // Pure coefficient visualization. Does not create a processor, read audio,
    // build an edit transaction or start preview playback.
    static std::vector<float> magnitudeResponse(const ParametricEqParameters& parameters,
                                               double sampleRate, double minimumHz,
                                               double maximumHz, int pointCount = 512);

    AnalyzeResult analyze(SingleTrackDocument::StatePtr state,
                          int64_t documentGeneration,
                          const AudioSelectionRequest& selection,
                          const ParametricEqParameters& parameters,
                          double maximumPreviewSeconds = 30.0) const;
    AudioSuitePreviewHandle createPreview(const ParametricEqPlanPtr& plan) const;
    AudioSuiteApplyResult apply(SingleTrackDocument& document,
                                const ParametricEqPlanPtr& plan,
                                int64_t currentDocumentGeneration,
                                const AudioSuiteCancellationToken& cancellation = {},
                                int renderBlockSize = 4096) const;

    static bool renderPlanForDiagnostics(const ParametricEqPlan& plan,
                                         juce::AudioBuffer<float>& buffer,
                                         int64_t absoluteStartSample,
                                         juce::Range<int64_t> activeRange);
    static int getLiveInstanceCountForDiagnostics() noexcept;

    ~ParametricEqProcessor();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::uint64_t observedResourceId() const noexcept { return lifecycleRelease.token.resourceId(); }
    OptionalLifecycleContext observedContext() const noexcept { return lifecycleRelease.token.creationContext(); }
#endif

private:
    explicit ParametricEqProcessor(CapabilityDescriptor descriptorToUse);

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    AudioSuiteObservedRelease lifecycleRelease;
#endif
    CapabilityDescriptor descriptor;
    static std::atomic<int> liveInstanceCount;
};
} // namespace singleeditor
