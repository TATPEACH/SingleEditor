#include "Metadata/TypedMetadataInspectorModel.h"

#include <algorithm>

namespace singleeditor
{
namespace
{
juce::String normaliseKey(const juce::String& key)
{
    auto result = key.trim().toLowerCase();
    result = result.replaceCharacters(" -./\\", "_____");
    while (result.contains("__"))
        result = result.replace("__", "_");
    return result.trimCharactersAtStart("_").trimCharactersAtEnd("_");
}

juce::String friendlyLabel(const juce::String& rawKey)
{
    const auto key = normaliseKey(rawKey);
    if (key == "title") return "Title";
    if (key == "artist") return "Artist";
    if (key == "album") return "Album";
    if (key == "genre") return "Genre";
    if (key == "comment" || key == "comments") return "Comment";
    if (key == "track" || key == "tracknumber" || key == "track_number") return "Track";
    if (key == "year" || key == "date") return "Date";
    return rawKey.trim().isNotEmpty() ? rawKey.trim() : juce::String("Unknown key");
}

TypedMetadataField makeField(juce::String id,
                             juce::String label,
                             MetadataValueType type,
                             juce::String normalised,
                             juce::String raw,
                             juce::String source,
                             juce::String provenance,
                             MetadataConfidence confidence,
                             MetadataConflictState conflict,
                             MetadataWriteCapability writeCapability,
                             bool optional)
{
    TypedMetadataField field;
    field.id = std::move(id);
    field.label = std::move(label);
    field.type = type;
    field.normalisedValue = normalised.isNotEmpty() ? std::move(normalised)
                                                    : juce::String("Unknown (empty value)");
    field.rawValues.add(raw);
    field.source = std::move(source);
    field.provenance = std::move(provenance);
    field.confidence = confidence;
    field.conflict = conflict;
    field.writeCapability = writeCapability;
    field.optional = optional;
    return field;
}

juce::String joinRawValues(const juce::StringArray& values)
{
    juce::StringArray escaped;
    for (const auto& value : values)
        escaped.add(value.isNotEmpty() ? value : "<empty>");
    return escaped.joinIntoString(" | ");
}
}

class TypedMetadataInspectorModel::ProjectionProvider final
{
public:
    juce::String rawText(const TypedMetadataField& field) const
    {
        return joinRawValues(field.rawValues);
    }

    juce::String provenanceText(const TypedMetadataField& field) const
    {
        return field.provenance.isNotEmpty() ? field.provenance : field.source;
    }
};

TypedMetadataInspectorModel::~TypedMetadataInspectorModel() = default;

std::unique_ptr<TypedMetadataInspectorModel> TypedMetadataInspectorModel::createPostCore(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (generation == 0 || !info.isValid())
        return {};

    auto model = std::unique_ptr<TypedMetadataInspectorModel>(new TypedMetadataInspectorModel());
    model->snapshot.generation = generation;
    model->snapshot.active = true;
    model->rebuild(info);
    return model;
}

bool TypedMetadataInspectorModel::updateCoreFacts(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (!snapshot.active || generation != snapshot.generation || !info.isValid())
        return false;

    rebuild(info);
    return true;
}

bool TypedMetadataInspectorModel::setProjection(std::uint64_t generation,
                                                bool showRaw,
                                                bool showProvenance)
{
    if (!snapshot.active || generation != snapshot.generation)
        return false;

    snapshot.rawProjectionVisible = showRaw;
    snapshot.provenanceProjectionVisible = showProvenance;
    if (showRaw || showProvenance)
    {
        if (projectionProvider == nullptr)
        {
            projectionProvider = std::make_unique<ProjectionProvider>();
            ++projectionProviderConstructionCount;
        }
    }
    else
    {
        projectionProvider.reset();
    }
    return true;
}

bool TypedMetadataInspectorModel::applyOptionalProviderFailure(
    std::uint64_t generation,
    juce::String message)
{
    if (!snapshot.active || generation != snapshot.generation)
        return false;

    snapshot.localFailure = message.trim().isNotEmpty()
        ? std::move(message)
        : juce::String("Optional metadata provider failed.");
    return true;
}

void TypedMetadataInspectorModel::rebuild(const SingleTrackDocument::FileInfo& info)
{
    const auto showRaw = snapshot.rawProjectionVisible;
    const auto showProvenance = snapshot.provenanceProjectionVisible;
    const auto localFailure = snapshot.localFailure;
    snapshot.groups.clear();

    TypedMetadataGroup core { "core", "Core Facts", {} };
    core.fields.push_back(makeField("core.path", "Path", MetadataValueType::path,
                                    info.file.getFullPathName(), info.file.getFullPathName(),
                                    "Document core", "FileInfo.file", MetadataConfidence::exact,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.format", "Format", MetadataValueType::enumeration,
                                    info.formatName, info.formatName,
                                    "Open-path reader", "FileInfo.formatName", MetadataConfidence::reported,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.sample_rate", "Sample Rate", MetadataValueType::sampleRate,
                                    juce::String(info.sampleRate / 1000.0, 1) + " kHz",
                                    juce::String(info.sampleRate, 3),
                                    "Document core", "FileInfo.sampleRate", MetadataConfidence::exact,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.channels", "Channels", MetadataValueType::integer,
                                    juce::String(info.numChannels), juce::String(info.numChannels),
                                    "Document core", "FileInfo.numChannels", MetadataConfidence::exact,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.bit_depth", "Bit Depth", MetadataValueType::integer,
                                    info.bitsPerSample > 0 ? juce::String(info.bitsPerSample) + "-bit" : juce::String(),
                                    juce::String(info.bitsPerSample),
                                    "Open-path reader", "FileInfo.bitsPerSample", MetadataConfidence::reported,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.duration", "Duration", MetadataValueType::duration,
                                    juce::String(info.durationSeconds, 3) + " s",
                                    juce::String(info.durationSeconds, 9),
                                    "Document core", "FileInfo.durationSeconds", MetadataConfidence::exact,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    core.fields.push_back(makeField("core.channel_order", "Channel Order", MetadataValueType::enumeration,
                                    info.channelLayout.displayName,
                                    info.channelLayout.orderText,
                                    info.channelLayout.detectedFromMetadata ? "Embedded metadata" : "Channel layout policy",
                                    "FileInfo.channelLayout", info.channelLayout.detectedFromMetadata
                                        ? MetadataConfidence::reported : MetadataConfidence::inferred,
                                    MetadataConflictState::none, MetadataWriteCapability::readOnly, false));
    snapshot.groups.push_back(std::move(core));

    struct TagAccumulator final
    {
        juce::String normalisedKey;
        juce::String label;
        juce::StringArray nativeKeys;
        juce::StringArray values;
    };

    TypedMetadataGroup tags { "embedded", "Embedded Tags", {} };
    const auto keys = info.metadata.getAllKeys();
    std::vector<TagAccumulator> accumulatedTags;
    for (const auto& key : keys)
    {
        const auto normalisedKey = normaliseKey(key);
        const auto found = std::find_if(accumulatedTags.begin(), accumulatedTags.end(),
                                        [&normalisedKey](const TagAccumulator& candidate)
                                        {
                                            return candidate.normalisedKey == normalisedKey;
                                        });
        auto* target = found != accumulatedTags.end() ? &*found : nullptr;
        if (target == nullptr)
        {
            accumulatedTags.push_back({ normalisedKey, friendlyLabel(key), {}, {} });
            target = &accumulatedTags.back();
        }
        target->nativeKeys.add(key);
        target->values.add(info.metadata[key]);
    }

    for (const auto& tag : accumulatedTags)
    {
        const auto normalisedValue = tag.values.size() > 1
            ? joinRawValues(tag.values)
            : tag.values[0];
        const auto allEmpty = std::all_of(tag.values.begin(), tag.values.end(),
                                          [](const juce::String& value)
                                          {
                                              return value.isEmpty();
                                          });
        auto field = makeField(
            "tag." + (tag.normalisedKey.isNotEmpty() ? tag.normalisedKey : juce::String("unknown")),
            tag.label,
            allEmpty ? MetadataValueType::unknown : MetadataValueType::text,
            normalisedValue,
            tag.values[0],
            "Open-path reader",
            info.formatName + " metadata key(s) \"" + tag.nativeKeys.joinIntoString("\", \"") + "\"",
            allEmpty ? MetadataConfidence::unknown : MetadataConfidence::reported,
            tag.values.size() > 1 ? MetadataConflictState::duplicateNormalisedKey
                                  : MetadataConflictState::none,
            MetadataWriteCapability::unsupported,
            true);
        field.rawValues = tag.values;
        field.nativeKeys = tag.nativeKeys;
        field.multiplicity = tag.values.size();
        tags.fields.push_back(std::move(field));
    }

    if (tags.fields.empty())
    {
        tags.fields.push_back(makeField(
            "tag.unavailable", "Embedded Tags", MetadataValueType::unknown,
            "No embedded tags reported", {}, "Open-path reader",
            "FileInfo.metadata", MetadataConfidence::reported,
            MetadataConflictState::none, MetadataWriteCapability::unsupported, true));
    }
    snapshot.groups.push_back(std::move(tags));
    snapshot.optionalFieldsLoaded = true;
    snapshot.rawProjectionVisible = showRaw;
    snapshot.provenanceProjectionVisible = showProvenance;
    snapshot.localFailure = localFailure;
}

juce::String TypedMetadataInspectorModel::copyText(const juce::String& groupId) const
{
    juce::String text;
    for (const auto& group : snapshot.groups)
    {
        if (groupId.isNotEmpty() && group.id != groupId)
            continue;
        if (text.isNotEmpty())
            text << juce::newLine;
        text << group.label << juce::newLine;
        for (const auto& field : group.fields)
        {
            text << field.label << ": " << field.normalisedValue
                 << " [" << typeName(field.type) << "; " << field.source
                 << "; confidence=" << confidenceName(field.confidence)
                 << "; conflict=" << conflictName(field.conflict)
                 << "; write=" << writeCapabilityName(field.writeCapability)
                 << "; multiplicity=" << field.multiplicity;
            if (!field.nativeKeys.isEmpty())
                text << "; native-keys=" << field.nativeKeys.joinIntoString("|");
            text << "]";
            if (snapshot.rawProjectionVisible)
                text << " raw=" << projectedRawText(field);
            if (snapshot.provenanceProjectionVisible)
                text << " provenance=" << projectedProvenanceText(field);
            text << juce::newLine;
        }
    }
    if (snapshot.localFailure.isNotEmpty())
        text << juce::newLine << "Optional metadata error: " << snapshot.localFailure << juce::newLine;
    return text;
}

juce::String TypedMetadataInspectorModel::projectedRawText(const TypedMetadataField& field) const
{
    return snapshot.rawProjectionVisible && projectionProvider != nullptr
        ? projectionProvider->rawText(field) : juce::String();
}

juce::String TypedMetadataInspectorModel::projectedProvenanceText(const TypedMetadataField& field) const
{
    return snapshot.provenanceProjectionVisible && projectionProvider != nullptr
        ? projectionProvider->provenanceText(field) : juce::String();
}

MetadataInspectorDensity TypedMetadataInspectorModel::densityForWidth(int width) noexcept
{
    if (width < 270)
        return MetadataInspectorDensity::compact;
    if (width < 380)
        return MetadataInspectorDensity::standard;
    return MetadataInspectorDensity::expanded;
}

juce::String TypedMetadataInspectorModel::typeName(MetadataValueType type)
{
    switch (type)
    {
        case MetadataValueType::text: return "text";
        case MetadataValueType::path: return "path";
        case MetadataValueType::duration: return "duration";
        case MetadataValueType::sampleRate: return "sample-rate";
        case MetadataValueType::integer: return "integer";
        case MetadataValueType::enumeration: return "enum";
        case MetadataValueType::unknown: return "unknown";
    }
    return "unknown";
}

juce::String TypedMetadataInspectorModel::confidenceName(MetadataConfidence confidence)
{
    switch (confidence)
    {
        case MetadataConfidence::exact: return "exact";
        case MetadataConfidence::reported: return "reported";
        case MetadataConfidence::inferred: return "inferred";
        case MetadataConfidence::unknown: return "unknown";
    }
    return "unknown";
}

juce::String TypedMetadataInspectorModel::conflictName(MetadataConflictState conflict)
{
    switch (conflict)
    {
        case MetadataConflictState::none: return "none";
        case MetadataConflictState::duplicateNormalisedKey: return "duplicate-key";
        case MetadataConflictState::sourceChanged: return "source-changed";
        case MetadataConflictState::providerFailure: return "provider-failure";
    }
    return "unknown";
}

juce::String TypedMetadataInspectorModel::writeCapabilityName(MetadataWriteCapability capability)
{
    return capability == MetadataWriteCapability::readOnly ? "read-only" : "unsupported";
}
} // namespace singleeditor
