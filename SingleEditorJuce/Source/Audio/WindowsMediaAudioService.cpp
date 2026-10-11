#include "Audio/WindowsMediaAudioService.h"
#include "Audio/BoundedDecodeProcess.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/DecodedWaveCache.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

#if JUCE_WINDOWS
#define INITGUID
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

namespace singleeditor
{
namespace
{
std::atomic<bool> mediaRuntimeLoaded { false };

#if JUCE_WINDOWS
template <typename Type>
using ComPtr = Microsoft::WRL::ComPtr<Type>;

class ScopedCom final
{
public:
    ScopedCom()
    {
        result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ownsInitialisation = SUCCEEDED(result);
    }

    ~ScopedCom()
    {
        if (ownsInitialisation)
            ::CoUninitialize();
    }

    bool isUsable() const noexcept
    {
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result = E_FAIL;
    bool ownsInitialisation = false;
};

class MediaFoundationRuntime final
{
public:
    using StartupFn = HRESULT (WINAPI*)(ULONG, DWORD);
    using ShutdownFn = HRESULT (WINAPI*)();
    using CreateMediaTypeFn = HRESULT (WINAPI*)(IMFMediaType**);
    using CreateMemoryBufferFn = HRESULT (WINAPI*)(DWORD, IMFMediaBuffer**);
    using CreateSampleFn = HRESULT (WINAPI*)(IMFSample**);
    using CreateSinkWriterFromUrlFn = HRESULT (WINAPI*)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**);
    using CreateSourceReaderFromUrlFn = HRESULT (WINAPI*)(LPCWSTR, IMFAttributes*, IMFSourceReader**);

    ~MediaFoundationRuntime()
    {
        if (started && shutdown != nullptr)
            shutdown();
    }

    bool ensureStarted(juce::String& errorMessage)
    {
        const std::scoped_lock lock(mutex);
        if (started)
            return true;
        if (loadAttempted)
        {
            errorMessage = loadError;
            return false;
        }

        loadAttempted = true;
        mediaRuntimeLoaded.store(true, std::memory_order_release);
        if (!mfplat.open("mfplat.dll") || !mfreadwrite.open("mfreadwrite.dll"))
            return fail("Windows Media Foundation DLLs are unavailable.", errorMessage);

        startup = load<StartupFn>(mfplat, "MFStartup");
        shutdown = load<ShutdownFn>(mfplat, "MFShutdown");
        createMediaType = load<CreateMediaTypeFn>(mfplat, "MFCreateMediaType");
        createMemoryBuffer = load<CreateMemoryBufferFn>(mfplat, "MFCreateMemoryBuffer");
        createSample = load<CreateSampleFn>(mfplat, "MFCreateSample");
        createSinkWriterFromUrl = load<CreateSinkWriterFromUrlFn>(mfreadwrite, "MFCreateSinkWriterFromURL");
        createSourceReaderFromUrl = load<CreateSourceReaderFromUrlFn>(mfreadwrite, "MFCreateSourceReaderFromURL");
        if (startup == nullptr || shutdown == nullptr || createMediaType == nullptr
            || createMemoryBuffer == nullptr || createSample == nullptr
            || createSinkWriterFromUrl == nullptr || createSourceReaderFromUrl == nullptr)
        {
            return fail("Windows Media Foundation entry points are incomplete.", errorMessage);
        }

        const auto status = startup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(status))
            return fail("Windows Media Foundation could not start (HRESULT " + hresultText(status) + ").",
                        errorMessage);

        started = true;
        return true;
    }

    CreateMediaTypeFn createMediaType = nullptr;
    CreateMemoryBufferFn createMemoryBuffer = nullptr;
    CreateSampleFn createSample = nullptr;
    CreateSinkWriterFromUrlFn createSinkWriterFromUrl = nullptr;
    CreateSourceReaderFromUrlFn createSourceReaderFromUrl = nullptr;

    static juce::String hresultText(HRESULT result)
    {
        return "0x" + juce::String::toHexString(static_cast<juce::int64>(static_cast<unsigned long>(result)));
    }

private:
    template <typename Function>
    static Function load(juce::DynamicLibrary& library, const char* name)
    {
        return reinterpret_cast<Function>(library.getFunction(name));
    }

    bool fail(const juce::String& message, juce::String& errorMessage)
    {
        loadError = message;
        errorMessage = message;
        return false;
    }

    std::mutex mutex;
    juce::DynamicLibrary mfplat;
    juce::DynamicLibrary mfreadwrite;
    StartupFn startup = nullptr;
    ShutdownFn shutdown = nullptr;
    bool loadAttempted = false;
    bool started = false;
    juce::String loadError;
};

MediaFoundationRuntime& runtime()
{
    static MediaFoundationRuntime instance;
    return instance;
}

bool setAttribute(IMFAttributes& attributes, REFGUID key, UINT32 value, juce::String& errorMessage)
{
    const auto status = attributes.SetUINT32(key, value);
    if (SUCCEEDED(status))
        return true;

    errorMessage = "Media Foundation rejected an audio attribute (HRESULT "
                 + MediaFoundationRuntime::hresultText(status) + ").";
    return false;
}

bool setAttribute(IMFAttributes& attributes, REFGUID key, REFGUID value, juce::String& errorMessage)
{
    const auto status = attributes.SetGUID(key, value);
    if (SUCCEEDED(status))
        return true;

    errorMessage = "Media Foundation rejected an audio format (HRESULT "
                 + MediaFoundationRuntime::hresultText(status) + ").";
    return false;
}

std::unique_ptr<juce::AudioFormatReader> createWaveReader(const juce::File& file)
{
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileInputStream>(file);
    if (!stream->openedOk())
        return {};
    return std::unique_ptr<juce::AudioFormatReader>(format.createReaderFor(stream.release(), true));
}

bool writePcmSample(MediaFoundationRuntime& media,
                    IMFSinkWriter& writer,
                    DWORD streamIndex,
                    const juce::AudioBuffer<float>& audio,
                    int64_t sampleOffset,
                    double sampleRate,
                    juce::String& errorMessage)
{
    const auto channels = audio.getNumChannels();
    const auto samples = audio.getNumSamples();
    const auto byteCount = static_cast<DWORD>(channels * samples * static_cast<int>(sizeof(int16_t)));

    ComPtr<IMFMediaBuffer> buffer;
    auto status = media.createMemoryBuffer(byteCount, buffer.GetAddressOf());
    if (FAILED(status))
    {
        errorMessage = "MP3 input buffer allocation failed (HRESULT "
                     + MediaFoundationRuntime::hresultText(status) + ").";
        return false;
    }

    BYTE* bytes = nullptr;
    DWORD maximumLength = 0;
    status = buffer->Lock(&bytes, &maximumLength, nullptr);
    if (FAILED(status) || maximumLength < byteCount)
    {
        if (SUCCEEDED(status))
            buffer->Unlock();
        errorMessage = "MP3 input buffer lock failed.";
        return false;
    }

    auto* interleaved = reinterpret_cast<int16_t*>(bytes);
    for (int sample = 0; sample < samples; ++sample)
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto value = juce::jlimit(-1.0f, 1.0f, audio.getSample(channel, sample));
            *interleaved++ = static_cast<int16_t>(std::lrint(value * 32767.0f));
        }
    }
    buffer->Unlock();
    if (FAILED(buffer->SetCurrentLength(byteCount)))
    {
        errorMessage = "MP3 input buffer length could not be committed.";
        return false;
    }

    ComPtr<IMFSample> mediaSample;
    status = media.createSample(mediaSample.GetAddressOf());
    if (FAILED(status) || FAILED(mediaSample->AddBuffer(buffer.Get())))
    {
        errorMessage = "MP3 sample allocation failed.";
        return false;
    }

    constexpr double ticksPerSecond = 10000000.0;
    mediaSample->SetSampleTime(static_cast<LONGLONG>(std::llround(sampleOffset * ticksPerSecond / sampleRate)));
    mediaSample->SetSampleDuration(static_cast<LONGLONG>(std::llround(samples * ticksPerSecond / sampleRate)));
    status = writer.WriteSample(streamIndex, mediaSample.Get());
    if (FAILED(status))
    {
        errorMessage = "MP3 encoder rejected an audio sample (HRESULT "
                     + MediaFoundationRuntime::hresultText(status) + ").";
        return false;
    }
    return true;
}

juce::File videoCacheFileFor(const juce::File& source)
{
    const auto identity = source.getFullPathName() + "|" + juce::String(source.getSize()) + "|"
                        + source.getLastModificationTime().toISO8601(true);
    const auto digest = juce::String::toHexString(identity.hashCode64());
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("SingleEditor")
        .getChildFile("media_cache")
        .getChildFile("video-audio-bounded-v1-" + digest + ".wav");
}

juce::File findFfmpegExecutable()
{
    const auto configured = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_FFMPEG_PATH", {});
    if (configured.isNotEmpty())
        return juce::File::isAbsolutePath(configured) && juce::File(configured).existsAsFile()
                 ? juce::File(configured) : juce::File();

    const auto executableDirectory = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                         .getParentDirectory();
    const std::array<juce::File, 3> packagedCandidates {{
        executableDirectory.getChildFile("ffmpeg.exe"),
        executableDirectory.getChildFile("MediaRuntime").getChildFile("ffmpeg.exe"),
        executableDirectory.getChildFile("Modules").getChildFile("Media").getChildFile("ffmpeg.exe"),
    }};
    for (const auto& candidate : packagedCandidates)
        if (candidate.existsAsFile())
            return candidate;

    // Preserve the explicitly installed command-line runtime, resolved only on
    // a video request. Never search the selected media directory or invoke a shell.
    const auto searchPath = juce::SystemStats::getEnvironmentVariable("PATH", {});
    for (const auto& directory : juce::StringArray::fromTokens(searchPath, ";", "\""))
    {
        const auto path = directory.trim().unquoted();
        if (!juce::File::isAbsolutePath(path)) continue;
        const auto candidate = juce::File(path).getChildFile("ffmpeg.exe");
        if (candidate.existsAsFile()) return candidate;
    }
    return {};
}
#endif
} // namespace

bool WindowsMediaAudioService::isSupportedVideoExtension(const juce::String& extension) noexcept
{
    const auto ext = extension.toLowerCase();
    return ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".avi"
        || ext == ".wmv" || ext == ".mkv" || ext == ".webm";
}

bool WindowsMediaAudioService::isRuntimeLoadedForTests() noexcept
{
    return mediaRuntimeLoaded.load(std::memory_order_acquire);
}

bool WindowsMediaAudioService::encodeWaveFileToMp3(const juce::File& sourceWaveFile,
                                                   const juce::File& targetMp3File,
                                                   int bitrateKbps,
                                                   juce::String& errorMessage)
{
#if JUCE_WINDOWS
    ScopedCom com;
    if (!com.isUsable())
    {
        errorMessage = "COM could not be initialised for MP3 encoding.";
        return false;
    }

    auto& media = runtime();
    if (!media.ensureStarted(errorMessage))
        return false;

    auto reader = createWaveReader(sourceWaveFile);
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        errorMessage = "The PCM staging WAV could not be opened.";
        return false;
    }
    if (reader->numChannels == 0 || reader->numChannels > 2)
    {
        errorMessage = "The Windows MP3 encoder supports mono or stereo output only.";
        return false;
    }
    if (!juce::Array<int> { 96, 128, 160, 192, 256, 320 }.contains(bitrateKbps))
    {
        errorMessage = "The requested MP3 bitrate is unsupported.";
        return false;
    }

    targetMp3File.getParentDirectory().createDirectory();
    targetMp3File.deleteFile();
    ComPtr<IMFSinkWriter> writer;
    auto status = media.createSinkWriterFromUrl(targetMp3File.getFullPathName().toWideCharPointer(),
                                                nullptr,
                                                nullptr,
                                                writer.GetAddressOf());
    if (FAILED(status))
    {
        errorMessage = "MP3 sink could not be created (HRESULT "
                     + MediaFoundationRuntime::hresultText(status) + ").";
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    status = media.createMediaType(outputType.GetAddressOf());
    const auto channels = static_cast<UINT32>(reader->numChannels);
    const auto sampleRate = static_cast<UINT32>(std::llround(reader->sampleRate));
    const auto bitrate = static_cast<UINT32>(bitrateKbps * 1000);
    if (FAILED(status)
        || !setAttribute(*outputType.Get(), MF_MT_MAJOR_TYPE, MFMediaType_Audio, errorMessage)
        || !setAttribute(*outputType.Get(), MF_MT_SUBTYPE, MFAudioFormat_MP3, errorMessage)
        || !setAttribute(*outputType.Get(), MF_MT_AUDIO_NUM_CHANNELS, channels, errorMessage)
        || !setAttribute(*outputType.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate, errorMessage)
        || !setAttribute(*outputType.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8u, errorMessage))
    {
        targetMp3File.deleteFile();
        return false;
    }

    DWORD streamIndex = 0;
    status = writer->AddStream(outputType.Get(), &streamIndex);
    if (FAILED(status))
    {
        errorMessage = "No compatible Windows MP3 encoder is installed (HRESULT "
                     + MediaFoundationRuntime::hresultText(status) + ").";
        targetMp3File.deleteFile();
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    status = media.createMediaType(inputType.GetAddressOf());
    constexpr UINT32 bitsPerSample = 16;
    const auto blockAlignment = channels * (bitsPerSample / 8u);
    if (FAILED(status)
        || !setAttribute(*inputType.Get(), MF_MT_MAJOR_TYPE, MFMediaType_Audio, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_SUBTYPE, MFAudioFormat_PCM, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_AUDIO_NUM_CHANNELS, channels, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlignment, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                         sampleRate * blockAlignment, errorMessage)
        || !setAttribute(*inputType.Get(), MF_MT_ALL_SAMPLES_INDEPENDENT, 1u, errorMessage))
    {
        targetMp3File.deleteFile();
        return false;
    }

    status = writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr);
    if (FAILED(status) || FAILED(writer->BeginWriting()))
    {
        errorMessage = "The Windows MP3 encoder could not begin writing.";
        targetMp3File.deleteFile();
        return false;
    }

    constexpr int chunkSamples = 4096;
    juce::AudioBuffer<float> audio(static_cast<int>(channels), chunkSamples);
    for (int64_t offset = 0; offset < reader->lengthInSamples; offset += chunkSamples)
    {
        const auto count = static_cast<int>(juce::jmin<int64_t>(chunkSamples, reader->lengthInSamples - offset));
        audio.setSize(static_cast<int>(channels), count, false, false, true);
        audio.clear();
        if (!reader->read(&audio, 0, count, offset, true, channels > 1)
            || !writePcmSample(media, *writer.Get(), streamIndex, audio, offset, reader->sampleRate, errorMessage))
        {
            writer.Reset();
            targetMp3File.deleteFile();
            return false;
        }
    }

    status = writer->Finalize();
    writer.Reset();
    if (FAILED(status) || !targetMp3File.existsAsFile() || targetMp3File.getSize() <= 0)
    {
        errorMessage = "The Windows MP3 encoder could not finalise the file.";
        targetMp3File.deleteFile();
        return false;
    }
    return true;
#else
    juce::ignoreUnused(sourceWaveFile, targetMp3File, bitrateKbps);
    errorMessage = "MP3 writing is currently available on Windows only.";
    return false;
#endif
}

static juce::File decodeVideoAudioWithFfmpeg(const juce::File& videoFile,
                                             juce::String& errorMessage,
                                             const media::OptionalMediaOperation::Ptr& operation)
{
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    const ScopedOptionalLifecycleContext lifecycleContext(operation ? operation->observedContext() : currentOptionalLifecycleContext());
#endif
#if JUCE_WINDOWS
    if (!videoFile.existsAsFile())
    {
        errorMessage = "The selected video source does not exist.";
        return {};
    }

    const auto ffmpeg = findFfmpegExecutable();
    if (operation && !operation->admitVideo(videoFile, ffmpeg))
    { errorMessage = "Video audio was not started: session retry policy declined this request."; return {}; }
    const auto context = media::DecodedWaveCache::capture(videoFile, ffmpeg,
        operation ? operation->videoPolicy() : "ffmpeg-pcm16/file-only/job600s-2GiB-4GiB-v1");
    if (!context)
    {
        if (operation) operation->noteCacheLookup(media::OptionalMediaOperation::CacheLookup::unavailable,
            media::DecodedWaveCache::LookupReason::contextUnavailable);
        errorMessage = "FFmpeg is not installed or its source/producer identity is unavailable.";
        if (operation) operation->fail(ffmpeg.existsAsFile() ? media::OptionalMediaFailureState::Reason::ioFailure
            : media::OptionalMediaFailureState::Reason::missingDependency, media::OptionalMediaFailureState::Stage::admission);
        return {};
    }
    const auto cacheFile = videoCacheFileFor(videoFile);
    if (operation && (operation->report().key->sourceFingerprint != context->sourceStamp
        || operation->report().dependencyFingerprint != context->producerStamp))
    {
        operation->fail(media::OptionalMediaFailureState::Reason::ioFailure, media::OptionalMediaFailureState::Stage::admission);
        errorMessage = "Source or decoder changed during admission; reopen to review its new identity.";
        return {};
    }
    media::DecodedWaveCache::LookupReason cacheReason;
    const bool reused = media::DecodedWaveCache::reusable(*context, cacheFile, {}, &cacheReason);
    if (operation) operation->noteCacheLookup(reused ? media::OptionalMediaOperation::CacheLookup::reused
                                                    : media::OptionalMediaOperation::CacheLookup::miss, cacheReason);
    if (reused)
    {
        if (operation) operation->completeDecodedAudio();
        return cacheFile;
    }

    cacheFile.getParentDirectory().createDirectory();
    const auto temporaryFile = cacheFile.getSiblingFile("." + cacheFile.getFileNameWithoutExtension()
                                                        + "-ffmpeg-" + juce::Uuid().toString() + ".wav");
    temporaryFile.deleteFile();

    const auto ext = videoFile.getFileExtension().toLowerCase();
    const auto format = (ext == ".mkv" || ext == ".webm") ? "matroska,webm"
                      : ext == ".avi" ? "avi" : ext == ".wmv" ? "asf"
                      : "mov,mp4,m4a,3gp,3g2,mj2";
    juce::StringArray arguments { "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-max_alloc", "268435456", "-threads", "2", "-protocol_whitelist", "file",
        "-format_whitelist", format, "-probesize", "8388608", "-analyzeduration", "10000000" };
    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov")
        arguments.addArray({ "-enable_drefs", "0", "-use_absolute_path", "0" });
    arguments.addArray({ "-i", videoFile.getFullPathName(), "-map", "0:a:0", "-vn",
                        "-threads", "2", "-c:a", "pcm_s16le", "-f", "wav", temporaryFile.getFullPathName() });
    media::BoundedDecodeProcess::Limits limits;
    limits.wallTimeMs = 600000;
    if (operation) limits = operation->videoLimits();
    const std::atomic<bool> cancel { false };
    const auto decoded = media::BoundedDecodeProcess::run(ffmpeg, arguments, temporaryFile, limits,
        operation ? operation->cancelFlag() : cancel
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , {}, operation ? operation->observedResourceId() : 0
#endif
        );
    if (operation) operation->observeProcess(decoded);
    if (!decoded)
    {
        temporaryFile.deleteFile();
        errorMessage = media::BoundedDecodeProcess::describe(decoded.failure);
        if (operation) operation->processFailed(decoded);
        return {};
    }

    if (!media::DecodedWaveCache::inspectComplete(temporaryFile))
    {
        temporaryFile.deleteFile();
        errorMessage = "FFmpeg produced an invalid video audio cache.";
        if (operation) operation->fail(media::OptionalMediaFailureState::Reason::cacheInvalid,
            media::OptionalMediaFailureState::Stage::cache);
        return {};
    }

    if (operation && operation->cancelFlag().load(std::memory_order_acquire))
    { temporaryFile.deleteFile(); operation->cancelled(); errorMessage = "Video audio cancelled."; return {}; }
    const bool published = temporaryFile.replaceFileIn(cacheFile) && media::DecodedWaveCache::seal(*context, cacheFile);
    if (operation) operation->noteCachePublication(published);
    if (!published)
    {
        temporaryFile.deleteFile();
        errorMessage = "The FFmpeg video audio cache could not be committed.";
        if (operation) operation->fail(media::OptionalMediaFailureState::Reason::cacheInvalid,
            media::OptionalMediaFailureState::Stage::cache);
        return {};
    }

    if (operation) operation->completeDecodedAudio();
    return cacheFile;
#else
    juce::ignoreUnused(videoFile, operation);
    errorMessage = "FFmpeg video fallback is currently available on Windows only.";
    return {};
#endif
}

juce::File WindowsMediaAudioService::getOrCreateVideoAudioCache(const juce::File& videoFile,
                                                                juce::String& errorMessage,
                                                                const media::OptionalMediaOperation::Ptr& operation)
{
    // Arbitrary video is always optional and out of process. The in-process
    // Media Foundation path remains only for encoding a trusted editor WAV.
    if (const auto cache = decodeVideoAudioWithFfmpeg(videoFile, errorMessage, operation); cache.existsAsFile())
    {
        errorMessage.clear();
        return cache;
    }

    return {};
}
} // namespace singleeditor
