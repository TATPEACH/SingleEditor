#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

#include "Audio/ChannelRouter.h"

namespace singleeditor
{
enum class SpeakerGeometryDimensionality
{
    ring2D = 0,
    sphere3D
};

enum class SpeakerGeometryWeightPolicy
{
    unity = 0,
    explicitStatic
};

enum class SpeakerGeometryDiagnosticSeverity
{
    warning = 0,
    error
};

struct SpeakerGeometryDiagnostic
{
    SpeakerGeometryDiagnosticSeverity severity = SpeakerGeometryDiagnosticSeverity::error;
    juce::String code;
    juce::String message;
    std::vector<juce::String> speakerIds;
};

struct SpeakerGeometrySpeaker
{
    juce::String stableId;
    juce::String label;
    SpeakerRole role = SpeakerRole::unknown;
    float azimuthDegrees = 0.0f;
    float elevationDegrees = 0.0f;
    float distanceMetres = 1.0f;
    float staticWeight = 1.0f;
    bool enabled = true;
    bool available = true;
    juce::var opaqueSource;
};

struct SpeakerGeometryProfile
{
    static constexpr int schemaVersion = 1;

    juce::String profileId;
    int64_t revision = 1;
    juce::String displayName;
    SpeakerGeometryDimensionality dimensionality = SpeakerGeometryDimensionality::ring2D;
    SpeakerGeometryWeightPolicy weightPolicy = SpeakerGeometryWeightPolicy::unity;
    juce::String generatorBackendId = "singleeditor-sampled-maxre";
    juce::String generatorVersion = "1.0.0";
    juce::String parentTemplateId;
    bool builtInReadOnly = false;
    std::vector<SpeakerGeometrySpeaker> speakers;
    juce::var opaqueSource;
};

struct SpeakerGeometryProfileParseResult
{
    bool success = false;
    SpeakerGeometryProfile profile;
    juce::String errorCode;
    juce::String errorMessage;
};

class SpeakerGeometryProfileCodec final
{
public:
    static juce::var toVar(const SpeakerGeometryProfile& profile);
    static SpeakerGeometryProfileParseResult fromVar(const juce::var& source);
};

struct SpeakerGeometryValidationResult
{
    juce::String profileHash;
    std::vector<SpeakerGeometryDiagnostic> diagnostics;

    bool isValid() const noexcept;
    bool hasWarnings() const noexcept;
};

struct AmbisonicDecodeMatrixArtifact
{
    static constexpr int schemaVersion = 1;

    juce::String artifactId;
    juce::String matrixHash;
    juce::String cacheKey;
    juce::String profileHash;
    juce::String generatorBackendId;
    juce::String generatorVersion;
    juce::String weightProfileId = "max-re-137.9deg-v1";
    juce::String staticNormalizationId = "fixed-reference-v1";
    juce::String channelOrdering = "ACN";
    juce::String ambisonicNormalization = "SN3D";
    int ambisonicOrder = 0;
    bool generatedOffline = false;
    SpeakerGeometryProfile geometry;
    RoutingMatrix decodeMatrix;

    bool isValid() const noexcept;
    ChannelLayoutState canonicalSpeakerBus() const;
};

struct StandardSpeakerGeometryView
{
    const char* stableId = nullptr;
    const char* label = nullptr;
    SpeakerRole role = SpeakerRole::unknown;
    float azimuthDegrees = 0.0f;
    float elevationDegrees = 0.0f;
    float distanceMetres = 1.0f;
    float staticWeight = 1.0f;
    bool enabled = false;
    bool available = false;
};

struct StandardAmbisonicMatrixView
{
    const char* artifactId = nullptr;
    const char* profileId = nullptr;
    const char* profileHash = nullptr;
    const char* matrixHash = nullptr;
    const char* cacheKey = nullptr;
    int ambisonicOrder = 0;
    int outputChannels = 0;
    int sourceChannels = 0;
    SpeakerGeometryDimensionality dimensionality = SpeakerGeometryDimensionality::ring2D;
    std::array<StandardSpeakerGeometryView, 12> speakers;
    const float* gains = nullptr;
};

class AmbisonicStandardMatrixCatalog final
{
public:
    static constexpr int artifactSchemaVersion = 1;

    static std::optional<StandardAmbisonicMatrixView> find(
        const ChannelLayoutState& targetLayout,
        int ambisonicOrder) noexcept;
    static std::shared_ptr<const SpeakerGeometryProfile> standardProfile(
        const juce::String& profileId);
    static std::vector<juce::String> standardProfileIds();
    static bool verifyAllArtifacts(juce::String& failureReason);
    static int getLookupCountForDiagnostics() noexcept;
    static void resetDiagnostics() noexcept;
};

enum class SpeakerGeometryGenerationStatus
{
    ready = 0,
    invalidProfile,
    unsupportedOrder,
    staleRevision,
    generationFailed
};

struct SpeakerGeometryGenerationRequest
{
    std::uint64_t requestId = 0;
    int64_t draftRevision = 0;
    int ambisonicOrder = 0;
    SpeakerGeometryProfile profile;
};

struct SpeakerGeometryGenerationResult
{
    SpeakerGeometryGenerationStatus status = SpeakerGeometryGenerationStatus::generationFailed;
    std::uint64_t requestId = 0;
    int64_t draftRevision = 0;
    SpeakerGeometryValidationResult validation;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> artifact;

    bool succeeded() const noexcept;
};

class AmbisonicSpeakerGeometryGenerator final
{
public:
    static SpeakerGeometryValidationResult validate(const SpeakerGeometryProfile& profile);
    static SpeakerGeometryGenerationResult generate(const SpeakerGeometryGenerationRequest& request);
    static juce::String profileIdentityHash(const SpeakerGeometryProfile& profile);
    static juce::String matrixHash(const RoutingMatrix& matrix);
    static int getGenerationCountForDiagnostics() noexcept;
    static void resetDiagnostics() noexcept;
};

enum class SpeakerGeometrySessionState
{
    empty = 0,
    draft,
    validated,
    generating,
    candidate,
    auditioning,
    active,
    error
};

class AmbisonicSpeakerGeometrySession final
{
public:
    using ActivationCallback =
        std::function<bool(std::shared_ptr<const AmbisonicDecodeMatrixArtifact>)>;

    explicit AmbisonicSpeakerGeometrySession(ActivationCallback activationCallback = {});

    void setDraft(SpeakerGeometryProfile profile);
    SpeakerGeometryValidationResult validateDraft();
    std::optional<SpeakerGeometryGenerationRequest> beginGeneration(int ambisonicOrder);
    SpeakerGeometryGenerationStatus acceptGenerationResult(
        SpeakerGeometryGenerationResult result);
    void cancelPendingGeneration() noexcept;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> beginAudition();
    void endAudition() noexcept;
    bool activateCandidate();

    SpeakerGeometrySessionState getState() const noexcept { return state; }
    const SpeakerGeometryProfile* getDraft() const noexcept;
    const SpeakerGeometryValidationResult& getValidation() const noexcept { return validation; }
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> getCandidate() const noexcept;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> getActive() const noexcept;
    std::size_t getCacheSizeForDiagnostics() const noexcept { return candidateCache.size(); }

private:
    ActivationCallback onActivate;
    SpeakerGeometrySessionState state = SpeakerGeometrySessionState::empty;
    std::optional<SpeakerGeometryProfile> draft;
    SpeakerGeometryValidationResult validation;
    std::uint64_t nextRequestId = 1;
    std::uint64_t pendingRequestId = 0;
    int64_t pendingDraftRevision = 0;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> candidate;
    std::shared_ptr<const AmbisonicDecodeMatrixArtifact> active;
    std::map<std::string, std::shared_ptr<const AmbisonicDecodeMatrixArtifact>> candidateCache;
};
} // namespace singleeditor
