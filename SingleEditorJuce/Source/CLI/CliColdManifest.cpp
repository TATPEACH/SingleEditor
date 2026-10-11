#include "CLI/CliColdManifest.h"

#include <juce_cryptography/juce_cryptography.h>

namespace singleeditor
{
namespace
{
#ifndef SINGLE_EDITOR_CLI_VERSION
#define SINGLE_EDITOR_CLI_VERSION "0.0.0"
#endif

juce::String sha256For(const juce::String& text)
{
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}

juce::var makeParameter(const juce::String& id,
                        const juce::String& type,
                        bool required = true)
{
    auto* parameter = new juce::DynamicObject();
    parameter->setProperty("id", id);
    parameter->setProperty("type", type);
    parameter->setProperty("required", required);
    return juce::var(parameter);
}

juce::Array<juce::var> parameterSchemaFor(const CliColdOperationDeclaration& declaration)
{
    juce::Array<juce::var> parameters;
    if (declaration.id == CliColdManifest::referenceGain().id)
    {
        auto gain = makeParameter("gain_db", "number");
        gain.getDynamicObject()->setProperty("minimum", -120.0);
        gain.getDynamicObject()->setProperty("maximum", 24.0);
        gain.getDynamicObject()->setProperty("unit", "dB");
        parameters.add(gain);
    }
    else if (declaration.id == CliColdManifest::parametricEq().id)
    {
        auto preset = makeParameter("preset_id", "string");
        preset.getDynamicObject()->setProperty(
            "built_in_values",
            juce::var(juce::StringArray { "flat", "vocal-clarity", "bass-control" }));
        parameters.add(preset);

        auto bands = makeParameter("bands", "array");
        bands.getDynamicObject()->setProperty("item_count", 8);
        bands.getDynamicObject()->setProperty(
            "fields",
            juce::var(juce::StringArray { "index", "enabled", "type",
                                          "frequency_hz", "gain_db", "q", "slope" }));
        bands.getDynamicObject()->setProperty(
            "type_values",
            juce::var(juce::StringArray { "high-pass", "low-shelf", "bell",
                                          "notch", "high-shelf", "low-pass" }));
        bands.getDynamicObject()->setProperty(
            "slope_values", juce::var(juce::StringArray { "db12", "db24" }));
        parameters.add(bands);
    }
    else
    {
        parameters.add(makeParameter("enabled", "boolean"));
        auto threshold = makeParameter("threshold_db", "number");
        threshold.getDynamicObject()->setProperty("minimum", -36.0);
        threshold.getDynamicObject()->setProperty("maximum", 0.0);
        parameters.add(threshold);
        auto ceiling = makeParameter("ceiling_dbtp", "number");
        ceiling.getDynamicObject()->setProperty("minimum", -12.0);
        ceiling.getDynamicObject()->setProperty("maximum", 0.0);
        parameters.add(ceiling);
        parameters.add(makeParameter("lookahead_ms", "number"));
        parameters.add(makeParameter("release_mode", "automatic|manual"));
        parameters.add(makeParameter("release_ms", "number"));
        parameters.add(makeParameter("oversampling", "sample-rate-aware-fixed"));
        parameters.add(makeParameter("channels_linked", "boolean"));
    }
    return parameters;
}

juce::var makeOperation(const CliColdOperationDeclaration& declaration)
{
    auto* formats = new juce::DynamicObject();
    formats->setProperty("input", juce::var(juce::StringArray { "wav" }));
    formats->setProperty("output", juce::var(juce::StringArray { "wav" }));
    formats->setProperty("preserve_channel_count", true);
    formats->setProperty("preserve_sample_rate", true);

    auto* targeting = new juce::DynamicObject();
    targeting->setProperty(
        "modes",
        juce::var(juce::StringArray { "all-source-channels",
                                      "explicit-source-channels" }));
    targeting->setProperty("channel_index_base", 0);
    targeting->setProperty("sample_range", "half-open");

    auto* operation = new juce::DynamicObject();
    operation->setProperty("id", declaration.id);
    operation->setProperty("display_name", declaration.displayName);
    operation->setProperty("capability", "audiosuite.offline.single-file");
    operation->setProperty("category", declaration.category);
    operation->setProperty("operation_version", declaration.operationVersion);
    operation->setProperty("algorithm_version", declaration.algorithmVersion);
    operation->setProperty("schema_version", 1);
    operation->setProperty("availability", "available");
    operation->setProperty("lazy_factory_token", declaration.lazyFactoryToken);
    operation->setProperty("parameters", juce::var(parameterSchemaFor(declaration)));
    operation->setProperty("formats", juce::var(formats));
    operation->setProperty("channel_targeting", juce::var(targeting));
    operation->setProperty(
        "destination_authorization",
        juce::var(juce::StringArray { "create_new", "in_place",
                                      "overwrite_existing" }));
    return juce::var(operation);
}

juce::var makeManifestCore()
{
    auto* compatibility = new juce::DynamicObject();
    compatibility->setProperty("core_version", SINGLE_EDITOR_CLI_VERSION);
    compatibility->setProperty("event_protocol", CliColdManifest::eventProtocolVersion);
    compatibility->setProperty("discovery_schema", CliColdManifest::discoverySchemaVersion);
    compatibility->setProperty("plan_schema", CliColdManifest::planSchemaVersion);
    compatibility->setProperty("batch_plan_schema",
                               CliColdManifest::batchPlanSchemaVersion);
    compatibility->setProperty("batch_evidence_schema",
                               CliColdManifest::batchEvidenceSchemaVersion);
    compatibility->setProperty("minimum_event_protocol", "1.0.0");
    compatibility->setProperty("maximum_event_protocol", "1.x");

    juce::Array<juce::var> operations;
    for (const auto& declaration : CliColdManifest::operations())
        operations.add(makeOperation(declaration));

    auto* batch = new juce::DynamicObject();
    batch->setProperty("plan_schema", CliColdManifest::batchPlanSchema);
    batch->setProperty("plan_schema_version", CliColdManifest::batchPlanSchemaVersion);
    batch->setProperty("evidence_schema", CliColdManifest::batchEvidenceSchema);
    batch->setProperty("evidence_schema_version",
                       CliColdManifest::batchEvidenceSchemaVersion);
    batch->setProperty("maximum_items", CliColdManifest::maximumBatchItems);
    batch->setProperty("maximum_concurrency",
                       CliColdManifest::maximumBatchConcurrency);
    batch->setProperty("cancellation_granularity", "file-boundary");
    batch->setProperty("resume_contract", "immutable-plan-and-evidence-bound");

    auto* root = new juce::DynamicObject();
    root->setProperty("schema", CliColdManifest::discoverySchema);
    root->setProperty("schema_version", CliColdManifest::discoverySchemaVersion);
    root->setProperty("compatibility", juce::var(compatibility));
    root->setProperty("operations", juce::var(operations));
    root->setProperty("batch", juce::var(batch));
    return juce::var(root);
}
} // namespace

const CliColdOperationDeclaration& CliColdManifest::referenceGain() noexcept
{
    static const CliColdOperationDeclaration declaration {
        "singleeditor.reference-gain",
        "Reference Gain",
        "1.0.0",
        "reference-gain-linear-v1",
        "channel-targeted",
        "audiosuite.reference-gain.v1"
    };
    return declaration;
}

const CliColdOperationDeclaration& CliColdManifest::parametricEq() noexcept
{
    static const CliColdOperationDeclaration declaration {
        "singleeditor.parametric-eq-v1",
        "Parametric EQ",
        "1.0.0",
        "parametric-eq-minimum-phase-v1",
        "equalization",
        "audiosuite.parametric-eq.v1"
    };
    return declaration;
}

const CliColdOperationDeclaration& CliColdManifest::truePeakLimiter() noexcept
{
    static const CliColdOperationDeclaration declaration {
        "singleeditor.true-peak-limiter-v1",
        "True-Peak Limiter",
        "1.0.0",
        "true-peak-limiter-linked-v1",
        "dynamics",
        "audiosuite.true-peak-limiter.v1"
    };
    return declaration;
}

const std::array<CliColdOperationDeclaration, 3>&
CliColdManifest::operations() noexcept
{
    static const std::array<CliColdOperationDeclaration, 3> declarations {
        referenceGain(), parametricEq(), truePeakLimiter()
    };
    return declarations;
}

const CliColdOperationDeclaration* CliColdManifest::findOperation(
    const juce::String& idOrAlias) noexcept
{
    const auto value = idOrAlias.trim().toLowerCase();
    for (const auto& operation : operations())
    {
        if (value == operation.id.toLowerCase())
            return &operation;
    }
    if (value == "reference-gain") return &operations()[0];
    if (value == "parametric-eq" || value == "eq") return &operations()[1];
    if (value == "true-peak-limiter" || value == "limiter") return &operations()[2];
    return nullptr;
}

juce::String CliColdManifest::manifestSha256()
{
    return sha256For(juce::JSON::toString(makeManifestCore(), true));
}

juce::var CliColdManifest::createDiscoveryPayload()
{
    auto payload = makeManifestCore();
    payload.getDynamicObject()->setProperty("manifest_sha256", manifestSha256());
    return payload;
}
} // namespace singleeditor
