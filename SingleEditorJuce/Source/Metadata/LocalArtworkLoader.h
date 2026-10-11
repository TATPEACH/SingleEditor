#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>

namespace singleeditor
{
// Optional, post-CoreReady local artwork. No directory enumeration, reader,
// shell thumbnail provider or network service. Call request/cancel on the UI thread.
class LocalArtworkLoader final : private juce::Thread, private juce::AsyncUpdater
{
public:
    using Completion = std::function<void(const juce::File&, const juce::Image&)>;
    LocalArtworkLoader() : juce::Thread("Local file artwork") {}
    ~LocalArtworkLoader() override
    {
        cancel();
        signalThreadShouldExit();
        notify();
        stopThread(-1);
        cancelPendingUpdate();
    }

    void cancel()
    {
        ++revision;
        completion = {};
        std::lock_guard<std::mutex> lock(mutex);
        pendingFile = {};
        readyImage = {};
        readyRevision = 0;
    }

    void request(const juce::File& file, Completion callback)
    {
        cancel();
        completion = std::move(callback);
        {
            std::lock_guard<std::mutex> lock(mutex);
            pendingFile = file;
            pendingRevision = revision.load();
        }
        if (!isThreadRunning()) startThread(juce::Thread::Priority::low);
        notify();
    }

    // Bounded synchronous work, also exercised with real image files in tests.
    static juce::Image readLocalCover(const juce::File& audio,
                                     const std::function<bool()>& cancelled = [] { return false; })
    {
        if (audio == juce::File()) return {};
        const auto directory = audio.getParentDirectory();
        for (const auto& stem : juce::StringArray { audio.getFileNameWithoutExtension(), "cover", "folder" })
            for (const auto* extension : { ".png", ".jpg", ".jpeg" })
            {
                if (cancelled()) return {};
                const auto candidate = directory.getChildFile(stem + extension);
                if (candidate == audio) continue;
                auto stream = candidate.createInputStream();
                if (stream == nullptr) continue;
                const auto length = stream->getTotalLength();
                if (length < 24 || length > 8 * 1024 * 1024) continue;
                juce::MemoryBlock bytes(static_cast<size_t>(length));
                size_t offset = 0;
                while (offset < bytes.getSize() && !cancelled())
                {
                    const auto count = stream->read(static_cast<char*>(bytes.getData()) + offset,
                        static_cast<int>(juce::jmin<size_t>(64 * 1024, bytes.getSize() - offset)));
                    if (count <= 0) break;
                    offset += static_cast<size_t>(count);
                }
                if (cancelled()) return {};
                if (offset != bytes.getSize() || !hasSafeDimensions(bytes)) continue;
                auto image = juce::ImageFileFormat::loadFrom(bytes.getData(), bytes.getSize());
                if (cancelled()) return {};
                if (!image.isValid()) continue;
                const auto longest = juce::jmax(image.getWidth(), image.getHeight());
                if (longest > 512)
                    image = image.rescaled(juce::jmax(1, image.getWidth() * 512 / longest),
                                          juce::jmax(1, image.getHeight() * 512 / longest),
                                          juce::Graphics::highResamplingQuality);
                return image;
            }
        return {};
    }

private:
    static bool hasSafeDimensions(const juce::MemoryBlock& bytes)
    {
        const auto* p = static_cast<const unsigned char*>(bytes.getData());
        const auto size = bytes.getSize();
        const auto safe = [](juce::uint32 width, juce::uint32 height)
        {
            return width > 0 && height > 0 && width <= 4096 && height <= 4096
                && static_cast<juce::uint64>(width) * height <= 8 * 1024 * 1024;
        };
        if (size >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0
            && std::memcmp(p + 12, "IHDR", 4) == 0)
            return safe(juce::ByteOrder::bigEndianInt(p + 16), juce::ByteOrder::bigEndianInt(p + 20));
        if (size < 4 || p[0] != 0xff || p[1] != 0xd8) return false;
        size_t cursor = 2;
        while (cursor + 4 <= size)
        {
            if (p[cursor++] != 0xff) return false;
            while (cursor < size && p[cursor] == 0xff) ++cursor;
            if (cursor >= size) return false;
            const auto marker = p[cursor++];
            if (marker == 0xda || marker == 0xd9) return false;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (cursor + 2 > size) return false;
            const auto length = static_cast<size_t>(juce::ByteOrder::bigEndianShort(p + cursor));
            if (length < 2 || length > size - cursor) return false;
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)
                return length >= 8 && safe(juce::ByteOrder::bigEndianShort(p + cursor + 5),
                                            juce::ByteOrder::bigEndianShort(p + cursor + 3));
            cursor += length;
        }
        return false;
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            juce::File file;
            std::uint64_t token = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                file = pendingFile;
                token = pendingRevision;
                pendingFile = {};
            }
            if (file == juce::File()) { wait(-1); continue; }
            const auto cancelled = [this, token] { return threadShouldExit() || revision.load() != token; };
            auto image = readLocalCover(file, cancelled);
            if (cancelled()) continue;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (cancelled()) continue;
                readyFile = file;
                readyImage = std::move(image);
                readyRevision = token;
            }
            triggerAsyncUpdate();
        }
    }

    void handleAsyncUpdate() override
    {
        juce::File file;
        juce::Image image;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (readyRevision == 0 || readyRevision != revision.load()) return;
            file = readyFile;
            image = readyImage;
            readyRevision = 0;
        }
        // A completion may request another file and replace the stored callback.
        const auto callback = completion;
        if (callback) callback(file, image);
    }

    std::atomic<std::uint64_t> revision { 0 };
    std::mutex mutex;
    juce::File pendingFile, readyFile;
    juce::Image readyImage;
    std::uint64_t pendingRevision = 0, readyRevision = 0;
    Completion completion;
};
} // namespace singleeditor
