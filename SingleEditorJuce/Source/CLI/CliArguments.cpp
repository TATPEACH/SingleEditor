#include "CLI/CliArguments.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace singleeditor
{
namespace
{
bool parseFiniteFloat(const juce::String& text, float& value) noexcept
{
    const auto trimmed = text.trim();
    const auto* start = trimmed.toRawUTF8();
    char* end = nullptr;
    const auto parsed = std::strtof(start, &end);
    if (end == start || end == nullptr || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

void addInputPath(CliArguments& result, const juce::String& pathText)
{
    const juce::File requested(pathText);
    if (!pathText.containsAnyOf("*?"))
    {
        if (!result.inputs.contains(requested))
            result.inputs.add(requested);
        return;
    }

    auto matches = requested.getParentDirectory().findChildFiles(
        juce::File::findFiles, false, requested.getFileName());
    std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right)
    {
        return left.getFullPathName().compareIgnoreCase(right.getFullPathName()) < 0;
    });
    if (matches.isEmpty())
        matches.add(requested);

    for (const auto& match : matches)
    {
        if (!result.inputs.contains(match))
            result.inputs.add(match);
    }
}
}

juce::String cliCommandName(CliCommand command)
{
    switch (command)
    {
        case CliCommand::operations: return "operations";
        case CliCommand::inspect: return "inspect";
        case CliCommand::analyze: return "analyze";
        case CliCommand::apply: return "apply";
        case CliCommand::validate: return "validate";
        case CliCommand::routing: return "routing";
        case CliCommand::invalid: break;
    }
    return "invalid";
}

CliArguments parseCliArguments(const juce::StringArray& arguments)
{
    CliArguments result;
    result.json = arguments.contains("--json");
    bool analyzeOptionsSpecified = false;
    bool trimOptionSpecified = false;
    bool gainOptionSpecified = false;
    bool fadeOptionSpecified = false;
    bool normalizeOptionSpecified = false;
    bool pipelineOptionSpecified = false;
    bool planOptionsSpecified = false;
    bool applyOptionsSpecified = false;
    if (arguments.isEmpty())
    {
        result.error = "Missing command: expected operations, inspect, analyze, validate, or routing";
        return result;
    }

    const auto command = arguments[0].toLowerCase();
    result.requestedCommand = command;
    if (command == "operations")
        result.command = CliCommand::operations;
    else if (command == "inspect")
        result.command = CliCommand::inspect;
    else if (command == "analyze")
        result.command = CliCommand::analyze;
    else if (command == "apply")
        result.command = CliCommand::apply;
    else if (command == "validate")
        result.command = CliCommand::validate;
    else if (command == "routing")
        result.command = CliCommand::routing;
    else
    {
        result.error = "Unknown command: " + arguments[0];
        return result;
    }

    int firstOptionIndex = 1;
    if (result.command == CliCommand::analyze
        && firstOptionIndex < arguments.size()
        && !arguments[firstOptionIndex].startsWithChar('-'))
    {
        const auto requestedOperation = arguments[firstOptionIndex++].trim().toLowerCase();
        if (requestedOperation == "trim-silence"
            || requestedOperation == "trim-silence.v1")
        {
            result.operationId = "trim-silence.v1";
        }
        else if (requestedOperation == "gain"
                 || requestedOperation == "gain.v1")
        {
            result.operationId = "gain.v1";
        }
        else if (requestedOperation == "fade"
                 || requestedOperation == "fade.v1")
        {
            result.operationId = "fade.v1";
        }
        else if (requestedOperation == "normalize-peak"
                 || requestedOperation == "normalize.peak.v1")
        {
            result.operationId = "normalize.peak.v1";
        }
        else if (requestedOperation == "pipeline"
                 || requestedOperation == "edit-pipeline.v1")
        {
            result.operationId = "edit-pipeline.v1";
        }
        else
        {
            result.errorCode = AutomationExitCode::unsupported;
            result.error = "Unknown analyze operation: " + requestedOperation;
            return result;
        }
    }

    for (int index = firstOptionIndex; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index];
        if (argument == "--json")
        {
            result.json = true;
        }
        else if (argument == "--sample-stats")
        {
            result.sampleStats = true;
        }
        else if (argument == "--head-only")
        {
            result.headOnly = true;
            analyzeOptionsSpecified = true;
            trimOptionSpecified = true;
        }
        else if (argument == "--threshold-dbfs")
        {
            if (++index >= arguments.size())
            {
                result.error = "--threshold-dbfs requires a value";
                return result;
            }

            float threshold = 0.0f;
            if (!parseFiniteFloat(arguments[index], threshold)
                || threshold < -160.0f
                || threshold > 0.0f)
            {
                result.error = "--threshold-dbfs requires a value from -160 to 0";
                return result;
            }
            result.thresholdDbfs = threshold;
            analyzeOptionsSpecified = true;
            trimOptionSpecified = true;
        }
        else if (argument == "--gain-db")
        {
            if (++index >= arguments.size())
            {
                result.error = "--gain-db requires a value";
                return result;
            }
            float gain = 0.0f;
            if (!parseFiniteFloat(arguments[index], gain)
                || gain < -120.0f || gain > 24.0f)
            {
                result.error = "--gain-db requires a value from -120 to 24";
                return result;
            }
            result.gainDb = gain;
            analyzeOptionsSpecified = true;
            gainOptionSpecified = true;
        }
        else if (argument == "--fade-in-seconds"
                 || argument == "--fade-out-seconds")
        {
            if (++index >= arguments.size())
            {
                result.error = argument + " requires a value";
                return result;
            }
            float seconds = 0.0f;
            if (!parseFiniteFloat(arguments[index], seconds) || seconds <= 0.0f)
            {
                result.error = argument + " requires a finite positive value";
                return result;
            }
            if (argument == "--fade-in-seconds")
                result.fadeInSeconds = seconds;
            else
                result.fadeOutSeconds = seconds;
            analyzeOptionsSpecified = true;
            fadeOptionSpecified = true;
        }
        else if (argument == "--target-dbfs")
        {
            if (++index >= arguments.size())
            {
                result.error = "--target-dbfs requires a value";
                return result;
            }
            float target = 0.0f;
            if (!parseFiniteFloat(arguments[index], target)
                || target < -60.0f || target > 0.0f)
            {
                result.error = "--target-dbfs requires a value from -60 to 0";
                return result;
            }
            result.targetDbfs = target;
            analyzeOptionsSpecified = true;
            normalizeOptionSpecified = true;
        }
        else if (argument == "--pipeline")
        {
            if (++index >= arguments.size() || arguments[index].startsWith("--"))
            {
                result.error = "--pipeline requires a JSON file path";
                return result;
            }
            result.pipelineFile = juce::File(arguments[index]);
            analyzeOptionsSpecified = true;
            pipelineOptionSpecified = true;
        }
        else if (argument == "--plan")
        {
            if (++index >= arguments.size()
                || arguments[index].startsWith("--"))
            {
                result.error = "--plan requires a file path";
                return result;
            }
            result.planFile = juce::File(arguments[index]);
            planOptionsSpecified = true;
        }
        else if (argument == "--dry-run")
        {
            result.dryRun = true;
            planOptionsSpecified = true;
        }
        else if (argument == "--in-place")
        {
            result.inPlace = true;
            applyOptionsSpecified = true;
        }
        else if (argument == "--output-dir")
        {
            if (++index >= arguments.size() || arguments[index].startsWith("--"))
            {
                result.error = "--output-dir requires a directory path";
                return result;
            }
            result.outputDirectory = juce::File(arguments[index]);
            applyOptionsSpecified = true;
        }
        else if (argument == "--on-conflict")
        {
            if (++index >= arguments.size())
            {
                result.error = "--on-conflict requires fail, overwrite, skip, or rename";
                return result;
            }
            result.conflictPolicy = arguments[index].trim().toLowerCase();
            if (!juce::StringArray { "fail", "overwrite", "skip", "rename" }
                     .contains(result.conflictPolicy))
            {
                result.error = "--on-conflict requires fail, overwrite, skip, or rename";
                return result;
            }
            applyOptionsSpecified = true;
        }
        else if (argument == "--backup")
        {
            result.backup = true;
            applyOptionsSpecified = true;
        }
        else if (argument == "--backup-dir")
        {
            if (++index >= arguments.size() || arguments[index].startsWith("--"))
            {
                result.error = "--backup-dir requires a directory path";
                return result;
            }
            result.backup = true;
            result.backupDirectory = juce::File(arguments[index]);
            applyOptionsSpecified = true;
        }
        else if (argument == "--allow-lossy-reencode")
        {
            result.allowLossyReencode = true;
            applyOptionsSpecified = true;
        }
        else if (argument == "--fail-fast")
        {
            result.failFast = true;
            applyOptionsSpecified = true;
        }
        else if (argument == "--manifest")
        {
            if (++index >= arguments.size() || arguments[index].startsWith("--"))
            {
                result.error = "--manifest requires a file path";
                return result;
            }
            result.manifestFile = juce::File(arguments[index]);
            applyOptionsSpecified = true;
        }
        else if (argument == "--output-channels")
        {
            if (++index >= arguments.size())
            {
                result.error = "--output-channels requires a value";
                return result;
            }

            const auto value = arguments[index].trim();
            result.outputChannels = value.containsOnly("0123456789")
                                  ? value.getIntValue()
                                  : 0;
        }
        else if (argument == "--source-preset")
        {
            if (++index >= arguments.size())
            {
                result.error = "--source-preset requires a preset id";
                return result;
            }
            result.sourcePresetId = arguments[index].trim();
        }
        else if (argument == "--include-lfe")
        {
            result.includeLfe = true;
        }
        else if (argument == "--output-map")
        {
            if (++index >= arguments.size() || !arguments[index].containsChar('='))
            {
                result.error = "--output-map requires Role=OutputChannel";
                return result;
            }
            result.outputMappings.add(arguments[index].trim());
        }
        else if (argument == "--mute-channel" || argument == "--solo-channel")
        {
            if (++index >= arguments.size()
                || !arguments[index].containsOnly("0123456789"))
            {
                result.error = argument + " requires a one-based source channel";
                return result;
            }

            const auto sourceChannel = arguments[index].getIntValue() - 1;
            if (!juce::isPositiveAndBelow(sourceChannel, 32))
            {
                result.error = argument + " requires a channel from 1 to 32";
                return result;
            }
            auto& channels = argument == "--mute-channel"
                           ? result.mutedSourceChannels
                           : result.soloedSourceChannels;
            if (!channels.contains(sourceChannel))
                channels.add(sourceChannel);
        }
        else if (argument == "--monitor-mode")
        {
            if (++index >= arguments.size())
            {
                result.error = "--monitor-mode requires auto, multichannel, or stereo";
                return result;
            }
            result.monitorMode = arguments[index].trim().toLowerCase();
            if (!juce::StringArray { "auto", "multichannel", "stereo" }.contains(result.monitorMode))
            {
                result.error = "--monitor-mode requires auto, multichannel, or stereo";
                return result;
            }
        }
        else if (argument == "--ambisonic-monitor")
        {
            if (++index >= arguments.size())
            {
                result.error = "--ambisonic-monitor requires speaker or binaural";
                return result;
            }
            result.ambisonicMonitorMode = arguments[index].trim().toLowerCase();
            if (!juce::StringArray { "speaker", "binaural" }.contains(
                    result.ambisonicMonitorMode))
            {
                result.error = "--ambisonic-monitor requires speaker or binaural";
                return result;
            }
        }
        else if (argument == "--ambisonic-yaw"
                 || argument == "--ambisonic-pitch"
                 || argument == "--ambisonic-roll")
        {
            if (++index >= arguments.size())
            {
                result.error = argument + " requires a degree value";
                return result;
            }

            float degrees = 0.0f;
            if (!parseFiniteFloat(arguments[index], degrees))
            {
                result.error = argument + " requires a finite degree value";
                return result;
            }

            const auto limit = argument == "--ambisonic-pitch" ? 90.0f : 180.0f;
            if (degrees < -limit || degrees > limit)
            {
                result.error = argument + " is outside " + juce::String(-limit, 0)
                             + " to " + juce::String(limit, 0) + " degrees";
                return result;
            }

            if (argument == "--ambisonic-yaw")
                result.ambisonicOrientation.yawDegrees = degrees;
            else if (argument == "--ambisonic-pitch")
                result.ambisonicOrientation.pitchDegrees = degrees;
            else
                result.ambisonicOrientation.rollDegrees = degrees;
            result.ambisonicOrientationSpecified = true;
        }
        else if (argument == "--runtime-probe")
        {
            result.runtimeProbe = true;
        }
        else if (argument == "--probe-start-seconds")
        {
            if (++index >= arguments.size())
            {
                result.error = "--probe-start-seconds requires a non-negative value";
                return result;
            }

            float seconds = 0.0f;
            if (!parseFiniteFloat(arguments[index], seconds) || seconds < 0.0f)
            {
                result.error = "--probe-start-seconds requires a finite non-negative value";
                return result;
            }
            result.probeStartSeconds = seconds;
            result.runtimeProbe = true;
        }
        else if (argument == "--input")
        {
            if (++index >= arguments.size()
                || arguments[index].startsWith("--"))
            {
                result.error = "--input requires a file path";
                return result;
            }
            addInputPath(result, arguments[index]);
        }
        else if (argument == "--operation")
        {
            result.errorCode = AutomationExitCode::unsupported;
            result.error = "--operation is not supported by plan apply";
            return result;
        }
        else
        {
            result.error = "Unknown argument: " + argument;
            return result;
        }
    }

    if (result.command == CliCommand::operations && !result.inputs.isEmpty())
        result.error = "operations does not accept --input";
    else if (result.command == CliCommand::apply && !result.inputs.isEmpty())
        result.error = "apply reads inputs from --plan and does not accept --input";
    else if (result.command != CliCommand::operations
             && result.command != CliCommand::apply
             && result.inputs.isEmpty())
        result.error = cliCommandName(result.command) + " requires at least one --input";
    else if (result.command == CliCommand::analyze && result.operationId.isEmpty())
        result.error = "analyze requires an operation id";
    else if (result.command == CliCommand::analyze
             && result.operationId == "gain.v1" && !gainOptionSpecified)
        result.error = "analyze gain requires --gain-db";
    else if (result.command == CliCommand::analyze
             && result.operationId == "fade.v1" && !fadeOptionSpecified)
        result.error = "analyze fade requires --fade-in-seconds or --fade-out-seconds";
    else if (result.command == CliCommand::analyze
             && result.operationId == "edit-pipeline.v1" && !pipelineOptionSpecified)
        result.error = "analyze pipeline requires --pipeline";
    else if (result.command == CliCommand::analyze
             && ((gainOptionSpecified && result.operationId != "gain.v1")
                 || (fadeOptionSpecified && result.operationId != "fade.v1")
                 || (trimOptionSpecified
                     && result.operationId != "trim-silence.v1")
                 || (normalizeOptionSpecified
                     && result.operationId != "normalize.peak.v1")
                 || (pipelineOptionSpecified
                     && result.operationId != "edit-pipeline.v1")))
        result.error = "analyze operation flags do not match the requested operation";
    else if (result.command != CliCommand::inspect && result.sampleStats)
        result.error = "--sample-stats is valid only for inspect";
    else if (result.command != CliCommand::analyze && analyzeOptionsSpecified)
        result.error = "analyze flags are valid only for analyze";
    else if (result.command != CliCommand::analyze
             && result.command != CliCommand::apply
             && planOptionsSpecified)
        result.error = "--plan and --dry-run are valid only for analyze or apply";
    else if (result.command != CliCommand::apply && applyOptionsSpecified)
    {
        result.errorCode = AutomationExitCode::unsupported;
        result.error = "apply output flags are valid only for apply";
    }
    else if (result.command == CliCommand::apply
             && result.planFile == juce::File())
        result.error = "apply requires --plan";
    else if (result.command == CliCommand::apply
             && result.inPlace == (result.outputDirectory != juce::File()))
        result.error = "apply requires exactly one of --in-place or --output-dir";
    else if (result.command == CliCommand::routing
             && !juce::Array<int> { 1, 2, 4, 6, 8, 12 }.contains(result.outputChannels))
        result.error = "routing requires --output-channels 1, 2, 4, 6, 8, or 12";
    else if (result.command != CliCommand::routing
             && (result.outputChannels != 0
                 || result.sourcePresetId.isNotEmpty()
                 || result.includeLfe
                 || !result.outputMappings.isEmpty()
                 || !result.mutedSourceChannels.isEmpty()
                  || !result.soloedSourceChannels.isEmpty()
                  || result.monitorMode != "auto"
                  || result.ambisonicMonitorMode != "speaker"
                  || result.ambisonicOrientationSpecified
                  || result.runtimeProbe
                   || result.probeStartSeconds != 0.0))
        result.error = "routing flags are valid only for routing";

    if (result.error.isEmpty()
        && result.command == CliCommand::analyze
        && result.planFile != juce::File())
    {
        for (const auto& input : result.inputs)
        {
            if (input == result.planFile)
            {
                result.error = "--plan must not target an input audio file";
                break;
            }
        }
    }

    return result;
}
} // namespace singleeditor
