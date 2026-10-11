#include "Automation/BuiltInOperations.h"

#include "Automation/AudioInspectionService.h"
#include "Automation/EditPipelineAnalysisService.h"
#include "Automation/OperationRegistry.h"
#include "Automation/RoutingInspectionService.h"
#include "Automation/TrimSilenceAnalysisService.h"

#include <memory>
#include <utility>

#ifndef SINGLE_EDITOR_CLI_VERSION
#define SINGLE_EDITOR_CLI_VERSION "development"
#endif

namespace singleeditor
{
namespace
{
AutomationExitCode batchExitCode(int succeeded, int failed)
{
    if (failed == 0)
        return AutomationExitCode::success;
    if (succeeded == 0)
        return AutomationExitCode::processingFailed;
    return AutomationExitCode::partialFailure;
}

juce::var makePayload(juce::Array<juce::var> results,
                      int succeeded,
                      int failed)
{
    auto* payload = new juce::DynamicObject();
    payload->setProperty("results", juce::var(results));

    auto* summary = new juce::DynamicObject();
    summary->setProperty("total", succeeded + failed);
    summary->setProperty("succeeded", succeeded);
    summary->setProperty("failed", failed);
    payload->setProperty("summary", juce::var(summary));
    return juce::var(payload);
}

juce::var inspectParameterSchema()
{
    auto* schema = new juce::DynamicObject();
    schema->setProperty("type", "object");

    auto* properties = new juce::DynamicObject();
    auto* sampleStats = new juce::DynamicObject();
    sampleStats->setProperty("type", "boolean");
    sampleStats->setProperty("default", false);
    properties->setProperty("sample_stats", juce::var(sampleStats));
    schema->setProperty("properties", juce::var(properties));
    return juce::var(schema);
}

juce::var trimSilenceParameterSchema()
{
    auto* schema = new juce::DynamicObject();
    schema->setProperty("type", "object");
    auto* properties = new juce::DynamicObject();

    auto* headOnly = new juce::DynamicObject();
    headOnly->setProperty("type", "boolean");
    headOnly->setProperty("default", true);
    properties->setProperty("head_only", juce::var(headOnly));

    auto* threshold = new juce::DynamicObject();
    threshold->setProperty("type", "number");
    threshold->setProperty("minimum", -160.0);
    threshold->setProperty("maximum", 0.0);
    threshold->setProperty("default", -60.0);
    properties->setProperty("threshold_dbfs", juce::var(threshold));
    schema->setProperty("properties", juce::var(properties));
    return juce::var(schema);
}

juce::var numberParameterSchema(const juce::String& name,
                                double minimum,
                                double maximum,
                                double defaultValue)
{
    auto* schema = new juce::DynamicObject();
    schema->setProperty("type", "object");
    auto* properties = new juce::DynamicObject();
    auto* value = new juce::DynamicObject();
    value->setProperty("type", "number");
    value->setProperty("minimum", minimum);
    value->setProperty("maximum", maximum);
    value->setProperty("default", defaultValue);
    properties->setProperty(name, juce::var(value));
    schema->setProperty("properties", juce::var(properties));
    return juce::var(schema);
}

juce::var fadeParameterSchema()
{
    auto* schema = new juce::DynamicObject();
    schema->setProperty("type", "object");
    auto* properties = new juce::DynamicObject();
    for (const auto& name : { "fade_in_seconds", "fade_out_seconds" })
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("type", "number");
        value->setProperty("minimum", 0.0);
        value->setProperty("default", 0.0);
        properties->setProperty(name, juce::var(value));
    }
    schema->setProperty("properties", juce::var(properties));
    return juce::var(schema);
}

juce::var pipelineParameterSchema()
{
    auto* schema = new juce::DynamicObject();
    schema->setProperty("type", "object");
    auto* properties = new juce::DynamicObject();
    auto* pipeline = new juce::DynamicObject();
    pipeline->setProperty("type", "object");
    pipeline->setProperty("schema", "single-editor.edit-pipeline.v1");
    properties->setProperty("pipeline", juce::var(pipeline));
    schema->setProperty("properties", juce::var(properties));
    return juce::var(schema);
}

juce::var makeRequestedPipeline(const juce::String& operationId,
                                const juce::var& parameters)
{
    if (operationId == "edit-pipeline.v1")
        return parameters.getProperty("pipeline", {});

    juce::Array<juce::var> operations;
    if (operationId == "gain.v1")
    {
        auto* operation = new juce::DynamicObject();
        operation->setProperty("id", "gain.v1");
        operation->setProperty("gainDb",
                               parameters.getProperty("gain_db", 0.0));
        operations.add(juce::var(operation));
    }
    else if (operationId == "fade.v1")
    {
        const auto addFade = [&operations](const char* direction, double duration)
        {
            if (duration <= 0.0)
                return;
            auto* operation = new juce::DynamicObject();
            operation->setProperty("id", "fade.v1");
            operation->setProperty("direction", direction);
            operation->setProperty("durationSeconds", duration);
            operations.add(juce::var(operation));
        };
        addFade("in", static_cast<double>(
            parameters.getProperty("fade_in_seconds", 0.0)));
        addFade("out", static_cast<double>(
            parameters.getProperty("fade_out_seconds", 0.0)));
    }
    else if (operationId == "normalize.peak.v1")
    {
        auto* operation = new juce::DynamicObject();
        operation->setProperty("id", "normalize.peak.v1");
        operation->setProperty("targetDbfs",
                               parameters.getProperty("target_dbfs", -1.0));
        operations.add(juce::var(operation));
    }

    auto* pipeline = new juce::DynamicObject();
    pipeline->setProperty("schema", "single-editor.edit-pipeline.v1");
    pipeline->setProperty("schemaVersion", 1);
    pipeline->setProperty("operations", juce::var(operations));
    return juce::var(pipeline);
}

juce::var makeEditAnalysisPayload(const juce::String& operationId,
                                  const juce::var& canonicalPipeline,
                                  juce::Array<juce::var> items,
                                  int succeeded,
                                  int failed)
{
    auto* operation = new juce::DynamicObject();
    operation->setProperty("id", operationId);
    operation->setProperty("schema_version", 1);
    operation->setProperty("parameters", canonicalPipeline);

    auto* manifest = new juce::DynamicObject();
    manifest->setProperty("schema", "single-editor.automation-manifest.v1");
    manifest->setProperty("schema_version", 1);
    manifest->setProperty("cli_version", SINGLE_EDITOR_CLI_VERSION);
    manifest->setProperty("run_id", juce::Uuid().toString());
    manifest->setProperty("created_at",
                          juce::Time::getCurrentTime().toISO8601(true));
    manifest->setProperty("status",
                          failed == 0 ? "planned"
                                      : succeeded == 0 ? "failed" : "partial");

    auto* plan = new juce::DynamicObject();
    plan->setProperty("schema", "single-editor.analysis-plan.v1");
    plan->setProperty("schema_version", 1);
    plan->setProperty("manifest", juce::var(manifest));
    plan->setProperty("operation", juce::var(operation));
    plan->setProperty("items", juce::var(items));
    auto* summary = new juce::DynamicObject();
    summary->setProperty("total", succeeded + failed);
    summary->setProperty("succeeded", succeeded);
    summary->setProperty("failed", failed);
    plan->setProperty("summary", juce::var(summary));

    auto payload = makePayload(items, succeeded, failed);
    payload.getDynamicObject()->setProperty("plan", juce::var(plan));
    return payload;
}

class InspectOperation final : public AudioOperation
{
public:
    AudioOperationDescriptor descriptor() const override
    {
        return { "audio.inspect.v1",
                 1,
                 "Inspect audio",
                 AudioOperationMode::inspect,
                 inspectParameterSchema() };
    }

    AudioOperationResult execute(const AudioOperationRequest& request) const override
    {
        if (request.inputs.isEmpty())
            return { AutomationExitCode::invalidArguments,
                     {},
                     "inspect requires input" };

        const auto scan = static_cast<bool>(
            request.parameters.getProperty("sample_stats", false));
        juce::Array<juce::var> results;
        int succeeded = 0;
        int failed = 0;
        for (const auto& input : request.inputs)
        {
            const auto inspection = AudioInspectionService::inspect(input,
                                                                     { scan, 65536 });
            const auto ok = inspection.opened
                         && inspection.errorMessage.isEmpty()
                         && (!scan || inspection.sampleScanCompleted);
            auto item = inspection.toJson();
            if (auto* object = item.getDynamicObject())
                object->setProperty("ok", ok);
            ok ? ++succeeded : ++failed;
            results.add(std::move(item));
        }

        const auto code = batchExitCode(succeeded, failed);
        return { code,
                 makePayload(std::move(results), succeeded, failed),
                 failed == 0
                     ? juce::String()
                     : "One or more inputs could not be inspected" };
    }
};

class ValidateOperation final : public AudioOperation
{
public:
    AudioOperationDescriptor descriptor() const override
    {
        auto* schema = new juce::DynamicObject();
        schema->setProperty("type", "object");
        schema->setProperty("properties", juce::var(new juce::DynamicObject()));
        return { "audio.validate.v1",
                 1,
                 "Validate audio",
                 AudioOperationMode::inspect,
                 juce::var(schema) };
    }

    AudioOperationResult execute(const AudioOperationRequest& request) const override
    {
        if (request.inputs.isEmpty())
            return { AutomationExitCode::invalidArguments,
                     {},
                     "validate requires input" };

        juce::Array<juce::var> results;
        int succeeded = 0;
        int failed = 0;
        for (const auto& input : request.inputs)
        {
            const auto inspection = AudioInspectionService::inspect(input,
                                                                     { true, 65536 });
            const auto ok = inspection.opened
                         && inspection.sampleScanCompleted
                         && inspection.errorMessage.isEmpty()
                         && !inspection.hasNonFiniteSamples();
            auto item = inspection.toJson();
            if (auto* object = item.getDynamicObject())
            {
                object->setProperty("ok", ok);
                if (!ok
                    && inspection.errorMessage.isEmpty()
                    && inspection.hasNonFiniteSamples())
                {
                    object->setProperty("error", "Non-finite samples found");
                }
            }
            ok ? ++succeeded : ++failed;
            results.add(std::move(item));
        }

        const auto code = batchExitCode(succeeded, failed);
        return { code,
                 makePayload(std::move(results), succeeded, failed),
                 failed == 0
                     ? juce::String()
                     : "One or more inputs failed validation" };
    }
};

class TrimSilenceAnalyzeOperation final : public AudioOperation
{
public:
    AudioOperationDescriptor descriptor() const override
    {
        return { "trim-silence.v1",
                 1,
                 "Analyze leading silence",
                 AudioOperationMode::analyze,
                 trimSilenceParameterSchema() };
    }

    AudioOperationResult execute(const AudioOperationRequest& request) const override
    {
        if (request.inputs.isEmpty())
            return { AutomationExitCode::invalidArguments,
                     {},
                     "analyze trim-silence requires input" };

        TrimSilenceAnalysisOptions options;
        options.headOnly = static_cast<bool>(
            request.parameters.getProperty("head_only", true));
        options.thresholdDbfs = static_cast<double>(
            request.parameters.getProperty("threshold_dbfs", -60.0));

        juce::Array<juce::var> results;
        int succeeded = 0;
        int failed = 0;
        for (const auto& input : request.inputs)
        {
            const auto analysis = TrimSilenceAnalysisService::analyze(input, options);
            analysis.succeeded ? ++succeeded : ++failed;
            results.add(analysis.toPlanItem(options));
        }

        auto* operation = new juce::DynamicObject();
        operation->setProperty("id", "trim-silence.v1");
        operation->setProperty("schema_version", 1);
        auto* parameters = new juce::DynamicObject();
        parameters->setProperty("head_only", options.headOnly);
        parameters->setProperty("threshold_dbfs", options.thresholdDbfs);
        parameters->setProperty("window_ms", options.windowMilliseconds);
        parameters->setProperty("confirmation_ms", options.confirmationMilliseconds);
        parameters->setProperty("hysteresis_db", options.hysteresisDb);
        parameters->setProperty("preroll_ms", options.prerollMilliseconds);
        operation->setProperty("parameters", juce::var(parameters));

        auto* manifest = new juce::DynamicObject();
        manifest->setProperty("schema", "single-editor.automation-manifest.v1");
        manifest->setProperty("schema_version", 1);
        manifest->setProperty("cli_version", SINGLE_EDITOR_CLI_VERSION);
        manifest->setProperty("run_id", juce::Uuid().toString());
        manifest->setProperty("created_at",
                              juce::Time::getCurrentTime().toISO8601(true));
        manifest->setProperty("status",
                              failed == 0 ? "planned"
                                          : succeeded == 0 ? "failed" : "partial");

        auto* plan = new juce::DynamicObject();
        plan->setProperty("schema", "single-editor.analysis-plan.v1");
        plan->setProperty("schema_version", 1);
        plan->setProperty("manifest", juce::var(manifest));
        plan->setProperty("operation", juce::var(operation));
        plan->setProperty("items", juce::var(results));
        auto* planSummary = new juce::DynamicObject();
        planSummary->setProperty("total", succeeded + failed);
        planSummary->setProperty("succeeded", succeeded);
        planSummary->setProperty("failed", failed);
        plan->setProperty("summary", juce::var(planSummary));

        auto payload = makePayload(results, succeeded, failed);
        if (auto* payloadObject = payload.getDynamicObject())
            payloadObject->setProperty("plan", juce::var(plan));

        const auto code = batchExitCode(succeeded, failed);
        return { code,
                 std::move(payload),
                 failed == 0
                     ? juce::String()
                     : "One or more inputs could not be analyzed" };
    }
};

class RoutingInspectOperation final : public AudioOperation
{
public:
    AudioOperationDescriptor descriptor() const override
    {
        auto* schema = new juce::DynamicObject();
        schema->setProperty("type", "object");
        auto* properties = new juce::DynamicObject();

        auto* outputChannels = new juce::DynamicObject();
        outputChannels->setProperty("type", "integer");
        juce::Array<juce::var> supportedOutputChannels;
        for (const auto count : { 1, 2, 4, 6, 8, 12 })
            supportedOutputChannels.add(count);
        outputChannels->setProperty("enum", juce::var(supportedOutputChannels));
        properties->setProperty("output_channels", juce::var(outputChannels));

        auto* sourcePreset = new juce::DynamicObject();
        sourcePreset->setProperty("type", "string");
        sourcePreset->setProperty("default", "auto");
        properties->setProperty("source_preset", juce::var(sourcePreset));

        auto* includeLfe = new juce::DynamicObject();
        includeLfe->setProperty("type", "boolean");
        includeLfe->setProperty("default", false);
        properties->setProperty("include_lfe", juce::var(includeLfe));

        const auto addArrayProperty = [properties](const juce::Identifier& name,
                                                   const juce::String& itemType)
        {
            auto* value = new juce::DynamicObject();
            value->setProperty("type", "array");
            auto* items = new juce::DynamicObject();
            items->setProperty("type", itemType);
            value->setProperty("items", juce::var(items));
            properties->setProperty(name, juce::var(value));
        };
        addArrayProperty("output_map", "string");
        addArrayProperty("mute_channels", "integer");
        addArrayProperty("solo_channels", "integer");

        auto* monitorMode = new juce::DynamicObject();
        monitorMode->setProperty("type", "string");
        juce::Array<juce::var> monitorModes { "auto", "multichannel", "stereo" };
        monitorMode->setProperty("enum", juce::var(monitorModes));
        monitorMode->setProperty("default", "auto");
        properties->setProperty("monitor_mode", juce::var(monitorMode));

        auto* ambisonicMonitor = new juce::DynamicObject();
        ambisonicMonitor->setProperty("type", "string");
        juce::Array<juce::var> ambisonicMonitorModes { "speaker", "binaural" };
        ambisonicMonitor->setProperty("enum", juce::var(ambisonicMonitorModes));
        ambisonicMonitor->setProperty("default", "speaker");
        properties->setProperty("ambisonic_monitor", juce::var(ambisonicMonitor));

        const auto addOrientationProperty = [properties](const juce::Identifier& name,
                                                         double minimum,
                                                         double maximum)
        {
            auto* value = new juce::DynamicObject();
            value->setProperty("type", "number");
            value->setProperty("minimum", minimum);
            value->setProperty("maximum", maximum);
            value->setProperty("default", 0.0);
            properties->setProperty(name, juce::var(value));
        };
        addOrientationProperty("ambisonic_yaw", -180.0, 180.0);
        addOrientationProperty("ambisonic_pitch", -90.0, 90.0);
        addOrientationProperty("ambisonic_roll", -180.0, 180.0);

        auto* runtimeProbe = new juce::DynamicObject();
        runtimeProbe->setProperty("type", "boolean");
        runtimeProbe->setProperty("default", false);
        properties->setProperty("runtime_probe", juce::var(runtimeProbe));

        auto* probeStartSeconds = new juce::DynamicObject();
        probeStartSeconds->setProperty("type", "number");
        probeStartSeconds->setProperty("minimum", 0.0);
        probeStartSeconds->setProperty("default", 0.0);
        properties->setProperty("probe_start_seconds", juce::var(probeStartSeconds));
        schema->setProperty("properties", juce::var(properties));
        return { "audio.routing.inspect.v1",
                 1,
                 "Inspect audio routing",
                 AudioOperationMode::inspect,
                 juce::var(schema) };
    }

    AudioOperationResult execute(const AudioOperationRequest& request) const override
    {
        if (request.inputs.isEmpty())
            return { AutomationExitCode::invalidArguments,
                     {},
                     "routing requires input" };

        RoutingInspectionOptions options;
        options.outputChannels = static_cast<int>(
            request.parameters.getProperty("output_channels", 0));
        options.sourcePresetId = request.parameters.getProperty("source_preset", {}).toString();
        options.includeLfe = static_cast<bool>(
            request.parameters.getProperty("include_lfe", false));
        options.runtimeProbe = static_cast<bool>(
            request.parameters.getProperty("runtime_probe", false));
        options.probeStartSeconds = static_cast<double>(
            request.parameters.getProperty("probe_start_seconds", 0.0));
        options.ambisonicOrientation.yawDegrees = static_cast<float>(
            request.parameters.getProperty("ambisonic_yaw", 0.0));
        options.ambisonicOrientation.pitchDegrees = static_cast<float>(
            request.parameters.getProperty("ambisonic_pitch", 0.0));
        options.ambisonicOrientation.rollDegrees = static_cast<float>(
            request.parameters.getProperty("ambisonic_roll", 0.0));
        if (const auto* parameters = request.parameters.getDynamicObject())
        {
            options.ambisonicOrientationSpecified =
                (parameters->hasProperty("ambisonic_yaw")
                 || parameters->hasProperty("ambisonic_pitch")
                 || parameters->hasProperty("ambisonic_roll"))
                && !options.ambisonicOrientation.isIdentity();
        }
        if (const auto* mappings = request.parameters.getProperty("output_map", {}).getArray())
        {
            for (const auto& mapping : *mappings)
                options.outputMappings.add(mapping.toString());
        }
        if (const auto* muted = request.parameters.getProperty("mute_channels", {}).getArray())
        {
            for (const auto& channel : *muted)
                options.mutedSourceChannels.add(static_cast<int>(channel));
        }
        if (const auto* soloed = request.parameters.getProperty("solo_channels", {}).getArray())
        {
            for (const auto& channel : *soloed)
                options.soloedSourceChannels.add(static_cast<int>(channel));
        }

        const auto monitorMode = request.parameters.getProperty("monitor_mode", "auto")
                                     .toString()
                                     .toLowerCase();
        if (monitorMode == "multichannel")
            options.monitorMode = MonitorMode::forceMultichannel;
        else if (monitorMode == "stereo")
            options.monitorMode = MonitorMode::forceStereoDownmix;
        else if (monitorMode != "auto")
            return { AutomationExitCode::invalidArguments, {}, "Unknown monitor mode" };
        const auto ambisonicMonitorMode = request.parameters
                                              .getProperty("ambisonic_monitor", "speaker")
                                              .toString()
                                              .toLowerCase();
        if (ambisonicMonitorMode == "binaural")
            options.ambisonicMonitorMode = AmbisonicMonitorMode::binauralHrtf;
        else if (ambisonicMonitorMode != "speaker")
            return { AutomationExitCode::invalidArguments,
                     {},
                     "Unknown Ambisonic monitor mode" };
        if (!juce::Array<int> { 1, 2, 4, 6, 8, 12 }.contains(options.outputChannels))
            return { AutomationExitCode::invalidArguments,
                     {},
                     "routing requires a supported output channel count" };

        juce::Array<juce::var> results;
        int succeeded = 0;
        int failed = 0;
        for (const auto& input : request.inputs)
        {
            const auto inspection = RoutingInspectionService::inspect(input, options);
            inspection.succeeded() ? ++succeeded : ++failed;
            results.add(inspection.toJson());
        }

        const auto code = batchExitCode(succeeded, failed);
        return { code,
                 makePayload(std::move(results), succeeded, failed),
                 failed == 0
                     ? juce::String()
                     : "One or more inputs failed routing inspection" };
    }
};

class EditAnalyzeOperation final : public AudioOperation
{
public:
    EditAnalyzeOperation(juce::String operationId,
                         juce::String operationName,
                         juce::var schema)
        : id(std::move(operationId)),
          name(std::move(operationName)),
          parameterSchema(std::move(schema))
    {
    }

    AudioOperationDescriptor descriptor() const override
    {
        return { id, 1, name, AudioOperationMode::analyze, parameterSchema };
    }

    AudioOperationResult execute(const AudioOperationRequest& request) const override
    {
        if (request.inputs.isEmpty())
            return { AutomationExitCode::invalidArguments, {},
                     id + " analyze requires input" };

        const auto requestedPipeline = makeRequestedPipeline(id, request.parameters);
        const auto validation = EditPipelineAnalysisService::validate(requestedPipeline);
        if (!validation.succeeded)
            return { AutomationExitCode::invalidArguments, {},
                     validation.errorMessage };

        juce::Array<juce::var> items;
        int succeeded = 0;
        int failed = 0;
        for (const auto& input : request.inputs)
        {
            const auto analysis = EditPipelineAnalysisService::analyze(
                input, validation.canonicalPipeline);
            analysis.succeeded ? ++succeeded : ++failed;
            items.add(analysis.toPlanItem());
        }

        const auto code = batchExitCode(succeeded, failed);
        return { code,
                 makeEditAnalysisPayload(id, validation.canonicalPipeline,
                                         std::move(items), succeeded, failed),
                 failed == 0
                     ? juce::String()
                     : "One or more inputs could not be analyzed" };
    }

private:
    juce::String id;
    juce::String name;
    juce::var parameterSchema;
};
} // namespace

void registerBuiltInReadOnlyOperations(OperationRegistry& registry)
{
    const auto inspectAdded = registry.add(std::make_unique<InspectOperation>());
    const auto validateAdded = registry.add(std::make_unique<ValidateOperation>());
    const auto trimSilenceAdded = registry.add(
        std::make_unique<TrimSilenceAnalyzeOperation>());
    const auto routingAdded = registry.add(std::make_unique<RoutingInspectOperation>());
    const auto gainAdded = registry.add(std::make_unique<EditAnalyzeOperation>(
        "gain.v1", "Analyze whole-file gain",
        numberParameterSchema("gain_db", -120.0, 24.0, 0.0)));
    const auto fadeAdded = registry.add(std::make_unique<EditAnalyzeOperation>(
        "fade.v1", "Analyze head/tail fades", fadeParameterSchema()));
    const auto normalizeAdded = registry.add(std::make_unique<EditAnalyzeOperation>(
        "normalize.peak.v1", "Analyze peak normalization",
        numberParameterSchema("target_dbfs", -60.0, 0.0, -1.0)));
    const auto pipelineAdded = registry.add(std::make_unique<EditAnalyzeOperation>(
        "edit-pipeline.v1", "Analyze versioned edit pipeline",
        pipelineParameterSchema()));
    jassert(inspectAdded && validateAdded && trimSilenceAdded && routingAdded
            && gainAdded && fadeAdded && normalizeAdded && pipelineAdded);
    juce::ignoreUnused(inspectAdded, validateAdded, trimSilenceAdded, routingAdded,
                       gainAdded, fadeAdded, normalizeAdded, pipelineAdded);
}
} // namespace singleeditor
