#include "CliDiagnostics.h"
#include "CLI/CliColdManifest.h"
#include "Diagnostics/DiagnosticBundle.h"
#include "Diagnostics/DiagnosticSnapshot.h"
#include "Diagnostics/ApplicationDiagnosticIdentity.h"
#include "Diagnostics/CodecDiagnosticProbe.h"
#include "Diagnostics/DeviceDiagnosticProbe.h"
#include "Diagnostics/ReproductionFiles.h"

namespace singleeditor
{
namespace
{
const char* codeFor(diagnostics::BundleError error)
{
    using Error = diagnostics::BundleError;
    switch (error)
    {
        case Error::none: return "ok";
        case Error::invalidInput: return "invalid-input";
        case Error::budget: return "bundle-budget-exceeded";
        case Error::malformedArchive: return "malformed-archive";
        case Error::malformedJson: return "malformed-json";
        case Error::unsupportedSchema: return "unsupported-core-schema-use-compatible-validator";
        case Error::inconsistentManifest: return "inconsistent-manifest";
        case Error::hashMismatch: return "payload-hash-mismatch";
        case Error::cancelled: return "cancelled";
        case Error::ioFailure: return "bundle-read-failed";
        case Error::destinationExists: return "destination-exists";
    }
    return "internal-error";
}
juce::var event(int sequence, bool terminal, AutomationExitCode exitCode, const juce::String& command = "diagnostics validate")
{
    auto* object = new juce::DynamicObject();
    object->setProperty("schema", CliColdManifest::eventSchema);
    object->setProperty("protocol_version", CliColdManifest::eventProtocolVersion);
    object->setProperty("sequence", sequence);
    object->setProperty("type", terminal ? (exitCode == AutomationExitCode::success ? "result" : "error") : "started");
    object->setProperty("terminal", terminal);
    object->setProperty("command", command);
    object->setProperty("operation", juce::var());
    if (terminal) { object->setProperty("ok", exitCode == AutomationExitCode::success); object->setProperty("exit_code", static_cast<int>(exitCode)); }
    return juce::var(object);
}
juce::var snapshotPreview(const diagnostics::PreparedSnapshot& prepared)
{
    const auto& snapshot = prepared.content(); const auto& identity = snapshot.identity;
    auto* object = new juce::DynamicObject();
    object->setProperty("snapshot_id", snapshot.snapshotId.toString());
    object->setProperty("origin", snapshot.origin);
    object->setProperty("captured_ms", snapshot.timestampMs);
    object->setProperty("privacy_tier", snapshot.tier == diagnostics::PrivacyTier::supportSafe ? "support-safe"
        : snapshot.tier == diagnostics::PrivacyTier::localIdentifiers ? "local-identifiers" : "reproduction-material");
    object->setProperty("gui_state_available", false);
    object->setProperty("audio_content_included", prepared.preview().attachmentCount == 0 ? juce::var(false) : juce::var());
    object->setProperty("reproduction_content_included", prepared.preview().attachmentCount != 0);
    auto* application = new juce::DynamicObject();
    application->setProperty("version", identity.version); application->setProperty("commit", identity.commit);
    application->setProperty("binary_sha256", identity.binarySha256); application->setProperty("configuration", identity.configuration);
    application->setProperty("distribution", identity.distribution); application->setProperty("process_architecture", identity.processArchitecture);
    application->setProperty("os_architecture", identity.osArchitecture); application->setProperty("os_build", identity.osBuild);
    application->setProperty("timing_instrumentation", identity.timingInstrumentation);
    object->setProperty("application", juce::var(application));
    juce::Array<juce::var> categories; for (const auto& category : prepared.preview().categories) categories.add(category);
    object->setProperty("categories", categories);
    auto* sections = new juce::DynamicObject();
    juce::Array<juce::var> attachments;
    for (const auto& entry : snapshot.entries)
    {
        if (entry.kind == diagnostics::EntryKind::provider) sections->setProperty(entry.id, juce::JSON::parse(entry.bytes.toString()));
        else
        {
            auto* attachment = new juce::DynamicObject();
            attachment->setProperty("id", entry.id); attachment->setProperty("kind", "opaque-reproduction");
            attachment->setProperty("bytes", static_cast<juce::int64>(entry.bytes.getSize()));
            attachment->setProperty("sha256", juce::SHA256(entry.bytes).toHexString());
            attachments.add(juce::var(attachment));
        }
    }
    object->setProperty("sections", juce::var(sections));
    object->setProperty("attachments", attachments);
    return juce::var(object);
}
CliSingleFileExecution executeCapture(const juce::StringArray& arguments, const DiagnosticCliInteraction& interaction)
{
    using namespace diagnostics;
    CliSingleFileExecution execution;
    const bool exporting = arguments[1] == "export";
    const auto command = "diagnostics " + arguments[1];
    juce::String source, output, error;
    bool jsonl = false, active = false, includePath = false, includeName = false, includeReproduction = false;
    bool deviceRequested = false, includeDeviceName = false;
    DeviceProbeRequest deviceRequest;
    const auto parseNumber = [&](const juce::String& text, int& value)
    {
        if (text.isEmpty() || text.length() > 6 || !text.containsOnly("0123456789")) error = "invalid-device-probe-number";
        else value = text.getIntValue();
    };
    juce::StringArray attachmentPaths;
    std::set<juce::String> seen;
    for (int i = 2; i < arguments.size(); ++i)
    {
        const auto key = arguments[i];
        if (key != "--attach" && !seen.insert(key).second) { error = "duplicate-diagnostics-option"; continue; }
        if (key == "--jsonl") jsonl = true;
        else if (key == "--active-probe") active = true;
        else if (key == "--include-path") includePath = true;
        else if (key == "--include-file-name") includeName = true;
        else if (key == "--include-device-name") includeDeviceName = true;
        else if (key == "--device" && i + 1 < arguments.size()) { deviceRequested = true; deviceRequest.backend = arguments[++i]; }
        else if (key == "--output-device" && i + 1 < arguments.size()) deviceRequest.outputDevice = arguments[++i];
        else if (key == "--sample-rate" && i + 1 < arguments.size()) parseNumber(arguments[++i], deviceRequest.sampleRate);
        else if (key == "--buffer-size" && i + 1 < arguments.size()) parseNumber(arguments[++i], deviceRequest.bufferSamples);
        else if (key == "--output-channels" && i + 1 < arguments.size()) parseNumber(arguments[++i], deviceRequest.outputChannels);
        else if (key == "--ping-channel" && i + 1 < arguments.size()) parseNumber(arguments[++i], deviceRequest.pingChannel);
        else if (key == "--include-reproduction") includeReproduction = true;
        else if (key == "--attach" && i + 1 < arguments.size()) attachmentPaths.add(arguments[++i]);
        else if ((key == "--codec" || key == "--output") && i + 1 < arguments.size())
        { if (key == "--codec") source = arguments[++i]; else output = arguments[++i]; }
        else error = "invalid-diagnostics-option";
    }
    const auto absolute = [](const juce::String& value) { return value.isNotEmpty() && value.length() <= 32760 && juce::File::isAbsolutePath(value); };
    if ((!source.isEmpty() && !absolute(source)) || (exporting ? !absolute(output) : output.isNotEmpty())
        || (!exporting && source.isEmpty() && !deviceRequested) || (active != (source.isNotEmpty() || deviceRequested))
        || ((includePath || includeName) && source.isEmpty())) error = "invalid-diagnostics-arguments";
    if ((deviceRequested && (source.isNotEmpty() || !DeviceDiagnosticProbe::valid(deviceRequest)))
        || (!deviceRequested && (includeDeviceName || seen.count("--output-device") || seen.count("--sample-rate")
            || seen.count("--buffer-size") || seen.count("--output-channels") || seen.count("--ping-channel"))))
        error = "invalid-device-probe-arguments";
    if ((!attachmentPaths.isEmpty() && !exporting) || (includeReproduction != !attachmentPaths.isEmpty())
        || attachmentPaths.size() > static_cast<int>(ReproductionFiles::maximumFiles)
        || std::any_of(attachmentPaths.begin(), attachmentPaths.end(), [&](const auto& path) { return !absolute(path); }))
        error = "invalid-reproduction-arguments";
    int sequence = 1;
    const auto emit = [&](const juce::var& message)
    {
        execution.stdoutText += juce::JSON::toString(message, jsonl) + "\n";
    };
    emit(event(sequence++, false, AutomationExitCode::success, command));
    const auto finish = [&](AutomationExitCode code, const juce::var& payload, const juce::String& failure)
    {
        execution.exitCode = code;
        auto terminal = event(sequence++, true, code, command);
        terminal.getDynamicObject()->setProperty("payload", payload);
        if (failure.isNotEmpty()) terminal.getDynamicObject()->setProperty("error", failure);
        emit(terminal); return execution;
    };
    if (error.isNotEmpty()) return finish(AutomationExitCode::invalidArguments, {}, error);
    const std::atomic<bool> running { false };
    const auto& cancelled = interaction.cancelled ? *interaction.cancelled : running;
    const auto approve = [&](const char* type, const juce::Uuid& id, const juce::var& payload)
    {
        auto review = event(sequence++, false, AutomationExitCode::success, command);
        review.getDynamicObject()->setProperty("type", type);
        review.getDynamicObject()->setProperty("approval_id", id.toString());
        review.getDynamicObject()->setProperty("payload", payload);
        review.getDynamicObject()->setProperty("instruction", "Review this exact preview. Type its approval_id followed by Enter; any other input cancels.");
        emit(review);
        if (!interaction.confirm || cancelled.load()) return false;
        juce::String response;
        try { response = interaction.confirm(execution.stdoutText); }
        catch (...) { return false; }
        execution.stdoutText.clear(); // callback has displayed the complete prefix
        return !cancelled.load() && response == id.toString();
    };
    CodecProbeResult probe;
    DeviceProbeResult deviceProbe;
    bool probed = false;
    if (active && !deviceRequested)
    {
        const juce::Uuid operation;
        auto* effect = new juce::DynamicObject();
        effect->setProperty("action", "codec-initial-window");
        effect->setProperty("origin", "standalone-cli");
        effect->setProperty("effect", "Read the selected source in a temporary child process. No device, playback, repair or persistent cache. Temporary technical result is removed afterward.");
        effect->setProperty("maximum_read_bytes", CodecDiagnosticProbe::readBudget);
        effect->setProperty("maximum_frames", CodecDiagnosticProbe::windowFrames);
        effect->setProperty("maximum_worker_ms", CodecDiagnosticProbe::limits().wallTimeMs);
        effect->setProperty("maximum_worker_memory_bytes", static_cast<juce::int64>(CodecDiagnosticProbe::limits().memoryBytes));
        if (!approve("effect-preview", operation, juce::var(effect))) return finish(AutomationExitCode::cancelled, {}, "probe-not-approved");
        probe = CodecDiagnosticProbe::run(interaction.workerExecutable == juce::File()
            ? juce::File::getSpecialLocation(juce::File::currentExecutableFile) : interaction.workerExecutable,
            juce::File(source), operation, cancelled);
        probed = true;
        if (probe.cancelled || cancelled.load()) return finish(AutomationExitCode::cancelled, {}, "probe-cancelled");
    }
    if (active && deviceRequested)
    {
        const juce::Uuid operation;
        auto* effect = new juce::DynamicObject();
        effect->setProperty("action", deviceRequest.pingChannel == 0 ? "device-negotiation" : "device-channel-ping");
        effect->setProperty("origin", "standalone-cli");
        effect->setProperty("effect", "Open a temporary output device in this CLI's bounded child, then stop and close it. No system default, saved settings, routing, Document or cache changes. This is independent of any GUI; stop other playback/monitoring before approving. A device held exclusively elsewhere may refuse this probe.");
        effect->setProperty("backend", deviceRequest.backend);
        effect->setProperty("output_selection", deviceRequest.outputDevice.isEmpty() ? "backend-default-output" : "explicit-output-name");
        effect->setProperty("requested_rate", deviceRequest.sampleRate);
        effect->setProperty("requested_buffer", deviceRequest.bufferSamples);
        effect->setProperty("requested_output_channels", deviceRequest.outputChannels);
        effect->setProperty("emits_sound", deviceRequest.pingChannel != 0);
        effect->setProperty("physical_channel_one_based", deviceRequest.pingChannel);
        effect->setProperty("peak_dbfs", DeviceDiagnosticProbe::peakDbfs);
        effect->setProperty("tone_hz", DeviceDiagnosticProbe::toneHz);
        effect->setProperty("duration_ms", DeviceDiagnosticProbe::durationMs);
        effect->setProperty("maximum_worker_ms", DeviceDiagnosticProbe::limits().wallTimeMs);
        effect->setProperty("maximum_worker_memory_bytes", static_cast<juce::int64>(DeviceDiagnosticProbe::limits().memoryBytes));
        if (!approve("effect-preview", operation, juce::var(effect))) return finish(AutomationExitCode::cancelled, {}, "probe-not-approved");
        deviceProbe = DeviceDiagnosticProbe::run(interaction.workerExecutable == juce::File()
            ? juce::File::getSpecialLocation(juce::File::currentExecutableFile) : interaction.workerExecutable,
            deviceRequest, operation, cancelled);
        probed = true;
        if (deviceProbe.cancelled || cancelled.load()) return finish(AutomationExitCode::cancelled, {}, "probe-cancelled");
    }
    const auto probeSucceeded = deviceRequested ? deviceProbe.succeeded : probe.succeeded;
    SnapshotSelection selection;
    if (includeReproduction)
    {
        auto* contents = new juce::DynamicObject();
        contents->setProperty("action", "include-reproduction-contents");
        contents->setProperty("file_count", attachmentPaths.size());
        contents->setProperty("effect", "Copy only the explicitly selected complete files into this local bundle. Contents are not redacted, opened or parsed. Choose minimal examples. Nothing is uploaded.");
        contents->setProperty("maximum_file_bytes", DiagnosticBundle::maximumEntryBytes);
        contents->setProperty("maximum_total_bytes", ReproductionFiles::maximumTotalBytes);
        if (!approve("effect-preview", juce::Uuid(), juce::var(contents))) return finish(AutomationExitCode::cancelled, {}, "reproduction-not-approved");
        std::vector<juce::File> files; for (const auto& path : attachmentPaths) files.emplace_back(path);
        auto included = ReproductionFiles::capture(files, cancelled);
        if (!included.status) return finish(included.status.error == BundleError::cancelled ? AutomationExitCode::cancelled
            : included.status.error == BundleError::budget ? AutomationExitCode::validationFailed : AutomationExitCode::executionFailed,
            {}, codeFor(included.status.error));
        selection.reproductionConsent = true; selection.chosenReproduction = std::move(included.entries);
    }
    if (includePath) selection.localIdentifiers.insert(LocalIdentifierKind::path);
    if (includeName) selection.localIdentifiers.insert(LocalIdentifierKind::fileName);
    if (includeDeviceName) selection.localIdentifiers.insert(LocalIdentifierKind::deviceName);
    const auto identity = captureApplicationIdentity(cancelled, "standalone-cli");
    const auto prepared = DiagnosticSnapshot::capture(identity, "standalone-cli",
        !probed ? std::vector<LoadedProviderRef>{} : deviceRequested
            ? std::vector<LoadedProviderRef>{{ ProviderKind::audio, &deviceProbe }}
            : std::vector<LoadedProviderRef>{{ ProviderKind::codec, &probe }}, selection);
    if (cancelled.load()) return finish(AutomationExitCode::cancelled, {}, "capture-cancelled");
    if (!prepared.status()) return finish(AutomationExitCode::executionFailed, {}, codeFor(prepared.status().error));
    const auto preview = snapshotPreview(prepared);
    if (!exporting) return finish(probeSucceeded ? AutomationExitCode::success : AutomationExitCode::executionFailed,
                                  preview, probeSucceeded ? juce::String{} : deviceRequested ? "device-probe-failed" : "codec-probe-failed");
    if (!approve("snapshot-review", prepared.preview().snapshotId, preview)) return finish(AutomationExitCode::cancelled, {}, "snapshot-not-approved");
    const auto published = DiagnosticSnapshot::publishReviewed(prepared, prepared.preview().snapshotId, juce::File(output), &cancelled);
    auto* result = new juce::DynamicObject();
    result->setProperty("snapshot_id", prepared.preview().snapshotId.toString());
    result->setProperty("origin", "standalone-cli"); result->setProperty("write_performed", static_cast<bool>(published));
    result->setProperty("partial_retained", published.partialRetained);
    result->setProperty("probe_succeeded", probed ? juce::var(probeSucceeded) : juce::var());
    result->setProperty("reproduction_count", static_cast<int>(prepared.preview().attachmentCount));
    return finish(published ? AutomationExitCode::success : published.error == BundleError::cancelled ? AutomationExitCode::cancelled
        : published.error == BundleError::destinationExists ? AutomationExitCode::conflict : AutomationExitCode::executionFailed,
        juce::var(result), published ? juce::String{} : codeFor(published.error));
}
}
std::optional<CliSingleFileExecution> tryExecuteDiagnosticsCli(const juce::StringArray& arguments, const DiagnosticCliInteraction& interaction)
{
    if (arguments.isEmpty() || arguments[0] != "diagnostics") return std::nullopt;
    if (arguments.size() >= 2 && (arguments[1] == "probe" || arguments[1] == "export")) return executeCapture(arguments, interaction);
    CliSingleFileExecution execution;
    constexpr auto usage = "Usage: SingleEditorCLI diagnostics validate --bundle <absolute-path.zip> [--jsonl]\n";
    if (arguments.size() == 2 && arguments[1] == "--help")
    {
        execution.stdoutText = juce::String(usage) + "Read-only bundle validation, core schema 1. No extraction.\n"
                            + "diagnostics probe --codec <absolute-audio-path> --active-probe [--jsonl]\n"
                            + "diagnostics probe --device <exact-backend-type> --active-probe [--output-device <exact-name>] [--sample-rate <Hz>] [--buffer-size <samples>] [--output-channels <1..32>] [--ping-channel <1..outputs>] [--include-device-name] [--jsonl]\n"
                            + "diagnostics export --output <absolute-new.zip> [--codec <absolute-audio-path> --active-probe] [--include-path] [--include-file-name] [--jsonl]\n"
                            + "  Add --include-reproduction --attach <absolute-file> [--attach <another-file>] to explicitly include complete reproduction contents.\n"
                            + "Export also accepts the same device options instead of --codec. Rate/buffer default to 0 (backend policy); outputs default to 2. No ping flag means a silent callback check.\n"
                            + "Effect preview and exact-ID approval precede each probe. Export then requires separate snapshot-ID approval via stdin.\n"
                            + "Export without --active-probe observes only this CLI process; it never reads GUI state or emits audio. Device ping requires separate disclosed approval (250ms, -40dBFS, 100Hz).\n"
                            + "Reproduction files require separate contents consent and are reviewed by ID/length/hash; attachments are never parsed.\n"
                            + "Cache probes and GUI device restoration are not yet available.\n";
        return execution;
    }
    juce::String path, error;
    bool jsonl = false, sawBundle = false;
    if (arguments.size() < 2 || arguments[1] != "validate") error = "diagnostic-command-unavailable";
    for (int i = 2; i < arguments.size(); ++i)
    {
        if (arguments[i] == "--jsonl" && !jsonl) jsonl = true;
        else if (arguments[i] == "--bundle" && !sawBundle && i + 1 < arguments.size()) { sawBundle = true; path = arguments[++i]; }
        else error = "invalid-diagnostics-arguments";
    }
    if (!sawBundle || path.isEmpty() || path.length() > 32760 || !juce::File::isAbsolutePath(path)) error = "invalid-diagnostics-arguments";
    diagnostics::BundleView view;
    if (error.isEmpty())
    {
        view = diagnostics::DiagnosticBundle::validate(juce::File(path), interaction.cancelled);
        if (!view.status)
        {
            error = codeFor(view.status.error);
            execution.exitCode = view.status.error == diagnostics::BundleError::ioFailure ? AutomationExitCode::executionFailed
                               : view.status.error == diagnostics::BundleError::cancelled ? AutomationExitCode::cancelled : AutomationExitCode::validationFailed;
        }
    }
    else execution.exitCode = AutomationExitCode::invalidArguments;
    if (!jsonl)
    {
        if (execution.exitCode == AutomationExitCode::success) execution.stdoutText = view.summary;
        else execution.stderrText = error + "\n" + usage;
        return execution;
    }
    execution.stdoutText = juce::JSON::toString(event(1, false, execution.exitCode), true) + "\n";
    auto terminal = event(2, true, execution.exitCode);
    if (execution.exitCode == AutomationExitCode::success)
    {
        auto* payload = new juce::DynamicObject();
        payload->setProperty("core_schema_major", view.manifest["coreSchemaMajor"]);
        payload->setProperty("snapshot_id", view.manifest["snapshotId"]);
        payload->setProperty("origin", view.manifest["origin"]);
        payload->setProperty("privacy_tier", view.manifest["privacyTier"]);
        payload->setProperty("verified_entries", static_cast<int>(view.verifiedEntries));
        payload->setProperty("verified_bytes", static_cast<juce::int64>(view.verifiedBytes));
        payload->setProperty("producer_authenticated", false);
        payload->setProperty("write_performed", false);
        terminal.getDynamicObject()->setProperty("payload", juce::var(payload));
    }
    else
    {
        auto* failure = new juce::DynamicObject(); failure->setProperty("code", error);
        failure->setProperty("message", "Bundle validation did not succeed. No entries were extracted or opened.");
        terminal.getDynamicObject()->setProperty("error", juce::var(failure));
    }
    execution.stdoutText += juce::JSON::toString(terminal, true) + "\n";
    return execution;
}
}
