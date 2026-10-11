#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace singleeditor
{
struct CliColdOperationDeclaration
{
    juce::String id;
    juce::String displayName;
    juce::String operationVersion;
    juce::String algorithmVersion;
    juce::String category;
    juce::String lazyFactoryToken;
};

class CliColdManifest final
{
public:
    static constexpr const char* discoverySchema = "singleeditor.cli.discovery";
    static constexpr const char* discoverySchemaVersion = "1.0.0";
    static constexpr const char* eventSchema = "singleeditor.cli.event";
    static constexpr const char* eventProtocolVersion = "1.0.0";
    static constexpr const char* planSchema = "singleeditor.cli.single-file-plan";
    static constexpr const char* planSchemaVersion = "1.0.0";
    static constexpr const char* batchPlanSchema = "singleeditor.cli.batch-plan";
    static constexpr const char* batchPlanSchemaVersion = "1.0.0";
    static constexpr const char* batchEvidenceSchema = "singleeditor.cli.batch-evidence";
    static constexpr const char* batchEvidenceSchemaVersion = "1.0.0";
    static constexpr int maximumBatchItems = 256;
    static constexpr int maximumBatchConcurrency = 1;

    static const CliColdOperationDeclaration& referenceGain() noexcept;
    static const CliColdOperationDeclaration& parametricEq() noexcept;
    static const CliColdOperationDeclaration& truePeakLimiter() noexcept;
    static const std::array<CliColdOperationDeclaration, 3>& operations() noexcept;
    static const CliColdOperationDeclaration* findOperation(
        const juce::String& idOrAlias) noexcept;
    static juce::var createDiscoveryPayload();
    static juce::String manifestSha256();
};
} // namespace singleeditor
