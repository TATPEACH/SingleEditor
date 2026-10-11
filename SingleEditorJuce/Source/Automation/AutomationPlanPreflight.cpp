#include "Automation/AutomationPlanPreflight.h"

#include "Automation/AutomationPlanIO.h"
#include "Automation/EditPipelineAnalysisService.h"
#include "Automation/FileIdentity.h"

namespace singleeditor
{
namespace
{
juce::DynamicObject* makeSummary(int total, int ready, int failed)
{
    auto* summary = new juce::DynamicObject();
    summary->setProperty("total", total);
    summary->setProperty("ready", ready);
    summary->setProperty("failed", failed);
    return summary;
}

juce::DynamicObject* makePreflight(bool ready)
{
    auto* preflight = new juce::DynamicObject();
    preflight->setProperty("schema", "single-editor.preflight.v1");
    preflight->setProperty("schema_version", 1);
    preflight->setProperty("ready", ready);
    preflight->setProperty("write_performed", false);
    preflight->setProperty("output_policy", "not_evaluated_read_only_stage");
    return preflight;
}

bool samePath(const juce::String& left, const juce::String& right)
{
#if JUCE_WINDOWS
    return left.equalsIgnoreCase(right);
#else
    return left == right;
#endif
}

AutomationPlanPreflightResult invalidPlan(const juce::String& error)
{
    AutomationPlanPreflightResult result;
    result.errorMessage = error;
    auto* payload = new juce::DynamicObject();
    payload->setProperty("preflight", juce::var(makePreflight(false)));
    juce::Array<juce::var> items;
    payload->setProperty("results", juce::var(items));
    payload->setProperty("summary", juce::var(makeSummary(0, 0, 0)));
    result.payload = juce::var(payload);
    return result;
}
}

AutomationPlanPreflightResult AutomationPlanPreflight::run(
    const juce::File& planFile)
{
    juce::var plan;
    juce::String error;
    if (!readAutomationPlan(planFile, plan, error))
        return invalidPlan(error);

    const auto* root = plan.getDynamicObject();
    if (root == nullptr
        || root->getProperty("schema").toString()
               != "single-editor.analysis-plan.v1"
        || static_cast<int>(root->getProperty("schema_version")) != 1)
    {
        return invalidPlan("Unsupported analysis plan schema");
    }

    const auto operation = root->getProperty("operation");
    const auto operationId = operation["id"].toString();
    const juce::StringArray supportedOperations {
        "trim-silence.v1", "gain.v1", "fade.v1",
        "normalize.peak.v1", "edit-pipeline.v1"
    };
    if (!supportedOperations.contains(operationId)
        || static_cast<int>(operation["schema_version"]) != 1)
    {
        return invalidPlan("Unsupported plan operation");
    }

    const auto manifest = root->getProperty("manifest");
    const auto manifestStatus = manifest["status"].toString();
    if (manifest["schema"].toString()
            != "single-editor.automation-manifest.v1"
        || static_cast<int>(manifest["schema_version"]) != 1
        || !juce::StringArray { "planned", "partial" }.contains(manifestStatus))
    {
        return invalidPlan("Unsupported automation manifest");
    }

    const auto itemsValue = root->getProperty("items");
    const auto* items = itemsValue.getArray();
    if (items == nullptr || items->isEmpty())
        return invalidPlan("Plan contains no items");

    AutomationPlanPreflightResult result;
    result.operationId = operationId;
    juce::Array<juce::var> itemResults;
    int readyCount = 0;
    int staleCount = 0;
    int invalidCount = 0;
    juce::StringArray seenInputs;

    for (const auto& itemValue : *items)
    {
        auto* itemResult = new juce::DynamicObject();
        const auto inputPath = itemValue["input"].toString();
        itemResult->setProperty("input", inputPath);

        FileIdentity expected;
        juce::String identityError;
        const auto input = juce::File(inputPath);
        const auto identityValid = FileIdentity::fromJson(
            itemValue["input_identity"], expected, identityError);
        const auto canonicalKey = identityValid
                                ?
#if JUCE_WINDOWS
                                  expected.canonicalPath.toLowerCase()
#else
                                  expected.canonicalPath
#endif
                                : juce::String();
        const auto range = itemValue["suggested_sample_range"];
        const auto media = itemValue["media"];
        const auto rangeStart = static_cast<juce::int64>(range["start_sample"]);
        const auto rangeEnd = static_cast<juce::int64>(range["end_sample"]);
        const auto sourceSamples = static_cast<juce::int64>(media["num_samples"]);
        juce::String pipelineError;
        const auto resolvedPipelineValid = operationId == "trim-silence.v1"
            || EditPipelineAnalysisService::validateResolvedPipeline(
                   itemValue["render_pipeline"], sourceSamples, pipelineError);
        if (inputPath.isEmpty()
            || !identityValid
            || !samePath(input.getFullPathName(), expected.canonicalPath)
            || !juce::StringArray { "planned", "skipped" }
                    .contains(itemValue["status"].toString())
            || !range.isObject()
            || !media.isObject()
            || rangeStart < 0
            || rangeEnd < rangeStart
            || sourceSamples < 0
            || rangeEnd > sourceSamples
            || !resolvedPipelineValid
            || seenInputs.contains(canonicalKey))
        {
            itemResult->setProperty("status", "invalid");
            itemResult->setProperty("ready", false);
            itemResult->setProperty("error_code", "INVALID_PLAN");
            itemResult->setProperty(
                "error", identityError.isNotEmpty()
                           ? identityError
                           : pipelineError.isNotEmpty()
                               ? pipelineError
                               : "Plan item failed structural validation");
            ++invalidCount;
            itemResults.add(juce::var(itemResult));
            continue;
        }
        seenInputs.add(canonicalKey);

        itemResult->setProperty("expected_identity", expected.toJson());
        if (!input.existsAsFile())
        {
            itemResult->setProperty("status", "stale");
            itemResult->setProperty("ready", false);
            itemResult->setProperty("error_code", "STALE_INPUT");
            itemResult->setProperty("error", "Input file no longer exists");
            ++staleCount;
            itemResults.add(juce::var(itemResult));
            continue;
        }

        const auto current = FileIdentity::capture(input);
        itemResult->setProperty("current_identity", current.toJson());
        if (!expected.matches(current))
        {
            itemResult->setProperty("status", "stale");
            itemResult->setProperty("ready", false);
            itemResult->setProperty("error_code", "STALE_INPUT");
            itemResult->setProperty("error", "Input identity changed after analysis");
            ++staleCount;
        }
        else
        {
            itemResult->setProperty("status", "ready");
            itemResult->setProperty("ready", true);
            itemResult->setProperty("error_code", juce::var());
            itemResult->setProperty("error", juce::var());
            ++readyCount;
        }
        itemResults.add(juce::var(itemResult));
    }

    const auto failedCount = staleCount + invalidCount;
    const auto allReady = failedCount == 0;
    if (allReady)
        result.exitCode = AutomationExitCode::success;
    else if (readyCount > 0)
        result.exitCode = AutomationExitCode::partialFailure;
    else if (invalidCount > 0)
        result.exitCode = AutomationExitCode::invalidArguments;
    else
        result.exitCode = AutomationExitCode::staleInput;

    if (!allReady)
        result.errorMessage = invalidCount > 0
                                  ? "Plan preflight found invalid items"
                                  : "Plan preflight found stale inputs";

    auto* payload = new juce::DynamicObject();
    payload->setProperty("preflight", juce::var(makePreflight(allReady)));
    payload->setProperty("results", juce::var(itemResults));
    payload->setProperty("summary",
                         juce::var(makeSummary(items->size(),
                                               readyCount,
                                               failedCount)));
    result.payload = juce::var(payload);
    return result;
}
} // namespace singleeditor
