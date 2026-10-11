#include "App/WindowShellResources.h"
#include "WindowShellTrayPack.h"
#include <juce_cryptography/juce_cryptography.h>

namespace singleeditor::windowShellResources
{
juce::File trayPackFile()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory().getChildFile(identity::relativePath);
}

TrayImage loadTrayImage()
{
    jassert(!juce::MessageManager::existsAndIsCurrentThread());
    const auto fail = [](const char* code) { return TrayImage { {}, code }; };
    static_assert(identity::packBytes <= 512 * 1024 && identity::imageBytes <= 512 * 1024);
    static_assert(identity::manifestBytes <= 4096);
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    ++packReadAttempts;
#endif
    auto input = trayPackFile().createInputStream();
    if (!input || input->failedToOpen()) return fail("tray_pack_unavailable");
    if (input->getTotalLength() != static_cast<juce::int64>(identity::packBytes))
        return fail("tray_pack_size_mismatch");
    juce::MemoryBlock archive;
    if (input->readIntoMemoryBlock(archive, identity::packBytes) != identity::packBytes
        || juce::SHA256(archive).toHexString() != identity::packHash)
        return fail("tray_pack_identity_mismatch");

    juce::MemoryInputStream memory(archive, false);
    juce::ZipFile zip(memory);
    if (zip.getNumEntries() != 2) return fail("tray_pack_entries_mismatch");
    const auto readEntry = [&zip](int index, const char* name, std::size_t size,
                                  const char* digest, juce::MemoryBlock& bytes)
    {
        const auto* entry = zip.getEntry(index);
        if (!entry || entry->filename != name || entry->isSymbolicLink
            || entry->uncompressedSize != static_cast<juce::int64>(size)) return false;
        std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(index));
        return stream && stream->readIntoMemoryBlock(bytes, size) == size && stream->isExhausted()
            && juce::SHA256(bytes).toHexString() == digest;
    };
    juce::MemoryBlock manifest, png;
    if (!readEntry(0, "manifest.json", identity::manifestBytes, identity::manifestHash, manifest))
        return fail("tray_pack_manifest_mismatch");
    const auto description = juce::JSON::parse(manifest.toString());
    if (description["schema"].toString() != "singleeditor.owner-resource-pack"
        || static_cast<int>(description["schema_version"]) != 1
        || description["artifact_id"].toString() != identity::artifactId
        || description["version"].toString() != identity::productVersion)
        return fail("tray_pack_version_mismatch");
    if (!readEntry(1, "tray-icon.png", identity::imageBytes, identity::imageHash, png))
        return fail("tray_pack_image_mismatch");
    // Fixed source identity above bounds the expected PNG; also cap decoded
    // dimensions before asking the image codec to allocate its pixel buffer.
    const auto* bytes = static_cast<const juce::uint8*>(png.getData());
    if (png.getSize() < 33 || juce::ByteOrder::bigEndianInt(bytes + 16) > 1024
        || juce::ByteOrder::bigEndianInt(bytes + 20) > 1024)
        return fail("tray_pack_image_dimensions");
#if SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    ++imageDecodes;
#endif
    auto image = juce::ImageFileFormat::loadFrom(png.getData(), png.getSize());
    if (!image.isValid()) return fail("tray_pack_image_decode");
    return { std::move(image), "ready" };
}
}
