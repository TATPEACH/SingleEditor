#pragma once

#include <juce_core/juce_core.h>

namespace singleeditor
{
struct FileIdentity
{
    juce::String canonicalPath;
    juce::int64 sizeBytes = 0;
    juce::int64 modifiedMilliseconds = 0;
    juce::uint64 filesystemFileId = 0;

    static FileIdentity capture(const juce::File& file);
    static bool fromJson(const juce::var& value,
                         FileIdentity& identity,
                         juce::String& errorMessage);
    bool matches(const FileIdentity& other) const noexcept;
    juce::var toJson() const;
};
} // namespace singleeditor
