#pragma once

#include "Automation/AutomationExitCode.h"
#include "Audio/AmbisonicMonitorDecoder.h"

#include <juce_core/juce_core.h>

namespace singleeditor
{
enum class CliCommand
{
    invalid,
    operations,
    inspect,
    analyze,
    apply,
    validate,
    routing
};

struct CliArguments
{
    CliCommand command = CliCommand::invalid;
    juce::Array<juce::File> inputs;
    bool json = false;
    bool sampleStats = false;
    juce::String operationId;
    bool headOnly = true;
    double thresholdDbfs = -60.0;
    double gainDb = 0.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    double targetDbfs = -1.0;
    juce::File pipelineFile;
    juce::File planFile;
    bool dryRun = false;
    bool inPlace = false;
    juce::File outputDirectory;
    juce::String conflictPolicy { "fail" };
    bool backup = false;
    juce::File backupDirectory;
    bool allowLossyReencode = false;
    bool failFast = false;
    juce::File manifestFile;
    int outputChannels = 0;
    juce::String sourcePresetId;
    bool includeLfe = false;
    juce::StringArray outputMappings;
    juce::Array<int> mutedSourceChannels;
    juce::Array<int> soloedSourceChannels;
    juce::String monitorMode { "auto" };
    juce::String ambisonicMonitorMode { "speaker" };
    AmbisonicOrientation ambisonicOrientation;
    bool ambisonicOrientationSpecified = false;
    bool runtimeProbe = false;
    double probeStartSeconds = 0.0;
    AutomationExitCode errorCode = AutomationExitCode::invalidArguments;
    juce::String requestedCommand;
    juce::String error;

    bool isValid() const noexcept
    {
        return command != CliCommand::invalid && error.isEmpty();
    }
};

CliArguments parseCliArguments(const juce::StringArray& arguments);
juce::String cliCommandName(CliCommand command);
} // namespace singleeditor
