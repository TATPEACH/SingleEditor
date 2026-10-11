#include "Automation/FileIdentity.h"

namespace singleeditor
{
FileIdentity FileIdentity::capture(const juce::File& file)
{
    FileIdentity identity;
    identity.canonicalPath = file.getFullPathName();
    identity.sizeBytes = file.getSize();
    identity.modifiedMilliseconds = file.getLastModificationTime().toMilliseconds();
    identity.filesystemFileId = file.getFileIdentifier();
    return identity;
}

bool FileIdentity::fromJson(const juce::var& value,
                            FileIdentity& identity,
                            juce::String& errorMessage)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr
        || !object->hasProperty("canonical_path")
        || !object->hasProperty("size_bytes")
        || !object->hasProperty("modified_ms"))
    {
        errorMessage = "Plan item has an invalid input_identity";
        return false;
    }

    identity.canonicalPath = object->getProperty("canonical_path").toString();
    identity.sizeBytes = static_cast<juce::int64>(
        object->getProperty("size_bytes"));
    identity.modifiedMilliseconds = static_cast<juce::int64>(
        object->getProperty("modified_ms"));
    const auto fileId = object->getProperty("filesystem_file_id").toString();
    identity.filesystemFileId = fileId.isEmpty()
                                  ? 0
                                  : static_cast<juce::uint64>(fileId.getHexValue64());
    if (identity.canonicalPath.isEmpty()
        || identity.sizeBytes < 0
        || identity.modifiedMilliseconds <= 0)
    {
        errorMessage = "Plan item has an invalid input_identity";
        return false;
    }
    return true;
}

bool FileIdentity::matches(const FileIdentity& other) const noexcept
{
#if JUCE_WINDOWS
    const auto samePath = canonicalPath.equalsIgnoreCase(other.canonicalPath);
#else
    const auto samePath = canonicalPath == other.canonicalPath;
#endif
    const auto sameFileId = filesystemFileId == 0
                         || other.filesystemFileId == 0
                         || filesystemFileId == other.filesystemFileId;
    return samePath
        && sizeBytes == other.sizeBytes
        && modifiedMilliseconds == other.modifiedMilliseconds
        && sameFileId;
}

juce::var FileIdentity::toJson() const
{
    auto* identity = new juce::DynamicObject();
    identity->setProperty("canonical_path", canonicalPath);
    identity->setProperty("size_bytes", sizeBytes);
    identity->setProperty("modified_ms", modifiedMilliseconds);
    identity->setProperty(
        "filesystem_file_id",
        filesystemFileId == 0
            ? juce::var()
            : juce::var(juce::String::toHexString(
                  static_cast<juce::int64>(filesystemFileId))));
    return juce::var(identity);
}
} // namespace singleeditor
