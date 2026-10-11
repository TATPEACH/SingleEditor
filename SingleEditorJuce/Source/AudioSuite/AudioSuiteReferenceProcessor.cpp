#include "AudioSuite/AudioSuiteReferenceProcessor.h"

#include <algorithm>
#include <cmath>

namespace singleeditor
{
std::atomic<int> AudioSuiteReferenceProcessor::liveInstanceCount { 0 };

namespace
{
float referenceGainForSample(const ResolvedAudioSelection& selection,
                             float linearGain,
                             int boundaryCrossfadeSamples,
                             int sourceChannelIndex,
                             int64_t absoluteSample,
                             juce::Range<int64_t> activeRange) noexcept
{
    const auto effectiveRange = selection.sampleRange.getIntersectionWith(activeRange);
    if (!effectiveRange.contains(absoluteSample)
        || !selection.containsSourceChannel(sourceChannelIndex))
    {
        return 1.0f;
    }

    const auto fadeSamples = juce::jmin<int64_t>(
        boundaryCrossfadeSamples, selection.sampleRange.getLength() / 2);
    if (fadeSamples <= 0)
        return linearGain;

    const auto fromStart = absoluteSample - selection.sampleRange.getStart();
    const auto fromEnd = selection.sampleRange.getEnd() - 1 - absoluteSample;
    const auto edgeDistance = juce::jmin(fromStart, fromEnd);
    const auto wet = juce::jlimit(0.0f,
                                  1.0f,
                                  static_cast<float>(edgeDistance)
                                      / static_cast<float>(fadeSamples));
    return 1.0f + (linearGain - 1.0f) * wet;
}

class ReferenceGainRenderSession final : public AudioSuiteRenderSession
{
public:
    explicit ReferenceGainRenderSession(const AudioSuitePlan& plan)
        : selection(plan.getSelection()),
          linearGain(plan.getLinearGain()),
          boundaryCrossfadeSamples(plan.getBoundaryCrossfadeSamples())
    {
    }

    void reset() noexcept override {}

    void renderBlockInPlace(juce::AudioBuffer<float>& buffer,
                            int64_t absoluteStartSample,
                            juce::Range<int64_t> activeRange) noexcept override
    {
        const auto channels = juce::jmin(buffer.getNumChannels(),
                                         selection.sourceChannelCount);
        for (int channel = 0; channel < channels; ++channel)
        {
            if (!selection.containsSourceChannel(channel))
                continue;

            auto* samples = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                samples[sample] *= referenceGainForSample(selection,
                                                          linearGain,
                                                          boundaryCrossfadeSamples,
                                                          channel,
                                                          absoluteStartSample + sample,
                                                          activeRange);
            }
        }
    }

private:
    ResolvedAudioSelection selection;
    float linearGain = 1.0f;
    int boundaryCrossfadeSamples = 0;
};

class ReferenceGainRenderFactory final : public AudioSuiteRenderFactory
{
public:
    std::unique_ptr<AudioSuiteRenderSession> createRenderSession(
        const AudioSuitePlan& plan) const override
    {
        return std::make_unique<ReferenceGainRenderSession>(plan);
    }
};
} // namespace

SourceChannelIdentity SourceChannelIdentity::fromSourceIndex(int sourceChannelIndex)
{
    return { sourceChannelIndex, "source-channel:" + juce::String(sourceChannelIndex) };
}

bool ResolvedAudioSelection::isValid() const noexcept
{
    return !sampleRange.isEmpty()
        && sourceChannelCount > 0
        && !sourceChannels.empty();
}

bool ResolvedAudioSelection::containsSourceChannel(int sourceChannelIndex) const noexcept
{
    return std::any_of(sourceChannels.begin(), sourceChannels.end(),
                       [sourceChannelIndex](const auto& identity)
                       {
                           return identity.sourceChannelIndex == sourceChannelIndex;
                       });
}

bool AudioSuitePlan::matchesState(SingleTrackDocument::StatePtr state,
                                  int64_t currentDocumentGeneration,
                                  bool verifySourceFile) const
{
    if (state == nullptr
        || !state->info.isValid()
        || documentGeneration <= 0
        || currentDocumentGeneration != documentGeneration
        || state->audioRevision != audioRevision
        || state->changeGeneration != changeGeneration
        || state->info.file != sourceIdentity.file
        || state->info.numSamples < selection.sampleRange.getEnd()
        || state->info.numChannels != selection.sourceChannelCount)
    {
        return false;
    }

    if (!verifySourceFile)
        return sourceIdentity.valid;

    return sourceIdentity.matchesCurrentFile();
}

std::unique_ptr<AudioSuiteRenderSession> AudioSuitePlan::createRenderSession() const
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (renderFactory == nullptr) return nullptr;
    auto request = OptionalLifecycleToken::request(lifecycleContext, lifecycleOwner,
        OptionalLifecycleResourceKind::renderSession, processorResourceId);
    try
    {
        auto session = renderFactory->createRenderSession(*this);
        if (session == nullptr) { request.failed(); return nullptr; }
        request.created();
        session->lifecycleToken = std::move(request);
        return session;
    }
    catch (...)
    {
        request.failed();
        throw;
    }
#else
    return renderFactory != nullptr ? renderFactory->createRenderSession(*this) : nullptr;
#endif
}

AudioSuitePlanBuildResult AudioSuitePlanBuilder::build(
    const AudioSuitePlanBuildRequest& request)
{
    AudioSuitePlanBuildResult result;
    if (request.state == nullptr || !request.state->info.isValid())
    {
        result.errorMessage = "No valid source audio is available.";
        return result;
    }
    if (request.documentGeneration <= 0)
    {
        result.errorMessage = "The document generation is invalid.";
        return result;
    }
    if (request.processorId.isEmpty()
        || request.processorVersion.isEmpty()
        || request.renderFactory == nullptr)
    {
        result.errorMessage = "The AudioSuite processor contract is incomplete.";
        return result;
    }
    if (!std::isfinite(request.referenceLinearGain)
        || request.referenceLinearGain < 0.0f
        || request.referenceLinearGain > 16.0f)
    {
        result.errorMessage = "The reference gain is outside the supported range.";
        return result;
    }
    if (!std::isfinite(request.maximumPreviewSeconds)
        || request.maximumPreviewSeconds <= 0.0)
    {
        result.errorMessage = "The preview duration limit is invalid.";
        return result;
    }

    auto resolved = AudioSuiteReferenceProcessor::resolveSelection(request.selection,
                                                                    *request.state);
    if (!resolved.isValid())
    {
        result.errorMessage = "The time-by-channel selection is invalid.";
        return result;
    }

    const auto sourceIdentity = SingleTrackDocument::SourceFileIdentity::capture(
        request.state->info.file);
    if (!sourceIdentity.valid)
    {
        result.errorMessage = "The source identity could not be captured.";
        return result;
    }

    auto plan = std::make_shared<AudioSuitePlan>();
    plan->processorId = request.processorId;
    plan->processorVersion = request.processorVersion;
    plan->documentGeneration = request.documentGeneration;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    plan->lifecycleContext = request.lifecycleContext;
    // The explicit document generation is independent of Undo's change counter.
    plan->lifecycleContext.generation = static_cast<std::uint64_t>(request.documentGeneration);
    plan->lifecycleOwner = request.lifecycleOwner;
    plan->processorResourceId = request.processorResourceId;
#endif
    plan->audioRevision = request.state->audioRevision;
    plan->changeGeneration = request.state->changeGeneration;
    plan->sourceIdentity = sourceIdentity;
    plan->selection = std::move(resolved);
    plan->linearGain = request.referenceLinearGain;
    plan->renderFactory = request.renderFactory;
    plan->hiddenPreRollSamples = juce::jlimit(
        0, 2048, static_cast<int>(std::ceil(request.state->info.sampleRate * 0.01)));
    plan->boundaryCrossfadeSamples = juce::jlimit(
        0, 256, static_cast<int>(std::ceil(request.state->info.sampleRate * 0.005)));
    plan->analysisReadRange = {
        juce::jmax<int64_t>(0,
                            plan->selection.sampleRange.getStart()
                                - plan->hiddenPreRollSamples),
        juce::jmin<int64_t>(request.state->info.numSamples,
                            plan->selection.sampleRange.getEnd()
                                + plan->boundaryCrossfadeSamples)
    };

    const auto maximumPreviewSamples = juce::jmax<int64_t>(
        1, static_cast<int64_t>(std::floor(request.maximumPreviewSeconds
                                          * request.state->info.sampleRate)));
    plan->previewRange = {
        plan->selection.sampleRange.getStart(),
        juce::jmin(plan->selection.sampleRange.getEnd(),
                   plan->selection.sampleRange.getStart() + maximumPreviewSamples)
    };
    result.plan = std::move(plan);
    return result;
}

AudioSuiteCancellationToken::AudioSuiteCancellationToken()
    : cancelled(std::make_shared<std::atomic<bool>>(false))
{
}

void AudioSuiteCancellationToken::cancel() const noexcept
{
    cancelled->store(true, std::memory_order_release);
}

bool AudioSuiteCancellationToken::isCancelled() const noexcept
{
    return cancelled->load(std::memory_order_acquire);
}

bool AudioSuitePreviewState::isActive() const noexcept
{
    return plan != nullptr
        && renderSession != nullptr
        && !cancellation.isCancelled();
}

void AudioSuitePreviewState::resetRenderSession() const noexcept
{
    if (renderSession != nullptr)
        renderSession->reset();
}

int AudioSuitePreviewState::getRequiredLookAheadSamples() const noexcept
{
    return isActive()
         ? juce::jlimit(0, 65536, renderSession->getRequiredLookAheadSamples())
         : 0;
}

bool AudioSuitePreviewState::renderBlockInPlace(
    juce::AudioBuffer<float>& buffer,
    int64_t absoluteStartSample,
    juce::Range<int64_t> activeRange) const noexcept
{
    if (!isActive())
        return false;

    renderSession->renderBlockInPlace(buffer, absoluteStartSample, activeRange);
    return true;
}

bool AudioSuitePreviewState::renderBlockWithLookAheadInPlace(
    juce::AudioBuffer<float>& buffer,
    int validOutputSamples,
    int64_t absoluteStartSample,
    juce::Range<int64_t> activeRange) const noexcept
{
    if (!isActive())
        return false;

    renderSession->renderBlockWithLookAheadInPlace(buffer,
                                                   validOutputSamples,
                                                   absoluteStartSample,
                                                   activeRange);
    return true;
}

AudioSuitePreviewHandle::AudioSuitePreviewHandle(
    std::shared_ptr<const AudioSuitePreviewState> previewState)
    : state(std::move(previewState))
{
}

bool AudioSuitePreviewHandle::isValid() const noexcept
{
    return state != nullptr && state->plan != nullptr;
}

void AudioSuitePreviewHandle::cancel() const noexcept
{
    if (state != nullptr)
        state->cancellation.cancel();
}

juce::Range<int64_t> AudioSuitePreviewHandle::getPlaybackRange() const noexcept
{
    return isValid() ? state->plan->getPreviewRange() : juce::Range<int64_t> {};
}

std::shared_ptr<const AudioSuitePreviewState> AudioSuitePreviewHandle::getState() const noexcept
{
    return state;
}

AudioSuitePreviewHandle AudioSuiteTransactionExecutor::createPreview(
    const AudioSuitePlanPtr& plan)
{
    if (plan == nullptr
        || !plan->getSelection().isValid()
        || plan->getPreviewRange().isEmpty())
    {
        return {};
    }

    auto session = plan->createRenderSession();
    if (session == nullptr)
        return {};

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto request = OptionalLifecycleToken::request(plan->observedContext(), plan->observedOwner(),
        OptionalLifecycleResourceKind::previewState, session->observedResourceId());
    try
    {
#endif
    auto state = std::make_shared<AudioSuitePreviewState>();
    state->plan = plan;
    state->renderSession = std::shared_ptr<AudioSuiteRenderSession>(std::move(session));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    request.created();
    state->lifecycleRelease.token = std::move(request);
#endif
    return AudioSuitePreviewHandle(std::move(state));
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    }
    catch (...)
    {
        request.failed();
        throw;
    }
#endif
}

AudioSuiteApplyResult AudioSuiteTransactionExecutor::apply(
    SingleTrackDocument& document,
    const AudioSuitePlanPtr& plan,
    int64_t currentDocumentGeneration,
    const AudioSuiteCancellationToken& cancellation,
    int renderBlockSize)
{
    AudioSuiteApplyResult result;
    if (plan == nullptr || !plan->getSelection().isValid())
        return result;

    const auto currentState = document.getState();
    if (!plan->matchesState(currentState, currentDocumentGeneration, true))
    {
        result.status = AudioSuiteApplyStatus::stalePlan;
        return result;
    }
    if (cancellation.isCancelled())
    {
        result.status = AudioSuiteApplyStatus::cancelled;
        return result;
    }

    auto renderSession = plan->createRenderSession();
    if (renderSession == nullptr)
        return result;

    auto transaction = document.beginEqualLengthAudioMutation(currentState);
    if (transaction == nullptr || !transaction->isValid())
    {
        result.status = AudioSuiteApplyStatus::transactionFailed;
        return result;
    }

    renderBlockSize = juce::jlimit(64, 65536, renderBlockSize);
    const auto renderLookAhead = juce::jlimit(
        0, 65536, renderSession->getRequiredLookAheadSamples());
    juce::AudioBuffer<float> block;
    for (int64_t start = 0; start < transaction->getNumSamples(); start += renderBlockSize)
    {
        if (cancellation.isCancelled())
        {
            result.status = AudioSuiteApplyStatus::cancelled;
            return result;
        }

        const auto count = static_cast<int>(juce::jmin<int64_t>(
            renderBlockSize, transaction->getNumSamples() - start));
        const auto availableTail = transaction->getNumSamples() - start - count;
        const auto readCount = count + static_cast<int>(juce::jmin<int64_t>(
            renderLookAhead, availableTail));
        if (!transaction->readSourceBlock(start, readCount, block))
        {
            result.status = AudioSuiteApplyStatus::transactionFailed;
            return result;
        }

        renderSession->renderBlockWithLookAheadInPlace(
            block,
            count,
            start,
            plan->getSelection().sampleRange);
        if (!transaction->writeRenderedBlock(start, block, 0, count))
        {
            result.status = AudioSuiteApplyStatus::transactionFailed;
            return result;
        }
        result.renderedSamples += count;
        ++result.renderedBlocks;
    }

    if (cancellation.isCancelled())
    {
        result.status = AudioSuiteApplyStatus::cancelled;
        return result;
    }

    if (renderSession->requiresPostRenderVerification())
    {
        renderSession->beginPostRenderVerification(transaction->getNumChannels(),
                                                   transaction->getNumSamples(),
                                                   transaction->getSampleRate());
        const auto verificationLookAhead = juce::jlimit(
            0, 64, renderSession->getPostRenderVerificationLookAheadSamples());
        for (int64_t start = 0;
             start < transaction->getNumSamples();
             start += renderBlockSize)
        {
            if (cancellation.isCancelled())
            {
                result.status = AudioSuiteApplyStatus::cancelled;
                return result;
            }

            const auto count = static_cast<int>(juce::jmin<int64_t>(
                renderBlockSize, transaction->getNumSamples() - start));
            const auto availableTail = transaction->getNumSamples() - start - count;
            const auto readCount = count + static_cast<int>(juce::jmin<int64_t>(
                verificationLookAhead, availableTail));
            if (!transaction->readRenderedBlock(start, readCount, block))
            {
                result.status = AudioSuiteApplyStatus::transactionFailed;
                return result;
            }
            if (!renderSession->verifyRenderedBlock(
                    block,
                    count,
                    start,
                    plan->getSelection().sampleRange))
            {
                result.verification = renderSession->finishPostRenderVerification();
                result.status = AudioSuiteApplyStatus::verificationFailed;
                return result;
            }
        }

        result.verification = renderSession->finishPostRenderVerification();
        if (!result.verification.passed)
        {
            result.status = AudioSuiteApplyStatus::verificationFailed;
            return result;
        }
    }
    if (!plan->matchesState(document.getState(), currentDocumentGeneration, true))
    {
        result.status = AudioSuiteApplyStatus::stalePlan;
        return result;
    }
    if (!document.commitEqualLengthAudioMutation(std::move(transaction)))
    {
        result.status = AudioSuiteApplyStatus::transactionFailed;
        return result;
    }

    result.status = AudioSuiteApplyStatus::applied;
    return result;
}

std::unique_ptr<AudioSuiteReferenceProcessor>
AudioSuiteReferenceProcessor::CapabilityDescriptor::create() const
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto request = OptionalLifecycleToken::request(currentOptionalLifecycleContext(),
        OptionalLifecycleOwner::referenceGain, OptionalLifecycleResourceKind::processor);
    try
    {
        auto processor = std::unique_ptr<AudioSuiteReferenceProcessor>(new AudioSuiteReferenceProcessor(*this));
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
    return std::unique_ptr<AudioSuiteReferenceProcessor>(
        new AudioSuiteReferenceProcessor(*this));
#endif
}

AudioSuiteReferenceProcessor::CapabilityDescriptor
AudioSuiteReferenceProcessor::describe() noexcept
{
    return {};
}

ResolvedAudioSelection AudioSuiteReferenceProcessor::resolveSelection(
    const AudioSelectionRequest& request,
    const SingleTrackDocument::State& state)
{
    ResolvedAudioSelection resolved;
    if (!state.info.isValid()
        || state.info.sampleRate <= 0.0
        || state.info.numSamples <= 0
        || state.info.numChannels <= 0)
    {
        return resolved;
    }

    resolved.sourceChannelCount = state.info.numChannels;

    if (request.timeRangeSeconds.has_value())
    {
        const auto startSeconds = juce::jlimit(0.0,
                                                state.info.durationSeconds,
                                                request.timeRangeSeconds->getStart());
        const auto endSeconds = juce::jlimit(0.0,
                                              state.info.durationSeconds,
                                              request.timeRangeSeconds->getEnd());
        const auto startSample = juce::jlimit<int64_t>(
            0, state.info.numSamples,
            static_cast<int64_t>(std::floor(juce::jmin(startSeconds, endSeconds)
                                            * state.info.sampleRate)));
        const auto endSample = juce::jlimit<int64_t>(
            0, state.info.numSamples,
            static_cast<int64_t>(std::ceil(juce::jmax(startSeconds, endSeconds)
                                           * state.info.sampleRate)));
        resolved.sampleRange = { startSample, endSample };
    }
    else
    {
        resolved.sampleRange = { 0, state.info.numSamples };
    }

    if (resolved.sampleRange.isEmpty())
        return {};

    if (request.sourceChannels.empty())
    {
        resolved.sourceChannels.reserve(static_cast<std::size_t>(state.info.numChannels));
        for (int channel = 0; channel < state.info.numChannels; ++channel)
            resolved.sourceChannels.push_back(SourceChannelIdentity::fromSourceIndex(channel));
    }
    else
    {
        auto indices = std::vector<int> {};
        indices.reserve(request.sourceChannels.size());
        for (const auto& identity : request.sourceChannels)
        {
            if (!juce::isPositiveAndBelow(identity.sourceChannelIndex, state.info.numChannels))
                return {};
            indices.push_back(identity.sourceChannelIndex);
        }

        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        resolved.sourceChannels.reserve(indices.size());
        for (const auto index : indices)
            resolved.sourceChannels.push_back(SourceChannelIdentity::fromSourceIndex(index));
    }

    resolved.wholeDuration = resolved.sampleRange == juce::Range<int64_t>(0, state.info.numSamples);
    resolved.allSourceChannels = static_cast<int>(resolved.sourceChannels.size()) == state.info.numChannels;
    return resolved;
}

AudioSuiteReferenceProcessor::AnalyzeResult AudioSuiteReferenceProcessor::analyze(
    SingleTrackDocument::StatePtr state,
    int64_t documentGeneration,
    const AudioSelectionRequest& selectionRequest,
    float linearGain,
    double maximumPreviewSeconds) const
{
    if (!std::isfinite(linearGain) || linearGain < 0.0f || linearGain > 16.0f)
    {
        AnalyzeResult result;
        result.errorMessage = "Reference gain must be between 0 and 16 linear.";
        return result;
    }

    AudioSuitePlanBuildRequest request;
    request.state = std::move(state);
    request.documentGeneration = documentGeneration;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    request.lifecycleContext = observedContext();
    request.lifecycleOwner = OptionalLifecycleOwner::referenceGain;
    request.processorResourceId = observedResourceId();
#endif
    request.selection = selectionRequest;
    request.processorId = descriptor.processorId;
    request.processorVersion = descriptor.processorVersion;
    request.renderFactory = std::make_shared<ReferenceGainRenderFactory>();
    request.referenceLinearGain = linearGain;
    request.maximumPreviewSeconds = maximumPreviewSeconds;
    return AudioSuitePlanBuilder::build(request);
}

AudioSuitePreviewHandle AudioSuiteReferenceProcessor::createPreview(
    const AudioSuitePlanPtr& plan) const
{
    if (plan == nullptr
        || plan->getProcessorId() != descriptor.processorId
        || plan->getProcessorVersion() != descriptor.processorVersion)
    {
        return {};
    }
    return AudioSuiteTransactionExecutor::createPreview(plan);
}

AudioSuiteReferenceProcessor::ApplyResult AudioSuiteReferenceProcessor::apply(
    SingleTrackDocument& document,
    const AudioSuitePlanPtr& plan,
    int64_t currentDocumentGeneration,
    const AudioSuiteCancellationToken& cancellation,
    int renderBlockSize) const
{
    if (plan == nullptr
        || plan->getProcessorId() != descriptor.processorId
        || plan->getProcessorVersion() != descriptor.processorVersion)
    {
        return {};
    }
    return AudioSuiteTransactionExecutor::apply(document,
                                                plan,
                                                currentDocumentGeneration,
                                                cancellation,
                                                renderBlockSize);
}

void AudioSuiteReferenceProcessor::renderBlockInPlace(
    const AudioSuitePlan& plan,
    juce::AudioBuffer<float>& buffer,
    int64_t absoluteStartSample,
    juce::Range<int64_t> activeRange)
{
    if (auto session = plan.createRenderSession())
        session->renderBlockInPlace(buffer, absoluteStartSample, activeRange);
}

float AudioSuiteReferenceProcessor::gainForSample(
    const AudioSuitePlan& plan,
    int sourceChannelIndex,
    int64_t absoluteSample,
    juce::Range<int64_t> activeRange) noexcept
{
    return referenceGainForSample(plan.getSelection(),
                                  plan.getLinearGain(),
                                  plan.getBoundaryCrossfadeSamples(),
                                  sourceChannelIndex,
                                  absoluteSample,
                                  activeRange);
}

int AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() noexcept
{
    return liveInstanceCount.load(std::memory_order_acquire);
}

AudioSuiteReferenceProcessor::AudioSuiteReferenceProcessor(
    CapabilityDescriptor descriptorToUse)
    : descriptor(std::move(descriptorToUse))
{
    liveInstanceCount.fetch_add(1, std::memory_order_acq_rel);
}

AudioSuiteReferenceProcessor::~AudioSuiteReferenceProcessor()
{
    liveInstanceCount.fetch_sub(1, std::memory_order_acq_rel);
}
} // namespace singleeditor
