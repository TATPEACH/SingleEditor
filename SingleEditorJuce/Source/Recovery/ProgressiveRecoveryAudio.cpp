#include "ProgressiveRecoveryAudio.h"
#include <bit>
#include <cstring>
#include <limits>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace singleeditor::recovery
{
namespace
{
#if JUCE_WINDOWS
struct Handle final
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};

// Pin every existing ancestor without delete sharing while creating the file.
// A junction swap must not redirect this allocation into another owner's tree.
bool pinScratchDirectory(juce::File directory, std::vector<std::unique_ptr<Handle>>& pins, bool createFinal = false)
{
    const auto requested = directory;
    std::vector<juce::File> chain;
    for (;;)
    {
        chain.push_back(directory);
        const auto parent = directory.getParentDirectory();
        if (parent == directory) break;
        directory = parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        auto pin = std::make_unique<Handle>();
        pin->value = CreateFileW(it->getFullPathName().toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (createFinal && pin->value == INVALID_HANDLE_VALUE && *it == requested && GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            // Explicit restore may create its final scratch directory only
            // after all existing ancestors are pinned. Never follow a junction
            // or recursively create an unchecked ancestor on the UI thread.
            if (!CreateDirectoryW(it->getFullPathName().toWideCharPointer(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            pin->value = CreateFileW(it->getFullPathName().toWideCharPointer(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        }
        BY_HANDLE_FILE_INFORMATION info {};
        if (pin->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(pin->value, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
        pins.push_back(std::move(pin));
    }
    return true;
}
#endif

// A mapped page may incur OS paging even though this reader makes no file calls.
// Catch storage page faults at primitive leaves (no C++ objects to unwind).
bool copyMapped(void* destination, const void* source, size_t size) noexcept
{
#if JUCE_WINDOWS && defined(_MSC_VER)
    __try { std::memcpy(destination, source, size); }
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
#else
    std::memcpy(destination, source, size);
#endif
    return true;
}
bool readMapped(const char* bytes, int* const* destination, int destinationChannels,
                int offset, int channels, int frames) noexcept
{
#if JUCE_WINDOWS && defined(_MSC_VER)
    __try
#endif
    {
        for (int channel = 0; channel < destinationChannels; ++channel)
        {
            if (destination[channel] == nullptr) continue;
            auto* target = reinterpret_cast<float*>(destination[channel]) + offset;
            for (int frame = 0; frame < frames; ++frame)
                target[frame] = channel < channels
                    ? std::bit_cast<float>(juce::ByteOrder::littleEndianInt(
                        bytes + (static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)) * 4))
                    : 0.0f;
        }
    }
#if JUCE_WINDOWS && defined(_MSC_VER)
    __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
#endif
    return true;
}
} // namespace

struct ProgressiveRecoveryAudio::Storage final
{
    SingleTrackDocument::PlaybackAvailability availability = SingleTrackDocument::PlaybackAvailability::contiguousPrefix(0);
    std::atomic<int64_t> ready { 0 };
    std::atomic<bool> readFault { false };
    int channels = 0;
    int64_t frames = 0;
    double rate = 0;
    void* data = nullptr;
    std::atomic<std::shared_ptr<const DurableReference>> durableReference;
    std::shared_ptr<const void> allocationLease;
#if JUCE_WINDOWS
    Handle file, mapping;
#endif
    ~Storage()
    {
#if JUCE_WINDOWS
        if (data != nullptr) UnmapViewOfFile(data);
        // Mapping then file handle close; delete-on-close targets exactly our
        // created file, never a later occupant of a path. No recursive cleanup.
#endif
    }
    Status allocate(const juce::File& directory, const AudioGraphDescriptor::Sequence& sequence)
    {
#if JUCE_WINDOWS
        std::vector<std::unique_ptr<Handle>> pins;
        if (!pinScratchDirectory(directory, pins)) return { Error::resourceOrEnvironmentFault, "recovery_scratch_directory" };
        const auto bytes = static_cast<uint64_t>(sequence.frames) * static_cast<uint64_t>(sequence.channels) * 4;
        ULARGE_INTEGER freeBytes {};
        if (!GetDiskFreeSpaceExW(directory.getFullPathName().toWideCharPointer(), &freeBytes, nullptr, nullptr)
            || freeBytes.QuadPart < bytes + 64 * 1024 * 1024)
            return { Error::resourceOrEnvironmentFault, "recovery_scratch_space" };
        const auto scratch = directory.getChildFile("recovery-audio-" + juce::Uuid().toString() + ".pcm");
        file.value = CreateFileW(scratch.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE
                                     | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) return { Error::resourceOrEnvironmentFault, "recovery_scratch_create" };
        LARGE_INTEGER length {}; length.QuadPart = static_cast<int64_t>(bytes);
        if (!SetFilePointerEx(file.value, length, nullptr, FILE_BEGIN) || !SetEndOfFile(file.value))
            return { Error::resourceOrEnvironmentFault, "recovery_scratch_size" };
        mapping.value = CreateFileMappingW(file.value, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (mapping.value == nullptr) return { Error::resourceOrEnvironmentFault, "recovery_scratch_mapping" };
        data = MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
        if (data == nullptr) return { Error::resourceOrEnvironmentFault, "recovery_scratch_view" };
        channels = sequence.channels; frames = sequence.frames; rate = sequence.sampleRate;
        return {};
#else
        juce::ignoreUnused(directory, sequence);
        return { Error::resourceOrEnvironmentFault, "recovery_scratch_platform_unsupported" };
#endif
    }
};

class ProgressiveRecoveryAudio::Reader final : public juce::AudioFormatReader
{
public:
    std::shared_ptr<const DurableReference> reference() const { return data->durableReference.load(std::memory_order_acquire); }
    explicit Reader(std::shared_ptr<Storage> source) : AudioFormatReader(nullptr, "Verified Recovery PCM"), data(std::move(source))
    {
        sampleRate = data->rate; bitsPerSample = 32; lengthInSamples = data->frames;
        numChannels = static_cast<unsigned int>(data->channels); usesFloatingPointData = true;
    }
    bool readSamples(int* const* destination, int destinationChannels, int offset, juce::int64 start, int frames) override
    {
        if (destination == nullptr || destinationChannels < 1 || destinationChannels > 256 || offset < 0 || frames < 0
            || offset > std::numeric_limits<int>::max() - frames || start < 0) return false;
        const auto clear = [&]
        {
            for (int channel = 0; channel < destinationChannels; ++channel)
                if (destination[channel] != nullptr)
                    juce::FloatVectorOperations::clear(reinterpret_cast<float*>(destination[channel]) + offset, frames);
        };
        const auto count = static_cast<int>(std::min<int64_t>(frames, std::max<int64_t>(0, lengthInSamples - start)));
        // Do not trust an externally copied PlaybackAvailability: this private
        // release/acquire fence is the authority for prepared mapping bytes.
        const auto ready = data->ready.load(std::memory_order_acquire);
        if (data->readFault.load(std::memory_order_acquire) || (count > 0 && (start > ready || count > ready - start)))
        { clear(); return false; }
        clear();
        if (count == 0) return true;
        const auto* bytes = static_cast<const char*>(data->data) + static_cast<size_t>(start) * numChannels * 4;
        if (readMapped(bytes, destination, destinationChannels, offset, data->channels, count)) return true;
        data->readFault.store(true, std::memory_order_release);
        data->availability.markUnavailable();
        clear();
        return false;
    }
private:
    std::shared_ptr<Storage> data;
};

ProgressiveRecoveryAudio::ProgressiveRecoveryAudio(std::shared_ptr<Storage> backing, AudioGraphDescriptor::Sequence plan, size_t assetBytes)
    : storage(std::move(backing)), reader(std::make_shared<Reader>(storage)), sequence(std::move(plan)), maximumAssetBytes(assetBytes) {}
ProgressiveRecoveryAudio::~ProgressiveRecoveryAudio() = default;

Status ProgressiveRecoveryAudio::prepareScratchDirectory(const juce::File& directory) try
{
    if (directory == juce::File()) return { Error::invalidPacket, "recovery_scratch_directory" };
#if JUCE_WINDOWS
    std::vector<std::unique_ptr<Handle>> pins;
    if (!pinScratchDirectory(directory, pins, true)) return { Error::resourceOrEnvironmentFault, "recovery_scratch_directory" };
    return {};
#else
    return { Error::resourceOrEnvironmentFault, "recovery_scratch_platform_unsupported" };
#endif
}
catch (...) { return { Error::resourceOrEnvironmentFault, "recovery_scratch_directory" }; }

Status ProgressiveRecoveryAudio::create(const juce::String& json, size_t stateIndex, const juce::File& directory,
    std::unique_ptr<ProgressiveRecoveryAudio>& output, AudioGraphLimits limits, const RecoveryStore* durableSource,
    std::shared_ptr<const void> allocationLease)
{
    AudioGraphDescriptor graph;
    const auto described = RecoveryAudioGraph::describe(json, graph, limits);
    if (!described) return described;
    if (stateIndex >= graph.states.size()) return { Error::invalidPacket, "recovery_audio_state_index" };
    try
    {
        auto plan = std::move(graph.sequences[graph.states[stateIndex]]);
        auto backing = std::make_shared<Storage>();
        backing->allocationLease = std::move(allocationLease);
        if (durableSource != nullptr)
        {
            if (durableSource->rootDirectory() == juce::File() || durableSource->ownerId().isNull())
                return { Error::invalidPacket, "recovery_audio_source_contract" };
            std::shared_ptr<RecoveryOwnerLease> lease;
            const auto acquired = durableSource->acquireOwnerLease(lease);
            if (!acquired) return acquired;
            backing->durableReference.store(std::make_shared<const DurableReference>(DurableReference { *durableSource, plan, std::move(lease) }),
                std::memory_order_release);
        }
        const auto allocated = backing->allocate(directory, plan);
        if (!allocated) return allocated;
        auto candidate = std::unique_ptr<ProgressiveRecoveryAudio>(new ProgressiveRecoveryAudio(std::move(backing), std::move(plan), limits.assetBytes));
        output = std::move(candidate);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "recovery_audio_create_exception" }; }
}

Status ProgressiveRecoveryAudio::loadNext(const RecoveryStore& store, RecoveryAudioGraph::Cancel cancel)
{
    if (!terminalStatus) return terminalStatus;
    const auto fail = [&] (Status status)
    {
        terminalStatus = std::move(status);
        storage->availability.sealContiguousPrefix();
        return terminalStatus;
    };
    if (storage->readFault.load(std::memory_order_acquire))
        return fail({ Error::resourceOrEnvironmentFault, "recovery_audio_read_fault" });
    if (isComplete()) return {};
    try
    {
        if (cancel && cancel()) return fail({ Error::cancelled, "recovery_audio_cancelled" });
        const auto reference = storage->durableReference.load(std::memory_order_acquire);
        if (reference && (reference->store.ownerId() != store.ownerId() || reference->store.rootDirectory() != store.rootDirectory()))
            return fail({ Error::sourceConflict, "recovery_audio_owner_changed" });
        if (!reference)
        {
            std::shared_ptr<RecoveryOwnerLease> lease;
            const auto acquired = store.acquireOwnerLease(lease);
            if (!acquired) return fail(acquired);
            storage->durableReference.store(std::make_shared<const DurableReference>(DurableReference { store, sequence, std::move(lease) }),
                std::memory_order_release);
        }
        const auto& chunk = sequence.chunks[nextChunk];
        AudioBlock block;
        const auto status = store.readAudio(chunk.id, block);
        if (!status) return fail(status);
        const auto bytes = static_cast<size_t>(chunk.frames) * static_cast<size_t>(sequence.channels) * 4;
        if (block.sampleRate != sequence.sampleRate || block.channels != sequence.channels || block.frames != chunk.frames
            || block.samples.getSize() != bytes || bytes > maximumAssetBytes)
            return fail({ Error::missingOrCorruptAsset, "recovery_audio_chunk_dimensions" });
        if (cancel && cancel()) return fail({ Error::cancelled, "recovery_audio_cancelled" });
        auto* destination = static_cast<char*>(storage->data) + static_cast<size_t>(chunk.start) * static_cast<size_t>(sequence.channels) * 4;
        if (!copyMapped(destination, block.samples.getData(), bytes))
            return fail({ Error::resourceOrEnvironmentFault, "recovery_audio_write_fault" });
        if (cancel && cancel()) return fail({ Error::cancelled, "recovery_audio_cancelled" });
        // Writes only beyond the old prefix; concurrently reading published
        // samples never races a write. No allocation, disk read or hash in Reader.
        ++nextChunk;
        storage->ready.store(chunk.start + chunk.frames, std::memory_order_release);
        storage->availability.advanceContiguousPrefix(chunk.start + chunk.frames);
        if (isComplete()) storage->availability.markRandomAccess();
        if (storage->readFault.load(std::memory_order_acquire))
        {
            storage->availability.markUnavailable();
            return fail({ Error::resourceOrEnvironmentFault, "recovery_audio_read_fault" });
        }
        return {};
    }
    catch (...) { return fail({ Error::resourceOrEnvironmentFault, "recovery_audio_load_exception" }); }
}

std::shared_ptr<juce::AudioFormatReader> ProgressiveRecoveryAudio::getReader() const noexcept { return reader; }
std::shared_ptr<const ProgressiveRecoveryAudio::DurableReference> ProgressiveRecoveryAudio::getDurableReference(
    const std::shared_ptr<juce::AudioFormatReader>& source)
{
    const auto* verified = dynamic_cast<const Reader*>(source.get());
    return verified != nullptr ? verified->reference() : nullptr;
}
void ProgressiveRecoveryAudio::cancel()
{
    if (terminalStatus && !isComplete())
    {
        terminalStatus = { Error::cancelled, "recovery_audio_cancelled" };
        storage->availability.sealContiguousPrefix();
    }
}
Status ProgressiveRecoveryAudio::bindMetadata(const SingleTrackDocument::State& metadata, SingleTrackDocument::StatePtr& output) const
{
    if (metadata.info.sampleRate != sequence.sampleRate || metadata.info.numChannels != sequence.channels
        || metadata.info.numSamples != sequence.frames)
        return { Error::invalidPacket, "recovery_audio_metadata_dimensions" };
    try
    {
        auto state = std::make_shared<SingleTrackDocument::State>(metadata);
        state->audioBuffer.reset(); state->memoryMappedReader.reset(); state->readerSourceFile = {};
        state->streamingReader = reader;
        state->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::recoveryStream;
        state->playbackAvailability = getAvailability(); state->info.recoveryBacked = true;
        output = std::move(state);
        return {};
    }
    catch (...) { return { Error::resourceOrEnvironmentFault, "recovery_audio_bind_exception" }; }
}
SingleTrackDocument::PlaybackAvailability ProgressiveRecoveryAudio::getAvailability() const noexcept { return storage->availability; }
int64_t ProgressiveRecoveryAudio::readyFrames() const noexcept { return storage->ready.load(std::memory_order_acquire); }
bool ProgressiveRecoveryAudio::isComplete() const noexcept
{
    return !storage->readFault.load(std::memory_order_acquire) && readyFrames() == sequence.frames;
}
} // namespace singleeditor::recovery
