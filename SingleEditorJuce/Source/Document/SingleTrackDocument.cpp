#include "Document/SingleTrackDocument.h"
#include "Core/NativeFilePath.h"

#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicBinauralFilterBank.h"
#include "Audio/ReaderAdmission.h"
#include "Audio/ChannelRouter.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#include "Audio/VgmstreamDecodeService.h"
#include "Audio/WindowsMediaAudioService.h"

namespace singleeditor
{
namespace
{
static_assert(media::maximumSourceChannels == RoutingMatrix::maxChannels);

std::shared_ptr<juce::AudioBuffer<float>> allocateMaterialisedBuffer(int channels, int64_t frames)
{
    if (!media::admitMaterialisation(static_cast<uint64_t>(channels), frames)) return {};
    try { return std::make_shared<juce::AudioBuffer<float>>(channels, static_cast<int>(frames)); }
    catch (const std::bad_alloc&) { return {}; }
}

void registerReadFormats(juce::AudioFormatManager& manager)
{
    manager.clearFormats();
    manager.registerFormat(new juce::WavAudioFormat(), true);
    manager.registerFormat(new juce::FlacAudioFormat(), true);
    manager.registerFormat(new juce::AiffAudioFormat(), true);
    manager.registerFormat(new juce::OggVorbisAudioFormat(), true);
#if JUCE_USE_WINDOWS_MEDIA_FORMAT
    manager.registerFormat(new juce::WindowsMediaAudioFormat(), true);
#endif
#if JUCE_USE_MP3AUDIOFORMAT
    manager.registerFormat(new juce::MP3AudioFormat(), true);
#endif
}

struct ThreadLocalReadFormatRegistry
{
    ThreadLocalReadFormatRegistry()
    {
        registerReadFormats(manager);
    }

    juce::AudioFormatManager manager;
};

juce::AudioFormatManager& getThreadLocalReadFormatManager()
{
    thread_local ThreadLocalReadFormatRegistry registry;
    return registry.manager;
}

std::unique_ptr<juce::AudioFormatReader> createReaderWithFormat(juce::AudioFormat& format, const juce::File& file)
{
    auto stream = std::make_unique<juce::FileInputStream>(file);
    if (!stream->openedOk())
        return {};

    if (auto* reader = format.createReaderFor(stream.release(), true))
        return std::unique_ptr<juce::AudioFormatReader>(reader);

    return {};
}

media::SourceFileWitness::Ptr sourceFactsForReader(const media::SourceFileLease* lease, const juce::AudioFormatReader& reader)
{
    if (lease == nullptr || lease->recheck().failed()) return {};
    auto facts = std::make_shared<media::SourceFileWitness>(*lease->witness());
    facts->readerFormat = reader.getFormatName(); facts->sampleRate = reader.sampleRate;
    facts->channels = static_cast<int>(reader.numChannels); facts->bitsPerSample = static_cast<int>(reader.bitsPerSample);
    facts->frames = reader.lengthInSamples;
    return facts;
}

std::shared_ptr<juce::MemoryMappedAudioFormatReader> createMappedReaderWithFormat(juce::AudioFormat& format,
                                                                                  const juce::File& file,
                                                                                  bool mapEntireFileOnCreate)
{
    std::unique_ptr<juce::MemoryMappedAudioFormatReader> mappedReader(format.createMemoryMappedReader(file));
    if (mappedReader != nullptr
        && media::admitReader(mappedReader->sampleRate, mappedReader->numChannels, mappedReader->lengthInSamples)
        && (!mapEntireFileOnCreate || mappedReader->mapEntireFile()))
    {
        return std::shared_ptr<juce::MemoryMappedAudioFormatReader>(std::move(mappedReader));
    }

    return {};
}

std::unique_ptr<juce::AudioFormatReader> createDirectStandardAudioReaderForFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    const auto extension = file.getFileExtension().toLowerCase();
    if (extension == ".wav")
    {
        juce::WavAudioFormat format;
        return createReaderWithFormat(format, file);
    }

    if (extension == ".aif" || extension == ".aiff")
    {
        juce::AiffAudioFormat format;
        return createReaderWithFormat(format, file);
    }

    if (extension == ".flac")
    {
        juce::FlacAudioFormat format;
        return createReaderWithFormat(format, file);
    }

    if (extension == ".ogg")
    {
        juce::OggVorbisAudioFormat format;
        return createReaderWithFormat(format, file);
    }

#if JUCE_USE_MP3AUDIOFORMAT
    if (extension == ".mp3")
    {
        juce::MP3AudioFormat format;
        return createReaderWithFormat(format, file);
    }
#endif

#if JUCE_USE_WINDOWS_MEDIA_FORMAT
    if (extension == ".wma" || extension == ".asf")
    {
        juce::WindowsMediaAudioFormat format;
        return createReaderWithFormat(format, file);
    }
#endif

    return {};
}

std::shared_ptr<juce::MemoryMappedAudioFormatReader> createDirectMappedReaderForFile(const juce::File& file,
                                                                                     bool mapEntireFileOnCreate)
{
    if (!file.existsAsFile())
        return {};

    const auto extension = file.getFileExtension().toLowerCase();
    if (extension == ".wav")
    {
        juce::WavAudioFormat format;
        return createMappedReaderWithFormat(format, file, mapEntireFileOnCreate);
    }

    if (extension == ".aif" || extension == ".aiff")
    {
        juce::AiffAudioFormat format;
        return createMappedReaderWithFormat(format, file, mapEntireFileOnCreate);
    }

    return {};
}

std::unique_ptr<juce::AudioFormatReader> createReaderForWem(const juce::File& file)
{
    if (!file.existsAsFile() || !file.hasFileExtension(".wem"))
        return {};

    const auto strategy = VgmstreamDecodeService::chooseWemOpenStrategy(file);
    if (strategy.kind == VgmstreamDecodeService::WemOpenStrategy::Kind::directWaveReader)
    {
        juce::WavAudioFormat wavFormat;
        auto stream = std::make_unique<juce::FileInputStream>(file);
        if (!stream->openedOk())
            return {};

        if (auto* reader = wavFormat.createReaderFor(stream.release(), true))
            return std::unique_ptr<juce::AudioFormatReader>(reader);
    }

    juce::StringArray diagnostics;
    if (auto reader = VgmstreamDecodeService::createReaderForDecodedFile(file, &diagnostics))
        return reader;

    return {};
}

juce::uint32 parseWaveChannelMask(const juce::StringPairArray& metadata)
{
    const auto keys = metadata.getAllKeys();
    for (const auto& key : keys)
    {
        if (!key.equalsIgnoreCase("ChannelMask"))
            continue;

        const auto value = metadata[key].trim();
        if (value.startsWithIgnoreCase("0x"))
            return static_cast<juce::uint32>(value.fromFirstOccurrenceOf("x", false, true).getHexValue64());

        return static_cast<juce::uint32>(value.getLargeIntValue());
    }

    return 0;
}

juce::String readFourCC(juce::InputStream& stream)
{
    char value[5] {};
    return stream.read(value, 4) == 4 ? juce::String::fromUTF8(value, 4) : juce::String();
}

void addMetadataLabel(juce::StringArray& labels, const juce::String& label)
{
    if (label.isNotEmpty())
        labels.addIfNotAlreadyThere(label);
}

juce::StringArray inspectContainerMetadata(const juce::File& file)
{
    juce::StringArray labels;
    auto stream = file.createInputStream();
    if (stream == nullptr)
        return labels;

    const auto extension = file.getFileExtension().toLowerCase();
    if (extension == ".mp3")
    {
        if (readFourCC(*stream).startsWith("ID3"))
            addMetadataLabel(labels, "ID3v2 metadata");
        if (file.getSize() >= 128 && stream->setPosition(file.getSize() - 128)
            && readFourCC(*stream).startsWith("TAG"))
        {
            addMetadataLabel(labels, "ID3v1 metadata");
        }
        if (file.getSize() >= 32 && stream->setPosition(file.getSize() - 32))
        {
            char footer[9] {};
            if (stream->read(footer, 8) == 8 && juce::String::fromUTF8(footer, 8) == "APETAGEX")
                addMetadataLabel(labels, "APEv2 metadata");
        }
        return labels;
    }

    if (extension == ".flac")
    {
        if (readFourCC(*stream) != "fLaC")
            return labels;

        for (auto block = 0; block < 128 && stream->getPosition() + 4 <= file.getSize(); ++block)
        {
            const auto header = static_cast<juce::uint8>(stream->readByte());
            const auto isLast = (header & 0x80u) != 0;
            const auto type = header & 0x7fu;
            const auto size = (static_cast<juce::uint32>(static_cast<juce::uint8>(stream->readByte())) << 16u)
                            | (static_cast<juce::uint32>(static_cast<juce::uint8>(stream->readByte())) << 8u)
                            | static_cast<juce::uint32>(static_cast<juce::uint8>(stream->readByte()));
            const auto dataStart = stream->getPosition();
            switch (type)
            {
                case 2: addMetadataLabel(labels, "FLAC application metadata"); break;
                case 4:
                    if (size >= 8 && size <= 16u * 1024u * 1024u)
                    {
                        const auto vendorLength = static_cast<juce::uint32>(stream->readInt());
                        if (vendorLength <= size - 8
                            && stream->setPosition(stream->getPosition() + vendorLength))
                        {
                            const auto commentCount = static_cast<juce::uint32>(stream->readInt());
                            if (commentCount > 0)
                                addMetadataLabel(labels, "FLAC Vorbis comments");
                        }
                    }
                    break;
                case 5: addMetadataLabel(labels, "FLAC cue sheet"); break;
                case 6: addMetadataLabel(labels, "FLAC picture metadata"); break;
                default:
                    if (type > 6)
                        addMetadataLabel(labels, "FLAC metadata block " + juce::String(type));
                    break;
            }
            if (!stream->setPosition(dataStart + size) || isLast)
                break;
        }
        return labels;
    }

    if (extension == ".wav" || extension == ".wave")
    {
        const auto container = readFourCC(*stream);
        stream->readInt();
        if ((container != "RIFF" && container != "RF64") || readFourCC(*stream) != "WAVE")
            return labels;

        const juce::StringArray structuralChunks { "fmt ", "data", "ds64", "fact", "JUNK", "FLLR", "PAD " };
        const std::map<juce::String, juce::String> metadataChunks {
            { "bext", "BWF bext" }, { "iXML", "iXML metadata" }, { "axml", "AXML metadata" },
            { "smpl", "WAV sampler loops" }, { "inst", "WAV instrument metadata" },
            { "cue ", "WAV cue points" }, { "LIST", "WAV LIST metadata" },
            { "acid", "ACID metadata" }, { "trkn", "Tracktion metadata" }
        };
        for (auto chunk = 0; chunk < 4096 && stream->getPosition() + 8 <= file.getSize(); ++chunk)
        {
            const auto id = readFourCC(*stream);
            const auto size = static_cast<juce::uint32>(stream->readInt());
            const auto dataStart = stream->getPosition();
            if (const auto found = metadataChunks.find(id); found != metadataChunks.end())
                addMetadataLabel(labels, found->second);
            else if (!structuralChunks.contains(id))
                addMetadataLabel(labels, "Unsupported WAV chunk '" + id.trimEnd() + "'");

            const auto next = dataStart + size + (size & 1u);
            if (next <= dataStart || !stream->setPosition(next))
                break;
        }
        return labels;
    }

    if (extension == ".aif" || extension == ".aiff" || extension == ".aifc")
    {
        if (readFourCC(*stream) != "FORM")
            return labels;
        stream->readIntBigEndian();
        const auto form = readFourCC(*stream);
        if (form != "AIFF" && form != "AIFC")
            return labels;

        const juce::StringArray structuralChunks { "COMM", "SSND", "FVER" };
        const std::map<juce::String, juce::String> metadataChunks {
            { "MARK", "AIFF markers" }, { "COMT", "AIFF comments" }, { "INST", "AIFF instrument metadata" }
        };
        for (auto chunk = 0; chunk < 4096 && stream->getPosition() + 8 <= file.getSize(); ++chunk)
        {
            const auto id = readFourCC(*stream);
            const auto size = static_cast<juce::uint32>(stream->readIntBigEndian());
            const auto dataStart = stream->getPosition();
            if (const auto found = metadataChunks.find(id); found != metadataChunks.end())
                addMetadataLabel(labels, found->second);
            else if (!structuralChunks.contains(id))
                addMetadataLabel(labels, "Unsupported AIFF chunk '" + id.trimEnd() + "'");

            const auto next = dataStart + size + (size & 1u);
            if (next <= dataStart || !stream->setPosition(next))
                break;
        }
        return labels;
    }

    if (extension == ".ogg")
    {
        juce::MemoryBlock data;
        stream->setPosition(0);
        stream->readIntoMemoryBlock(data, juce::jmin<int64_t>(file.getSize(), 4 * 1024 * 1024));
        const auto* bytes = static_cast<const juce::uint8*>(data.getData());
        const auto dataSize = data.getSize();
        constexpr juce::uint8 signature[] { 3, 'v', 'o', 'r', 'b', 'i', 's' };
        for (size_t offset = 0; offset + sizeof(signature) + 8 <= dataSize; ++offset)
        {
            if (std::memcmp(bytes + offset, signature, sizeof(signature)) != 0)
                continue;

            auto position = offset + sizeof(signature);
            const auto readLittleEndian32 = [&](size_t at) -> juce::uint32
            {
                return static_cast<juce::uint32>(bytes[at])
                     | (static_cast<juce::uint32>(bytes[at + 1]) << 8u)
                     | (static_cast<juce::uint32>(bytes[at + 2]) << 16u)
                     | (static_cast<juce::uint32>(bytes[at + 3]) << 24u);
            };
            const auto vendorLength = readLittleEndian32(position);
            position += 4;
            if (vendorLength > dataSize - position || position + vendorLength + 4 > dataSize)
                break;
            position += vendorLength;
            const auto commentCount = readLittleEndian32(position);
            position += 4;
            for (juce::uint32 comment = 0; comment < commentCount && position + 4 <= dataSize; ++comment)
            {
                const auto length = readLittleEndian32(position);
                position += 4;
                if (length > dataSize - position)
                    break;
                const auto text = juce::String::fromUTF8(reinterpret_cast<const char*>(bytes + position),
                                                         static_cast<int>(length));
                position += length;
                const auto key = text.upToFirstOccurrenceOf("=", false, false).trim().toUpperCase();
                const juce::StringArray supported { "ENCODER", "TITLE", "ARTIST", "ALBUM", "COMMENT",
                                                    "DATE", "GENRE", "TRACKNUMBER" };
                if (key.isNotEmpty() && !supported.contains(key))
                    addMetadataLabel(labels, "Unsupported Ogg comment '" + key + "'");
            }
            break;
        }
        return labels;
    }

    if (WindowsMediaAudioService::isSupportedVideoExtension(extension))
        addMetadataLabel(labels, "Video container and stream metadata");
    return labels;
}

bool isTimelineMetadata(const juce::String& name)
{
    const auto lower = name.toLowerCase();
    return lower.contains("time reference")
        || lower.contains("ixml")
        || lower.contains("cue")
        || lower.contains("smpl")
        || lower.contains("loop")
        || lower.contains("marker")
        || lower.contains("instrument");
}

bool parseNonNegativeMetadataValue(const juce::String& text, int64_t& value)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return false;

    for (const auto character : trimmed)
        if (!juce::CharacterFunctions::isDigit(character))
            return false;

    value = trimmed.getLargeIntValue();
    return value >= 0;
}

void removeIndexedMetadata(juce::StringPairArray& metadata,
                           const juce::String& prefix,
                           int count,
                           const juce::StringArray& fields)
{
    for (int index = 0; index < count; ++index)
        for (const auto& field : fields)
        {
            const auto key = prefix + juce::String(index) + field;
            metadata.remove(juce::StringRef(key));
        }
}

void markTimelineMetadata(const juce::StringPairArray& metadata,
                          const juce::String& prefix,
                          const juce::String& containerLabel,
                          bool changed,
                          juce::StringArray& safeKeys,
                          juce::StringArray& transformedKeys)
{
    for (const auto& key : metadata.getAllKeys())
    {
        if (!key.startsWithIgnoreCase(prefix))
            continue;

        safeKeys.addIfNotAlreadyThere(key);
        if (changed)
            transformedKeys.addIfNotAlreadyThere(key);
    }

    safeKeys.addIfNotAlreadyThere(containerLabel);
    if (changed)
        transformedKeys.addIfNotAlreadyThere(containerLabel);
}

struct CuePointMetadata
{
    std::map<juce::String, juce::String> values;
    int64_t identifier = 0;
    int64_t offset = 0;
};

bool transformCueMetadata(juce::StringPairArray& metadata,
                          const juce::Range<int64_t>& deletion,
                          juce::StringArray& safeKeys,
                          juce::StringArray& transformedKeys)
{
    if (!metadata.containsKey("NumCuePoints"))
        return true;

    int64_t cueCountValue = 0;
    if (!parseNonNegativeMetadataValue(metadata["NumCuePoints"], cueCountValue)
        || cueCountValue > 100000)
    {
        return false;
    }

    const auto cueCount = static_cast<int>(cueCountValue);
    const juce::StringArray fields { "Identifier", "Order", "ChunkID", "ChunkStart", "BlockStart", "Offset" };
    std::vector<CuePointMetadata> survivingCues;
    std::vector<int64_t> removedIdentifiers;
    std::map<int64_t, int64_t> originalOffsets;
    bool changed = false;

    for (int index = 0; index < cueCount; ++index)
    {
        CuePointMetadata cue;
        const auto prefix = "Cue" + juce::String(index);
        for (const auto& field : fields)
        {
            const auto key = prefix + field;
            if (metadata.containsKey(key))
                cue.values[field] = metadata[key];
        }

        if (!parseNonNegativeMetadataValue(metadata.getValue(prefix + "Identifier", juce::String(index)),
                                           cue.identifier)
            || !parseNonNegativeMetadataValue(metadata.getValue(prefix + "Offset", "0"), cue.offset))
        {
            return false;
        }

        int64_t chunkStart = 0;
        int64_t blockStart = 0;
        if (!parseNonNegativeMetadataValue(metadata.getValue(prefix + "ChunkStart", "0"), chunkStart)
            || !parseNonNegativeMetadataValue(metadata.getValue(prefix + "BlockStart", "0"), blockStart)
            || chunkStart != 0
            || blockStart != 0)
        {
            return false;
        }

        originalOffsets[cue.identifier] = cue.offset;
        if (deletion.contains(cue.offset))
        {
            removedIdentifiers.push_back(cue.identifier);
            changed = true;
            continue;
        }

        if (cue.offset >= deletion.getEnd())
        {
            cue.offset -= deletion.getLength();
            cue.values["Offset"] = juce::String(cue.offset);
            changed = true;
        }
        survivingCues.push_back(std::move(cue));
    }

    removeIndexedMetadata(metadata, "Cue", cueCount, fields);
    metadata.set("NumCuePoints", juce::String(static_cast<int>(survivingCues.size())));
    for (int index = 0; index < static_cast<int>(survivingCues.size()); ++index)
    {
        const auto prefix = "Cue" + juce::String(index);
        for (const auto& [field, value] : survivingCues[static_cast<std::size_t>(index)].values)
            metadata.set(prefix + field, value);
    }

    const auto compactReferences = [&](const juce::String& countKey,
                                       const juce::String& prefix,
                                       const juce::StringArray& referenceFields,
                                       bool adjustRegionLength) -> bool
    {
        if (!metadata.containsKey(countKey))
            return true;

        int64_t countValue = 0;
        if (!parseNonNegativeMetadataValue(metadata[countKey], countValue) || countValue > 100000)
            return false;

        std::vector<std::map<juce::String, juce::String>> surviving;
        const auto count = static_cast<int>(countValue);
        for (int index = 0; index < count; ++index)
        {
            const auto itemPrefix = prefix + juce::String(index);
            int64_t identifier = 0;
            if (!parseNonNegativeMetadataValue(metadata.getValue(itemPrefix + "Identifier", "0"), identifier))
                return false;

            std::map<juce::String, juce::String> values;
            for (const auto& field : referenceFields)
            {
                const auto key = itemPrefix + field;
                if (metadata.containsKey(key))
                    values[field] = metadata[key];
            }

            if (std::find(removedIdentifiers.begin(), removedIdentifiers.end(), identifier)
                != removedIdentifiers.end())
            {
                changed = true;
                continue;
            }

            if (adjustRegionLength && values.count("SampleLength") != 0)
            {
                int64_t sampleLength = 0;
                const auto offset = originalOffsets.find(identifier);
                if (offset == originalOffsets.end()
                    || !parseNonNegativeMetadataValue(values["SampleLength"], sampleLength)
                    || sampleLength > std::numeric_limits<int64_t>::max() - offset->second)
                {
                    return false;
                }

                const juce::Range<int64_t> region(offset->second, offset->second + sampleLength);
                const auto overlap = region.getIntersectionWith(deletion).getLength();
                if (overlap > 0)
                {
                    values["SampleLength"] = juce::String(sampleLength - overlap);
                    changed = true;
                }
            }
            surviving.push_back(std::move(values));
        }

        removeIndexedMetadata(metadata, prefix, count, referenceFields);
        metadata.set(countKey, juce::String(static_cast<int>(surviving.size())));
        for (int index = 0; index < static_cast<int>(surviving.size()); ++index)
            for (const auto& [field, value] : surviving[static_cast<std::size_t>(index)])
                metadata.set(prefix + juce::String(index) + field, value);
        return true;
    };

    if (!compactReferences("NumCueLabels", "CueLabel", { "Identifier", "Text" }, false)
        || !compactReferences("NumCueNotes", "CueNote", { "Identifier", "Text" }, false)
        || !compactReferences("NumCueRegions", "CueRegion",
                              { "Identifier", "SampleLength", "Purpose", "Country", "Language", "Dialect", "CodePage", "Text" },
                              true))
    {
        return false;
    }

    markTimelineMetadata(metadata, "Cue", "WAV cue points", changed, safeKeys, transformedKeys);
    for (const auto& countKey : juce::StringArray { "NumCuePoints", "NumCueLabels", "NumCueNotes", "NumCueRegions" })
    {
        if (!metadata.containsKey(countKey))
            continue;
        safeKeys.addIfNotAlreadyThere(countKey);
        if (changed)
            transformedKeys.addIfNotAlreadyThere(countKey);
    }

    if (metadata.containsKey("NumCueLabels") || metadata.containsKey("NumCueNotes") || metadata.containsKey("NumCueRegions"))
    {
        safeKeys.addIfNotAlreadyThere("WAV LIST metadata");
        if (changed)
            transformedKeys.addIfNotAlreadyThere("WAV LIST metadata");
    }
    return true;
}

bool transformSamplerLoopMetadata(juce::StringPairArray& metadata,
                                  const juce::Range<int64_t>& deletion,
                                  juce::StringArray& safeKeys,
                                  juce::StringArray& transformedKeys)
{
    if (!metadata.containsKey("NumSampleLoops"))
        return true;

    int64_t loopCountValue = 0;
    if (!parseNonNegativeMetadataValue(metadata["NumSampleLoops"], loopCountValue)
        || loopCountValue > 64)
    {
        return false;
    }

    const juce::StringArray fields { "Identifier", "Type", "Start", "End", "Fraction", "PlayCount" };
    std::vector<std::map<juce::String, juce::String>> surviving;
    const auto loopCount = static_cast<int>(loopCountValue);
    bool changed = false;
    for (int index = 0; index < loopCount; ++index)
    {
        const auto prefix = "Loop" + juce::String(index);
        int64_t start = 0;
        int64_t end = 0;
        if (!parseNonNegativeMetadataValue(metadata.getValue(prefix + "Start", "0"), start)
            || !parseNonNegativeMetadataValue(metadata.getValue(prefix + "End", "0"), end)
            || end < start
            || end == std::numeric_limits<int64_t>::max())
        {
            return false;
        }

        std::map<juce::String, juce::String> values;
        for (const auto& field : fields)
        {
            const auto key = prefix + field;
            if (metadata.containsKey(key))
                values[field] = metadata[key];
        }

        const juce::Range<int64_t> loopRange(start, end + 1);
        const auto remainingLength = loopRange.getLength()
                                   - loopRange.getIntersectionWith(deletion).getLength();
        if (remainingLength <= 0)
        {
            changed = true;
            continue;
        }

        const auto transformedStart = start >= deletion.getEnd()
                                    ? start - deletion.getLength()
                                    : juce::jmin(start, deletion.getStart());
        const auto transformedEnd = transformedStart + remainingLength - 1;
        if (transformedStart != start || transformedEnd != end)
            changed = true;
        values["Start"] = juce::String(transformedStart);
        values["End"] = juce::String(transformedEnd);
        surviving.push_back(std::move(values));
    }

    removeIndexedMetadata(metadata, "Loop", loopCount, fields);
    metadata.set("NumSampleLoops", juce::String(static_cast<int>(surviving.size())));
    for (int index = 0; index < static_cast<int>(surviving.size()); ++index)
        for (const auto& [field, value] : surviving[static_cast<std::size_t>(index)])
            metadata.set("Loop" + juce::String(index) + field, value);

    safeKeys.addIfNotAlreadyThere("NumSampleLoops");
    if (changed)
        transformedKeys.addIfNotAlreadyThere("NumSampleLoops");
    markTimelineMetadata(metadata, "Loop", "WAV sampler loops", changed, safeKeys, transformedKeys);
    return true;
}

void transformTimelineMetadataForDeletion(SingleTrackDocument::State& state,
                                          const juce::Range<int64_t>& deletion)
{
    state.hasTimelineEdits = true;
    auto metadata = state.info.metadata;
    auto safeKeys = state.safeTimelineMetadataKeys;
    auto transformedKeys = state.transformedMetadataKeys;

    if (metadata.containsKey(juce::WavAudioFormat::bwavTimeReference))
    {
        int64_t timeReference = 0;
        if (!parseNonNegativeMetadataValue(metadata[juce::WavAudioFormat::bwavTimeReference], timeReference)
            || (deletion.getStart() == 0
                && timeReference > std::numeric_limits<int64_t>::max() - deletion.getLength()))
        {
            return;
        }

        const auto changed = deletion.getStart() == 0;
        if (changed)
            metadata.set(juce::WavAudioFormat::bwavTimeReference,
                         juce::String(timeReference + deletion.getLength()));
        safeKeys.addIfNotAlreadyThere(juce::WavAudioFormat::bwavTimeReference);
        safeKeys.addIfNotAlreadyThere("BWF bext");
        if (changed)
        {
            transformedKeys.addIfNotAlreadyThere(juce::WavAudioFormat::bwavTimeReference);
            transformedKeys.addIfNotAlreadyThere("BWF bext");
        }
    }

    if (!transformCueMetadata(metadata, deletion, safeKeys, transformedKeys)
        || !transformSamplerLoopMetadata(metadata, deletion, safeKeys, transformedKeys))
    {
        return;
    }

    state.info.metadata = std::move(metadata);
    state.safeTimelineMetadataKeys = std::move(safeKeys);
    state.transformedMetadataKeys = std::move(transformedKeys);
}

bool isOggMetadataKey(const juce::String& key)
{
    return key == juce::OggVorbisAudioFormat::encoderName
        || key == juce::OggVorbisAudioFormat::id3title
        || key == juce::OggVorbisAudioFormat::id3artist
        || key == juce::OggVorbisAudioFormat::id3album
        || key == juce::OggVorbisAudioFormat::id3comment
        || key == juce::OggVorbisAudioFormat::id3date
        || key == juce::OggVorbisAudioFormat::id3genre
        || key == juce::OggVorbisAudioFormat::id3trackNumber;
}

SingleTrackDocument::SaveMetadataReport buildSaveMetadataReport(
    const SingleTrackDocument::FileInfo& sourceInfo,
    const juce::File& targetFile,
    SingleTrackDocument::SaveIntent intent,
    bool timelineEdited,
    const juce::StringArray& safeTimelineKeys,
    const juce::StringArray& transformedMetadataKeys)
{
    SingleTrackDocument::SaveMetadataReport report;
    const auto sourceExtension = sourceInfo.file.getFileExtension().toLowerCase();
    const auto targetExtension = targetFile.getFileExtension().toLowerCase();

    auto classify = [&](const juce::String& key, bool targetCanPreserve)
    {
        if (key.equalsIgnoreCase("MetaDataSource"))
            return;

        if (!targetCanPreserve)
        {
            report.omittedKeys.addIfNotAlreadyThere(key);
            return;
        }

        if (timelineEdited && isTimelineMetadata(key))
        {
            if (!safeTimelineKeys.contains(key))
            {
                report.omittedKeys.addIfNotAlreadyThere(key);
                return;
            }

            if (transformedMetadataKeys.contains(key))
                report.transformedKeys.addIfNotAlreadyThere(key);
            else
                report.preservedKeys.addIfNotAlreadyThere(key);
            return;
        }

        report.preservedKeys.addIfNotAlreadyThere(key);
    };

    for (const auto& key : sourceInfo.metadata.getAllKeys())
    {
        const auto targetCanPreserve = (targetExtension == ".wav" && sourceExtension == ".wav")
                                    || ((targetExtension == ".aif" || targetExtension == ".aiff")
                                        && (sourceExtension == ".aif" || sourceExtension == ".aiff"))
                                    || (targetExtension == ".ogg" && isOggMetadataKey(key))
                                    || (targetExtension == ".wav" && key.equalsIgnoreCase("ChannelMask"));
        classify(key, targetCanPreserve);
    }

    for (const auto& label : inspectContainerMetadata(sourceInfo.file))
    {
        const auto isUnsupported = label.startsWith("Unsupported ");
        const auto sameWave = sourceExtension == ".wav" && targetExtension == ".wav";
        const auto sameAiff = (sourceExtension == ".aif" || sourceExtension == ".aiff")
                           && (targetExtension == ".aif" || targetExtension == ".aiff");
        const auto targetCanPreserve = !isUnsupported
                                    && ((sameWave && !label.startsWith("FLAC") && !label.startsWith("ID3"))
                                        || (sameAiff && label.startsWith("AIFF")));
        classify(label, targetCanPreserve);
    }

    const auto strictReplacement = intent == SingleTrackDocument::SaveIntent::checkpoint
                                || sourceInfo.file == targetFile;
    if (strictReplacement && !report.omittedKeys.isEmpty())
    {
        report.rejectedKeys = report.omittedKeys;
        report.omittedKeys.clear();
    }

    if (!report.rejectedKeys.isEmpty())
        report.description = "Metadata rejected: " + report.rejectedKeys.joinIntoString(", ");
    else if (!report.omittedKeys.isEmpty())
        report.description = "Metadata omitted: " + report.omittedKeys.joinIntoString(", ");
    else if (!report.transformedKeys.isEmpty())
        report.description = "Metadata transformed: " + report.transformedKeys.joinIntoString(", ");
    else if (!report.preservedKeys.isEmpty())
        report.description = "Metadata preserved (" + juce::String(report.preservedKeys.size()) + " item(s))";
    else
        report.description = "No source metadata";
    return report;
}

juce::String ambiXPresetIdForMetadata(int channelCount, const juce::StringPairArray& metadata)
{
    juce::String presetId;
    switch (channelCount)
    {
        case 4:  presetId = "foa-acn-sn3d"; break;
        case 9:  presetId = "2oa-acn-sn3d"; break;
        case 16: presetId = "3oa-acn-sn3d"; break;
        default: return {};
    }

    for (const auto& key : metadata.getAllKeys())
    {
        const auto normalisedKey = key.toLowerCase();
        if (!normalisedKey.contains("coding history")
            && !normalisedKey.contains("description")
            && !normalisedKey.contains("comment"))
        {
            continue;
        }

        // AmbiX is an explicit ACN/SN3D format marker, unlike a generic
        // "Ambisonic" description that may refer to FuMa or rendered audio.
        if (metadata[key].containsIgnoreCase("ambix"))
            return presetId;
    }

    return {};
}

ChannelLayoutState makeChannelLayoutForMetadata(int channelCount,
                                                const juce::StringPairArray& metadata,
                                                const juce::String& selectedPresetId = {})
{
    const auto waveChannelMask = parseWaveChannelMask(metadata);
    if (selectedPresetId.isNotEmpty())
        return ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(channelCount,
                                                                          waveChannelMask,
                                                                          selectedPresetId);

    if (const auto ambiXPresetId = ambiXPresetIdForMetadata(channelCount, metadata);
        ambiXPresetId.isNotEmpty())
    {
        auto layout = ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(channelCount,
                                                                                  waveChannelMask,
                                                                                  ambiXPresetId);
        layout.detectedFromMetadata = true;
        return layout;
    }

    return ChannelLayoutCatalog::layoutForPresetIdWithWaveChannelMask(channelCount,
                                                                      waveChannelMask,
                                                                      {});
}

juce::AudioChannelSet makeWriterChannelLayoutForWaveMask(int channelCount, juce::uint32 channelMask)
{
    if (channelCount <= 0 || channelMask == 0)
        return {};

    auto layout = juce::AudioChannelSet::fromWaveChannelMask(static_cast<juce::int32>(channelMask));
    return layout.size() == channelCount ? layout : juce::AudioChannelSet();
}

bool shouldPreserveWaveChannelMaskOnWrite(const juce::File& targetFile) noexcept
{
    const auto extension = targetFile.getFileExtension().toLowerCase();
    return extension == ".wav" || extension == ".bwf";
}

SingleTrackDocument::FileInfo makeFileInfoFromReader(const juce::File& file, const juce::AudioFormatReader& reader)
{
    SingleTrackDocument::FileInfo info;
    info.file = file;
    info.formatName = file.hasFileExtension(".wem") ? juce::String("Wwise WEM")
                                                    : reader.getFormatName();
    info.metadata = reader.metadataValues;
    if (file.hasFileExtension(".wem"))
        info.metadata.set("Container", "Wwise Streamed Media");
    info.sampleRate = reader.sampleRate;
    info.numChannels = static_cast<int>(reader.numChannels);
    info.bitsPerSample = reader.bitsPerSample;
    info.numSamples = reader.lengthInSamples;
    info.durationSeconds = reader.sampleRate > 0.0 ? static_cast<double>(reader.lengthInSamples) / reader.sampleRate : 0.0;
    info.channelLayout = makeChannelLayoutForMetadata(info.numChannels, info.metadata);
    return info;
}

void setPreparedReaderInfo(SingleTrackDocument::PreparedFileLoad& prepared, const juce::File& file,
                           const juce::AudioFormatReader& reader)
{
    const auto admission = media::admitReader(reader.sampleRate, reader.numChannels, reader.lengthInSamples);
    prepared.admissionFailure = admission.failure;
    if (!admission)
    {
        prepared.info = {};
        prepared.errorMessage = media::describeReaderFailure(admission.failure);
        return;
    }
    prepared.info = makeFileInfoFromReader(file, reader);
}

bool supportsMemoryMappedOpen(const juce::File& file)
{
    if (!file.existsAsFile() || file.hasFileExtension(".wem"))
        return false;

    const auto extension = file.getFileExtension().toLowerCase();
    return extension == ".wav"
        || extension == ".aif"
        || extension == ".aiff";
}

bool shouldMapEntireFileOnOpen(const juce::File& file)
{
    // Small PCM files can still afford an eager whole-file map. Large files stay
    // section-mapped so startup reaches a truthful playable state first.
    constexpr int64_t eagerMemoryMapLimitBytes = 64 * 1024 * 1024;
    return file.getSize() <= eagerMemoryMapLimitBytes;
}

std::shared_ptr<juce::MemoryMappedAudioFormatReader> createMemoryMappedReaderForFile(const juce::File& file)
{
    if (!supportsMemoryMappedOpen(file))
        return {};

    const auto mapEntireFileOnCreate = shouldMapEntireFileOnOpen(file);

    if (auto mappedReader = createDirectMappedReaderForFile(file, mapEntireFileOnCreate))
        return mappedReader;

    if (auto* format = getThreadLocalReadFormatManager().findFormatForFileExtension(file.getFileExtension()))
        return createMappedReaderWithFormat(*format, file, mapEntireFileOnCreate);

    return {};
}
} // namespace

SingleTrackDocument::SourceFileIdentity SingleTrackDocument::SourceFileIdentity::capture(
    const juce::File& sourceFile)
{
    SourceFileIdentity identity;
    if (sourceFile == juce::File())
        return identity;

    identity.file = sourceFile;
    identity.size = sourceFile.getSize();
    identity.lastModificationMilliseconds = sourceFile.getLastModificationTime().toMilliseconds();
    identity.valid = identity.size >= 0 && identity.lastModificationMilliseconds > 0;
    return identity;
}

bool SingleTrackDocument::SourceFileIdentity::matchesCurrentFile() const
{
    if (!valid || file == juce::File())
        return false;

    const auto current = capture(file);
    return current.valid
        && current.size == size
        && current.lastModificationMilliseconds == lastModificationMilliseconds;
}

// Large DSP results use the existing independent WAV-reader/recovery path.
// The temporary file is owned by its readers, including every Undo snapshot;
// no persistent cache index, background service or ordinary-open work is added.
class SingleTrackDocument::RenderedAudioStorage final
    : public std::enable_shared_from_this<RenderedAudioStorage>
{
public:
    static std::shared_ptr<RenderedAudioStorage> create(int channels, int64_t frames, double rate)
    {
        const auto size = media::admitMaterialisation(static_cast<uint64_t>(channels), frames);
        if (!size || channels <= 0 || frames <= 0) return {};
        auto result = std::shared_ptr<RenderedAudioStorage>(new RenderedAudioStorage(channels, frames, rate));
        const auto free = result->file.getFile().getParentDirectory().getBytesFreeOnVolume();
        if (free < 0 || static_cast<uint64_t>(free) < size.floatBytes + 64ULL * 1024 * 1024) return {};
        std::unique_ptr<juce::OutputStream> stream(result->file.getFile().createOutputStream());
        if (!stream) return {};
        const auto options = juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(channels)
            .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        juce::WavAudioFormat format;
        result->writer = format.createWriterFor(stream, options);
        return result->writer ? result : nullptr;
    }

    bool append(const juce::AudioBuffer<float>& source, int start, int count)
    {
        if (!writer || count <= 0 || written + count > frames
            || !writer->writeFromAudioSampleBuffer(source, start, count)) return false;
        written += count;
        return written != frames || finish();
    }
    bool read(int64_t start, int count, juce::AudioBuffer<float>& destination) const
    {
        if (!verifier || start < 0 || count <= 0 || start > frames - count) return false;
        destination.setSize(channels, count, false, false, true);
        return verifier->read(&destination, 0, count, start, true, channels > 1);
    }
    bool isComplete() const noexcept { return witness != nullptr && verifier != nullptr && written == frames; }
    juce::File getFile() const { return file.getFile(); }
    media::SourceFileWitness::Ptr getWitness() const { return witness; }
    std::shared_ptr<juce::AudioFormatReader> createOwnedReader();

private:
    class Reader;
    RenderedAudioStorage(int channelCount, int64_t frameCount, double sampleRate)
        : file(".wav"), channels(channelCount), frames(frameCount), rate(sampleRate) {}
    bool finish()
    {
        writer.reset(); // Finalise the floating-point header before admission.
        std::unique_ptr<media::SourceFileLease> guard;
        if (media::SourceFileLease::open(file.getFile(), guard).failed()) return false;
        verifier = SingleTrackDocument::createReaderForFile(file.getFile());
        if (!verifier || !verifier->usesFloatingPointData || verifier->bitsPerSample != 32
            || verifier->numChannels != static_cast<unsigned int>(channels)
            || verifier->lengthInSamples != frames || verifier->sampleRate != rate) return false;
        auto facts = std::make_shared<media::SourceFileWitness>(*guard->witness());
        facts->readerFormat = verifier->getFormatName(); facts->sampleRate = rate;
        facts->channels = channels; facts->bitsPerSample = 32; facts->frames = frames;
        witness = std::move(facts); lease = std::move(guard);
        return true;
    }
    // Destroy readers/writer and the read guard before TemporaryFile cleanup.
    juce::TemporaryFile file;
    int channels = 0;
    int64_t frames = 0, written = 0;
    double rate = 0;
    std::shared_ptr<media::SourceFileLease> lease;
    media::SourceFileWitness::Ptr witness;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::unique_ptr<juce::AudioFormatReader> verifier;
};

class SingleTrackDocument::RenderedAudioStorage::Reader final
    : public juce::AudioFormatReader, public media::IndependentAudioBacking
{
public:
    Reader(std::shared_ptr<RenderedAudioStorage> storage, std::unique_ptr<juce::AudioFormatReader> source)
        : AudioFormatReader(nullptr, source->getFormatName()), backing(std::move(storage)), inner(std::move(source))
    {
        sampleRate = inner->sampleRate; bitsPerSample = inner->bitsPerSample; lengthInSamples = inner->lengthInSamples;
        numChannels = inner->numChannels; usesFloatingPointData = inner->usesFloatingPointData;
        metadataValues = inner->metadataValues;
    }
    bool readSamples(int* const* destination, int channelCount, int offset, juce::int64 start, int count) override
    { return inner->readSamples(destination, channelCount, offset, start, count); }
    media::SourceFileWitness::Ptr validatedBacking() const noexcept override { return backing->getWitness(); }
private:
    std::shared_ptr<RenderedAudioStorage> backing;
    std::unique_ptr<juce::AudioFormatReader> inner;
};

std::shared_ptr<juce::AudioFormatReader> SingleTrackDocument::RenderedAudioStorage::createOwnedReader()
{
    if (!isComplete() || lease->recheck().failed()) return {};
    auto reader = SingleTrackDocument::createReaderForFile(file.getFile());
    if (!reader || reader->lengthInSamples != frames || reader->numChannels != static_cast<unsigned int>(channels)) return {};
    return std::make_shared<Reader>(shared_from_this(), std::move(reader));
}

SingleTrackDocument::EqualLengthAudioMutationTransaction::EqualLengthAudioMutationTransaction(
    StatePtr expectedStateToUse,
    std::shared_ptr<juce::AudioBuffer<float>> sourceAudioToUse,
    std::shared_ptr<juce::AudioBuffer<float>> stagingAudioToUse)
    : expectedState(std::move(expectedStateToUse)),
      sourceAudio(std::move(sourceAudioToUse)),
      stagingAudio(std::move(stagingAudioToUse))
{
}

SingleTrackDocument::EqualLengthAudioMutationTransaction::EqualLengthAudioMutationTransaction(
    StatePtr expectedStateToUse, std::shared_ptr<juce::AudioFormatReader> sourceReaderToUse,
    std::shared_ptr<media::SourceFileLease> sourceLeaseToUse, std::shared_ptr<RenderedAudioStorage> stagingFileToUse)
    : expectedState(std::move(expectedStateToUse)), sourceAudio(expectedState->audioBuffer),
      sourceReader(std::move(sourceReaderToUse)), sourceLease(std::move(sourceLeaseToUse)),
      stagingFile(std::move(stagingFileToUse))
{
}

bool SingleTrackDocument::EqualLengthAudioMutationTransaction::isValid() const noexcept
{
    if (stagingFile)
        return expectedState && expectedState->info.isValid()
            && ((sourceAudio && sourceAudio->getNumChannels() == expectedState->info.numChannels
                && sourceAudio->getNumSamples() == expectedState->info.numSamples)
                || (sourceReader && sourceReader->numChannels == static_cast<unsigned int>(expectedState->info.numChannels)
                    && sourceReader->lengthInSamples == expectedState->info.numSamples));
    return expectedState != nullptr
        && sourceAudio != nullptr
        && stagingAudio != nullptr
        && sourceAudio->getNumChannels() == stagingAudio->getNumChannels()
        && sourceAudio->getNumSamples() == stagingAudio->getNumSamples()
        && sourceAudio->getNumChannels() > 0
        && sourceAudio->getNumSamples() > 0;
}

int SingleTrackDocument::EqualLengthAudioMutationTransaction::getNumChannels() const noexcept
{
    return isValid() ? expectedState->info.numChannels : 0;
}

int64_t SingleTrackDocument::EqualLengthAudioMutationTransaction::getNumSamples() const noexcept
{
    return isValid() ? expectedState->info.numSamples : 0;
}

double SingleTrackDocument::EqualLengthAudioMutationTransaction::getSampleRate() const noexcept
{
    return expectedState != nullptr ? expectedState->info.sampleRate : 0.0;
}

SingleTrackDocument::StatePtr
SingleTrackDocument::EqualLengthAudioMutationTransaction::getSourceState() const noexcept
{
    return expectedState;
}

bool SingleTrackDocument::EqualLengthAudioMutationTransaction::readSourceBlock(
    int64_t startSample,
    int numSamples,
    juce::AudioBuffer<float>& destination) const
{
    if (!isValid()
        || startSample < 0
        || numSamples <= 0
        || startSample > getNumSamples() - numSamples)
    {
        return false;
    }

    destination.setSize(getNumChannels(), numSamples, false, false, true);
    if (sourceReader)
        return sourceReader->read(&destination, 0, numSamples, startSample, true, getNumChannels() > 1);
    for (int channel = 0; channel < sourceAudio->getNumChannels(); ++channel)
    {
        destination.copyFrom(channel,
                             0,
                             *sourceAudio,
                             channel,
                             static_cast<int>(startSample),
                             numSamples);
    }
    return true;
}

bool SingleTrackDocument::EqualLengthAudioMutationTransaction::readRenderedBlock(
    int64_t startSample,
    int numSamples,
    juce::AudioBuffer<float>& destination) const
{
    if (!isValid()
        || nextWriteSample != getNumSamples()
        || startSample < 0
        || numSamples <= 0
        || startSample > getNumSamples() - numSamples)
    {
        return false;
    }

    if (stagingFile) return stagingFile->read(startSample, numSamples, destination);
    destination.setSize(stagingAudio->getNumChannels(), numSamples, false, false, true);
    for (int channel = 0; channel < stagingAudio->getNumChannels(); ++channel)
    {
        destination.copyFrom(channel,
                             0,
                             *stagingAudio,
                             channel,
                             static_cast<int>(startSample),
                             numSamples);
    }
    return true;
}

bool SingleTrackDocument::EqualLengthAudioMutationTransaction::writeRenderedBlock(
    int64_t startSample,
    const juce::AudioBuffer<float>& source,
    int sourceStartSample,
    int numSamples)
{
    if (!isValid()
        || startSample != nextWriteSample
        || startSample < 0
        || numSamples <= 0
        || startSample > getNumSamples() - numSamples
        || sourceStartSample < 0
        || sourceStartSample + numSamples > source.getNumSamples()
        || source.getNumChannels() < getNumChannels())
    {
        return false;
    }

    if (stagingFile)
    {
        if (!stagingFile->append(source, sourceStartSample, numSamples)) return false;
        nextWriteSample += numSamples;
        return true;
    }
    for (int channel = 0; channel < stagingAudio->getNumChannels(); ++channel)
    {
        stagingAudio->copyFrom(channel,
                               static_cast<int>(startSample),
                               source,
                               channel,
                               sourceStartSample,
                               numSamples);
    }
    nextWriteSample += numSamples;
    return true;
}

SingleTrackDocument::SingleTrackDocument()
    : currentState(makeEmptyState())
{
}

SingleTrackDocument::WemOpenStrategy SingleTrackDocument::getWemOpenStrategy(const juce::File& file)
{
    if (!file.hasFileExtension(".wem"))
        return WemOpenStrategy::notWem;

    const auto strategy = VgmstreamDecodeService::chooseWemOpenStrategy(file);
    return strategy.kind == VgmstreamDecodeService::WemOpenStrategy::Kind::directWaveReader
        ? WemOpenStrategy::directWaveReader
        : WemOpenStrategy::vgmstreamFallback;
}

std::unique_ptr<juce::AudioFormatReader> SingleTrackDocument::createReaderForFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return {};

    if (auto reader = createDirectStandardAudioReaderForFile(file))
        return reader;

    if (WindowsMediaAudioService::isSupportedVideoExtension(file.getFileExtension()))
    {
        juce::String errorMessage;
        const auto cacheFile = WindowsMediaAudioService::getOrCreateVideoAudioCache(file, errorMessage);
        return createDirectStandardAudioReaderForFile(cacheFile);
    }

    if (auto* reader = getThreadLocalReadFormatManager().createReaderFor(file))
        return std::unique_ptr<juce::AudioFormatReader>(reader);

    return createReaderForWem(file);
}

std::unique_ptr<juce::AudioFormatReader> SingleTrackDocument::createReaderForRetainedSource(const media::SourceFileWitness::Ptr& source)
{
    if (!source) return {};
    auto input = media::SourceFileLease::createRetainedInputStream(source->retainedInput());
    if (!input) return {};
    return std::unique_ptr<juce::AudioFormatReader>(getThreadLocalReadFormatManager().createReaderFor(std::move(input)));
}

SingleTrackDocument::PreparedFileLoad SingleTrackDocument::prepareAudioFileForOpen(const juce::File& file,
    const std::shared_ptr<media::OptionalMediaOperation>& optionalOperation)
{
    PreparedFileLoad prepared;
    if (!file.existsAsFile())
        return prepared;

    // Ordinary source admission records physical facts under a short read lease.
    // No persistent handle/service remains after this function. If a busy or
    // unsupported source cannot provide proof, ordinary reading still works;
    // a later recovery consumer must report that missing proof explicitly.
    std::unique_ptr<media::SourceFileLease> sourceLease;
    if (file.hasFileExtension("wav;aif;aiff;flac;ogg;mp3")) media::SourceFileLease::open(file, sourceLease);
    prepared.readerSourceFile = file;
    if (WindowsMediaAudioService::isSupportedVideoExtension(file.getFileExtension()))
    {
        prepared.readerSourceFile = WindowsMediaAudioService::getOrCreateVideoAudioCache(file,
                                                                                         prepared.errorMessage, optionalOperation);
        if (!prepared.readerSourceFile.existsAsFile())
            return prepared;

        // The completed, sealed WAV is the actual playback backing. Capture its
        // independent read facts under a short lease; these are not facts about
        // the original video and must not grant original-source identity/Save.
        media::SourceFileLease::open(prepared.readerSourceFile, sourceLease);
        prepared.ownedReader = createDirectStandardAudioReaderForFile(prepared.readerSourceFile);
        if (prepared.ownedReader == nullptr)
        {
            prepared.errorMessage = "The decoded video audio cache could not be opened.";
            return prepared;
        }

        setPreparedReaderInfo(prepared, file, *prepared.ownedReader);
        if (!prepared.info.isValid())
        {
            prepared.ownedReader.reset();
            if (prepared.errorMessage.isEmpty())
                prepared.errorMessage = "The decoded video audio cache contains no readable samples.";
            return prepared;
        }
        prepared.info.formatName = file.getFileExtension().trimCharactersAtStart(".").toUpperCase()
                                 + " audio track";
        prepared.sourceFileWitness = sourceFactsForReader(sourceLease.get(), *prepared.ownedReader);
        return prepared;
    }

    if (file.hasFileExtension(".wem")
        && VgmstreamDecodeService::chooseWemOpenStrategy(file).kind
               == VgmstreamDecodeService::WemOpenStrategy::Kind::decodedFallback)
    {
        prepared.playbackAvailability = PlaybackAvailability::contiguousPrefix(0);
        auto availability = prepared.playbackAvailability;
        juce::StringArray diagnostics;
        prepared.ownedReader = VgmstreamDecodeService::createProgressiveReaderForDecodedFile(
            file,
            [availability](int64_t readyEndSample, bool decodeComplete, bool decodeFailed)
            {
                if (decodeFailed)
                {
                    availability.markUnavailable();
                    return;
                }

                availability.advanceContiguousPrefix(readyEndSample);
                if (decodeComplete)
                    availability.markRandomAccess();
            },
            prepared.readerSourceFile,
            &diagnostics, optionalOperation);
        if (prepared.ownedReader == nullptr)
            return {};

        setPreparedReaderInfo(prepared, file, *prepared.ownedReader);
        if (!prepared.info.isValid())
        {
            prepared.ownedReader.reset();
            return prepared;
        }
        return prepared;
    }

    if (auto mappedReader = createMemoryMappedReaderForFile(file))
    {
        setPreparedReaderInfo(prepared, file, *mappedReader);
        if (prepared.info.isValid())
        {
            prepared.memoryMappedReader = std::move(mappedReader);
            prepared.sourceFileWitness = sourceFactsForReader(sourceLease.get(), *prepared.memoryMappedReader);
            return prepared;
        }
    }

    prepared.ownedReader = createReaderForFile(file);
    if (prepared.ownedReader == nullptr)
        return prepared;

    setPreparedReaderInfo(prepared, file, *prepared.ownedReader);
    if (!prepared.info.isValid())
        prepared.ownedReader.reset();

    if (prepared.ownedReader) prepared.sourceFileWitness = sourceFactsForReader(sourceLease.get(), *prepared.ownedReader);
    return prepared;
}

SingleTrackDocument::FileInfo SingleTrackDocument::buildFileInfoForReader(const juce::File& file, const juce::AudioFormatReader& reader)
{
    if (!media::admitReader(reader.sampleRate, reader.numChannels, reader.lengthInSamples))
        return {};

    return makeFileInfoFromReader(file, reader);
}

SingleTrackDocument::FileInfo SingleTrackDocument::probeFileInfo(const juce::File& file)
{
    return prepareAudioFileForOpen(file).info;
}

std::shared_ptr<SingleTrackDocument::State> SingleTrackDocument::createPreviewState(const FileInfo& info)
{
    if (!info.isValid() || !media::admitReader(info.sampleRate, static_cast<uint64_t>(info.numChannels), info.numSamples))
        return {};

    auto newState = std::make_shared<State>();
    newState->info = info;
    newState->audioBuffer.reset();
    newState->audioBackingStore = State::AudioBackingStore::previewOnly;
    newState->envelopePoints = makeDefaultEnvelope(newState->info.durationSeconds);
    return newState;
}

std::shared_ptr<SingleTrackDocument::State> SingleTrackDocument::createStateFromFile(const juce::File& file)
{
    return createStateFromPreparedFile(prepareAudioFileForOpen(file));
}

std::shared_ptr<SingleTrackDocument::State> SingleTrackDocument::createStateFromPreparedFile(PreparedFileLoad prepared)
{
    auto* reader = prepared.getReader();
    if (reader == nullptr || !prepared.info.isValid()
        || !media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples)
        || prepared.info.sampleRate != reader->sampleRate
        || prepared.info.numSamples != reader->lengthInSamples
        || static_cast<uint64_t>(prepared.info.numChannels) != reader->numChannels)
        return {};

    auto newState = std::make_shared<State>();
    newState->info = prepared.info;
    newState->info.sourceAdmitted = true;
    newState->audioBuffer.reset();
    newState->readerSourceFile = prepared.readerSourceFile;
    newState->sourceFileWitness = std::move(prepared.sourceFileWitness);
    newState->matchesSourceFileAudio = true;

    if (prepared.memoryMappedReader != nullptr)
    {
        newState->memoryMappedReader = std::move(prepared.memoryMappedReader);
        newState->audioBackingStore = State::AudioBackingStore::memoryMappedFile;
    }
    else
    {
        newState->streamingReader = std::shared_ptr<juce::AudioFormatReader>(std::move(prepared.ownedReader));
        newState->audioBackingStore = State::AudioBackingStore::streamingReader;
    }

    newState->playbackAvailability = prepared.playbackAvailability;
    newState->envelopePoints = makeDefaultEnvelope(newState->info.durationSeconds);
    return newState;
}

std::shared_ptr<SingleTrackDocument::State> SingleTrackDocument::createStateFromOwnedReader(const juce::File& file,
                                                                                             std::unique_ptr<juce::AudioFormatReader> reader)
{
    if (reader == nullptr)
        return {};

    if (!media::admitReader(reader->sampleRate, reader->numChannels, reader->lengthInSamples))
        return {};

    auto newState = std::make_shared<State>();
    newState->info = buildFileInfoForReader(file, *reader);
    newState->info.sourceAdmitted = true;
    newState->audioBuffer.reset();
    newState->streamingReader = std::shared_ptr<juce::AudioFormatReader>(std::move(reader));
    newState->readerSourceFile = file;
    newState->matchesSourceFileAudio = true;
    newState->audioBackingStore = State::AudioBackingStore::streamingReader;
    newState->playbackAvailability = PlaybackAvailability::randomAccess();

    newState->envelopePoints = makeDefaultEnvelope(newState->info.durationSeconds);
    return newState;
}

std::shared_ptr<SingleTrackDocument::State> SingleTrackDocument::createStateFromReader(const juce::File& file, juce::AudioFormatReader& reader)
{
    if (!media::admitReader(reader.sampleRate, reader.numChannels, reader.lengthInSamples))
        return {};

    auto newState = std::make_shared<State>();
    newState->info = buildFileInfoForReader(file, reader);
    newState->info.sourceAdmitted = true;
    newState->readerSourceFile = file;
    newState->matchesSourceFileAudio = true;

    std::unique_ptr<media::SourceFileLease> sourceLease;
    media::SourceFileLease::open(file, sourceLease);
    if (auto mappedReader = createMemoryMappedReaderForFile(file))
    {
        if (mappedReader->sampleRate != newState->info.sampleRate || mappedReader->numChannels != static_cast<unsigned int>(newState->info.numChannels)
            || mappedReader->lengthInSamples != newState->info.numSamples) return {};
        newState->sourceFileWitness = sourceFactsForReader(sourceLease.get(), *mappedReader);
        newState->audioBuffer.reset();
        newState->memoryMappedReader = std::move(mappedReader);
        newState->audioBackingStore = State::AudioBackingStore::memoryMappedFile;
    }
    else
    {
        newState->audioBuffer = allocateMaterialisedBuffer(newState->info.numChannels, reader.lengthInSamples);
        if (newState->audioBuffer == nullptr) return {};
        newState->audioBuffer->clear();
        if (!reader.read(newState->audioBuffer.get(),
                    0,
                    static_cast<int>(reader.lengthInSamples),
                    0,
                    true,
                    newState->info.numChannels > 1)) return {};
        newState->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    }

    newState->playbackAvailability = PlaybackAvailability::randomAccess();
    newState->envelopePoints = makeDefaultEnvelope(newState->info.durationSeconds);
    return newState;
}

bool SingleTrackDocument::loadFile(const juce::File& file)
{
    return adoptLoadedState(createStateFromFile(file));
}

bool SingleTrackDocument::adoptPreviewState(const FileInfo& info)
{
    if (info.recoveryBacked) return false;
    auto state = createPreviewState(info);
    if (state == nullptr || !state->info.isValid())
        return false;

    state->audioRevision = nextAudioRevisionNumber();
    state->envelopeRevision = nextEnvelopeRevisionNumber();
    state->changeGeneration = nextChangeGenerationNumber();
    sanitiseEnvelope(state->envelopePoints, state->info.durationSeconds);

    {
        const juce::ScopedLock sl(stateLock);
        undoStack.clear();
        redoStack.clear();
        envelopeGestureActive = false;
        envelopeGestureDidMutate = false;
        envelopeGestureUndoState.reset();
        documentTargetFile = state->info.file;
        documentSourceIdentity = {};
        documentSourceVersion.reset();
        cleanChangeGeneration = state->changeGeneration;
        recoveryRequiresSaveAs = false;
        recoveryOrigin.reset();
        lineageIdentity = juce::Uuid();
        sourceBindingIdentity = juce::Uuid();
        currentState = state;
    }

    notifyDocumentChanged(state);
    return true;
}

bool SingleTrackDocument::adoptLoadedState(std::shared_ptr<State> state)
{
    if (state == nullptr || state->info.recoveryBacked || state->audioBackingStore == State::AudioBackingStore::recoveryStream
        || !state->info.isValid() || state->isPreviewOnly() || !state->hasPlaybackSource()
        || !media::admitReader(state->info.sampleRate, static_cast<uint64_t>(state->info.numChannels), state->info.numSamples))
        return false;

    if (state->info.channelLayout.isAmbisonic
        && AmbisonicMonitorDecoder::statusFor(state->info.channelLayout)
               != AmbisonicMonitorDecodeStatus::ready)
    {
        return false;
    }

    const auto sourceIdentity = SourceFileIdentity::capture(state->info.file);
    state->audioRevision = nextAudioRevisionNumber();
    state->envelopeRevision = nextEnvelopeRevisionNumber();
    state->changeGeneration = nextChangeGenerationNumber();
    sanitiseEnvelope(state->envelopePoints, state->info.durationSeconds);

    {
        const juce::ScopedLock sl(stateLock);
        undoStack.clear();
        redoStack.clear();
        envelopeGestureActive = false;
        envelopeGestureDidMutate = false;
        envelopeGestureUndoState.reset();
        documentTargetFile = state->info.file;
        documentSourceIdentity = sourceIdentity;
        documentSourceVersion = state->sourceFileWitness && state->sourceFileWitness->file == sourceIdentity.file
            ? state->sourceFileWitness : media::SourceFileWitness::Ptr {};
        cleanChangeGeneration = state->changeGeneration;
        recoveryRequiresSaveAs = false;
        recoveryOrigin.reset();
        lineageIdentity = juce::Uuid();
        sourceBindingIdentity = juce::Uuid();
        currentState = state;
    }

    notifyDocumentChanged(state);
    return true;
}

void SingleTrackDocument::clear()
{
    const auto empty = makeEmptyState();
    {
        const juce::ScopedLock sl(stateLock);
        undoStack.clear();
        redoStack.clear();
        envelopeGestureActive = false;
        envelopeGestureDidMutate = false;
        envelopeGestureUndoState.reset();
        documentTargetFile = {};
        documentSourceIdentity = {};
        documentSourceVersion.reset();
        cleanChangeGeneration = 0;
        recoveryRequiresSaveAs = false;
        recoveryOrigin.reset();
        lineageIdentity = juce::Uuid(nullptr);
        sourceBindingIdentity = juce::Uuid(nullptr);
        currentState = empty;
    }

    notifyDocumentChanged(empty);
}

media::IdentitySnapshot SingleTrackDocument::getMediaIdentity() const
{
    const juce::ScopedLock sl(stateLock);
    media::IdentitySnapshot identity;
    identity.document = { lineageIdentity, runtimeIdentity, currentState != nullptr ? currentState->changeGeneration : 0 };
    identity.source = { sourceBindingIdentity, { documentTargetFile }, documentSourceIdentity.size,
                        documentSourceIdentity.lastModificationMilliseconds, documentSourceIdentity.valid };
    identity.savedGeneration = cleanChangeGeneration;
    return identity;
}

std::optional<SingleTrackDocument::CommittedStateSnapshot> SingleTrackDocument::captureCommittedState(
    const media::DocumentIdentity& expected) const
{
    const juce::ScopedLock sl(stateLock);
    const auto identity = getMediaIdentity();
    if (!identity.document.sameVersion(expected)) return std::nullopt;
    auto mutation = lastCanonicalMutation;
    if (mutation && (!mutation->after.sameVersion(identity.document) || mutation->sourceBinding != identity.source.binding)) mutation.reset();
    return CommittedStateSnapshot { identity, currentState, undoStack, redoStack, std::move(mutation), documentSourceVersion };
}

std::unique_ptr<SingleTrackDocument> SingleTrackDocument::createRecoveredCopy(
    const CommittedStateSnapshot& snapshot, juce::String& error)
{
    error.clear();
    if (!snapshot.identity.document.isValid() || snapshot.identity.source.binding.isNull()
        || snapshot.identity.asset.has_value() || !snapshot.state
        || snapshot.state->changeGeneration != snapshot.identity.document.generation
        || snapshot.undo.size() > maxUndoSteps || snapshot.redo.size() > maxUndoSteps)
    { error = "recovery_snapshot_contract"; return {}; }
    try
    {
        std::vector<StatePtr> states { snapshot.state };
        states.insert(states.end(), snapshot.undo.begin(), snapshot.undo.end());
        states.insert(states.end(), snapshot.redo.begin(), snapshot.redo.end());
        std::map<int64_t, int64_t> audioRevisions, envelopeRevisions, generations;
        const auto remap = [] (auto& values, int64_t original)
        {
            const auto next = static_cast<int64_t>(values.size() + 1);
            return values.emplace(original, next).first->second;
        };
        std::vector<StatePtr> adopted;
        for (const auto& source : states)
        {
            if (!source) { error = "recovery_state_not_materialised"; return {}; }
            const auto memoryBacked = source->audioBackingStore == State::AudioBackingStore::inMemoryBuffer
                && source->audioBuffer && source->playbackAvailability.isRandomAccess()
                && media::admitMaterialisation(static_cast<uint64_t>(source->info.numChannels), source->info.numSamples)
                && source->audioBuffer->getNumChannels() == source->info.numChannels
                && source->audioBuffer->getNumSamples() == source->info.numSamples;
            const auto recoveryStream = source->audioBackingStore == State::AudioBackingStore::recoveryStream
                && !source->audioBuffer && !source->memoryMappedReader && source->streamingReader
                && source->readerSourceFile == juce::File()
                && source->streamingReader->sampleRate == source->info.sampleRate
                && source->streamingReader->numChannels == static_cast<unsigned int>(source->info.numChannels)
                && source->streamingReader->lengthInSamples == source->info.numSamples
                && source->streamingReader->usesFloatingPointData && source->streamingReader->bitsPerSample == 32
                && source->playbackAvailability.getAccess() != PlaybackAvailability::Access::unavailable;
            if ((!memoryBacked && !recoveryStream)
                || !media::admitReader(source->info.sampleRate, static_cast<uint64_t>(source->info.numChannels), source->info.numSamples)
                || source->info.file == juce::File() || source->audioRevision < 1 || source->envelopeRevision < 1
                || source->changeGeneration < 1 || source->info.channelLayout.sourceChannelCount != source->info.numChannels
                || source->info.channelLayout.selectedRoles.size() != static_cast<size_t>(source->info.numChannels)
                || source->envelopePoints.size() < 2 || source->envelopePoints.size() > 4096)
            { error = "recovery_state_not_materialised"; return {}; }
            if (adopted.empty() && source->getPlayableSampleRange().isEmpty())
            { error = "recovery_current_audio_not_ready"; return {}; }
            const auto duration = static_cast<double>(source->info.numSamples) / source->info.sampleRate;
            if (source->info.durationSeconds != duration
                || source->envelopePoints.front().timeSeconds != 0 || source->envelopePoints.back().timeSeconds != duration
                || (source->info.channelLayout.isAmbisonic
                    && AmbisonicMonitorDecoder::statusFor(source->info.channelLayout) != AmbisonicMonitorDecodeStatus::ready))
            { error = "recovery_state_semantics"; return {}; }
            double previous = -1;
            for (const auto& point : source->envelopePoints)
            {
                if (!std::isfinite(point.timeSeconds) || !std::isfinite(point.gain) || point.timeSeconds <= previous
                    || point.timeSeconds < 0 || point.timeSeconds > duration || point.gain < 0 || point.gain > 4)
                { error = "recovery_envelope_semantics"; return {}; }
                previous = point.timeSeconds;
            }
            auto state = std::make_shared<State>(*source);
            state->info.recoveryBacked = true;
            state->memoryMappedReader.reset(); state->readerSourceFile = {};
            if (memoryBacked)
            {
                state->streamingReader.reset();
                state->playbackAvailability = PlaybackAvailability::randomAccess();
            }
            state->ambisonicOrientation = {};
            state->ambisonicMonitorMode = AmbisonicMonitorMode::speakerProjection;
            state->ambisonicBinauralFilterBank.reset(); state->ambisonicBinauralStatus.clear();
            // New runtime identities also receive bounded fresh revision spaces.
            // Preserve equality/Undo relationships, never inherit near-overflow counters.
            state->audioRevision = remap(audioRevisions, source->audioRevision);
            state->envelopeRevision = remap(envelopeRevisions, source->envelopeRevision);
            state->changeGeneration = remap(generations, source->changeGeneration);
            adopted.push_back(std::move(state));
        }
        auto result = std::make_unique<SingleTrackDocument>();
        result->currentState = adopted.front();
        for (size_t index = 0; index < snapshot.undo.size(); ++index) result->undoStack.push_back(adopted[index + 1]);
        for (size_t index = 0; index < snapshot.redo.size(); ++index) result->redoStack.push_back(adopted[index + 1 + snapshot.undo.size()]);
        result->documentTargetFile = snapshot.identity.source.location.file;
        if (result->documentTargetFile == juce::File()) result->documentTargetFile = snapshot.state->info.file;
        result->documentSourceIdentity = {};
        result->lineageIdentity = juce::Uuid(); result->sourceBindingIdentity = juce::Uuid();
        result->cleanChangeGeneration = 0;
        result->recoveryRequiresSaveAs = true;
        result->recoveryOrigin = snapshot.identity.document;
        result->nextAudioRevision.store(static_cast<int64_t>(audioRevisions.size() + 1));
        result->nextEnvelopeRevision.store(static_cast<int64_t>(envelopeRevisions.size() + 1));
        result->nextChangeGeneration.store(static_cast<int64_t>(generations.size() + 1));
        result->notifyDocumentChanged(result->currentState);
        return result;
    }
    catch (...) { error = "recovery_adoption_resource_failure"; return {}; }
}

bool SingleTrackDocument::WorkspaceSnapshot::hasUnsavedChanges() const noexcept
{
    return contents.state && contents.state->info.isValid() && contents.state->changeGeneration != 0
        && contents.state->changeGeneration != contents.identity.savedGeneration;
}

bool SingleTrackDocument::WorkspaceSnapshot::sameContents(const WorkspaceSnapshot& other) const noexcept
{
    return contents.identity.document.sameVersion(other.contents.identity.document)
        && contents.identity.savedGeneration == other.contents.identity.savedGeneration
        && contents.state == other.contents.state && contents.undo == other.contents.undo && contents.redo == other.contents.redo
        && target == other.target && sourceVersion == other.sourceVersion && requiresSaveAs == other.requiresSaveAs;
}

SingleTrackDocument::WorkspaceSnapshot::Ptr SingleTrackDocument::captureWorkspaceSnapshot() const
{
    const juce::ScopedLock sl(stateLock);
    if (!currentState || currentState->info.numSamples <= 0 || currentState->info.file == juce::File() || !currentState->hasPlaybackSource()
        || currentState->isPreviewOnly() || envelopeGestureActive || undoStack.size() > maxUndoSteps || redoStack.size() > maxUndoSteps)
        return {};
    auto snapshot = std::shared_ptr<WorkspaceSnapshot>(new WorkspaceSnapshot());
    snapshot->contents = { getMediaIdentity(), currentState, undoStack, redoStack, std::nullopt };
    snapshot->target = documentTargetFile; snapshot->source = documentSourceIdentity;
    snapshot->sourceVersion = documentSourceVersion; snapshot->requiresSaveAs = recoveryRequiresSaveAs;
    snapshot->origin = recoveryOrigin;
    return snapshot;
}

bool SingleTrackDocument::adoptWorkspaceSnapshot(const WorkspaceSnapshot& snapshot, juce::String& error)
{
    error.clear();
    const auto& saved = snapshot.contents;
    if (!saved.state || saved.state->info.numSamples <= 0 || saved.state->info.file == juce::File() || !saved.state->hasPlaybackSource()
        || saved.state->getPlayableSampleRange().isEmpty() || !saved.identity.document.isValid())
    { error = "workspace_snapshot_not_ready"; return false; }
    StatePtr active;
    try
    {
        const juce::ScopedLock sl(stateLock);
        std::map<int64_t, int64_t> audio, envelope, generations;
        const auto remap = [] (auto& map, int64_t old, auto next)
        {
            const auto found = map.find(old);
            if (found != map.end()) return found->second;
            return map.emplace(old, next()).first->second;
        };
        const auto adopt = [&] (const StatePtr& source) -> StatePtr
        {
            auto state = std::make_shared<State>(*source);
            state->audioRevision = remap(audio, source->audioRevision, [&] { return nextAudioRevisionNumber(); });
            state->envelopeRevision = remap(envelope, source->envelopeRevision, [&] { return nextEnvelopeRevisionNumber(); });
            state->changeGeneration = remap(generations, source->changeGeneration, [&] { return nextChangeGenerationNumber(); });
            return state;
        };
        // Allocate the entire replacement before mutating any observable state.
        // Revision counters may skip on allocation failure; identity/history do not.
        active = adopt(saved.state);
        std::vector<StatePtr> undo, redo;
        undo.reserve(saved.undo.size()); redo.reserve(saved.redo.size());
        for (const auto& state : saved.undo) undo.push_back(adopt(state));
        for (const auto& state : saved.redo) redo.push_back(adopt(state));
        const auto clean = saved.identity.savedGeneration == 0 ? int64_t{0}
            : remap(generations, saved.identity.savedGeneration, [&] { return nextChangeGenerationNumber(); });
        const juce::Uuid newLineage, newBinding;
        auto target = snapshot.target; auto source = snapshot.source;
        auto sourceVersion = snapshot.sourceVersion; auto origin = snapshot.origin;
        currentState = active; undoStack = std::move(undo); redoStack = std::move(redo);
        documentTargetFile = std::move(target); documentSourceIdentity = std::move(source);
        documentSourceVersion = std::move(sourceVersion); recoveryOrigin = std::move(origin);
        recoveryRequiresSaveAs = snapshot.requiresSaveAs; cleanChangeGeneration = clean;
        lineageIdentity = newLineage; sourceBindingIdentity = newBinding;
        lastCanonicalMutation.reset(); envelopeGestureUndoState.reset();
        envelopeGestureActive = false; envelopeGestureDidMutate = false;
    }
    catch (...) { error = "workspace_snapshot_adoption_exception"; return false; }
    notifyDocumentChanged(active);
    return true;
}

bool SingleTrackDocument::requiresRecoverySaveAs() const
{
    const juce::ScopedLock sl(stateLock);
    return recoveryRequiresSaveAs;
}

std::optional<media::DocumentIdentity> SingleTrackDocument::getRecoveryOrigin() const
{
    const juce::ScopedLock sl(stateLock);
    return recoveryOrigin;
}

SingleTrackDocument::StatePtr SingleTrackDocument::getState() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return currentState;
}

SingleTrackDocument::FileInfo SingleTrackDocument::getFileInfo() const
{
    const juce::ScopedLock sl(stateLock);
    auto info = currentState->info;
    if (documentTargetFile != juce::File())
        info.file = documentTargetFile;
    return info;
}

bool SingleTrackDocument::hasAudio() const noexcept
{
    return getState()->info.isValid();
}

bool SingleTrackDocument::hasUnsavedChanges() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return currentState != nullptr
        && currentState->info.isValid()
        && currentState->changeGeneration != 0
        && currentState->changeGeneration != cleanChangeGeneration;
}

double SingleTrackDocument::getDurationSeconds() const noexcept
{
    return getState()->info.durationSeconds;
}

int64_t SingleTrackDocument::getNumSamples() const noexcept
{
    return getState()->info.numSamples;
}

double SingleTrackDocument::getSampleRate() const noexcept
{
    return getState()->info.sampleRate;
}

juce::Range<int64_t> SingleTrackDocument::getPlayableSampleRange() const noexcept
{
    return getState()->getPlayableSampleRange();
}

juce::Range<double> SingleTrackDocument::getPlayableTimeRange() const noexcept
{
    const auto state = getState();
    if (state->info.sampleRate <= 0.0)
        return {};

    const auto samples = state->getPlayableSampleRange();
    return { static_cast<double>(samples.getStart()) / state->info.sampleRate,
             static_cast<double>(samples.getEnd()) / state->info.sampleRate };
}

bool SingleTrackDocument::setChannelLayoutPreset(const juce::String& presetId)
{
    auto base = getState();
    if (!base->info.isValid() || presetId.isEmpty())
        return false;

    auto nextLayout = makeChannelLayoutForMetadata(base->info.numChannels, base->info.metadata, presetId);
    if (nextLayout.selectedPresetId == base->info.channelLayout.selectedPresetId)
        return true;

    auto next = std::make_shared<State>(*base);
    next->info.channelLayout = std::move(nextLayout);
    if (!next->info.channelLayout.isAmbisonic)
    {
        next->ambisonicOrientation = {};
        next->ambisonicMonitorMode = AmbisonicMonitorMode::speakerProjection;
        next->ambisonicBinauralFilterBank.reset();
        next->ambisonicBinauralStatus.clear();
    }
    if (next->info.channelLayout.isAmbisonic
        && AmbisonicMonitorDecoder::statusFor(next->info.channelLayout)
               != AmbisonicMonitorDecodeStatus::ready)
    {
        return false;
    }
    next->changeGeneration = base->changeGeneration;
    commitState(std::move(next), false);
    return true;
}

AmbisonicOrientation SingleTrackDocument::getAmbisonicOrientation() const noexcept
{
    return getState()->ambisonicOrientation;
}

bool SingleTrackDocument::setAmbisonicOrientation(AmbisonicOrientation orientation)
{
    auto base = getState();
    if (AmbisonicMonitorDecoder::statusFor(base->info.channelLayout)
            != AmbisonicMonitorDecodeStatus::ready)
    {
        return false;
    }

    orientation = AmbisonicMonitorDecoder::canonicalizeOrientation(orientation);
    const auto& current = base->ambisonicOrientation;
    if (std::abs(current.yawDegrees - orientation.yawDegrees) < 0.0001f
        && std::abs(current.pitchDegrees - orientation.pitchDegrees) < 0.0001f
        && std::abs(current.rollDegrees - orientation.rollDegrees) < 0.0001f)
    {
        return true;
    }

    auto next = std::make_shared<State>(*base);
    next->ambisonicOrientation = orientation;

    next->changeGeneration = base->changeGeneration;
    commitState(std::move(next), false);
    return true;
}

AmbisonicMonitorMode SingleTrackDocument::getAmbisonicMonitorMode() const noexcept
{
    return getState()->ambisonicMonitorMode;
}

bool SingleTrackDocument::setAmbisonicMonitorMode(AmbisonicMonitorMode mode)
{
    auto base = getState();
    if (AmbisonicMonitorDecoder::statusFor(base->info.channelLayout)
            != AmbisonicMonitorDecodeStatus::ready)
        return false;
    if (mode == base->ambisonicMonitorMode
        && (mode != AmbisonicMonitorMode::binauralHrtf
            || base->ambisonicBinauralFilterBank != nullptr))
    {
        return true;
    }

    if (mode == AmbisonicMonitorMode::binauralHrtf)
    {
        juce::String error;
        auto bank = AmbisonicBinauralFilterBank::loadDefault(error);
        return applyPreparedBinauralMonitor(base, std::move(bank), error);
    }

    auto next = std::make_shared<State>(*base);
    next->ambisonicMonitorMode = mode;
    next->ambisonicBinauralStatus.clear();
    next->ambisonicBinauralFilterBank.reset();
    next->changeGeneration = base->changeGeneration;
    commitState(std::move(next), false);
    return true;
}

bool SingleTrackDocument::applyPreparedBinauralMonitor(
    StatePtr expectedState, std::shared_ptr<const AmbisonicBinauralFilterBank> bank,
    const juce::String& errorMessage)
{
    if (!expectedState || AmbisonicMonitorDecoder::statusFor(expectedState->info.channelLayout)
        != AmbisonicMonitorDecodeStatus::ready) return false;
    auto next = std::make_shared<State>(*expectedState);
    next->ambisonicBinauralFilterBank = std::move(bank);
    next->ambisonicMonitorMode = next->ambisonicBinauralFilterBank ? AmbisonicMonitorMode::binauralHrtf
                                                               : AmbisonicMonitorMode::speakerProjection;
    next->ambisonicBinauralStatus = next->ambisonicBinauralFilterBank ? juce::String {} : errorMessage;
    {
        const juce::ScopedLock sl(stateLock);
        // Undo generations may decrease. Only this exact document state may
        // accept the asynchronous result; never overwrite a replacement/edit.
        if (currentState != expectedState) return false;
        currentState = next;
        lastCanonicalMutation.reset();
    }
    notifyDocumentChanged(next);
    return next->ambisonicBinauralFilterBank != nullptr;
}

juce::String SingleTrackDocument::getAmbisonicBinauralStatus() const
{
    return getState()->ambisonicBinauralStatus;
}

std::vector<SingleTrackDocument::EnvelopePoint> SingleTrackDocument::getEnvelopePoints() const
{
    return getState()->envelopePoints;
}

float SingleTrackDocument::getEnvelopeGainAt(double timeSeconds) const noexcept
{
    return evaluateEnvelope(getState()->envelopePoints, timeSeconds);
}

void SingleTrackDocument::resetEnvelope()
{
    auto base = getState();
    if (!base->info.isValid())
        return;

    auto next = std::make_shared<State>(*base);
    next->envelopePoints = makeDefaultEnvelope(base->info.durationSeconds);
    next->envelopeRevision = nextEnvelopeRevisionNumber();
    commitState(std::move(next), true, CanonicalMutation::Kind::envelopeReplace);
}

int SingleTrackDocument::insertEnvelopePoint(double timeSeconds)
{
    auto base = getState();
    if (!base->info.isValid())
        return -1;

    auto next = std::make_shared<State>(*base);
    const auto clampedTime = juce::jlimit(0.0, base->info.durationSeconds, timeSeconds);
    const auto gain = evaluateEnvelope(base->envelopePoints, clampedTime);
    next->envelopePoints.push_back({clampedTime, gain});
    sanitiseEnvelope(next->envelopePoints, base->info.durationSeconds);
    next->envelopeRevision = nextEnvelopeRevisionNumber();

    int insertedIndex = -1;
    for (int i = 0; i < static_cast<int>(next->envelopePoints.size()); ++i)
    {
        if (std::abs(next->envelopePoints[static_cast<std::size_t>(i)].timeSeconds - clampedTime) < 0.0001)
        {
            insertedIndex = i;
            break;
        }
    }

    commitState(std::move(next), true, CanonicalMutation::Kind::envelopeReplace);
    return insertedIndex;
}

void SingleTrackDocument::beginEnvelopeGesture()
{
    const juce::ScopedLock sl(stateLock);
    envelopeGestureActive = true;
    envelopeGestureDidMutate = false;
    envelopeGestureUndoState = currentState;
}

void SingleTrackDocument::endEnvelopeGesture() noexcept
{
    const juce::ScopedLock sl(stateLock);
    envelopeGestureActive = false;
    envelopeGestureDidMutate = false;
    envelopeGestureUndoState.reset();
}

bool SingleTrackDocument::moveEnvelopePoint(int index, double timeSeconds, float gain)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    auto points = base->envelopePoints;
    if (index < 0 || index >= static_cast<int>(points.size()))
        return false;

    auto& point = points[static_cast<std::size_t>(index)];
    const auto maxTime = base->info.durationSeconds;
    point.timeSeconds = juce::jlimit(0.0, maxTime, timeSeconds);
    point.gain = juce::jlimit(0.0f, 4.0f, gain);

    if (index == 0)
        point.timeSeconds = 0.0;
    if (index == static_cast<int>(points.size()) - 1)
        point.timeSeconds = maxTime;

    sanitiseEnvelope(points, maxTime);

    if (points.size() == base->envelopePoints.size()
        && std::equal(points.begin(), points.end(), base->envelopePoints.begin(),
            [] (const EnvelopePoint& left, const EnvelopePoint& right)
            { return left.timeSeconds == right.timeSeconds && left.gain == right.gain; }))
        return false;

    auto next = std::make_shared<State>(*base);
    next->envelopePoints = std::move(points);
    next->envelopeRevision = nextEnvelopeRevisionNumber();

    const auto gestureActive = envelopeGestureActive;
    if (!gestureActive)
    {
        commitState(std::move(next), true, CanonicalMutation::Kind::envelopeReplace);
        return true;
    }

    sanitiseEnvelope(next->envelopePoints, next->info.durationSeconds);

    StatePtr publishedState = next;
    {
        const juce::ScopedLock sl(stateLock);
        const auto before = getMediaIdentity().document;
        bool pushed = false;
        bool canonical = currentState == base;
        if (envelopeGestureActive && !envelopeGestureDidMutate && envelopeGestureUndoState != nullptr)
        {
            canonical = canonical && envelopeGestureUndoState == base;
            pushUndoState(envelopeGestureUndoState);
            redoStack.clear();
            envelopeGestureDidMutate = true;
            pushed = true;
        }

        // Live envelope points change document content even though the gesture
        // owns one Undo boundary. Each published point needs a distinct version.
        next->changeGeneration = nextChangeGenerationNumber();
        currentState = publishedState;
        lastCanonicalMutation.reset();
        if (canonical) lastCanonicalMutation = CanonicalMutation { CanonicalMutation::Kind::envelopeReplace,
            before, getMediaIdentity().document, sourceBindingIdentity, pushed };
    }

    notifyDocumentChanged(publishedState);
    return true;
}

bool SingleTrackDocument::removeEnvelopePoint(int index)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    auto points = base->envelopePoints;
    if (index <= 0 || index >= static_cast<int>(points.size()) - 1)
        return false;

    points.erase(points.begin() + index);
    sanitiseEnvelope(points, base->info.durationSeconds);

    auto next = std::make_shared<State>(*base);
    next->envelopePoints = std::move(points);
    next->envelopeRevision = nextEnvelopeRevisionNumber();
    commitState(std::move(next), true, CanonicalMutation::Kind::envelopeReplace);
    return true;
}

bool SingleTrackDocument::applySelectionGain(const juce::Range<double>& selectionSeconds, float linearGain)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    const auto sampleRange = secondsToSampleRange(*base, selectionSeconds);
    if (sampleRange.isEmpty())
        return false;

    auto next = std::make_shared<State>(*base);
    if (base->audioBuffer != nullptr)
        next->audioBuffer = copyBuffer(*base->audioBuffer);
    else if (!materialiseAudioBufferIfNeeded(*next))
        return false;
    if (next->audioBuffer == nullptr) return false;

    next->envelopePoints = base->envelopePoints;
    next->audioRevision = nextAudioRevisionNumber();
    next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->memoryMappedReader.reset();

    for (int channel = 0; channel < next->audioBuffer->getNumChannels(); ++channel)
    {
        next->audioBuffer->applyGain(channel,
                                     static_cast<int>(sampleRange.getStart()),
                                     static_cast<int>(sampleRange.getLength()),
                                     linearGain);
    }

    commitState(std::move(next), true);
    return true;
}

bool SingleTrackDocument::applyFadeIn(const juce::Range<double>& selectionSeconds)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    const auto sampleRange = secondsToSampleRange(*base, selectionSeconds);
    if (sampleRange.isEmpty())
        return false;

    auto next = std::make_shared<State>(*base);
    if (base->audioBuffer != nullptr)
        next->audioBuffer = copyBuffer(*base->audioBuffer);
    else if (!materialiseAudioBufferIfNeeded(*next))
        return false;
    if (next->audioBuffer == nullptr) return false;

    next->envelopePoints = base->envelopePoints;
    next->audioRevision = nextAudioRevisionNumber();
    next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->memoryMappedReader.reset();

    const auto numSamples = static_cast<int>(sampleRange.getLength());
    for (int channel = 0; channel < next->audioBuffer->getNumChannels(); ++channel)
    {
        auto* dest = next->audioBuffer->getWritePointer(channel, static_cast<int>(sampleRange.getStart()));
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = static_cast<float>(i) / static_cast<float>(juce::jmax(1, numSamples - 1));
            dest[i] *= gain;
        }
    }

    commitState(std::move(next), true);
    return true;
}

bool SingleTrackDocument::applyFadeOut(const juce::Range<double>& selectionSeconds)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    const auto sampleRange = secondsToSampleRange(*base, selectionSeconds);
    if (sampleRange.isEmpty())
        return false;

    auto next = std::make_shared<State>(*base);
    if (base->audioBuffer != nullptr)
        next->audioBuffer = copyBuffer(*base->audioBuffer);
    else if (!materialiseAudioBufferIfNeeded(*next))
        return false;
    if (next->audioBuffer == nullptr) return false;

    next->envelopePoints = base->envelopePoints;
    next->audioRevision = nextAudioRevisionNumber();
    next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->memoryMappedReader.reset();

    const auto numSamples = static_cast<int>(sampleRange.getLength());
    for (int channel = 0; channel < next->audioBuffer->getNumChannels(); ++channel)
    {
        auto* dest = next->audioBuffer->getWritePointer(channel, static_cast<int>(sampleRange.getStart()));
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gain = 1.0f - (static_cast<float>(i) / static_cast<float>(juce::jmax(1, numSamples - 1)));
            dest[i] *= gain;
        }
    }

    commitState(std::move(next), true);
    return true;
}

bool SingleTrackDocument::deleteSelection(const juce::Range<double>& selectionSeconds)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    const auto sampleRange = secondsToSampleRange(*base, selectionSeconds);
    if (sampleRange.isEmpty() || sampleRange.getLength() >= base->info.numSamples)
        return false;

    auto sourceState = std::make_shared<State>(*base);
    if (!materialiseAudioBufferIfNeeded(*sourceState))
        return false;

    const auto remainingSamples = base->info.numSamples - sampleRange.getLength();
    if (remainingSamples <= 0)
        return false;

    auto next = std::make_shared<State>();
    next->info = base->info;
    next->info.numSamples = remainingSamples;
    next->info.durationSeconds = next->info.sampleRate > 0.0 ? static_cast<double>(remainingSamples) / next->info.sampleRate : 0.0;
    transformTimelineMetadataForDeletion(*next, sampleRange);
    next->audioBuffer = allocateMaterialisedBuffer(sourceState->audioBuffer->getNumChannels(), remainingSamples);
    if (next->audioBuffer == nullptr) return false;
    next->audioBuffer->clear();
    next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->envelopePoints = makeDefaultEnvelope(next->info.durationSeconds);
    next->audioRevision = nextAudioRevisionNumber();
    next->envelopeRevision = nextEnvelopeRevisionNumber();

    const auto headSamples = static_cast<int>(sampleRange.getStart());
    const auto tailStart = static_cast<int>(sampleRange.getEnd());
    const auto tailSamples = static_cast<int>(base->info.numSamples - sampleRange.getEnd());

    for (int channel = 0; channel < sourceState->audioBuffer->getNumChannels(); ++channel)
    {
        if (headSamples > 0)
            next->audioBuffer->copyFrom(channel, 0, *sourceState->audioBuffer, channel, 0, headSamples);

        if (tailSamples > 0)
            next->audioBuffer->copyFrom(channel, headSamples, *sourceState->audioBuffer, channel, tailStart, tailSamples);

        const auto fadeSamples = juce::jmin(256, juce::jmin(headSamples, tailSamples));
        if (fadeSamples > 0)
        {
            auto* dest = next->audioBuffer->getWritePointer(channel);
            for (int i = 0; i < fadeSamples; ++i)
            {
                const auto fadeOutGain = 1.0f - (static_cast<float>(i) / static_cast<float>(fadeSamples));
                const auto fadeInGain = static_cast<float>(i) / static_cast<float>(fadeSamples);
                const auto leftIndex = headSamples - fadeSamples + i;
                const auto rightIndex = headSamples + i;
                dest[leftIndex] *= fadeOutGain;
                dest[rightIndex] *= fadeInGain;
            }
        }
    }

    commitState(std::move(next), true);
    return true;
}

bool SingleTrackDocument::applyOfflineEditPipeline(
    const OfflineEditPipeline& pipeline)
{
    auto base = getState();
    if (!base->info.isValid())
        return false;

    const auto sourceSamples = base->info.numSamples;
    const auto deletion = pipeline.deletionRange;
    if (deletion.getStart() < 0
        || deletion.getEnd() > sourceSamples
        || deletion.getLength() >= sourceSamples)
    {
        return false;
    }

    const auto outputSamples = sourceSamples - deletion.getLength();
    if (outputSamples <= 0
        || outputSamples > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    for (const auto& edit : pipeline.amplitudeEdits)
    {
        if (edit.sampleRange.isEmpty()
            || edit.sampleRange.getStart() < 0
            || edit.sampleRange.getEnd() > outputSamples
            || !std::isfinite(edit.linearGain)
            || edit.linearGain < 0.0f)
        {
            return false;
        }
    }

    if (deletion.isEmpty() && pipeline.amplitudeEdits.empty())
        return false;

    auto sourceState = std::make_shared<State>(*base);
    if (!materialiseAudioBufferIfNeeded(*sourceState))
        return false;

    auto next = std::make_shared<State>(*base);
    next->audioBuffer = allocateMaterialisedBuffer(
        sourceState->audioBuffer->getNumChannels(),
        outputSamples);
    if (next->audioBuffer == nullptr) return false;
    next->audioBuffer->clear();
    next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->memoryMappedReader.reset();
    next->audioRevision = nextAudioRevisionNumber();

    const auto headSamples = static_cast<int>(deletion.getStart());
    const auto tailStart = static_cast<int>(deletion.getEnd());
    const auto tailSamples = static_cast<int>(sourceSamples - deletion.getEnd());
    for (int channel = 0; channel < next->audioBuffer->getNumChannels(); ++channel)
    {
        if (headSamples > 0)
        {
            next->audioBuffer->copyFrom(channel, 0, *sourceState->audioBuffer,
                                        channel, 0, headSamples);
        }
        if (tailSamples > 0)
        {
            next->audioBuffer->copyFrom(channel, headSamples,
                                        *sourceState->audioBuffer, channel,
                                        tailStart, tailSamples);
        }

        const auto crossfadeSamples = juce::jmin(
            256, juce::jmin(headSamples, tailSamples));
        if (!deletion.isEmpty() && crossfadeSamples > 0)
        {
            auto* destination = next->audioBuffer->getWritePointer(channel);
            for (int sample = 0; sample < crossfadeSamples; ++sample)
            {
                const auto fadeOut = 1.0f
                                   - static_cast<float>(sample)
                                         / static_cast<float>(crossfadeSamples);
                const auto fadeIn = static_cast<float>(sample)
                                  / static_cast<float>(crossfadeSamples);
                destination[headSamples - crossfadeSamples + sample] *= fadeOut;
                destination[headSamples + sample] *= fadeIn;
            }
        }
    }

    if (!deletion.isEmpty())
    {
        next->info.numSamples = outputSamples;
        next->info.durationSeconds = static_cast<double>(outputSamples)
                                   / next->info.sampleRate;
        transformTimelineMetadataForDeletion(*next, deletion);
        next->envelopePoints = makeDefaultEnvelope(next->info.durationSeconds);
        next->envelopeRevision = nextEnvelopeRevisionNumber();
    }

    for (const auto& edit : pipeline.amplitudeEdits)
    {
        const auto start = static_cast<int>(edit.sampleRange.getStart());
        const auto count = static_cast<int>(edit.sampleRange.getLength());
        for (int channel = 0; channel < next->audioBuffer->getNumChannels(); ++channel)
        {
            if (edit.kind == OfflineAmplitudeEdit::Kind::gain)
            {
                next->audioBuffer->applyGain(channel, start, count,
                                             edit.linearGain);
                continue;
            }

            auto* samples = next->audioBuffer->getWritePointer(channel, start);
            for (int sample = 0; sample < count; ++sample)
            {
                const auto position = static_cast<float>(sample)
                                    / static_cast<float>(juce::jmax(1, count - 1));
                const auto gain = edit.kind == OfflineAmplitudeEdit::Kind::fadeIn
                                ? position
                                : 1.0f - position;
                samples[sample] *= gain;
            }
        }
    }

    commitState(std::move(next), true);
    return true;
}

std::unique_ptr<SingleTrackDocument::EqualLengthAudioMutationTransaction>
SingleTrackDocument::beginEqualLengthAudioMutation(StatePtr expectedState)
{
    if (expectedState == nullptr
        || !expectedState->info.isValid()
        || expectedState->info.numChannels <= 0
        || expectedState->info.numSamples <= 0
        || expectedState->info.numSamples > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    media::IdentitySnapshot expectedIdentity;
    {
        const juce::ScopedLock sl(stateLock);
        if (currentState != expectedState)
            return {};
        expectedIdentity = getMediaIdentity();
    }

    const auto pcm = media::admitMaterialisation(static_cast<uint64_t>(expectedState->info.numChannels),
                                                expectedState->info.numSamples);
    if (!pcm) return {};
    // Keep the established small-buffer transaction. Large results are ordinary
    // immutable reader-backed states, so recovery retains bounded decoder work
    // rather than another whole PCM allocation. Existing admission ceilings stay.
    if (pcm.floatBytes > 256ULL * 1024 * 1024)
    {
        if (!expectedState->hasPlaybackSource()
            || expectedState->getPlayableSampleRange() != juce::Range<int64_t>(0, expectedState->info.numSamples)) return {};
        std::shared_ptr<juce::AudioFormatReader> reader;
        std::shared_ptr<media::SourceFileLease> lease;
        if (!expectedState->audioBuffer || expectedState->audioBuffer->getNumSamples() == 0)
        {
            if (expectedState->audioBackingStore == State::AudioBackingStore::recoveryStream)
                reader = expectedState->streamingReader; // Isolated immutable PCM; never reopen its historical locator.
            else
            {
                auto witness = expectedState->sourceFileWitness;
                if (!witness)
                    if (const auto* independent = dynamic_cast<const media::IndependentAudioBacking*>(expectedState->streamingReader.get()))
                        witness = independent->validatedBacking();
                if (!witness || witness->file != expectedState->readerSourceFile) return {};
                lease = witness->retainedInput();
                if (lease)
                {
                    if (lease->recheck().failed()) return {};
                    reader = createReaderForRetainedSource(witness);
                }
                else
                {
                    std::unique_ptr<media::SourceFileLease> opened;
                    if (media::SourceFileLease::open(witness->file, opened).failed()
                        || !opened->witness()->sameFileVersion(*witness)) return {};
                    lease = std::move(opened);
                    reader = createReaderForFile(witness->file);
                }
                if (!reader || reader->sampleRate != expectedState->info.sampleRate) return {};
            }
        }
        auto staging = RenderedAudioStorage::create(expectedState->info.numChannels,
                                                    expectedState->info.numSamples, expectedState->info.sampleRate);
        if (!staging) return {};
        auto transaction = std::unique_ptr<EqualLengthAudioMutationTransaction>(
            new EqualLengthAudioMutationTransaction(expectedState, std::move(reader), std::move(lease), std::move(staging)));
        if (!transaction->isValid()) return {};
        transaction->expectedIdentity = expectedIdentity.document;
        transaction->expectedSourceIdentity = expectedIdentity.source;
        return transaction;
    }

    auto materialised = std::make_shared<State>(*expectedState);
    if (!materialiseAudioBufferIfNeeded(*materialised))
        return {};

    auto staging = allocateMaterialisedBuffer(
        materialised->audioBuffer->getNumChannels(),
        materialised->audioBuffer->getNumSamples());
    if (staging == nullptr) return {};
    staging->clear();

    auto transaction = std::unique_ptr<EqualLengthAudioMutationTransaction>(
        new EqualLengthAudioMutationTransaction(expectedState,
                                                std::move(materialised->audioBuffer),
                                                std::move(staging)));
    transaction->expectedIdentity = expectedIdentity.document;
    transaction->expectedSourceIdentity = expectedIdentity.source;
    return transaction;
}

bool SingleTrackDocument::commitEqualLengthAudioMutation(
    std::unique_ptr<EqualLengthAudioMutationTransaction> transaction)
{
    if (transaction == nullptr
        || !transaction->isValid()
        || transaction->nextWriteSample != transaction->getNumSamples())
    {
        return false;
    }

    if (transaction->sourceLease && transaction->sourceLease->recheck().failed()) return false;
    StatePtr undoState = transaction->expectedState;
    auto next = std::make_shared<State>(*transaction->expectedState);
    next->matchesSourceFileAudio = false;
    if (transaction->stagingFile)
    {
        auto reader = transaction->stagingFile->createOwnedReader();
        if (!reader) return false;
        next->audioBuffer.reset(); // Reader-backed playback uses a null PCM buffer.
        next->memoryMappedReader.reset();
        next->streamingReader = std::move(reader);
        next->readerSourceFile = transaction->stagingFile->getFile();
        next->sourceFileWitness = transaction->stagingFile->getWitness();
        next->audioBackingStore = State::AudioBackingStore::streamingReader;
    }
    else
    {
        auto materialisedUndo = std::make_shared<State>(*transaction->expectedState);
        materialisedUndo->audioBuffer = transaction->sourceAudio;
        materialisedUndo->memoryMappedReader.reset();
        materialisedUndo->streamingReader.reset();
        materialisedUndo->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
        materialisedUndo->playbackAvailability = PlaybackAvailability::randomAccess();
        undoState = std::move(materialisedUndo);
        next->audioBuffer = std::move(transaction->stagingAudio);
        next->memoryMappedReader.reset();
        next->streamingReader.reset();
        next->audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    }
    next->playbackAvailability = PlaybackAvailability::randomAccess();
    next->audioRevision = nextAudioRevisionNumber();
    next->changeGeneration = nextChangeGenerationNumber();
    sanitiseEnvelope(next->envelopePoints, next->info.durationSeconds);

    StatePtr publishedState = next;
    {
        const juce::ScopedLock sl(stateLock);
        if (currentState != transaction->expectedState
            || !getMediaIdentity().document.sameVersion(transaction->expectedIdentity)
            || !getMediaIdentity().source.sameBinding(transaction->expectedSourceIdentity))
            return false;

        pushUndoState(std::move(undoState));
        currentState = publishedState;
        redoStack.clear();
    }

    notifyDocumentChanged(publishedState);
    return true;
}

bool SingleTrackDocument::canSaveToFile(const juce::File& targetFile) const
{
    if (!canWriteExtension(targetFile.getFileExtension()))
        return false;

    const auto info = getFileInfo();
    return !targetFile.hasFileExtension(".mp3") || !info.isValid() || info.numChannels <= 2;
}

bool SingleTrackDocument::canSaveOverSourceFile() const
{
    if (requiresRecoverySaveAs()) return false;
    const auto info = getFileInfo();
    return info.isValid() && info.file.existsAsFile() && canSaveToFile(info.file);
}

juce::String SingleTrackDocument::getDefaultSaveExtension() const
{
    const auto info = getFileInfo();
    if (!info.isValid())
        return ".wav";

    const auto ext = info.file.getFileExtension().toLowerCase();
    return canWriteExtension(ext) ? ext : ".wav";
}

SingleTrackDocument::SaveSnapshot SingleTrackDocument::createSaveSnapshot(
    const juce::File& targetFile,
    SaveIntent intent,
    SaveEncodingOptions encodingOptions) const
{
    StatePtr state;
    juce::File currentFile;
    SourceFileIdentity sourceIdentity;
    media::DocumentIdentity documentIdentity;
    bool requiresSaveAs = false;
    juce::Uuid sourceBinding { nullptr };
    uint64_t requestSerial = 0;
    media::SourceFileWitness::Ptr sourceVersion;
    {
        const juce::ScopedLock sl(stateLock);
        state = currentState;
        currentFile = documentTargetFile;
        sourceIdentity = documentSourceIdentity;
        documentIdentity = getMediaIdentity().document;
        requiresSaveAs = recoveryRequiresSaveAs;
        sourceBinding = sourceBindingIdentity;
        sourceVersion = documentSourceVersion;
        if (nextSaveRequestSerial == std::numeric_limits<uint64_t>::max()) return {};
        requestSerial = ++nextSaveRequestSerial;
    }
    if (state == nullptr || !state->info.isValid()
        || (state->info.recoveryBacked && (!state->hasPlaybackSource()
            || state->getPlayableSampleRange().getEnd() != state->info.numSamples)))
        return {};
    if (requiresSaveAs && (intent == SaveIntent::checkpoint || targetFile == juce::File())) return {};

    const auto resolvedTarget = targetFile == juce::File() ? currentFile : targetFile;
    auto sourceInfo = state->info;
    if (!requiresSaveAs && currentFile != state->info.file)
    {
        if (auto currentTargetInfo = probeFileInfo(currentFile); currentTargetInfo.isValid())
            sourceInfo = std::move(currentTargetInfo);
    }
    sourceInfo.file = currentFile;
    auto metadataReport = buildSaveMetadataReport(sourceInfo,
                                                  resolvedTarget,
                                                  intent,
                                                  state->hasTimelineEdits,
                                                  state->safeTimelineMetadataKeys,
                                                  state->transformedMetadataKeys);
    return { state,
             resolvedTarget,
             state->changeGeneration,
             intent,
             encodingOptions,
             std::move(metadataReport),
             std::move(sourceIdentity),
             documentIdentity,
             requiresSaveAs,
             sourceBinding,
             requestSerial,
             std::move(sourceVersion) };
}

SingleTrackDocument::SavePublication::SavePublication(const SaveSnapshot& saved, media::SourceFileWitness::Ptr target,
                                                      media::SourceFileWitness::Ptr replaced)
    : state(saved.state), file(std::move(target)), replacedSource(std::move(replaced)), requestedSource(saved.sourceVersion),
      identity(saved.documentIdentity), sourceBinding(saved.sourceBinding),
      intent(saved.intent), requestSerial(saved.requestSerial) {}

bool SingleTrackDocument::SavePublication::matches(const SaveSnapshot& snapshot) const
{
    const auto held = state.lock();
    return held && held == snapshot.state && !held.owner_before(snapshot.state) && !snapshot.state.owner_before(held)
        && file && file->file == snapshot.targetFile && identity.sameVersion(snapshot.documentIdentity)
        && snapshot.changeGeneration == identity.generation && sourceBinding == snapshot.sourceBinding
        && intent == snapshot.intent && requestSerial != 0 && requestSerial == snapshot.requestSerial
        && requestedSource.lock() == snapshot.sourceVersion;
}

SingleTrackDocument::SaveWriteResult SingleTrackDocument::writeSaveSnapshot(const SaveSnapshot& snapshot)
{
    SaveWriteResult result;
    result.targetFile = snapshot.targetFile;
    result.changeGeneration = snapshot.changeGeneration;
    result.metadataDescription = snapshot.metadataReport.description;
    const auto targetExtension = snapshot.targetFile.getFileExtension().toLowerCase();
    if (targetExtension == ".mp3")
        result.encodingDescription = "MP3 " + juce::String(snapshot.encodingOptions.mp3BitrateKbps) + " kbps";
    else if (targetExtension == ".ogg")
        result.encodingDescription = "Ogg Vorbis q" + juce::String(snapshot.encodingOptions.oggQualityIndex);
    else if (targetExtension == ".flac")
        result.encodingDescription = "FLAC level " + juce::String(snapshot.encodingOptions.flacCompressionLevel);

    if (!snapshot.isValid())
    {
        result.failureReason = SaveFailureReason::invalidSnapshot;
        result.errorMessage = "The save snapshot is invalid.";
        return result;
    }

    const auto replacesLoadedSource = snapshot.intent == SaveIntent::checkpoint
                                   && snapshot.sourceIdentity.valid
                                   && snapshot.targetFile == snapshot.sourceIdentity.file;
    if (replacesLoadedSource && !snapshot.sourceIdentity.matchesCurrentFile())
    {
        result.failureReason = SaveFailureReason::staleSource;
        result.errorMessage = "The source file changed outside Single Editor.";
        return result;
    }

    // Capture a stable physical input before rendering. The current save target
    // version is separate from the original audio backing retained by Undo.
    std::shared_ptr<media::SourceFileLease> sourceLease;
    media::SourceFileWitness::Ptr originalBacking, replacedVersion;
#if JUCE_WINDOWS
    if (snapshot.state->sourceFileWitness && snapshot.state->sourceFileWitness->file == snapshot.targetFile
        && !snapshot.state->sourceFileWitness->retainedInput()) originalBacking = snapshot.state->sourceFileWitness;
    const auto expectedSource = replacesLoadedSource && snapshot.sourceVersion ? snapshot.sourceVersion : originalBacking;
    if (expectedSource)
    {
        const auto opened = media::SourceFileLease::openReplacementSource(*expectedSource, sourceLease);
        if (opened.failed())
        {
            result.failureReason = SaveFailureReason::staleSource;
            result.errorMessage = "The source cannot be verified for saving: " + opened.getErrorMessage();
            return result;
        }
        replacedVersion = sourceLease->witness();
        if (originalBacking && !originalBacking->sameFileVersion(*replacedVersion))
        {
            result.failureReason = SaveFailureReason::staleSource;
            result.errorMessage = "The original audio backing changed before saving.";
            return result;
        }
    }
#endif

    if (!snapshot.metadataReport.canProceed())
    {
        result.failureReason = SaveFailureReason::metadataRejected;
        result.errorMessage = snapshot.metadataReport.description;
        return result;
    }

    if (!canWriteExtension(snapshot.targetFile.getFileExtension()))
    {
        result.failureReason = SaveFailureReason::unsupportedFormat;
        result.errorMessage = "The target format is not writable.";
        return result;
    }

    if (snapshot.targetFile.hasFileExtension(".mp3") && snapshot.state->info.numChannels > 2)
    {
        result.failureReason = SaveFailureReason::unsupportedFormat;
        result.errorMessage = "MP3 output supports mono or stereo only; no implicit downmix was performed.";
        return result;
    }

    const auto targetFile = snapshot.targetFile;
    const auto parentDirectory = targetFile.getParentDirectory();
    files::nativeFile(parentDirectory).createDirectory();

    const auto temporaryFile = parentDirectory.getChildFile("." + targetFile.getFileNameWithoutExtension()
                                                           + ".singleeditor-save-"
                                                           + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())
                                                           + targetFile.getFileExtension());
    files::nativeFile(temporaryFile).deleteFile();

    if (!writeStateAudioToFile(*snapshot.state,
                               files::nativeFile(temporaryFile),
                               snapshot.encodingOptions,
                               snapshot.metadataReport.omittedKeys))
    {
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = SaveFailureReason::writeFailed;
        result.errorMessage = "The rendered audio could not be written.";
        return result;
    }

    const auto renderedInfo = probeFileInfo(files::nativeFile(temporaryFile));
    const auto renderedExtension = temporaryFile.getFileExtension().toLowerCase();
    const auto isLossyTarget = renderedExtension == ".mp3" || renderedExtension == ".ogg";
    const auto sampleTolerance = isLossyTarget
        ? juce::jmax<int64_t>(4096, static_cast<int64_t>(std::ceil(snapshot.state->info.sampleRate * 0.1)))
        : 0;
    if (!renderedInfo.isValid()
        || std::abs(renderedInfo.numSamples - snapshot.state->info.numSamples) > sampleTolerance
        || renderedInfo.numChannels != snapshot.state->info.numChannels
        || std::abs(renderedInfo.sampleRate - snapshot.state->info.sampleRate) > 0.001)
    {
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = SaveFailureReason::validationFailed;
        result.errorMessage = "The rendered audio failed validation.";
        return result;
    }

    if ((replacesLoadedSource && !snapshot.sourceIdentity.matchesCurrentFile()) || (sourceLease && sourceLease->recheck().failed()))
    {
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = SaveFailureReason::staleSource;
        result.errorMessage = "The source file changed outside Single Editor while saving.";
        return result;
    }

#if JUCE_WINDOWS
    std::unique_ptr<media::SourceFileLease> published;
    auto publication = media::SourceFileLease::openStagedWrite(temporaryFile, published);
    if (publication.failed())
    {
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = SaveFailureReason::durabilityFailed;
        result.errorMessage = "The rendered file could not be flushed before publication: " + publication.getErrorMessage();
        return result;
    }
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (sourceLease && sourceLease->recheck().failed())
        {
            publication = juce::Result::fail("save_source_changed_before_publication");
            break;
        }
        publication = published->publishTo(targetFile);
        if (publication.wasOk() || !publication.getErrorMessage().startsWith("save_publication_replace:")) break;
        if (attempt < 3) juce::Thread::sleep(25);
    }
    if (publication.failed())
    {
        published.reset();
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = publication.getErrorMessage().startsWith("save_publication_replace:")
            ? SaveFailureReason::replaceFailed : SaveFailureReason::durabilityFailed;
        result.errorMessage = "The save could not be verified. The target may already contain the rendered audio; "
                              "the document remains unsaved: " + publication.getErrorMessage();
        return result;
    }
    publication = published->recheck();
    if (publication.failed())
    {
        result.failureReason = SaveFailureReason::durabilityFailed;
        result.errorMessage = "The published save changed before confirmation: " + publication.getErrorMessage();
        return result;
    }
    if (originalBacking)
    {
        const auto preserved = media::SourceFileLease::preserveReplacedInput(sourceLease, originalBacking, published->witness());
        if (preserved.failed())
        {
            result.failureReason = SaveFailureReason::durabilityFailed;
            result.errorMessage = "The file was saved but its previous input could not be retained for Undo: " + preserved.getErrorMessage();
            return result;
        }
    }
    // Reuse the actual staged-output validation, including lossy frame counts
    // and selected output depth. Later source epochs must not infer these facts
    // from the in-memory edit/history that predates the Save publication.
    auto publishedFacts = std::make_shared<media::SourceFileWitness>(*published->witness());
    publishedFacts->readerFormat = renderedInfo.formatName; publishedFacts->sampleRate = renderedInfo.sampleRate;
    publishedFacts->channels = renderedInfo.numChannels; publishedFacts->bitsPerSample = renderedInfo.bitsPerSample;
    publishedFacts->frames = renderedInfo.numSamples;
    result.publication = std::shared_ptr<const SavePublication>(new SavePublication(snapshot, std::move(publishedFacts), replacedVersion));
#else
    // Preserve the existing non-Windows save path. It does not issue the
    // Windows physical-file publication proof required by M3 recovery cleanup.
    auto replaced = false;
    for (int attempt = 0; attempt < 4 && !replaced; ++attempt)
    {
        replaced = temporaryFile.replaceFileIn(targetFile);
        if (!replaced)
            juce::Thread::sleep(25);
    }

    if (!replaced)
    {
        files::nativeFile(temporaryFile).deleteFile();
        result.failureReason = SaveFailureReason::replaceFailed;
        result.errorMessage = "The source file could not be replaced.";
        return result;
    }
#endif

    result.succeeded = true;
    result.failureReason = SaveFailureReason::none;
    return result;
}

bool SingleTrackDocument::markSaveSnapshotCommitted(const SaveSnapshot& snapshot, const SaveWriteResult& result)
{
    if (!snapshot.isValid() || !result.succeeded || result.targetFile != snapshot.targetFile
        || result.changeGeneration != snapshot.changeGeneration)
        return false;

    if (snapshot.intent == SaveIntent::exportCopy)
        return false;

#if JUCE_WINDOWS
    if (!result.publication || !result.publication->matches(snapshot)) return false;
    std::unique_ptr<media::SourceFileLease> savedFile;
    if (media::SourceFileLease::open(snapshot.targetFile, savedFile).failed()
        || !savedFile->witness()->sameFileVersion(*result.publication->targetVersion())) return false;
#endif

    const auto committedIdentity = SourceFileIdentity::capture(snapshot.targetFile);
    {
        const juce::ScopedLock sl(stateLock);
        // A write may finish after another file/open/clear, even with identical
        // names, paths or per-instance generation numbers. Never rebind it.
        if (!getMediaIdentity().document.sameDocument(snapshot.documentIdentity)
            || snapshot.changeGeneration != snapshot.documentIdentity.generation
            || snapshot.requestSerial == 0 || snapshot.requestSerial <= appliedSaveRequestSerial)
            return false;
        if (snapshot.intent == SaveIntent::checkpoint && documentTargetFile != snapshot.targetFile)
            return false;
        if (snapshot.intent == SaveIntent::saveAs)
        {
            documentTargetFile = snapshot.targetFile;
            sourceBindingIdentity = juce::Uuid();
            recoveryRequiresSaveAs = false;
        }
        documentSourceIdentity = committedIdentity;
#if JUCE_WINDOWS
        documentSourceVersion = result.publication->targetVersion();
#else
        documentSourceVersion.reset();
#endif
        cleanChangeGeneration = snapshot.changeGeneration;
        appliedSaveRequestSerial = snapshot.requestSerial;
    }
#if JUCE_WINDOWS
    savedFile.reset(); // Notification consumers may start the next save/capture.
#endif
    notifyIdentityChanged(snapshot.intent == SaveIntent::saveAs
        ? media::IdentityEventKind::sourceRebound : media::IdentityEventKind::saveCheckpoint);
    return true;
}

bool SingleTrackDocument::saveAsToFile(const juce::File& targetFile)
{
    const auto snapshot = createSaveSnapshot(targetFile, SaveIntent::saveAs);
    const auto result = writeSaveSnapshot(snapshot);
    if (!result.succeeded)
        return false;

    return markSaveSnapshotCommitted(snapshot, result);
}

bool SingleTrackDocument::saveToFile(const juce::File& targetFile)
{
    const auto state = getState();
    if (!state->info.isValid())
        return false;

    auto outputFile = targetFile;
    if (outputFile == juce::File())
        return false;

    if (outputFile == getFileInfo().file)
        return saveOverSourceFile();

    const auto snapshot = createSaveSnapshot(outputFile, SaveIntent::exportCopy);
    const auto result = writeSaveSnapshot(snapshot);
    if (!result.succeeded)
        return false;
    return true;
}

bool SingleTrackDocument::saveOverSourceFile()
{
    const auto state = getState();
    if (!state->info.isValid() || !canSaveOverSourceFile())
        return false;

    const auto snapshot = createSaveSnapshot();
    const auto result = writeSaveSnapshot(snapshot);
    if (!result.succeeded)
        return false;

    return markSaveSnapshotCommitted(snapshot, result);
}

bool SingleTrackDocument::canUndo() const
{
    const juce::ScopedLock sl(stateLock);
    return !undoStack.empty() && (!undoStack.back()->info.recoveryBacked
        || (undoStack.back()->hasPlaybackSource() && !undoStack.back()->getPlayableSampleRange().isEmpty()));
}

bool SingleTrackDocument::canRedo() const
{
    const juce::ScopedLock sl(stateLock);
    return !redoStack.empty() && (!redoStack.back()->info.recoveryBacked
        || (redoStack.back()->hasPlaybackSource() && !redoStack.back()->getPlayableSampleRange().isEmpty()));
}

bool SingleTrackDocument::undo()
{
    StatePtr nextState;
    {
        const juce::ScopedLock sl(stateLock);
        if (undoStack.empty() || (undoStack.back()->info.recoveryBacked
            && (!undoStack.back()->hasPlaybackSource() || undoStack.back()->getPlayableSampleRange().isEmpty())))
            return false;

        const auto before = getMediaIdentity().document;
        redoStack.push_back(currentState);
        nextState = undoStack.back();
        undoStack.pop_back();
        currentState = nextState;
        lastCanonicalMutation = CanonicalMutation { CanonicalMutation::Kind::undo,
            before, getMediaIdentity().document, sourceBindingIdentity, false };
    }

    notifyDocumentChanged(nextState);
    return true;
}

bool SingleTrackDocument::redo()
{
    StatePtr nextState;
    {
        const juce::ScopedLock sl(stateLock);
        if (redoStack.empty() || (redoStack.back()->info.recoveryBacked
            && (!redoStack.back()->hasPlaybackSource() || redoStack.back()->getPlayableSampleRange().isEmpty())))
            return false;

        const auto before = getMediaIdentity().document;
        undoStack.push_back(currentState);
        nextState = redoStack.back();
        redoStack.pop_back();
        currentState = nextState;
        lastCanonicalMutation = CanonicalMutation { CanonicalMutation::Kind::redo,
            before, getMediaIdentity().document, sourceBindingIdentity, false };
    }

    notifyDocumentChanged(nextState);
    return true;
}

void SingleTrackDocument::addListener(Listener* listener)
{
    listeners.add(listener);
}

void SingleTrackDocument::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

std::vector<SingleTrackDocument::EnvelopePoint> SingleTrackDocument::makeDefaultEnvelope(double durationSeconds)
{
    return {{0.0, 1.0f}, {juce::jmax(0.0, durationSeconds), 1.0f}};
}

bool SingleTrackDocument::isEnvelopeFlatUnity(const std::vector<EnvelopePoint>& points) noexcept
{
    return points.size() == 2
        && std::abs(points.front().timeSeconds) < 0.0001
        && std::abs(points.front().gain - 1.0f) < 0.0001f
        && std::abs(points.back().gain - 1.0f) < 0.0001f;
}

void SingleTrackDocument::sanitiseEnvelope(std::vector<EnvelopePoint>& points, double durationSeconds)
{
    if (points.empty())
    {
        points = makeDefaultEnvelope(durationSeconds);
        return;
    }

    for (auto& point : points)
    {
        point.timeSeconds = juce::jlimit(0.0, durationSeconds, point.timeSeconds);
        point.gain = juce::jlimit(0.0f, 4.0f, point.gain);
    }

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b)
    {
        return a.timeSeconds < b.timeSeconds;
    });

    std::vector<EnvelopePoint> deduped;
    deduped.reserve(points.size());
    for (const auto& point : points)
    {
        if (!deduped.empty() && std::abs(deduped.back().timeSeconds - point.timeSeconds) < 0.0001)
            deduped.back() = point;
        else
            deduped.push_back(point);
    }

    if (deduped.front().timeSeconds > 0.0)
        deduped.insert(deduped.begin(), {0.0, deduped.front().gain});
    else
        deduped.front().timeSeconds = 0.0;

    if (deduped.back().timeSeconds < durationSeconds)
        deduped.push_back({durationSeconds, deduped.back().gain});
    else
        deduped.back().timeSeconds = durationSeconds;

    if (deduped.size() == 1)
        deduped.push_back({durationSeconds, deduped.front().gain});

    points = std::move(deduped);
}

juce::Range<int64_t> SingleTrackDocument::secondsToSampleRange(const State& state, const juce::Range<double>& seconds) noexcept
{
    if (!state.info.isValid() || state.info.sampleRate <= 0.0)
        return {};

    const auto startSeconds = juce::jlimit(0.0, state.info.durationSeconds, seconds.getStart());
    const auto endSeconds = juce::jlimit(0.0, state.info.durationSeconds, seconds.getEnd());
    const auto startSample = juce::jlimit<int64_t>(0, state.info.numSamples, static_cast<int64_t>(std::floor(startSeconds * state.info.sampleRate)));
    const auto endSample = juce::jlimit<int64_t>(0, state.info.numSamples, static_cast<int64_t>(std::ceil(endSeconds * state.info.sampleRate)));
    return {juce::jmin(startSample, endSample), juce::jmax(startSample, endSample)};
}

std::shared_ptr<juce::AudioBuffer<float>> SingleTrackDocument::copyBuffer(const juce::AudioBuffer<float>& source)
{
    auto copy = allocateMaterialisedBuffer(source.getNumChannels(), source.getNumSamples());
    if (copy == nullptr) return {};
    copy->makeCopyOf(source);
    return copy;
}

bool SingleTrackDocument::materialiseAudioBufferIfNeeded(State& state)
{
    // Recovery never allocates a full buffer or reopens its historical locator
    // to fill unprepared ranges. The explicit restore owner supplies coverage.
    if (state.info.recoveryBacked && (!state.hasPlaybackSource()
        || state.getPlayableSampleRange().getEnd() != state.info.numSamples)) return false;
    if (!media::admitReader(state.info.sampleRate, static_cast<uint64_t>(state.info.numChannels), state.info.numSamples)
        || !media::admitMaterialisation(static_cast<uint64_t>(state.info.numChannels), state.info.numSamples))
        return false;
    if (state.audioBuffer != nullptr && state.audioBuffer->getNumSamples() > 0)
        return true;

    if (!state.info.isValid() || state.info.numChannels <= 0 || state.info.numSamples <= 0)
        return false;

    auto loadedBuffer = allocateMaterialisedBuffer(state.info.numChannels, state.info.numSamples);
    if (loadedBuffer == nullptr) return false;
    loadedBuffer->clear();

    auto readSuccess = false;
    if (state.memoryMappedReader != nullptr)
    {
        if (state.memoryMappedReader->getMappedSection() != juce::Range<int64_t>(0, state.info.numSamples)
            && !state.memoryMappedReader->mapEntireFile())
        {
            return false;
        }

        readSuccess = state.memoryMappedReader->read(loadedBuffer.get(),
                                                     0,
                                                     static_cast<int>(state.info.numSamples),
                                                     0,
                                                     true,
                                                     state.info.numChannels > 1);
    }
    else if (state.streamingReader != nullptr)
    {
        readSuccess = state.streamingReader->read(loadedBuffer.get(),
                                                  0,
                                                  static_cast<int>(state.info.numSamples),
                                                  0,
                                                  true,
                                                  state.info.numChannels > 1);
    }
    else if (auto reader = createReaderForFile(state.info.file))
    {
        readSuccess = reader->read(loadedBuffer.get(),
                                   0,
                                   static_cast<int>(state.info.numSamples),
                                   0,
                                   true,
                                   state.info.numChannels > 1);
    }

    if (!readSuccess)
        return false;

    state.audioBuffer = std::move(loadedBuffer);
    state.memoryMappedReader.reset();
    state.streamingReader.reset();
    state.audioBackingStore = State::AudioBackingStore::inMemoryBuffer;
    state.playbackAvailability = PlaybackAvailability::randomAccess();
    return true;
}

bool SingleTrackDocument::writeStateAudioToFile(const State& state,
                                                const juce::File& targetFile,
                                                const SaveEncodingOptions& encodingOptions,
                                                const juce::StringArray& omittedMetadataKeys)
{
    if (state.info.recoveryBacked && (!state.hasPlaybackSource()
        || state.getPlayableSampleRange().getEnd() != state.info.numSamples)) return false;
    if (!state.info.isValid() || state.info.numSamples <= 0 || state.info.numChannels <= 0)
        return false;

    if (targetFile.hasFileExtension(".mp3"))
    {
        if (state.info.numChannels > 2)
            return false;

        const auto stagingWave = targetFile.getSiblingFile(
            "." + targetFile.getFileNameWithoutExtension() + "-pcm-" + juce::Uuid().toString() + ".wav");
        stagingWave.deleteFile();
        const auto rendered = writeStateAudioToFile(state,
                                                    stagingWave,
                                                    encodingOptions,
                                                    omittedMetadataKeys);
        if (!rendered)
        {
            stagingWave.deleteFile();
            return false;
        }

        juce::String errorMessage;
        const auto encoded = WindowsMediaAudioService::encodeWaveFileToMp3(stagingWave,
                                                                           targetFile,
                                                                           encodingOptions.mp3BitrateKbps,
                                                                           errorMessage);
        stagingWave.deleteFile();
        return encoded;
    }

    auto writerFormat = createWriterFormatForExtension(targetFile.getFileExtension());
    if (writerFormat == nullptr)
        return false;

    targetFile.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream(targetFile.createOutputStream());
    if (stream == nullptr)
        return false;

    const auto bits = state.info.bitsPerSample > 0 ? state.info.bitsPerSample : 24;
    std::unordered_map<juce::String, juce::String> metadataValues;
    const auto metadataKeys = state.info.metadata.getAllKeys();
    for (const auto& key : metadataKeys)
    {
        if (!key.equalsIgnoreCase("MetaDataSource")
            && !omittedMetadataKeys.contains(key))
        {
            metadataValues[key] = state.info.metadata[key];
        }
    }

    auto writerOptions = juce::AudioFormatWriterOptions()
                           .withSampleRate(state.info.sampleRate)
                           .withNumChannels(state.info.numChannels)
                           .withBitsPerSample(bits)
                           .withMetadataValues(metadataValues);

    const auto targetExtension = targetFile.getFileExtension().toLowerCase();
    if (targetExtension == ".ogg")
        writerOptions = writerOptions.withQualityOptionIndex(
            juce::jlimit(0, 10, encodingOptions.oggQualityIndex));
    else if (targetExtension == ".flac")
        writerOptions = writerOptions.withQualityOptionIndex(
            juce::jlimit(0, 8, encodingOptions.flacCompressionLevel));

    if (shouldPreserveWaveChannelMaskOnWrite(targetFile))
    {
        const auto waveChannelMask = parseWaveChannelMask(state.info.metadata);
        const auto writerLayout = makeWriterChannelLayoutForWaveMask(state.info.numChannels, waveChannelMask);
        if (writerLayout.size() == state.info.numChannels)
            writerOptions = writerOptions.withChannelLayout(writerLayout);
    }

    std::unique_ptr<juce::AudioFormatWriter> writer(writerFormat->createWriterFor(stream, writerOptions));
    if (writer == nullptr)
        return false;

    constexpr int saveChunkSamples = 65536;
    const auto envelopeIsUnity = isEnvelopeFlatUnity(state.envelopePoints);

    if (state.audioBuffer != nullptr && state.audioBuffer->getNumSamples() > 0 && envelopeIsUnity)
    {
        for (int offset = 0; offset < state.audioBuffer->getNumSamples(); offset += saveChunkSamples)
        {
            const auto count = juce::jmin(saveChunkSamples, state.audioBuffer->getNumSamples() - offset);
            if (!writer->writeFromAudioSampleBuffer(*state.audioBuffer, offset, count))
                return false;
        }

        return true;
    }

    std::unique_ptr<juce::AudioFormatReader> ownedReader;
    juce::AudioFormatReader* reader = nullptr;
    if (state.audioBuffer == nullptr || state.audioBuffer->getNumSamples() <= 0)
    {
        if (state.info.recoveryBacked || state.audioBackingStore == State::AudioBackingStore::recoveryStream)
        {
            // Restored audio belongs to the immutable recovery reader. Its
            // historical locator is descriptive and must never supply Save PCM.
            reader = state.streamingReader.get();
        }
        else if (state.memoryMappedReader != nullptr
            && state.memoryMappedReader->getMappedSection().contains(juce::Range<int64_t>(0, state.info.numSamples)))
        {
            reader = state.memoryMappedReader.get();
        }
        else
        {
            const auto input = state.readerSourceFile != juce::File() ? state.readerSourceFile : state.info.file;
            ownedReader = state.sourceFileWitness && state.sourceFileWitness->retainedInput()
                ? createReaderForRetainedSource(state.sourceFileWitness) : createReaderForFile(input);
            if (ownedReader != nullptr
                && ownedReader->lengthInSamples == state.info.numSamples
                && static_cast<int>(ownedReader->numChannels) == state.info.numChannels
                && std::abs(ownedReader->sampleRate - state.info.sampleRate) < 0.001)
            {
                reader = ownedReader.get();
            }
            else if (state.readerSourceFile != juce::File() && state.readerSourceFile != state.info.file)
            {
                // Decoded backing loss is an explicit failure, not permission
                // to reopen/re-decode its original optional media or borrow the
                // concurrently used progressive playback reader.
                return false;
            }
            else if (state.memoryMappedReader != nullptr)
            {
                reader = state.memoryMappedReader.get();
            }
            else
            {
                reader = state.streamingReader.get();
            }
        }

        if (reader == nullptr)
            return false;
    }

    juce::AudioBuffer<float> scratch(state.info.numChannels,
                                      static_cast<int>(juce::jmin<int64_t>(saveChunkSamples, state.info.numSamples)));
    for (int64_t offset = 0; offset < state.info.numSamples; offset += saveChunkSamples)
    {
        const auto count = static_cast<int>(juce::jmin<int64_t>(saveChunkSamples, state.info.numSamples - offset));
        scratch.setSize(state.info.numChannels, count, false, false, true);

        if (state.audioBuffer != nullptr && state.audioBuffer->getNumSamples() > 0)
        {
            for (int channel = 0; channel < state.info.numChannels; ++channel)
                scratch.copyFrom(channel, 0, *state.audioBuffer, channel, static_cast<int>(offset), count);
        }
        else
        {
            scratch.clear();
            if (!reader->read(&scratch, 0, count, offset, true, state.info.numChannels > 1))
                return false;
        }

        if (!envelopeIsUnity && state.info.sampleRate > 0.0)
        {
            for (int sample = 0; sample < count; ++sample)
            {
                const auto gain = evaluateEnvelope(state.envelopePoints,
                                                   static_cast<double>(offset + sample) / state.info.sampleRate);
                for (int channel = 0; channel < state.info.numChannels; ++channel)
                    scratch.setSample(channel, sample, scratch.getSample(channel, sample) * gain);
            }
        }

        if (!writer->writeFromAudioSampleBuffer(scratch, 0, count))
            return false;
    }

    return true;
}

SingleTrackDocument::StatePtr SingleTrackDocument::makeEmptyState()
{
    auto state = std::make_shared<State>();
    state->audioBackingStore = State::AudioBackingStore::empty;
    return state;
}

float SingleTrackDocument::evaluateEnvelope(const std::vector<EnvelopePoint>& points, double timeSeconds) noexcept
{
    if (points.empty())
        return 1.0f;

    if (timeSeconds <= points.front().timeSeconds)
        return points.front().gain;

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        const auto& left = points[i - 1];
        const auto& right = points[i];
        if (timeSeconds <= right.timeSeconds)
        {
            const auto span = juce::jmax(0.000001, right.timeSeconds - left.timeSeconds);
            const auto alpha = static_cast<float>((timeSeconds - left.timeSeconds) / span);
            return left.gain + (right.gain - left.gain) * alpha;
        }
    }

    return points.back().gain;
}

void SingleTrackDocument::registerFormats(juce::AudioFormatManager& manager)
{
    registerReadFormats(manager);
}

bool SingleTrackDocument::canWriteExtension(const juce::String& extension) noexcept
{
    const auto ext = extension.toLowerCase();
    return ext == ".wav" || ext == ".flac" || ext == ".aiff" || ext == ".aif"
        || ext == ".ogg" || ext == ".mp3";
}

std::unique_ptr<juce::AudioFormat> SingleTrackDocument::createWriterFormatForExtension(const juce::String& extension)
{
    const auto ext = extension.toLowerCase();

    if (ext == ".wav")
        return std::make_unique<juce::WavAudioFormat>();

    if (ext == ".flac")
        return std::make_unique<juce::FlacAudioFormat>();

    if (ext == ".aiff" || ext == ".aif")
        return std::make_unique<juce::AiffAudioFormat>();

    if (ext == ".ogg")
        return std::make_unique<juce::OggVorbisAudioFormat>();

    return {};
}

void SingleTrackDocument::pushUndoState(StatePtr state)
{
    undoStack.push_back(std::move(state));
    if (undoStack.size() > maxUndoSteps)
        undoStack.erase(undoStack.begin());
}

void SingleTrackDocument::markCurrentStateClean(StatePtr savedState)
{
    StatePtr publishedState;
    {
        const juce::ScopedLock sl(stateLock);
        if (currentState == nullptr)
            return;

        if (savedState != nullptr && currentState != savedState)
            return;

        cleanChangeGeneration = currentState->changeGeneration;
        publishedState = currentState;
    }

    notifyDocumentChanged(publishedState);
}

void SingleTrackDocument::commitState(std::shared_ptr<State> newState, bool pushUndo,
                                     std::optional<CanonicalMutation::Kind> mutationKind)
{
    sanitiseEnvelope(newState->envelopePoints, newState->info.durationSeconds);
    if (pushUndo)
        newState->changeGeneration = nextChangeGenerationNumber();
    else if (newState->changeGeneration == 0)
        newState->changeGeneration = cleanChangeGeneration;

    StatePtr publishedState = newState;
    {
        const juce::ScopedLock sl(stateLock);
        const auto before = mutationKind ? getMediaIdentity().document : media::DocumentIdentity {};
        if (pushUndo && currentState != nullptr)
            pushUndoState(currentState);

        currentState = publishedState;
        if (pushUndo)
            redoStack.clear();
        lastCanonicalMutation.reset();
        if (mutationKind && before.isValid()) lastCanonicalMutation = CanonicalMutation {
            *mutationKind, before, getMediaIdentity().document, sourceBindingIdentity, pushUndo };
    }

    notifyDocumentChanged(publishedState);
}

void SingleTrackDocument::notifyDocumentChanged(StatePtr state)
{
    notifyIdentityChanged();
    listeners.call([&state](Listener& listener) { listener.documentChanged(state); });
}

void SingleTrackDocument::notifyIdentityChanged(std::optional<media::IdentityEventKind> forcedKind)
{
    media::IdentityEvent event;
    {
        const juce::ScopedLock sl(stateLock);
        const auto next = getMediaIdentity();
        if (!forcedKind.has_value()
            && next.document.lineage == lastNotifiedIdentity.document.lineage
            && next.document.generation == lastNotifiedIdentity.document.generation)
            return;
        event.before = lastNotifiedIdentity;
        event.after = next;
        event.sequence = ++identityEventSequence;
        event.kind = forcedKind.value_or(!next.document.isValid() ? media::IdentityEventKind::cleared
            : !next.document.sameDocument(event.before.document) ? media::IdentityEventKind::opened
                                                               : media::IdentityEventKind::generationChanged);
        lastNotifiedIdentity = next;
    }
    listeners.call([&event] (Listener& listener) { listener.documentIdentityChanged(event); });
}

int64_t SingleTrackDocument::nextAudioRevisionNumber() noexcept
{
    return nextAudioRevision.fetch_add(1, std::memory_order_relaxed);
}

int64_t SingleTrackDocument::nextEnvelopeRevisionNumber() noexcept
{
    return nextEnvelopeRevision.fetch_add(1, std::memory_order_relaxed);
}

int64_t SingleTrackDocument::nextChangeGenerationNumber() noexcept
{
    return nextChangeGeneration.fetch_add(1, std::memory_order_relaxed);
}
} // namespace singleeditor
