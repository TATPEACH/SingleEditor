#include "Audio/AmbisonicBinauralFilterBank.h"
#include "AmbisonicPack.h"
#include <juce_cryptography/juce_cryptography.h>

#include <cstring>

namespace singleeditor
{
namespace
{
constexpr char resourceMagic[8] = { 'S', 'E', 'H', 'R', 'I', 'R', '1', '\0' };
constexpr int resourceVersion = 1;
constexpr int resourceHeaderBytes = 60;
constexpr int bankHeaderBytes = 8;
constexpr int maximumTapCount = 4096;

uint32_t crc32(const uint8_t* data, std::size_t size) noexcept
{
    auto crc = 0xffffffffu;
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (auto bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

uint32_t readUint32(const uint8_t*& cursor, const uint8_t* end, bool& valid) noexcept
{
    if (end - cursor < 4)
    {
        valid = false;
        return 0;
    }
    const auto value = juce::ByteOrder::littleEndianInt(cursor);
    cursor += 4;
    return value;
}

float readFloat32(const uint8_t*& cursor, const uint8_t* end, bool& valid) noexcept
{
    const auto bits = readUint32(cursor, end, valid);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
} // namespace

std::shared_ptr<const AmbisonicBinauralFilterBank> AmbisonicBinauralFilterBank::loadFromFile(
    const juce::File& file,
    juce::String& errorMessage)
{
    errorMessage.clear();
    if (!file.existsAsFile())
    {
        errorMessage = "Binaural HRTF resource is missing: " + file.getFullPathName();
        return {};
    }

    juce::MemoryBlock bytes;
    auto input = file.createInputStream();
    constexpr juce::int64 maximumResourceBytes = 1024 * 1024;
    const auto fileBytes = input ? input->getTotalLength() : 0;
    if (!input || fileBytes < resourceHeaderBytes || fileBytes > maximumResourceBytes
        || input->readIntoMemoryBlock(bytes, static_cast<std::size_t>(fileBytes)) != static_cast<std::size_t>(fileBytes))
    {
        errorMessage = "Binaural HRTF resource is truncated or unreadable.";
        return {};
    }
    return decode(bytes, file.getFileNameWithoutExtension(), errorMessage);
}

std::shared_ptr<const AmbisonicBinauralFilterBank> AmbisonicBinauralFilterBank::decode(
    const juce::MemoryBlock& bytes, const juce::String& resourceName, juce::String& errorMessage)
{
    if (bytes.getSize() < resourceHeaderBytes)
    {
        errorMessage = "Binaural HRTF resource is truncated.";
        return {};
    }
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    const auto* cursor = begin;
    const auto* end = begin + bytes.getSize();
    if (std::memcmp(cursor, resourceMagic, sizeof(resourceMagic)) != 0)
    {
        errorMessage = "Binaural HRTF resource has an unsupported signature.";
        return {};
    }
    cursor += sizeof(resourceMagic);

    auto valid = true;
    const auto version = static_cast<int>(readUint32(cursor, end, valid));
    const auto sampleRate = static_cast<int>(readUint32(cursor, end, valid));
    const auto tapCount = static_cast<int>(readUint32(cursor, end, valid));
    const auto bankCount = static_cast<int>(readUint32(cursor, end, valid));
    if (!valid || end - cursor < 36)
    {
        errorMessage = "Binaural HRTF resource header is truncated.";
        return {};
    }

    const juce::String sourceMd5 = juce::String::fromUTF8(
        reinterpret_cast<const char*>(cursor), 32);
    cursor += 32;
    const auto expectedChecksum = readUint32(cursor, end, valid);
    if (!valid || version != resourceVersion || sampleRate < 8000 || sampleRate > 384000
        || tapCount <= 0 || tapCount > maximumTapCount || bankCount != 3)
    {
        errorMessage = "Binaural HRTF resource header is unsupported.";
        return {};
    }

    if (crc32(cursor, static_cast<std::size_t>(end - cursor)) != expectedChecksum)
    {
        errorMessage = "Binaural HRTF resource checksum does not match.";
        return {};
    }

    auto result = std::shared_ptr<AmbisonicBinauralFilterBank>(new AmbisonicBinauralFilterBank());
    result->sampleRate = sampleRate;
    result->tapCount = tapCount;
    result->sourceMd5 = sourceMd5;

    for (auto bankIndex = 0; bankIndex < bankCount; ++bankIndex)
    {
        if (end - cursor < bankHeaderBytes)
        {
            errorMessage = "Binaural HRTF bank header is truncated.";
            return {};
        }

        auto& bank = result->banks[static_cast<std::size_t>(bankIndex)];
        bank.order = static_cast<int>(readUint32(cursor, end, valid));
        bank.sourceChannels = static_cast<int>(readUint32(cursor, end, valid));
        const auto expectedChannels = (bank.order + 1) * (bank.order + 1);
        if (!valid || bank.order != bankIndex + 1 || bank.sourceChannels != expectedChannels)
        {
            errorMessage = "Binaural HRTF bank order is invalid.";
            return {};
        }

        const auto sampleCount = static_cast<int64_t>(2) * bank.sourceChannels * tapCount;
        if (sampleCount <= 0 || end - cursor < sampleCount * static_cast<int64_t>(sizeof(float)))
        {
            errorMessage = "Binaural HRTF bank samples are truncated.";
            return {};
        }

        bank.samples.resize(static_cast<std::size_t>(sampleCount));
        for (auto& sample : bank.samples)
        {
            sample = readFloat32(cursor, end, valid);
            if (!valid || !std::isfinite(sample))
            {
                errorMessage = "Binaural HRTF bank contains an invalid sample.";
                return {};
            }
        }
    }

    if (cursor != end)
    {
        errorMessage = "Binaural HRTF resource contains unexpected trailing data.";
        return {};
    }

    result->resourceIdentity = resourceName
                             + ":v" + juce::String(resourceVersion)
                             + ":" + result->sourceMd5;
    return result;
}

std::shared_ptr<const AmbisonicBinauralFilterBank> AmbisonicBinauralFilterBank::loadDefault(
    juce::String& errorMessage)
{
    namespace identity = ambisonicResources::identity;
    errorMessage.clear();
    const auto fail = [&errorMessage](const char* message) -> std::shared_ptr<const AmbisonicBinauralFilterBank>
    { errorMessage = message; return {}; };
    static_assert(identity::packBytes <= 65536 && identity::dataBytes <= 61440 && identity::manifestBytes <= 4096);
#if SINGLE_EDITOR_AMBISONIC_RESOURCE_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    ++ambisonicResources::packReadAttempts;
    if (juce::Thread::getCurrentThread() != nullptr) ++ambisonicResources::workerReads;
#endif
    auto input = defaultResourceFile().createInputStream();
    if (!input || input->failedToOpen()) return fail("Binaural HRTF resource pack is missing or unreadable.");
    if (input->getTotalLength() != static_cast<juce::int64>(identity::packBytes))
        return fail("Binaural HRTF resource pack has an unexpected size.");
    juce::MemoryBlock archive;
    if (input->readIntoMemoryBlock(archive, identity::packBytes) != identity::packBytes
        || juce::SHA256(archive).toHexString() != identity::packHash)
        return fail("Binaural HRTF resource pack does not match this application build.");
    juce::MemoryInputStream memory(archive, false);
    juce::ZipFile zip(memory);
    if (zip.getNumEntries() != 2) return fail("Binaural HRTF resource pack entries are unavailable.");
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
    juce::MemoryBlock manifest, data;
    if (!readEntry(0, "manifest.json", identity::manifestBytes, identity::manifestHash, manifest))
        return fail("Binaural HRTF resource pack manifest is unavailable.");
    const auto description = juce::JSON::parse(manifest.toString());
    if (description["schema"].toString() != "singleeditor.owner-resource-pack"
        || static_cast<int>(description["schema_version"]) != 1
        || description["artifact_id"].toString() != identity::artifactId
        || description["version"].toString() != identity::productVersion)
        return fail("Binaural HRTF resource pack version is unsupported.");
    if (!readEntry(1, "binaural.sehrir", identity::dataBytes, identity::dataHash, data))
        return fail("Binaural HRTF filter data is unavailable.");
    // Preserve the existing public DSP identity and exact filter samples.
    auto bank = decode(data, "SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k", errorMessage);
#if SINGLE_EDITOR_AMBISONIC_RESOURCE_TESTING || SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST
    if (bank) ++ambisonicResources::preparedBanks;
#endif
    return bank;
}

juce::File AmbisonicBinauralFilterBank::defaultResourceFile()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory().getChildFile(ambisonicResources::identity::relativePath);
}

int AmbisonicBinauralFilterBank::getSourceChannelCount(int ambisonicOrder) const noexcept
{
    const auto* bank = findBank(ambisonicOrder);
    return bank != nullptr ? bank->sourceChannels : 0;
}

float AmbisonicBinauralFilterBank::getSample(int ambisonicOrder,
                                             int ear,
                                             int sourceChannel,
                                             int tap) const noexcept
{
    const auto* bank = findBank(ambisonicOrder);
    if (bank == nullptr || !juce::isPositiveAndBelow(ear, 2)
        || !juce::isPositiveAndBelow(sourceChannel, bank->sourceChannels)
        || !juce::isPositiveAndBelow(tap, tapCount))
    {
        return 0.0f;
    }

    const auto index = ((ear * bank->sourceChannels) + sourceChannel) * tapCount + tap;
    return bank->samples[static_cast<std::size_t>(index)];
}

const AmbisonicBinauralFilterBank::Bank* AmbisonicBinauralFilterBank::findBank(
    int ambisonicOrder) const noexcept
{
    if (!juce::isPositiveAndBelow(ambisonicOrder - 1, static_cast<int>(banks.size())))
        return nullptr;
    const auto& bank = banks[static_cast<std::size_t>(ambisonicOrder - 1)];
    return bank.order == ambisonicOrder ? &bank : nullptr;
}
} // namespace singleeditor
