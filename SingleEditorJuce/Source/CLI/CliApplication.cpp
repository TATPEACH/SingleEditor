#include "CLI/CliApplication.h"

#include "Automation/AutomationPlanIO.h"
#include "Automation/AutomationPlanApply.h"
#include "Automation/AutomationPlanPreflight.h"

namespace singleeditor
{
namespace
{
juce::String operationIdFor(CliCommand command)
{
    if (command == CliCommand::inspect)
        return "audio.inspect.v1";
    if (command == CliCommand::analyze)
        return {};
    if (command == CliCommand::validate)
        return "audio.validate.v1";
    if (command == CliCommand::routing)
        return "audio.routing.inspect.v1";
    return {};
}

juce::String operationModeName(AudioOperationMode mode)
{
    switch (mode)
    {
        case AudioOperationMode::inspect: return "inspect";
        case AudioOperationMode::analyze: return "analyze";
        case AudioOperationMode::render: return "render";
    }
    return "inspect";
}

juce::DynamicObject* makeEnvelope(const CliArguments& arguments,
                                  AutomationExitCode exitCode,
                                  const juce::String& operationId,
                                  const juce::String& error)
{
    auto* envelope = new juce::DynamicObject();
    envelope->setProperty("schema", "single-editor.cli.v1");
    envelope->setProperty("command",
                          arguments.requestedCommand.isNotEmpty()
                              ? arguments.requestedCommand
                              : cliCommandName(arguments.command));
    envelope->setProperty("operation",
                          operationId.isEmpty() ? juce::var() : juce::var(operationId));
    envelope->setProperty("ok", exitCode == AutomationExitCode::success);
    envelope->setProperty("exit_code", static_cast<int>(exitCode));
    envelope->setProperty("error",
                          error.isEmpty() ? juce::var() : juce::var(error));
    return envelope;
}

juce::String jsonLine(juce::DynamicObject* object)
{
    return juce::JSON::toString(juce::var(object), true) + "\n";
}
} // namespace

CliExecution executeCli(const CliArguments& arguments,
                        const OperationRegistry& registry)
{
    CliExecution execution;
    if (!arguments.isValid())
    {
        execution.exitCode = arguments.errorCode;
        if (arguments.json)
        {
            execution.stdoutText = jsonLine(
                makeEnvelope(arguments, execution.exitCode, {}, arguments.error));
        }
        else
        {
            execution.stderrText = arguments.error + "\n";
        }
        return execution;
    }

    if (arguments.command == CliCommand::operations)
    {
        const auto descriptors = registry.descriptors();
        if (!arguments.json)
        {
            for (const auto& descriptor : descriptors)
                execution.stdoutText << descriptor.id << "\n";
            return execution;
        }

        auto* envelope = makeEnvelope(arguments,
                                      AutomationExitCode::success,
                                      {},
                                      {});
        juce::Array<juce::var> operations;
        for (const auto& descriptor : descriptors)
        {
            auto* item = new juce::DynamicObject();
            item->setProperty("id", descriptor.id);
            item->setProperty("schema_version", descriptor.schemaVersion);
            item->setProperty("display_name", descriptor.displayName);
            item->setProperty("mode", operationModeName(descriptor.mode));
            item->setProperty("parameters", descriptor.parameterSchema);
            operations.add(juce::var(item));
        }
        envelope->setProperty("operations", juce::var(operations));
        execution.stdoutText = jsonLine(envelope);
        return execution;
    }

    if (arguments.command == CliCommand::apply)
    {
        if (arguments.dryRun)
        {
            const auto preflight = AutomationPlanPreflight::run(arguments.planFile);
            execution.exitCode = preflight.exitCode;
            auto* envelope = makeEnvelope(arguments,
                                          preflight.exitCode,
                                          preflight.operationId,
                                          preflight.errorMessage);
            if (const auto* payload = preflight.payload.getDynamicObject())
            {
                envelope->setProperty("preflight", payload->getProperty("preflight"));
                envelope->setProperty("results", payload->getProperty("results"));
                envelope->setProperty("summary", payload->getProperty("summary"));
            }
            envelope->setProperty("dry_run", true);
            envelope->setProperty("write_performed", false);
            envelope->setProperty("plan_path", arguments.planFile.getFullPathName());
            execution.stdoutText = jsonLine(envelope);
            return execution;
        }

        AutomationPlanApplyOptions options;
        options.inPlace = arguments.inPlace;
        options.outputDirectory = arguments.outputDirectory;
        options.conflictPolicy = arguments.conflictPolicy;
        options.backup = arguments.backup;
        options.backupDirectory = arguments.backupDirectory;
        options.allowLossyReencode = arguments.allowLossyReencode;
        options.failFast = arguments.failFast;
        options.manifestFile = arguments.manifestFile;
        const auto apply = AutomationPlanApply::run(arguments.planFile, options);
        execution.exitCode = apply.exitCode;
        auto* envelope = makeEnvelope(arguments, apply.exitCode,
                                      apply.operationId, apply.errorMessage);
        if (const auto* payload = apply.payload.getDynamicObject())
        {
            envelope->setProperty("manifest_schema", payload->getProperty("schema"));
            envelope->setProperty("output_policy", payload->getProperty("output_policy"));
            envelope->setProperty("write_performed", payload->getProperty("write_performed"));
            envelope->setProperty("results", payload->getProperty("results"));
            envelope->setProperty("summary", payload->getProperty("summary"));
        }
        envelope->setProperty("dry_run", false);
        envelope->setProperty("plan_path", arguments.planFile.getFullPathName());
        execution.stdoutText = jsonLine(envelope);
        return execution;
    }

    const auto operationId = arguments.command == CliCommand::analyze
                           ? arguments.operationId
                           : operationIdFor(arguments.command);
    const auto* operation = registry.find(operationId);
    if (operation == nullptr)
    {
        execution.exitCode = AutomationExitCode::processingFailed;
        execution.stdoutText = jsonLine(makeEnvelope(
            arguments,
            execution.exitCode,
            operationId,
            "Required operation is not registered"));
        return execution;
    }

    auto* parameters = new juce::DynamicObject();
    parameters->setProperty("sample_stats", arguments.sampleStats);
    parameters->setProperty("head_only", arguments.headOnly);
    parameters->setProperty("threshold_dbfs", arguments.thresholdDbfs);
    parameters->setProperty("gain_db", arguments.gainDb);
    parameters->setProperty("fade_in_seconds", arguments.fadeInSeconds);
    parameters->setProperty("fade_out_seconds", arguments.fadeOutSeconds);
    parameters->setProperty("target_dbfs", arguments.targetDbfs);
    if (arguments.pipelineFile != juce::File())
    {
        parameters->setProperty(
            "pipeline",
            juce::JSON::parse(arguments.pipelineFile.loadFileAsString()));
    }
    parameters->setProperty("output_channels", arguments.outputChannels);
    parameters->setProperty("source_preset", arguments.sourcePresetId);
    parameters->setProperty("include_lfe", arguments.includeLfe);
    juce::Array<juce::var> outputMappings;
    for (const auto& mapping : arguments.outputMappings)
        outputMappings.add(mapping);
    parameters->setProperty("output_map", juce::var(outputMappings));

    juce::Array<juce::var> mutedChannels;
    for (const auto channel : arguments.mutedSourceChannels)
        mutedChannels.add(channel);
    parameters->setProperty("mute_channels", juce::var(mutedChannels));

    juce::Array<juce::var> soloedChannels;
    for (const auto channel : arguments.soloedSourceChannels)
        soloedChannels.add(channel);
    parameters->setProperty("solo_channels", juce::var(soloedChannels));
    parameters->setProperty("monitor_mode", arguments.monitorMode);
    parameters->setProperty("ambisonic_monitor", arguments.ambisonicMonitorMode);
    parameters->setProperty("runtime_probe", arguments.runtimeProbe);
    parameters->setProperty("probe_start_seconds", arguments.probeStartSeconds);
    if (arguments.ambisonicOrientationSpecified)
    {
        parameters->setProperty("ambisonic_yaw", arguments.ambisonicOrientation.yawDegrees);
        parameters->setProperty("ambisonic_pitch", arguments.ambisonicOrientation.pitchDegrees);
        parameters->setProperty("ambisonic_roll", arguments.ambisonicOrientation.rollDegrees);
    }
    AudioOperationRequest request { arguments.inputs, juce::var(parameters) };
    const auto result = operation->execute(request);
    auto exitCode = result.exitCode;
    auto errorMessage = result.errorMessage;
    juce::var plan;
    bool planWritten = false;
    if (const auto* payload = result.payload.getDynamicObject())
        plan = payload->getProperty("plan");

    if (arguments.command == CliCommand::analyze
        && plan.isObject()
        && arguments.planFile != juce::File()
        && !arguments.dryRun)
    {
        if (!writeAutomationPlanAtomically(arguments.planFile, plan, errorMessage))
            exitCode = AutomationExitCode::processingFailed;
        else
            planWritten = true;
    }
    execution.exitCode = exitCode;

    auto* envelope = makeEnvelope(arguments,
                                  exitCode,
                                  operationId,
                                  errorMessage);
    if (const auto* payload = result.payload.getDynamicObject())
    {
        envelope->setProperty("results", payload->getProperty("results"));
        envelope->setProperty("summary", payload->getProperty("summary"));
    }
    else
    {
        juce::Array<juce::var> emptyResults;
        envelope->setProperty("results", juce::var(emptyResults));
        auto* summary = new juce::DynamicObject();
        summary->setProperty("total", 0);
        summary->setProperty("succeeded", 0);
        summary->setProperty("failed", 0);
        envelope->setProperty("summary", juce::var(summary));
    }
    if (arguments.command == CliCommand::analyze)
    {
        envelope->setProperty("plan", plan);
        envelope->setProperty("dry_run", arguments.dryRun);
        envelope->setProperty("plan_written", planWritten);
        envelope->setProperty(
            "plan_path",
            arguments.planFile == juce::File()
                ? juce::var()
                : juce::var(arguments.planFile.getFullPathName()));
    }
    execution.stdoutText = jsonLine(envelope);
    return execution;
}
} // namespace singleeditor
