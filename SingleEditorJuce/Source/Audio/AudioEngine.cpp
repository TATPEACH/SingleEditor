#include "Audio/AudioEngine.h"
#include "Audio/NativeAudioDeviceBinding.h"
#include "Audio/NativeAsioDevice.h"
#include "Audio/SystemAudioDeviceManager.h"
#include "Audio/DeviceSwitchCoordinator.h"
#include "App/ShutdownDiagnostics.h"

#include "Audio/AmbisonicBinauralRenderer.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicStageGraph.h"
#include "Audio/ChannelRouter.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"

#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
#include <algorithm>
#endif

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace singleeditor
{
namespace
{
int countEnabledChannels(const juce::BigInteger& channels, int channelLimit) noexcept
{
    auto count = 0;
    for (int channel = 0; channel < channelLimit; ++channel)
    {
        if (channels[channel])
            ++count;
    }
    return count;
}

bool routingMatricesDiffer(const RoutingMatrix& left,
                           const RoutingMatrix& right) noexcept
{
    if (left.sourceChannels != right.sourceChannels
        || left.outputChannels != right.outputChannels)
    {
        return true;
    }

    for (auto output = 0; output < left.outputChannels; ++output)
    {
        for (auto source = 0; source < left.sourceChannels; ++source)
        {
            if (std::abs(left.gain(output, source) - right.gain(output, source)) > 1.0e-6f)
                return true;
        }
    }
    return false;
}
} // namespace

class AudioEngine::DocumentAudioSource final : public juce::PositionableAudioSource
{
public:
    struct OrientationPreview
    {
        std::shared_ptr<const AmbisonicStageGraph> stageGraph;
        AmbisonicOrientation orientation;
        AmbisonicMonitorMode monitorMode = AmbisonicMonitorMode::speakerProjection;
    };

    DocumentAudioSource(SingleTrackDocument& documentToUse,
                        std::atomic<std::shared_ptr<const DeviceRoutePlan>>& routeToRead,
                        std::atomic<bool>& unavailableToUpdate,
                        std::atomic<float>& monitorGainToRead,
                        std::atomic<bool>& includeLfeToRead,
                        std::atomic<int>& monitorModeToRead,
                        std::array<std::atomic<bool>, 32>& mutedChannelsToRead,
                        std::array<std::atomic<bool>, 32>& soloedChannelsToRead,
                        std::atomic<bool>& outputMappingEnabledToRead,
                        std::array<std::atomic<int>, ChannelOutputMapping::maxRoleSlots>& outputMappingToRead,
                        std::atomic<bool>& previewActiveToRead,
                        std::atomic<double>& previewStartToRead,
                        std::atomic<double>& previewEndToRead,
                        std::atomic<float>& previewGainToRead,
                        std::atomic<std::shared_ptr<const AudioSuitePreviewState>>& audioSuitePreviewToRead) noexcept
        : document(documentToUse),
          deviceRoutePlan(routeToRead),
          deviceRestoreUnavailable(unavailableToUpdate),
          monitorGain(monitorGainToRead),
          includeLfeInDownmix(includeLfeToRead),
          monitorModeValue(monitorModeToRead),
          sourceChannelMuted(mutedChannelsToRead),
          sourceChannelSoloed(soloedChannelsToRead),
          outputMappingEnabled(outputMappingEnabledToRead),
          outputChannelBySpeakerRole(outputMappingToRead),
          previewActive(previewActiveToRead),
          previewStartSeconds(previewStartToRead),
          previewEndSeconds(previewEndToRead),
          previewGainLinear(previewGainToRead),
          audioSuitePreviewState(audioSuitePreviewToRead)
    {
    }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    void setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        optionalLifecycleContext = context;
    }
    ~DocumentAudioSource() override
    {
        // Engine shutdown already detached its callback. Retained shared readers
        // own their own terminals; this only closes the actual playback demand.
        if (playbackLifecycle.isOpen()) playbackLifecycle.release();
    }
#endif

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        speakerRoutingState = nullptr;
        preparedMaximumBlockSize = juce::jmax(1, samplesPerBlockExpected);
        preparedSampleRate = sampleRate;
        playbackLeaseActive = true;
        rebuildAmbisonicStageGraphLocked();
        if (binauralState != nullptr
            && binauralState->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
        {
            binauralResetRequested.store(true, std::memory_order_release);
        }
        rebuildBinauralRendererLocked();
    }

    void releaseResources() override
    {
        {
            const juce::ScopedLock sl(binauralConfigurationLock);
            auto previous = publishedBinauralRenderer.exchange(
                std::shared_ptr<AmbisonicBinauralRenderer> {},
                std::memory_order_acq_rel);
            if (previous != nullptr)
                retiredBinauralRenderers.push_back(std::move(previous));
            collectRetiredBinauralRenderersLocked();
            binauralRendererChanged.store(true, std::memory_order_release);
            preparedMaximumBlockSize = 0;
            preparedSampleRate = 0.0;
            playbackLeaseActive = false;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (playbackLifecycle.isOpen()) playbackLifecycle.release();
#endif
            publishAmbisonicStageGraphLocked({});
            if (const auto preview = publishedOrientationPreview.load(std::memory_order_acquire);
                preview != nullptr && preview->stageGraph != nullptr)
            {
                publishOrientationPreviewLocked(std::make_shared<const OrientationPreview>(
                    OrientationPreview { {}, preview->orientation, preview->monitorMode }));
            }
        }
        binauralRuntimeActive.store(false, std::memory_order_release);
        speakerRoutingState = nullptr;
    }

    void configureBinauralState(SingleTrackDocument::StatePtr state)
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        binauralState = std::move(state);
        if (previewCustomDecodeArtifact != nullptr
            && (binauralState == nullptr
                || !binauralState->info.channelLayout.isAmbisonic
                || binauralState->info.file != decodeArtifactPreviewSourceFile
                || previewCustomDecodeArtifact->ambisonicOrder
                       != binauralState->info.channelLayout.ambisonicOrder))
        {
            previewCustomDecodeArtifact.reset();
            decodeArtifactPreviewSourceFile = {};
        }
        if (const auto preview = publishedOrientationPreview.load(std::memory_order_acquire);
            preview != nullptr
                && (binauralState == nullptr
                    || !binauralState->info.channelLayout.isAmbisonic
                    || binauralState->ambisonicMonitorMode != preview->monitorMode
                    || binauralState->info.file != orientationPreviewSourceFile))
        {
            publishOrientationPreviewLocked({});
            orientationPreviewSourceFile = {};
        }
        rebuildAmbisonicFieldAnalyzerLocked();
        rebuildAmbisonicStageGraphLocked();
        rebuildBinauralRendererLocked();
    }

    std::uint64_t acquireAmbisonicFieldAnalysisLease()
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        const auto leaseId = nextFieldAnalysisLeaseId++;
        activeFieldAnalysisLeases.insert(leaseId);
        rebuildAmbisonicFieldAnalyzerLocked();
        return leaseId;
    }

    void releaseAmbisonicFieldAnalysisLease(std::uint64_t leaseId)
    {
        if (leaseId == 0)
            return;

        const juce::ScopedLock sl(binauralConfigurationLock);
        if (activeFieldAnalysisLeases.erase(leaseId) != 0)
            rebuildAmbisonicFieldAnalyzerLocked();
    }

    int getAmbisonicFieldAnalysisLeaseCount() const noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        return static_cast<int>(activeFieldAnalysisLeases.size());
    }

    bool isBinauralRendererActive() const noexcept
    {
        return binauralRuntimeActive.load(std::memory_order_acquire);
    }

    void collectRetiredBinauralRenderers()
    {
        if (!binauralRetirementPending.load(std::memory_order_acquire))
            return;
        const juce::ScopedLock sl(binauralConfigurationLock);
        collectRetiredBinauralRenderersLocked();
    }

#if SINGLE_EDITOR_AUDIO_RESOURCE_TESTING
    std::shared_ptr<const void> retainBinauralRendererForTesting() const
    {
        return publishedBinauralRenderer.load(std::memory_order_acquire);
    }
#endif

    // Caller holds the existing device callback lock, or owns an engine with
    // no device callback. Copy the renderer's existing cache; no RT observer,
    // sampling timer, allocation, or diagnostic history is introduced.
    AudioEngine::RenderDiagnostics renderDiagnostics(const SingleTrackDocument::StatePtr& current) const
    {
        AudioEngine::RenderDiagnostics result;
        if (current == nullptr || speakerRoutingState != current.get()) return result;
        result.availability = AudioEngine::RenderDiagnostics::Availability::available;
        result.monitorMode = cachedTargetMonitorMode;
        result.ambisonic = current->info.channelLayout.isAmbisonic;
        result.binauralRequested = current->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf;
        result.binauralActive = binauralRuntimeActive.load(std::memory_order_acquire);
        if (!result.binauralRequested && speakerRoutingInitialised
            && lastSpeakerRouting.sourceChannels > 0 && lastSpeakerRouting.outputChannels > 0)
            result.matrix = lastSpeakerRouting;
        return result;
    }

    AmbisonicFieldSnapshot getAmbisonicFieldSnapshot() const noexcept
    {
        const auto analyzer = publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire);
        return analyzer != nullptr ? analyzer->getSnapshot() : AmbisonicFieldSnapshot {};
    }

    bool hasAmbisonicFieldAnalyzer() const noexcept
    {
        return publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire) != nullptr;
    }

    bool hasAmbisonicStageGraph() const noexcept
    {
        return publishedAmbisonicStageGraph.load(std::memory_order_acquire) != nullptr;
    }

    bool isCustomAmbisonicDecodeMatrixActive() const noexcept
    {
        const auto graph = publishedAmbisonicStageGraph.load(std::memory_order_acquire);
        return graph != nullptr && graph->usesCustomSpeakerGeometry();
    }

    std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
    getAmbisonicDecodeMatrixArtifact() const noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        return activeCustomDecodeArtifact;
    }

    std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
    getAmbisonicDecodeMatrixPreview() const noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        return previewCustomDecodeArtifact;
    }

    bool activateAmbisonicDecodeMatrixArtifact(
        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact)
    {
        if (artifact == nullptr || !artifact->isValid())
            return false;
        const juce::ScopedLock sl(binauralConfigurationLock);
        if (binauralState != nullptr && binauralState->info.channelLayout.isAmbisonic
            && artifact->ambisonicOrder != binauralState->info.channelLayout.ambisonicOrder)
        {
            return false;
        }
        activeCustomDecodeArtifact = std::move(artifact);
        previewCustomDecodeArtifact.reset();
        decodeArtifactPreviewSourceFile = {};
        rebuildAmbisonicStageGraphLocked();
        return true;
    }

    void clearAmbisonicDecodeMatrixArtifact() noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        activeCustomDecodeArtifact.reset();
        previewCustomDecodeArtifact.reset();
        decodeArtifactPreviewSourceFile = {};
        rebuildAmbisonicStageGraphLocked();
    }

    bool previewAmbisonicDecodeMatrixArtifact(
        std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact)
    {
        if (artifact == nullptr || !artifact->isValid())
            return false;
        const juce::ScopedLock sl(binauralConfigurationLock);
        if (binauralState == nullptr || !binauralState->info.channelLayout.isAmbisonic
            || artifact->ambisonicOrder != binauralState->info.channelLayout.ambisonicOrder)
        {
            return false;
        }
        previewCustomDecodeArtifact = std::move(artifact);
        decodeArtifactPreviewSourceFile = binauralState->info.file;
        rebuildAmbisonicStageGraphLocked();
        return true;
    }

    void clearAmbisonicDecodeMatrixPreview() noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        previewCustomDecodeArtifact.reset();
        decodeArtifactPreviewSourceFile = {};
        rebuildAmbisonicStageGraphLocked();
    }

    bool setAmbisonicOrientationPreview(AmbisonicOrientation orientation)
    {
        const auto state = document.getState();
        if (state == nullptr
            || !state->info.channelLayout.isAmbisonic)
        {
            return false;
        }

        orientation = AmbisonicMonitorDecoder::canonicalizeOrientation(orientation);
        const juce::ScopedLock sl(binauralConfigurationLock);
        std::shared_ptr<const AmbisonicStageGraph> stageGraph;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const ScopedOptionalLifecycleContext construction(optionalLifecycleContext);
#endif
        if (playbackLeaseActive)
        {
            stageGraph = AmbisonicStageGraph::create(state->info.channelLayout,
                                                     orientation,
                                                     nextStageGraphBindingGeneration++,
                                                     effectiveCustomArtifactLocked());
            if (stageGraph == nullptr)
                return false;
        }

        orientationPreviewSourceFile = state->info.file;
        publishOrientationPreviewLocked(std::make_shared<const OrientationPreview>(
            OrientationPreview { std::move(stageGraph), orientation, state->ambisonicMonitorMode }));
        if (state->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
        {
            if (const auto renderer = publishedBinauralRenderer.load(std::memory_order_acquire);
                renderer != nullptr)
            {
                renderer->setOrientation(orientation);
            }
        }
        return true;
    }

    void clearAmbisonicOrientationPreview() noexcept
    {
        const juce::ScopedLock sl(binauralConfigurationLock);
        publishOrientationPreviewLocked({});
        orientationPreviewSourceFile = {};
        if (binauralState != nullptr
            && binauralState->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf)
        {
            if (const auto renderer = publishedBinauralRenderer.load(std::memory_order_acquire);
                renderer != nullptr)
            {
                renderer->setOrientation(binauralState->ambisonicOrientation);
            }
        }
    }

    void requestAmbisonicFieldReset() noexcept
    {
        if (const auto analyzer = publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire);
            analyzer != nullptr)
        {
            analyzer->requestReset();
        }
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        bufferToFill.clearActiveBufferRegion();

        const auto state = document.getState();
        if (!state->info.isValid() || !state->hasPlaybackSource())
            return;

        const auto outputChannels = bufferToFill.buffer->getNumChannels();
        const auto sourceChannels = state->audioBuffer != nullptr ? state->audioBuffer->getNumChannels()
                                                                  : state->info.numChannels;
        if (sourceChannels <= 0)
            return;

        const auto totalSamples = state->info.numSamples;
        const auto sampleRate = state->info.sampleRate;
        const auto previewOn = previewActive.load(std::memory_order_relaxed);
        const auto previewStart = previewStartSeconds.load(std::memory_order_relaxed);
        const auto previewEnd = previewEndSeconds.load(std::memory_order_relaxed);
        const auto previewGain = previewGainLinear.load(std::memory_order_relaxed);
        const auto monitorGainValue = monitorGain.load(std::memory_order_relaxed);
        const auto unityEnvelope = SingleTrackDocument::isEnvelopeFlatUnity(state->envelopePoints);
        const auto sourceStart = juce::jlimit<juce::int64>(0, totalSamples, nextReadPosition);
        const auto playableSamples = state->getPlayableSampleRange();
        if (!playableSamples.contains(sourceStart))
            return;

        const auto samplesToRead = static_cast<int>(juce::jlimit<juce::int64>(0,
                                                                               static_cast<juce::int64>(bufferToFill.numSamples),
                                                                               juce::jmin(totalSamples,
                                                                                          playableSamples.getEnd())
                                                                                   - sourceStart));
        if (samplesToRead <= 0)
            return;

        const auto audioSuitePreview = audioSuitePreviewState.load(std::memory_order_acquire);
        const auto audioSuitePreviewMatches = audioSuitePreview != nullptr
            && audioSuitePreview->isActive()
            && audioSuitePreview->plan->matchesState(
                state, audioSuitePreview->plan->getDocumentGeneration(), false)
            && audioSuitePreview->plan->getPreviewRange().intersects(
                { sourceStart, sourceStart + samplesToRead });
        const auto previewLookAhead = audioSuitePreviewMatches
            ? audioSuitePreview->getRequiredLookAheadSamples()
            : 0;
        const auto readableEnd = juce::jmin(totalSamples, playableSamples.getEnd());
        const auto sourceSamplesToRead = samplesToRead + static_cast<int>(
            juce::jmin<int64_t>(previewLookAhead,
                                readableEnd - sourceStart - samplesToRead));

        const juce::AudioBuffer<float>* sourceBuffer = state->audioBuffer.get();
        if (sourceBuffer == nullptr)
        {
            juce::AudioFormatReader* reader = nullptr;
            if (state->memoryMappedReader != nullptr)
            {
                const auto requestedRange = juce::Range<juce::int64>(
                    sourceStart, sourceStart + sourceSamplesToRead);
                if (!state->memoryMappedReader->getMappedSection().contains(requestedRange)
                    && !state->memoryMappedReader->mapSectionOfFile(requestedRange))
                {
                    return;
                }

                reader = state->memoryMappedReader.get();
            }
            else
            {
                reader = state->streamingReader.get();
            }

            if (reader == nullptr)
                return;

            scratchReadBuffer.setSize(sourceChannels,
                                      sourceSamplesToRead,
                                      false,
                                      false,
                                      true);
            scratchReadBuffer.clear();
            if (!reader->read(&scratchReadBuffer,
                              0,
                              sourceSamplesToRead,
                              sourceStart,
                              true,
                              sourceChannels > 1))
            {
                return;
            }

            sourceBuffer = &scratchReadBuffer;
        }

        auto sourceBufferOffset = sourceBuffer == state->audioBuffer.get()
                                ? static_cast<int>(sourceStart)
                                : 0;

        if (audioSuitePreviewMatches)
        {
            audioSuitePreviewBuffer.setSize(sourceChannels,
                                            sourceSamplesToRead,
                                            false,
                                            false,
                                            true);
            for (int channel = 0; channel < sourceChannels; ++channel)
            {
                audioSuitePreviewBuffer.copyFrom(channel,
                                                 0,
                                                 *sourceBuffer,
                                                 channel,
                                                 sourceBufferOffset,
                                                 sourceSamplesToRead);
            }
            audioSuitePreview->renderBlockWithLookAheadInPlace(
                audioSuitePreviewBuffer,
                samplesToRead,
                sourceStart,
                audioSuitePreview->plan->getPreviewRange());
            sourceBuffer = &audioSuitePreviewBuffer;
            sourceBufferOffset = 0;
        }

        const auto& sourceLayout = state->info.channelLayout;
        ChannelPlaybackState channelPlayback;
        channelPlayback.includeLfeInDownmix = includeLfeInDownmix.load(std::memory_order_relaxed);
        channelPlayback.monitorMode = static_cast<MonitorMode>(monitorModeValue.load(std::memory_order_relaxed));
        for (int sourceChannel = 0; sourceChannel < juce::jmin(sourceChannels, RoutingMatrix::maxChannels); ++sourceChannel)
        {
            channelPlayback.mutedBySourceChannel[static_cast<std::size_t>(sourceChannel)] =
                sourceChannelMuted[static_cast<std::size_t>(sourceChannel)].load(std::memory_order_relaxed);
            channelPlayback.soloedBySourceChannel[static_cast<std::size_t>(sourceChannel)] =
                sourceChannelSoloed[static_cast<std::size_t>(sourceChannel)].load(std::memory_order_relaxed);
        }
        const auto routePlan = deviceRoutePlan.load(std::memory_order_acquire);
        if (!routePlan && outputMappingEnabled.load(std::memory_order_acquire))
        {
            channelPlayback.outputMapping.enabled = true;
            for (int roleIndex = 0; roleIndex < ChannelOutputMapping::maxRoleSlots; ++roleIndex)
            {
                channelPlayback.outputMapping.outputChannelByRole[static_cast<std::size_t>(roleIndex)] =
                    outputChannelBySpeakerRole[static_cast<std::size_t>(roleIndex)].load(
                        std::memory_order_relaxed);
            }
        }

        auto effectiveOrientation = state->ambisonicOrientation;
        std::shared_ptr<const AmbisonicStageGraph> effectiveStageGraph;
        if (sourceLayout.isAmbisonic)
        {
            if (const auto preview = publishedOrientationPreview.load(std::memory_order_acquire);
                preview != nullptr)
            {
                effectiveStageGraph = preview->stageGraph;
                effectiveOrientation = preview->orientation;
            }
            if (effectiveStageGraph == nullptr)
                effectiveStageGraph = publishedAmbisonicStageGraph.load(std::memory_order_acquire);

            if (const auto analyzer = publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire);
                analyzer != nullptr)
            {
                analyzer->process(*sourceBuffer,
                                  sourceBufferOffset,
                                  samplesToRead,
                                  sourceLayout,
                                  effectiveOrientation,
                                  channelPlayback);
            }
        }

        const auto& targetLayout = routePlan ? routePlan->canonicalLayout()
            : resolveTargetLayout(outputChannels, channelPlayback.monitorMode, sourceChannels);
        const auto binauralRequested = state->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf;
        auto binauralRenderer = binauralRequested
                              ? publishedBinauralRenderer.load(std::memory_order_acquire)
                              : std::shared_ptr<AmbisonicBinauralRenderer> {};
        if (binauralRenderer != nullptr
            && binauralResetRequested.exchange(false, std::memory_order_acq_rel))
        {
            binauralRenderer->reset();
        }
        const auto binauralActive = binauralRenderer != nullptr && outputChannels >= 2;
        binauralRuntimeActive.store(binauralActive, std::memory_order_release);
        if (binauralActive)
        {
            // A drained renderer can now be freed immediately. Do not let an
            // allocator reusing its address suppress the new-renderer fade.
            const auto rendererChanged = binauralRendererChanged.exchange(false, std::memory_order_acq_rel);
            if (rendererChanged || lastRenderedBinauralRenderer != binauralRenderer.get())
            {
                lastRenderedBinauralRenderer = binauralRenderer.get();
                binauralFadeSamplesRemaining = juce::jmin(64, samplesToRead);
            }
            binauralRenderer->process(*sourceBuffer,
                                      sourceBufferOffset,
                                      *bufferToFill.buffer,
                                      bufferToFill.startSample,
                                      samplesToRead,
                                      channelPlayback);
        }
        else
        {
            lastRenderedBinauralRenderer = nullptr;
            binauralFadeSamplesRemaining = 0;
        }

        RoutingMatrix routing;
        RoutingMatrix previousRouting;
        auto blendSpeakerRouting = false;
        if (!binauralRequested)
        {
            if (sourceLayout.isAmbisonic)
            {
                if (effectiveStageGraph != nullptr)
                {
                    if (routePlan && !effectiveStageGraph->matchesCanonicalBus(routePlan->bus()))
                        deviceRestoreUnavailable.store(true, std::memory_order_release);
                    else
                    {
                        routing = routePlan ? effectiveStageGraph->buildCanonicalSpeakerBusMatrix(targetLayout, channelPlayback)
                                            : effectiveStageGraph->buildPhysicalRoutingMatrix(targetLayout, channelPlayback);
                        if (routePlan && routing.outputChannels != targetLayout.sourceChannelCount)
                            deviceRestoreUnavailable.store(true, std::memory_order_release);
                    }
                }
            }
            else
            {
                routing = ChannelRouter::buildRoutingMatrix(sourceLayout,
                                                            targetLayout,
                                                            channelPlayback);
            }

            const auto stateChanged = speakerRoutingState != state.get();
            const auto dimensionsChanged = lastSpeakerRouting.sourceChannels != routing.sourceChannels
                                        || lastSpeakerRouting.outputChannels != routing.outputChannels;
            if (!speakerRoutingInitialised || stateChanged || dimensionsChanged)
            {
                lastSpeakerRouting = routing;
                speakerRoutingState = state.get();
                speakerRoutingInitialised = true;
            }
            else if (routingMatricesDiffer(lastSpeakerRouting, routing))
            {
                if (sourceLayout.isAmbisonic)
                {
                    previousRouting = lastSpeakerRouting;
                    blendSpeakerRouting = true;
                }
                lastSpeakerRouting = routing;
            }
        }
        else
        {
            speakerRoutingInitialised = false;
            speakerRoutingState = state.get(); // binds the last binaural render to its Document too
        }
        const auto routedSourceChannels = juce::jmin(sourceChannels, routing.sourceChannels);

        for (int sample = 0; sample < samplesToRead; ++sample)
        {
            const auto sourceIndex = sourceStart + sample;

            const auto timeSeconds = sampleRate > 0.0 ? static_cast<double>(sourceIndex) / sampleRate : 0.0;
            auto gain = unityEnvelope ? 1.0f
                                      : SingleTrackDocument::evaluateEnvelope(state->envelopePoints, timeSeconds);
            if (previewOn && timeSeconds >= previewStart && timeSeconds <= previewEnd)
                gain *= previewGain;
            auto binauralTransitionGain = 1.0f;
            if (binauralActive && binauralFadeSamplesRemaining > 0)
            {
                binauralTransitionGain = 1.0f
                                       - (static_cast<float>(binauralFadeSamplesRemaining) / 64.0f);
                --binauralFadeSamplesRemaining;
            }
            const auto routingBlend = blendSpeakerRouting && samplesToRead > 1
                                    ? static_cast<float>(sample) / static_cast<float>(samplesToRead - 1)
                                    : 1.0f;

            // Preserve both ears before writing canonical rows (L/R can be reordered).
            const auto binauralLeft = binauralActive ? bufferToFill.buffer->getSample(0, bufferToFill.startSample + sample) : 0.0f;
            const auto binauralRight = binauralActive ? bufferToFill.buffer->getSample(1, bufferToFill.startSample + sample) : 0.0f;
            for (int channel = 0; channel < outputChannels; ++channel)
            {
                auto mixedSample = binauralActive
                                 ? bufferToFill.buffer->getSample(channel,
                                                                  bufferToFill.startSample + sample)
                                 : 0.0f;
                if (binauralActive && routePlan)
                {
                    const auto role = channel < targetLayout.sourceChannelCount
                        ? targetLayout.selectedRoles[static_cast<std::size_t>(channel)] : SpeakerRole::unknown;
                    mixedSample = role == SpeakerRole::L ? binauralLeft : role == SpeakerRole::R ? binauralRight : 0.0f;
                }
                if (!binauralRequested && channel < routing.outputChannels)
                {
                    for (int sourceChannel = 0; sourceChannel < routedSourceChannels; ++sourceChannel)
                    {
                        const auto nextGain = routing.gain(channel, sourceChannel);
                        const auto routeGain = blendSpeakerRouting
                                             ? previousRouting.gain(channel, sourceChannel)
                                                   + (routingBlend
                                                      * (nextGain
                                                         - previousRouting.gain(channel, sourceChannel)))
                                             : nextGain;
                        mixedSample += sourceBuffer->getSample(sourceChannel, sourceBufferOffset + sample)
                                     * routeGain;
                    }
                }

                const auto sampleValue = mixedSample * gain * monitorGainValue * binauralTransitionGain;
                bufferToFill.buffer->setSample(channel,
                                               bufferToFill.startSample + sample,
                                               sampleValue);
            }
        }

        nextReadPosition = sourceStart + samplesToRead;
        nextReadPosition = juce::jmin(nextReadPosition, totalSamples);
    }

    void setNextReadPosition(juce::int64 newPosition) override
    {
        nextReadPosition = juce::jmax<juce::int64>(0, newPosition);
        binauralResetRequested.store(true, std::memory_order_release);
        if (const auto state = document.getState(); state != nullptr
            && state->info.channelLayout.isAmbisonic)
        {
            if (const auto analyzer = publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire);
                analyzer != nullptr)
            {
                analyzer->requestReset();
            }
        }
    }

    juce::int64 getNextReadPosition() const override
    {
        return nextReadPosition;
    }

    juce::int64 getTotalLength() const override
    {
        return document.getNumSamples();
    }

    bool isLooping() const override
    {
        return false;
    }

private:
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> effectiveCustomArtifactLocked() const
    {
        if (binauralState == nullptr
            || !binauralState->info.channelLayout.isAmbisonic
            || (previewCustomDecodeArtifact != nullptr
                && (decodeArtifactPreviewSourceFile != binauralState->info.file
                    || previewCustomDecodeArtifact->ambisonicOrder
                           != binauralState->info.channelLayout.ambisonicOrder)))
        {
            return {};
        }
        if (previewCustomDecodeArtifact != nullptr)
            return previewCustomDecodeArtifact;
        if (activeCustomDecodeArtifact != nullptr
            && activeCustomDecodeArtifact->ambisonicOrder
                   == binauralState->info.channelLayout.ambisonicOrder)
        {
            return activeCustomDecodeArtifact;
        }
        return {};
    }

    void publishAmbisonicStageGraphLocked(std::shared_ptr<const AmbisonicStageGraph> next)
    {
        publishedAmbisonicStageGraph.exchange(std::move(next),
                                              std::memory_order_acq_rel);
    }

    void rebuildAmbisonicStageGraphLocked()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const ScopedOptionalLifecycleContext construction(optionalLifecycleContext);
#endif
        if (!playbackLeaseActive
            || binauralState == nullptr
            || !binauralState->info.channelLayout.isAmbisonic)
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            if (playbackLifecycle.isOpen()) playbackLifecycle.release();
#endif
            publishAmbisonicStageGraphLocked({});
            return;
        }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (!playbackLifecycle.isOpen())
        {
            playbackLifecycle = OptionalLifecycleToken::request(optionalLifecycleContext,
                OptionalLifecycleOwner::ambisonics, OptionalLifecycleResourceKind::ambisonicPlaybackConsumer);
            playbackLifecycle.created();
        }
#endif

        publishAmbisonicStageGraphLocked(AmbisonicStageGraph::create(
            binauralState->info.channelLayout,
            binauralState->ambisonicOrientation,
            nextStageGraphBindingGeneration++,
            effectiveCustomArtifactLocked()));

        if (const auto preview = publishedOrientationPreview.load(std::memory_order_acquire);
            preview != nullptr && binauralState->info.file == orientationPreviewSourceFile)
        {
            auto previewGraph = AmbisonicStageGraph::create(
                binauralState->info.channelLayout,
                preview->orientation,
                nextStageGraphBindingGeneration++,
                effectiveCustomArtifactLocked());
            publishOrientationPreviewLocked(std::make_shared<const OrientationPreview>(
                OrientationPreview { std::move(previewGraph),
                                     preview->orientation,
                                     preview->monitorMode }));
        }
    }

    void rebuildAmbisonicFieldAnalyzerLocked()
    {
        const auto shouldExist = !activeFieldAnalysisLeases.empty()
                              && binauralState != nullptr
                              && binauralState->info.channelLayout.isAmbisonic;
        if (!shouldExist)
        {
            publishedAmbisonicFieldAnalyzer.exchange({},
                                                     std::memory_order_acq_rel);
            fieldAnalyzerSourceFile = {};
            fieldAnalyzerAudioRevision = 0;
            fieldAnalyzerPresetId.clear();
            return;
        }

        const auto bindingMatches = publishedAmbisonicFieldAnalyzer.load(std::memory_order_acquire) != nullptr
                                 && fieldAnalyzerSourceFile == binauralState->info.file
                                 && fieldAnalyzerAudioRevision == binauralState->audioRevision
                                 && fieldAnalyzerPresetId == binauralState->info.channelLayout.selectedPresetId;
        if (bindingMatches)
            return;

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        auto lifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
            OptionalLifecycleOwner::ambisonics, OptionalLifecycleResourceKind::ambisonicFieldAnalyzer);
        try
        {
#endif
        auto analyzer = std::make_shared<AmbisonicFieldAnalyzer>();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        lifetime.created();
        analyzer->observeLifetime(std::move(lifetime));
#endif
        analyzer->prepare(binauralState->info.sampleRate);
        publishedAmbisonicFieldAnalyzer.exchange(std::move(analyzer),
                                                 std::memory_order_acq_rel);
        fieldAnalyzerSourceFile = binauralState->info.file;
        fieldAnalyzerAudioRevision = binauralState->audioRevision;
        fieldAnalyzerPresetId = binauralState->info.channelLayout.selectedPresetId;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        }
        catch (...)
        {
            if (lifetime.isOpen()) lifetime.failed();
            throw;
        }
#endif
    }

    void publishOrientationPreviewLocked(std::shared_ptr<const OrientationPreview> next)
    {
        publishedOrientationPreview.exchange(std::move(next),
                                             std::memory_order_acq_rel);
    }

    void rebuildBinauralRendererLocked()
    {
        std::shared_ptr<AmbisonicBinauralRenderer> nextRenderer;
        if (binauralState != nullptr
            && binauralState->ambisonicMonitorMode == AmbisonicMonitorMode::binauralHrtf
            && binauralState->ambisonicBinauralFilterBank != nullptr
            && binauralState->info.channelLayout.isAmbisonic
            && preparedSampleRate > 0.0
            && preparedMaximumBlockSize > 0)
        {
            auto effectiveOrientation = binauralState->ambisonicOrientation;
            if (const auto preview = publishedOrientationPreview.load(std::memory_order_acquire);
                preview != nullptr && binauralState->info.file == orientationPreviewSourceFile)
            {
                effectiveOrientation = preview->orientation;
            }

            if (const auto current = publishedBinauralRenderer.load(std::memory_order_acquire);
                current != nullptr
                    && current->isCompatible(binauralState->ambisonicBinauralFilterBank,
                                             binauralState->info.channelLayout.ambisonicOrder,
                                             preparedSampleRate,
                                             preparedMaximumBlockSize))
            {
                current->setOrientation(effectiveOrientation);
                return;
            }

            juce::String error;
            auto renderer = AmbisonicBinauralRenderer::create(
                binauralState->ambisonicBinauralFilterBank,
                binauralState->info.channelLayout.ambisonicOrder,
                effectiveOrientation,
                preparedSampleRate,
                preparedMaximumBlockSize,
                error);
            if (renderer != nullptr)
                nextRenderer.reset(renderer.release());
        }

        auto previous = publishedBinauralRenderer.exchange(std::move(nextRenderer),
                                                           std::memory_order_acq_rel);
        if (previous != nullptr)
            retiredBinauralRenderers.push_back(std::move(previous));
        collectRetiredBinauralRenderersLocked();
        binauralRendererChanged.store(true, std::memory_order_release);
        binauralResetRequested.store(true, std::memory_order_release);
    }

    void collectRetiredBinauralRenderersLocked()
    {
        // The object has been removed from publication, so no new callback can
        // acquire it. Keep our final owner while any existing callback still
        // holds a lease; destruction then occurs here, never on that callback.
        std::erase_if(retiredBinauralRenderers, [](const auto& renderer) { return renderer.use_count() == 1; });
        binauralRetirementPending.store(!retiredBinauralRenderers.empty(), std::memory_order_release);
    }

    const ChannelLayoutState& resolveTargetLayout(int outputChannels, MonitorMode mode, int sourceChannels)
    {
        if (cachedTargetOutputChannels == outputChannels
            && cachedTargetSourceChannels == sourceChannels
            && cachedTargetMonitorMode == mode)
        {
            return cachedTargetLayout;
        }

        cachedTargetOutputChannels = outputChannels;
        cachedTargetSourceChannels = sourceChannels;
        cachedTargetMonitorMode = mode;

        cachedTargetLayout = mode == MonitorMode::forceStereoDownmix
            ? ChannelLayoutCatalog::stereoOutputLayout()
            : ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(outputChannels);

        if (mode == MonitorMode::forceMultichannel
            && outputChannels < juce::jmin(sourceChannels, 12))
        {
            cachedTargetLayout = ChannelLayoutCatalog::stereoOutputLayout();
        }

        cachedTargetLayout.sourceChannelCount = juce::jmin(outputChannels, cachedTargetLayout.sourceChannelCount);
        return cachedTargetLayout;
    }

    SingleTrackDocument& document;
    std::atomic<std::shared_ptr<const DeviceRoutePlan>>& deviceRoutePlan;
    std::atomic<bool>& deviceRestoreUnavailable;
    std::atomic<float>& monitorGain;
    std::atomic<bool>& includeLfeInDownmix;
    std::atomic<int>& monitorModeValue;
    std::array<std::atomic<bool>, 32>& sourceChannelMuted;
    std::array<std::atomic<bool>, 32>& sourceChannelSoloed;
    std::atomic<bool>& outputMappingEnabled;
    std::array<std::atomic<int>, ChannelOutputMapping::maxRoleSlots>& outputChannelBySpeakerRole;
    std::atomic<bool>& previewActive;
    std::atomic<double>& previewStartSeconds;
    std::atomic<double>& previewEndSeconds;
    std::atomic<float>& previewGainLinear;
    std::atomic<std::shared_ptr<const AudioSuitePreviewState>>& audioSuitePreviewState;
    juce::int64 nextReadPosition = 0;
    juce::AudioBuffer<float> scratchReadBuffer;
    juce::AudioBuffer<float> audioSuitePreviewBuffer;
    ChannelLayoutState cachedTargetLayout;
    int cachedTargetOutputChannels = -1;
    int cachedTargetSourceChannels = -1;
    MonitorMode cachedTargetMonitorMode = MonitorMode::autoFollowDevice;
    juce::CriticalSection binauralConfigurationLock;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleContext optionalLifecycleContext = currentOptionalLifecycleContext();
    OptionalLifecycleToken playbackLifecycle;
#endif
    SingleTrackDocument::StatePtr binauralState;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> activeCustomDecodeArtifact;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> previewCustomDecodeArtifact;
    juce::File decodeArtifactPreviewSourceFile;
    std::atomic<std::shared_ptr<AmbisonicBinauralRenderer>> publishedBinauralRenderer;
    std::vector<std::shared_ptr<AmbisonicBinauralRenderer>> retiredBinauralRenderers;
    std::atomic<bool> binauralRetirementPending { false };
    std::atomic<bool> binauralRendererChanged { false };
    std::atomic<bool> binauralResetRequested { false };
    std::atomic<bool> binauralRuntimeActive { false };
    std::atomic<std::shared_ptr<AmbisonicFieldAnalyzer>> publishedAmbisonicFieldAnalyzer;
    std::atomic<std::shared_ptr<const AmbisonicStageGraph>> publishedAmbisonicStageGraph;
    std::atomic<std::shared_ptr<const OrientationPreview>> publishedOrientationPreview;
    juce::File orientationPreviewSourceFile;
    std::unordered_set<std::uint64_t> activeFieldAnalysisLeases;
    std::uint64_t nextFieldAnalysisLeaseId = 1;
    std::uint64_t nextStageGraphBindingGeneration = 1;
    juce::File fieldAnalyzerSourceFile;
    int64_t fieldAnalyzerAudioRevision = 0;
    juce::String fieldAnalyzerPresetId;
    bool playbackLeaseActive = false;
    RoutingMatrix lastSpeakerRouting;
    const SingleTrackDocument::State* speakerRoutingState = nullptr;
    bool speakerRoutingInitialised = false;
    const AmbisonicBinauralRenderer* lastRenderedBinauralRenderer = nullptr;
    int binauralFadeSamplesRemaining = 0;
    int preparedMaximumBlockSize = 0;
    double preparedSampleRate = 0.0;
};

struct AudioEngine::AmbisonicFieldLeaseControl
{
    void release(std::uint64_t leaseId) noexcept
    {
        const juce::ScopedLock sl(lock);
        if (source != nullptr)
            source->releaseAmbisonicFieldAnalysisLease(leaseId);
    }

    juce::CriticalSection lock;
    DocumentAudioSource* source = nullptr;
};

AudioEngine::AmbisonicFieldAnalysisLease::AmbisonicFieldAnalysisLease(
    std::shared_ptr<AmbisonicFieldLeaseControl> controlToUse,
    std::uint64_t leaseIdToUse) noexcept
    : control(std::move(controlToUse)),
      leaseId(leaseIdToUse)
{
}

AudioEngine::AmbisonicFieldAnalysisLease::AmbisonicFieldAnalysisLease(
    AmbisonicFieldAnalysisLease&& other) noexcept
    : control(std::move(other.control)),
      leaseId(std::exchange(other.leaseId, 0))
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
      , optionalLifecycleToken(std::move(other.optionalLifecycleToken))
#endif
{
}

AudioEngine::AmbisonicFieldAnalysisLease&
AudioEngine::AmbisonicFieldAnalysisLease::operator=(AmbisonicFieldAnalysisLease&& other) noexcept
{
    if (this != &other)
    {
        reset();
        control = std::move(other.control);
        leaseId = std::exchange(other.leaseId, 0);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        optionalLifecycleToken = std::move(other.optionalLifecycleToken);
#endif
    }
    return *this;
}

AudioEngine::AmbisonicFieldAnalysisLease::~AmbisonicFieldAnalysisLease()
{
    reset();
}

void AudioEngine::AmbisonicFieldAnalysisLease::reset() noexcept
{
    if (control != nullptr && leaseId != 0)
        control->release(leaseId);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (optionalLifecycleToken.isOpen()) optionalLifecycleToken.release();
#endif
    leaseId = 0;
    control.reset();
}

class AudioEngine::TransportRenderSource final : public juce::AudioSource
{
public:
    TransportRenderSource(AudioEngine& engineToUpdate,
                          juce::AudioTransportSource& transportToUse,
                          std::atomic<double>& positionToUpdate,
                          std::atomic<double>& durationToRead,
                          std::atomic<double>& loopStartToRead,
                          std::atomic<bool>& loopFlagToRead,
                          std::atomic<AudioEngine::PlaybackState>& stateToUpdate,
                          std::atomic<int>& monitorModeToRead,
                          std::atomic<bool>& outputMappingEnabledToRead,
                          std::array<std::atomic<int>, ChannelOutputMapping::maxRoleSlots>& outputMappingToRead,
                          std::atomic<int>& pingRoleToRead,
                          std::atomic<int>& pingSamplesRemainingToRead,
                          std::atomic<std::shared_ptr<const AudioSuitePreviewState>>& audioSuitePreviewToRead) noexcept
        : engine(engineToUpdate),
          transport(transportToUse),
          currentPositionSeconds(positionToUpdate),
          durationSeconds(durationToRead),
          playStartSeconds(loopStartToRead),
          loopEnabled(loopFlagToRead),
          playbackState(stateToUpdate),
          monitorModeValue(monitorModeToRead),
          outputMappingEnabled(outputMappingEnabledToRead),
          outputChannelBySpeakerRole(outputMappingToRead),
          speakerPingRoleValue(pingRoleToRead),
          speakerPingSamplesRemaining(pingSamplesRemainingToRead),
          audioSuitePreviewState(audioSuitePreviewToRead)
    {
    }

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    ~TransportRenderSource() override
    {
        // Engine teardown reaches this after detaching the real audio callback.
        // Witness destruction, including analyzer storage, before the terminal.
        meter.reset();
        if (optionalMeterLifecycleToken.isOpen())
            optionalMeterLifecycleToken.release();
    }

    std::uint64_t outputMeterLifecycleResourceId() const noexcept
    {
        return optionalMeterLifecycleToken.resourceId();
    }
#endif

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const SpatialEnergyService::Change spatialChange(engine.spatialEnergy);
        ++meterPreparation;
        if (const auto plan = engine.deviceRoutePlan.load(std::memory_order_acquire))
        {
            canonicalBuffer.setSize(juce::jmax(2, plan->canonicalLayout().sourceChannelCount), juce::jmax(1, samplesPerBlockExpected));
            preparedRenderChannels = canonicalBuffer.getNumChannels();
        }
        else
        {
            canonicalBuffer.setSize(0, 0);
            if (diagnosticChannels > 0) preparedRenderChannels = diagnosticChannels;
            else if (engine.deviceManager && engine.deviceManager->getCurrentAudioDevice())
                preparedRenderChannels = engine.deviceManager->getCurrentAudioDevice()->getActiveOutputChannels().countNumberOfSetBits();
        }
        if (engine.playbackRateSource) engine.playbackRateSource->setRenderChannelCount(preparedRenderChannels);
        transport.prepareToPlay(samplesPerBlockExpected, sampleRate);
        if (engine.playbackRateSource && engine.playbackRateSource->snapshot().mode == PlaybackRateSource::Mode::unavailable)
            engine.deviceRestoreUnavailable.store(true, std::memory_order_release);
    }

    int preparedRenderChannels = 2;
    int diagnosticChannels = 0; // only the device-free diagnostic renderer sets this

    void startOutputMeter()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        auto request = OptionalLifecycleToken::request(engine.optionalLifecycleContext,
            OptionalLifecycleOwner::outputMeter, OptionalLifecycleResourceKind::meterState);
        try
        {
#endif
        auto prepared = std::make_unique<MeterState>();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        request.created();
        optionalMeterLifecycleToken = std::move(request);
#endif
        const juce::SpinLock::ScopedLockType lock(meterLock);
        meter = std::move(prepared);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        }
        catch (...)
        {
            request.failed();
            throw;
        }
#endif
    }

    void stopOutputMeter() noexcept
    {
        std::unique_ptr<MeterState> retired;
        {
            const juce::SpinLock::ScopedLockType lock(meterLock);
            retired = std::move(meter);
            engine.publishOutputMeterSnapshot({});
        }
        // Filters are destroyed by the control thread, never the audio callback.
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        retired.reset();
        optionalMeterLifecycleToken.release();
#endif
    }

    void releaseResources() override
    {
        transport.releaseResources();
    }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        const auto spatialRevision = engine.spatialEnergy.beginBlock();
        const auto wasPlaying = transport.isPlaying();
        const auto plan = engine.deviceRoutePlan.load(std::memory_order_acquire);
        if (!plan)
            transport.getNextAudioBlock(bufferToFill);
        else if (bufferToFill.buffer && bufferToFill.numSamples <= canonicalBuffer.getNumSamples()
            && canonicalBuffer.getNumChannels() >= plan->canonicalLayout().sourceChannelCount
            && bufferToFill.buffer->getNumChannels() == static_cast<int>(plan->device().physicalOutputs.size()))
        {
            transport.getNextAudioBlock({ &canonicalBuffer, 0, bufferToFill.numSamples });
            std::array<float*, DeviceRoutePlan::maxBusChannels> outputs {};
            for (int channel = 0; channel < bufferToFill.buffer->getNumChannels(); ++channel)
                outputs[static_cast<std::size_t>(channel)] = bufferToFill.buffer->getWritePointer(channel, bufferToFill.startSample);
            // The transport has finished its common-timebase processing. Only
            // the explicit unity connection matrix touches physical slots here.
            plan->render(canonicalBuffer.getArrayOfReadPointers(), plan->canonicalLayout().sourceChannelCount,
                outputs.data(), bufferToFill.buffer->getNumChannels(), bufferToFill.numSamples);
        }
        else
        {
            bufferToFill.clearActiveBufferRegion();
            engine.deviceRestoreUnavailable.store(true, std::memory_order_release);
        }
        if (engine.playbackRateSource && engine.playbackRateSource->snapshot().mode == PlaybackRateSource::Mode::unavailable)
            engine.deviceRestoreUnavailable.store(true, std::memory_order_release);
        if (engine.deviceRestoreUnavailable.load(std::memory_order_acquire))
            bufferToFill.clearActiveBufferRegion();
        renderSpeakerPing(bufferToFill);
        auto spatialTransport = wasPlaying ? SpatialEnergyMeter::Transport::running
            : playbackState.load(std::memory_order_relaxed) == AudioEngine::PlaybackState::paused
                ? SpatialEnergyMeter::Transport::paused : SpatialEnergyMeter::Transport::stopped;
        if (engine.deviceLeased.load(std::memory_order_acquire) || engine.deviceRestoreUnavailable.load(std::memory_order_acquire))
            spatialTransport = SpatialEnergyMeter::Transport::unavailable;
        engine.spatialEnergy.process(bufferToFill, plan.get(), currentSampleRate, spatialRevision, spatialTransport);
        updateOutputMeter(bufferToFill);

        const auto current = transport.getCurrentPosition();

        // The last callback may acknowledge a switch fade-out at a loop or
        // preview boundary. A newly attached callback also runs before the
        // owner's timebase/seek handoff finishes. Neither may overwrite the
        // latest user position or restart the transport during that handoff.
        if (engine.deviceLeased.load(std::memory_order_acquire)) return;
        currentPositionSeconds.store(current, std::memory_order_relaxed);

        const auto audioSuitePreview = audioSuitePreviewState.load(std::memory_order_acquire);
        if (wasPlaying
            && audioSuitePreview != nullptr
            && audioSuitePreview->isActive())
        {
            const auto previewDocument = engine.document.getState();
            if (audioSuitePreview->plan->matchesState(
                    previewDocument, audioSuitePreview->plan->getDocumentGeneration(), false))
            {
                // Plans use source samples; transport positions use seconds. The
                // device rate may differ after resampling and cannot convert this range.
                const auto sourceRate = juce::jmax(1.0, previewDocument->info.sampleRate);
                const auto previewRange = audioSuitePreview->plan->getPreviewRange();
                const auto previewStart = static_cast<double>(previewRange.getStart()) / sourceRate;
                const auto previewEnd = static_cast<double>(previewRange.getEnd()) / sourceRate;
                if (current >= previewEnd)
                {
                    const SpatialEnergyService::Change spatialChange(engine.spatialEnergy);
                    audioSuitePreview->resetRenderSession();
                    transport.setPosition(previewStart);
                    transport.start();
                    currentPositionSeconds.store(previewStart, std::memory_order_relaxed);
                    playbackState.store(AudioEngine::PlaybackState::playing,
                                        std::memory_order_relaxed);
                    return;
                }
            }
        }

        const auto knownDuration = durationSeconds.load(std::memory_order_relaxed);
        if (wasPlaying && !transport.isPlaying() && knownDuration > 0.0 && current >= knownDuration - 0.02)
        {
            if (loopEnabled.load(std::memory_order_relaxed))
            {
                const SpatialEnergyService::Change spatialChange(engine.spatialEnergy);
                const auto loopStart = juce::jlimit(0.0, knownDuration, playStartSeconds.load(std::memory_order_relaxed));
                transport.setPosition(loopStart);
                transport.start();
                currentPositionSeconds.store(loopStart, std::memory_order_relaxed);
                playbackState.store(AudioEngine::PlaybackState::playing, std::memory_order_relaxed);
                return;
            }

            playbackState.store(AudioEngine::PlaybackState::stopped, std::memory_order_relaxed);
        }
    }

private:
    static bool roleIsLfe(SpeakerRole role) noexcept
    {
        return role == SpeakerRole::LFE;
    }

    void renderSpeakerPing(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        auto remaining = speakerPingSamplesRemaining.load(std::memory_order_relaxed);
        if (remaining <= 0 || bufferToFill.buffer == nullptr)
            return;
        if (engine.deviceRestoreUnavailable.load(std::memory_order_acquire)) return;

        const auto role = static_cast<SpeakerRole>(speakerPingRoleValue.load(std::memory_order_relaxed));
        if (role == SpeakerRole::unknown || role == SpeakerRole::discrete)
            return;

        const auto outputChannels = juce::jmin(bufferToFill.buffer->getNumChannels(), RoutingMatrix::maxChannels);
        if (outputChannels <= 0)
            return;

        RoutingMatrix pingRouting;
        if (const auto plan = engine.deviceRoutePlan.load(std::memory_order_acquire))
        {
            pingRouting.sourceChannels = 1;
            pingRouting.outputChannels = outputChannels;
            for (int slot = 0; slot < outputChannels; ++slot)
                for (int row = 0; row < static_cast<int>(plan->bus().speakers.size()); ++row)
                    if (plan->bus().speakers[static_cast<std::size_t>(row)].role == role && plan->connected(row, slot))
                        pingRouting.gain(slot, 0) = 1.0f;
        }
        else
        {
            ChannelLayoutState sourceLayout;
            sourceLayout.sourceChannelCount = 1;
            sourceLayout.selectedRoles = { role };
            auto targetLayout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(outputChannels);
            if (static_cast<MonitorMode>(monitorModeValue.load(std::memory_order_relaxed)) == MonitorMode::forceStereoDownmix)
                targetLayout = ChannelLayoutCatalog::stereoOutputLayout();
            targetLayout.sourceChannelCount = juce::jmin(outputChannels, targetLayout.sourceChannelCount);
            ChannelPlaybackState pingState;
            pingState.includeLfeInDownmix = true;
            if (outputMappingEnabled.load(std::memory_order_acquire))
            {
                pingState.outputMapping.enabled = true;
                for (int roleIndex = 0; roleIndex < ChannelOutputMapping::maxRoleSlots; ++roleIndex)
                    pingState.outputMapping.outputChannelByRole[static_cast<std::size_t>(roleIndex)] =
                        outputChannelBySpeakerRole[static_cast<std::size_t>(roleIndex)].load(std::memory_order_relaxed);
            }
            pingRouting = ChannelRouter::buildRoutingMatrix(sourceLayout, targetLayout, pingState);
        }

        if (remaining >= speakerPingTotalSamples)
        {
            pingPhase = 0.0;
            pingNoiseState = 0x12345678u;
            pingNoiseLowpass = 0.0f;
        }

        const auto frequency = roleIsLfe(role) ? 80.0 : 900.0;
        const auto phaseStep = (juce::MathConstants<double>::twoPi * frequency) / currentSampleRate;
        for (int sample = 0; sample < bufferToFill.numSamples && remaining > 0; ++sample, --remaining)
        {
            const auto fade = remaining < 384 ? static_cast<float>(remaining) / 384.0f : 1.0f;
            float pingSample = 0.0f;
            if (roleIsLfe(role))
            {
                pingSample = static_cast<float>(std::sin(pingPhase)) * 0.08f * fade;
                pingPhase += phaseStep;
                if (pingPhase >= juce::MathConstants<double>::twoPi)
                    pingPhase -= juce::MathConstants<double>::twoPi;
            }
            else
            {
                pingNoiseState = (1664525u * pingNoiseState) + 1013904223u;
                const auto white = (static_cast<float>((pingNoiseState >> 8) & 0x00ffffffu) / 8388607.5f) - 1.0f;
                pingNoiseLowpass = (pingNoiseLowpass * 0.92f) + (white * 0.08f);
                pingSample = pingNoiseLowpass * 0.055f * fade;
            }

            for (int channel = 0; channel < outputChannels; ++channel)
            {
                const auto gain = pingRouting.gain(channel, 0);
                if (gain == 0.0f)
                    continue;

                const auto index = bufferToFill.startSample + sample;
                bufferToFill.buffer->addSample(channel, index, pingSample * gain);
            }
        }

        speakerPingSamplesRemaining.store(remaining, std::memory_order_relaxed);
    }

    void updateOutputMeter(const juce::AudioSourceChannelInfo& bufferToFill)
    {
        // A view can retire the producer while audio continues. Never wait on
        // the control thread or allocate a producer in the audio callback.
        const juce::SpinLock::ScopedTryLockType lock(meterLock);
        if (!lock.isLocked() || !meter || bufferToFill.buffer == nullptr)
            return;

        const auto outputChannels = juce::jmin(bufferToFill.buffer->getNumChannels(),
                                                OutputMeterSnapshot::maxChannels);
        std::array<SpeakerRole, OutputMeterSnapshot::maxChannels> roles {};
        const auto plan = engine.deviceRoutePlan.load(std::memory_order_acquire);
        if (!plan)
        {
            const auto targetLayout = ChannelLayoutCatalog::deviceDefaultLayoutForOutputChannels(outputChannels);
            for (int channel = 0; channel < juce::jmin(outputChannels, static_cast<int>(targetLayout.selectedRoles.size())); ++channel)
                roles[static_cast<std::size_t>(channel)] = targetLayout.selectedRoles[static_cast<std::size_t>(channel)];
            if (outputMappingEnabled.load(std::memory_order_acquire))
            {
                for (int roleIndex = 1; roleIndex < ChannelOutputMapping::maxRoleSlots; ++roleIndex)
                {
                    const auto outputChannel = outputChannelBySpeakerRole[static_cast<std::size_t>(roleIndex)].load(std::memory_order_relaxed);
                    if (juce::isPositiveAndBelow(outputChannel, outputChannels))
                        roles[static_cast<std::size_t>(outputChannel)] = static_cast<SpeakerRole>(roleIndex);
                }
            }
        }
        else
        {
            roles.fill(SpeakerRole::unknown);
            for (int slot = 0; slot < outputChannels; ++slot)
            {
                int contributors = 0;
                for (int row = 0; row < static_cast<int>(plan->bus().speakers.size()); ++row)
                    if (plan->connected(row, slot))
                    {
                        ++contributors;
                        roles[static_cast<std::size_t>(slot)] = plan->bus().speakers[static_cast<std::size_t>(row)].role;
                    }
                if (contributors != 1) roles[static_cast<std::size_t>(slot)] = SpeakerRole::unknown;
            }
        }

        if (meter->preparation != meterPreparation || meter->channels != outputChannels || meter->roles != roles)
        {
            meter->analyzer.prepare(currentSampleRate, outputChannels, roles);
            meter->preparation = meterPreparation;
            meter->channels = outputChannels;
            meter->roles = roles;
        }

        const auto snapshot = meter->analyzer.process(*bufferToFill.buffer,
                                                  bufferToFill.startSample,
                                                  bufferToFill.numSamples);
        engine.publishOutputMeterSnapshot(snapshot);
    }

    AudioEngine& engine;
    juce::AudioBuffer<float> canonicalBuffer;
    juce::AudioTransportSource& transport;
    std::atomic<double>& currentPositionSeconds;
    std::atomic<double>& durationSeconds;
    std::atomic<double>& playStartSeconds;
    std::atomic<bool>& loopEnabled;
    std::atomic<AudioEngine::PlaybackState>& playbackState;
    std::atomic<int>& monitorModeValue;
    std::atomic<bool>& outputMappingEnabled;
    std::array<std::atomic<int>, ChannelOutputMapping::maxRoleSlots>& outputChannelBySpeakerRole;
    std::atomic<int>& speakerPingRoleValue;
    std::atomic<int>& speakerPingSamplesRemaining;
    std::atomic<std::shared_ptr<const AudioSuitePreviewState>>& audioSuitePreviewState;
    static constexpr int speakerPingTotalSamples = 12000;
    double currentSampleRate = 48000.0;
    double pingPhase = 0.0;
    juce::uint32 pingNoiseState = 0x12345678u;
    float pingNoiseLowpass = 0.0f;
    struct MeterState
    {
        OutputMeterAnalyzer analyzer;
        std::array<SpeakerRole, OutputMeterSnapshot::maxChannels> roles {};
        int channels = -1;
        std::uint64_t preparation = 0;
    };
    juce::SpinLock meterLock;
    std::unique_ptr<MeterState> meter;
    std::uint64_t meterPreparation = 0;
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OptionalLifecycleToken optionalMeterLifecycleToken;
#endif
};

struct AudioEngine::OutputMeterLeaseControl
{
    juce::CriticalSection lock;
    TransportRenderSource* source = nullptr;
    std::atomic<int> consumers { 0 };
};

AudioEngine::OutputMeterAnalysisLease::OutputMeterAnalysisLease(
    std::shared_ptr<OutputMeterLeaseControl> value) noexcept : control(std::move(value)) {}
AudioEngine::OutputMeterAnalysisLease::OutputMeterAnalysisLease(OutputMeterAnalysisLease&& other) noexcept
    : control(std::move(other.control))
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    , optionalLifecycleToken(std::move(other.optionalLifecycleToken))
#endif
{}
AudioEngine::OutputMeterAnalysisLease& AudioEngine::OutputMeterAnalysisLease::operator=(OutputMeterAnalysisLease&& other) noexcept
{
    if (this != &other) { reset(); control = std::move(other.control); }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (this != &other) optionalLifecycleToken = std::move(other.optionalLifecycleToken);
#endif
    return *this;
}
AudioEngine::OutputMeterAnalysisLease::~OutputMeterAnalysisLease() { reset(); }
void AudioEngine::OutputMeterAnalysisLease::reset() noexcept
{
    if (auto held = std::move(control))
    {
        const juce::ScopedLock lock(held->lock);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const auto lastConsumer = held->source && held->consumers.fetch_sub(1) == 1;
        // This handle relinquished its demand; an engine-invalidated late handle
        // has no producer to touch, but still closes its own original token.
        optionalLifecycleToken.release();
        if (lastConsumer)
#else
        if (held->source && held->consumers.fetch_sub(1) == 1)
#endif
            held->source->stopOutputMeter();
    }
}
AudioEngine::OutputMeterAnalysisLease AudioEngine::acquireOutputMeterAnalysisLease()
{
    const juce::ScopedLock lock(outputMeterLeaseControl->lock);
    if (!outputMeterLeaseControl->source) return {};
    if (outputMeterLeaseControl->consumers.load() == 0)
        outputMeterLeaseControl->source->startOutputMeter();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto request = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::outputMeter, OptionalLifecycleResourceKind::meterConsumer,
        outputMeterLeaseControl->source->outputMeterLifecycleResourceId());
#endif
    outputMeterLeaseControl->consumers.fetch_add(1);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    OutputMeterAnalysisLease lease(outputMeterLeaseControl);
    lease.optionalLifecycleToken = std::move(request);
    lease.optionalLifecycleToken.created();
    return lease;
#else
    return OutputMeterAnalysisLease(outputMeterLeaseControl);
#endif
}
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
void AudioEngine::setOptionalLifecycleContext(OptionalLifecycleContext context) noexcept
{
    const juce::ScopedLock lock(outputMeterLeaseControl->lock);
    optionalLifecycleContext = context;
    if (documentSource) documentSource->setOptionalLifecycleContext(context);
    spatialEnergy.setOptionalLifecycleContext(context);
}
#endif
int AudioEngine::getOutputMeterAnalysisLeaseCountForDiagnostics() const noexcept
{
    return outputMeterLeaseControl->consumers.load();
}

AudioEngine::AudioEngine(SingleTrackDocument& documentToUse)
    : document(documentToUse)
{
    for (auto& outputChannel : outputChannelBySpeakerRole)
        outputChannel.store(-1, std::memory_order_relaxed);
    for (auto& role : outputMeterRoles)
        role.store(static_cast<int>(SpeakerRole::unknown), std::memory_order_relaxed);
    for (auto& contribution : outputMeterKContributionDb)
        contribution.store(-120.0f, std::memory_order_relaxed);

    document.addListener(this);

    documentSource = std::make_unique<DocumentAudioSource>(document,
                                                           deviceRoutePlan,
                                                           deviceRestoreUnavailable,
                                                           monitorGainLinear,
                                                           includeLfeInDownmix,
                                                           monitorModeValue,
                                                           sourceChannelMuted,
                                                           sourceChannelSoloed,
                                                           outputMappingEnabled,
                                                           outputChannelBySpeakerRole,
                                                           selectionPreviewActive,
                                                           selectionPreviewStartSeconds,
                                                           selectionPreviewEndSeconds,
                                                           selectionPreviewGainLinear,
                                                           audioSuitePreviewState);
    ambisonicFieldLeaseControl = std::make_shared<AmbisonicFieldLeaseControl>();
    ambisonicFieldLeaseControl->source = documentSource.get();
    renderSource = std::make_unique<TransportRenderSource>(*this,
                                                           transportSource,
                                                           currentPositionSeconds,
                                                           durationSeconds,
                                                           playStartSeconds,
                                                           loopEnabled,
                                                           playbackState,
                                                           monitorModeValue,
                                                           outputMappingEnabled,
                                                           outputChannelBySpeakerRole,
                                                           speakerPingRoleValue,
                                                           speakerPingSamplesRemaining,
                                                           audioSuitePreviewState);

    outputMeterLeaseControl = std::make_shared<OutputMeterLeaseControl>();
    outputMeterLeaseControl->source = renderSource.get();
    sourcePlayer.setSource(renderSource.get());
    updateFromDocumentState(document.getState());
}

AudioEngine::~AudioEngine()
{
    if (const auto operation = deviceSwitchOperation.lock()) operation->abandon();
    if (deviceLeaseIdentity) deviceLeaseIdentity->ownerAlive.store(false, std::memory_order_release);
    traceShutdown(deviceWarmupTask.isPending()
                      ? "engine.destructor.begin.warmup-in-flight" : "engine.destructor.begin");
    deviceWarmupTask.cancelAndWait();
    traceShutdown("engine.warmup.joined");

    document.removeListener(this);
    {
        const juce::ScopedLock lock(outputMeterLeaseControl->lock);
        outputMeterLeaseControl->source = nullptr;
        outputMeterLeaseControl->consumers.store(0);
    }
    if (ambisonicFieldLeaseControl != nullptr)
    {
        const juce::ScopedLock sl(ambisonicFieldLeaseControl->lock);
        ambisonicFieldLeaseControl->source = nullptr;
    }
    if (audioDeviceReady.load(std::memory_order_acquire) && deviceManager != nullptr)
        deviceManager->removeAudioCallback(&sourcePlayer);
    if (deviceOutput) deviceOutput->detach();
    traceShutdown("engine.callback.removed");
    sourcePlayer.setSource(nullptr);
    transportSource.stop();
    transportSource.setSource(nullptr);
    traceShutdown("engine.transport.detached");
    renderSource.reset();
    playbackRateSource.reset();
    documentSource.reset();
    ambisonicFieldLeaseControl.reset();
    traceShutdown("engine.destructor.body.end");
}

bool AudioEngine::renderDocumentBlockForDiagnostics(int64_t startSample,
                                                    juce::AudioBuffer<float>& destination)
{
    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource()
        || destination.getNumChannels() <= 0 || destination.getNumSamples() <= 0)
    {
        return false;
    }

    destination.clear();
    documentSource->configureBinauralState(state);
    documentSource->prepareToPlay(destination.getNumSamples(), state->info.sampleRate);
    documentSource->setNextReadPosition(juce::jmax<int64_t>(0, startSample));
    juce::AudioSourceChannelInfo block(&destination, 0, destination.getNumSamples());
    documentSource->getNextAudioBlock(block);
    return true;
}

bool AudioEngine::renderTransportBlockForDiagnostics(int64_t startSample,
                                                     juce::AudioBuffer<float>& destination,
                                                     double outputSampleRate)
{
    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource()
        || destination.getNumChannels() <= 0 || destination.getNumSamples() <= 0
        || audioDeviceReady.load(std::memory_order_acquire))
    {
        return false;
    }

    destination.clear();
    documentSource->configureBinauralState(state);
    renderSource->diagnosticChannels = destination.getNumChannels();
    renderSource->prepareToPlay(destination.getNumSamples(),
                               outputSampleRate > 0.0 ? outputSampleRate : state->info.sampleRate);
    transportSource.setPosition(static_cast<double>(juce::jmax<int64_t>(0, startSample))
                                / state->info.sampleRate);
    transportSource.start();
    juce::AudioSourceChannelInfo block(&destination, 0, destination.getNumSamples());
    renderSource->getNextAudioBlock(block);
    transportSource.stop();
    renderSource->releaseResources();
    renderSource->diagnosticChannels = 0;
    return true;
}

int AudioEngine::getTransportChannelCapacityForDiagnostics() const noexcept
{
    return transportChannelCapacity.load(std::memory_order_acquire);
}

bool AudioEngine::isAmbisonicBinauralRendererActive() const noexcept
{
    return documentSource != nullptr && documentSource->isBinauralRendererActive();
}

void AudioEngine::collectRetiredBinauralRenderers()
{
    if (documentSource != nullptr)
        documentSource->collectRetiredBinauralRenderers();
}

#if SINGLE_EDITOR_AUDIO_RESOURCE_TESTING
std::shared_ptr<const void> AudioEngine::retainBinauralRendererForTesting() const
{
    return documentSource != nullptr ? documentSource->retainBinauralRendererForTesting() : nullptr;
}
#endif

bool AudioEngine::hasAmbisonicFieldAnalyzerForDiagnostics() const noexcept
{
    return documentSource != nullptr && documentSource->hasAmbisonicFieldAnalyzer();
}

bool AudioEngine::hasAmbisonicStageGraphForDiagnostics() const noexcept
{
    return documentSource != nullptr && documentSource->hasAmbisonicStageGraph();
}

bool AudioEngine::isCustomAmbisonicDecodeMatrixActiveForDiagnostics() const noexcept
{
    return documentSource != nullptr
        && documentSource->isCustomAmbisonicDecodeMatrixActive();
}

std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
AudioEngine::getAmbisonicDecodeMatrixArtifactForDiagnostics() const noexcept
{
    return documentSource != nullptr
         ? documentSource->getAmbisonicDecodeMatrixArtifact()
         : std::shared_ptr<const AmbisonicDecodeMatrixArtifact> {};
}

std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
AudioEngine::getAmbisonicDecodeMatrixPreviewForDiagnostics() const noexcept
{
    return documentSource != nullptr
         ? documentSource->getAmbisonicDecodeMatrixPreview()
         : std::shared_ptr<const AmbisonicDecodeMatrixArtifact> {};
}

int AudioEngine::getAmbisonicFieldAnalysisLeaseCountForDiagnostics() const noexcept
{
    return documentSource != nullptr
         ? documentSource->getAmbisonicFieldAnalysisLeaseCount()
         : 0;
}

bool AudioEngine::loadFile(const juce::File& file)
{
    return document.loadFile(file);
}

void AudioEngine::play()
{
    if (switchResumePlayback)
    {
        if (hasLoadedFile() && canPlayAtSeconds(getCurrentPositionSeconds()))
        {
            *switchResumePlayback = true;
            playStartSeconds.store(getCurrentPositionSeconds(), std::memory_order_relaxed);
        }
        return;
    }
    if (deviceLeased.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)
        || !hasLoadedFile())
        return;

    ensureAudioDeviceReady();

    if (hasExplicitOutputSelection() && !isSelectedOutputReady()) return;
    const auto duration = getDurationSeconds();
    auto current = getCurrentPositionSeconds();
    if (duration > 0.0 && current >= duration - 0.01)
    {
        current = 0.0;
        transportSource.setPosition(current);
        currentPositionSeconds.store(current, std::memory_order_relaxed);
    }

    if (!canPlayAtSeconds(current))
        return;

    playStartSeconds.store(current, std::memory_order_relaxed);
    const SpatialEnergyService::Change spatialChange(spatialEnergy, getPlaybackState() == PlaybackState::stopped);
    transportSource.start();
    updatePositionFromTransport();
    setPlaybackState(PlaybackState::playing);
}

void AudioEngine::pause()
{
    if (switchResumePlayback) *switchResumePlayback = false;
    if (!hasLoadedFile())
        return;

    transportSource.stop();
    updatePositionFromTransport();
    setPlaybackState(PlaybackState::paused);
}

void AudioEngine::stop()
{
    if (switchResumePlayback) *switchResumePlayback = false;
    const auto current = transportSource.getCurrentPosition();
    transportSource.stop();
    const auto target = shouldReturnToPlayStartOnStop()
                      ? clampSeconds(getPlayStartSeconds(), getDurationSeconds())
                      : clampSeconds(current, getDurationSeconds());
    transportSource.setPosition(target);
    currentPositionSeconds.store(target, std::memory_order_relaxed);
    setPlaybackState(PlaybackState::stopped);
}

void AudioEngine::seekToSeconds(double seconds)
{
    if (!hasLoadedFile())
        return;

    const auto clamped = clampSeconds(seconds, getDurationSeconds());
    const auto state = document.getState();
    if (state->info.sampleRate <= 0.0 || state->info.numSamples <= 0)
        return;

    const auto targetSample = juce::jlimit<int64_t>(0,
                                                    state->info.numSamples,
                                                    static_cast<int64_t>(std::floor(clamped * state->info.sampleRate)));
    const auto playableSamples = state->getPlayableSampleRange();
    const auto isFullyReadyEnd = targetSample == state->info.numSamples
                              && playableSamples.getEnd() == state->info.numSamples;
    if (!isFullyReadyEnd && !playableSamples.contains(targetSample))
        return;

    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    transportSource.setPosition(clamped);
    currentPositionSeconds.store(clamped, std::memory_order_relaxed);

    if (isPlaying() || switchResumePlayback.value_or(false))
        playStartSeconds.store(clamped, std::memory_order_relaxed);
}

void AudioEngine::seekBySeconds(double deltaSeconds)
{
    seekToSeconds(getCurrentPositionSeconds() + deltaSeconds);
}

void AudioEngine::stepByFrames(int deltaFrames)
{
    const auto info = getLoadedFileInfo();
    if (!info.isValid() || info.sampleRate <= 0.0)
        return;

    seekToSeconds(getCurrentPositionSeconds() + (static_cast<double>(deltaFrames) / info.sampleRate));
}

void AudioEngine::setLoopEnabled(bool shouldLoop) noexcept
{
    loopEnabled.store(shouldLoop, std::memory_order_relaxed);
}

bool AudioEngine::isLoopEnabled() const noexcept
{
    return loopEnabled.load(std::memory_order_relaxed);
}

void AudioEngine::setReturnToPlayStartOnStop(bool shouldReturn) noexcept
{
    returnToPlayStartOnStop.store(shouldReturn, std::memory_order_relaxed);
}

bool AudioEngine::shouldReturnToPlayStartOnStop() const noexcept
{
    return returnToPlayStartOnStop.load(std::memory_order_relaxed);
}

void AudioEngine::setMonitorGainLinear(float linearGain) noexcept
{
    monitorGainLinear.store(juce::jlimit(0.0f, 2.0f, linearGain), std::memory_order_relaxed);
}

float AudioEngine::getMonitorGainLinear() const noexcept
{
    return monitorGainLinear.load(std::memory_order_relaxed);
}

void AudioEngine::setIncludeLfeInDownmix(bool shouldInclude) noexcept
{
    includeLfeInDownmix.store(shouldInclude, std::memory_order_relaxed);
}

bool AudioEngine::shouldIncludeLfeInDownmix() const noexcept
{
    return includeLfeInDownmix.load(std::memory_order_relaxed);
}

void AudioEngine::setMonitorMode(MonitorMode mode) noexcept
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    monitorModeValue.store(static_cast<int>(mode), std::memory_order_relaxed);
}

MonitorMode AudioEngine::getMonitorMode() const noexcept
{
    const auto raw = monitorModeValue.load(std::memory_order_relaxed);
    if (raw < static_cast<int>(MonitorMode::autoFollowDevice)
        || raw > static_cast<int>(MonitorMode::forceStereoDownmix))
    {
        return MonitorMode::autoFollowDevice;
    }

    return static_cast<MonitorMode>(raw);
}

void AudioEngine::setSourceChannelMuted(int sourceChannel, bool shouldMute) noexcept
{
    if (juce::isPositiveAndBelow(sourceChannel, static_cast<int>(sourceChannelMuted.size())))
    {
        sourceChannelMuted[static_cast<std::size_t>(sourceChannel)].store(shouldMute, std::memory_order_relaxed);
        if (documentSource != nullptr)
            documentSource->requestAmbisonicFieldReset();
    }
}

bool AudioEngine::isSourceChannelMuted(int sourceChannel) const noexcept
{
    return juce::isPositiveAndBelow(sourceChannel, static_cast<int>(sourceChannelMuted.size()))
        && sourceChannelMuted[static_cast<std::size_t>(sourceChannel)].load(std::memory_order_relaxed);
}

void AudioEngine::setSourceChannelSoloed(int sourceChannel, bool shouldSolo) noexcept
{
    if (juce::isPositiveAndBelow(sourceChannel, static_cast<int>(sourceChannelSoloed.size())))
    {
        sourceChannelSoloed[static_cast<std::size_t>(sourceChannel)].store(shouldSolo, std::memory_order_relaxed);
        if (documentSource != nullptr)
            documentSource->requestAmbisonicFieldReset();
    }
}

bool AudioEngine::isSourceChannelSoloed(int sourceChannel) const noexcept
{
    return juce::isPositiveAndBelow(sourceChannel, static_cast<int>(sourceChannelSoloed.size()))
        && sourceChannelSoloed[static_cast<std::size_t>(sourceChannel)].load(std::memory_order_relaxed);
}

void AudioEngine::clearSourceChannelSoloMute() noexcept
{
    for (auto& muted : sourceChannelMuted)
        muted.store(false, std::memory_order_relaxed);
    for (auto& soloed : sourceChannelSoloed)
        soloed.store(false, std::memory_order_relaxed);
    if (documentSource != nullptr)
        documentSource->requestAmbisonicFieldReset();
}

void AudioEngine::setOutputMapping(const ChannelOutputMapping& mapping) noexcept
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    outputMappingEnabled.store(false, std::memory_order_release);
    for (int roleIndex = 0; roleIndex < ChannelOutputMapping::maxRoleSlots; ++roleIndex)
    {
        outputChannelBySpeakerRole[static_cast<std::size_t>(roleIndex)].store(
            mapping.outputChannelByRole[static_cast<std::size_t>(roleIndex)],
            std::memory_order_relaxed);
    }
    outputMappingEnabled.store(mapping.enabled, std::memory_order_release);
}

ChannelOutputMapping AudioEngine::getOutputMapping() const noexcept
{
    ChannelOutputMapping mapping;
    mapping.enabled = outputMappingEnabled.load(std::memory_order_acquire);
    for (int roleIndex = 0; roleIndex < ChannelOutputMapping::maxRoleSlots; ++roleIndex)
    {
        mapping.outputChannelByRole[static_cast<std::size_t>(roleIndex)] =
            outputChannelBySpeakerRole[static_cast<std::size_t>(roleIndex)].load(
                std::memory_order_relaxed);
    }
    return mapping;
}

void AudioEngine::clearOutputMapping() noexcept
{
    setOutputMapping(ChannelOutputMapping {});
}

void AudioEngine::pingSpeakerRole(SpeakerRole role) noexcept
{
    if (deviceLeased.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)) return;
    speakerPingRoleValue.store(static_cast<int>(role), std::memory_order_relaxed);
    speakerPingSamplesRemaining.store(12000, std::memory_order_relaxed);
    warmUpAudioDeviceAsync(0);
}

float AudioEngine::getOutputPeakLinear() const noexcept
{
    return outputPeakLinear.load(std::memory_order_relaxed);
}

float AudioEngine::getTruePeakLinear() const noexcept
{
    return truePeakLinear.load(std::memory_order_relaxed);
}

float AudioEngine::getOutputRmsLinear() const noexcept
{
    return outputRmsLinear.load(std::memory_order_relaxed);
}

float AudioEngine::getMomentaryLufs() const noexcept
{
    return momentaryLufs.load(std::memory_order_relaxed);
}

OutputMeterSnapshot AudioEngine::getOutputMeterSnapshot() const noexcept
{
    OutputMeterSnapshot snapshot;
    snapshot.channelCount = outputMeterChannelCount.load(std::memory_order_acquire);
    snapshot.momentaryLufs = outputMeterMomentaryLufs.load(std::memory_order_relaxed);
    for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        snapshot.roles[index] = static_cast<SpeakerRole>(outputMeterRoles[index].load(std::memory_order_relaxed));
        snapshot.truePeakLinear[index] = outputMeterTruePeak[index].load(std::memory_order_relaxed);
        snapshot.rmsLinear[index] = outputMeterRms[index].load(std::memory_order_relaxed);
        snapshot.kWeightedContributionDb[index] = outputMeterKContributionDb[index].load(std::memory_order_relaxed);
    }
    return snapshot;
}

AmbisonicFieldSnapshot AudioEngine::getAmbisonicFieldSnapshot() const noexcept
{
    const auto state = document.getState();
    if (state == nullptr || !state->info.channelLayout.isAmbisonic || documentSource == nullptr)
        return {};

    return documentSource->getAmbisonicFieldSnapshot();
}

AudioEngine::AmbisonicFieldAnalysisLease AudioEngine::acquireAmbisonicFieldAnalysisLease()
{
    if (documentSource == nullptr || ambisonicFieldLeaseControl == nullptr)
        return {};

    const juce::ScopedLock sl(ambisonicFieldLeaseControl->lock);
    if (ambisonicFieldLeaseControl->source == nullptr)
        return {};
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    auto lifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
        OptionalLifecycleOwner::ambisonics, OptionalLifecycleResourceKind::ambisonicFieldConsumer);
    try
    {
#endif
    const auto leaseId = documentSource->acquireAmbisonicFieldAnalysisLease();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    AmbisonicFieldAnalysisLease result(ambisonicFieldLeaseControl, leaseId);
    lifetime.created();
    result.optionalLifecycleToken = std::move(lifetime);
    return result;
    }
    catch (...)
    {
        lifetime.failed();
        throw;
    }
#else
    return { ambisonicFieldLeaseControl, leaseId };
#endif
}

SpatialEnergyService::Lease AudioEngine::acquireSpatialEnergyAnalysisLease(bool postCoreReady)
{
    auto lease = spatialEnergy.acquire(postCoreReady && hasLoadedFile());
    if (lease) refreshSpatialEnergyAnalysis();
    return lease;
}

void AudioEngine::refreshSpatialEnergyAnalysis()
{
    if (!spatialEnergy.hasConsumers()) return;
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked()) return; // temporary owner transaction, retry on the visible poll
    SpatialEnergyService::Configuration next;
    if (!deviceLeased.load(std::memory_order_acquire) && !deviceRestoreUnavailable.load(std::memory_order_acquire))
    {
        if (deviceOutput && deviceOutput->isOpen() && !deviceOutput->outputSuspended()) next.device = deviceOutput->snapshot();
        else if (!deviceOutput && deviceManager)
        {
            // Inspect only the already open endpoint. No warmup, enumeration or
            // inferred speaker roles. Legacy unmapped outputs remain discrete.
            if (auto* device = deviceManager->getCurrentAudioDevice(); device && device->isOpen())
            {
                DeviceOutput::Snapshot actual;
                actual.backend = deviceManager->getCurrentAudioDeviceType().toStdString();
                actual.stableId = device->getName().toStdString();
                actual.sampleRate = device->getCurrentSampleRate();
                const auto channels = device->getActiveOutputChannels();
                for (int channel = 0; channel <= channels.getHighestBit(); ++channel)
                    if (channels[channel]) actual.physicalOutputs.push_back(channel);
                for (const auto& name : device->getOutputChannelNames()) actual.outputNames.push_back(name.toStdString());
                next.device = std::move(actual);
            }
        }
        next.route = deviceRoutePlan.load(std::memory_order_acquire);
        auto artifact = getAmbisonicDecodeMatrixPreviewForDiagnostics();
        if (!artifact) artifact = getAmbisonicDecodeMatrixArtifactForDiagnostics();
        if (artifact) next.geometry = std::shared_ptr<const SpeakerGeometryProfile>(artifact, &artifact->geometry);
    }
    spatialEnergy.configure(std::move(next));
}

bool AudioEngine::setAmbisonicOrientationPreview(AmbisonicOrientation orientation)
{
    return documentSource != nullptr
        && documentSource->setAmbisonicOrientationPreview(orientation);
}

void AudioEngine::clearAmbisonicOrientationPreview() noexcept
{
    if (documentSource != nullptr)
        documentSource->clearAmbisonicOrientationPreview();
}

bool AudioEngine::activateAmbisonicDecodeMatrixArtifact(
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact)
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    return documentSource != nullptr
        && documentSource->activateAmbisonicDecodeMatrixArtifact(std::move(artifact));
}

void AudioEngine::clearAmbisonicDecodeMatrixArtifact() noexcept
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    if (documentSource != nullptr)
        documentSource->clearAmbisonicDecodeMatrixArtifact();
}

bool AudioEngine::previewAmbisonicDecodeMatrixArtifact(
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact)
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    return documentSource != nullptr
        && documentSource->previewAmbisonicDecodeMatrixArtifact(std::move(artifact));
}

void AudioEngine::clearAmbisonicDecodeMatrixPreview() noexcept
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy);
    if (documentSource != nullptr)
        documentSource->clearAmbisonicDecodeMatrixPreview();
}

void AudioEngine::publishOutputMeterSnapshot(const OutputMeterSnapshot& snapshot) noexcept
{
    auto aggregateTruePeak = 0.0f;
    auto aggregateMeanSquare = 0.0;
    for (int channel = 0; channel < OutputMeterSnapshot::maxChannels; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        outputMeterRoles[index].store(static_cast<int>(snapshot.roles[index]), std::memory_order_relaxed);
        outputMeterTruePeak[index].store(snapshot.truePeakLinear[index], std::memory_order_relaxed);
        outputMeterRms[index].store(snapshot.rmsLinear[index], std::memory_order_relaxed);
        outputMeterKContributionDb[index].store(snapshot.kWeightedContributionDb[index], std::memory_order_relaxed);
        if (channel < snapshot.channelCount)
        {
            aggregateTruePeak = juce::jmax(aggregateTruePeak, snapshot.truePeakLinear[index]);
            aggregateMeanSquare += static_cast<double>(snapshot.rmsLinear[index]) * snapshot.rmsLinear[index];
        }
    }

    outputMeterMomentaryLufs.store(snapshot.momentaryLufs, std::memory_order_relaxed);
    outputMeterChannelCount.store(snapshot.channelCount, std::memory_order_release);
    outputPeakLinear.store(aggregateTruePeak, std::memory_order_relaxed);
    truePeakLinear.store(aggregateTruePeak, std::memory_order_relaxed);
    outputRmsLinear.store(snapshot.channelCount > 0
                            ? static_cast<float>(std::sqrt(aggregateMeanSquare / snapshot.channelCount))
                            : 0.0f,
                          std::memory_order_relaxed);
    momentaryLufs.store(snapshot.momentaryLufs, std::memory_order_relaxed);
}

void AudioEngine::setSelectionPreview(const std::optional<juce::Range<double>>& selectionSeconds, float linearGain) noexcept
{
    if (!selectionSeconds.has_value())
    {
        clearSelectionPreview();
        return;
    }

    selectionPreviewStartSeconds.store(selectionSeconds->getStart(), std::memory_order_relaxed);
    selectionPreviewEndSeconds.store(selectionSeconds->getEnd(), std::memory_order_relaxed);
    selectionPreviewGainLinear.store(linearGain, std::memory_order_relaxed);
    selectionPreviewActive.store(true, std::memory_order_relaxed);
}

void AudioEngine::clearSelectionPreview() noexcept
{
    selectionPreviewActive.store(false, std::memory_order_relaxed);
    selectionPreviewGainLinear.store(1.0f, std::memory_order_relaxed);
}

void AudioEngine::setAudioSuitePreview(
    std::shared_ptr<const AudioSuitePreviewState> previewState) noexcept
{
    clearSelectionPreview();
    clearAudioSuitePreview();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const auto observedContext = previewState && previewState->plan
        ? previewState->plan->observedContext() : OptionalLifecycleContext {};
    const auto observedOwner = previewState && previewState->plan
        ? previewState->plan->observedOwner() : OptionalLifecycleOwner::referenceGain;
    const auto observedState = previewState ? previewState->observedResourceId() : 0;
#endif
    audioSuitePreviewState.store(std::move(previewState), std::memory_order_release);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    // Publication is an engine lease, not the shared state's final lifetime.
    audioSuitePreviewPublication.token = OptionalLifecycleToken::publish(observedContext,
        observedOwner, OptionalLifecycleResourceKind::previewPublication, observedState);
#endif
}

void AudioEngine::clearAudioSuitePreview() noexcept
{
    auto previous = audioSuitePreviewState.exchange({}, std::memory_order_acq_rel);
    if (previous != nullptr)
        previous->cancellation.cancel();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    if (previous != nullptr)
        audioSuitePreviewPublication.token.withdraw();
#endif
}

void AudioEngine::warmUpAudioDeviceAsync(int delayMs)
{
    if (hasExplicitOutputSelection() || deviceLeased.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)
        || audioDeviceReady.load(std::memory_order_acquire))
        return;

#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const auto observedWarmupContext = optionalLifecycleContext;
#endif
    deviceWarmupTask.schedule(delayMs, [this]
    {
#if JUCE_WINDOWS
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        ensureAudioDeviceReady();
    }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    , observedWarmupContext
#endif
    );
}

bool AudioEngine::reserveExplicitStartupOutput() noexcept
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || deviceWarmupTask.isPending() || deviceLeased.load(std::memory_order_acquire)
        || audioDeviceReady.load(std::memory_order_acquire) || deviceOutput) return false;
    explicitOutputSelection.store(true, std::memory_order_release);
    return true;
}

bool AudioEngine::isSelectedOutputReady() const noexcept
{
    if (!audioDeviceReady.load(std::memory_order_acquire) || deviceLeased.load(std::memory_order_acquire)
        || deviceRestoreUnavailable.load(std::memory_order_acquire)) return false;
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked()) return false;
    if (deviceOutput) return deviceOutput->isOpen() && !deviceOutput->outputSuspended();
    auto* device = deviceManager ? deviceManager->getCurrentAudioDevice() : nullptr;
    return device && device->isOpen();
}

std::optional<AsioDriverEvents::Snapshot> AudioEngine::readManagedOutputEvents(bool consume, bool counters)
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || deviceLeased.load() || !deviceOutput || !deviceOutput->hasManagedEvents()) return {};
    return deviceOutput->driverEvents(consume, counters);
}
std::optional<DeviceOutput::Snapshot> AudioEngine::activeOutputSnapshot() const
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || deviceLeased.load() || !deviceOutput) return {};
    return deviceOutput->snapshot();
}
bool AudioEngine::refreshManagedOutputLatency()
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || deviceLeased.load() || !deviceOutput || !deviceOutput->hasManagedEvents()) return false;
    return deviceOutput->refreshDriverLatency();
}

bool AudioEngine::hasLoadedFile() const noexcept
{
    const juce::ScopedLock sl(infoLock);
    return loadedFileInfo.isValid();
}

bool AudioEngine::isPlaying() const noexcept
{
    return getPlaybackState() == PlaybackState::playing;
}

bool AudioEngine::isIdleForDiagnosticProbe() const noexcept
{
    if (deviceLeased.load(std::memory_order_acquire) || deviceWarmupTask.isPending() || isPlaying()) return false;
    if (speakerPingSamplesRemaining.load(std::memory_order_relaxed) <= 0) return true;
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked()) return false;
    if (deviceOutput) return !deviceOutput->isOpen();
    // audioDeviceReady records completed initialization, including a failed
    // device open. Inspect the existing device without creating or waiting.
    auto* device = deviceManager ? deviceManager->getCurrentAudioDevice() : nullptr;
    return device == nullptr || !device->isOpen();
}

AudioEngine::PlaybackState AudioEngine::getPlaybackState() const noexcept
{
    return playbackState.load(std::memory_order_relaxed);
}

double AudioEngine::getCurrentPositionSeconds() const noexcept
{
    return currentPositionSeconds.load(std::memory_order_relaxed);
}

double AudioEngine::getDurationSeconds() const noexcept
{
    return durationSeconds.load(std::memory_order_relaxed);
}

double AudioEngine::getPlayStartSeconds() const noexcept
{
    return playStartSeconds.load(std::memory_order_relaxed);
}

juce::Range<int64_t> AudioEngine::getPlayableSampleRange() const noexcept
{
    return document.getPlayableSampleRange();
}

juce::Range<double> AudioEngine::getPlayableTimeRange() const noexcept
{
    return document.getPlayableTimeRange();
}

bool AudioEngine::canPlayAtSeconds(double seconds) const noexcept
{
    const auto state = document.getState();
    if (state->info.sampleRate <= 0.0 || state->info.numSamples <= 0)
        return false;

    const auto sample = static_cast<int64_t>(std::floor(juce::jmax(0.0, seconds) * state->info.sampleRate));
    return state->canPlaySample(sample);
}

AudioEngine::LoadedFileInfo AudioEngine::getLoadedFileInfo() const
{
    const juce::ScopedLock sl(infoLock);
    return loadedFileInfo;
}

juce::AudioDeviceManager* AudioEngine::getDeviceManagerForConfiguration() noexcept
{
    if (deviceLeased.load(std::memory_order_acquire)) return nullptr;
    ensureAudioDeviceReady();
    return deviceManager.get();
}

std::optional<AudioEngine::DeviceProbeConfiguration> AudioEngine::getDeviceProbeConfiguration() const
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || !isIdleForDiagnosticProbe() || !deviceManager) return {};
    auto* device = deviceManager->getCurrentAudioDevice();
    if (!device || !device->isOpen()) return {};
    return DeviceProbeConfiguration { deviceManager->getAudioDeviceSetup(),
        deviceManager->getCurrentAudioDeviceType(), copyWasapiOutputEndpoint(device) };
}

std::unique_ptr<AudioEngine::DeviceLease> AudioEngine::takeDeviceLeaseForProbe(const DeviceProbeConfiguration* expected)
{
    return takeDeviceLease(false, expected);
}

std::unique_ptr<AudioEngine::DeviceLease> AudioEngine::takeDeviceLeaseForSwitch()
{
    return takeDeviceLease(true, nullptr);
}

std::unique_ptr<AudioEngine::DeviceLease> AudioEngine::takeDeviceLease(bool forSwitch, const DeviceProbeConfiguration* expected)
{
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || deviceLeased.load(std::memory_order_acquire)
        || deviceWarmupTask.isPending() || (!forSwitch && !isIdleForDiagnosticProbe()) || deviceWarmupTask.isStopping()
        || (!forSwitch && deviceManager == nullptr)) return {};
    if (expected)
    {
        const auto current = getDeviceProbeConfiguration();
        if (!current || *current != *expected) return {};
    }
    auto lease = std::unique_ptr<DeviceLease>(new DeviceLease());
    lease->forSwitch = forSwitch;
    lease->identity = std::make_shared<DeviceLeaseIdentity>();
    lease->wasReady = audioDeviceReady.load(std::memory_order_acquire);
    if (deviceOutput)
    {
        lease->wasOpen = deviceOutput->isOpen();
        lease->backend = deviceOutput->snapshot().backend;
    }
    if (deviceManager)
    {
        lease->setup = deviceManager->getAudioDeviceSetup();
        lease->backend = deviceManager->getCurrentAudioDeviceType();
    }
    if (auto* device = deviceManager ? deviceManager->getCurrentAudioDevice() : nullptr; device && device->isOpen())
    {
        lease->wasOpen = true;
        lease->endpoint = copyWasapiOutputEndpoint(device);
        lease->deviceName = device->getName();
        lease->inputs = device->getActiveInputChannels();
        lease->outputs = device->getActiveOutputChannels();
        lease->rate = device->getCurrentSampleRate();
        lease->buffer = device->getCurrentBufferSizeSamples();
    }
    deviceLeaseIdentity = lease->identity;
    deviceLeased.store(true, std::memory_order_release);
    if (forSwitch)
    {
        switchResumePlayback = isPlaying();
        // Stop while callbacks are still attached so JUCE can acknowledge its
        // fade-out. Never call play() on restoration: it changes play-start.
        if (deviceOutput && deviceOutput->outputSuspended())
        {
            // A gated ASIO callback cannot acknowledge JUCE's fade-out. Drain
            // it first, then rebind the same source without the one-second wait.
            deviceOutput->detach();
            const auto position = transportSource.getCurrentPosition();
            transportSource.setSource(nullptr);
            if (playbackRateSource)
                transportSource.setSource(playbackRateSource.get(), 0, nullptr, 0.0, transportChannelCapacity.load());
            transportSource.setPosition(position);
        }
        else transportSource.stop();
    }
    // Removing the callback waits for any in-flight callback and releases its
    // resources while the engine is alive. No worker can subsequently call it.
    if (deviceManager) deviceManager->removeAudioCallback(&sourcePlayer);
    if (deviceOutput) deviceOutput->detach();
    speakerPingSamplesRemaining.store(0, std::memory_order_relaxed);
    audioDeviceReady.store(false, std::memory_order_release);
    lease->manager = std::move(deviceManager);
    lease->output = std::move(deviceOutput);
    if (forSwitch)
    {
        updatePositionFromTransport();
        if (isPlaying()) setPlaybackState(PlaybackState::paused);
    }
    return lease;
}

bool AudioEngine::DeviceLease::closeForProbe()
{
    return !forSwitch && closeForTransfer();
}

bool AudioEngine::DeviceLease::closeForSwitch()
{
    return forSwitch && closeForTransfer();
}

bool AudioEngine::DeviceLease::hasExclusiveSwitchWindow() const noexcept
{
    return forSwitch && closed && !restoreAttempted && exclusiveAccess().isValid();
}

bool AudioEngine::DeviceLease::releaseForReplacement()
{
    if (!hasExclusiveSwitchWindow()) return false;
    const auto released = !output || output->releaseForReplacement();
    if (!released) identity->windowOpen.store(false, std::memory_order_release);
    return released;
}

bool AudioEngine::DeviceLease::closeForTransfer()
{
    if (closeAttempted || restoreAttempted) return false;
    closeAttempted = true;
    if (output) { closed = output->closeForSwitch(); identity->windowOpen.store(forSwitch && closed, std::memory_order_release); return closed; }
    if (!manager) { closed = forSwitch; identity->windowOpen.store(forSwitch && closed, std::memory_order_release); return closed; }
    try
    {
        manager->closeAudioDevice();
        closed = manager->getCurrentAudioDevice() == nullptr;
    }
    catch (...) { closed = false; }
    identity->windowOpen.store(forSwitch && closed, std::memory_order_release);
    return closed;
}

bool AudioEngine::DeviceLease::restore(bool mayReopen)
{
    if (restoreAttempted || !closeAttempted) return false;
    restoreAttempted = true;
    identity->windowOpen.store(false, std::memory_order_release);
    if (!ownerAlive() || !mayReopen) return false;
    if (output)
    {
        // An inspection of an unavailable retained owner preserves its absence.
        // Only a new explicit activation or Retry may reopen it.
        restored = forSwitch && !wasOpen ? !output->isOpen() : output->restore();
        return restored;
    }
    if (!manager) { restored = forSwitch && !wasOpen; return restored; }
    // false preserves JUCE's last explicitly selected settings. Reopen only
    // what was actually open, with no default-device fallback or channel growth.
    try
    {
        const auto error = wasOpen ? manager->setAudioDeviceSetup(setup, false) : juce::String();
        auto* device = manager->getCurrentAudioDevice();
        restored = error.isEmpty() && manager->getCurrentAudioDeviceType() == backend
        && manager->getAudioDeviceSetup() == setup
        && (!wasOpen ? device == nullptr : device && device->isOpen()
            && device->getName() == deviceName && device->getTypeName() == backend
            && device->getActiveInputChannels() == inputs && device->getActiveOutputChannels() == outputs
            && device->getCurrentSampleRate() == rate && device->getCurrentBufferSizeSamples() == buffer
            && (endpoint.isEmpty() || copyWasapiOutputEndpoint(device) == endpoint));
    }
    catch (...) { restored = false; }
    if (!restored) { try { manager->closeAudioDevice(); } catch (...) {} }
    return restored;
}

bool AudioEngine::returnDeviceLease(std::unique_ptr<DeviceLease>& lease)
{
    const juce::ScopedLock lock(audioDeviceLock);
    if (!lease || !deviceLeased.load(std::memory_order_acquire) || deviceManager || deviceOutput
        || lease->identity != deviceLeaseIdentity || !lease->ownerAlive() || !lease->restoreAttempted) return false;
    deviceManager = std::move(lease->manager);
    deviceOutput = std::move(lease->output);
    auto restored = lease->restored;
    const auto latestPosition = getCurrentPositionSeconds();
    if (restored && lease->wasReady && lease->wasOpen && deviceOutput)
    {
        restored = deviceOutput->attach(sourcePlayer);
        if (!restored) deviceOutput->closeForSwitch();
    }
    if (restored && lease->wasReady && deviceManager) deviceManager->addAudioCallback(&sourcePlayer);
    // A file may have changed while the device was leased. Its new SRC has no
    // device timebase until attach prepares it; apply the latest seconds only
    // after that preparation, rather than reinterpreting an unprepared frame.
    if (lease->forSwitch && restored && lease->wasReady && lease->wasOpen)
    {
        transportSource.setPosition(latestPosition);
        currentPositionSeconds.store(latestPosition, std::memory_order_relaxed);
    }
    // Failed restoration requires an explicit device setup; playback/warmup
    // cannot silently open a fallback and report the old state as restored.
    audioDeviceReady.store(lease->wasReady, std::memory_order_release);
    deviceRestoreUnavailable.store(!restored || (lease->forSwitch && !lease->wasOpen), std::memory_order_release);
    deviceLeased.store(false, std::memory_order_release);
    deviceLeaseIdentity.reset();
    const auto resume = lease->forSwitch && restored && lease->wasOpen && switchResumePlayback.value_or(false)
        && hasLoadedFile() && canPlayAtSeconds(getCurrentPositionSeconds());
    switchResumePlayback.reset();
    lease.reset();
    if (resume)
    {
        transportSource.start();
        setPlaybackState(PlaybackState::playing);
    }
    return true;
}

void AudioEngine::rebindTransportForRoute()
{
    const auto state = document.getState();
    if (!state || !state->info.isValid()) return;
    const auto plan = deviceRoutePlan.load(std::memory_order_acquire);
    const auto channels = juce::jmax(state->info.channelLayout.isAmbisonic ? OutputMeterSnapshot::maxChannels : juce::jmax(2, state->info.numChannels),
        plan ? plan->canonicalLayout().sourceChannelCount : 0);
    if (channels == transportChannelCapacity.load(std::memory_order_acquire)) return;
    const auto position = getCurrentPositionSeconds();
    transportSource.setSource(nullptr);
    playbackRateSource = std::make_unique<PlaybackRateSource>(*documentSource, state->info.sampleRate, renderSource->preparedRenderChannels);
    transportSource.setSource(playbackRateSource.get(), 0, nullptr, 0.0, channels);
    transportChannelCapacity.store(channels, std::memory_order_release);
    transportSource.setPosition(position);
}

bool AudioEngine::commitDeviceLease(std::unique_ptr<DeviceLease>& lease, std::unique_ptr<DeviceOutput>& output,
    std::shared_ptr<const DeviceRoutePlan> plan, const DeviceRoutePlan::Bus& currentBus,
    const DeviceRoutePlan::TrustedRoles& trustedRoles)
{
    const juce::ScopedLock lock(audioDeviceLock);
    if (!lease || !lease->hasExclusiveSwitchWindow() || lease->identity != deviceLeaseIdentity
        || !deviceLeased.load(std::memory_order_acquire) || deviceManager || deviceOutput
        || !output || !output->isOpen()) return false;
    const auto& actual = output->snapshot();
    if (!plan || !plan->matches(actual, currentBus, trustedRoles)) return false;
    if (actual.stableId.empty() || actual.backend.empty() || !std::isfinite(actual.sampleRate) || actual.sampleRate <= 0
        || actual.bufferSamples <= 0 || actual.bitDepth <= 0 || actual.outputNames.size() > 512
        || actual.physicalOutputs.empty() || actual.physicalOutputs.size() > 512
        || !std::is_sorted(actual.physicalOutputs.begin(), actual.physicalOutputs.end())
        || std::adjacent_find(actual.physicalOutputs.begin(), actual.physicalOutputs.end()) != actual.physicalOutputs.end()
        || actual.physicalOutputs.front() < 0 || actual.physicalOutputs.back() >= static_cast<int>(actual.outputNames.size())) return false;
    // Both old and new callbacks are detached here. Keep the old immutable plan
    // alive until the new callback is attached or completely drained on failure.
    const auto previousPlan = deviceRoutePlan.exchange(plan, std::memory_order_acq_rel);
    const auto previousUnavailable = deviceRestoreUnavailable.exchange(false, std::memory_order_acq_rel);
    const auto latestPosition = getCurrentPositionSeconds();
    rebindTransportForRoute();
    if (!output->attach(sourcePlayer) || deviceRestoreUnavailable.load(std::memory_order_acquire)
        || !plan->matches(output->snapshot(), currentBus, trustedRoles))
    {
        output->detach();
        deviceRoutePlan.store(previousPlan, std::memory_order_release);
        deviceRestoreUnavailable.store(previousUnavailable, std::memory_order_release);
        rebindTransportForRoute();
        return false;
    }
    transportSource.setPosition(latestPosition);
    currentPositionSeconds.store(latestPosition, std::memory_order_relaxed);
    output->acceptMaintenance();
    deviceOutput = std::move(output);
    audioDeviceReady.store(true, std::memory_order_release);
    deviceLeased.store(false, std::memory_order_release);
    lease->restoreAttempted = true; // retire the old owner: it must never reopen after commit
    lease->identity->windowOpen.store(false, std::memory_order_release);
    deviceLeaseIdentity.reset();
    const auto resume = switchResumePlayback.value_or(false) && hasLoadedFile() && canPlayAtSeconds(getCurrentPositionSeconds());
    switchResumePlayback.reset();
    if (resume) { transportSource.start(); setPlaybackState(PlaybackState::playing); }
    return true;
}

std::optional<AudioEngine::RuntimeMetrics> AudioEngine::getRuntimeMetricsForDiagnostics() const
{
    if (!audioDeviceReady.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)) return {};
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (!lock.isLocked() || (deviceManager == nullptr && !deviceOutput)) return {};
    return readActiveDeviceMetrics();
}

std::optional<AudioEngine::RuntimeMetrics> AudioEngine::readActiveDeviceMetrics() const
{
    if (deviceOutput)
    {
        if (!deviceOutput->isOpen() || deviceRestoreUnavailable.load(std::memory_order_acquire)) return {};
        const auto& actual = deviceOutput->snapshot();
        RuntimeMetrics result;
        result.deviceName = actual.displayName; result.backend = actual.backend;
        result.sampleRate = actual.sampleRate; result.bufferSamples = actual.bufferSamples;
        result.activeOutputChannels = static_cast<int>(actual.physicalOutputs.size());
        result.outputLatencySamples = actual.outputLatencySamples.value_or(-1);
        result.callbackLoadFraction = -1; result.totalXruns = -1; // unavailable until measured by this owner
        if (deviceOutput->hasManagedEvents())
        {
            const auto events = deviceOutput->driverEvents(false, true);
            result.driverXruns = static_cast<int>(std::min<std::uint64_t>(events.xruns, INT_MAX));
            result.totalXruns = result.driverXruns;
            result.driverResets = events.resets;
        }
        readPlaybackMetrics(result);
        if (deviceOutput->outputSuspended())
        { result.aggregateOutputLatencySamples.reset(); result.aggregateOutputLatencyMs.reset(); }
        return result;
    }
    if (deviceManager == nullptr) return {};
    auto* device = deviceManager->getCurrentAudioDevice();
    if (device == nullptr || !device->isOpen()) return {};
    RuntimeMetrics result;
    result.deviceName = device->getName();
    result.backend = device->getTypeName();
    result.sampleRate = device->getCurrentSampleRate();
    result.bufferSamples = device->getCurrentBufferSizeSamples();
    result.activeOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
    result.outputLatencySamples = device->getOutputLatencyInSamples();
    result.callbackLoadFraction = deviceManager->getCpuUsage();
    result.totalXruns = deviceManager->getXRunCount();
    result.driverXruns = device->getXRunCount();
    readPlaybackMetrics(result);
    return result;
}

void AudioEngine::readPlaybackMetrics(RuntimeMetrics& result) const
{
    const auto preview = audioSuitePreviewState.load(std::memory_order_acquire);
    result.audioSuitePreviewConfigured = preview != nullptr && preview->isActive();
    if (result.audioSuitePreviewConfigured)
        result.previewLookaheadSourceSamples = preview->getRequiredLookAheadSamples();
    if (playbackRateSource) result.playbackRate = playbackRateSource->snapshot();
    const auto& rate = result.playbackRate;
    // File SRC and AudioSuite consume future source samples to return the
    // requested timeline immediately. Canonical/physical matrices and the
    // direct binaural FIR add no block delay. Their look-ahead/filter response
    // is not an extra queue between this timeline and the device.
    result.processingLatencySamples = 0;
    if ((rate.mode == PlaybackRateSource::Mode::bypassed || rate.mode == PlaybackRateSource::Mode::converting)
        && std::isfinite(result.sampleRate) && result.sampleRate > 0 && rate.deviceRate == result.sampleRate
        && result.outputLatencySamples >= 0)
    {
        result.aggregateOutputLatencySamples = result.outputLatencySamples + result.processingLatencySamples;
        result.aggregateOutputLatencyMs = 1000.0 * *result.aggregateOutputLatencySamples / result.sampleRate;
    }
}

AudioEngine::DeviceDiagnostics AudioEngine::getDeviceDiagnostics() const
{
    DeviceDiagnostics result;
    const juce::ScopedTryLock lock(audioDeviceLock);
    if (lock.isLocked() && deviceRequests)
    {
        if (const auto requests = deviceRequests->snapshot())
        {
            result.latestRequest = requests->latest;
            result.latestFailure = requests->failure;
        }
        else result.requestObservationBusy = true;
    }
    if (!lock.isLocked() || deviceLeased.load(std::memory_order_acquire))
    {
        if (result.latestRequest && !result.latestRequest->direct)
        {
            result.latestRequest.reset();
            result.latestFailure.reset();
        }
        result.availability = DeviceDiagnostics::Availability::busy;
        result.render.availability = RenderDiagnostics::Availability::busy;
        return result;
    }
    if (deviceOutput)
    {
        result.availability = DeviceDiagnostics::Availability::available;
        result.active = readActiveDeviceMetrics();
        if (result.active)
        {
            auto& mapping = result.outputChannelMapping.emplace();
            for (const auto channel : deviceOutput->snapshot().physicalOutputs) mapping.add(channel);
            result.outputPlan = activeOutputPlan();
            result.activeOutputIdentity = juce::String(deviceOutput->snapshot().stableId);
            result.driverIdentity = deviceOutput->snapshot().driverIdentity;
        }
        // No manager callback lock exists for the direct backend. Until its
        // observation seam is integrated, do not race the render cache.
        result.render.availability = RenderDiagnostics::Availability::notSupported;
        return result;
    }
    if (deviceManager == nullptr)
    {
        result.render = documentSource->renderDiagnostics(document.getState());
        return result;
    }
    result.availability = DeviceDiagnostics::Availability::available;
    result.active = readActiveDeviceMetrics();
    if (result.active)
    {
        auto* device = deviceManager->getCurrentAudioDevice();
        result.outputEndpoint = copyWasapiOutputEndpoint(device);
        result.activeOutputIdentity = result.outputEndpoint;
       #if JUCE_WINDOWS
        result.driverIdentity = copyAsioDriverIdentity(device);
       #endif
        const auto channels = device->getActiveOutputChannels();
        // Match the bounded diagnostics array contract; never publish a
        // truncated map or add observation work to the audio callback.
        if (channels.countNumberOfSetBits() <= 512)
        {
            auto& mapping = result.outputChannelMapping.emplace();
            for (int channel = channels.findNextSetBit(0); channel >= 0; channel = channels.findNextSetBit(channel + 1))
                mapping.add(channel);
        }
    }
    {
        const juce::ScopedTryLock callbackLock(deviceManager->getAudioCallbackLock());
        if (callbackLock.isLocked()) result.render = documentSource->renderDiagnostics(document.getState());
        else result.render.availability = RenderDiagnostics::Availability::busy;
    }
    return result;
}

AudioEngine::DeviceRequestRecord AudioEngine::makeDeviceRequest(DeviceRequestKind kind,
    const juce::AudioDeviceManager::AudioDeviceSetup* setup) const
{
    DeviceRequestRecord result;
    result.operationId = juce::Uuid(); // only an actual device operation creates an ID
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    std::array<std::uint8_t, 16> observedOperation {};
    std::copy_n(result.operationId.getRawData(), observedOperation.size(), observedOperation.begin());
    AudioDeviceWarmupTask::bindCurrentOperation(observedOperation);
#endif
    result.kind = kind;
    result.stage = kind;
    if (deviceManager != nullptr) result.requestedBackend = deviceManager->getCurrentAudioDeviceType().substring(0, 128);
    if (setup != nullptr)
    {
        result.requestedRate = setup->sampleRate;
        result.requestedBuffer = setup->bufferSize;
        result.defaultOutputChannels = setup->useDefaultOutputChannels;
        if (!setup->useDefaultOutputChannels) result.requestedOutputChannels = setup->outputChannels.countNumberOfSetBits();
    }
    return result;
}

std::shared_ptr<DeviceRequestState> AudioEngine::ensureDeviceRequestState()
{
    const juce::ScopedLock lock(audioDeviceLock);
    if (!deviceRequests) deviceRequests = std::make_shared<DeviceRequestState>();
    return deviceRequests;
}

void AudioEngine::finishDeviceRequest(DeviceRequestRecord result, bool rejected, bool stopped)
{
    auto* device = deviceManager != nullptr ? deviceManager->getCurrentAudioDevice() : nullptr;
    result.outcome = stopped ? DeviceRequestOutcome::stopped : rejected ? DeviceRequestOutcome::rejected
        : device == nullptr || !device->isOpen() ? DeviceRequestOutcome::noOpenDevice : DeviceRequestOutcome::succeeded;
    ensureDeviceRequestState()->begin(std::move(result));
}

void AudioEngine::selectAudioDeviceType(const juce::String& type)
{
    if (deviceLeased.load(std::memory_order_acquire)) return;
    ensureAudioDeviceReady();
    const juce::ScopedLock lock(audioDeviceLock);
    if (deviceManager == nullptr) return;
    auto request = makeDeviceRequest(DeviceRequestKind::selectBackend, nullptr);
    request.requestedBackend = type.substring(0, 128);
    deviceManager->setCurrentAudioDeviceType(type, true);
    // JUCE's void API provides no driver error code; preserve only observable rejection/no-open-device.
    request.outcome = deviceManager->getCurrentAudioDeviceType() != type ? DeviceRequestOutcome::rejected : DeviceRequestOutcome::succeeded;
    enableMaximumDeviceOutputs(12, &request);
    const auto rejected = request.outcome == DeviceRequestOutcome::rejected;
    finishDeviceRequest(std::move(request), rejected);
    if (!rejected && readActiveDeviceMetrics())
    {
        if (deviceRestoreUnavailable.exchange(false)) deviceManager->addAudioCallback(&sourcePlayer);
    }
}

juce::String AudioEngine::applyAudioDeviceSetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    if (deviceLeased.load(std::memory_order_acquire)) return "Audio device is temporarily owned by another operation.";
    ensureAudioDeviceReady();
    const juce::ScopedLock lock(audioDeviceLock);
    if (deviceManager == nullptr) return "Audio device manager is unavailable.";
    auto request = makeDeviceRequest(DeviceRequestKind::applySetup, &setup);
    const auto error = deviceManager->setAudioDeviceSetup(setup, true);
    request.outcome = error.isNotEmpty() ? DeviceRequestOutcome::rejected : DeviceRequestOutcome::succeeded;
    enableMaximumDeviceOutputs(12, &request);
    const auto rejected = request.outcome == DeviceRequestOutcome::rejected;
    finishDeviceRequest(std::move(request), rejected);
    if (!rejected && readActiveDeviceMetrics())
    {
        if (deviceRestoreUnavailable.exchange(false)) deviceManager->addAudioCallback(&sourcePlayer);
    }
    return error; // existing Preferences warning stays local; no raw error retained in the capsule
}

int AudioEngine::enableMaximumDeviceOutputs(int channelLimit, DeviceRequestRecord* parent)
{
    auto* device = deviceManager != nullptr ? deviceManager->getCurrentAudioDevice() : nullptr;
    if (device == nullptr) return 0;
    const auto availableChannels = juce::jmin(channelLimit, device->getOutputChannelNames().size());
    if (availableChannels <= 0) return 0;
    auto setup = deviceManager->getAudioDeviceSetup();
    const auto currentEnabled = countEnabledChannels(setup.outputChannels, availableChannels);
    if (currentEnabled >= availableChannels) return currentEnabled;
    juce::BigInteger enabledChannels;
    enabledChannels.setRange(0, availableChannels, true);
    setup.outputChannels = enabledChannels;
    // Preserve the enclosing user request. Its automatic follow-up must not
    // overwrite requested rate/buffer with values already negotiated by the driver.
    auto request = parent == nullptr ? makeDeviceRequest(DeviceRequestKind::enableOutputs, &setup) : DeviceRequestRecord{};
    if (parent != nullptr) parent->followupOutputChannels = availableChannels;
    else request.requestedOutputChannels = availableChannels;
    const auto error = deviceManager->setAudioDeviceSetup(setup, true);
    if (parent == nullptr) finishDeviceRequest(std::move(request), error.isNotEmpty());
    else if (error.isNotEmpty() && parent->outcome != DeviceRequestOutcome::rejected)
    { parent->outcome = DeviceRequestOutcome::rejected; parent->stage = DeviceRequestKind::enableOutputs; }
    return error.isNotEmpty() ? currentEnabled : availableChannels;
}

int AudioEngine::preferMaximumOutputChannelsForCurrentDevice()
{
    if (deviceLeased.load(std::memory_order_acquire)) return 0;
    ensureAudioDeviceReady();
    const juce::ScopedLock sl(audioDeviceLock);
    if (deviceManager == nullptr)
        return 0;

    return enableMaximumDeviceOutputs(12);
}

void AudioEngine::addListener(Listener* listener)
{
    listeners.add(listener);
}

void AudioEngine::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

bool AudioEngine::initialiseAudioDevice()
{
    if (deviceManager == nullptr)
        deviceManager = std::make_unique<SystemAudioDeviceManager>();

    constexpr auto maxOutputChannelsNeeded = 12;
    auto request = makeDeviceRequest(DeviceRequestKind::initialise, nullptr);
    request.requestedOutputChannels = maxOutputChannelsNeeded;
    const auto error = deviceManager->initialise(0, maxOutputChannelsNeeded, nullptr, true, {}, nullptr);
    if (deviceWarmupTask.isStopping())
    {
        finishDeviceRequest(std::move(request), error.isNotEmpty(), true);
        return false;
    }
    request.outcome = error.isNotEmpty() ? DeviceRequestOutcome::rejected : DeviceRequestOutcome::succeeded;
    enableMaximumDeviceOutputs(maxOutputChannelsNeeded, &request);
    const auto rejected = request.outcome == DeviceRequestOutcome::rejected;
    finishDeviceRequest(std::move(request), rejected, deviceWarmupTask.isStopping());
    if (deviceWarmupTask.isStopping())
        return false;
    deviceManager->addAudioCallback(&sourcePlayer);
    return true;
}

void AudioEngine::ensureAudioDeviceReady()
{
    if (hasExplicitOutputSelection() || deviceLeased.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)
        || deviceWarmupTask.isStopping() || audioDeviceReady.load(std::memory_order_acquire))
        return;

    const juce::ScopedLock sl(audioDeviceLock);
    if (hasExplicitOutputSelection() || deviceLeased.load(std::memory_order_acquire) || deviceRestoreUnavailable.load(std::memory_order_acquire)
        || deviceWarmupTask.isStopping() || audioDeviceReady.load(std::memory_order_relaxed))
        return;

    audioDeviceReady.store(initialiseAudioDevice(), std::memory_order_release);
}

void AudioEngine::setPlaybackState(PlaybackState newState)
{
    const auto oldState = playbackState.exchange(newState, std::memory_order_relaxed);
    if (oldState != newState)
        listeners.call([newState](Listener& listener) { listener.playbackStateChanged(newState); });
}

void AudioEngine::updatePositionFromTransport() noexcept
{
    currentPositionSeconds.store(transportSource.getCurrentPosition(), std::memory_order_relaxed);
}

void AudioEngine::updateFromDocumentState(SingleTrackDocument::StatePtr state)
{
    LoadedFileInfo info;
    if (state->info.isValid())
    {
        info.file = state->info.file;
        info.formatName = state->info.formatName;
        info.metadata = state->info.metadata;
        info.durationSeconds = state->info.durationSeconds;
        info.sampleRate = state->info.sampleRate;
        info.numChannels = state->info.numChannels;
        info.bitsPerSample = state->info.bitsPerSample;
        info.audioRevision = state->audioRevision;
        info.channelLayout = state->info.channelLayout;
    }

    LoadedFileInfo previousInfo;
    {
        const juce::ScopedLock sl(infoLock);
        previousInfo = loadedFileInfo;
    }

    const auto sourceBindingChanged = previousInfo.file != info.file
                                   || std::abs(previousInfo.sampleRate - info.sampleRate) > 0.001
                                   || previousInfo.numChannels != info.numChannels
                                   || previousInfo.channelLayout.isAmbisonic != info.channelLayout.isAmbisonic;

    const SpatialEnergyService::Change spatialChange(spatialEnergy, sourceBindingChanged
        || previousInfo.audioRevision != info.audioRevision
        || previousInfo.channelLayout.selectedPresetId != info.channelLayout.selectedPresetId);
    documentSource->configureBinauralState(state);

    if (sourceBindingChanged)
    {
        clearSourceChannelSoloMute();
        transportSource.stop();
        transportSource.setSource(nullptr);
        playbackRateSource.reset();
        transportChannelCapacity.store(0, std::memory_order_release);

        if (state->info.isValid())
        {
            const auto plan = deviceRoutePlan.load(std::memory_order_acquire);
            const auto maximumTransportChannels = juce::jmax(plan ? plan->canonicalLayout().sourceChannelCount : 0, state->info.channelLayout.isAmbisonic
                                                      ? OutputMeterSnapshot::maxChannels
                                                      : juce::jmax(2, state->info.numChannels));
            playbackRateSource = std::make_unique<PlaybackRateSource>(*documentSource, state->info.sampleRate, renderSource->preparedRenderChannels);
            transportSource.setSource(playbackRateSource.get(),
                                      0,
                                      nullptr,
                                      0.0,
                                      maximumTransportChannels);
            transportChannelCapacity.store(maximumTransportChannels, std::memory_order_release);
        }
    }

    if (state->info.isValid())
    {
        const auto clampedPosition = clampSeconds(getCurrentPositionSeconds(), state->info.durationSeconds);
        transportSource.setPosition(clampedPosition);
        currentPositionSeconds.store(clampedPosition, std::memory_order_relaxed);
    }
    else
    {
        currentPositionSeconds.store(0.0, std::memory_order_relaxed);
        playStartSeconds.store(0.0, std::memory_order_relaxed);
        clearSelectionPreview();
        outputPeakLinear.store(0.0f, std::memory_order_relaxed);
        truePeakLinear.store(0.0f, std::memory_order_relaxed);
        outputRmsLinear.store(0.0f, std::memory_order_relaxed);
        momentaryLufs.store(-120.0f, std::memory_order_relaxed);
    }

    {
        const juce::ScopedLock sl(infoLock);
        loadedFileInfo = info;
    }

    durationSeconds.store(info.durationSeconds, std::memory_order_relaxed);
    if (!state->info.isValid())
        setPlaybackState(PlaybackState::stopped);
    else if (!previousInfo.isValid())
        setPlaybackState(PlaybackState::stopped);

    listeners.call([info](Listener& listener) { listener.loadedFileChanged(info); });
}

void AudioEngine::documentChanged(SingleTrackDocument::StatePtr state)
{
    updateFromDocumentState(std::move(state));
}

void AudioEngine::documentIdentityChanged(const media::IdentityEvent& event)
{
    const SpatialEnergyService::Change spatialChange(spatialEnergy, !event.before.document.sameDocument(event.after.document));
    // Reopening the same path can still replace the document. Undo/Redo and
    // Save within the same lineage do not cancel the user's resume intent.
    if (switchResumePlayback && !event.before.document.sameDocument(event.after.document))
        *switchResumePlayback = false;
}

double AudioEngine::clampSeconds(double seconds, double totalDuration) noexcept
{
    if (totalDuration <= 0.0)
        return 0.0;

    return juce::jlimit(0.0, totalDuration, seconds);
}
} // namespace singleeditor
