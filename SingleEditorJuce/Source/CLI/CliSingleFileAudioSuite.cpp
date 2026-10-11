#include "CLI/CliSingleFileAudioSuite.h"

#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "Automation/AutomationPlanIO.h"
#include "Automation/FileIdentity.h"
#include "CLI/CliAudioSuiteOperationRegistry.h"
#include "CLI/CliColdManifest.h"
#include "Document/SingleTrackDocument.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace singleeditor
{
namespace
{
class CliDocumentIdentityObserver final : private SingleTrackDocument::Listener
{
public:
    explicit CliDocumentIdentityObserver(SingleTrackDocument& owner) : document(owner)
    {
        document.addListener(this);
    }
    ~CliDocumentIdentityObserver() override { document.removeListener(this); }
    juce::var receipt() const
    {
        auto value = latest.toJson();
        value.getDynamicObject()->setProperty("event_sequence", static_cast<juce::int64>(sequence));
        return value;
    }
private:
    void documentIdentityChanged(const media::IdentityEvent& event) override
    {
        latest = event.after.document;
        sequence = event.sequence;
    }
    SingleTrackDocument& document;
    media::DocumentIdentity latest;
    uint64_t sequence = 0;
};

struct CommandOutcome
{
    AutomationExitCode exitCode = AutomationExitCode::success;
    juce::var payload;
    juce::String errorCode;
    juce::String errorMessage;

    bool succeeded() const noexcept
    {
        return exitCode == AutomationExitCode::success;
    }
};

class EventStream final
{
public:
    EventStream(juce::String commandToUse, juce::String operationToUse)
        : command(std::move(commandToUse)), operation(std::move(operationToUse))
    {
        append("started", false, {});
    }

    void progress(const juce::String& phase, double fraction)
    {
        auto* payload = new juce::DynamicObject();
        payload->setProperty("phase", phase);
        payload->setProperty("fraction", juce::jlimit(0.0, 1.0, fraction));
        append("progress", false, juce::var(payload));
    }

    void result(const juce::var& payload)
    {
        appendTerminal("result", AutomationExitCode::success, payload, {}, {});
    }

    void error(AutomationExitCode exitCode,
               const juce::String& code,
               const juce::String& message)
    {
        appendTerminal("error", exitCode, {}, code, message);
    }

    juce::String takeText() const { return text; }

private:
    juce::DynamicObject* makeBase(const juce::String& type, bool terminal)
    {
        auto* event = new juce::DynamicObject();
        event->setProperty("schema", CliColdManifest::eventSchema);
        event->setProperty("protocol_version", CliColdManifest::eventProtocolVersion);
        event->setProperty("sequence", ++sequence);
        event->setProperty("type", type);
        event->setProperty("terminal", terminal);
        event->setProperty("command", command);
        event->setProperty("operation",
                           operation.isEmpty() ? juce::var() : juce::var(operation));
        return event;
    }

    void append(const juce::String& type, bool terminal, const juce::var& payload)
    {
        auto* event = makeBase(type, terminal);
        if (!payload.isVoid())
            event->setProperty("payload", payload);
        text << juce::JSON::toString(juce::var(event), true) << "\n";
    }

    void appendTerminal(const juce::String& type,
                        AutomationExitCode exitCode,
                        const juce::var& payload,
                        const juce::String& code,
                        const juce::String& message)
    {
        if (hasTerminal)
            return;
        hasTerminal = true;
        auto* event = makeBase(type, true);
        event->setProperty("ok", exitCode == AutomationExitCode::success);
        event->setProperty("exit_code", static_cast<int>(exitCode));
        if (!payload.isVoid())
            event->setProperty("payload", payload);
        if (code.isNotEmpty() || message.isNotEmpty())
        {
            auto* errorObject = new juce::DynamicObject();
            errorObject->setProperty("code", code);
            errorObject->setProperty("message", message);
            event->setProperty("error", juce::var(errorObject));
        }
        text << juce::JSON::toString(juce::var(event), true) << "\n";
    }

    juce::String command;
    juce::String operation;
    juce::String text;
    int sequence = 0;
    bool hasTerminal = false;
};

struct AnalyzeArguments
{
    juce::String operationId;
    juce::File input;
    juce::File plan;
    juce::File destination;
    juce::File parametersFile;
    juce::String destinationMode;
    double gainDb = 0.0;
    bool gainSpecified = false;
    int64_t startSample = -1;
    int64_t endSample = -1;
    std::vector<int> channels;
    bool jsonl = false;
};

struct ApplyArguments
{
    juce::File plan;
    bool allowInPlace = false;
    bool allowOverwriteExisting = false;
    bool jsonl = false;
};

struct CanonicalPlan
{
    juce::String manifestSha256;
    juce::String eventProtocolVersion;
    juce::String operationId;
    juce::String operationVersion;
    juce::String algorithmVersion;
    FileIdentity sourceIdentity;
    juce::String sourceFormatName;
    juce::String sourceExtension;
    double sampleRate = 0.0;
    int sourceChannels = 0;
    int bitsPerSample = 0;
    int64_t sourceSamples = 0;
    int64_t startSample = 0;
    int64_t endSample = 0;
    std::vector<int> channels;
    bool wholeDuration = false;
    bool allSourceChannels = false;
    juce::var operationParameters;
    juce::String destinationMode;
    juce::String destinationPath;
    std::optional<FileIdentity> destinationIdentity;
    juce::String planSha256;
};

juce::String sha256For(const juce::String& text)
{
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}

bool samePath(const juce::String& left, const juce::String& right) noexcept
{
#if JUCE_WINDOWS
    return left.equalsIgnoreCase(right);
#else
    return left == right;
#endif
}

bool isWavPath(const juce::File& file)
{
    return file.getFileExtension().equalsIgnoreCase(".wav");
}

bool parseFiniteDouble(const juce::String& text, double& value) noexcept
{
    const auto trimmed = text.trim();
    const auto* start = trimmed.toRawUTF8();
    char* end = nullptr;
    const auto parsed = std::strtod(start, &end);
    if (end == start || end == nullptr || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

bool parseInt64(const juce::String& text, int64_t& value) noexcept
{
    const auto trimmed = text.trim();
    const auto* start = trimmed.toRawUTF8();
    char* end = nullptr;
    const auto parsed = std::strtoll(start, &end, 10);
    if (end == start || end == nullptr || *end != '\0')
        return false;
    value = static_cast<int64_t>(parsed);
    return true;
}

bool takeValue(const juce::StringArray& arguments,
               int& index,
               juce::String& value,
               juce::String& error)
{
    if (index + 1 >= arguments.size())
    {
        error = arguments[index] + " requires a value";
        return false;
    }
    value = arguments[++index];
    return true;
}

bool parseAnalyzeArguments(const juce::StringArray& arguments,
                           AnalyzeArguments& parsed,
                           juce::String& error)
{
    parsed.operationId = CliAudioSuiteOperationRegistry::canonicalOperationId(
        arguments.size() > 1 ? arguments[1] : juce::String());
    if (parsed.operationId.isEmpty())
    {
        error = "Analyze operation is not present in the cold manifest";
        return false;
    }
    bool startSpecified = false;
    bool endSpecified = false;
    for (int index = 2; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index];
        juce::String value;
        if (argument == "--jsonl")
        {
            parsed.jsonl = true;
        }
        else if (argument == "--input")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            if (parsed.input != juce::File())
            {
                error = "M3 single-file Analyze accepts exactly one --input";
                return false;
            }
            parsed.input = juce::File(value);
        }
        else if (argument == "--plan")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.plan = juce::File(value);
        }
        else if (argument == "--destination")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.destination = juce::File(value);
        }
        else if (argument == "--destination-mode")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.destinationMode = value.toLowerCase();
        }
        else if (argument == "--gain-db")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            if (!parseFiniteDouble(value, parsed.gainDb))
            {
                error = "--gain-db must be finite";
                return false;
            }
            parsed.gainSpecified = true;
        }
        else if (argument == "--parameters")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            if (parsed.parametersFile != juce::File())
            {
                error = "Analyze accepts exactly one --parameters file";
                return false;
            }
            parsed.parametersFile = juce::File(value);
        }
        else if (argument == "--start-sample" || argument == "--end-sample")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            int64_t sample = 0;
            if (!parseInt64(value, sample))
            {
                error = argument + " must be an integer";
                return false;
            }
            if (argument == "--start-sample")
            {
                parsed.startSample = sample;
                startSpecified = true;
            }
            else
            {
                parsed.endSample = sample;
                endSpecified = true;
            }
        }
        else if (argument == "--channel")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            int64_t channel = 0;
            if (!parseInt64(value, channel)
                || channel < 0
                || channel > std::numeric_limits<int>::max())
            {
                error = "--channel must be a non-negative source channel index";
                return false;
            }
            parsed.channels.push_back(static_cast<int>(channel));
        }
        else
        {
            error = "Unknown M3 Analyze argument: " + argument;
            return false;
        }
    }

    if (!parsed.jsonl)
        error = "M3 machine commands require --jsonl";
    else if (parsed.input == juce::File())
        error = "M3 single-file Analyze requires --input";
    else if (parsed.plan == juce::File())
        error = "M3 single-file Analyze requires --plan";
    else if (parsed.operationId == CliColdManifest::referenceGain().id
             && parsed.gainSpecified == (parsed.parametersFile != juce::File()))
        error = "Reference Gain requires exactly one of --gain-db or --parameters";
    else if (parsed.operationId != CliColdManifest::referenceGain().id
             && (parsed.gainSpecified || parsed.parametersFile == juce::File()))
        error = "EQ and Limiter Analyze require exactly one --parameters JSON file";
    else if (parsed.gainSpecified
             && (parsed.gainDb < -120.0 || parsed.gainDb > 24.0))
        error = "--gain-db is outside the advertised -120..24 dB range";
    else if (startSpecified != endSpecified)
        error = "--start-sample and --end-sample must be supplied together";
    else if (startSpecified && (parsed.startSample < 0
                                || parsed.endSample <= parsed.startSample))
        error = "The requested half-open sample range is invalid";
    else if (!juce::StringArray { "create_new", "in_place", "overwrite_existing" }
                  .contains(parsed.destinationMode))
        error = "--destination-mode must be create_new, in_place, or overwrite_existing";
    else if (parsed.destinationMode != "in_place"
             && parsed.destination == juce::File())
        error = parsed.destinationMode + " requires --destination";

    std::sort(parsed.channels.begin(), parsed.channels.end());
    if (std::adjacent_find(parsed.channels.begin(), parsed.channels.end())
        != parsed.channels.end())
        error = "Duplicate --channel values are not allowed";
    return error.isEmpty();
}

bool parseApplyArguments(const juce::StringArray& arguments,
                         ApplyArguments& parsed,
                         juce::String& error)
{
    for (int index = 1; index < arguments.size(); ++index)
    {
        const auto argument = arguments[index];
        juce::String value;
        if (argument == "--jsonl")
            parsed.jsonl = true;
        else if (argument == "--allow-in-place")
            parsed.allowInPlace = true;
        else if (argument == "--allow-overwrite-existing")
            parsed.allowOverwriteExisting = true;
        else if (argument == "--plan")
        {
            if (!takeValue(arguments, index, value, error)) return false;
            parsed.plan = juce::File(value);
        }
        else
        {
            error = "Unknown M3 apply argument: " + argument;
            return false;
        }
    }

    if (!parsed.jsonl)
        error = "M3 machine commands require --jsonl";
    else if (parsed.plan == juce::File())
        error = "apply requires --plan";
    else if (parsed.allowInPlace && parsed.allowOverwriteExisting)
        error = "Apply accepts only the authorization required by its Plan";
    return error.isEmpty();
}

bool hasExactProperties(const juce::DynamicObject& object,
                        std::initializer_list<const char*> names)
{
    if (object.getProperties().size() != static_cast<int>(names.size()))
        return false;
    for (const auto* name : names)
        if (!object.hasProperty(name))
            return false;
    return true;
}

juce::var buildCanonicalPlan(const CanonicalPlan& plan, bool includeHash)
{
    auto* operation = new juce::DynamicObject();
    operation->setProperty("id", plan.operationId);
    operation->setProperty("version", plan.operationVersion);
    operation->setProperty("algorithm_version", plan.algorithmVersion);

    auto* format = new juce::DynamicObject();
    format->setProperty("name", plan.sourceFormatName);
    format->setProperty("extension", plan.sourceExtension);
    format->setProperty("sample_rate", plan.sampleRate);
    format->setProperty("channels", plan.sourceChannels);
    format->setProperty("bits_per_sample", plan.bitsPerSample);
    format->setProperty("samples", plan.sourceSamples);

    auto* source = new juce::DynamicObject();
    source->setProperty("identity", plan.sourceIdentity.toJson());
    source->setProperty("format", juce::var(format));

    juce::Array<juce::var> channelValues;
    for (const auto channel : plan.channels)
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("source_index", channel);
        value->setProperty("stable_id",
                           SourceChannelIdentity::fromSourceIndex(channel).stableId);
        channelValues.add(juce::var(value));
    }
    auto* selection = new juce::DynamicObject();
    selection->setProperty("start_sample", plan.startSample);
    selection->setProperty("end_sample", plan.endSample);
    selection->setProperty("channels", juce::var(channelValues));
    selection->setProperty("whole_duration", plan.wholeDuration);
    selection->setProperty("all_source_channels", plan.allSourceChannels);

    auto* destination = new juce::DynamicObject();
    destination->setProperty("mode", plan.destinationMode);
    destination->setProperty("canonical_path", plan.destinationPath);
    destination->setProperty("identity",
                             plan.destinationIdentity.has_value()
                                 ? plan.destinationIdentity->toJson()
                                 : juce::var());

    auto* root = new juce::DynamicObject();
    root->setProperty("schema", CliColdManifest::planSchema);
    root->setProperty("schema_version", CliColdManifest::planSchemaVersion);
    root->setProperty("manifest_sha256", plan.manifestSha256);
    root->setProperty("event_protocol", plan.eventProtocolVersion);
    root->setProperty("operation", juce::var(operation));
    root->setProperty("source", juce::var(source));
    root->setProperty("selection", juce::var(selection));
    root->setProperty("parameters", plan.operationParameters);
    root->setProperty("destination", juce::var(destination));
    if (includeHash)
        root->setProperty("plan_sha256", plan.planSha256);
    return juce::var(root);
}

juce::String canonicalPlanHash(const CanonicalPlan& plan)
{
    return sha256For(juce::JSON::toString(buildCanonicalPlan(plan, false), true));
}

bool parseCanonicalPlan(const juce::var& value,
                        CanonicalPlan& plan,
                        juce::String& error)
{
    const auto* root = value.getDynamicObject();
    if (root == nullptr
        || !hasExactProperties(*root,
                               { "schema", "schema_version", "manifest_sha256",
                                 "event_protocol", "operation", "source",
                                 "selection", "parameters", "destination",
                                 "plan_sha256" }))
    {
        error = "Plan root does not match the canonical single-file schema";
        return false;
    }
    if (root->getProperty("schema").toString() != CliColdManifest::planSchema
        || root->getProperty("schema_version").toString()
               != CliColdManifest::planSchemaVersion)
    {
        error = "Plan schema/version is incompatible";
        return false;
    }

    const auto* operation = root->getProperty("operation").getDynamicObject();
    const auto* source = root->getProperty("source").getDynamicObject();
    const auto* selection = root->getProperty("selection").getDynamicObject();
    const auto* parameters = root->getProperty("parameters").getDynamicObject();
    const auto* destination = root->getProperty("destination").getDynamicObject();
    if (operation == nullptr || source == nullptr || selection == nullptr
        || parameters == nullptr || destination == nullptr
        || !hasExactProperties(*operation, { "id", "version", "algorithm_version" })
        || !hasExactProperties(*source, { "identity", "format" })
        || !hasExactProperties(*selection,
                               { "start_sample", "end_sample", "channels",
                                 "whole_duration", "all_source_channels" })
        || !hasExactProperties(*destination,
                               { "mode", "canonical_path", "identity" }))
    {
        error = "Plan contains an invalid canonical object";
        return false;
    }

    const auto* format = source->getProperty("format").getDynamicObject();
    if (format == nullptr
        || !hasExactProperties(*format,
                               { "name", "extension", "sample_rate", "channels",
                                 "bits_per_sample", "samples" }))
    {
        error = "Plan source format is invalid";
        return false;
    }

    plan.manifestSha256 = root->getProperty("manifest_sha256").toString();
    plan.eventProtocolVersion = root->getProperty("event_protocol").toString();
    plan.operationId = operation->getProperty("id").toString();
    plan.operationVersion = operation->getProperty("version").toString();
    plan.algorithmVersion = operation->getProperty("algorithm_version").toString();
    if (!FileIdentity::fromJson(source->getProperty("identity"),
                                plan.sourceIdentity, error))
        return false;
    plan.sourceFormatName = format->getProperty("name").toString();
    plan.sourceExtension = format->getProperty("extension").toString();
    plan.sampleRate = static_cast<double>(format->getProperty("sample_rate"));
    plan.sourceChannels = static_cast<int>(format->getProperty("channels"));
    plan.bitsPerSample = static_cast<int>(format->getProperty("bits_per_sample"));
    plan.sourceSamples = static_cast<int64_t>(format->getProperty("samples"));
    plan.startSample = static_cast<int64_t>(selection->getProperty("start_sample"));
    plan.endSample = static_cast<int64_t>(selection->getProperty("end_sample"));
    plan.wholeDuration = static_cast<bool>(selection->getProperty("whole_duration"));
    plan.allSourceChannels = static_cast<bool>(
        selection->getProperty("all_source_channels"));
    juce::var canonicalParameters;
    if (!CliAudioSuiteOperationRegistry::validateCanonicalParameters(
            plan.operationId, root->getProperty("parameters"),
            canonicalParameters, error))
    {
        if (error.isEmpty())
            error = "Plan parameters are not canonical";
        return false;
    }
    plan.operationParameters = canonicalParameters;
    plan.destinationMode = destination->getProperty("mode").toString();
    plan.destinationPath = destination->getProperty("canonical_path").toString();
    plan.planSha256 = root->getProperty("plan_sha256").toString();

    const auto* channelValues = selection->getProperty("channels").getArray();
    if (channelValues == nullptr || channelValues->isEmpty())
    {
        error = "Plan selection has no source channels";
        return false;
    }
    for (const auto& valueForChannel : *channelValues)
    {
        const auto* channel = valueForChannel.getDynamicObject();
        if (channel == nullptr
            || !hasExactProperties(*channel, { "source_index", "stable_id" }))
        {
            error = "Plan source channel entry is invalid";
            return false;
        }
        const auto index = static_cast<int>(channel->getProperty("source_index"));
        if (index < 0 || index >= plan.sourceChannels
            || channel->getProperty("stable_id").toString()
                   != SourceChannelIdentity::fromSourceIndex(index).stableId)
        {
            error = "Plan source channel identity is invalid";
            return false;
        }
        plan.channels.push_back(index);
    }
    if (!std::is_sorted(plan.channels.begin(), plan.channels.end())
        || std::adjacent_find(plan.channels.begin(), plan.channels.end())
               != plan.channels.end())
    {
        error = "Plan source channels are not canonical";
        return false;
    }

    const auto destinationIdentityValue = destination->getProperty("identity");
    if (!destinationIdentityValue.isVoid())
    {
        FileIdentity identity;
        if (!FileIdentity::fromJson(destinationIdentityValue, identity, error))
            return false;
        plan.destinationIdentity = identity;
    }

    const auto* operationDeclaration = CliColdManifest::findOperation(plan.operationId);
    const auto validDestinationMode = juce::StringArray {
        "create_new", "in_place", "overwrite_existing"
    }.contains(plan.destinationMode);
    if (plan.manifestSha256 != CliColdManifest::manifestSha256()
        || plan.eventProtocolVersion != CliColdManifest::eventProtocolVersion
        || operationDeclaration == nullptr
        || plan.operationId != operationDeclaration->id
        || plan.operationVersion != operationDeclaration->operationVersion
        || plan.algorithmVersion != operationDeclaration->algorithmVersion
        || plan.sourceExtension != "wav"
        || !std::isfinite(plan.sampleRate) || plan.sampleRate <= 0.0
        || plan.sourceChannels <= 0 || plan.bitsPerSample <= 0
        || plan.sourceSamples <= 0 || plan.startSample < 0
        || plan.endSample <= plan.startSample || plan.endSample > plan.sourceSamples
        || plan.destinationPath.isEmpty() || !validDestinationMode)
    {
        error = "Plan contract values are invalid or incompatible";
        return false;
    }
    if (plan.wholeDuration != (plan.startSample == 0
                               && plan.endSample == plan.sourceSamples)
        || plan.allSourceChannels
               != (static_cast<int>(plan.channels.size()) == plan.sourceChannels))
    {
        error = "Plan selection flags do not match its resolved range/channels";
        return false;
    }

    const auto sourcePath = plan.sourceIdentity.canonicalPath;
    if (plan.destinationMode == "create_new")
    {
        if (plan.destinationIdentity.has_value()
            || samePath(sourcePath, plan.destinationPath))
        {
            error = "create_new destination authorization is invalid";
            return false;
        }
    }
    else if (plan.destinationMode == "in_place")
    {
        if (!plan.destinationIdentity.has_value()
            || !samePath(sourcePath, plan.destinationPath)
            || !plan.destinationIdentity->matches(plan.sourceIdentity))
        {
            error = "in_place destination authorization is invalid";
            return false;
        }
    }
    else if (!plan.destinationIdentity.has_value()
             || samePath(sourcePath, plan.destinationPath))
    {
        error = "overwrite_existing destination authorization is invalid";
        return false;
    }

    if (plan.planSha256 != canonicalPlanHash(plan))
    {
        error = "Plan hash does not match its canonical content";
        return false;
    }
    return true;
}

juce::var selectionPayload(const CanonicalPlan& plan)
{
    juce::Array<juce::var> channels;
    for (const auto channel : plan.channels)
        channels.add(channel);
    auto* selection = new juce::DynamicObject();
    selection->setProperty("start_sample", plan.startSample);
    selection->setProperty("end_sample", plan.endSample);
    selection->setProperty("source_channels", juce::var(channels));
    return juce::var(selection);
}

CommandOutcome analyzeAudioSuiteOperation(const AnalyzeArguments& arguments)
{
    CommandOutcome outcome;
    if (!arguments.input.existsAsFile() || !isWavPath(arguments.input))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "UNSUPPORTED_SOURCE";
        outcome.errorMessage = "M3 AudioSuite CLI requires one existing WAV input";
        return outcome;
    }

    auto destination = arguments.destination;
    if (arguments.destinationMode == "in_place")
    {
        if (destination == juce::File())
            destination = arguments.input;
        else if (!samePath(destination.getFullPathName(),
                           arguments.input.getFullPathName()))
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "DESTINATION_MODE_MISMATCH";
            outcome.errorMessage = "in_place destination must be the source path";
            return outcome;
        }
    }
    if (!isWavPath(destination)
        || (!destination.getParentDirectory().isDirectory()))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "UNSUPPORTED_DESTINATION";
        outcome.errorMessage = "Destination must be a WAV path in an existing directory";
        return outcome;
    }
    if (samePath(arguments.plan.getFullPathName(), arguments.input.getFullPathName())
        || samePath(arguments.plan.getFullPathName(), destination.getFullPathName()))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "PLAN_PATH_ALIAS";
        outcome.errorMessage = "Plan path cannot alias source or destination";
        return outcome;
    }
    if (arguments.destinationMode == "create_new" && destination.exists())
    {
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = "DESTINATION_EXISTS";
        outcome.errorMessage = "create_new destination already exists";
        return outcome;
    }
    if (arguments.destinationMode == "overwrite_existing"
        && !destination.existsAsFile())
    {
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = "DESTINATION_MISSING";
        outcome.errorMessage = "overwrite_existing destination does not exist";
        return outcome;
    }

    SingleTrackDocument document;
    CliDocumentIdentityObserver identityObserver(document);
    if (!document.loadFile(arguments.input))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "SOURCE_OPEN_FAILED";
        outcome.errorMessage = "Could not open the source WAV";
        return outcome;
    }
    const auto state = document.getState();
    if (state == nullptr || !state->info.isValid())
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "SOURCE_STATE_INVALID";
        outcome.errorMessage = "Source WAV did not produce a valid document state";
        return outcome;
    }
    if (arguments.endSample > state->info.numSamples)
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "SELECTION_OUT_OF_RANGE";
        outcome.errorMessage = "Requested sample range exceeds the source";
        return outcome;
    }
    for (const auto channel : arguments.channels)
    {
        if (channel >= state->info.numChannels)
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "CHANNEL_OUT_OF_RANGE";
            outcome.errorMessage = "Requested source channel does not exist";
            return outcome;
        }
    }

    AudioSelectionRequest selection;
    if (arguments.startSample >= 0)
    {
        selection.timeRangeSeconds = juce::Range<double>(
            static_cast<double>(arguments.startSample) / state->info.sampleRate,
            static_cast<double>(arguments.endSample) / state->info.sampleRate);
    }
    for (const auto channel : arguments.channels)
        selection.sourceChannels.push_back(SourceChannelIdentity::fromSourceIndex(channel));

    juce::var inputParameters;
    if (arguments.parametersFile != juce::File())
    {
        if (!arguments.parametersFile.existsAsFile()
            || arguments.parametersFile.getSize() > 1024 * 1024)
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "PARAMETERS_READ_FAILED";
            outcome.errorMessage = "Parameters must be one existing JSON file no larger than 1 MiB";
            return outcome;
        }
        inputParameters = juce::JSON::parse(arguments.parametersFile.loadFileAsString());
        if (!inputParameters.isObject())
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "PARAMETERS_INVALID";
            outcome.errorMessage = "Parameters JSON must contain one object";
            return outcome;
        }
    }
    else
    {
        auto* parameters = new juce::DynamicObject();
        parameters->setProperty("gain_db", arguments.gainDb);
        inputParameters = juce::var(parameters);
    }

    const auto* coldOperation = CliColdManifest::findOperation(arguments.operationId);
    if (coldOperation == nullptr)
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "OPERATION_UNSUPPORTED";
        outcome.errorMessage = "Operation is not present in the cold manifest";
        return outcome;
    }
    const auto analyzed = CliAudioSuiteOperationRegistry::analyze(
        arguments.operationId, state, 1, selection, inputParameters);
    if (!analyzed.succeeded())
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "ANALYZE_FAILED";
        outcome.errorMessage = analyzed.errorMessage;
        return outcome;
    }

    CanonicalPlan plan;
    plan.manifestSha256 = CliColdManifest::manifestSha256();
    plan.eventProtocolVersion = CliColdManifest::eventProtocolVersion;
    plan.operationId = coldOperation->id;
    plan.operationVersion = coldOperation->operationVersion;
    plan.algorithmVersion = coldOperation->algorithmVersion;
    plan.sourceIdentity = FileIdentity::capture(arguments.input);
    plan.sourceFormatName = state->info.formatName;
    plan.sourceExtension = "wav";
    plan.sampleRate = state->info.sampleRate;
    plan.sourceChannels = state->info.numChannels;
    plan.bitsPerSample = state->info.bitsPerSample;
    plan.sourceSamples = state->info.numSamples;
    plan.startSample = analyzed.selection.sampleRange.getStart();
    plan.endSample = analyzed.selection.sampleRange.getEnd();
    for (const auto& channel : analyzed.selection.sourceChannels)
        plan.channels.push_back(channel.sourceChannelIndex);
    plan.wholeDuration = analyzed.selection.wholeDuration;
    plan.allSourceChannels = analyzed.selection.allSourceChannels;
    plan.operationParameters = analyzed.canonicalParameters;
    plan.destinationMode = arguments.destinationMode;
    plan.destinationPath = destination.getFullPathName();
    if (arguments.destinationMode == "in_place")
        plan.destinationIdentity = plan.sourceIdentity;
    else if (arguments.destinationMode == "overwrite_existing")
        plan.destinationIdentity = FileIdentity::capture(destination);
    plan.planSha256 = canonicalPlanHash(plan);

    juce::String writeError;
    if (!writeAutomationPlanAtomically(arguments.plan,
                                       buildCanonicalPlan(plan, true),
                                       writeError))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "PLAN_WRITE_FAILED";
        outcome.errorMessage = writeError;
        return outcome;
    }

    auto* payload = new juce::DynamicObject();
    payload->setProperty("plan_path", arguments.plan.getFullPathName());
    payload->setProperty("plan_sha256", plan.planSha256);
    payload->setProperty("manifest_sha256", plan.manifestSha256);
    payload->setProperty("source_identity", plan.sourceIdentity.toJson());
    // Runtime identity belongs to this execution receipt, never to the stable
    // cross-process immutable Plan or its content hash.
    payload->setProperty("document_identity", identityObserver.receipt());
    payload->setProperty("selection", selectionPayload(plan));
    auto* destinationPayload = new juce::DynamicObject();
    destinationPayload->setProperty("mode", plan.destinationMode);
    destinationPayload->setProperty("canonical_path", plan.destinationPath);
    payload->setProperty("destination", juce::var(destinationPayload));
    payload->setProperty("write_performed", false);
    outcome.payload = juce::var(payload);
    return outcome;
}

CommandOutcome applyAudioSuiteOperation(const ApplyArguments& arguments,
                                        EventStream& events)
{
    CommandOutcome outcome;
    juce::var planValue;
    juce::String planReadError;
    if (!readAutomationPlan(arguments.plan, planValue, planReadError))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "PLAN_READ_FAILED";
        outcome.errorMessage = planReadError;
        return outcome;
    }
    CanonicalPlan plan;
    juce::String planError;
    if (!parseCanonicalPlan(planValue, plan, planError))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "PLAN_INVALID";
        outcome.errorMessage = planError;
        return outcome;
    }

    const juce::File source(plan.sourceIdentity.canonicalPath);
    const juce::File destination(plan.destinationPath);
    if (!plan.sourceIdentity.matches(FileIdentity::capture(source)))
    {
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = "STALE_SOURCE";
        outcome.errorMessage = "Source identity changed after Analyze";
        return outcome;
    }
    if (!isWavPath(source) || !isWavPath(destination)
        || !destination.getParentDirectory().isDirectory())
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "FORMAT_OR_DESTINATION_INVALID";
        outcome.errorMessage = "Plan no longer resolves to supported WAV paths";
        return outcome;
    }
    if (plan.destinationMode == "create_new")
    {
        if (arguments.allowInPlace || arguments.allowOverwriteExisting)
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "UNEXPECTED_AUTHORIZATION";
            outcome.errorMessage = "create_new Plan does not accept an overwrite flag";
            return outcome;
        }
        if (destination.exists())
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "DESTINATION_EXISTS";
            outcome.errorMessage = "create_new destination appeared after Analyze";
            return outcome;
        }
    }
    else if (plan.destinationMode == "in_place")
    {
        if (!arguments.allowInPlace || arguments.allowOverwriteExisting)
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "IN_PLACE_NOT_AUTHORIZED";
            outcome.errorMessage = "Apply requires --allow-in-place for this Plan";
            return outcome;
        }
    }
    else
    {
        if (!arguments.allowOverwriteExisting || arguments.allowInPlace)
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "OVERWRITE_NOT_AUTHORIZED";
            outcome.errorMessage = "Apply requires --allow-overwrite-existing for this Plan";
            return outcome;
        }
        if (!destination.existsAsFile()
            || !plan.destinationIdentity->matches(FileIdentity::capture(destination)))
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "STALE_DESTINATION";
            outcome.errorMessage = "Overwrite destination changed after Analyze";
            return outcome;
        }
    }
    events.progress("plan-validated", 0.10);

    SingleTrackDocument document;
    CliDocumentIdentityObserver identityObserver(document);
    if (!document.loadFile(source))
    {
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "SOURCE_OPEN_FAILED";
        outcome.errorMessage = "Could not reopen the planned source WAV";
        return outcome;
    }
    const auto state = document.getState();
    if (state == nullptr || state->info.numSamples != plan.sourceSamples
        || state->info.numChannels != plan.sourceChannels
        || state->info.bitsPerSample != plan.bitsPerSample
        || std::abs(state->info.sampleRate - plan.sampleRate) > 0.001)
    {
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = "SOURCE_SHAPE_CHANGED";
        outcome.errorMessage = "Source audio shape no longer matches the Plan";
        return outcome;
    }

    AudioSelectionRequest selection;
    selection.timeRangeSeconds = juce::Range<double>(
        static_cast<double>(plan.startSample) / plan.sampleRate,
        static_cast<double>(plan.endSample) / plan.sampleRate);
    for (const auto channel : plan.channels)
        selection.sourceChannels.push_back(SourceChannelIdentity::fromSourceIndex(channel));

    const auto rehydrated = CliAudioSuiteOperationRegistry::analyze(
        plan.operationId, state, 1, selection, plan.operationParameters);
    const auto rehydratedBase = CliAudioSuiteOperationRegistry::basePlan(rehydrated);
    if (!rehydrated.succeeded()
        || rehydratedBase == nullptr
        || rehydratedBase->getProcessorId() != plan.operationId
        || rehydratedBase->getProcessorVersion() != plan.operationVersion
        || rehydrated.selection.sampleRange
               != juce::Range<int64_t>(plan.startSample, plan.endSample)
        || rehydrated.selection.sourceChannels != selection.sourceChannels
        || rehydrated.selection.wholeDuration != plan.wholeDuration
        || rehydrated.selection.allSourceChannels != plan.allSourceChannels
        || juce::JSON::toString(rehydrated.canonicalParameters, true)
               != juce::JSON::toString(plan.operationParameters, true))
    {
        outcome.exitCode = AutomationExitCode::validationFailed;
        outcome.errorCode = "PLAN_REHYDRATION_MISMATCH";
        outcome.errorMessage = rehydrated.errorMessage.isNotEmpty()
                                 ? rehydrated.errorMessage
                                 : "Frozen Plan cannot be rehydrated exactly";
        return outcome;
    }
    const auto applied = CliAudioSuiteOperationRegistry::apply(
        rehydrated, document, 1);
    if (!applied.succeeded())
    {
        if (applied.status == AudioSuiteApplyStatus::stalePlan)
        {
            outcome.exitCode = AutomationExitCode::conflict;
            outcome.errorCode = "STALE_PLAN";
        }
        else if (applied.status == AudioSuiteApplyStatus::cancelled)
        {
            outcome.exitCode = AutomationExitCode::cancelled;
            outcome.errorCode = "CANCELLED";
        }
        else if (applied.status == AudioSuiteApplyStatus::invalidPlan)
        {
            outcome.exitCode = AutomationExitCode::validationFailed;
            outcome.errorCode = "PLAN_EXECUTION_INVALID";
        }
        else
        {
            outcome.exitCode = AutomationExitCode::executionFailed;
            outcome.errorCode = "RENDER_FAILED";
        }
        outcome.errorMessage = "AudioSuite transaction did not apply";
        return outcome;
    }
    events.progress("rendered", 0.80);

    const auto staging = destination.getParentDirectory().getNonexistentChildFile(
        "." + destination.getFileNameWithoutExtension()
            + ".singleeditor-staging",
        destination.getFileExtension(), false);
    const auto snapshot = document.createSaveSnapshot(
        staging, SingleTrackDocument::SaveIntent::exportCopy);
    const auto write = SingleTrackDocument::writeSaveSnapshot(snapshot);
    if (!write.succeeded || !staging.existsAsFile())
    {
        staging.deleteFile();
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "ATOMIC_STAGE_FAILED";
        outcome.errorMessage = write.errorMessage.isNotEmpty()
                                 ? write.errorMessage
                                 : "Could not stage the rendered output";
        return outcome;
    }

    const auto sourceStillMatches = plan.sourceIdentity.matches(FileIdentity::capture(source));
    const auto destinationStillMatches = plan.destinationMode == "create_new"
        ? !destination.exists()
        : plan.destinationIdentity->matches(FileIdentity::capture(destination));
    if (!sourceStillMatches || !destinationStillMatches)
    {
        staging.deleteFile();
        outcome.exitCode = AutomationExitCode::conflict;
        outcome.errorCode = !sourceStillMatches ? "STALE_SOURCE" : "STALE_DESTINATION";
        outcome.errorMessage = "Source or destination changed before atomic publication";
        return outcome;
    }

    const auto published = plan.destinationMode == "create_new"
                         ? staging.moveFileTo(destination)
                         : staging.replaceFileIn(destination);
    if (!published)
    {
        staging.deleteFile();
        outcome.exitCode = AutomationExitCode::executionFailed;
        outcome.errorCode = "ATOMIC_PUBLISH_FAILED";
        outcome.errorMessage = "Could not atomically publish the staged output";
        return outcome;
    }
    events.progress("published", 1.0);

    auto* payload = new juce::DynamicObject();
    payload->setProperty("operation", plan.operationId);
    payload->setProperty("plan_sha256", plan.planSha256);
    payload->setProperty("output", destination.getFullPathName());
    payload->setProperty("output_identity", FileIdentity::capture(destination).toJson());
    payload->setProperty("document_identity", identityObserver.receipt());
    payload->setProperty("write_performed", true);
    payload->setProperty("rendered_samples", applied.renderedSamples);
    payload->setProperty("rendered_blocks", applied.renderedBlocks);
    payload->setProperty("selection", selectionPayload(plan));
    outcome.payload = juce::var(payload);
    return outcome;
}

bool isM3Analyze(const juce::StringArray& arguments)
{
    if (arguments.size() < 2 || arguments[0] != "analyze")
        return false;
    return CliColdManifest::findOperation(arguments[1]) != nullptr;
}

bool isM3Apply(const juce::StringArray& arguments)
{
    if (arguments.isEmpty() || arguments[0] != "apply")
        return false;
    if (arguments.contains("--jsonl"))
        return true;

    const auto planIndex = arguments.indexOf("--plan");
    if (planIndex < 0 || planIndex + 1 >= arguments.size())
        return false;
    const juce::File planFile(arguments[planIndex + 1]);
    if (!planFile.existsAsFile() || planFile.getSize() > 16 * 1024 * 1024)
        return false;
    const auto plan = juce::JSON::parse(planFile.loadFileAsString());
    return plan.isObject()
        && plan["schema"].toString() == CliColdManifest::planSchema;
}

juce::String operationIdForApplyEvent(const juce::StringArray& arguments)
{
    const auto planIndex = arguments.indexOf("--plan");
    if (planIndex < 0 || planIndex + 1 >= arguments.size())
        return {};
    const juce::File planFile(arguments[planIndex + 1]);
    if (!planFile.existsAsFile() || planFile.getSize() > 16 * 1024 * 1024)
        return {};
    const auto value = juce::JSON::parse(planFile.loadFileAsString());
    return value["operation"]["id"].toString();
}

CliSingleFileExecution finish(EventStream& events, const CommandOutcome& outcome)
{
    if (outcome.succeeded())
        events.result(outcome.payload);
    else
        events.error(outcome.exitCode, outcome.errorCode, outcome.errorMessage);
    CliSingleFileExecution execution;
    execution.exitCode = outcome.exitCode;
    execution.stdoutText = events.takeText();
    return execution;
}
} // namespace

bool inspectM3SingleFilePlan(const juce::File& planFile,
                             juce::var& canonicalPlan,
                             juce::String& error)
{
    juce::var value;
    if (!readAutomationPlan(planFile, value, error))
        return false;
    return inspectM3SingleFilePlanValue(value, canonicalPlan, error);
}

bool inspectM3SingleFilePlanValue(const juce::var& planValue,
                                  juce::var& canonicalPlan,
                                  juce::String& error)
{
    CanonicalPlan parsed;
    if (!parseCanonicalPlan(planValue, parsed, error))
        return false;
    canonicalPlan = buildCanonicalPlan(parsed, true);
    return true;
}

std::optional<CliSingleFileExecution> tryExecuteM3SingleFileCli(
    const juce::StringArray& arguments)
{
    if (arguments.isEmpty())
        return std::nullopt;

    if (arguments[0] == "discover")
    {
        EventStream events("discover", {});
        CommandOutcome outcome;
        if (arguments.size() != 2 || arguments[1] != "--jsonl")
        {
            outcome.exitCode = AutomationExitCode::invalidArguments;
            outcome.errorCode = "USAGE";
            outcome.errorMessage = "discover requires exactly --jsonl";
        }
        else
        {
            outcome.payload = CliColdManifest::createDiscoveryPayload();
        }
        return finish(events, outcome);
    }

    if (isM3Analyze(arguments))
    {
        const auto* operation = CliColdManifest::findOperation(arguments[1]);
        EventStream events("analyze", operation != nullptr ? operation->id
                                                            : juce::String());
        AnalyzeArguments parsed;
        juce::String error;
        if (!parseAnalyzeArguments(arguments, parsed, error))
        {
            CommandOutcome outcome;
            outcome.exitCode = AutomationExitCode::invalidArguments;
            outcome.errorCode = "USAGE";
            outcome.errorMessage = error;
            return finish(events, outcome);
        }
        return finish(events, analyzeAudioSuiteOperation(parsed));
    }

    if (isM3Apply(arguments))
    {
        EventStream events("apply", operationIdForApplyEvent(arguments));
        ApplyArguments parsed;
        juce::String error;
        if (!parseApplyArguments(arguments, parsed, error))
        {
            CommandOutcome outcome;
            outcome.exitCode = AutomationExitCode::invalidArguments;
            outcome.errorCode = "USAGE";
            outcome.errorMessage = error;
            return finish(events, outcome);
        }
        return finish(events, applyAudioSuiteOperation(parsed, events));
    }

    return std::nullopt;
}
} // namespace singleeditor
