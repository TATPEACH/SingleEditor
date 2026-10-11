#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Document/SingleTrackDocument.h"

namespace singleeditor
{
enum class MetadataValueType
{
    text = 0,
    path,
    duration,
    sampleRate,
    integer,
    enumeration,
    unknown
};

enum class MetadataConfidence
{
    exact = 0,
    reported,
    inferred,
    unknown
};

enum class MetadataConflictState
{
    none = 0,
    duplicateNormalisedKey,
    sourceChanged,
    providerFailure
};

enum class MetadataWriteCapability
{
    readOnly = 0,
    unsupported
};

enum class MetadataInspectorDensity
{
    compact = 0,
    standard,
    expanded
};

struct TypedMetadataField final
{
    juce::String id;
    juce::String label;
    MetadataValueType type = MetadataValueType::unknown;
    juce::String normalisedValue;
    juce::StringArray rawValues;
    juce::StringArray nativeKeys;
    int multiplicity = 1;
    juce::String source;
    juce::String provenance;
    MetadataConfidence confidence = MetadataConfidence::unknown;
    MetadataConflictState conflict = MetadataConflictState::none;
    MetadataWriteCapability writeCapability = MetadataWriteCapability::unsupported;
    bool optional = false;
};

struct TypedMetadataGroup final
{
    juce::String id;
    juce::String label;
    std::vector<TypedMetadataField> fields;
};

struct MetadataInspectorSnapshot final
{
    std::uint64_t generation = 0;
    bool active = false;
    bool optionalFieldsLoaded = false;
    bool rawProjectionVisible = false;
    bool provenanceProjectionVisible = false;
    juce::String localFailure;
    std::vector<TypedMetadataGroup> groups;
};

class TypedMetadataInspectorModel final
{
public:
    static std::unique_ptr<TypedMetadataInspectorModel> createPostCore(
        std::uint64_t generation,
        const SingleTrackDocument::FileInfo& info);

    ~TypedMetadataInspectorModel();

    bool updateCoreFacts(std::uint64_t generation,
                         const SingleTrackDocument::FileInfo& info);
    bool setProjection(std::uint64_t generation,
                       bool showRaw,
                       bool showProvenance);
    bool applyOptionalProviderFailure(std::uint64_t generation,
                                      juce::String message);

    [[nodiscard]] const MetadataInspectorSnapshot& getSnapshot() const noexcept { return snapshot; }
    [[nodiscard]] juce::String copyText(const juce::String& groupId = {}) const;
    [[nodiscard]] juce::String projectedRawText(const TypedMetadataField& field) const;
    [[nodiscard]] juce::String projectedProvenanceText(const TypedMetadataField& field) const;
    [[nodiscard]] std::size_t getProjectionProviderConstructionCount() const noexcept
    {
        return projectionProviderConstructionCount;
    }

    [[nodiscard]] static MetadataInspectorDensity densityForWidth(int width) noexcept;
    [[nodiscard]] static juce::String typeName(MetadataValueType type);
    [[nodiscard]] static juce::String confidenceName(MetadataConfidence confidence);
    [[nodiscard]] static juce::String conflictName(MetadataConflictState conflict);
    [[nodiscard]] static juce::String writeCapabilityName(MetadataWriteCapability capability);

private:
    class ProjectionProvider;

    TypedMetadataInspectorModel() = default;
    void rebuild(const SingleTrackDocument::FileInfo& info);

    MetadataInspectorSnapshot snapshot;
    std::unique_ptr<ProjectionProvider> projectionProvider;
    std::size_t projectionProviderConstructionCount = 0;
};
} // namespace singleeditor
