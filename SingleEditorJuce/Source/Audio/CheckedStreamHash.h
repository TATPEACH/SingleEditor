#pragma once
#include <juce_cryptography/juce_cryptography.h>
#include <functional>
#include <optional>

namespace singleeditor::media
{
using WorkCheckpoint = std::function<bool()>;

// SHA256's stream API consumes 64-byte blocks. Buffer bounded reads underneath
// it so cancellation/deadline checks happen before each <= 64 KiB source read.
// A stopped/short read can never be mistaken for a digest of the whole region.
inline std::optional<juce::SHA256> checkedStreamHash(juce::InputStream& input,
                                                   int64_t bytes,
                                                   const WorkCheckpoint& checkpoint = {})
{
    if (bytes < 0 || (checkpoint && !checkpoint())) return {};
    juce::SubregionStream region(&input, input.getPosition(), bytes, false);
    struct CheckedInput final : juce::InputStream
    {
        CheckedInput(juce::InputStream& stream, const WorkCheckpoint& check) : source(stream), check(check) {}
        juce::int64 getTotalLength() override { return source.getTotalLength(); }
        juce::int64 getPosition() override { return source.getPosition(); }
        bool setPosition(juce::int64 position) override { return source.setPosition(position); }
        bool isExhausted() override { return stopped || source.isExhausted(); }
        int read(void* destination, int count) override
        {
            if (stopped || (check && !check())) { stopped = true; return 0; }
            return source.read(destination, juce::jmin(count, 64 * 1024));
        }
        juce::InputStream& source;
        const WorkCheckpoint& check;
        bool stopped = false;
    } checked(region, checkpoint);
    juce::BufferedInputStream buffered(checked, 64 * 1024);
    const auto hash = juce::SHA256(buffered, bytes);
    if (checked.stopped || buffered.getPosition() != bytes || region.getPosition() != bytes
        || (checkpoint && !checkpoint())) return {};
    return hash;
}
}
