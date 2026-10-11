#include "Waveform/WaveformModel.h"
#include "Waveform/SpectrogramMapping.h"
#include "App/ShutdownDiagnostics.h"

#include "Audio/ChannelLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include <juce_dsp/juce_dsp.h>

#ifndef SINGLE_EDITOR_ENABLE_OPEN_TIMING
#define SINGLE_EDITOR_ENABLE_OPEN_TIMING 1
#endif

namespace singleeditor
{
namespace
{
constexpr juce::int64 waveformCacheMagic = 0x5345574650434143;
// Earlier versions could publish private rendered PCM under a source-file key.
// Separate filenames also let retained older candidates keep their own caches.
constexpr int waveformCacheVersion = 3;
constexpr juce::int64 waveformStartupPreviewMagic = 0x5345574650524556;
constexpr int waveformStartupPreviewVersion = 3;
constexpr juce::int64 spectrogramCacheMagic = 0x5345535043414348;
constexpr int spectrogramCacheVersion = 2;
constexpr int minimumPeakLodBinCount = 128;
constexpr int startupPreviewTargetBins = 512;
constexpr double startupPreviewHeadSeconds = 6.0;
constexpr int startupDisplayFullMaxBins = 8192;
constexpr int progressivePeakPreferredSteps = 4;
constexpr double progressivePeakMinChunkSeconds = 2.0;
constexpr double progressivePeakMaxChunkSeconds = 60.0;

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
juce::File getWaveformOpenTimingLogFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SingleEditor");
    directory.createDirectory();
    return directory.getChildFile("wave_cache_debug.log");
}

juce::File getWaveformPortableOpenTimingLogFile()
{
    auto directory = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    if (directory != juce::File())
        return directory.getChildFile("wave_cache_debug.log");

    return {};
}

void appendWaveformTimingLine(const juce::File& targetFile, const juce::String& line)
{
    if (targetFile == juce::File() || line.isEmpty())
        return;

    if (auto parent = targetFile.getParentDirectory(); parent != juce::File())
        parent.createDirectory();

    juce::FileOutputStream stream(targetFile);
    if (!stream.openedOk())
        return;

    if (targetFile.existsAsFile())
        stream.setPosition(targetFile.getSize());

    stream.writeText(line + juce::newLine, false, false, nullptr);
    stream.flush();
}

void logPeakCacheEvent(const juce::String& phase,
                       const SingleTrackDocument::FileInfo& info,
                       const WaveformModel::PeakCache* cache)
{
    if (!info.isValid())
        return;

    juce::String line = "wave-cache | phase=" + phase
                      + " | file=" + info.file.getFullPathName();
    if (cache != nullptr)
    {
        line << " | preview=" << juce::String(cache->isPreview ? 1 : 0)
             << " | bins=" << juce::String(cache->binCount)
             << " | loaded=" << juce::String(cache->loadedSampleRange.getEnd())
             << "/" << juce::String(cache->totalSamples);
    }

    const auto stampedLine = juce::Time::getCurrentTime().toISO8601(true) + " | " + line;
    appendWaveformTimingLine(getWaveformOpenTimingLogFile(), stampedLine);
    appendWaveformTimingLine(getWaveformPortableOpenTimingLogFile(), stampedLine);
}
#endif

int chooseBinCount(int64_t totalSamples)
{
    if (totalSamples <= 0)
        return 0;

    if (totalSamples <= 4096)
        return static_cast<int>(totalSamples);

    const auto adaptive = static_cast<int>(totalSamples / 512);
    return juce::jlimit(2048, 32768, adaptive);
}

int chooseStartupDisplayBinCount(int64_t totalSamples)
{
    const auto fullBinCount = chooseBinCount(totalSamples);
    return fullBinCount > 0 ? juce::jmin(fullBinCount, startupDisplayFullMaxBins) : 0;
}

int chooseSpectrogramWidth(int64_t totalSamples, double durationSeconds)
{
    if (totalSamples <= 0 || durationSeconds <= 0.0)
        return 0;

    const auto adaptive = static_cast<int>(durationSeconds * 72.0);
    return juce::jlimit(360, 1600, adaptive);
}

juce::Colour colourFromSpectralEnergy(float normalised)
{
    normalised = juce::jlimit(0.0f, 1.0f, normalised);

    struct Stop
    {
        float t;
        juce::Colour colour;
    };

    static const std::array<Stop, 5> stops {{
        { 0.0f, juce::Colour(0xff08111d) },
        { 0.22f, juce::Colour(0xff164e63) },
        { 0.48f, juce::Colour(0xff2563eb) },
        { 0.74f, juce::Colour(0xff8b5cf6) },
        { 1.0f, juce::Colour(0xfff97316) },
    }};

    for (std::size_t i = 1; i < stops.size(); ++i)
    {
        if (normalised <= stops[i].t)
        {
            const auto local = juce::jmap(normalised, stops[i - 1].t, stops[i].t, 0.0f, 1.0f);
            return stops[i - 1].colour.interpolatedWith(stops[i].colour, local);
        }
    }

    return stops.back().colour;
}

juce::File getWaveformCacheDirectory()
{
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    if (const auto root = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_NATIVE_TEST_WAVEFORM_CACHE_ROOT", {});
        root.isNotEmpty())
    {
        return juce::File(root);
    }
#endif
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("SingleEditor")
                       .getChildFile("waveform_cache");
    return directory;
}

juce::String makeCacheKey(const juce::File& file)
{
    const auto seed = file.getFullPathName().toLowerCase()
                    + "|"
                    + juce::String(file.getSize())
                    + "|"
                    + juce::String(file.getLastModificationTime().toMilliseconds());
    return juce::String::toHexString(static_cast<juce::int64>(seed.hashCode64()));
}

juce::File getWaveformCacheFile(const SingleTrackDocument::FileInfo& info)
{
    return getWaveformCacheDirectory().getChildFile(makeCacheKey(info.file) + "-pcm3.swfc");
}

juce::File getWaveformCacheFile(const juce::File& file)
{
    return getWaveformCacheDirectory().getChildFile(makeCacheKey(file) + "-pcm3.swfc");
}

juce::File getWaveformStartupPreviewFile(const SingleTrackDocument::FileInfo& info)
{
    return getWaveformCacheDirectory().getChildFile(makeCacheKey(info.file) + "-pcm3.swfp");
}

juce::File getWaveformStartupPreviewFile(const juce::File& file)
{
    return getWaveformCacheDirectory().getChildFile(makeCacheKey(file) + "-pcm3.swfp");
}

juce::File getSpectrogramCacheFile(const juce::File& file)
{
    return getWaveformCacheDirectory().getChildFile(makeCacheKey(file) + "-pcm2.sspc");
}

juce::File getSpectrogramCacheFile(const SingleTrackDocument::FileInfo& info)
{
    return getSpectrogramCacheFile(info.file);
}

struct StateReaderHandle
{
    juce::AudioFormatReader* reader = nullptr;
    std::shared_ptr<juce::MemoryMappedAudioFormatReader> mappedReader;
    std::shared_ptr<juce::AudioFormatReader> isolatedReader;
    std::unique_ptr<juce::AudioFormatReader> ownedReader;
};

bool canUseFileCache(const SingleTrackDocument::StatePtr& state) noexcept
{
    // Both in-memory and private file-backed DSP retain historical FileInfo.
    // A retained old backing after Save also no longer describes the current path.
    return state != nullptr && state->info.isValid() && state->matchesSourceFileAudio
        && state->audioBuffer == nullptr && !state->info.recoveryBacked
        && (!state->sourceFileWitness || !state->sourceFileWitness->retainedInput());
}

StateReaderHandle openReaderForState(const SingleTrackDocument::StatePtr& state)
{
    StateReaderHandle handle;
    if (state == nullptr || !state->info.isValid())
        return handle;

    // A recovery locator is historical provenance, never waveform input or a
    // file-cache key. Its isolated reader supports concurrent immutable reads.
    if (state->info.recoveryBacked)
    {
        handle.isolatedReader = state->streamingReader;
        handle.reader = handle.isolatedReader.get();
        return handle;
    }

    // The declared backing is the only file input for this admitted State.
    // A missing optional cache must not reopen the compressed origin and launch
    // a decoder behind its session policy, nor share the live playback cursor.
    if (state->sourceFileWitness && state->sourceFileWitness->retainedInput())
    {
        handle.ownedReader = SingleTrackDocument::createReaderForRetainedSource(state->sourceFileWitness);
        handle.reader = handle.ownedReader.get();
        return handle;
    }
    const auto sourceFile = state->readerSourceFile != juce::File() ? state->readerSourceFile : state->info.file;
    if (state->readerSourceFile != juce::File() && !sourceFile.existsAsFile()) return handle;
    if (!state->playbackAvailability.isRandomAccess() && sourceFile.hasFileExtension(".wav"))
    {
        juce::WavAudioFormat wavFormat;
        auto stream = std::make_unique<juce::FileInputStream>(sourceFile);
        if (stream->openedOk())
        {
            if (auto* reader = wavFormat.createReaderFor(stream.release(), true))
            {
                handle.ownedReader.reset(reader);
                handle.reader = handle.ownedReader.get();
                return handle;
            }
        }
    }

    auto prepared = SingleTrackDocument::prepareAudioFileForOpen(sourceFile);
    if (prepared.memoryMappedReader != nullptr)
    {
        handle.mappedReader = std::move(prepared.memoryMappedReader);
        handle.reader = handle.mappedReader.get();
        return handle;
    }

    if (prepared.ownedReader != nullptr)
    {
        handle.ownedReader = std::move(prepared.ownedReader);
        handle.reader = handle.ownedReader.get();
        return handle;
    }

    // Keep non-file-backed synthetic states usable, but file-backed playback and
    // waveform workers must never share a mutable reader/mapping.
    if (state->readerSourceFile != juce::File()) return handle;
    handle.reader = state->memoryMappedReader != nullptr ? state->memoryMappedReader.get()
                                                         : state->streamingReader.get();
    return handle;
}

int64_t getStateTotalSamples(const SingleTrackDocument::StatePtr& state) noexcept
{
    if (state == nullptr || !state->info.isValid())
        return 0;

    if (state->audioBuffer != nullptr && state->audioBuffer->getNumSamples() > 0)
        return state->audioBuffer->getNumSamples();

    if (state->memoryMappedReader != nullptr && state->memoryMappedReader->lengthInSamples > 0)
        return state->memoryMappedReader->lengthInSamples;

    if (state->streamingReader != nullptr && state->streamingReader->lengthInSamples > 0)
        return state->streamingReader->lengthInSamples;

    return state->info.numSamples;
}

int getStateChannelCount(const SingleTrackDocument::StatePtr& state) noexcept
{
    if (state == nullptr || !state->info.isValid())
        return 0;

    if (state->audioBuffer != nullptr && state->audioBuffer->getNumChannels() > 0)
        return state->audioBuffer->getNumChannels();

    if (state->memoryMappedReader != nullptr && state->memoryMappedReader->numChannels > 0)
        return static_cast<int>(state->memoryMappedReader->numChannels);

    if (state->streamingReader != nullptr && state->streamingReader->numChannels > 0)
        return static_cast<int>(state->streamingReader->numChannels);

    return state->info.numChannels;
}

bool hasMatchingPersistentCacheHeader(const juce::File& sourceFile,
                                      const juce::File& cacheFile,
                                      juce::int64 expectedMagic,
                                      int expectedVersion)
{
    if (!sourceFile.existsAsFile() || !cacheFile.existsAsFile())
        return false;

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return false;

    if (input.readInt64() != expectedMagic)
        return false;

    if (input.readInt() != expectedVersion)
        return false;

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    return storedPath == sourceFile.getFullPathName()
        && storedSize == sourceFile.getSize()
        && storedModified == sourceFile.getLastModificationTime().toMilliseconds();
}

juce::String inferFormatNameFromFile(const juce::File& file)
{
    auto extension = file.getFileExtension().trimCharactersAtStart(".");
    if (extension.isEmpty())
        return "Audio";

    return extension.toUpperCase();
}

void initialisePeakLevelStorage(std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                std::vector<WaveformModel::PeakBin>& mixedBins,
                                int channelCount,
                                int binCount,
                                bool primeForAccumulation = false)
{
    channelBins.resize(static_cast<std::size_t>(juce::jmax(0, channelCount)));
    for (auto& lane : channelBins)
    {
        lane.resize(static_cast<std::size_t>(juce::jmax(0, binCount)));
        if (primeForAccumulation)
        {
            for (auto& peak : lane)
            {
                peak.minValue = 1.0f;
                peak.maxValue = -1.0f;
                peak.startSeconds = 0.0;
                peak.endSeconds = 0.0;
            }
        }
    }

    mixedBins.resize(static_cast<std::size_t>(juce::jmax(0, binCount)));
    if (primeForAccumulation)
    {
        for (auto& peak : mixedBins)
        {
            peak.minValue = 1.0f;
            peak.maxValue = -1.0f;
            peak.startSeconds = 0.0;
            peak.endSeconds = 0.0;
        }
    }
}

void sanitisePeakLevelBins(std::vector<std::vector<WaveformModel::PeakBin>>& channelBins)
{
    for (auto& lane : channelBins)
    {
        for (auto& peak : lane)
        {
            if (peak.minValue > peak.maxValue)
            {
                peak.minValue = 0.0f;
                peak.maxValue = 0.0f;
            }
        }
    }
}

void fillPeakTimeExtents(std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                         std::vector<WaveformModel::PeakBin>& mixedBins,
                         double sampleRate,
                         juce::Range<int64_t> sampleRange,
                         int binCount)
{
    if (sampleRate <= 0.0 || sampleRange.getLength() <= 0 || binCount <= 0)
        return;

    const auto rangeStart = sampleRange.getStart();
    const auto rangeLength = sampleRange.getLength();
    for (int bin = 0; bin < binCount; ++bin)
    {
        const auto startSample = rangeStart + static_cast<int64_t>((static_cast<double>(bin) / binCount) * rangeLength);
        const auto endSample = juce::jlimit<int64_t>(startSample + 1,
                                                     sampleRange.getEnd(),
                                                     rangeStart + static_cast<int64_t>((static_cast<double>(bin + 1) / binCount) * rangeLength));
        const auto startSeconds = static_cast<double>(startSample) / sampleRate;
        const auto endSeconds = static_cast<double>(endSample) / sampleRate;

        for (auto& lane : channelBins)
        {
            auto& peak = lane[static_cast<std::size_t>(bin)];
            peak.startSeconds = startSeconds;
            peak.endSeconds = endSeconds;
        }

        auto& mixed = mixedBins[static_cast<std::size_t>(bin)];
        mixed.startSeconds = startSeconds;
        mixed.endSeconds = endSeconds;
    }
}

void fillBinTimeExtents(WaveformModel::PeakCache& cache)
{
    fillPeakTimeExtents(cache.channelBins,
                        cache.mixedBins,
                        cache.sampleRate,
                        cache.loadedSampleRange,
                        cache.binCount);
}

void rebuildMixedBins(std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                      std::vector<WaveformModel::PeakBin>& mixedBins,
                      double sampleRate,
                      int64_t totalSamples,
                      int binCount)
{
    mixedBins.resize(static_cast<std::size_t>(juce::jmax(0, binCount)));

    for (int bin = 0; bin < binCount; ++bin)
    {
        float minValue = 1.0f;
        float maxValue = -1.0f;
        for (const auto& lane : channelBins)
        {
            const auto& source = lane[static_cast<std::size_t>(bin)];
            minValue = juce::jmin(minValue, source.minValue);
            maxValue = juce::jmax(maxValue, source.maxValue);
        }

        auto& mixed = mixedBins[static_cast<std::size_t>(bin)];
        mixed.minValue = minValue;
        mixed.maxValue = maxValue;
    }

    fillPeakTimeExtents(channelBins, mixedBins, sampleRate, { 0, totalSamples }, binCount);
}

void rebuildMixedBins(WaveformModel::PeakCache& cache)
{
    rebuildMixedBins(cache.channelBins,
                     cache.mixedBins,
                     cache.sampleRate,
                     cache.totalSamples,
                     cache.binCount);
}

void buildPeakLodPyramid(WaveformModel::PeakCache& cache)
{
    cache.lodLevels.clear();

    auto appendNextLevel = [&](const std::vector<std::vector<WaveformModel::PeakBin>>& sourceChannelBins,
                               int sourceBinCount) -> bool
    {
        if (sourceBinCount <= minimumPeakLodBinCount)
            return false;

        WaveformModel::PeakCache::PeakLodLevel level;
        level.binCount = juce::jmax(1, (sourceBinCount + 1) / 2);
        initialisePeakLevelStorage(level.channelBins,
                                   level.mixedBins,
                                   cache.channelCount,
                                   level.binCount,
                                   true);

        for (int channel = 0; channel < cache.channelCount; ++channel)
        {
            const auto& sourceLane = sourceChannelBins[static_cast<std::size_t>(channel)];
            auto& targetLane = level.channelBins[static_cast<std::size_t>(channel)];
            for (int targetBin = 0; targetBin < level.binCount; ++targetBin)
            {
                const auto sourceStart = targetBin * 2;
                const auto sourceEnd = juce::jmin(sourceBinCount, sourceStart + 2);
                auto& targetPeak = targetLane[static_cast<std::size_t>(targetBin)];
                for (int sourceBin = sourceStart; sourceBin < sourceEnd; ++sourceBin)
                {
                    const auto& sourcePeak = sourceLane[static_cast<std::size_t>(sourceBin)];
                    targetPeak.minValue = juce::jmin(targetPeak.minValue, sourcePeak.minValue);
                    targetPeak.maxValue = juce::jmax(targetPeak.maxValue, sourcePeak.maxValue);
                }
            }
        }

        sanitisePeakLevelBins(level.channelBins);
        rebuildMixedBins(level.channelBins,
                         level.mixedBins,
                         cache.sampleRate,
                         cache.totalSamples,
                         level.binCount);
        cache.lodLevels.push_back(std::move(level));
        return true;
    };

    const auto* currentChannelBins = &cache.channelBins;
    auto currentBinCount = cache.binCount;
    while (appendNextLevel(*currentChannelBins, currentBinCount))
    {
        const auto& currentLevel = cache.lodLevels.back();
        currentChannelBins = &currentLevel.channelBins;
        currentBinCount = currentLevel.binCount;
    }
}

std::shared_ptr<WaveformModel::PeakCache> createStartupPreviewCache(const WaveformModel::PeakCache& source,
                                                                    int targetBinCount = startupPreviewTargetBins)
{
    if (source.channelCount <= 0 || source.channelBins.empty() || source.binCount <= 0)
        return {};

    const std::vector<std::vector<WaveformModel::PeakBin>>* selectedChannelBins = &source.channelBins;
    const std::vector<WaveformModel::PeakBin>* selectedMixedBins = &source.mixedBins;
    int selectedBinCount = source.binCount;

    auto chooseCandidate = [&](const std::vector<std::vector<WaveformModel::PeakBin>>* channelBins,
                               const std::vector<WaveformModel::PeakBin>* mixedBins,
                               int candidateBinCount)
    {
        if (candidateBinCount <= 0)
            return;

        const auto currentOvershoot = selectedBinCount > targetBinCount ? selectedBinCount - targetBinCount : std::numeric_limits<int>::max();
        const auto candidateOvershoot = candidateBinCount > targetBinCount ? candidateBinCount - targetBinCount : std::numeric_limits<int>::max();

        if ((candidateBinCount <= targetBinCount && selectedBinCount > targetBinCount)
            || (candidateBinCount <= targetBinCount && candidateBinCount > selectedBinCount)
            || (candidateBinCount > targetBinCount && candidateOvershoot < currentOvershoot))
        {
            selectedChannelBins = channelBins;
            selectedMixedBins = mixedBins;
            selectedBinCount = candidateBinCount;
        }
    };

    for (const auto& level : source.lodLevels)
        chooseCandidate(&level.channelBins, &level.mixedBins, level.binCount);

    auto preview = std::make_shared<WaveformModel::PeakCache>();
    preview->durationSeconds = source.durationSeconds;
    preview->sampleRate = source.sampleRate;
    preview->totalSamples = source.totalSamples;
    preview->channelCount = source.channelCount;
    preview->binCount = selectedBinCount;
    preview->channelBins = *selectedChannelBins;
    preview->mixedBins = *selectedMixedBins;

    auto loadedEnd = juce::jlimit<int64_t>(0, preview->totalSamples, source.loadedSampleRange.getEnd());
    if (loadedEnd >= preview->totalSamples && preview->sampleRate > 0.0)
    {
        loadedEnd = juce::jlimit<int64_t>(1,
                                          preview->totalSamples,
                                          static_cast<int64_t>(std::ceil(preview->sampleRate * startupPreviewHeadSeconds)));
    }

    preview->loadedSampleRange = { 0, loadedEnd };
    preview->isPreview = loadedEnd < preview->totalSamples;
    return preview;
}

bool writePeakLevel(juce::OutputStream& output,
                    const std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                    int channelCount,
                    int binCount,
                    const std::function<bool()>& cancel = {})
{
    output.writeInt(channelCount);
    output.writeInt(binCount);

    if constexpr (!juce::ByteOrder::isBigEndian())
    {
        std::vector<float> values;
        constexpr int chunkBins = 2048;
        values.reserve(chunkBins * 2);
        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto& lane = channelBins[static_cast<std::size_t>(channel)];
            for (int start = 0; start < binCount; start += chunkBins)
            {
                if (cancel && cancel()) return false;
                values.clear();
                for (int bin = start; bin < juce::jmin(binCount, start + chunkBins); ++bin)
                {
                    const auto& peak = lane[static_cast<std::size_t>(bin)];
                    values.push_back(peak.minValue);
                    values.push_back(peak.maxValue);
                }
                if (!output.write(values.data(), values.size() * sizeof(float))) return false;
            }
        }
        return true;
    }

    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto& lane = channelBins[static_cast<std::size_t>(channel)];
        for (int bin = 0; bin < binCount; ++bin)
        {
            if ((bin & 2047) == 0 && cancel && cancel()) return false;
            const auto& peak = lane[static_cast<std::size_t>(bin)];
            if (!output.writeFloat(peak.minValue) || !output.writeFloat(peak.maxValue)) return false;
        }
    }
    return true;
}

bool readPeakValues(juce::FileInputStream& input,
                    std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                    int channelCount,
                    int binCount)
{
    if constexpr (!juce::ByteOrder::isBigEndian())
    {
        const auto valueCount = static_cast<std::size_t>(channelCount) * static_cast<std::size_t>(binCount) * 2;
        const auto byteCount = valueCount * sizeof(float);
        if (byteCount > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return false;

        std::vector<float> values(valueCount);
        if (byteCount > 0 && input.read(values.data(), static_cast<int>(byteCount)) != static_cast<int>(byteCount))
            return false;

        const auto* cursor = values.data();
        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto& lane = channelBins[static_cast<std::size_t>(channel)];
            for (int bin = 0; bin < binCount; ++bin)
            {
                auto& peak = lane[static_cast<std::size_t>(bin)];
                peak.minValue = *cursor++;
                peak.maxValue = *cursor++;
            }
        }

        return true;
    }

    for (int channel = 0; channel < channelCount; ++channel)
    {
        auto& lane = channelBins[static_cast<std::size_t>(channel)];
        for (int bin = 0; bin < binCount; ++bin)
        {
            auto& peak = lane[static_cast<std::size_t>(bin)];
            peak.minValue = input.readFloat();
            peak.maxValue = input.readFloat();
        }
    }

    return !input.getStatus().failed();
}

bool readPeakLevel(juce::FileInputStream& input,
                   int expectedChannelCount,
                   std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                   std::vector<WaveformModel::PeakBin>& mixedBins,
                   double sampleRate,
                   int64_t totalSamples,
                   int& binCount)
{
    const auto storedChannelCount = input.readInt();
    binCount = input.readInt();
    if (storedChannelCount != expectedChannelCount || binCount <= 0)
        return false;

    initialisePeakLevelStorage(channelBins, mixedBins, storedChannelCount, binCount, false);
    if (!readPeakValues(input, channelBins, storedChannelCount, binCount))
        return false;

    rebuildMixedBins(channelBins, mixedBins, sampleRate, totalSamples, binCount);
    return true;
}

bool readPeakLevelBody(juce::FileInputStream& input,
                       int channelCount,
                       int binCount,
                       std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                       std::vector<WaveformModel::PeakBin>& mixedBins,
                       double sampleRate,
                       int64_t totalSamples)
{
    if (channelCount <= 0 || binCount <= 0)
        return false;

    initialisePeakLevelStorage(channelBins, mixedBins, channelCount, binCount, false);
    if (!readPeakValues(input, channelBins, channelCount, binCount))
        return false;

    rebuildMixedBins(channelBins, mixedBins, sampleRate, totalSamples, binCount);
    return true;
}

bool skipPeakLevelBody(juce::FileInputStream& input, int channelCount, int binCount)
{
    if (channelCount <= 0 || binCount <= 0)
        return false;

    const auto valueCount = static_cast<int64_t>(channelCount) * static_cast<int64_t>(binCount) * 2;
    const auto byteCount = valueCount * static_cast<int64_t>(sizeof(float));
    if (byteCount < 0)
        return false;

    input.setPosition(input.getPosition() + byteCount);
    return !input.getStatus().failed();
}

template <typename ShouldCancel>
bool accumulatePeakBinsFromReader(juce::AudioFormatReader& reader,
                                  std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                  int channelCount,
                                  int binCount,
                                  int64_t totalSamples,
                                  juce::Range<int64_t> loadedSampleRange,
                                  ShouldCancel&& shouldCancel)
{
    if (channelCount <= 0 || binCount <= 0 || totalSamples <= 0 || loadedSampleRange.getLength() <= 0)
        return false;

    const auto loadedSampleStart = loadedSampleRange.getStart();
    const auto loadedSampleLength = loadedSampleRange.getLength();
    constexpr int readChunkSize = 16384;
    juce::AudioBuffer<float> readBuffer(channelCount,
                                        static_cast<int>(juce::jmin<int64_t>(loadedSampleRange.getLength(), readChunkSize)));

    for (int64_t sampleOffset = loadedSampleRange.getStart();
         sampleOffset < loadedSampleRange.getEnd();
         sampleOffset += readBuffer.getNumSamples())
    {
        if (shouldCancel())
            return false;

        const auto samplesThisChunk = static_cast<int>(juce::jmin<int64_t>(readBuffer.getNumSamples(),
                                                                           loadedSampleRange.getEnd() - sampleOffset));
        readBuffer.clear();
        if (!reader.read(&readBuffer,
                         0,
                         samplesThisChunk,
                         sampleOffset,
                         true,
                         channelCount > 1))
        {
            return false;
        }

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto* samples = readBuffer.getReadPointer(channel);
            auto& lane = channelBins[static_cast<std::size_t>(channel)];
            for (int sampleIndex = 0; sampleIndex < samplesThisChunk; ++sampleIndex)
            {
                const auto relativeSample = juce::jlimit<int64_t>(0,
                                                                  loadedSampleLength - 1,
                                                                  (sampleOffset + sampleIndex) - loadedSampleStart);
                const auto bin = juce::jlimit(0,
                                              binCount - 1,
                                              static_cast<int>((static_cast<double>(relativeSample) / loadedSampleLength) * binCount));
                auto& peak = lane[static_cast<std::size_t>(bin)];
                peak.minValue = juce::jmin(peak.minValue, samples[sampleIndex]);
                peak.maxValue = juce::jmax(peak.maxValue, samples[sampleIndex]);
            }
        }
    }

    sanitisePeakLevelBins(channelBins);
    return true;
}

template <typename ShouldCancel>
bool accumulatePeakBinsFromMappedReader(juce::MemoryMappedAudioFormatReader& reader,
                                        std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                        int channelCount,
                                        int binCount,
                                        juce::Range<int64_t> loadedSampleRange,
                                        ShouldCancel&& shouldCancel)
{
    if (channelCount <= 0 || binCount <= 0 || loadedSampleRange.getLength() <= 0)
        return false;

    if (!reader.getMappedSection().contains(loadedSampleRange)
        && !reader.mapSectionOfFile(loadedSampleRange))
    {
        return false;
    }

    std::vector<juce::Range<float>> levels(static_cast<std::size_t>(channelCount));
    const auto loadedSampleStart = loadedSampleRange.getStart();
    const auto loadedSampleEnd = loadedSampleRange.getEnd();
    const auto loadedSampleLength = loadedSampleRange.getLength();

    for (int bin = 0; bin < binCount; ++bin)
    {
        if (shouldCancel())
            return false;

        const auto startSample = loadedSampleStart
            + static_cast<int64_t>((static_cast<double>(bin) / binCount) * loadedSampleLength);
        const auto endSample = juce::jlimit<int64_t>(startSample + 1,
                                                     loadedSampleEnd,
                                                     loadedSampleStart
                                                        + static_cast<int64_t>((static_cast<double>(bin + 1) / binCount)
                                                                               * loadedSampleLength));

        reader.readMaxLevels(startSample, endSample - startSample, levels.data(), channelCount);

        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto& peak = channelBins[static_cast<std::size_t>(channel)][static_cast<std::size_t>(bin)];
            const auto& level = levels[static_cast<std::size_t>(channel)];
            peak.minValue = level.getStart();
            peak.maxValue = level.getEnd();
        }
    }

    sanitisePeakLevelBins(channelBins);
    return true;
}

template <typename ShouldCancel>
bool accumulatePeakBinsFromAudioBufferFullTimeline(const juce::AudioBuffer<float>& buffer,
                                                   std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                                   int channelCount,
                                                   int binCount,
                                                   int64_t totalSamples,
                                                   juce::Range<int64_t> sampleRange,
                                                   ShouldCancel&& shouldCancel)
{
    if (channelCount <= 0 || binCount <= 0 || totalSamples <= 0 || sampleRange.getLength() <= 0)
        return false;

    const auto clampedRange = sampleRange.getIntersectionWith({ 0, totalSamples });
    if (clampedRange.getLength() <= 0)
        return false;

    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel);
        auto& lane = channelBins[static_cast<std::size_t>(channel)];

        for (int64_t sample = clampedRange.getStart(); sample < clampedRange.getEnd(); ++sample)
        {
            if (((sample - clampedRange.getStart()) & 0x0fff) == 0 && shouldCancel())
                return false;

            const auto bin = juce::jlimit(0,
                                          binCount - 1,
                                          static_cast<int>((static_cast<double>(sample) / totalSamples) * binCount));
            auto& peak = lane[static_cast<std::size_t>(bin)];
            peak.minValue = juce::jmin(peak.minValue, samples[static_cast<int>(sample)]);
            peak.maxValue = juce::jmax(peak.maxValue, samples[static_cast<int>(sample)]);
        }
    }

    sanitisePeakLevelBins(channelBins);
    return true;
}

template <typename ShouldCancel>
bool accumulatePeakBinsFromReaderFullTimeline(juce::AudioFormatReader& reader,
                                              std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                              int channelCount,
                                              int binCount,
                                              int64_t totalSamples,
                                              juce::Range<int64_t> sampleRange,
                                              ShouldCancel&& shouldCancel)
{
    if (channelCount <= 0 || binCount <= 0 || totalSamples <= 0 || sampleRange.getLength() <= 0)
        return false;

    const auto clampedRange = sampleRange.getIntersectionWith({ 0, totalSamples });
    if (clampedRange.getLength() <= 0)
        return false;

    constexpr int readChunkSize = 16384;
    juce::AudioBuffer<float> readBuffer(channelCount,
                                        static_cast<int>(juce::jmin<int64_t>(clampedRange.getLength(), readChunkSize)));

    for (int64_t sampleOffset = clampedRange.getStart();
         sampleOffset < clampedRange.getEnd();
         sampleOffset += readBuffer.getNumSamples())
    {
        if (shouldCancel())
            return false;

        const auto samplesThisChunk = static_cast<int>(juce::jmin<int64_t>(readBuffer.getNumSamples(),
                                                                           clampedRange.getEnd() - sampleOffset));
        readBuffer.clear();
        if (!reader.read(&readBuffer,
                         0,
                         samplesThisChunk,
                         sampleOffset,
                         true,
                         channelCount > 1))
        {
            return false;
        }

        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto* samples = readBuffer.getReadPointer(channel);
            auto& lane = channelBins[static_cast<std::size_t>(channel)];
            for (int sampleIndex = 0; sampleIndex < samplesThisChunk; ++sampleIndex)
            {
                const auto absoluteSample = sampleOffset + sampleIndex;
                const auto bin = juce::jlimit(0,
                                              binCount - 1,
                                              static_cast<int>((static_cast<double>(absoluteSample) / totalSamples) * binCount));
                auto& peak = lane[static_cast<std::size_t>(bin)];
                peak.minValue = juce::jmin(peak.minValue, samples[sampleIndex]);
                peak.maxValue = juce::jmax(peak.maxValue, samples[sampleIndex]);
            }
        }
    }

    sanitisePeakLevelBins(channelBins);
    return true;
}

template <typename ShouldCancel>
bool accumulatePeakBinsFromMappedReaderFullTimeline(juce::MemoryMappedAudioFormatReader& reader,
                                                    std::vector<std::vector<WaveformModel::PeakBin>>& channelBins,
                                                    int channelCount,
                                                    int binCount,
                                                    int64_t totalSamples,
                                                    juce::Range<int64_t> sampleRange,
                                                    ShouldCancel&& shouldCancel)
{
    if (channelCount <= 0 || binCount <= 0 || totalSamples <= 0 || sampleRange.getLength() <= 0)
        return false;

    const auto clampedRange = sampleRange.getIntersectionWith({ 0, totalSamples });
    if (clampedRange.getLength() <= 0)
        return false;

    if (!reader.getMappedSection().contains(clampedRange)
        && !reader.mapSectionOfFile(clampedRange))
    {
        return false;
    }

    std::vector<juce::Range<float>> levels(static_cast<std::size_t>(channelCount));
    const auto firstBin = juce::jlimit(0,
                                       binCount - 1,
                                       static_cast<int>((static_cast<double>(clampedRange.getStart()) / totalSamples) * binCount));
    const auto lastSample = juce::jmax<int64_t>(clampedRange.getStart(), clampedRange.getEnd() - 1);
    const auto lastBin = juce::jlimit(0,
                                      binCount - 1,
                                      static_cast<int>((static_cast<double>(lastSample) / totalSamples) * binCount));

    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        if (shouldCancel())
            return false;

        const auto binStartSample = static_cast<int64_t>((static_cast<double>(bin) / binCount) * totalSamples);
        const auto binEndSample = juce::jlimit<int64_t>(binStartSample + 1,
                                                        totalSamples,
                                                        static_cast<int64_t>((static_cast<double>(bin + 1) / binCount) * totalSamples));
        const auto coveredStart = juce::jmax(binStartSample, clampedRange.getStart());
        const auto coveredEnd = juce::jmin(binEndSample, clampedRange.getEnd());
        if (coveredEnd <= coveredStart)
            continue;

        reader.readMaxLevels(coveredStart, coveredEnd - coveredStart, levels.data(), channelCount);
        for (int channel = 0; channel < channelCount; ++channel)
        {
            auto& peak = channelBins[static_cast<std::size_t>(channel)][static_cast<std::size_t>(bin)];
            const auto& level = levels[static_cast<std::size_t>(channel)];
            peak.minValue = juce::jmin(peak.minValue, level.getStart());
            peak.maxValue = juce::jmax(peak.maxValue, level.getEnd());
        }
    }

    sanitisePeakLevelBins(channelBins);
    return true;
}

void persistStartupPreviewToDisk(const SingleTrackDocument::FileInfo& info, const WaveformModel::PeakCache& cache,
                                 const std::function<bool()>& cancel = {},
                                 const std::function<void(juce::TemporaryFile&)>& publish = {})
{
    if (!info.isValid() || cache.channelBins.empty() || (cancel && cancel()))
        return;

    auto previewCache = createStartupPreviewCache(cache);
    if (previewCache == nullptr)
        return;

    auto targetFile = getWaveformStartupPreviewFile(info);
    if ((cancel && cancel()) || targetFile.getParentDirectory().createDirectory().failed()) return;
    juce::TemporaryFile tempFile(targetFile);
    {
        juce::FileOutputStream output(tempFile.getFile());
        if (!output.openedOk())
            return;

        output.writeInt64(waveformStartupPreviewMagic);
        output.writeInt(waveformStartupPreviewVersion);
        output.writeString(info.file.getFullPathName());
        output.writeInt64(info.file.getSize());
        output.writeInt64(info.file.getLastModificationTime().toMilliseconds());
        output.writeDouble(previewCache->durationSeconds);
        output.writeDouble(previewCache->sampleRate);
        output.writeInt64(previewCache->totalSamples);
        output.writeInt64(previewCache->loadedSampleRange.getStart());
        output.writeInt64(previewCache->loadedSampleRange.getEnd());
        output.writeBool(previewCache->isPreview);
        output.writeInt(previewCache->channelCount);
        if (!writePeakLevel(output, previewCache->channelBins, previewCache->channelCount, previewCache->binCount, cancel)) return;
        output.flush();
        if (output.getStatus().failed())
        {
            tempFile.getFile().deleteFile();
            return;
        }
    }

    if (cancel && cancel()) return;
    if (publish) publish(tempFile);
    else tempFile.overwriteTargetFileWithTemporary();
}

void persistPeakCacheToDisk(const SingleTrackDocument::FileInfo& info, const WaveformModel::PeakCache& cache,
                            const std::function<bool()>& cancel,
                            const std::function<void(juce::TemporaryFile&)>& publish)
{
    if (!info.isValid() || cache.channelBins.empty() || cancel())
        return;

    auto targetFile = getWaveformCacheFile(info);
    if (cancel() || targetFile.getParentDirectory().createDirectory().failed()) return;
    juce::TemporaryFile tempFile(targetFile);
    {
        juce::FileOutputStream output(tempFile.getFile());
        if (!output.openedOk())
            return;

        output.writeInt64(waveformCacheMagic);
        output.writeInt(waveformCacheVersion);
        output.writeString(info.file.getFullPathName());
        output.writeInt64(info.file.getSize());
        output.writeInt64(info.file.getLastModificationTime().toMilliseconds());
        output.writeDouble(cache.durationSeconds);
        output.writeDouble(cache.sampleRate);
        output.writeInt64(cache.totalSamples);
        output.writeInt64(cache.loadedSampleRange.getStart());
        output.writeInt64(cache.loadedSampleRange.getEnd());
        output.writeBool(cache.isPreview);
        if (!writePeakLevel(output, cache.channelBins, cache.channelCount, cache.binCount, cancel)) return;
        output.writeInt(static_cast<int>(cache.lodLevels.size()));

        for (const auto& level : cache.lodLevels)
        {
            if (!writePeakLevel(output, level.channelBins, cache.channelCount, level.binCount, cancel)) return;
        }

        output.flush();
        if (output.getStatus().failed())
        {
            tempFile.getFile().deleteFile();
            return;
        }
    }

    if (cancel()) return;
    publish(tempFile);
    persistStartupPreviewToDisk(info, cache, cancel, publish);
}

std::shared_ptr<WaveformModel::PeakCache> loadPersistentDisplayPeakCache(const SingleTrackDocument::FileInfo& info,
                                                                         int targetMaxBins)
{
    if (!info.isValid() || targetMaxBins <= 0)
        return {};

    const auto cacheFile = getWaveformCacheFile(info);
    if (!cacheFile.existsAsFile())
        return {};

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return {};

    if (input.readInt64() != waveformCacheMagic)
        return {};

    if (input.readInt() != waveformCacheVersion)
        return {};

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    const auto durationSeconds = input.readDouble();
    const auto sampleRate = input.readDouble();
    const auto totalSamples = input.readInt64();
    const auto loadedSampleStart = input.readInt64();
    const auto loadedSampleEnd = input.readInt64();
    const auto isPreview = input.readBool();
    const auto fullChannelCount = input.readInt();
    const auto fullBinCount = input.readInt();

    if (storedPath != info.file.getFullPathName()
        || storedSize != info.file.getSize()
        || storedModified != info.file.getLastModificationTime().toMilliseconds()
        || std::abs(durationSeconds - info.durationSeconds) > 0.0001
        || std::abs(sampleRate - info.sampleRate) > 0.0001
        || totalSamples != info.numSamples
        || fullChannelCount != info.numChannels
        || fullBinCount <= 0)
    {
        return {};
    }

    auto createCache = [&](int binCount) -> std::shared_ptr<WaveformModel::PeakCache>
    {
        auto cache = std::make_shared<WaveformModel::PeakCache>();
        cache->audioRevision = 0;
        cache->durationSeconds = durationSeconds;
        cache->sampleRate = sampleRate;
        cache->totalSamples = totalSamples;
        cache->loadedSampleRange = { juce::jlimit<int64_t>(0, totalSamples, loadedSampleStart),
                                     juce::jlimit<int64_t>(0, totalSamples, loadedSampleEnd) };
        cache->channelCount = fullChannelCount;
        cache->isPreview = isPreview && cache->loadedSampleRange.getEnd() < totalSamples;
        cache->binCount = binCount;
        return cache;
    };

    if (fullBinCount <= targetMaxBins)
    {
        auto cache = createCache(fullBinCount);
        if (!readPeakLevelBody(input,
                               fullChannelCount,
                               cache->binCount,
                               cache->channelBins,
                               cache->mixedBins,
                               cache->sampleRate,
                               cache->totalSamples))
        {
            return {};
        }

        buildPeakLodPyramid(*cache);
        return input.getStatus().failed() ? std::shared_ptr<WaveformModel::PeakCache> {} : cache;
    }

    if (!skipPeakLevelBody(input, fullChannelCount, fullBinCount))
        return {};

    const auto lodLevelCount = input.readInt();
    if (lodLevelCount < 0)
        return {};

    for (int levelIndex = 0; levelIndex < lodLevelCount; ++levelIndex)
    {
        const auto levelChannelCount = input.readInt();
        const auto levelBinCount = input.readInt();
        if (levelChannelCount != fullChannelCount || levelBinCount <= 0)
            return {};

        if (levelBinCount > targetMaxBins)
        {
            if (!skipPeakLevelBody(input, levelChannelCount, levelBinCount))
                return {};
            continue;
        }

        auto cache = createCache(levelBinCount);
        if (!readPeakLevelBody(input,
                               levelChannelCount,
                               cache->binCount,
                               cache->channelBins,
                               cache->mixedBins,
                               cache->sampleRate,
                               cache->totalSamples))
        {
            return {};
        }

        buildPeakLodPyramid(*cache);
        return input.getStatus().failed() ? std::shared_ptr<WaveformModel::PeakCache> {} : cache;
    }

    return {};
}

void persistSpectrogramToDisk(const SingleTrackDocument::FileInfo& info, const WaveformModel::SpectrogramCache& cache)
{
    if (!info.isValid() || !cache.image.isValid() || cache.width <= 0 || cache.height <= 0)
        return;

    auto targetFile = getSpectrogramCacheFile(info);
    if (targetFile.getParentDirectory().createDirectory().failed()) return;
    juce::TemporaryFile tempFile(targetFile);
    {
        juce::FileOutputStream output(tempFile.getFile());
        if (!output.openedOk())
            return;

        output.writeInt64(spectrogramCacheMagic);
        output.writeInt(spectrogramCacheVersion);
        output.writeString(info.file.getFullPathName());
        output.writeInt64(info.file.getSize());
        output.writeInt64(info.file.getLastModificationTime().toMilliseconds());
        output.writeDouble(cache.durationSeconds);
        output.writeDouble(cache.sampleRate);
        output.writeInt(cache.width);
        output.writeInt(cache.height);

        juce::Image::BitmapData data(cache.image, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < cache.height; ++y)
            for (int x = 0; x < cache.width; ++x)
                output.writeInt(static_cast<int>(data.getPixelColour(x, y).getARGB()));

        output.flush();
        if (output.getStatus().failed())
        {
            tempFile.getFile().deleteFile();
            return;
        }
    }

    tempFile.overwriteTargetFileWithTemporary();
}

template <typename ShouldCancel>
std::shared_ptr<WaveformModel::PeakCache> buildPeakCacheForState(const SingleTrackDocument::StatePtr& state,
                                                                 int maxBins,
                                                                 bool isPreview,
                                                                 ShouldCancel&& shouldCancel)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return {};

    const auto channelCount = getStateChannelCount(state);
    const auto totalSamples = getStateTotalSamples(state);
    if (channelCount <= 0 || totalSamples <= 0)
        return {};

    auto cache = std::make_shared<WaveformModel::PeakCache>();
    cache->state = state;
    cache->audioRevision = state->audioRevision;
    cache->durationSeconds = state->info.durationSeconds;
    cache->sampleRate = state->info.sampleRate;
    cache->totalSamples = totalSamples;
    cache->loadedSampleRange = { 0, totalSamples };
    cache->channelCount = channelCount;
    cache->binCount = juce::jmax(1, juce::jmin(chooseBinCount(totalSamples), maxBins));
    cache->isPreview = isPreview;
    initialisePeakLevelStorage(cache->channelBins,
                               cache->mixedBins,
                               channelCount,
                               cache->binCount,
                               state->audioBuffer == nullptr);

    if (state->audioBuffer != nullptr)
    {
        const auto& buffer = *state->audioBuffer;
        for (int channel = 0; channel < channelCount; ++channel)
        {
            const auto* data = buffer.getReadPointer(channel);
            auto& lane = cache->channelBins[static_cast<std::size_t>(channel)];

            for (int bin = 0; bin < cache->binCount; ++bin)
            {
                if (shouldCancel())
                    return {};

                const auto startSample = static_cast<int64_t>((static_cast<double>(bin) / cache->binCount) * totalSamples);
                const auto endSample = juce::jlimit<int64_t>(startSample + 1,
                                                             totalSamples,
                                                             static_cast<int64_t>((static_cast<double>(bin + 1) / cache->binCount) * totalSamples));

                float minValue = 1.0f;
                float maxValue = -1.0f;
                for (auto sample = startSample; sample < endSample; ++sample)
                {
                    const auto value = data[sample];
                    minValue = juce::jmin(minValue, value);
                    maxValue = juce::jmax(maxValue, value);
                }

                auto& peakBin = lane[static_cast<std::size_t>(bin)];
                peakBin.minValue = minValue;
                peakBin.maxValue = maxValue;
                peakBin.startSeconds = cache->sampleRate > 0.0 ? static_cast<double>(startSample) / cache->sampleRate : 0.0;
                peakBin.endSeconds = cache->sampleRate > 0.0 ? static_cast<double>(endSample) / cache->sampleRate : peakBin.startSeconds;
            }
        }
    }
    else
    {
        auto readerHandle = openReaderForState(state);
        if (readerHandle.mappedReader != nullptr)
        {
            if (!accumulatePeakBinsFromMappedReader(*readerHandle.mappedReader,
                                                    cache->channelBins,
                                                    cache->channelCount,
                                                    cache->binCount,
                                                    cache->loadedSampleRange,
                                                    std::forward<ShouldCancel>(shouldCancel)))
            {
                return {};
            }
        }
        else
        {
            if (readerHandle.reader == nullptr)
                return {};

            if (!accumulatePeakBinsFromReader(*readerHandle.reader,
                                              cache->channelBins,
                                              cache->channelCount,
                                              cache->binCount,
                                              totalSamples,
                                              cache->loadedSampleRange,
                                              std::forward<ShouldCancel>(shouldCancel)))
            {
                return {};
            }
        }
    }

    rebuildMixedBins(*cache);
    if (!cache->isPreview)
        buildPeakLodPyramid(*cache);

    return cache;
}

template <typename ShouldCancel, typename PublishProgress>
std::shared_ptr<WaveformModel::PeakCache> buildProgressivePeakCacheForState(const SingleTrackDocument::StatePtr& state,
                                                                            int maxBins,
                                                                            juce::Range<int64_t> seedLoadedSampleRange,
                                                                            ShouldCancel&& shouldCancel,
                                                                            PublishProgress&& publishProgress)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return {};

    const auto channelCount = getStateChannelCount(state);
    const auto totalSamples = getStateTotalSamples(state);
    if (channelCount <= 0 || totalSamples <= 0)
        return {};

    auto shouldStop = [&shouldCancel]()
    {
        return shouldCancel();
    };

    auto cache = std::make_shared<WaveformModel::PeakCache>();
    cache->state = state;
    cache->audioRevision = state->audioRevision;
    cache->durationSeconds = state->info.durationSeconds;
    cache->sampleRate = state->info.sampleRate;
    cache->totalSamples = totalSamples;
    cache->loadedSampleRange = {};
    cache->channelCount = channelCount;
    cache->binCount = juce::jmax(1, juce::jmin(chooseBinCount(totalSamples), maxBins));
    cache->isPreview = true;
    initialisePeakLevelStorage(cache->channelBins,
                               cache->mixedBins,
                               channelCount,
                               cache->binCount,
                               true);
    fillPeakTimeExtents(cache->channelBins,
                        cache->mixedBins,
                        cache->sampleRate,
                        { 0, totalSamples },
                        cache->binCount);

    StateReaderHandle readerHandle;
    if (state->audioBuffer == nullptr)
    {
        readerHandle = openReaderForState(state);
        if (readerHandle.reader == nullptr)
            return {};
    }

    auto accumulateRange = [&](juce::Range<int64_t> rangeToAccumulate) -> bool
    {
        if (rangeToAccumulate.getLength() <= 0)
            return true;

        if (state->audioBuffer != nullptr)
        {
            return accumulatePeakBinsFromAudioBufferFullTimeline(*state->audioBuffer,
                                                                 cache->channelBins,
                                                                 cache->channelCount,
                                                                 cache->binCount,
                                                                 totalSamples,
                                                                 rangeToAccumulate,
                                                                 shouldStop);
        }

        if (readerHandle.mappedReader != nullptr)
        {
            return accumulatePeakBinsFromMappedReaderFullTimeline(*readerHandle.mappedReader,
                                                                  cache->channelBins,
                                                                  cache->channelCount,
                                                                  cache->binCount,
                                                                  totalSamples,
                                                                  rangeToAccumulate,
                                                                  shouldStop);
        }

        if (readerHandle.reader == nullptr)
            return false;

        return accumulatePeakBinsFromReaderFullTimeline(*readerHandle.reader,
                                                        cache->channelBins,
                                                        cache->channelCount,
                                                        cache->binCount,
                                                        totalSamples,
                                                        rangeToAccumulate,
                                                        shouldStop);
    };

    auto publishCurrentProgress = [&]()
    {
        rebuildMixedBins(*cache);
        cache->isPreview = true;

        auto publishedCache = std::make_shared<WaveformModel::PeakCache>(*cache);
        publishProgress(std::move(publishedCache), false);
    };

    const auto sampleRate = juce::jmax(1.0, cache->sampleRate);
    const auto minimumProgressSamples = juce::jmax<int64_t>(1,
                                                            static_cast<int64_t>(std::ceil(sampleRate * progressivePeakMinChunkSeconds)));
    const auto maximumProgressSamples = juce::jmax<int64_t>(minimumProgressSamples,
                                                            static_cast<int64_t>(std::ceil(sampleRate * progressivePeakMaxChunkSeconds)));
    const auto progressChunkSamples = juce::jlimit<int64_t>(minimumProgressSamples,
                                                            maximumProgressSamples,
                                                            static_cast<int64_t>(std::ceil(static_cast<double>(totalSamples)
                                                                                           / progressivePeakPreferredSteps)));

    auto currentLoadedEnd = juce::jlimit<int64_t>(0, totalSamples, seedLoadedSampleRange.getEnd());
    currentLoadedEnd = juce::jmin(currentLoadedEnd,
                                  state->getPlayableSampleRange().getEnd());
    if (currentLoadedEnd > 0)
    {
        if (!accumulateRange({ 0, currentLoadedEnd }))
            return {};

        cache->loadedSampleRange = { 0, currentLoadedEnd };
    }

    while (currentLoadedEnd < totalSamples)
    {
        if (shouldStop())
            return {};

        const auto playableEnd = state->getPlayableSampleRange().getEnd();
        if (playableEnd <= currentLoadedEnd)
        {
            if (state->playbackAvailability.getAccess()
                == SingleTrackDocument::PlaybackAvailability::Access::unavailable)
            {
                return {};
            }

            if (!state->playbackAvailability.canAdvanceContiguousPrefix()
                && state->getPlayableSampleRange().getEnd() <= currentLoadedEnd)
            {
                if (currentLoadedEnd == 0) return {};
                rebuildMixedBins(*cache);
                cache->isPreview = true;
                return cache;
            }

            juce::Thread::sleep(5);
            continue;
        }

        const auto nextLoadedEnd = juce::jmin(totalSamples,
                                              juce::jmin(currentLoadedEnd + progressChunkSamples,
                                                         playableEnd));
        if (!accumulateRange({ currentLoadedEnd, nextLoadedEnd }))
            return {};

        currentLoadedEnd = nextLoadedEnd;
        cache->loadedSampleRange = { 0, currentLoadedEnd };
        if (currentLoadedEnd < totalSamples)
            publishCurrentProgress();
    }

    rebuildMixedBins(*cache);
    cache->loadedSampleRange = { 0, totalSamples };
    cache->isPreview = false;
    buildPeakLodPyramid(*cache);
    return cache;
}

template <typename ShouldCancel>
std::shared_ptr<WaveformModel::SpectrogramCache> buildSpectrogramForState(const SingleTrackDocument::StatePtr& state,
                                                                          ShouldCancel&& shouldCancel)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return {};

    while (!state->playbackAvailability.isRandomAccess())
    {
        if (shouldCancel()
            || state->playbackAvailability.getAccess()
                   == SingleTrackDocument::PlaybackAvailability::Access::unavailable
            || (!state->playbackAvailability.canAdvanceContiguousPrefix()
                && !state->playbackAvailability.isRandomAccess()))
        {
            return {};
        }

        juce::Thread::sleep(10);
    }

    const auto channelCount = getStateChannelCount(state);
    const auto totalSamples = getStateTotalSamples(state);
    const auto sampleRate = state->info.sampleRate;
    if (channelCount <= 0 || totalSamples <= 0 || sampleRate <= 0.0)
        return {};

    constexpr int fftOrder = SpectrogramMapping::fftOrder;
    constexpr int fftSize = SpectrogramMapping::fftSize;
    constexpr int imageHeight = SpectrogramMapping::imageHeight;
    const auto imageWidth = chooseSpectrogramWidth(totalSamples, state->info.durationSeconds);
    if (imageWidth <= 0)
        return {};

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(fftSize, juce::dsp::WindowingFunction<float>::hann, false);

    auto cache = std::make_shared<WaveformModel::SpectrogramCache>();
    cache->state = state;
    cache->audioRevision = state->audioRevision;
    cache->durationSeconds = state->info.durationSeconds;
    cache->sampleRate = sampleRate;
    cache->width = imageWidth;
    cache->height = imageHeight;
    cache->image = juce::Image(juce::Image::ARGB, imageWidth, imageHeight, true);

    juce::Image::BitmapData pixels(cache->image, juce::Image::BitmapData::writeOnly);
    std::vector<float> fftData(static_cast<std::size_t>(fftSize * 2), 0.0f);
    juce::AudioBuffer<float> readerWindow;
    StateReaderHandle readerHandle;
    if (state->audioBuffer == nullptr)
    {
        readerHandle = openReaderForState(state);
        if (readerHandle.reader == nullptr)
            return {};

        readerWindow.setSize(channelCount, fftSize, false, false, true);
    }

    for (int x = 0; x < imageWidth; ++x)
    {
        if (shouldCancel())
            return {};

        std::fill(fftData.begin(), fftData.end(), 0.0f);
        const auto startSample = static_cast<int64_t>((static_cast<double>(x) / juce::jmax(1, imageWidth - 1))
                                                      * juce::jmax<int64_t>(0, totalSamples - static_cast<int64_t>(fftSize)));

        if (state->audioBuffer != nullptr)
        {
            const auto& buffer = *state->audioBuffer;
            for (int i = 0; i < fftSize; ++i)
            {
                const auto sampleIndex = juce::jlimit<int64_t>(0, totalSamples - 1, startSample + i);
                float mono = 0.0f;
                for (int channel = 0; channel < channelCount; ++channel)
                    mono += buffer.getSample(channel, static_cast<int>(sampleIndex));
                fftData[static_cast<std::size_t>(i)] = mono / static_cast<float>(channelCount);
            }
        }
        else
        {
            readerWindow.clear();
            // Large PCM sources are section-mapped. This worker owns its reader;
            // map only the FFT window instead of remapping the playback reader or
            // making the entire file resident to serve this optional view.
            if (readerHandle.mappedReader != nullptr)
            {
                const juce::Range<int64_t> windowRange { startSample,
                    juce::jmin(totalSamples, startSample + static_cast<int64_t>(fftSize)) };
                if (!readerHandle.mappedReader->getMappedSection().contains(windowRange)
                    && !readerHandle.mappedReader->mapSectionOfFile(windowRange))
                    return {};
            }
            if (!readerHandle.reader->read(&readerWindow,
                                           0,
                                           fftSize,
                                           startSample,
                                           true,
                                           channelCount > 1))
            {
                return {};
            }

            for (int i = 0; i < fftSize; ++i)
            {
                float mono = 0.0f;
                for (int channel = 0; channel < channelCount; ++channel)
                    mono += readerWindow.getSample(channel, i);
                fftData[static_cast<std::size_t>(i)] = mono / static_cast<float>(channelCount);
            }
        }

        window.multiplyWithWindowingTable(fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform(fftData.data());

        for (int y = 0; y < imageHeight; ++y)
        {
            const auto yNorm = static_cast<float>(y) / static_cast<float>(imageHeight - 1);
            const auto binFloat = SpectrogramMapping::binAtNormalisedY(yNorm);
            const auto binIndex = juce::jlimit(0, (fftSize / 2) - 2, static_cast<int>(binFloat));

            const auto magnitudeA = fftData[static_cast<std::size_t>(binIndex)];
            const auto magnitudeB = fftData[static_cast<std::size_t>(binIndex + 1)];
            const auto magnitude = 0.5f * (magnitudeA + magnitudeB) / static_cast<float>(fftSize);
            const auto db = juce::Decibels::gainToDecibels(juce::jmax(0.0000001f, magnitude), -96.0f);
            auto normalised = juce::jmap(db, -90.0f, -8.0f, 0.0f, 1.0f);
            normalised = std::sqrt(juce::jlimit(0.0f, 1.0f, normalised));
            pixels.setPixelColour(x, y, colourFromSpectralEnergy(normalised));
        }
    }

    return cache;
}
} // namespace

class WaveformModel::PeakBuildJob final : public juce::ThreadPoolJob
{
public:
    PeakBuildJob(WaveformModel& ownerToUse,
                 SingleTrackDocument::StatePtr stateToUse,
                 juce::Range<int64_t> seedLoadedSampleRangeToUse,
                 int64_t requestIdToUse)
        : juce::ThreadPoolJob("Waveform Peak Cache"),
          owner(ownerToUse),
          state(std::move(stateToUse)),
          seedLoadedSampleRange(seedLoadedSampleRangeToUse),
          requestId(requestIdToUse),
          persistenceRequestEpoch(ownerToUse.persistenceEpoch.load(std::memory_order_acquire))
    {
    }

    JobStatus runJob() override
    {
        const auto totalSamples = getStateTotalSamples(state);
        const auto displayBinCount = chooseStartupDisplayBinCount(totalSamples);
        auto cache = canUseFileCache(state) ? loadPersistentDisplayPeakCache(state->info, displayBinCount)
                                           : std::shared_ptr<WaveformModel::PeakCache> {};
        auto builtFreshCache = false;
        if (cache != nullptr)
        {
            cache->state = state;
            cache->audioRevision = state != nullptr ? state->audioRevision : 0;
        }

        const auto needsFreshBuild = cache == nullptr
                                  || (cache->isPreview
                                      && cache->loadedSampleRange.getEnd() < cache->totalSamples);
        if (needsFreshBuild)
        {
            builtFreshCache = true;
            const auto shouldCancel = [this]
            {
                return shouldExit()
                    || owner.latestBuildRequestId.load(std::memory_order_relaxed) != requestId;
            };

            if (state != nullptr
                && state->audioBuffer == nullptr
                && seedLoadedSampleRange.getLength() < getStateTotalSamples(state))
            {
                cache = buildProgressivePeakCacheForState(state,
                                                          displayBinCount,
                                                          seedLoadedSampleRange,
                                                          shouldCancel,
                                                          [this](std::shared_ptr<WaveformModel::PeakCache> progressCache,
                                                                 bool shouldPersist)
                                                          {
                                                              owner.publishPeakCache(requestId,
                                                                                     std::move(progressCache),
                                                                                     shouldPersist, persistenceRequestEpoch);
                                                          });
            }
            else
            {
                cache = buildPeakCacheForState(state,
                                               displayBinCount,
                                               false,
                                               shouldCancel);
            }
        }
        owner.publishPeakCache(requestId, std::move(cache), builtFreshCache, persistenceRequestEpoch);
        return jobHasFinished;
    }

private:
    WaveformModel& owner;
    SingleTrackDocument::StatePtr state;
    juce::Range<int64_t> seedLoadedSampleRange;
    int64_t requestId = 0;
    int64_t persistenceRequestEpoch = 0;
};

class WaveformModel::PeakCachePersistenceJob final : public juce::ThreadPoolJob
{
public:
    PeakCachePersistenceJob(WaveformModel& model, std::shared_ptr<const PeakCache> cacheToUse,
                           SingleTrackDocument::StatePtr stateToUse, int64_t epochToUse)
        : juce::ThreadPoolJob("Waveform Cache Persistence"), owner(model), cache(std::make_shared<PeakCache>(*cacheToUse)),
          state(std::move(stateToUse)), epoch(epochToUse) {}

    ~PeakCachePersistenceJob() override
    {
        owner.persistenceJobQueued.store(false, std::memory_order_release);
    }

    JobStatus runJob() override
    {
        const auto cancel = [this]
        {
            return shouldExit() || owner.persistenceEpoch.load(std::memory_order_acquire) != epoch;
        };
        const auto publish = [this, &cancel](juce::TemporaryFile& file)
        {
            // Publication and owner cancellation share the same lock. Serialization
            // itself is chunked and never holds the UI/cache lock.
            const juce::ScopedLock sl(owner.cacheLock);
            if (!cancel() && owner.document.getState() == state && canUseFileCache(state))
                file.overwriteTargetFileWithTemporary();
        };
        if (!cancel() && canUseFileCache(state))
            persistPeakCacheToDisk(state->info, *cache, cancel, publish);
        return jobHasFinished;
    }

private:
    WaveformModel& owner;
    std::shared_ptr<const PeakCache> cache;
    SingleTrackDocument::StatePtr state;
    int64_t epoch;
};

class WaveformModel::SpectrogramBuildJob final : public juce::ThreadPoolJob
{
public:
    SpectrogramBuildJob(WaveformModel& ownerToUse, SingleTrackDocument::StatePtr stateToUse, int64_t requestIdToUse)
        : juce::ThreadPoolJob("Waveform Spectrogram Cache"),
          owner(ownerToUse),
          state(std::move(stateToUse)),
          requestId(requestIdToUse)
    {
    }

    JobStatus runJob() override
    {
        auto cache = WaveformModel::loadPersistentSpectrogramCache(state);
        if (cache == nullptr)
        {
            cache = buildSpectrogramForState(state,
                                             [this]
                                             {
                                                 return shouldExit()
                                                     || owner.latestSpectrogramRequestId.load(std::memory_order_relaxed) != requestId;
                                             });
        }
        if (!shouldExit())
            owner.publishSpectrogramCache(requestId, state, std::move(cache));
        return jobHasFinished;
    }

private:
    WaveformModel& owner;
    SingleTrackDocument::StatePtr state;
    int64_t requestId = 0;
};

WaveformModel::WaveformModel(SingleTrackDocument& documentToUse)
    : document(documentToUse)
{
    document.addListener(this);
    schedulePeakCacheBuild(document.getState());
}

WaveformModel::~WaveformModel()
{
    traceShutdown("waveform.destructor.begin");
    document.removeListener(this);
    cancelPendingPeakCachePersistence();
    buildPool.removeAllJobs(true, 4000);
    traceShutdown("waveform.jobs.cancel.end");
}

bool WaveformModel::hasWaveform() const noexcept
{
    return document.hasAudio();
}

double WaveformModel::getLengthInSeconds() const noexcept
{
    return document.getDurationSeconds();
}

int WaveformModel::getChannelCount() const noexcept
{
    const auto state = document.getState();
    return state->audioBuffer != nullptr ? state->audioBuffer->getNumChannels() : state->info.numChannels;
}

SingleTrackDocument::StatePtr WaveformModel::getState() const noexcept
{
    return document.getState();
}

juce::Range<float> WaveformModel::getChannelMinMax(int channel, int64_t startSample, int64_t endSample) const noexcept
{
    const auto state = document.getState();
    const auto totalSamples = getStateTotalSamples(state);
    const auto channelCount = getStateChannelCount(state);
    if (!state->hasPlaybackSource()
        || channel < 0
        || channel >= channelCount
        || totalSamples <= 0)
    {
        return {};
    }

    const auto start = juce::jlimit<int64_t>(0, totalSamples, startSample);
    const auto end = juce::jlimit<int64_t>(0, totalSamples, endSample);
    if (end <= start)
        return {};

    if (state->audioBuffer != nullptr)
    {
        const auto* data = state->audioBuffer->getReadPointer(channel);
        float minValue = 1.0f;
        float maxValue = -1.0f;
        for (auto i = start; i < end; ++i)
        {
            minValue = juce::jmin(minValue, data[i]);
            maxValue = juce::jmax(maxValue, data[i]);
        }

        return { minValue, maxValue };
    }

    auto readerHandle = openReaderForState(state);
    if (readerHandle.reader == nullptr)
        return {};

    std::vector<juce::Range<float>> levels(static_cast<std::size_t>(channelCount));
    readerHandle.reader->readMaxLevels(start, end - start, levels.data(), channelCount);
    return levels[static_cast<std::size_t>(channel)];
}

float WaveformModel::getEnvelopeGainAt(double timeSeconds) const noexcept
{
    return document.getEnvelopeGainAt(timeSeconds);
}

std::shared_ptr<const WaveformModel::SpectrogramCache> WaveformModel::getSpectrogramCache() const noexcept
{
    const juce::ScopedLock sl(cacheLock);
    return spectrogramCache;
}

std::shared_ptr<const WaveformModel::PeakCache> WaveformModel::getPeakCache() const noexcept
{
    const juce::ScopedLock sl(cacheLock);
    return peakCache;
}

bool WaveformModel::isPeakCacheReady() const noexcept
{
    const auto cache = getPeakCache();
    const auto state = document.getState();
    return cache != nullptr
        && state->info.isValid()
        && cache->audioRevision == state->audioRevision;
}

bool WaveformModel::isSpectrogramCacheReady() const noexcept
{
    const auto cache = getSpectrogramCache();
    const auto state = document.getState();
    return cache != nullptr
        && state->info.isValid()
        && cache->audioRevision == state->audioRevision
        && cache->image.isValid();
}

WaveformModel::SpectrogramStatus WaveformModel::getSpectrogramStatus() const noexcept
{
    const auto state = document.getState();
    const juce::ScopedLock sl(cacheLock);
    if (state == nullptr || !state->info.isValid()
        || latestSpectrogramAudioRevisionRequested.load(std::memory_order_relaxed) != state->audioRevision)
        return SpectrogramStatus::notRequested;
    return spectrogramStatus;
}

void WaveformModel::ensureSpectrogramReady(bool retryAfterFailure)
{
    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return;

    if (isSpectrogramCacheReady())
        return;

    {
        const juce::ScopedLock sl(cacheLock);
        const auto requestedRevision = latestSpectrogramAudioRevisionRequested.load(std::memory_order_relaxed);
        if (requestedRevision == state->audioRevision
            && !(retryAfterFailure && spectrogramStatus == SpectrogramStatus::failed))
            return;

        latestSpectrogramAudioRevisionRequested.store(state->audioRevision, std::memory_order_relaxed);
        spectrogramStatus = SpectrogramStatus::preparing;
        scheduleSpectrogramBuild(state);
    }
    broadcaster.sendChangeMessage();
}

void WaveformModel::adoptPeakCache(std::shared_ptr<PeakCache> cache)
{
    if (cache == nullptr)
        return;

    if (!cache->isPreview)
        latestBuildRequestId.fetch_add(1, std::memory_order_relaxed);

    {
        const juce::ScopedLock sl(cacheLock);
        peakCache = cache;
    }

    broadcaster.sendChangeMessage();
}

void WaveformModel::adoptSpectrogramCache(std::shared_ptr<SpectrogramCache> cache)
{
    const auto state = document.getState();
    if (cache == nullptr || !cache->image.isValid() || state == nullptr
        || cache->audioRevision != state->audioRevision)
        return;

    {
        const juce::ScopedLock sl(cacheLock);
        latestSpectrogramRequestId.fetch_add(1, std::memory_order_relaxed);
        latestSpectrogramAudioRevisionRequested.store(state->audioRevision, std::memory_order_relaxed);
        spectrogramStatus = SpectrogramStatus::ready;
        spectrogramCache = std::move(cache);
    }

    broadcaster.sendChangeMessage();
}

void WaveformModel::prepareIncomingCaches(std::shared_ptr<PeakCache> incomingPeakCache,
                                          std::shared_ptr<SpectrogramCache> incomingSpectrogramCache)
{
    const juce::ScopedLock sl(cacheLock);
    pendingIncomingPeakCache = std::move(incomingPeakCache);
    pendingIncomingSpectrogramCache = std::move(incomingSpectrogramCache);
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildPeakCacheSnapshot(SingleTrackDocument::StatePtr state,
                                                                                int maxPreviewBins)
{
    const auto totalSamples = getStateTotalSamples(state);
    if (state && state->info.recoveryBacked && state->getPlayableSampleRange().getEnd() < totalSamples) return {};
    const auto boundedBins = juce::jlimit(512, 8192, maxPreviewBins);
    return buildPeakCacheForState(state,
                                  juce::jmin(chooseBinCount(totalSamples), boundedBins),
                                  true,
                                  [] { return false; });
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildCompletePeakCacheForState(SingleTrackDocument::StatePtr state,
                                                                                    std::function<bool()> cancel)
{
    const auto totalSamples = getStateTotalSamples(state);
    if (!state || totalSamples <= 0 || state->getPlayableSampleRange().getEnd() < totalSamples
        || (cancel && cancel())) return {};
    return buildPeakCacheForState(state, static_cast<int>(juce::jmin<int64_t>(totalSamples,
        chooseStartupDisplayBinCount(totalSamples))), false,
        [&cancel] { return cancel && cancel(); });
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildPreviewPeakCacheFromReader(juce::AudioFormatReader& reader,
                                                                                          int maxPreviewBins)
{
    if (reader.sampleRate <= 0.0 || reader.lengthInSamples <= 0 || reader.numChannels == 0)
        return {};

    auto cache = std::make_shared<PeakCache>();
    cache->durationSeconds = static_cast<double>(reader.lengthInSamples) / reader.sampleRate;
    cache->sampleRate = reader.sampleRate;
    cache->totalSamples = reader.lengthInSamples;
    cache->loadedSampleRange = { 0, reader.lengthInSamples };
    cache->channelCount = static_cast<int>(reader.numChannels);
    cache->binCount = juce::jmax(1, juce::jmin(chooseBinCount(reader.lengthInSamples), maxPreviewBins));
    cache->isPreview = true;
    initialisePeakLevelStorage(cache->channelBins,
                               cache->mixedBins,
                               cache->channelCount,
                               cache->binCount,
                               true);

    if (!accumulatePeakBinsFromReader(reader,
                                      cache->channelBins,
                                      cache->channelCount,
                                      cache->binCount,
                                      cache->totalSamples,
                                      cache->loadedSampleRange,
                                      [] { return false; }))
    {
        return {};
    }

    rebuildMixedBins(*cache);
    fillBinTimeExtents(*cache);
    return cache;
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildLeadingPreviewPeakCacheFromReader(juce::AudioFormatReader& reader,
                                                                                                 int maxPreviewBins,
                                                                                                 double leadingSeconds,
                                                                                                 int64_t readyEndSample)
{
    if (reader.sampleRate <= 0.0 || reader.lengthInSamples <= 0 || reader.numChannels == 0)
        return {};

    const auto totalSamples = reader.lengthInSamples;
    const auto readyEnd = readyEndSample < 0 ? totalSamples : juce::jlimit<int64_t>(0, totalSamples, readyEndSample);
    if (readyEnd <= 0) return {};
    const auto leadingSampleCount = juce::jmin(readyEnd, juce::jlimit<int64_t>(1,
                                                          totalSamples,
                                                          static_cast<int64_t>(std::ceil(reader.sampleRate * juce::jlimit(1.0, 30.0, leadingSeconds)))));

    auto cache = std::make_shared<PeakCache>();
    cache->durationSeconds = static_cast<double>(totalSamples) / reader.sampleRate;
    cache->sampleRate = reader.sampleRate;
    cache->totalSamples = totalSamples;
    cache->loadedSampleRange = { 0, leadingSampleCount };
    cache->channelCount = static_cast<int>(reader.numChannels);
    cache->binCount = juce::jmax(1, static_cast<int>(juce::jmin<int64_t>(leadingSampleCount,
        juce::jmin(chooseBinCount(totalSamples), maxPreviewBins))));
    cache->isPreview = leadingSampleCount < totalSamples;
    initialisePeakLevelStorage(cache->channelBins,
                               cache->mixedBins,
                               cache->channelCount,
                               cache->binCount,
                               true);

    auto accumulated = false;
    if (auto* mappedReader = dynamic_cast<juce::MemoryMappedAudioFormatReader*>(&reader))
    {
        accumulated = accumulatePeakBinsFromMappedReader(*mappedReader,
                                                         cache->channelBins,
                                                         cache->channelCount,
                                                         cache->binCount,
                                                         cache->loadedSampleRange,
                                                         [] { return false; });
    }
    else
    {
        accumulated = accumulatePeakBinsFromReader(reader,
                                                   cache->channelBins,
                                                   cache->channelCount,
                                                   cache->binCount,
                                                   totalSamples,
                                                   cache->loadedSampleRange,
                                                   [] { return false; });
    }

    if (!accumulated)
    {
        return {};
    }

    rebuildMixedBins(*cache);
    fillBinTimeExtents(*cache);
    return cache;
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildLeadingPreviewPeakCacheForState(SingleTrackDocument::StatePtr state,
                                                                                               int maxPreviewBins,
                                                                                               double leadingSeconds)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return {};

    if (state->audioBuffer != nullptr && state->audioBuffer->getNumSamples() > 0)
    {
        const auto totalSamples = getStateTotalSamples(state);
        const auto channelCount = getStateChannelCount(state);
        const auto requestedSampleCount = juce::jlimit<int64_t>(1,
                                                              totalSamples,
                                                              static_cast<int64_t>(std::ceil(state->info.sampleRate
                                                                                             * juce::jlimit(1.0, 30.0, leadingSeconds))));
        const auto leadingSampleCount = state->info.recoveryBacked
            ? juce::jmin(requestedSampleCount, state->getPlayableSampleRange().getEnd()) : requestedSampleCount;
        if (leadingSampleCount <= 0) return {};

        auto cache = std::make_shared<PeakCache>();
        cache->state = state;
        cache->audioRevision = state->audioRevision;
        cache->durationSeconds = state->info.durationSeconds;
        cache->sampleRate = state->info.sampleRate;
        cache->totalSamples = totalSamples;
        cache->loadedSampleRange = { 0, leadingSampleCount };
        cache->channelCount = channelCount;
        cache->binCount = juce::jmax(1, static_cast<int>(juce::jmin<int64_t>(leadingSampleCount,
            juce::jmin(chooseBinCount(totalSamples), maxPreviewBins))));
        cache->isPreview = leadingSampleCount < totalSamples;
        initialisePeakLevelStorage(cache->channelBins,
                                   cache->mixedBins,
                                   channelCount,
                                   cache->binCount,
                                   true);

        if (!accumulatePeakBinsFromAudioBufferFullTimeline(*state->audioBuffer,
                                                           cache->channelBins,
                                                           channelCount,
                                                           cache->binCount,
                                                           leadingSampleCount,
                                                           cache->loadedSampleRange,
                                                           [] { return false; }))
        {
            return {};
        }

        rebuildMixedBins(*cache);
        fillBinTimeExtents(*cache);
        return cache;
    }

    auto readerHandle = openReaderForState(state);
    if (readerHandle.reader == nullptr)
        return {};

    auto cache = buildLeadingPreviewPeakCacheFromReader(*readerHandle.reader, maxPreviewBins, leadingSeconds,
        state->info.recoveryBacked ? state->getPlayableSampleRange().getEnd() : -1);
    if (cache == nullptr)
        return {};

    cache->state = state;
    cache->audioRevision = state->audioRevision;
    return cache;
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::buildPreviewPeakCacheForFile(const juce::File& file,
                                                                                      int maxPreviewBins)
{
    auto preparedFile = SingleTrackDocument::prepareAudioFileForOpen(file);
    auto* reader = preparedFile.getReader();
    if (reader == nullptr)
        return {};

    return buildPreviewPeakCacheFromReader(*reader, maxPreviewBins);
}

bool WaveformModel::ensurePersistentPreviewCache(const juce::File& file, int maxPreviewBins)
{
    if (hasPersistentStartupPreviewCache(file))
        return true;

    if (auto cachedHeader = lookupPersistentPeakCacheHeaderForFile(file); cachedHeader.has_value())
    {
        if (auto fullCache = loadPersistentPeakCache(cachedHeader->info); fullCache != nullptr)
        {
            persistStartupPreviewCache(cachedHeader->info, *fullCache);
            return true;
        }
    }

    auto preparedFile = SingleTrackDocument::prepareAudioFileForOpen(file);
    if (!preparedFile.info.isValid() || !preparedFile.hasReader())
        return false;

    auto cache = buildLeadingPreviewPeakCacheFromReader(*preparedFile.getReader(),
                                                        juce::jmin(maxPreviewBins, startupPreviewTargetBins),
                                                        3.0);
    if (cache == nullptr)
        return false;

    persistStartupPreviewCache(preparedFile.info, *cache);
    return true;
}

bool WaveformModel::ensurePersistentSpectrogramCache(const juce::File& file)
{
    if (hasPersistentSpectrogramCache(file))
        return true;

    const auto info = SingleTrackDocument::probeFileInfo(file);
    if (!info.isValid())
        return false;

    auto state = SingleTrackDocument::createStateFromFile(file);
    if (state == nullptr || !state->hasPlaybackSource())
        return false;

    auto cache = buildSpectrogramForState(state, [] { return false; });
    if (cache == nullptr)
        return false;

    persistSpectrogramToDisk(info, *cache);
    return true;
}

std::optional<WaveformModel::PersistentPeakCacheLookup> WaveformModel::loadPersistentStartupPreviewForFile(const juce::File& file)
{
    const auto cacheFile = getWaveformStartupPreviewFile(file);
    if (!cacheFile.existsAsFile())
        return std::nullopt;

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return std::nullopt;

    if (input.readInt64() != waveformStartupPreviewMagic)
        return std::nullopt;

    if (input.readInt() != waveformStartupPreviewVersion)
        return std::nullopt;

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    const auto durationSeconds = input.readDouble();
    const auto sampleRate = input.readDouble();
    const auto totalSamples = input.readInt64();
    const auto loadedSampleStart = input.readInt64();
    const auto loadedSampleEnd = input.readInt64();
    const auto isPreview = input.readBool();
    const auto channelCount = input.readInt();
    auto previewBinCount = 0;

    if (storedPath != file.getFullPathName()
        || storedSize != file.getSize()
        || storedModified != file.getLastModificationTime().toMilliseconds()
        || durationSeconds <= 0.0
        || sampleRate <= 0.0
        || totalSamples <= 0
        || channelCount <= 0)
    {
        return std::nullopt;
    }

    SingleTrackDocument::FileInfo info;
    info.file = file;
    info.formatName = inferFormatNameFromFile(file);
    info.durationSeconds = durationSeconds;
    info.sampleRate = sampleRate;
    info.numChannels = channelCount;
    info.channelLayout = ChannelLayoutCatalog::defaultLayoutForChannelCount(info.numChannels);
    info.numSamples = totalSamples;
    info.bitsPerSample = 0;

    auto cache = std::make_shared<WaveformModel::PeakCache>();
    cache->durationSeconds = durationSeconds;
    cache->sampleRate = sampleRate;
    cache->totalSamples = totalSamples;
    cache->channelCount = channelCount;

    auto clampedLoadedStart = juce::jlimit<int64_t>(0, totalSamples, loadedSampleStart);
    auto clampedLoadedEnd = juce::jlimit<int64_t>(0, totalSamples, loadedSampleEnd);
    const auto fallbackPreviewEnd = sampleRate > 0.0
        ? juce::jlimit<int64_t>(1,
                                totalSamples,
                                static_cast<int64_t>(std::ceil(sampleRate * startupPreviewHeadSeconds)))
        : totalSamples;
    const auto shouldForceHeadPreview = !isPreview || clampedLoadedEnd >= totalSamples;
    if (shouldForceHeadPreview)
    {
        clampedLoadedStart = 0;
        clampedLoadedEnd = fallbackPreviewEnd;
    }

    cache->loadedSampleRange = { clampedLoadedStart, clampedLoadedEnd };
    cache->isPreview = cache->loadedSampleRange.getEnd() < totalSamples;

    if (!readPeakLevel(input,
                       channelCount,
                       cache->channelBins,
                       cache->mixedBins,
                       cache->sampleRate,
                       cache->totalSamples,
                       previewBinCount))
    {
        return std::nullopt;
    }

    cache->binCount = previewBinCount;
    fillBinTimeExtents(*cache);
    if (input.getStatus().failed())
        return std::nullopt;

    return WaveformModel::PersistentPeakCacheLookup { std::move(info), std::move(cache) };
}

void WaveformModel::persistStartupPreviewCache(const SingleTrackDocument::FileInfo& info,
                                               const PeakCache& cache)
{
    if (info.recoveryBacked || (cache.state && !canUseFileCache(cache.state))) return;
    persistStartupPreviewToDisk(info, cache);
}

std::optional<WaveformModel::PersistentPeakCacheLookup> WaveformModel::lookupPersistentPeakCacheHeaderForFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return std::nullopt;

    const auto cacheFile = getWaveformCacheFile(file);
    if (!cacheFile.existsAsFile())
        return std::nullopt;

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return std::nullopt;

    if (input.readInt64() != waveformCacheMagic)
        return std::nullopt;

    const auto storedVersion = input.readInt();
    if (storedVersion != waveformCacheVersion)
        return std::nullopt;

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    const auto durationSeconds = input.readDouble();
    const auto sampleRate = input.readDouble();
    const auto totalSamples = input.readInt64();
    const auto loadedSampleStart = input.readInt64();
    const auto loadedSampleEnd = input.readInt64();
    const auto isPreview = input.readBool();
    const auto channelCount = input.readInt();
    const auto headerBinCount = input.readInt();

    if (storedPath != file.getFullPathName()
        || storedSize != file.getSize()
        || storedModified != file.getLastModificationTime().toMilliseconds()
        || durationSeconds <= 0.0
        || sampleRate <= 0.0
        || totalSamples <= 0
        || channelCount <= 0
        || headerBinCount <= 0)
    {
        return std::nullopt;
    }

    SingleTrackDocument::FileInfo info;
    info.file = file;
    info.formatName = inferFormatNameFromFile(file);
    info.durationSeconds = durationSeconds;
    info.sampleRate = sampleRate;
    info.numChannels = channelCount;
    info.channelLayout = ChannelLayoutCatalog::defaultLayoutForChannelCount(info.numChannels);
    info.numSamples = totalSamples;
    info.bitsPerSample = 0;

    const auto _ignoreLoadedRange = juce::Range<int64_t> { juce::jlimit<int64_t>(0, totalSamples, loadedSampleStart),
                                                           juce::jlimit<int64_t>(0, totalSamples, loadedSampleEnd) };
    juce::ignoreUnused(_ignoreLoadedRange, isPreview, headerBinCount);
    return PersistentPeakCacheLookup { std::move(info), {} };
}

std::optional<WaveformModel::PersistentPeakCacheLookup> WaveformModel::loadPersistentPeakCacheForFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return std::nullopt;

    if (auto startupPreview = loadPersistentStartupPreviewForFile(file); startupPreview.has_value())
        return startupPreview;

    return lookupPersistentPeakCacheHeaderForFile(file);
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::loadPersistentPeakCache(const SingleTrackDocument::FileInfo& info)
{
    if (!info.isValid() || info.recoveryBacked)
        return {};

    const auto cacheFile = getWaveformCacheFile(info);
    if (!cacheFile.existsAsFile())
        return {};

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return {};

    if (input.readInt64() != waveformCacheMagic)
        return {};

    const auto storedVersion = input.readInt();
    if (storedVersion != waveformCacheVersion)
        return {};

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    const auto durationSeconds = input.readDouble();
    const auto sampleRate = input.readDouble();
    const auto totalSamples = input.readInt64();
    const auto loadedSampleStart = input.readInt64();
    const auto loadedSampleEnd = input.readInt64();
    const auto isPreview = input.readBool();
    const auto channelCount = input.readInt();
    const auto headerBinCount = input.readInt();

    if (storedPath != info.file.getFullPathName()
        || storedSize != info.file.getSize()
        || storedModified != info.file.getLastModificationTime().toMilliseconds()
        || std::abs(durationSeconds - info.durationSeconds) > 0.0001
        || std::abs(sampleRate - info.sampleRate) > 0.0001
        || totalSamples != info.numSamples
        || channelCount != info.numChannels
        || headerBinCount <= 0)
    {
        return {};
    }

    auto cache = std::make_shared<PeakCache>();
    cache->audioRevision = 0;
    cache->durationSeconds = durationSeconds;
    cache->sampleRate = sampleRate;
    cache->totalSamples = totalSamples;
    cache->loadedSampleRange = { juce::jlimit<int64_t>(0, totalSamples, loadedSampleStart),
                                 juce::jlimit<int64_t>(0, totalSamples, loadedSampleEnd) };
    cache->channelCount = channelCount;
    cache->isPreview = isPreview && cache->loadedSampleRange.getEnd() < totalSamples;
    cache->binCount = headerBinCount;
    if (!readPeakLevelBody(input,
                           channelCount,
                           cache->binCount,
                           cache->channelBins,
                           cache->mixedBins,
                           cache->sampleRate,
                           cache->totalSamples))
    {
        return {};
    }

    const auto lodLevelCount = input.readInt();
    if (lodLevelCount < 0)
        return {};

    cache->lodLevels.clear();
    for (int levelIndex = 0; levelIndex < lodLevelCount; ++levelIndex)
    {
        PeakCache::PeakLodLevel level;
        if (!readPeakLevel(input,
                           channelCount,
                           level.channelBins,
                           level.mixedBins,
                           cache->sampleRate,
                           cache->totalSamples,
                           level.binCount))
        {
            return {};
        }

        cache->lodLevels.push_back(std::move(level));
    }

    if (input.getStatus().failed())
        return {};

    if (cache->lodLevels.empty())
        buildPeakLodPyramid(*cache);

    return cache;
}

std::shared_ptr<WaveformModel::PeakCache> WaveformModel::loadPersistentPeakCache(SingleTrackDocument::StatePtr state)
{
    if (!canUseFileCache(state))
        return {};

    auto cache = loadPersistentPeakCache(state->info);
    if (cache == nullptr)
        return {};

    cache->state = state;
    cache->audioRevision = state->audioRevision;
    return cache;
}

std::shared_ptr<WaveformModel::SpectrogramCache> WaveformModel::loadPersistentSpectrogramCache(const SingleTrackDocument::FileInfo& info)
{
    if (!info.isValid() || info.recoveryBacked)
        return {};

    const auto cacheFile = getSpectrogramCacheFile(info);
    if (!cacheFile.existsAsFile())
        return {};

    juce::FileInputStream input(cacheFile);
    if (!input.openedOk())
        return {};

    if (input.readInt64() != spectrogramCacheMagic)
        return {};

    if (input.readInt() != spectrogramCacheVersion)
        return {};

    const auto storedPath = input.readString();
    const auto storedSize = input.readInt64();
    const auto storedModified = input.readInt64();
    const auto durationSeconds = input.readDouble();
    const auto sampleRate = input.readDouble();
    const auto width = input.readInt();
    const auto height = input.readInt();

    if (storedPath != info.file.getFullPathName()
        || storedSize != info.file.getSize()
        || storedModified != info.file.getLastModificationTime().toMilliseconds()
        || std::abs(durationSeconds - info.durationSeconds) > 0.0001
        || std::abs(sampleRate - info.sampleRate) > 0.0001
        || width <= 0
        || height <= 0)
    {
        return {};
    }

    auto cache = std::make_shared<SpectrogramCache>();
    cache->durationSeconds = durationSeconds;
    cache->sampleRate = sampleRate;
    cache->width = width;
    cache->height = height;
    cache->image = juce::Image(juce::Image::ARGB, width, height, true);

    juce::Image::BitmapData pixels(cache->image, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
            pixels.setPixelColour(x, y, juce::Colour(static_cast<juce::uint32>(input.readInt())));
    }

    if (input.getStatus().failed())
        return {};

    return cache;
}

std::shared_ptr<WaveformModel::SpectrogramCache> WaveformModel::loadPersistentSpectrogramCache(SingleTrackDocument::StatePtr state)
{
    if (!canUseFileCache(state))
        return {};

    auto cache = loadPersistentSpectrogramCache(state->info);
    if (cache == nullptr)
        return {};

    cache->state = state;
    cache->audioRevision = state->audioRevision;
    return cache;
}

juce::File WaveformModel::getPersistentCacheDirectory()
{
    return getWaveformCacheDirectory();
}

bool WaveformModel::hasPersistentPeakCache(const juce::File& file)
{
    return hasMatchingPersistentCacheHeader(file,
                                            getWaveformCacheFile(file),
                                            waveformCacheMagic,
                                            waveformCacheVersion);
}

bool WaveformModel::hasPersistentStartupPreviewCache(const juce::File& file)
{
    return hasMatchingPersistentCacheHeader(file,
                                            getWaveformStartupPreviewFile(file),
                                            waveformStartupPreviewMagic,
                                            waveformStartupPreviewVersion);
}

bool WaveformModel::hasPersistentSpectrogramCache(const juce::File& file)
{
    return hasMatchingPersistentCacheHeader(file,
                                            getSpectrogramCacheFile(file),
                                            spectrogramCacheMagic,
                                            spectrogramCacheVersion);
}

WaveformModel::PersistentCacheStats WaveformModel::getPersistentCacheStats()
{
    PersistentCacheStats stats;
    const auto directory = getWaveformCacheDirectory();
    if (!directory.exists())
        return stats;

    for (const auto& pattern : { "*.swfc", "*.swfp", "*.sspc" })
        for (const auto& file : directory.findChildFiles(juce::File::findFiles, false, pattern))
        {
            stats.totalBytes += file.getSize();
            ++stats.fileCount;
        }

    return stats;
}

void WaveformModel::clearPersistentCache()
{
    const auto directory = getWaveformCacheDirectory();
    if (!directory.exists())
        return;

    for (const auto& pattern : { "*.swfc", "*.swfp", "*.sspc" })
        for (const auto& file : directory.findChildFiles(juce::File::findFiles, false, pattern))
            file.deleteFile();
}

void WaveformModel::addChangeListener(juce::ChangeListener* listener)
{
    broadcaster.addChangeListener(listener);
}

void WaveformModel::removeChangeListener(juce::ChangeListener* listener)
{
    broadcaster.removeChangeListener(listener);
}

void WaveformModel::documentChanged(SingleTrackDocument::StatePtr newState)
{
    cancelPendingPeakCachePersistence();
    {
        const juce::ScopedLock sl(cacheLock);
        if (newState == nullptr || !newState->info.isValid()
            || latestSpectrogramAudioRevisionRequested.load(std::memory_order_relaxed) != newState->audioRevision)
        {
            // Invalidating a pending request also cancels old failure callbacks.
            // Envelope-only notifications retain the same audio revision and
            // must neither restart a failed build nor duplicate a pending one.
            latestSpectrogramRequestId.fetch_add(1, std::memory_order_relaxed);
            latestSpectrogramAudioRevisionRequested.store(-1, std::memory_order_relaxed);
            spectrogramStatus = SpectrogramStatus::notRequested;
        }
    }
    if (newState == nullptr || !newState->info.isValid())
    {
        {
            const juce::ScopedLock sl(cacheLock);
            peakCache.reset();
            spectrogramCache.reset();
            pendingIncomingPeakCache.reset();
            pendingIncomingSpectrogramCache.reset();
        }
        latestSpectrogramAudioRevisionRequested.store(-1, std::memory_order_relaxed);
        broadcaster.sendChangeMessage();
        return;
    }

    {
        const juce::ScopedLock sl(cacheLock);
        if (spectrogramCache != nullptr)
        {
            const auto cachedState = spectrogramCache->state;
            const auto fileChanged = cachedState == nullptr || cachedState->info.file != newState->info.file;
            const auto revisionChanged = spectrogramCache->audioRevision != newState->audioRevision;
            if (fileChanged || revisionChanged)
            {
                spectrogramCache.reset();
                latestSpectrogramAudioRevisionRequested.store(-1, std::memory_order_relaxed);
            }
        }
    }

    if (!newState->hasPlaybackSource())
    {
        broadcaster.sendChangeMessage();
        return;
    }

    const auto state = newState;

    std::shared_ptr<PeakCache> incomingPeakCache;
    std::shared_ptr<SpectrogramCache> incomingSpectrogramCache;
    {
        const juce::ScopedLock sl(cacheLock);
        incomingPeakCache = pendingIncomingPeakCache;
        incomingSpectrogramCache = pendingIncomingSpectrogramCache;
        pendingIncomingPeakCache.reset();
        pendingIncomingSpectrogramCache.reset();
    }

    auto adoptIncomingPeakCacheIfMatching = [&](std::shared_ptr<PeakCache> cacheToAdopt) -> bool
    {
        if (cacheToAdopt == nullptr)
            return false;

        if (cacheToAdopt->channelCount != state->info.numChannels
            || cacheToAdopt->totalSamples != state->info.numSamples
            || std::abs(cacheToAdopt->sampleRate - state->info.sampleRate) > 0.0001
            || std::abs(cacheToAdopt->durationSeconds - state->info.durationSeconds) > 0.0001)
        {
            return false;
        }

        cacheToAdopt->state = state;
        cacheToAdopt->audioRevision = state->audioRevision;

        const juce::ScopedLock sl(cacheLock);
        peakCache = std::move(cacheToAdopt);
        return true;
    };

    auto adoptIncomingSpectrogramCacheIfMatching = [&](std::shared_ptr<SpectrogramCache> cacheToAdopt) -> bool
    {
        if (cacheToAdopt == nullptr || !cacheToAdopt->image.isValid())
            return false;

        if (std::abs(cacheToAdopt->sampleRate - state->info.sampleRate) > 0.0001
            || std::abs(cacheToAdopt->durationSeconds - state->info.durationSeconds) > 0.0001)
        {
            return false;
        }

        cacheToAdopt->state = state;
        cacheToAdopt->audioRevision = state->audioRevision;

        {
            const juce::ScopedLock sl(cacheLock);
            spectrogramCache = std::move(cacheToAdopt);
            latestSpectrogramRequestId.fetch_add(1, std::memory_order_relaxed);
            spectrogramStatus = SpectrogramStatus::ready;
            latestSpectrogramAudioRevisionRequested.store(state->audioRevision, std::memory_order_relaxed);
        }
        return true;
    };

    adoptIncomingPeakCacheIfMatching(std::move(incomingPeakCache));
    adoptIncomingSpectrogramCacheIfMatching(std::move(incomingSpectrogramCache));

    auto cache = getPeakCache();
    const auto hasReusableFullPeakCache = [&]() -> bool
    {
        if (cache == nullptr || cache->isPreview)
            return false;

        if (cache->channelCount != state->info.numChannels
            || cache->totalSamples != state->info.numSamples
            || std::abs(cache->sampleRate - state->info.sampleRate) > 0.0001
            || std::abs(cache->durationSeconds - state->info.durationSeconds) > 0.0001)
        {
            return false;
        }

        if (cache->state != nullptr && cache->state->info.isValid())
            return cache->state->info.file == state->info.file;

        return true;
    }();

    const auto needsPeakRebuild = cache == nullptr
                               || cache->isPreview
                               || !hasReusableFullPeakCache
                               || cache->audioRevision != state->audioRevision;

    juce::Range<int64_t> seedLoadedSampleRange;
    if (cache != nullptr
        && cache->isPreview
        && cache->loadedSampleRange.getLength() > 0
        && cache->loadedSampleRange.getEnd() < cache->totalSamples)
    {
        seedLoadedSampleRange = cache->loadedSampleRange;
    }

    if (needsPeakRebuild)
        schedulePeakCacheBuild(state, seedLoadedSampleRange);

    if (cache != nullptr && cache->audioRevision == state->audioRevision)
        queuePeakCachePersistence(cache, state, persistenceEpoch.load(std::memory_order_acquire));

    broadcaster.sendChangeMessage();
}

void WaveformModel::queuePeakCachePersistence(std::shared_ptr<const PeakCache> cache,
                                               SingleTrackDocument::StatePtr state, int64_t epoch)
{
    if (!cache || !canUseFileCache(state)) return;
    const juce::ScopedLock sl(cacheLock);
    if (document.getState() != state || persistenceEpoch.load(std::memory_order_acquire) != epoch) return;
    pendingPersistentPeakCache = std::move(cache);
    pendingPersistenceState = std::move(state);
    persistenceDueAtMs = juce::Time::getMillisecondCounterHiRes() + 1200.0;
}

void WaveformModel::cancelPendingPeakCachePersistence()
{
    const juce::ScopedLock sl(cacheLock);
    persistenceEpoch.fetch_add(1, std::memory_order_release);
    pendingPersistentPeakCache.reset();
    pendingPersistenceState.reset();
}

void WaveformModel::dispatchPendingPeakCachePersistence()
{
    const juce::ScopedLock sl(cacheLock);
    if (persistenceJobQueued.load(std::memory_order_acquire) || !pendingPersistentPeakCache
        || juce::Time::getMillisecondCounterHiRes() < persistenceDueAtMs) return;
    if (document.getState() != pendingPersistenceState || !canUseFileCache(pendingPersistenceState))
    {
        pendingPersistentPeakCache.reset();
        pendingPersistenceState.reset();
        return;
    }
    persistenceJobQueued.store(true, std::memory_order_release);
    // Copy only at post-Core admission. Recovered workspace activation retains
    // its exact prepared peak object while this job owns an immutable snapshot.
    buildPool.addJob(new PeakCachePersistenceJob(*this, std::move(pendingPersistentPeakCache),
        std::move(pendingPersistenceState), persistenceEpoch.load(std::memory_order_acquire)), true);
}

void WaveformModel::schedulePeakCacheBuild(SingleTrackDocument::StatePtr state,
                                           juce::Range<int64_t> seedLoadedSampleRange)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return;

    const auto requestId = latestBuildRequestId.fetch_add(1, std::memory_order_relaxed) + 1;
    buildPool.addJob(new PeakBuildJob(*this, std::move(state), seedLoadedSampleRange, requestId), true);
}

void WaveformModel::scheduleSpectrogramBuild(SingleTrackDocument::StatePtr state)
{
    if (state == nullptr || !state->info.isValid() || !state->hasPlaybackSource())
        return;

    const auto requestId = latestSpectrogramRequestId.fetch_add(1, std::memory_order_relaxed) + 1;
    buildPool.addJob(new SpectrogramBuildJob(*this, std::move(state), requestId), true);
}

void WaveformModel::publishPeakCache(int64_t requestId,
                                     std::shared_ptr<PeakCache> cache,
                                     bool persistToDisk, int64_t persistenceRequestEpoch)
{
    if (cache == nullptr || latestBuildRequestId.load(std::memory_order_relaxed) != requestId)
        return;

    auto cacheToPersist = cache;
    {
        const juce::ScopedLock sl(cacheLock);
        peakCache = std::move(cache);
    }

#if SINGLE_EDITOR_ENABLE_OPEN_TIMING
    logPeakCacheEvent(cacheToPersist != nullptr && cacheToPersist->isPreview ? "progress-preview-published"
                                                                             : "full-cache-published",
                      cacheToPersist != nullptr && cacheToPersist->state != nullptr ? cacheToPersist->state->info
                                                                                    : SingleTrackDocument::FileInfo {},
                      cacheToPersist.get());
#endif

    broadcaster.sendChangeMessage();

    // Keep serialization pending until MainComponent admits this generation.
    if (persistToDisk)
        queuePeakCachePersistence(cacheToPersist, cacheToPersist->state, persistenceRequestEpoch);
}

void WaveformModel::publishSpectrogramCache(int64_t requestId,
                                           SingleTrackDocument::StatePtr requestedState,
                                           std::shared_ptr<SpectrogramCache> cache)
{
    auto cacheToPersist = cache;
    {
        const juce::ScopedLock sl(cacheLock);
        const auto currentState = document.getState();
        if (latestSpectrogramRequestId.load(std::memory_order_relaxed) != requestId
            || requestedState == nullptr || currentState == nullptr || !currentState->info.isValid()
            || currentState->audioRevision != requestedState->audioRevision)
            return;
        spectrogramStatus = cache != nullptr && cache->image.isValid()
            ? SpectrogramStatus::ready : SpectrogramStatus::failed;
        spectrogramCache = std::move(cache);
    }

    // A failed build is a terminal visible state, not an endless loading badge.
    // Notification does not reset the request; only an explicit retry may do so.
    broadcaster.sendChangeMessage();
    if (cacheToPersist != nullptr && cacheToPersist->state != nullptr && cacheToPersist->state->info.isValid())
    {
        if (canUseFileCache(cacheToPersist->state))
            persistSpectrogramToDisk(cacheToPersist->state->info, *cacheToPersist);
    }
}

int64_t WaveformModel::getLatestBuildRequestId() const noexcept
{
    return latestBuildRequestId.load(std::memory_order_relaxed);
}
} // namespace singleeditor
