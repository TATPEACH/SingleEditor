#include "Recovery/RecoveryCheckpointCodec.h"
#include "Recovery/ProgressiveRecoveryAudio.h"
#include "Waveform/WaveformModel.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void good(Status status, const char* message)
{
    if (!status) std::cerr << "Recovery waveform: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
template <typename Predicate> void waitFor(Predicate predicate, const char* message, std::function<void()> explain = {})
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!predicate() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    const bool passed = predicate();
    if (!passed && explain) explain();
    require(passed, message);
}
void checkLeadingPeaks(const WaveformModel::PeakCache& cache, const juce::AudioBuffer<float>& expected, int readyFrames)
{
    require(cache.loadedSampleRange == juce::Range<int64_t>(0, readyFrames)
        && cache.totalSamples == expected.getNumSamples() && cache.channelCount == expected.getNumChannels(),
        "peak metadata describes real prepared frames and original full duration separately");
    for (int channel = 0; channel < cache.channelCount; ++channel)
    {
        std::vector<float> low(static_cast<size_t>(cache.binCount), std::numeric_limits<float>::infinity());
        std::vector<float> high(static_cast<size_t>(cache.binCount), -std::numeric_limits<float>::infinity());
        for (int sample = 0; sample < readyFrames; ++sample)
        {
            const auto bin = std::min(cache.binCount - 1, static_cast<int>(static_cast<int64_t>(sample) * cache.binCount / readyFrames));
            low[static_cast<size_t>(bin)] = std::min(low[static_cast<size_t>(bin)], expected.getSample(channel, sample));
            high[static_cast<size_t>(bin)] = std::max(high[static_cast<size_t>(bin)], expected.getSample(channel, sample));
        }
        const auto& lane = cache.channelBins[static_cast<size_t>(channel)];
        for (int bin = 0; bin < cache.binCount; ++bin)
            require(std::abs(lane[static_cast<size_t>(bin)].minValue - low[static_cast<size_t>(bin)]) < 1.0e-7f
                && std::abs(lane[static_cast<size_t>(bin)].maxValue - high[static_cast<size_t>(bin)]) < 1.0e-7f,
                "native peak bins equal scalar peaks of verified recovery PCM, never foreign path audio");
        require(std::abs(lane.front().startSeconds) < 1.0e-12
            && std::abs(lane.back().endSeconds - readyFrames / cache.sampleRate) < 1.0e-12,
            "prefix peak time extents end at the actual ready boundary");
    }
}
}

void runProgressiveRecoveryWaveformTests(const PreparedRecoveryCheckpoint& prepared,
    const SingleTrackDocument::CommittedStateSnapshot& expected, const RecoveryStore& store, const juce::File& root, AudioGraphLimits limits)
{
    const auto foreign = root.getChildFile("recovery-waveform-foreign.wav");
    const auto scratch = root.getChildFile("recovery-waveform-scratch");
    require(scratch.createDirectory().wasOk(), "create private waveform recovery scratch directory");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(foreign.createOutputStream());
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(expected.state->info.sampleRate)
            .withNumChannels(expected.state->info.numChannels).withBitsPerSample(24));
        require(writer != nullptr, "create a valid but unrelated historical-path WAV");
        juce::AudioBuffer<float> data(expected.state->info.numChannels, static_cast<int>(expected.state->info.numSamples));
        for (int channel = 0; channel < data.getNumChannels(); ++channel)
            juce::FloatVectorOperations::fill(data.getWritePointer(channel), 0.95f, data.getNumSamples());
        require(writer->writeFromAudioSampleBuffer(data, 0, data.getNumSamples()), "write visibly different foreign samples with matching dimensions");
    }
    {
        SingleTrackDocument original;
        require(original.loadFile(foreign), "open private foreign WAV as an ordinary cache fixture");
        WaveformModel originalModel(original);
        waitFor([&]
        {
            // Explicit model-only admission for the foreign-cache fixture;
            // native MainComponent tests own the actual CoreReady prerequisite.
            originalModel.dispatchPendingPeakCachePersistence();
            return WaveformModel::loadPersistentPeakCache(original.getFileInfo()) != nullptr
                && WaveformModel::loadPersistentStartupPreviewForFile(foreign).has_value();
        }, "foreign ordinary peak cache is really persisted");
        require(WaveformModel::ensurePersistentPreviewCache(foreign), "persist independent foreign startup preview");
    }
    const auto foreignPreview = WaveformModel::loadPersistentStartupPreviewForFile(foreign);
    require(foreignPreview && foreignPreview->cache && foreignPreview->cache->channelBins[0][0].maxValue > 0.94f,
        "foreign path has a valid but incorrect cached preview for this recovery");
    {
        std::unique_ptr<ProgressiveRecoveryAudio> source;
        good(ProgressiveRecoveryAudio::create(prepared.getAudioJson(), 0, scratch, source, limits), "create waveform recovery source");
        auto metadata = *prepared.getStates().front(); metadata.info.file = foreign;
        SingleTrackDocument::StatePtr state;
        good(source->bindMetadata(metadata, state), "bind historical locator without reading it");
        require(!WaveformModel::buildLeadingPreviewPeakCacheForState(state, 64, 8.0), "zero-ready recovery cannot borrow a foreign preview");
        good(source->loadNext(store), "prepare first real recovery chunk for peak generation");
        const auto ready = static_cast<int>(source->readyFrames());
        auto leading = WaveformModel::buildLeadingPreviewPeakCacheForState(state, 64, 8.0);
        require(leading && leading->isPreview && ready < state->info.numSamples, "leading preview clamps an eight-second request to actual verified coverage");
        checkLeadingPeaks(*leading, *expected.state->audioBuffer, ready);
        auto dense = WaveformModel::buildLeadingPreviewPeakCacheForState(state, 8192, 8.0);
        require(dense && dense->binCount <= ready, "short verified prefix cannot create empty zero peak columns");
        checkLeadingPeaks(*dense, *expected.state->audioBuffer, ready);
        auto memoryPrefix = std::make_shared<SingleTrackDocument::State>(*expected.state);
        memoryPrefix->info.recoveryBacked = true;
        memoryPrefix->playbackAvailability = SingleTrackDocument::PlaybackAvailability::contiguousPrefix(ready);
        auto memoryLeading = WaveformModel::buildLeadingPreviewPeakCacheForState(memoryPrefix, 8192, 8.0);
        require(memoryLeading && memoryLeading->binCount == dense->binCount,
            "memory and reader preview use the same actual-prefix geometry");
        checkLeadingPeaks(*memoryLeading, *expected.state->audioBuffer, ready);
        require(!WaveformModel::loadPersistentPeakCache(state) && !WaveformModel::loadPersistentPeakCache(state->info)
            && !WaveformModel::loadPersistentSpectrogramCache(state->info) && !WaveformModel::buildPeakCacheSnapshot(state),
            "recovery refuses foreign file caches and incomplete full-waveform snapshots");
        WaveformModel::persistStartupPreviewCache(state->info, *leading);
        const auto unchangedPreview = WaveformModel::loadPersistentStartupPreviewForFile(foreign);
        require(unchangedPreview && unchangedPreview->cache->channelBins[0][0].maxValue == foreignPreview->cache->channelBins[0][0].maxValue,
            "recovery preview cannot overwrite the historical file's cache");

        SingleTrackDocument::CommittedStateSnapshot bound;
        bound.identity = prepared.getIdentity(); bound.identity.source.location.file = foreign; bound.state = state;
        juce::String error;
        auto recovered = SingleTrackDocument::createRecoveredCopy(bound, error);
        require(recovered != nullptr, "create actual recovered Document for background waveform generation");
        WaveformModel model(*recovered);
        waitFor([&] { const auto cache = model.getPeakCache(); return cache && cache->isPreview && cache->loadedSampleRange.getEnd() == ready; },
            "real waveform worker publishes only the prepared recovery range");
        require(model.getPeakCache()->channelBins[0][0].maxValue < 0.9f, "background worker also avoids the foreign persisted cache");
        require(source->loadNext(store, [] { return true; }).error == Error::cancelled
            && !source->getAvailability().canAdvanceContiguousPrefix()
            && source->getAvailability().getPlayableSampleRange(state->info.numSamples).getEnd() == ready,
            "terminal producer seals future coverage but retains existing playable prefix");
        model.ensureSpectrogramReady();
        waitFor([&] { return model.getSpectrogramStatus() == WaveformModel::SpectrogramStatus::failed; },
            "sealed prefix releases the waveform job so a later queued analysis can run and fail honestly on missing coverage",
            [&]
            {
                const auto cache = model.getPeakCache();
                std::cerr << "Recovery waveform timeout: spectrogram_status=" << static_cast<int>(model.getSpectrogramStatus())
                    << " access=" << static_cast<int>(state->playbackAvailability.getAccess())
                    << " can_advance=" << state->playbackAvailability.canAdvanceContiguousPrefix()
                    << " ready=" << state->getPlayableSampleRange().getEnd()
                    << " peak_ready=" << (cache ? cache->loadedSampleRange.getEnd() : -1)
                    << " preview=" << (cache && cache->isPreview) << '\n';
            });
        require(model.getPeakCache() && model.getPeakCache()->isPreview && model.getPeakCache()->loadedSampleRange.getEnd() == ready,
            "terminal recovery retains its last truthful partial waveform");
    }
    require(scratch.getNumberOfChildFiles(juce::File::findFiles) == 0, "waveform workers release their isolated recovery mapping ownership");
    const auto stillForeign = WaveformModel::loadPersistentStartupPreviewForFile(foreign);
    require(stillForeign && stillForeign->cache->channelBins[0][0].maxValue > 0.94f, "ordinary foreign preview remains unchanged after recovery retirement");
    std::cout << "ProgressiveRecoveryWaveformTests passed\n";
}
