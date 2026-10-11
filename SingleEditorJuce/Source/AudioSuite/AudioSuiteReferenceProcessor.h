#pragma once

#include "Document/SingleTrackDocument.h"
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include "Core/OptionalLifecycleObserver.h"
#endif

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace singleeditor
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
// Declare before real resource members so its terminal follows their actual
// destruction. This non-owning witness is absent from production targets.
struct AudioSuiteObservedRelease final
{
    OptionalLifecycleToken token;
    ~AudioSuiteObservedRelease() { token.release(); }
};
#endif

struct SourceChannelIdentity
{
    int sourceChannelIndex = -1;
    juce::String stableId;

    static SourceChannelIdentity fromSourceIndex(int sourceChannelIndex);
    bool operator==(const SourceChannelIdentity&) const noexcept = default;
};

struct AudioSelectionRequest
{
    std::optional<juce::Range<double>> timeRangeSeconds;
    std::vector<SourceChannelIdentity> sourceChannels;
};

struct ResolvedAudioSelection
{
    juce::Range<int64_t> sampleRange;
    std::vector<SourceChannelIdentity> sourceChannels;
    int sourceChannelCount = 0;
    bool wholeDuration = false;
    bool allSourceChannels = false;

    bool isValid() const noexcept;
    bool containsSourceChannel(int sourceChannelIndex) const noexcept;
};

class AudioSuitePlan;

struct AudioSuiteRenderVerification
{
    bool performed = false;
    bool passed = true;
    double maximumTruePeakDbtp = -std::numeric_limits<double>::infinity();
    int64_t clippedSamples = 0;
    int64_t nonFiniteSamples = 0;
    int verifiedChannels = 0;
    int64_t verifiedSamples = 0;
    double sampleRate = 0.0;
    juce::String failureMessage;
};

class AudioSuiteRenderSession
{
public:
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    virtual ~AudioSuiteRenderSession() { lifecycleToken.release(); }
    std::uint64_t observedResourceId() const noexcept { return lifecycleToken.resourceId(); }
#else
    virtual ~AudioSuiteRenderSession() = default;
#endif
    virtual void reset() noexcept = 0;
    virtual int getRequiredLookAheadSamples() const noexcept { return 0; }
    virtual void renderBlockInPlace(juce::AudioBuffer<float>& buffer,
                                    int64_t absoluteStartSample,
                                    juce::Range<int64_t> activeRange) noexcept = 0;
    virtual void renderBlockWithLookAheadInPlace(juce::AudioBuffer<float>& buffer,
                                                 int validOutputSamples,
                                                 int64_t absoluteStartSample,
                                                 juce::Range<int64_t> activeRange) noexcept
    {
        validOutputSamples = juce::jlimit(0, buffer.getNumSamples(), validOutputSamples);
        juce::AudioBuffer<float> outputView(buffer.getArrayOfWritePointers(),
                                            buffer.getNumChannels(),
                                            validOutputSamples);
        renderBlockInPlace(outputView, absoluteStartSample, activeRange);
    }
    virtual bool requiresPostRenderVerification() const noexcept { return false; }
    virtual int getPostRenderVerificationLookAheadSamples() const noexcept { return 0; }
    virtual void beginPostRenderVerification(int,
                                             int64_t,
                                             double) noexcept {}
    virtual bool verifyRenderedBlock(const juce::AudioBuffer<float>&,
                                     int,
                                     int64_t,
                                     juce::Range<int64_t>) noexcept
    {
        return true;
    }
    virtual AudioSuiteRenderVerification finishPostRenderVerification() noexcept
    {
        return {};
    }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
private:
    friend class AudioSuitePlan;
    OptionalLifecycleToken lifecycleToken;
#endif
};

class AudioSuiteRenderFactory
{
public:
    virtual ~AudioSuiteRenderFactory() = default;
    virtual std::unique_ptr<AudioSuiteRenderSession> createRenderSession(
        const AudioSuitePlan& plan) const = 0;
};

class AudioSuitePlan final
{
public:
    const juce::String& getProcessorId() const noexcept { return processorId; }
    const juce::String& getProcessorVersion() const noexcept { return processorVersion; }
    int64_t getDocumentGeneration() const noexcept { return documentGeneration; }
    int64_t getAudioRevision() const noexcept { return audioRevision; }
    int64_t getChangeGeneration() const noexcept { return changeGeneration; }
    const SingleTrackDocument::SourceFileIdentity& getSourceIdentity() const noexcept { return sourceIdentity; }
    const ResolvedAudioSelection& getSelection() const noexcept { return selection; }
    juce::Range<int64_t> getAnalysisReadRange() const noexcept { return analysisReadRange; }
    juce::Range<int64_t> getPreviewRange() const noexcept { return previewRange; }
    int getHiddenPreRollSamples() const noexcept { return hiddenPreRollSamples; }
    int getBoundaryCrossfadeSamples() const noexcept { return boundaryCrossfadeSamples; }
    float getLinearGain() const noexcept { return linearGain; }
    std::unique_ptr<AudioSuiteRenderSession> createRenderSession() const;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext observedContext() const noexcept { return lifecycleContext; }
    OptionalLifecycleOwner observedOwner() const noexcept { return lifecycleOwner; }
#endif

    bool matchesState(SingleTrackDocument::StatePtr state,
                      int64_t currentDocumentGeneration,
                      bool verifySourceFile) const;

private:
    friend class AudioSuiteReferenceProcessor;
    friend class AudioSuitePlanBuilder;

    juce::String processorId;
    juce::String processorVersion;
    int64_t documentGeneration = 0;
    int64_t audioRevision = 0;
    int64_t changeGeneration = 0;
    SingleTrackDocument::SourceFileIdentity sourceIdentity;
    ResolvedAudioSelection selection;
    juce::Range<int64_t> analysisReadRange;
    juce::Range<int64_t> previewRange;
    int hiddenPreRollSamples = 0;
    int boundaryCrossfadeSamples = 0;
    float linearGain = 1.0f;
    std::shared_ptr<const AudioSuiteRenderFactory> renderFactory;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext lifecycleContext;
    OptionalLifecycleOwner lifecycleOwner = OptionalLifecycleOwner::referenceGain;
    std::uint64_t processorResourceId = 0;
#endif
};

using AudioSuitePlanPtr = std::shared_ptr<const AudioSuitePlan>;

struct AudioSuitePlanBuildRequest
{
    SingleTrackDocument::StatePtr state;
    int64_t documentGeneration = 0;
    AudioSelectionRequest selection;
    juce::String processorId;
    juce::String processorVersion;
    std::shared_ptr<const AudioSuiteRenderFactory> renderFactory;
    float referenceLinearGain = 1.0f;
    double maximumPreviewSeconds = 30.0;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext lifecycleContext;
    OptionalLifecycleOwner lifecycleOwner = OptionalLifecycleOwner::referenceGain;
    std::uint64_t processorResourceId = 0;
#endif
};

struct AudioSuitePlanBuildResult
{
    AudioSuitePlanPtr plan;
    juce::String errorMessage;

    bool succeeded() const noexcept { return plan != nullptr; }
};

class AudioSuitePlanBuilder final
{
public:
    static AudioSuitePlanBuildResult build(const AudioSuitePlanBuildRequest& request);
};

class AudioSuiteCancellationToken final
{
public:
    AudioSuiteCancellationToken();

    void cancel() const noexcept;
    bool isCancelled() const noexcept;

private:
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct AudioSuitePreviewState
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    AudioSuiteObservedRelease lifecycleRelease;
    std::uint64_t observedResourceId() const noexcept { return lifecycleRelease.token.resourceId(); }
#endif
    AudioSuitePlanPtr plan;
    AudioSuiteCancellationToken cancellation;
    std::shared_ptr<AudioSuiteRenderSession> renderSession;

    bool isActive() const noexcept;
    void resetRenderSession() const noexcept;
    int getRequiredLookAheadSamples() const noexcept;
    bool renderBlockInPlace(juce::AudioBuffer<float>& buffer,
                            int64_t absoluteStartSample,
                            juce::Range<int64_t> activeRange) const noexcept;
    bool renderBlockWithLookAheadInPlace(juce::AudioBuffer<float>& buffer,
                                         int validOutputSamples,
                                         int64_t absoluteStartSample,
                                         juce::Range<int64_t> activeRange) const noexcept;
};

class AudioSuitePreviewHandle final
{
public:
    AudioSuitePreviewHandle() = default;
    explicit AudioSuitePreviewHandle(std::shared_ptr<const AudioSuitePreviewState> previewState);

    bool isValid() const noexcept;
    void cancel() const noexcept;
    juce::Range<int64_t> getPlaybackRange() const noexcept;
    std::shared_ptr<const AudioSuitePreviewState> getState() const noexcept;

private:
    std::shared_ptr<const AudioSuitePreviewState> state;
};

enum class AudioSuiteApplyStatus
{
    applied,
    cancelled,
    stalePlan,
    invalidPlan,
    transactionFailed,
    verificationFailed
};

struct AudioSuiteApplyResult
{
    AudioSuiteApplyStatus status = AudioSuiteApplyStatus::invalidPlan;
    int64_t renderedSamples = 0;
    int renderedBlocks = 0;
    AudioSuiteRenderVerification verification;

    bool succeeded() const noexcept { return status == AudioSuiteApplyStatus::applied; }
};

class AudioSuiteTransactionExecutor final
{
public:
    static AudioSuitePreviewHandle createPreview(const AudioSuitePlanPtr& plan);
    static AudioSuiteApplyResult apply(SingleTrackDocument& document,
                                       const AudioSuitePlanPtr& plan,
                                       int64_t currentDocumentGeneration,
                                       const AudioSuiteCancellationToken& cancellation = {},
                                       int renderBlockSize = 4096);
};

class AudioSuiteReferenceProcessor final
{
public:
    struct CapabilityDescriptor
    {
        juce::String processorId = "singleeditor.reference-gain";
        juce::String processorVersion = "1.0.0";
        juce::String displayName = "Reference Gain";

        std::unique_ptr<AudioSuiteReferenceProcessor> create() const;
    };

    using AnalyzeResult = AudioSuitePlanBuildResult;
    using ApplyStatus = AudioSuiteApplyStatus;
    using ApplyResult = AudioSuiteApplyResult;

    static CapabilityDescriptor describe() noexcept;
    static ResolvedAudioSelection resolveSelection(const AudioSelectionRequest& request,
                                                   const SingleTrackDocument::State& state);

    AnalyzeResult analyze(SingleTrackDocument::StatePtr state,
                          int64_t documentGeneration,
                          const AudioSelectionRequest& selection,
                          float linearGain,
                          double maximumPreviewSeconds = 30.0) const;
    AudioSuitePreviewHandle createPreview(const AudioSuitePlanPtr& plan) const;
    ApplyResult apply(SingleTrackDocument& document,
                      const AudioSuitePlanPtr& plan,
                      int64_t currentDocumentGeneration,
                      const AudioSuiteCancellationToken& cancellation = {},
                      int renderBlockSize = 4096) const;

    static void renderBlockInPlace(const AudioSuitePlan& plan,
                                   juce::AudioBuffer<float>& buffer,
                                   int64_t absoluteStartSample,
                                   juce::Range<int64_t> activeRange);
    static float gainForSample(const AudioSuitePlan& plan,
                               int sourceChannelIndex,
                               int64_t absoluteSample,
                               juce::Range<int64_t> activeRange) noexcept;

    static int getLiveInstanceCountForDiagnostics() noexcept;

    ~AudioSuiteReferenceProcessor();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::uint64_t observedResourceId() const noexcept { return lifecycleRelease.token.resourceId(); }
    OptionalLifecycleContext observedContext() const noexcept { return lifecycleRelease.token.creationContext(); }
#endif

private:
    explicit AudioSuiteReferenceProcessor(CapabilityDescriptor descriptorToUse);

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    AudioSuiteObservedRelease lifecycleRelease;
#endif
    CapabilityDescriptor descriptor;
    static std::atomic<int> liveInstanceCount;
};
} // namespace singleeditor
