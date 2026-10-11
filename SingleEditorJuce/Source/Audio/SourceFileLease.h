#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <atomic>

namespace singleeditor::media
{
class SourceFileLease;
// Facts from a guarded physical source open, not a content fingerprint or a
// recovery service. Read-only admission retains no handle. After an explicit
// overwrite, a shared backing may retain its old physical input for live Undo.
struct SourceFileWitness final
{
    using Ptr = std::shared_ptr<const SourceFileWitness>;
    SourceFileWitness() = default;
    SourceFileWitness(const SourceFileWitness&);
    SourceFileWitness& operator=(const SourceFileWitness&);
    juce::File file;
    juce::String volumeId, fileId;
    int64_t sizeBytes = 0, modifiedTicks = 0, changedTicks = 0;
    // Copied from the original successful decoder admission, before edits.
    juce::String readerFormat;
    double sampleRate = 0;
    int channels = 0, bitsPerSample = 0;
    int64_t frames = 0;
    bool sameFileVersion(const SourceFileWitness& other) const noexcept
    {
        return file == other.file && volumeId == other.volumeId && fileId == other.fileId
            && sizeBytes == other.sizeBytes && modifiedTicks == other.modifiedTicks && changedTicks == other.changedTicks;
    }
    bool operator==(const SourceFileWitness&) const noexcept;
    std::shared_ptr<SourceFileLease> retainedInput() const noexcept;
private:
    friend class SourceFileLease;
    mutable std::atomic<std::shared_ptr<SourceFileLease>> retained;
};

// A progressive decoded reader can expose an immutable backing witness only
// after its real decoder and cache validation have finished. This is never
// evidence for the compressed original. Recovery must still acquire/recheck
// its own independent read lease before using these bytes.
class IndependentAudioBacking
{
public:
    virtual ~IndependentAudioBacking() = default;
    virtual SourceFileWitness::Ptr validatedBacking() const noexcept = 0;
};

// Short explicit source-owner lease. Pins ancestors and denies write/delete
// sharing while reader creation or independent capture is in progress. Closing
// the lease releases every extra handle; it creates no file/thread/watcher.
class SourceFileLease final
{
public:
    ~SourceFileLease();
    static juce::Result open(const juce::File&, std::unique_ptr<SourceFileLease>&);
    // Explicit save-worker path for an already rendered sibling staging file.
    // The held object is flushed and renamed by handle without reopening its
    // mutable staging path. Ordinary reader admission never requests write access.
    static juce::Result openStagedWrite(const juce::File&, std::unique_ptr<SourceFileLease>&);
    // Explicit save worker: deny writes but permit the save's atomic replacement.
    static juce::Result openReplacementSource(const SourceFileWitness&, std::shared_ptr<SourceFileLease>&);
    static juce::Result preserveReplacedInput(const std::shared_ptr<SourceFileLease>&,
                                             const SourceFileWitness::Ptr& original,
                                             const SourceFileWitness::Ptr& publishedTarget);
    // Independent decoder cursor over the retained object, never the live
    // playback reader or a reopened historical path. Worker use only.
    static std::unique_ptr<juce::InputStream> createRetainedInputStream(std::shared_ptr<SourceFileLease>);
    juce::Result publishTo(const juce::File& siblingTarget);
    SourceFileWitness::Ptr witness() const noexcept { return facts; }
    juce::Result recheck() const;
private:
    friend struct SourceFileWitness;
    bool protectsInput(const SourceFileWitness&) const noexcept;
    SourceFileLease();
    static juce::Result openInternal(const juce::File&, std::unique_ptr<SourceFileLease>&, bool write, bool replacementSource = false);
    class RetainedStream;
    int readAt(int64_t position, void* destination, int bytes) const;
    struct Impl;
    std::unique_ptr<Impl> impl;
    SourceFileWitness::Ptr facts;
    SourceFileWitness::Ptr originalVersion;
};
}
