#include "SourceFileLease.h"
#include "Core/NativeFilePath.h"
#include <vector>
#include <cstring>
#include <cwchar>
#include <mutex>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace singleeditor::media
{
namespace
{
#if JUCE_WINDOWS
struct Handle final
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
};
int64_t ticks(const FILETIME& value)
{ return static_cast<int64_t>((static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime); }
juce::Result observe(HANDLE handle, const juce::File& file, SourceFileWitness& output)
{
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO stable{};
    if (!GetFileInformationByHandle(handle, &info) || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileInformationByHandleEx(handle, FileIdInfo, &stable, sizeof(stable))
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
        || basic.LastWriteTime.QuadPart <= 0 || basic.ChangeTime.QuadPart <= 0)
        return juce::Result::fail("source_lease_facts");
    const auto size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const auto id = juce::String::toHexString(stable.FileId.Identifier, sizeof(stable.FileId.Identifier), 0);
    if (size > static_cast<uint64_t>(INT64_MAX) || id == juce::String::repeatedString("0", 32)) return juce::Result::fail("source_lease_identity");
    output = {};
    output.file = file;
    output.volumeId = juce::String::toHexString(stable.VolumeSerialNumber).paddedLeft('0', 16);
    output.fileId = id; output.sizeBytes = static_cast<int64_t>(size);
    output.modifiedTicks = ticks(info.ftLastWriteTime); output.changedTicks = basic.ChangeTime.QuadPart;
    return juce::Result::ok();
}
#endif
}
SourceFileWitness::SourceFileWitness(const SourceFileWitness& other) { *this = other; }
SourceFileWitness& SourceFileWitness::operator=(const SourceFileWitness& other)
{
    if (this == &other) return *this;
    file = other.file; volumeId = other.volumeId; fileId = other.fileId;
    sizeBytes = other.sizeBytes; modifiedTicks = other.modifiedTicks; changedTicks = other.changedTicks;
    readerFormat = other.readerFormat; sampleRate = other.sampleRate; channels = other.channels;
    bitsPerSample = other.bitsPerSample; frames = other.frames;
    retained.store(other.retainedInput(), std::memory_order_release);
    return *this;
}
bool SourceFileWitness::operator==(const SourceFileWitness& other) const noexcept
{
    return sameFileVersion(other) && readerFormat == other.readerFormat && sampleRate == other.sampleRate
        && channels == other.channels && bitsPerSample == other.bitsPerSample && frames == other.frames;
}
std::shared_ptr<SourceFileLease> SourceFileWitness::retainedInput() const noexcept
{
    auto source = retained.load(std::memory_order_acquire);
    return source && source->protectsInput(*this) ? source : nullptr;
}
struct SourceFileLease::Impl final
{
#if JUCE_WINDOWS
    std::vector<std::unique_ptr<Handle>> parents;
    Handle file;
    bool writable = false;
    bool replacementSource = false, retained = false;
    mutable std::mutex readMutex;
#endif
};
SourceFileLease::SourceFileLease() : impl(std::make_unique<Impl>()) {}
SourceFileLease::~SourceFileLease() = default;
bool SourceFileLease::protectsInput(const SourceFileWitness& input) const noexcept
{
#if JUCE_WINDOWS
    return impl->retained && originalVersion && *originalVersion == input;
#else
    juce::ignoreUnused(input);
    return false;
#endif
}
juce::Result SourceFileLease::open(const juce::File& file, std::unique_ptr<SourceFileLease>& output)
{ return openInternal(file, output, false); }
juce::Result SourceFileLease::openStagedWrite(const juce::File& file, std::unique_ptr<SourceFileLease>& output)
{ return openInternal(file, output, true); }
juce::Result SourceFileLease::openInternal(const juce::File& file, std::unique_ptr<SourceFileLease>& output, bool write, bool replacementSource) try
{
    if (file == juce::File() || file.getFullPathName().length() > 4096) return juce::Result::fail("source_lease_path");
#if JUCE_WINDOWS
    auto lease = std::unique_ptr<SourceFileLease>(new SourceFileLease());
    std::vector<juce::File> parents;
    for (auto path = file.getParentDirectory();; path = path.getParentDirectory())
    {
        if (parents.size() >= 128) return juce::Result::fail("source_lease_depth");
        parents.push_back(path);
        if (path == path.getParentDirectory()) break;
    }
    for (auto it = parents.rbegin(); it != parents.rend(); ++it)
    {
        auto pin = std::make_unique<Handle>();
        pin->value = CreateFileW(files::nativePath(*it).toWideCharPointer(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (pin->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(pin->value, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return juce::Result::fail("source_lease_parent");
        lease->impl->parents.push_back(std::move(pin));
    }
    lease->impl->file.value = CreateFileW(files::nativePath(file).toWideCharPointer(), GENERIC_READ | (write ? GENERIC_WRITE | DELETE : 0),
        FILE_SHARE_READ | (replacementSource ? FILE_SHARE_DELETE : 0),
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lease->impl->file.value == INVALID_HANDLE_VALUE) return juce::Result::fail("source_lease_unavailable");
    if (write && !FlushFileBuffers(lease->impl->file.value)) return juce::Result::fail("save_staging_flush");
    lease->impl->writable = write;
    lease->impl->replacementSource = replacementSource;
    auto facts = std::make_shared<SourceFileWitness>();
    const auto status = observe(lease->impl->file.value, file, *facts);
    if (status.failed()) return status;
    lease->facts = std::move(facts);
    output = std::move(lease);
    return juce::Result::ok();
#else
    juce::ignoreUnused(output, write, replacementSource);
    return juce::Result::fail("source_lease_platform_unsupported");
#endif
}
catch (...) { return juce::Result::fail("source_lease_exception"); }
juce::Result SourceFileLease::openReplacementSource(const SourceFileWitness& expected, std::shared_ptr<SourceFileLease>& output)
{
    std::unique_ptr<SourceFileLease> candidate;
    const auto opened = openInternal(expected.file, candidate, false, true);
    if (opened.failed()) return opened;
    if (!candidate->witness()->sameFileVersion(expected)) return juce::Result::fail("save_source_version_changed");
    output = std::move(candidate);
    return juce::Result::ok();
}
juce::Result SourceFileLease::preserveReplacedInput(const std::shared_ptr<SourceFileLease>& source,
    const SourceFileWitness::Ptr& original, const SourceFileWitness::Ptr& target)
{
#if JUCE_WINDOWS
    if (!source || !source->impl->replacementSource || source->impl->retained || !original || !target
        || !original->sameFileVersion(*source->facts) || target->file != original->file
        || (target->fileId == original->fileId && target->volumeId == original->volumeId))
        return juce::Result::fail("save_retained_input_contract");
    auto after = std::make_shared<SourceFileWitness>();
    if (observe(source->impl->file.value, original->file, *after).failed()
        || after->volumeId != original->volumeId || after->fileId != original->fileId
        || after->sizeBytes != original->sizeBytes || after->modifiedTicks != original->modifiedTicks)
        return juce::Result::fail("save_retained_input_changed");
    // Replacement changes link/ChangeTime facts, not source audio. No writer
    // could open this object while the replacement lease was held.
    source->facts = std::move(after);
    auto origin = std::make_shared<SourceFileWitness>(*original);
    origin->retained.store({}, std::memory_order_release);
    source->originalVersion = std::move(origin);
    source->impl->retained = true;
    source->impl->parents.clear(); // Retained Undo must not pin user's directories.
    std::shared_ptr<SourceFileLease> empty;
    if (!original->retained.compare_exchange_strong(empty, source, std::memory_order_acq_rel))
        return empty && empty->recheck().wasOk() && empty->facts->fileId == original->fileId
            && empty->facts->volumeId == original->volumeId ? juce::Result::ok() : juce::Result::fail("save_retained_input_conflict");
    return juce::Result::ok();
#else
    juce::ignoreUnused(source, original, target);
    return juce::Result::fail("source_lease_platform_unsupported");
#endif
}
class SourceFileLease::RetainedStream final : public juce::InputStream
{
public:
    explicit RetainedStream(std::shared_ptr<SourceFileLease> value) : lease(std::move(value)) {}
    int64_t getTotalLength() override { return lease->facts->sizeBytes; }
    int64_t getPosition() override { return position; }
    bool isExhausted() override { return position >= getTotalLength(); }
    bool setPosition(int64_t next) override
    {
        if (next < 0 || next > getTotalLength()) return false;
        position = next; return true;
    }
    int read(void* destination, int bytes) override
    {
        const auto count = lease->readAt(position, destination, bytes);
        position += count; return count;
    }
private:
    std::shared_ptr<SourceFileLease> lease;
    int64_t position = 0;
};
std::unique_ptr<juce::InputStream> SourceFileLease::createRetainedInputStream(std::shared_ptr<SourceFileLease> source)
{
#if JUCE_WINDOWS
    if (source && source->impl->retained && source->recheck().wasOk()) return std::make_unique<RetainedStream>(std::move(source));
#else
    juce::ignoreUnused(source);
#endif
    return {};
}
int SourceFileLease::readAt(int64_t position, void* destination, int bytes) const
{
#if JUCE_WINDOWS
    if (!impl->retained || destination == nullptr || bytes <= 0 || position < 0 || position >= facts->sizeBytes) return 0;
    const std::lock_guard lock(impl->readMutex);
    LARGE_INTEGER offset; offset.QuadPart = position;
    DWORD read = 0;
    const auto count = static_cast<DWORD>(juce::jmin<int64_t>(bytes, facts->sizeBytes - position));
    if (!SetFilePointerEx(impl->file.value, offset, nullptr, FILE_BEGIN)
        || !ReadFile(impl->file.value, destination, count, &read, nullptr)) return 0;
    return static_cast<int>(read);
#else
    juce::ignoreUnused(position, destination, bytes);
    return 0;
#endif
}
juce::Result SourceFileLease::publishTo(const juce::File& target) try
{
#if JUCE_WINDOWS
    if (!impl->writable || target == juce::File() || target == facts->file
        || target.getParentDirectory() != facts->file.getParentDirectory() || target.getFullPathName().length() > 4096)
        return juce::Result::fail("save_publication_target");
    const auto checked = recheck();
    if (checked.failed()) return checked;
    const auto path = files::nativePath(target);
    const auto wide = path.toWideCharPointer();
    const auto length = std::wcslen(wide) * sizeof(wchar_t);
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + length);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    // Keep already-open playback/mapped readers on the old physical object
    // while the pathname atomically starts naming the rendered save.
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(length);
    std::memcpy(info->FileName, wide, length);
    if (!SetFileInformationByHandle(impl->file.value, FileRenameInfoEx, info, static_cast<DWORD>(storage.size())))
        return juce::Result::fail("save_publication_replace:" + juce::String(static_cast<int>(GetLastError())));
    // Once renamed, a flush/observation failure must not be retried as though
    // staging still existed or certify clean state. The caller keeps it dirty.
    impl->writable = false;
    if (!FlushFileBuffers(impl->file.value)) return juce::Result::fail("save_publication_flush");
    auto current = std::make_shared<SourceFileWitness>();
    const auto observed = observe(impl->file.value, target, *current);
    if (observed.failed()) return observed;
    if (current->volumeId != facts->volumeId || current->fileId != facts->fileId
        || current->sizeBytes != facts->sizeBytes || current->modifiedTicks != facts->modifiedTicks)
        return juce::Result::fail("save_publication_changed");
    facts = std::move(current);
    return juce::Result::ok();
#else
    juce::ignoreUnused(target);
    return juce::Result::fail("source_lease_platform_unsupported");
#endif
}
catch (...) { return juce::Result::fail("save_publication_exception"); }
juce::Result SourceFileLease::recheck() const
{
#if JUCE_WINDOWS
    SourceFileWitness current;
    const auto status = observe(impl->file.value, facts->file, current);
    if (status.failed()) return status;
    if (current != *facts) return juce::Result::fail("source_lease_changed");
    if (impl->retained) return juce::Result::ok(); // Original physical input intentionally has no pathname authority.
    // A POSIX replacement may leave an existing handle readable while the
    // pathname names a different object. Certify both the handle and its name.
    Handle named;
    named.value = CreateFileW(files::nativePath(facts->file).toWideCharPointer(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    SourceFileWitness atPath;
    if (named.value == INVALID_HANDLE_VALUE || observe(named.value, facts->file, atPath).failed() || atPath != *facts)
        return juce::Result::fail("source_lease_path_changed");
    return juce::Result::ok();
#else
    return juce::Result::fail("source_lease_platform_unsupported");
#endif
}
}
