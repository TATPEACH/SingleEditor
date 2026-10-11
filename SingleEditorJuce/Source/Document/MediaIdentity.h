#pragma once

#include <cstdint>
#include <optional>
#include <juce_core/juce_core.h>

namespace singleeditor::media
{
// A locator is never proof that two live documents are the same document.
struct FileLocation final
{
    juce::File file;
};

struct SourceIdentity final
{
    juce::Uuid binding { nullptr };
    FileLocation location;
    int64_t sizeBytes = -1;
    int64_t modifiedMilliseconds = 0;
    bool fastStampValid = false;

    bool sameBinding(const SourceIdentity& other) const noexcept
    {
        return !binding.isNull() && binding == other.binding;
    }
};

struct DocumentIdentity final
{
    juce::Uuid lineage { nullptr };
    juce::Uuid runtimeInstance { nullptr };
    int64_t generation = 0;

    bool isValid() const noexcept
    {
        return !lineage.isNull() && !runtimeInstance.isNull() && generation > 0;
    }
    bool sameDocument(const DocumentIdentity& other) const noexcept
    {
        return isValid() && other.isValid() && lineage == other.lineage
            && runtimeInstance == other.runtimeInstance;
    }
    bool sameVersion(const DocumentIdentity& other) const noexcept
    {
        return sameDocument(other) && generation == other.generation;
    }
    juce::var toJson() const
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("version", 1);
        value->setProperty("lineage", lineage.toString());
        value->setProperty("runtime_instance", runtimeInstance.toString());
        value->setProperty("generation", generation);
        return juce::var(value);
    }
};

// Only a future catalog may issue these opaque references. M3 creates none,
// performs no lookup, and never writes them into media/cache/adjacent sidecars.
struct MediaAssetRef final
{
    int version = 1;
    juce::String issuer;
    juce::String key;
};

struct IdentitySnapshot final
{
    DocumentIdentity document;
    SourceIdentity source;
    std::optional<MediaAssetRef> asset;
    int64_t savedGeneration = 0;
};

enum class IdentityEventKind { opened, cleared, generationChanged, saveCheckpoint, sourceRebound };

struct IdentityEvent final
{
    uint64_t sequence = 0;
    IdentityEventKind kind = IdentityEventKind::generationChanged;
    IdentitySnapshot before;
    IdentitySnapshot after;
};
} // namespace singleeditor::media
