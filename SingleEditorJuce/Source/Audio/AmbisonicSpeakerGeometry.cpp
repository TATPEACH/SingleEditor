#include "Audio/AmbisonicSpeakerGeometry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

#include <juce_cryptography/juce_cryptography.h>

#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicStandardMatrixArtifacts.generated.h"

namespace singleeditor
{
namespace
{
std::atomic<int> standardLookupCount { 0 };
std::atomic<int> customGenerationCount { 0 };

// Keep codec-only Identifier interning out of process startup. The bundle is
// initialized once, on the first explicit profile read or write.
struct GeometryProfileProperties
{
    const juce::Identifier schemaVersionProperty { "schemaVersion" };
    const juce::Identifier profileIdProperty { "profileId" };
    const juce::Identifier revisionProperty { "revision" };
    const juce::Identifier displayNameProperty { "displayName" };
    const juce::Identifier dimensionalityProperty { "dimensionality" };
    const juce::Identifier weightPolicyProperty { "weightPolicy" };
    const juce::Identifier generatorBackendProperty { "generatorBackendId" };
    const juce::Identifier generatorVersionProperty { "generatorVersion" };
    const juce::Identifier parentTemplateProperty { "parentTemplateId" };
    const juce::Identifier builtInReadOnlyProperty { "builtInReadOnly" };
    const juce::Identifier speakersProperty { "speakers" };
    const juce::Identifier stableIdProperty { "stableId" };
    const juce::Identifier labelProperty { "label" };
    const juce::Identifier roleProperty { "role" };
    const juce::Identifier azimuthProperty { "azimuthDegrees" };
    const juce::Identifier elevationProperty { "elevationDegrees" };
    const juce::Identifier distanceProperty { "distanceMetres" };
    const juce::Identifier staticWeightProperty { "staticWeight" };
    const juce::Identifier enabledProperty { "enabled" };
    const juce::Identifier availableProperty { "available" };
};

const GeometryProfileProperties& geometryProfileProperties()
{
    static const GeometryProfileProperties properties;
    return properties;
}

constexpr std::array<const char*, 7> forbiddenRootFields {
    "backendKind", "deviceIdentity", "driverIdentity", "sampleRate",
    "bufferSize", "physicalOutputChannels", "deviceBinding"
};
constexpr std::array<const char*, 4> forbiddenSpeakerFields {
    "physicalOutputChannel", "driverChannel", "deviceChannel", "deviceIndex"
};

juce::String dimensionalityToString(SpeakerGeometryDimensionality dimensionality)
{
    return dimensionality == SpeakerGeometryDimensionality::sphere3D ? "sphere3d" : "ring2d";
}

std::optional<SpeakerGeometryDimensionality> dimensionalityFromString(const juce::String& value)
{
    if (value == "ring2d")
        return SpeakerGeometryDimensionality::ring2D;
    if (value == "sphere3d")
        return SpeakerGeometryDimensionality::sphere3D;
    return {};
}

juce::String weightPolicyToString(SpeakerGeometryWeightPolicy policy)
{
    return policy == SpeakerGeometryWeightPolicy::explicitStatic ? "explicit-static" : "unity";
}

std::optional<SpeakerGeometryWeightPolicy> weightPolicyFromString(const juce::String& value)
{
    if (value == "unity")
        return SpeakerGeometryWeightPolicy::unity;
    if (value == "explicit-static")
        return SpeakerGeometryWeightPolicy::explicitStatic;
    return {};
}

juce::var cloneObjectOrCreate(const juce::var& source)
{
    auto result = source.isObject() ? source.clone() : juce::var(new juce::DynamicObject());
    if (!result.isObject())
        result = juce::var(new juce::DynamicObject());
    return result;
}

bool objectHasAnyProperty(const juce::DynamicObject& object,
                          const auto& propertyNames)
{
    for (const auto* name : propertyNames)
    {
        if (object.hasProperty(juce::Identifier(name)))
            return true;
    }
    return false;
}

void removeProperties(juce::DynamicObject& object, const auto& propertyNames)
{
    for (const auto* name : propertyNames)
        object.removeProperty(juce::Identifier(name));
}

juce::String sha256ForUtf8(const juce::String& value)
{
    const auto utf8 = value.toRawUTF8();
    return juce::SHA256(utf8, static_cast<std::size_t>(value.getNumBytesAsUTF8()))
        .toHexString().toUpperCase();
}

juce::String profileIdentity(const SpeakerGeometryProfile& profile)
{
    juce::String value = "speaker-geometry-profile-v1|" + profile.profileId
                       + "|" + juce::String(profile.revision)
                       + "|" + dimensionalityToString(profile.dimensionality)
                       + "|" + weightPolicyToString(profile.weightPolicy)
                       + "|" + profile.generatorBackendId
                       + "|" + profile.generatorVersion + "|";
    for (std::size_t index = 0; index < profile.speakers.size(); ++index)
    {
        const auto& speaker = profile.speakers[index];
        if (index != 0)
            value += ";";
        value += speaker.stableId + ","
              + ChannelLayoutCatalog::roleToAbbreviation(speaker.role) + ","
              + juce::String(speaker.azimuthDegrees, 6) + ","
              + juce::String(speaker.elevationDegrees, 6) + ","
              + juce::String(speaker.distanceMetres, 6) + ","
              + juce::String(speaker.staticWeight, 6) + ","
              + (speaker.enabled ? "1" : "0") + ","
              + (speaker.available ? "1" : "0");
    }
    return value;
}

juce::String makeCacheKey(const juce::String& profileId,
                          const juce::String& profileHash,
                          int ambisonicOrder,
                          const juce::String& matrixHash,
                          const juce::String& backend,
                          const juce::String& version,
                          const juce::String& weightProfile,
                          const juce::String& staticNormalization)
{
    return sha256ForUtf8(
        "ambisonic-matrix-cache-v1|ACN|SN3D|order=" + juce::String(ambisonicOrder)
        + "|profile=" + profileId + "|profileHash=" + profileHash
        + "|backend=" + backend + "|version=" + version
        + "|weight=" + weightProfile + "|normalization=" + staticNormalization
        + "|matrixHash=" + matrixHash);
}

juce::String matrixHashFromPointer(const float* gains,
                                   int outputChannels,
                                   int sourceChannels)
{
    juce::MemoryBlock bytes;
    bytes.setSize(static_cast<std::size_t>(outputChannels * sourceChannels * 4), false);
    auto* destination = static_cast<std::uint8_t*>(bytes.getData());
    auto offset = std::size_t { 0 };
    for (auto output = 0; output < outputChannels; ++output)
    {
        for (auto source = 0; source < sourceChannels; ++source)
        {
            std::uint32_t bits = 0;
            const auto value = gains[(output * sourceChannels) + source];
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(bits));
            destination[offset++] = static_cast<std::uint8_t>(bits & 0xffu);
            destination[offset++] = static_cast<std::uint8_t>((bits >> 8u) & 0xffu);
            destination[offset++] = static_cast<std::uint8_t>((bits >> 16u) & 0xffu);
            destination[offset++] = static_cast<std::uint8_t>((bits >> 24u) & 0xffu);
        }
    }
    return juce::SHA256(bytes).toHexString().toUpperCase();
}

const generated::StandardGeometryData* findGeneratedGeometry(const juce::String& profileId)
{
    for (const auto& candidate : generated::standardGeometries)
    {
        if (profileId == candidate.profileId)
            return &candidate;
    }
    return nullptr;
}

const generated::StandardGeometryData* findGeneratedGeometryForLayout(
    const ChannelLayoutState& targetLayout) noexcept
{
    for (const auto& candidate : generated::standardGeometries)
    {
        if (candidate.speakerCount != static_cast<int>(targetLayout.selectedRoles.size()))
            continue;
        auto matches = true;
        for (auto index = 0; index < candidate.speakerCount; ++index)
        {
            if (static_cast<SpeakerRole>(candidate.speakers[static_cast<std::size_t>(index)].role)
                != targetLayout.selectedRoles[static_cast<std::size_t>(index)])
            {
                matches = false;
                break;
            }
        }
        if (matches)
            return &candidate;
    }
    return nullptr;
}

SpeakerGeometryProfile profileFromGenerated(const generated::StandardGeometryData& source)
{
    SpeakerGeometryProfile profile;
    profile.profileId = source.profileId;
    profile.displayName = source.displayName;
    profile.dimensionality = juce::String(source.dimensionality) == "sphere3d"
                               ? SpeakerGeometryDimensionality::sphere3D
                               : SpeakerGeometryDimensionality::ring2D;
    profile.builtInReadOnly = true;
    profile.speakers.reserve(static_cast<std::size_t>(source.speakerCount));
    for (auto index = 0; index < source.speakerCount; ++index)
    {
        const auto& item = source.speakers[static_cast<std::size_t>(index)];
        SpeakerGeometrySpeaker next;
        next.stableId = item.stableId;
        next.label = item.label;
        next.role = static_cast<SpeakerRole>(item.role);
        next.azimuthDegrees = item.azimuthDegrees;
        next.elevationDegrees = item.elevationDegrees;
        next.distanceMetres = item.distanceMetres;
        next.staticWeight = item.staticWeight;
        next.enabled = item.enabled;
        next.available = item.available;
        profile.speakers.push_back(std::move(next));
    }
    return profile;
}

void addDiagnostic(SpeakerGeometryValidationResult& result,
                   SpeakerGeometryDiagnosticSeverity severity,
                   const juce::String& code,
                   const juce::String& message,
                   std::vector<juce::String> speakerIds = {})
{
    result.diagnostics.push_back({ severity, code, message, std::move(speakerIds) });
}

bool finite(float value) noexcept
{
    return std::isfinite(value);
}

std::array<float, 3> directionFor(float azimuthDegrees, float elevationDegrees) noexcept
{
    const auto azimuth = juce::degreesToRadians(azimuthDegrees);
    const auto elevation = juce::degreesToRadians(elevationDegrees);
    const auto elevationCosine = std::cos(elevation);
    return { elevationCosine * std::cos(azimuth),
             elevationCosine * std::sin(azimuth),
             std::sin(elevation) };
}

float angularSeparationDegrees(const SpeakerGeometrySpeaker& left,
                               const SpeakerGeometrySpeaker& right) noexcept
{
    const auto a = directionFor(left.azimuthDegrees, left.elevationDegrees);
    const auto b = directionFor(right.azimuthDegrees, right.elevationDegrees);
    const auto dot = juce::jlimit(-1.0f, 1.0f,
                                  (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]));
    return juce::radiansToDegrees(std::acos(dot));
}

float determinant(const std::array<float, 3>& a,
                  const std::array<float, 3>& b,
                  const std::array<float, 3>& c) noexcept
{
    return a[0] * ((b[1] * c[2]) - (b[2] * c[1]))
         - a[1] * ((b[0] * c[2]) - (b[2] * c[0]))
         + a[2] * ((b[0] * c[1]) - (b[1] * c[0]));
}

std::vector<const SpeakerGeometrySpeaker*> activeDirectionalSpeakers(
    const SpeakerGeometryProfile& profile)
{
    std::vector<const SpeakerGeometrySpeaker*> result;
    for (const auto& speaker : profile.speakers)
    {
        if (speaker.enabled && speaker.available
            && speaker.role != SpeakerRole::LFE && speaker.role != SpeakerRole::mono)
        {
            result.push_back(&speaker);
        }
    }
    return result;
}
} // namespace

juce::var SpeakerGeometryProfileCodec::toVar(const SpeakerGeometryProfile& profile)
{
    const auto& properties = geometryProfileProperties();
    auto result = cloneObjectOrCreate(profile.opaqueSource);
    auto* object = result.getDynamicObject();
    removeProperties(*object, forbiddenRootFields);
    object->setProperty(properties.schemaVersionProperty, SpeakerGeometryProfile::schemaVersion);
    object->setProperty(properties.profileIdProperty, profile.profileId);
    object->setProperty(properties.revisionProperty, static_cast<juce::int64>(profile.revision));
    object->setProperty(properties.displayNameProperty, profile.displayName);
    object->setProperty(properties.dimensionalityProperty, dimensionalityToString(profile.dimensionality));
    object->setProperty(properties.weightPolicyProperty, weightPolicyToString(profile.weightPolicy));
    object->setProperty(properties.generatorBackendProperty, profile.generatorBackendId);
    object->setProperty(properties.generatorVersionProperty, profile.generatorVersion);
    object->setProperty(properties.parentTemplateProperty, profile.parentTemplateId);
    object->setProperty(properties.builtInReadOnlyProperty, profile.builtInReadOnly);

    juce::Array<juce::var> speakers;
    for (const auto& source : profile.speakers)
    {
        auto encoded = cloneObjectOrCreate(source.opaqueSource);
        auto* speaker = encoded.getDynamicObject();
        removeProperties(*speaker, forbiddenSpeakerFields);
        speaker->setProperty(properties.stableIdProperty, source.stableId);
        speaker->setProperty(properties.labelProperty, source.label);
        speaker->setProperty(properties.roleProperty, ChannelLayoutCatalog::roleToAbbreviation(source.role));
        speaker->setProperty(properties.azimuthProperty, source.azimuthDegrees);
        speaker->setProperty(properties.elevationProperty, source.elevationDegrees);
        speaker->setProperty(properties.distanceProperty, source.distanceMetres);
        speaker->setProperty(properties.staticWeightProperty, source.staticWeight);
        speaker->setProperty(properties.enabledProperty, source.enabled);
        speaker->setProperty(properties.availableProperty, source.available);
        speakers.add(std::move(encoded));
    }
    object->setProperty(properties.speakersProperty, juce::var(std::move(speakers)));
    return result;
}

SpeakerGeometryProfileParseResult SpeakerGeometryProfileCodec::fromVar(const juce::var& source)
{
    SpeakerGeometryProfileParseResult result;
    const auto* object = source.getDynamicObject();
    if (object == nullptr)
    {
        result.errorCode = "profile_not_object";
        result.errorMessage = "Geometry profile must be a JSON object.";
        return result;
    }
    if (objectHasAnyProperty(*object, forbiddenRootFields))
    {
        result.errorCode = "device_binding_field_forbidden";
        result.errorMessage = "Geometry profiles cannot contain device or physical-output fields.";
        return result;
    }
    const auto& properties = geometryProfileProperties();
    if (static_cast<int>(object->getProperty(properties.schemaVersionProperty))
        != SpeakerGeometryProfile::schemaVersion)
    {
        result.errorCode = "unsupported_profile_schema";
        result.errorMessage = "Unsupported geometry profile schema.";
        return result;
    }

    auto dimensionality = dimensionalityFromString(object->getProperty(properties.dimensionalityProperty).toString());
    auto weightPolicy = weightPolicyFromString(object->getProperty(properties.weightPolicyProperty).toString());
    if (!dimensionality.has_value() || !weightPolicy.has_value())
    {
        result.errorCode = "unsupported_profile_policy";
        result.errorMessage = "Geometry dimensionality or weight policy is unsupported.";
        return result;
    }

    auto& profile = result.profile;
    profile.profileId = object->getProperty(properties.profileIdProperty).toString();
    profile.revision = static_cast<int64_t>(static_cast<juce::int64>(object->getProperty(properties.revisionProperty)));
    profile.displayName = object->getProperty(properties.displayNameProperty).toString();
    profile.dimensionality = *dimensionality;
    profile.weightPolicy = *weightPolicy;
    profile.generatorBackendId = object->getProperty(properties.generatorBackendProperty).toString();
    profile.generatorVersion = object->getProperty(properties.generatorVersionProperty).toString();
    profile.parentTemplateId = object->getProperty(properties.parentTemplateProperty).toString();
    profile.builtInReadOnly = false;
    profile.opaqueSource = source.clone();

    const auto encodedSpeakers = object->getProperty(properties.speakersProperty);
    const auto* array = encodedSpeakers.getArray();
    if (array == nullptr)
    {
        result.errorCode = "speakers_not_array";
        result.errorMessage = "Geometry speakers must be an array.";
        return result;
    }
    profile.speakers.reserve(static_cast<std::size_t>(array->size()));
    for (const auto& item : *array)
    {
        const auto* speakerObject = item.getDynamicObject();
        if (speakerObject == nullptr || objectHasAnyProperty(*speakerObject, forbiddenSpeakerFields))
        {
            result.errorCode = "speaker_device_field_forbidden";
            result.errorMessage = "Geometry speakers cannot contain physical-output fields.";
            return result;
        }
        SpeakerGeometrySpeaker speaker;
        speaker.stableId = speakerObject->getProperty(properties.stableIdProperty).toString();
        speaker.label = speakerObject->getProperty(properties.labelProperty).toString();
        speaker.role = ChannelLayoutCatalog::roleFromAbbreviation(
            speakerObject->getProperty(properties.roleProperty).toString());
        speaker.azimuthDegrees = static_cast<float>(speakerObject->getProperty(properties.azimuthProperty));
        speaker.elevationDegrees = static_cast<float>(speakerObject->getProperty(properties.elevationProperty));
        speaker.distanceMetres = static_cast<float>(speakerObject->getProperty(properties.distanceProperty));
        speaker.staticWeight = static_cast<float>(speakerObject->getProperty(properties.staticWeightProperty));
        speaker.enabled = static_cast<bool>(speakerObject->getProperty(properties.enabledProperty));
        speaker.available = static_cast<bool>(speakerObject->getProperty(properties.availableProperty));
        speaker.opaqueSource = item.clone();
        profile.speakers.push_back(std::move(speaker));
    }

    result.success = true;
    return result;
}

bool SpeakerGeometryValidationResult::isValid() const noexcept
{
    return std::none_of(diagnostics.begin(), diagnostics.end(), [] (const auto& diagnostic)
    {
        return diagnostic.severity == SpeakerGeometryDiagnosticSeverity::error;
    });
}

bool SpeakerGeometryValidationResult::hasWarnings() const noexcept
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [] (const auto& diagnostic)
    {
        return diagnostic.severity == SpeakerGeometryDiagnosticSeverity::warning;
    });
}

bool AmbisonicDecodeMatrixArtifact::isValid() const noexcept
{
    if (!juce::isPositiveAndBelow(ambisonicOrder, 4)
        || decodeMatrix.sourceChannels != (ambisonicOrder + 1) * (ambisonicOrder + 1)
        || decodeMatrix.outputChannels <= 0
        || decodeMatrix.outputChannels > RoutingMatrix::maxChannels
        || artifactId.isEmpty() || matrixHash.isEmpty() || cacheKey.isEmpty()
        || profileHash.isEmpty() || geometry.profileId.isEmpty())
    {
        return false;
    }
    if (!channelOrdering.equalsIgnoreCase("ACN")
        || !ambisonicNormalization.equalsIgnoreCase("SN3D")
        || generatorBackendId != geometry.generatorBackendId
        || generatorVersion != geometry.generatorVersion)
    {
        return false;
    }
    auto activeSpeakerCount = 0;
    for (const auto& speaker : geometry.speakers)
    {
        if (speaker.enabled && speaker.available)
            ++activeSpeakerCount;
    }
    if (activeSpeakerCount != decodeMatrix.outputChannels)
        return false;
    for (auto output = 0; output < decodeMatrix.outputChannels; ++output)
    {
        for (auto source = 0; source < decodeMatrix.sourceChannels; ++source)
        {
            if (!finite(decodeMatrix.gain(output, source)))
                return false;
        }
    }
    if (AmbisonicSpeakerGeometryGenerator::profileIdentityHash(geometry) != profileHash
        || AmbisonicSpeakerGeometryGenerator::matrixHash(decodeMatrix) != matrixHash)
    {
        return false;
    }
    return makeCacheKey(geometry.profileId,
                        profileHash,
                        ambisonicOrder,
                        matrixHash,
                        generatorBackendId,
                        generatorVersion,
                        weightProfileId,
                        staticNormalizationId) == cacheKey;
}

ChannelLayoutState AmbisonicDecodeMatrixArtifact::canonicalSpeakerBus() const
{
    ChannelLayoutState result;
    result.selectedPresetId = geometry.profileId;
    result.displayName = geometry.displayName;
    result.interpretationOrigin = ChannelInterpretationOrigin::explicitPreset;
    for (const auto& speaker : geometry.speakers)
    {
        if (!speaker.enabled || !speaker.available)
            continue;
        result.selectedRoles.push_back(speaker.role);
        if (result.orderText.isNotEmpty())
            result.orderText += " ";
        result.orderText += ChannelLayoutCatalog::roleToAbbreviation(speaker.role);
    }
    result.sourceChannelCount = static_cast<int>(result.selectedRoles.size());
    return result;
}

std::optional<StandardAmbisonicMatrixView> AmbisonicStandardMatrixCatalog::find(
    const ChannelLayoutState& targetLayout,
    int ambisonicOrder) noexcept
{
    standardLookupCount.fetch_add(1, std::memory_order_relaxed);
    const auto* geometry = findGeneratedGeometryForLayout(targetLayout);
    if (geometry == nullptr)
        return {};
    for (const auto& matrix : generated::standardMatrices)
    {
        if (ambisonicOrder != matrix.ambisonicOrder
            || std::strcmp(geometry->profileId, matrix.profileId) != 0)
        {
            continue;
        }
        StandardAmbisonicMatrixView result;
        result.artifactId = matrix.artifactId;
        result.profileId = matrix.profileId;
        result.profileHash = matrix.profileHash;
        result.matrixHash = matrix.matrixHash;
        result.cacheKey = matrix.cacheKey;
        result.ambisonicOrder = matrix.ambisonicOrder;
        result.outputChannels = matrix.outputChannels;
        result.sourceChannels = matrix.sourceChannels;
        result.dimensionality = std::strcmp(geometry->dimensionality, "sphere3d") == 0
                                  ? SpeakerGeometryDimensionality::sphere3D
                                  : SpeakerGeometryDimensionality::ring2D;
        for (auto index = 0; index < geometry->speakerCount; ++index)
        {
            const auto& item = geometry->speakers[static_cast<std::size_t>(index)];
            result.speakers[static_cast<std::size_t>(index)] = {
                item.stableId, item.label, static_cast<SpeakerRole>(item.role),
                item.azimuthDegrees, item.elevationDegrees,
                item.distanceMetres, item.staticWeight, item.enabled, item.available
            };
        }
        result.gains = matrix.gains.data();
        return result;
    }
    return {};
}

std::shared_ptr<const SpeakerGeometryProfile> AmbisonicStandardMatrixCatalog::standardProfile(
    const juce::String& profileId)
{
    const auto* source = findGeneratedGeometry(profileId);
    return source != nullptr
             ? std::make_shared<const SpeakerGeometryProfile>(profileFromGenerated(*source))
             : std::shared_ptr<const SpeakerGeometryProfile> {};
}

std::vector<juce::String> AmbisonicStandardMatrixCatalog::standardProfileIds()
{
    std::vector<juce::String> result;
    result.reserve(generated::standardGeometries.size());
    for (const auto& profile : generated::standardGeometries)
        result.emplace_back(profile.profileId);
    return result;
}

bool AmbisonicStandardMatrixCatalog::verifyAllArtifacts(juce::String& failureReason)
{
    for (const auto& profileData : generated::standardGeometries)
    {
        const auto profile = profileFromGenerated(profileData);
        if (AmbisonicSpeakerGeometryGenerator::profileIdentityHash(profile)
            != juce::String(profileData.profileHash))
        {
            failureReason = "profile_hash_mismatch:" + juce::String(profileData.profileId);
            return false;
        }
    }
    for (const auto& matrix : generated::standardMatrices)
    {
        const auto hash = matrixHashFromPointer(matrix.gains.data(),
                                                matrix.outputChannels,
                                                matrix.sourceChannels);
        if (hash != juce::String(matrix.matrixHash))
        {
            failureReason = "matrix_hash_mismatch:" + juce::String(matrix.artifactId);
            return false;
        }
        const auto cacheKey = makeCacheKey(matrix.profileId,
                                           matrix.profileHash,
                                           matrix.ambisonicOrder,
                                           matrix.matrixHash,
                                           generated::generatorBackend,
                                           generated::generatorVersion,
                                           generated::weightProfile,
                                           generated::staticNormalization);
        if (cacheKey != juce::String(matrix.cacheKey))
        {
            failureReason = "cache_key_mismatch:" + juce::String(matrix.artifactId);
            return false;
        }
        const auto* profile = findGeneratedGeometry(matrix.profileId);
        if (profile == nullptr || profile->speakerCount != matrix.outputChannels)
        {
            failureReason = "geometry_mismatch:" + juce::String(matrix.artifactId);
            return false;
        }
        for (auto output = 0; output < matrix.outputChannels; ++output)
        {
            const auto isLfe = static_cast<SpeakerRole>(profile->speakers[static_cast<std::size_t>(output)].role)
                             == SpeakerRole::LFE;
            for (auto source = 0; source < matrix.sourceChannels; ++source)
            {
                const auto gain = matrix.gains[static_cast<std::size_t>((output * matrix.sourceChannels) + source)];
                if (!finite(gain) || (isLfe && gain != 0.0f))
                {
                    failureReason = "matrix_fixture_failure:" + juce::String(matrix.artifactId);
                    return false;
                }
            }
        }
    }
    failureReason.clear();
    return true;
}

int AmbisonicStandardMatrixCatalog::getLookupCountForDiagnostics() noexcept
{
    return standardLookupCount.load(std::memory_order_relaxed);
}

void AmbisonicStandardMatrixCatalog::resetDiagnostics() noexcept
{
    standardLookupCount.store(0, std::memory_order_relaxed);
}

bool SpeakerGeometryGenerationResult::succeeded() const noexcept
{
    return status == SpeakerGeometryGenerationStatus::ready
        && artifact != nullptr && artifact->isValid();
}

SpeakerGeometryValidationResult AmbisonicSpeakerGeometryGenerator::validate(
    const SpeakerGeometryProfile& profile)
{
    SpeakerGeometryValidationResult result;
    result.profileHash = profileIdentityHash(profile);
    if (profile.profileId.trim().isEmpty())
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "profile_id_required", "Profile ID is required.");
    if (profile.revision <= 0)
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "revision_invalid", "Profile revision must be positive.");
    if (profile.builtInReadOnly)
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "builtin_read_only", "Built-in templates must be derived before editing.");
    if (profile.generatorBackendId != generated::generatorBackend
        || profile.generatorVersion != generated::generatorVersion)
    {
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "generator_unsupported", "Geometry generator backend or version is unsupported.");
    }
    if (profile.speakers.empty() || profile.speakers.size() > RoutingMatrix::maxChannels)
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "speaker_count_unsupported", "Geometry speaker count is unsupported.");

    std::set<std::string> stableIds;
    std::set<int> roles;
    auto minDistance = std::numeric_limits<float>::max();
    auto maxDistance = 0.0f;
    for (const auto& speaker : profile.speakers)
    {
        const auto id = speaker.stableId.trim();
        if (id.isEmpty() || !stableIds.insert(id.toStdString()).second)
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_id_duplicate", "Speaker stable IDs must be non-empty and unique.",
                          { speaker.stableId });
        const auto roleValue = static_cast<int>(speaker.role);
        if (speaker.role == SpeakerRole::unknown || speaker.role == SpeakerRole::discrete
            || ChannelLayoutCatalog::isAmbisonicRole(speaker.role))
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_role_unsupported", "Speaker role is unsupported by the canonical bus.",
                          { speaker.stableId });
        }
        else if (!roles.insert(roleValue).second)
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_role_duplicate", "Canonical speaker roles must be unique.",
                          { speaker.stableId });
        }
        if (!finite(speaker.azimuthDegrees) || !finite(speaker.elevationDegrees)
            || !finite(speaker.distanceMetres) || !finite(speaker.staticWeight))
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_coordinate_non_finite", "Speaker coordinates and weights must be finite.",
                          { speaker.stableId });
            continue;
        }
        if (speaker.azimuthDegrees < -180.0f || speaker.azimuthDegrees > 180.0f
            || speaker.elevationDegrees < -90.0f || speaker.elevationDegrees > 90.0f)
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_coordinate_out_of_range", "Speaker direction is outside canonical bounds.",
                          { speaker.stableId });
        }
        if (profile.dimensionality == SpeakerGeometryDimensionality::ring2D
            && std::abs(speaker.elevationDegrees) > 0.01f && speaker.role != SpeakerRole::LFE)
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "ring_elevation_mismatch", "2D ring speakers must have zero elevation.",
                          { speaker.stableId });
        }
        if (speaker.distanceMetres <= 0.0f)
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_distance_invalid", "Speaker distance must be positive.",
                          { speaker.stableId });
        if (profile.weightPolicy == SpeakerGeometryWeightPolicy::explicitStatic
            && (speaker.staticWeight <= 0.0f || speaker.staticWeight > 8.0f))
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "speaker_weight_invalid", "Explicit static weights must be in (0, 8].",
                          { speaker.stableId });
        }
        if (speaker.enabled && speaker.available && speaker.role != SpeakerRole::LFE)
        {
            minDistance = juce::jmin(minDistance, speaker.distanceMetres);
            maxDistance = juce::jmax(maxDistance, speaker.distanceMetres);
        }
        if (speaker.enabled && !speaker.available)
        {
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::warning,
                          "speaker_unavailable", "Enabled speaker is unavailable and will not enter the candidate bus.",
                          { speaker.stableId });
        }
    }

    const auto directional = activeDirectionalSpeakers(profile);
    const auto minimumSpeakers = profile.dimensionality == SpeakerGeometryDimensionality::sphere3D ? 4 : 3;
    if (static_cast<int>(directional.size()) < minimumSpeakers)
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                      "speaker_count_insufficient", "Active geometry has too few directional speakers.");

    for (std::size_t left = 0; left < directional.size(); ++left)
    {
        for (std::size_t right = left + 1; right < directional.size(); ++right)
        {
            if (angularSeparationDegrees(*directional[left], *directional[right]) < 1.0f)
            {
                addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                              "speaker_direction_duplicate", "Speaker directions are duplicate or near-duplicate.",
                              { directional[left]->stableId, directional[right]->stableId });
            }
        }
    }

    if (directional.size() >= 3)
    {
        std::vector<float> azimuths;
        azimuths.reserve(directional.size());
        for (const auto* speaker : directional)
            azimuths.push_back(speaker->azimuthDegrees < 0.0f
                                 ? speaker->azimuthDegrees + 360.0f
                                 : speaker->azimuthDegrees);
        std::sort(azimuths.begin(), azimuths.end());
        auto largestGap = 0.0f;
        for (std::size_t index = 0; index < azimuths.size(); ++index)
        {
            const auto next = index + 1 < azimuths.size() ? azimuths[index + 1]
                                                           : azimuths.front() + 360.0f;
            largestGap = juce::jmax(largestGap, next - azimuths[index]);
        }
        if (largestGap > 150.0f)
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::warning,
                          "azimuth_coverage_thin", "Speaker azimuth coverage contains a gap above 150 degrees.");
    }

    if (profile.dimensionality == SpeakerGeometryDimensionality::sphere3D
        && directional.size() >= 4)
    {
        auto maximumVolume = 0.0f;
        for (std::size_t a = 0; a < directional.size(); ++a)
            for (std::size_t b = a + 1; b < directional.size(); ++b)
                for (std::size_t c = b + 1; c < directional.size(); ++c)
                    maximumVolume = juce::jmax(maximumVolume,
                        std::abs(determinant(directionFor(directional[a]->azimuthDegrees,
                                                         directional[a]->elevationDegrees),
                                             directionFor(directional[b]->azimuthDegrees,
                                                         directional[b]->elevationDegrees),
                                             directionFor(directional[c]->azimuthDegrees,
                                                         directional[c]->elevationDegrees))));
        if (maximumVolume < 0.01f)
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::error,
                          "geometry_degenerate", "3D geometry is degenerate and cannot form a spatial solution.");
        const auto hasBelow = std::any_of(directional.begin(), directional.end(), [] (const auto* item)
        {
            return item->elevationDegrees < -5.0f;
        });
        if (!hasBelow)
            addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::warning,
                          "vertical_coverage_thin", "3D geometry has no speakers below the listener plane.");
    }

    if (minDistance < std::numeric_limits<float>::max()
        && maxDistance > minDistance * 4.0f)
    {
        addDiagnostic(result, SpeakerGeometryDiagnosticSeverity::warning,
                      "distance_spread_large", "Speaker distance spread exceeds 4:1.");
    }
    return result;
}

SpeakerGeometryGenerationResult AmbisonicSpeakerGeometryGenerator::generate(
    const SpeakerGeometryGenerationRequest& request)
{
    customGenerationCount.fetch_add(1, std::memory_order_relaxed);
    SpeakerGeometryGenerationResult result;
    result.requestId = request.requestId;
    result.draftRevision = request.draftRevision;
    if (request.draftRevision != request.profile.revision)
    {
        result.status = SpeakerGeometryGenerationStatus::staleRevision;
        return result;
    }
    if (!juce::isPositiveAndBelow(request.ambisonicOrder, 4))
    {
        result.status = SpeakerGeometryGenerationStatus::unsupportedOrder;
        return result;
    }
    result.validation = validate(request.profile);
    if (!result.validation.isValid())
    {
        result.status = SpeakerGeometryGenerationStatus::invalidProfile;
        return result;
    }

    auto artifact = std::make_shared<AmbisonicDecodeMatrixArtifact>();
    artifact->ambisonicOrder = request.ambisonicOrder;
    artifact->profileHash = result.validation.profileHash;
    artifact->generatorBackendId = request.profile.generatorBackendId;
    artifact->generatorVersion = request.profile.generatorVersion;
    artifact->geometry = request.profile;
    artifact->decodeMatrix.sourceChannels = (request.ambisonicOrder + 1)
                                           * (request.ambisonicOrder + 1);
    artifact->decodeMatrix.gains.fill(0.0f);

    std::array<float, 16> basis {};
    auto output = 0;
    for (const auto& speaker : request.profile.speakers)
    {
        if (!speaker.enabled || !speaker.available)
            continue;
        if (output >= RoutingMatrix::maxChannels)
        {
            result.status = SpeakerGeometryGenerationStatus::generationFailed;
            return result;
        }
        if (speaker.role == SpeakerRole::mono)
        {
            artifact->decodeMatrix.gain(output, 0) =
                AmbisonicMonitorDecoder::fixedReferenceGainForOrder(request.ambisonicOrder);
        }
        else if (speaker.role != SpeakerRole::LFE)
        {
            AmbisonicMonitorDecoder::evaluateSn3dBasis(request.ambisonicOrder,
                                                       speaker.azimuthDegrees,
                                                       speaker.elevationDegrees,
                                                       {}, basis);
            const auto staticWeight = request.profile.weightPolicy
                                        == SpeakerGeometryWeightPolicy::explicitStatic
                                    ? speaker.staticWeight : 1.0f;
            for (auto acn = 0; acn < artifact->decodeMatrix.sourceChannels; ++acn)
            {
                const auto degree = static_cast<int>(
                    std::floor(std::sqrt(static_cast<float>(acn))));
                artifact->decodeMatrix.gain(output, acn) = staticWeight
                    * AmbisonicMonitorDecoder::fixedReferenceGainForOrder(request.ambisonicOrder)
                    * AmbisonicMonitorDecoder::maxReWeightForDegree(request.ambisonicOrder, degree)
                    * basis[static_cast<std::size_t>(acn)];
            }
        }
        ++output;
    }
    artifact->decodeMatrix.outputChannels = output;
    artifact->matrixHash = matrixHash(artifact->decodeMatrix);
    artifact->cacheKey = makeCacheKey(request.profile.profileId,
                                      artifact->profileHash,
                                      request.ambisonicOrder,
                                      artifact->matrixHash,
                                      artifact->generatorBackendId,
                                      artifact->generatorVersion,
                                      artifact->weightProfileId,
                                      artifact->staticNormalizationId);
    artifact->artifactId = "singleeditor.custom-matrix.v1:"
                         + request.profile.profileId + ":r"
                         + juce::String(request.profile.revision) + ":o"
                         + juce::String(request.ambisonicOrder) + ":"
                         + artifact->matrixHash.substring(0, 16);
    if (!artifact->isValid())
    {
        result.status = SpeakerGeometryGenerationStatus::generationFailed;
        return result;
    }
    result.status = SpeakerGeometryGenerationStatus::ready;
    result.artifact = std::move(artifact);
    return result;
}

juce::String AmbisonicSpeakerGeometryGenerator::profileIdentityHash(
    const SpeakerGeometryProfile& profile)
{
    return sha256ForUtf8(profileIdentity(profile));
}

juce::String AmbisonicSpeakerGeometryGenerator::matrixHash(const RoutingMatrix& matrix)
{
    if (matrix.sourceChannels <= 0 || matrix.outputChannels <= 0)
        return {};
    std::array<float, RoutingMatrix::maxChannels * RoutingMatrix::maxChannels> packed {};
    auto index = 0;
    for (auto output = 0; output < matrix.outputChannels; ++output)
        for (auto source = 0; source < matrix.sourceChannels; ++source)
            packed[static_cast<std::size_t>(index++)] = matrix.gain(output, source);
    return matrixHashFromPointer(packed.data(), matrix.outputChannels, matrix.sourceChannels);
}

int AmbisonicSpeakerGeometryGenerator::getGenerationCountForDiagnostics() noexcept
{
    return customGenerationCount.load(std::memory_order_relaxed);
}

void AmbisonicSpeakerGeometryGenerator::resetDiagnostics() noexcept
{
    customGenerationCount.store(0, std::memory_order_relaxed);
}

AmbisonicSpeakerGeometrySession::AmbisonicSpeakerGeometrySession(
    ActivationCallback activationCallback)
    : onActivate(std::move(activationCallback))
{
}

void AmbisonicSpeakerGeometrySession::setDraft(SpeakerGeometryProfile profile)
{
    draft = std::move(profile);
    validation = {};
    candidate.reset();
    pendingRequestId = 0;
    pendingDraftRevision = 0;
    state = SpeakerGeometrySessionState::draft;
}

SpeakerGeometryValidationResult AmbisonicSpeakerGeometrySession::validateDraft()
{
    validation = draft.has_value()
               ? AmbisonicSpeakerGeometryGenerator::validate(*draft)
               : SpeakerGeometryValidationResult {};
    state = draft.has_value() && validation.isValid()
          ? SpeakerGeometrySessionState::validated
          : SpeakerGeometrySessionState::error;
    return validation;
}

std::optional<SpeakerGeometryGenerationRequest>
AmbisonicSpeakerGeometrySession::beginGeneration(int ambisonicOrder)
{
    if (!draft.has_value())
        return {};
    validation = AmbisonicSpeakerGeometryGenerator::validate(*draft);
    if (!validation.isValid())
    {
        state = SpeakerGeometrySessionState::error;
        return {};
    }
    SpeakerGeometryGenerationRequest request;
    request.requestId = nextRequestId++;
    request.draftRevision = draft->revision;
    request.ambisonicOrder = ambisonicOrder;
    request.profile = *draft;
    pendingRequestId = request.requestId;
    pendingDraftRevision = request.draftRevision;
    state = SpeakerGeometrySessionState::generating;
    return request;
}

SpeakerGeometryGenerationStatus AmbisonicSpeakerGeometrySession::acceptGenerationResult(
    SpeakerGeometryGenerationResult result)
{
    if (!draft.has_value() || pendingRequestId == 0
        || result.requestId != pendingRequestId
        || result.draftRevision != pendingDraftRevision
        || draft->revision != pendingDraftRevision)
    {
        return SpeakerGeometryGenerationStatus::staleRevision;
    }
    pendingRequestId = 0;
    pendingDraftRevision = 0;
    validation = result.validation;
    if (!result.succeeded())
    {
        state = SpeakerGeometrySessionState::error;
        return result.status;
    }
    const auto key = result.artifact->cacheKey.toStdString();
    if (const auto found = candidateCache.find(key); found != candidateCache.end())
        candidate = found->second;
    else
    {
        candidate = std::move(result.artifact);
        candidateCache.emplace(key, candidate);
    }
    state = SpeakerGeometrySessionState::candidate;
    return SpeakerGeometryGenerationStatus::ready;
}

void AmbisonicSpeakerGeometrySession::cancelPendingGeneration() noexcept
{
    pendingRequestId = 0;
    pendingDraftRevision = 0;
    if (draft.has_value())
        state = validation.isValid() ? SpeakerGeometrySessionState::validated
                                     : SpeakerGeometrySessionState::draft;
    else
        state = SpeakerGeometrySessionState::empty;
}

std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
AmbisonicSpeakerGeometrySession::beginAudition()
{
    if (candidate == nullptr)
        return {};
    state = SpeakerGeometrySessionState::auditioning;
    return candidate;
}

void AmbisonicSpeakerGeometrySession::endAudition() noexcept
{
    if (state == SpeakerGeometrySessionState::auditioning)
        state = candidate != nullptr ? SpeakerGeometrySessionState::candidate
                                     : SpeakerGeometrySessionState::draft;
}

bool AmbisonicSpeakerGeometrySession::activateCandidate()
{
    if (candidate == nullptr || !candidate->isValid())
        return false;
    const auto proposed = candidate;
    if (onActivate && !onActivate(proposed))
    {
        state = SpeakerGeometrySessionState::candidate;
        return false;
    }
    active = proposed;
    state = SpeakerGeometrySessionState::active;
    return true;
}

const SpeakerGeometryProfile* AmbisonicSpeakerGeometrySession::getDraft() const noexcept
{
    return draft.has_value() ? &*draft : nullptr;
}

std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
AmbisonicSpeakerGeometrySession::getCandidate() const noexcept
{
    return candidate;
}

std::shared_ptr<const AmbisonicDecodeMatrixArtifact>
AmbisonicSpeakerGeometrySession::getActive() const noexcept
{
    return active;
}
} // namespace singleeditor
