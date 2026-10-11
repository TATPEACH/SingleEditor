#include "Audio/VgmstreamDecodeService.h"
#include "Audio/BoundedDecodeProcess.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/SourceFileLease.h"
#include <thread>

#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor
{
namespace
{
uint16_t readLittleEndian16(const juce::uint8* data) noexcept
{
    return static_cast<uint16_t>(data[0])
         | static_cast<uint16_t>(data[1] << 8);
}

uint32_t readLittleEndian32(const juce::uint8* data) noexcept
{
    return static_cast<uint32_t>(data[0])
         | (static_cast<uint32_t>(data[1]) << 8)
         | (static_cast<uint32_t>(data[2]) << 16)
         | (static_cast<uint32_t>(data[3]) << 24);
}

juce::File getDecodeInProgressMarker(const juce::File& decodedWaveFile)
{
    return decodedWaveFile.getSiblingFile(decodedWaveFile.getFileName() + ".decoding");
}

bool decodeMarkerShowsFailure(const juce::File& marker)
{
    return marker.existsAsFile() && marker.loadFileAsString().trim() == "failed";
}

void discardStaleOrFailedDecode(const juce::File& marker, const juce::File& decodedWaveFile)
{
    const auto stale = marker.existsAsFile()
                    && marker.getLastModificationTime() < juce::Time::getCurrentTime() - juce::RelativeTime::minutes(2.0);
    if (decodeMarkerShowsFailure(marker) || stale)
    {
        marker.deleteFile();
        decodedWaveFile.deleteFile();
    }
}

struct GrowingWaveInfo
{
    int64_t dataStart = 0;
    int bytesPerFrame = 0;
    uint16_t channels = 0;
    int64_t totalSamples = 0;
    double sampleRate = 0.0;
};

std::optional<GrowingWaveInfo> inspectGrowingWave(const juce::File& file)
{
    juce::FileInputStream stream(file);
    if (!stream.openedOk() || stream.getTotalLength() < 44)
        return std::nullopt;

    juce::uint8 header[12] {};
    if (stream.read(header, 12) != 12
        || std::memcmp(header, "RIFF", 4) != 0
        || std::memcmp(header + 8, "WAVE", 4) != 0)
    {
        return std::nullopt;
    }

    int bytesPerFrame = 0;
    uint16_t channels = 0;
    double sampleRate = 0.0;
    constexpr int64_t maximumHeaderBytes = 1024 * 1024;
    while (stream.getPosition() + 8 <= juce::jmin(stream.getTotalLength(), maximumHeaderBytes))
    {
        juce::uint8 chunkHeader[8] {};
        if (stream.read(chunkHeader, 8) != 8)
            return std::nullopt;

        const auto chunkSize = static_cast<int64_t>(readLittleEndian32(chunkHeader + 4));
        const auto chunkDataStart = stream.getPosition();
        if (std::memcmp(chunkHeader, "fmt ", 4) == 0)
        {
            if (chunkSize < 16 || chunkDataStart + 16 > stream.getTotalLength())
                return std::nullopt;

            juce::uint8 formatBytes[16] {};
            if (stream.read(formatBytes, 16) != 16)
                return std::nullopt;

            sampleRate = static_cast<double>(readLittleEndian32(formatBytes + 4));
            bytesPerFrame = static_cast<int>(readLittleEndian16(formatBytes + 12));
            channels = readLittleEndian16(formatBytes + 2);
            const auto bits = readLittleEndian16(formatBytes + 14);
            if (bits == 0 || bits > 32 || bits % 8 != 0
                || bytesPerFrame != static_cast<int>(channels * (bits / 8)))
                return std::nullopt;
        }
        else if (std::memcmp(chunkHeader, "data", 4) == 0)
        {
            if (bytesPerFrame <= 0 || chunkSize <= 0 || chunkSize % bytesPerFrame != 0
                || !media::admitReader(sampleRate, channels, chunkSize / bytesPerFrame))
                return std::nullopt;

            return GrowingWaveInfo { chunkDataStart,
                                     bytesPerFrame,
                                     channels,
                                     chunkSize / bytesPerFrame,
                                     sampleRate };
        }

        const auto nextChunk = chunkDataStart + chunkSize + (chunkSize & 1);
        if (nextChunk <= chunkDataStart || !stream.setPosition(nextChunk))
            return std::nullopt;
    }

    return std::nullopt;
}

juce::String cacheLockName(const juce::File& wave)
{
    const auto path = wave.getFullPathName().toLowerCase();
    return "SingleEditor-vgmstream-" + juce::SHA256(path.toRawUTF8(), path.getNumBytesAsUTF8()).toHexString();
}

bool usableWave(const media::DecodedWaveCache::Context& context, const juce::File& wave,
                const media::WorkCheckpoint& checkpoint = {}, media::DecodedWaveCache::LookupReason* = nullptr);

struct DecodedBackingState final
{
    std::atomic<media::SourceFileWitness::Ptr> witness;
};

media::SourceFileWitness::Ptr sealedBacking(const media::DecodedWaveCache::Context& context,
                                           const juce::File& wave, const media::WorkCheckpoint& checkpoint)
{
    std::unique_ptr<media::SourceFileLease> lease;
    if (media::SourceFileLease::open(wave, lease).failed()
        || !media::DecodedWaveCache::reusable(context, wave, checkpoint)) return {};
    const auto complete = media::DecodedWaveCache::inspectComplete(wave, checkpoint);
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileInputStream>(wave);
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));
    if (!reader || !complete || !media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples)
        || reader->sampleRate != complete->rate || reader->numChannels != complete->channels
        || static_cast<uint64_t>(reader->lengthInSamples) != complete->frames
        || lease->recheck().failed()) return {};
    auto facts = std::make_shared<media::SourceFileWitness>(*lease->witness());
    facts->readerFormat = reader->getFormatName();
    facts->sampleRate = reader->sampleRate; facts->channels = static_cast<int>(reader->numChannels);
    facts->bitsPerSample = static_cast<int>(reader->bitsPerSample); facts->frames = reader->lengthInSamples;
    return facts;
}

// The worker owns its process/cache resources independently of the reader or UI.
// Dropping the last reader requests cancellation; it never kills/joins this thread.
class ProgressiveDecodeSession final
{
    struct Work final
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        struct ObservedLifetime final
        {
            explicit ObservedLifetime(const media::OptionalMediaOperation::Ptr& request) noexcept
                : token(OptionalLifecycleToken::request(request ? request->observedContext() : currentOptionalLifecycleContext(),
                    OptionalLifecycleOwner::vgmstream, OptionalLifecycleResourceKind::packedDecodeWork,
                    request ? request->observedResourceId() : 0)) {}
            ~ObservedLifetime() { if (created) token.release(); else token.failed(); }
            OptionalLifecycleToken token;
            bool created = false;
        } observedLifetime; // Last to die, after all actual Work resources and captures.
#endif
        Work(juce::File producer, juce::File source, juce::File wave,
             VgmstreamDecodeService::DecodeProgressCallback progress,
             media::DecodedWaveCache::Context cacheContext, media::OptionalMediaOperation::Ptr request)
            :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
              observedLifetime(request),
#endif
              cli(std::move(producer)), input(std::move(source)), output(std::move(wave)),
              marker(getDecodeInProgressMarker(output)), callback(std::move(progress)),
              context(std::move(cacheContext)), operation(std::move(request))
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            observedLifetime.token.created(); observedLifetime.created = true;
#endif
        }

        void requestCancel()
        {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            observedLifetime.token.note(OptionalLifecycleAction::cancelRequested);
#endif
            cancel.store(true, std::memory_order_release);
            if (operation) operation->requestCancel();
        }
        bool isCancelled() const { return cancel.load(std::memory_order_acquire)
            || (operation && operation->cancelFlag().load(std::memory_order_acquire)); }
        void failure(media::OptionalMediaFailureState::Reason reason,
                     media::OptionalMediaFailureState::Stage stage)
        {
            if (operation) operation->fail(reason, stage);
            finishWithFailure();
        }
        void run()
        {
            // The lock is acquired and released on this worker, never transferred
            // across threads (Win32 mutex ownership is thread-affine).
            juce::InterProcessLock cacheLock(cacheLockName(output));
            const auto started = juce::Time::getMillisecondCounter();
            const auto limits = operation ? operation->packedLimits() : media::BoundedDecodeProcess::Limits();
            while (!cacheLock.enter(25))
            {
                if (isCancelled()) { if (operation) operation->cancelled(); finishWithFailure(); return; }
                if (juce::Time::getMillisecondCounter() - started >= static_cast<uint32_t>(limits.wallTimeMs))
                { failure(media::OptionalMediaFailureState::Reason::ioFailure, media::OptionalMediaFailureState::Stage::cache); return; }
            }
            if (isCancelled()) { if (operation) operation->cancelled(); finishWithFailure(); return; }

            const auto checkpoint = [this] { return !isCancelled(); };
            media::DecodedWaveCache::LookupReason cacheReason;
            const bool reused = usableWave(context, output, checkpoint, &cacheReason);
            if (operation) operation->noteCacheLookup(reused ? media::OptionalMediaOperation::CacheLookup::reused
                                                            : media::OptionalMediaOperation::CacheLookup::miss, cacheReason);
            if (reused)
            {
                if (static_cast<uint64_t>(output.getSize()) > limits.outputBytes)
                { failure(media::OptionalMediaFailureState::Reason::outputQuota, media::OptionalMediaFailureState::Stage::cache); return; }
                if (publishComplete()) return;
                failure(media::OptionalMediaFailureState::Reason::cacheInvalid, media::OptionalMediaFailureState::Stage::cache);
                return;
            }
            if (isCancelled()) { if (operation) operation->cancelled(); finishWithFailure(); return; }
            // Exclusive producer lock proves no live decoder owns this marker.
            // Deleting output must succeed before removing its failed marker.
            if (output.getParentDirectory().createDirectory().failed() || !output.deleteFile()
                || !marker.deleteFile() || !media::DecodedWaveCache::sealFile(output).deleteFile()
                || !marker.replaceWithText("decoding"))
            { failure(media::OptionalMediaFailureState::Reason::ioFailure, media::OptionalMediaFailureState::Stage::cache); return; }
            ownsOutput = true;
            juce::StringArray arguments { "-i", "-W", "1", "-o", output.getFullPathName(), input.getFullPathName() };
            int64_t lastPublishedReadyEnd = 0;
            const auto result = media::BoundedDecodeProcess::run(cli, arguments, output, limits,
                operation ? operation->cancelFlag() : cancel, [&]
                {
                    publishFileGrowth(lastPublishedReadyEnd);
                    return media::BoundedDecodeProcess::Failure::none;
                }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                , observedLifetime.token.resourceId()
#endif
                );
            if (operation) operation->observeProcess(result);
            if (!result || isCancelled())
            {
                if (operation) { if (isCancelled() && result) operation->cancelled(); else operation->processFailed(result); }
                finishWithFailure();
                return;
            }
            publishFileGrowth(lastPublishedReadyEnd);
            const bool published = media::DecodedWaveCache::inspectComplete(output, checkpoint)
                && media::DecodedWaveCache::seal(context, output, checkpoint) && publishComplete();
            if (operation) operation->noteCachePublication(published);
            if (!published)
            { failure(media::OptionalMediaFailureState::Reason::cacheInvalid, media::OptionalMediaFailureState::Stage::cache); return; }
        }
        bool publishComplete()
        {
            const auto facts = sealedBacking(context, output, [this] { return !isCancelled(); });
            if (!facts || isCancelled()) return false;
            backing->witness.store(facts, std::memory_order_release);
            if (ownsOutput && !marker.deleteFile()) { backing->witness.store({}); return false; }
            if (operation)
            {
                operation->noteProgress(facts->frames, facts->frames); operation->completeDecodedAudio();
                if (operation->report().cancelled) { backing->witness.store({}); return false; }
            }
            // Publish the real immutable backing before random-access availability.
            if (callback) callback(facts->frames, true, false);
            playablePrefixReady.store(true, std::memory_order_release);
            finished.store(true, std::memory_order_release);
            firstPlayablePrefix.signal();
            return true;
        }
        void publishFileGrowth(int64_t& lastPublishedReadyEnd)
        {
            const auto waveInfo = inspectGrowingWave(output);
            if (!waveInfo) return;
            const auto readyEnd = juce::jlimit<int64_t>(0, waveInfo->totalSamples,
                juce::jmax<int64_t>(0, output.getSize() - waveInfo->dataStart) / waveInfo->bytesPerFrame);
            if (readyEnd > lastPublishedReadyEnd)
            {
                lastPublishedReadyEnd = readyEnd;
                if (operation) operation->noteProgress(readyEnd, waveInfo->totalSamples);
                if (callback) callback(readyEnd, false, false);
            }
            if (readyEnd >= juce::jmin<int64_t>(waveInfo->totalSamples, static_cast<int64_t>(std::ceil(waveInfo->sampleRate))))
            { playablePrefixReady.store(true, std::memory_order_release); firstPlayablePrefix.signal(); }
        }
        void finishWithFailure()
        {
            backing->witness.store({}, std::memory_order_release);
            if (ownsOutput)
            {
                marker.replaceWithText("failed");
                // A live prefix reader may prevent deletion. Leave a failed marker
                // for the next explicitly locked attempt; no destructor deletes it.
                if (output.deleteFile()) { media::DecodedWaveCache::sealFile(output).deleteFile(); marker.deleteFile(); }
            }
            if (callback) callback(0, false, true);
            playablePrefixReady.store(false, std::memory_order_release);
            finished.store(true, std::memory_order_release);
            firstPlayablePrefix.signal();
        }
        juce::File cli, input, output, marker;
        VgmstreamDecodeService::DecodeProgressCallback callback;
        const media::DecodedWaveCache::Context context;
        media::OptionalMediaOperation::Ptr operation;
        std::shared_ptr<DecodedBackingState> backing = std::make_shared<DecodedBackingState>();
        std::atomic<bool> cancel { false }, playablePrefixReady { false }, finished { false };
        juce::WaitableEvent firstPlayablePrefix;
        bool ownsOutput = false;
    };
public:
    ProgressiveDecodeSession(juce::File cli, juce::File input, juce::File output,
        VgmstreamDecodeService::DecodeProgressCallback callback,
        media::DecodedWaveCache::Context context, media::OptionalMediaOperation::Ptr operation)
        : work(std::make_shared<Work>(std::move(cli), std::move(input), std::move(output),
                                      std::move(callback), std::move(context), std::move(operation))) {}
    ~ProgressiveDecodeSession()
    {
        if (!work->finished.load(std::memory_order_acquire)) work->requestCancel();
    }
    bool startAndWaitForPlayablePrefix()
    {
        try
        {
            std::thread([state = work]
            {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                // The immutable creation context crosses the actual detached
                // thread boundary; no later document generation is sampled.
                const ScopedOptionalLifecycleContext context(state->observedLifetime.token.creationContext());
                struct RunObservation final
                {
                    explicit RunObservation(const OptionalLifecycleToken& value) noexcept : token(value)
                    { token.note(OptionalLifecycleAction::taskStarted); }
                    ~RunObservation() { token.note(OptionalLifecycleAction::taskCompleted); }
                    const OptionalLifecycleToken& token;
                } runObservation(state->observedLifetime.token);
#endif
                juce::Thread::setCurrentThreadName("vgmstream progressive decode");
               #if JUCE_WINDOWS
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
               #endif
                try { state->run(); }
                catch (...) { state->ownsOutput = false; state->failure(media::OptionalMediaFailureState::Reason::ioFailure,
                                             media::OptionalMediaFailureState::Stage::decode); }
            }).detach();
        }
        catch (...) { work->failure(media::OptionalMediaFailureState::Reason::ioFailure,
                                    media::OptionalMediaFailureState::Stage::launch); return false; }
        while (!work->firstPlayablePrefix.wait(50)) {}
        return work->playablePrefixReady.load(std::memory_order_acquire);
    }
    std::shared_ptr<DecodedBackingState> backing() const { return work->backing; }
private:
    std::shared_ptr<Work> work;
};

class ProgressiveWaveReader final : public juce::AudioFormatReader, public media::IndependentAudioBacking
{
public:
    ProgressiveWaveReader(std::unique_ptr<juce::AudioFormatReader> reader, std::shared_ptr<DecodedBackingState> state)
        : juce::AudioFormatReader(nullptr, reader->getFormatName()), inner(std::move(reader)), backing(std::move(state))
    {
        sampleRate = inner->sampleRate; bitsPerSample = inner->bitsPerSample; lengthInSamples = inner->lengthInSamples;
        numChannels = inner->numChannels; usesFloatingPointData = inner->usesFloatingPointData; metadataValues = inner->metadataValues;
    }
    bool readSamples(int* const* destinations, int channels, int offset, juce::int64 start, int count) override
    { return inner->readSamples(destinations, channels, offset, start, count); }
    media::SourceFileWitness::Ptr validatedBacking() const noexcept override
    { return backing->witness.load(std::memory_order_acquire); }
private:
    std::unique_ptr<juce::AudioFormatReader> inner;
    std::shared_ptr<DecodedBackingState> backing;
};

class SessionFileInputStream final : public juce::InputStream
{
public:
    SessionFileInputStream(const juce::File& file, std::shared_ptr<ProgressiveDecodeSession> sessionToKeepAlive)
        : session(std::move(sessionToKeepAlive)), stream(file)
    {
    }

    bool openedOk() const noexcept { return stream.openedOk(); }
    int64_t getTotalLength() override { return stream.getTotalLength(); }
    int read(void* destination, int bytesToRead) override { return stream.read(destination, bytesToRead); }
    bool isExhausted() override { return stream.isExhausted(); }
    int64_t getPosition() override { return stream.getPosition(); }
    bool setPosition(int64_t position) override { return stream.setPosition(position); }

private:
    std::shared_ptr<ProgressiveDecodeSession> session;
    juce::FileInputStream stream;
};

juce::File findCliFrom(const juce::File& start)
{
    auto current = start;
    for (int i = 0; i < 8 && current != juce::File(); ++i)
    {
        const auto candidate = current.getChildFile("VGM").getChildFile("vgmstream-win").getChildFile("vgmstream-cli.exe");
        if (candidate.existsAsFile())
            return candidate;

        current = current.getParentDirectory();
    }

    return {};
}

bool looksLikeSafeDirectWaveWem(const juce::File& inputFile, juce::String* detail)
{
    juce::FileInputStream stream(inputFile);
    if (!stream.openedOk())
    {
        if (detail != nullptr)
            *detail = "WEM probe failed to open the container.";
        return false;
    }

    constexpr int probeBytes = 96;
    juce::HeapBlock<juce::uint8> bytes(static_cast<size_t>(probeBytes), true);
    const auto bytesRead = stream.read(bytes.getData(), probeBytes);
    if (bytesRead < 20)
    {
        if (detail != nullptr)
            *detail = "WEM probe read too little data for a safe direct-open decision.";
        return false;
    }

    const auto* data = bytes.getData();
    const auto hasRiffHeader = std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0;
    if (!hasRiffHeader)
    {
        if (detail != nullptr)
            *detail = "WEM is not a plain RIFF/WAVE container, forcing vgmstream fallback.";
        return false;
    }

    auto offset = 12;
    while (offset + 8 <= bytesRead)
    {
        const auto* chunk = data + offset;
        const auto chunkSize = static_cast<uint64_t>(readLittleEndian32(chunk + 4));
        if (std::memcmp(chunk, "fmt ", 4) == 0)
        {
            if (chunkSize < 16 || offset + 8 + 16 > bytesRead)
            {
                if (detail != nullptr)
                    *detail = "WEM fmt chunk is truncated, forcing vgmstream fallback.";
                return false;
            }

            const auto formatTag = readLittleEndian16(chunk + 8);
            const auto isDirectSafe = formatTag == 0x0001 || formatTag == 0x0003;
            if (detail != nullptr)
            {
                *detail = isDirectSafe
                    ? "WEM probe found a safe PCM/float RIFF wave payload."
                    : "WEM fmt tag is not plain PCM/float, forcing vgmstream fallback.";
            }
            return isDirectSafe;
        }

        const auto nextOffset = static_cast<uint64_t>(offset) + 8 + chunkSize + (chunkSize & 1);
        if (nextOffset > static_cast<uint64_t>(bytesRead)) break;
        offset = static_cast<int>(nextOffset);
    }

    if (detail != nullptr)
        *detail = "WEM fmt chunk was not found during the direct-open probe.";
    return false;
}
} // namespace

juce::File VgmstreamDecodeService::getCliPath()
{
    const auto configured = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_VGMSTREAM_PATH", {});
    if (configured.isNotEmpty() && juce::File::isAbsolutePath(configured)) return juce::File(configured);
    const auto executableCandidate = findCliFrom(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());
    if (executableCandidate.existsAsFile())
        return executableCandidate;

    const auto workingDirectoryCandidate = findCliFrom(juce::File::getCurrentWorkingDirectory());
    if (workingDirectoryCandidate.existsAsFile())
        return workingDirectoryCandidate;

    return {};
}

bool VgmstreamDecodeService::isAvailable()
{
    return getCliPath().existsAsFile();
}

juce::File VgmstreamDecodeService::getDecodeCacheDirectory()
{
    const auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("SingleEditor")
                          .getChildFile("vgmstream_decode_cache");
    root.createDirectory();
    return root;
}

juce::File VgmstreamDecodeService::getDecodedWaveFile(const juce::File& inputFile)
{
    const auto cacheIdentity = inputFile.getFullPathName()
                             + "|"
                             + juce::String(inputFile.getSize())
                             + "|"
                             + juce::String(inputFile.getLastModificationTime().toMilliseconds());
    const auto key = juce::String::toHexString(static_cast<juce::int64>(cacheIdentity.hashCode64()));
    return getDecodeCacheDirectory().getChildFile(key + ".wav");
}

VgmstreamDecodeService::WemOpenStrategy VgmstreamDecodeService::chooseWemOpenStrategy(const juce::File& inputFile)
{
    WemOpenStrategy strategy;
    strategy.kind = WemOpenStrategy::Kind::decodedFallback;
    strategy.detail = "WEM will be decoded through the vgmstream cache.";

    if (!inputFile.existsAsFile() || !inputFile.hasFileExtension(".wem"))
        return strategy;

    juce::String probeDetail;
    if (looksLikeSafeDirectWaveWem(inputFile, &probeDetail))
    {
        strategy.kind = WemOpenStrategy::Kind::directWaveReader;
        strategy.detail = probeDetail;
        return strategy;
    }

    if (probeDetail.isNotEmpty())
        strategy.detail = probeDetail;

    return strategy;
}

juce::Result VgmstreamDecodeService::ensureDecodedWave(const juce::File& inputFile,
                                                       juce::File& decodedWaveFile,
                                                       juce::String* detail,
                                                       media::OptionalMediaOperation::Ptr operation)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext lifecycleContext(operation ? operation->observedContext() : currentOptionalLifecycleContext());
#endif
    decodedWaveFile = {};
    using S = media::OptionalMediaFailureState;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto baseLimits = operation ? operation->bankLimits() : media::BoundedDecodeProcess::Limits {};
    const auto remaining = [&]
    {
        return operation ? operation->remainingWallTimeMs()
            : static_cast<int>(juce::jmax(0.0, baseLimits.wallTimeMs - (juce::Time::getMillisecondCounterHiRes() - started)));
    };
    const auto proceed = [&](S::Stage stage)
    { return operation ? operation->continueWork(stage) : remaining() > 0; };
    const auto checkpoint = [&] { return proceed(S::Stage::cache); };
    const auto fail = [&](const juce::String& message, S::Reason reason, S::Stage stage)
    {
        if (operation) operation->fail(reason, stage);
        return juce::Result::fail(message);
    };
    if (operation && operation->module() != S::Module::bank)
        return fail("Complete decode requires its admitted bank operation.", S::Reason::ioFailure, S::Stage::admission);
    if (!proceed(S::Stage::admission)) return juce::Result::fail("Decode request ended before source access.");
    if (!inputFile.existsAsFile())
        return fail("Input file does not exist.", S::Reason::ioFailure, S::Stage::admission);

    const auto cli = getCliPath();
    if (!cli.existsAsFile())
        return fail("vgmstream-cli.exe not found.", S::Reason::missingDependency, S::Stage::launch);

    const auto cacheFile = getDecodedWaveFile(inputFile);
    juce::InterProcessLock producerLock(cacheLockName(cacheFile));
    // Same-thread ownership and cancellation-aware waiting also apply to the
    // complete-reader path. Unrelated cache entries share no process-wide lock.
    while (!producerLock.enter(25))
        if (!proceed(S::Stage::cache)) return juce::Result::fail("Decode cache wait ended without producer ownership.");
    if (!proceed(S::Stage::cache)) return juce::Result::fail("Decode request ended after cache wait.");
    const auto context = media::DecodedWaveCache::capture(inputFile, cli,
        operation ? operation->bankPolicy() : "vgmstream-pcm/job30s-2GiB-4GiB-v1");
    if (!context || (operation && context->producerStamp != operation->report().dependencyFingerprint))
    {
        if (operation) operation->noteCacheLookup(media::OptionalMediaOperation::CacheLookup::unavailable,
            context ? media::DecodedWaveCache::LookupReason::contextChanged : media::DecodedWaveCache::LookupReason::contextUnavailable);
        return fail("Optional source/producer identity changed.", S::Reason::ioFailure, S::Stage::admission);
    }
    const auto marker = getDecodeInProgressMarker(cacheFile);
    discardStaleOrFailedDecode(marker, cacheFile);
    if (marker.existsAsFile())
    {
        while (marker.existsAsFile())
        {
            if (!proceed(S::Stage::cache)) return juce::Result::fail("Concurrent decode wait ended without output ownership.");
            juce::Thread::sleep(10);
        }

        media::DecodedWaveCache::LookupReason cacheReason;
        const bool reused = hasUsableDecodedWave(*context, cacheFile, checkpoint, &cacheReason);
        if (operation) operation->noteCacheLookup(reused ? media::OptionalMediaOperation::CacheLookup::reused
                                                        : media::OptionalMediaOperation::CacheLookup::miss, cacheReason);
        if (reused)
        {
            if (!proceed(S::Stage::cache)) return juce::Result::fail("Decode request ended during cache validation.");
            decodedWaveFile = cacheFile;
            if (detail != nullptr)
                *detail = "Reused completed progressive vgmstream decode.";
            return juce::Result::ok();
        }

        // A timeout is not ownership of the other session's live output.
        return fail("Concurrent vgmstream output was not valid.", S::Reason::cacheInvalid, S::Stage::cache);
    }

    media::DecodedWaveCache::LookupReason cacheReason;
    const bool reused = hasUsableDecodedWave(*context, cacheFile, checkpoint, &cacheReason);
    if (operation) operation->noteCacheLookup(reused ? media::OptionalMediaOperation::CacheLookup::reused
                                                    : media::OptionalMediaOperation::CacheLookup::miss, cacheReason);
    if (reused)
    {
        if (!proceed(S::Stage::cache)) return juce::Result::fail("Decode request ended during cache validation.");
        decodedWaveFile = cacheFile;
        if (detail != nullptr)
            *detail = "Reused cached vgmstream decode.";
        return juce::Result::ok();
    }

    if (!proceed(S::Stage::launch)) return juce::Result::fail("Decode request ended before launch.");
    if (cacheFile.getParentDirectory().createDirectory().failed())
        return fail("Decode cache directory is unavailable.", S::Reason::ioFailure, S::Stage::cache);
    const auto staging = cacheFile.getSiblingFile(cacheFile.getFileNameWithoutExtension()
                                                  + "." + juce::Uuid().toString() + ".staging.wav");

    const std::atomic<bool> uncancelled { false };
    auto limits = baseLimits;
    limits.wallTimeMs = remaining();
    if (limits.wallTimeMs <= 0)
    { proceed(S::Stage::launch); return juce::Result::fail("Decode request reached its deadline before launch."); }
    const auto& cancel = operation ? operation->cancelFlag() : uncancelled;
    const auto decoded = media::BoundedDecodeProcess::run(cli,
        { "-i", "-o", staging.getFullPathName(), inputFile.getFullPathName() }, staging, limits, cancel
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , {}, operation ? operation->observedResourceId() : 0
#endif
        );
    if (operation) operation->observeProcess(decoded);
    if (!decoded)
    {
        staging.deleteFile();
        if (operation) operation->processFailed(decoded);
        return juce::Result::fail(media::BoundedDecodeProcess::describe(decoded.failure));
    }
    if (!proceed(S::Stage::cache)) { staging.deleteFile(); return juce::Result::fail("Decode request ended before cache publication."); }
    if (!media::DecodedWaveCache::inspectComplete(staging, checkpoint))
    { staging.deleteFile(); return fail("vgmstream-cli output failed validation.", S::Reason::cacheInvalid, S::Stage::cache); }
    const bool published = proceed(S::Stage::cache) && staging.replaceFileIn(cacheFile)
        && media::DecodedWaveCache::seal(*context, cacheFile, checkpoint);
    if (operation) operation->noteCachePublication(published);
    if (!published)
    {
        staging.deleteFile();
        return fail("vgmstream output could not be committed with its complete cache seal.", S::Reason::cacheInvalid, S::Stage::cache);
    }

    if (!proceed(S::Stage::cache)) return juce::Result::fail("Decode request ended during cache publication.");
    decodedWaveFile = cacheFile;
    if (detail != nullptr)
        *detail = "Decoded media with bounded vgmstream.";
    return juce::Result::ok();
}

std::unique_ptr<juce::AudioFormatReader> VgmstreamDecodeService::createReaderForDecodedFile(const juce::File& inputFile,
                                                                                             juce::StringArray* diagnostics,
                                                                                             media::OptionalMediaOperation::Ptr operation)
{
    juce::File decodedWave;
    juce::String detail;
    const auto result = ensureDecodedWave(inputFile, decodedWave, &detail, operation);
    if (diagnostics != nullptr && detail.isNotEmpty())
        diagnostics->add(detail);

    if (result.failed() || !decodedWave.existsAsFile())
    {
        if (diagnostics != nullptr)
            diagnostics->add(result.getErrorMessage());
        return {};
    }

    juce::WavAudioFormat wavFormat;
    auto stream = std::make_unique<juce::FileInputStream>(decodedWave);
    if (!stream->openedOk())
    {
        if (operation) operation->fail(media::OptionalMediaFailureState::Reason::ioFailure, media::OptionalMediaFailureState::Stage::cache);
        if (diagnostics != nullptr)
            diagnostics->add("Decoded WAV cache could not be opened.");
        return {};
    }

    if (auto* reader = wavFormat.createReaderFor(stream.release(), true))
    {
        if (operation) operation->noteReader(media::OptionalMediaOperation::ReaderKind::bankDecodedWave);
        return std::unique_ptr<juce::AudioFormatReader>(reader);
    }

    if (diagnostics != nullptr)
        diagnostics->add("Decoded WAV cache is not readable.");
    if (operation) operation->fail(media::OptionalMediaFailureState::Reason::cacheInvalid, media::OptionalMediaFailureState::Stage::cache);
    return {};
}

std::unique_ptr<juce::AudioFormatReader> VgmstreamDecodeService::createProgressiveReaderForDecodedFile(
    const juce::File& inputFile, DecodeProgressCallback progressCallback, juce::File& decodedWaveFile,
    juce::StringArray* diagnostics, media::OptionalMediaOperation::Ptr operation)
{
    decodedWaveFile = {};
    const auto cli = getCliPath();
    if (operation && !operation->admitPacked(inputFile, cli)) return {};
    auto fail = [&](const char* message, media::OptionalMediaFailureState::Reason reason)
    {
        if (diagnostics) diagnostics->add(message);
        if (operation) operation->fail(reason, media::OptionalMediaFailureState::Stage::cache);
    };
    if (!inputFile.existsAsFile()) { fail("Input file does not exist.", media::OptionalMediaFailureState::Reason::ioFailure); return {}; }
    if (!cli.existsAsFile()) { fail("vgmstream-cli.exe not found.", media::OptionalMediaFailureState::Reason::missingDependency); return {}; }
    const auto cacheFile = getDecodedWaveFile(inputFile);
    const auto context = media::DecodedWaveCache::capture(inputFile, cli,
        operation ? operation->packedPolicy() : "vgmstream-pcm/job30s-2GiB-4GiB-v1");
    if (!context)
    {
        if (operation) operation->noteCacheLookup(media::OptionalMediaOperation::CacheLookup::unavailable,
            media::DecodedWaveCache::LookupReason::contextUnavailable);
        fail("Unable to capture decode source/producer identity.", media::OptionalMediaFailureState::Reason::ioFailure); return {};
    }
    if (operation)
    {
        const auto admitted = operation->report();
        if (!admitted.key || context->sourceStamp != admitted.key->sourceFingerprint
            || context->producerStamp != admitted.dependencyFingerprint)
        { fail("Decode identity changed during admission.", media::OptionalMediaFailureState::Reason::ioFailure); return {}; }
    }
    auto session = std::make_shared<ProgressiveDecodeSession>(cli, inputFile, cacheFile,
        std::move(progressCallback), *context, operation);
    if (!session->startAndWaitForPlayablePrefix())
    {
        if (diagnostics) diagnostics->add("vgmstream ended without a playable prefix.");
        return {};
    }
    juce::WavAudioFormat wavFormat;
    auto stream = std::make_unique<SessionFileInputStream>(cacheFile, session);
    if (!stream->openedOk())
    { fail("Growing WAV cache could not be opened.", media::OptionalMediaFailureState::Reason::ioFailure); return {}; }
    std::unique_ptr<juce::AudioFormatReader> reader(wavFormat.createReaderFor(stream.release(), true));
    if (!reader || !media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples))
    { fail("Growing WAV cache header was not readable.", media::OptionalMediaFailureState::Reason::cacheInvalid); return {}; }
    decodedWaveFile = cacheFile;
    if (diagnostics) diagnostics->add("Opened vgmstream cache at its actual playable prefix.");
    if (operation) operation->succeed(); // Completion still requires the real decoder/seal.
    auto progressiveReader = std::make_unique<ProgressiveWaveReader>(std::move(reader), session->backing());
    if (operation) operation->noteReader(media::OptionalMediaOperation::ReaderKind::packedProgressiveWave);
    return progressiveReader;
}

bool VgmstreamDecodeService::hasUsableDecodedWave(const media::DecodedWaveCache::Context& context,
                                                 const juce::File& decodedWaveFile, const media::WorkCheckpoint& checkpoint,
                                                 media::DecodedWaveCache::LookupReason* reason)
{ return usableWave(context, decodedWaveFile, checkpoint, reason); }

namespace
{
bool usableWave(const media::DecodedWaveCache::Context& context, const juce::File& decodedWaveFile,
                const media::WorkCheckpoint& checkpoint, media::DecodedWaveCache::LookupReason* observation)
{
    using Reason = media::DecodedWaveCache::LookupReason;
    const auto finish = [&](Reason reason)
    { if (observation) *observation = reason; return reason == Reason::reused; };
    if (getDecodeInProgressMarker(decodedWaveFile).existsAsFile())
        return finish(Reason::incompleteMarker);

    if (!decodedWaveFile.existsAsFile()) return finish(Reason::entryUnavailable);
    if (decodedWaveFile.getSize() <= 44
        || static_cast<uint64_t>(decodedWaveFile.getSize()) > media::BoundedDecodeProcess::Limits().outputBytes)
        return finish(Reason::entrySizeRejected);

    if (!media::DecodedWaveCache::reusable(context, decodedWaveFile, checkpoint, observation)) return false;
    juce::WavAudioFormat wavFormat;
    auto stream = std::make_unique<juce::FileInputStream>(decodedWaveFile);
    if (!stream->openedOk())
        return finish(Reason::readerUnavailable);

    std::unique_ptr<juce::AudioFormatReader> reader(wavFormat.createReaderFor(stream.release(), true));
    const auto info = inspectGrowingWave(decodedWaveFile);
    const bool admitted = reader != nullptr && info.has_value()
        && media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples)
        && reader->lengthInSamples == info->totalSamples
        && reader->numChannels == info->channels && reader->sampleRate == info->sampleRate
        && decodedWaveFile.getSize() >= info->dataStart + info->totalSamples * info->bytesPerFrame;
    return finish(admitted ? Reason::reused : Reason::readerRejected);
}
} // namespace
} // namespace singleeditor
