#include "Automation/OperationRegistry.h"
#include "Automation/AudioInspectionService.h"
#include "Automation/BuiltInOperations.h"
#include "Automation/RoutingInspectionService.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "CLI/CliArguments.h"
#include "CLI/CliApplication.h"
#include "CLI/CliAudioSuiteOperationRegistry.h"
#include "CLI/CliBatchAutomation.h"
#include "CLI/CliColdManifest.h"
#include "CLI/CliSingleFileAudioSuite.h"
#include "CLI/CliDiagnostics.h"
#include "Diagnostics/DiagnosticBundle.h"
#include "Diagnostics/CodecDiagnosticProbe.h"
#include "DeviceDiagnosticProbeTests.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

namespace
{
using namespace singleeditor;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

class FakeOperation final : public AudioOperation
{
public:
    explicit FakeOperation(juce::String operationId)
        : id(std::move(operationId))
    {
    }

    AudioOperationDescriptor descriptor() const override
    {
        return { id, 1, id, AudioOperationMode::inspect, juce::var() };
    }

    AudioOperationResult execute(const AudioOperationRequest&) const override
    {
        return {};
    }

private:
    juce::String id;
};

void runRegistryTests()
{
    OperationRegistry registry;
    require(!registry.add(nullptr), "null operations are rejected");
    require(registry.add(std::make_unique<FakeOperation>("audio.inspect.v1")),
            "first unique operation is accepted");
    require(!registry.add(std::make_unique<FakeOperation>("audio.inspect.v1")),
            "duplicate operation id is rejected");
    require(registry.find("audio.inspect.v1") != nullptr,
            "registered operation is found");
    require(registry.find("missing.v1") == nullptr,
            "unknown operation is absent");
    require(registry.descriptors().size() == 1,
            "descriptor list remains deterministic");
}

int64_t writeLargeMappedWavFile(const juce::File& file)
{
    constexpr int blockSize = 65536;
    constexpr int64_t targetSamples = (65LL * 1024LL * 1024LL) / 2LL;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return 0;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(1)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream,
                                                                           options));
    if (writer == nullptr)
        return 0;

    juce::AudioBuffer<float> zeros(1, blockSize);
    zeros.clear();
    int64_t written = 0;
    while (written < targetSamples)
    {
        const auto count = static_cast<int>(
            std::min<int64_t>(blockSize, targetSamples - written));
        if (!writer->writeFromAudioSampleBuffer(zeros, 0, count))
            return 0;
        written += count;
    }
    writer.reset();
    return written;
}

bool writeRoutingWavFile(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withChannelLayout(juce::AudioChannelSet::fromWaveChannelMask(0x63f))
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(8, 8);
    source.clear();
    for (int channel = 0; channel < source.getNumChannels(); ++channel)
        source.setSample(channel, channel, 0.1f * static_cast<float>(channel + 1));

    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writeHeightRoutingWavFile(const juce::File& file)
{
    constexpr auto sevenOneFourWaveMask = 0x2d63f;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withChannelLayout(juce::AudioChannelSet::fromWaveChannelMask(sevenOneFourWaveMask))
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(12, 16);
    source.clear();
    for (int channel = 0; channel < source.getNumChannels(); ++channel)
        source.setSample(channel, channel, 0.05f * static_cast<float>(channel + 1));

    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writePlainMultichannelWavFile(const juce::File& file, int channelCount)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(channelCount)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(channelCount, 16);
    source.clear();
    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writeFoaFrontWavFile(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(4)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(4, 4096);
    source.clear();
    for (int sample = 0; sample < source.getNumSamples(); ++sample)
    {
        source.setSample(0, sample, 0.25f); // ACN0 W
        source.setSample(3, sample, 0.25f); // ACN3 X (front)
    }
    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writeTrimSilenceWavFile(const juce::File& file)
{
    constexpr auto sampleRate = 48000.0;
    constexpr auto silentSamples = 4800;
    constexpr auto contentSamples = 4800;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(2)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(2, silentSamples + contentSamples);
    source.clear();
    for (int sample = silentSamples; sample < source.getNumSamples(); ++sample)
    {
        source.setSample(0, sample, 0.25f);
        source.setSample(1, sample, -0.20f);
    }
    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writeEditPipelineWavFile(const juce::File& file)
{
    constexpr auto sampleRate = 1000.0;
    constexpr auto numSamples = 1000;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(1)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(1, numSamples);
    source.clear();
    for (int sample = 0; sample < numSamples; ++sample)
        source.setSample(0, sample, 0.25f);
    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

bool writeReferenceGainWavFile(const juce::File& file)
{
    constexpr auto sampleRate = 1000.0;
    constexpr auto numSamples = 1000;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(2)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> source(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        source.setSample(0, sample,
                         0.20f + 0.05f * std::sin(static_cast<float>(sample) * 0.03f));
        source.setSample(1, sample,
                         -0.15f + 0.03f * std::cos(static_cast<float>(sample) * 0.02f));
    }
    return writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples());
}

juce::AudioBuffer<float> readAudioFile(const juce::File& file)
{
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(manager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return {};

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels),
                                    static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true))
        return {};
    return buffer;
}

juce::Array<juce::var> parseJsonLines(const juce::String& text)
{
    juce::StringArray lines;
    lines.addLines(text);
    lines.removeEmptyStrings(true);
    juce::Array<juce::var> events;
    for (const auto& line : lines)
        events.add(juce::JSON::parse(line));
    return events;
}

int terminalEventCount(const juce::Array<juce::var>& events)
{
    int count = 0;
    for (const auto& event : events)
        if (static_cast<bool>(event["terminal"]))
            ++count;
    return count;
}

void runInspectionTests()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-cli-inspection", ".wav", false);
    temp.deleteFile();

    juce::AudioBuffer<float> source(2, 4);
    const float left[] { 0.0f, 0.5f, -0.5f, 1.0f };
    const float right[] { 0.25f, 0.25f, 0.25f, 0.25f };
    source.copyFrom(0, 0, left, 4);
    source.copyFrom(1, 0, right, 4);

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(temp.createOutputStream());
    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(2)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    require(writer != nullptr, "synthetic WAV writer opens");
    require(writer->writeFromAudioSampleBuffer(source, 0, 4),
            "synthetic WAV writes");
    writer.reset();

    const auto headerOnly = AudioInspectionService::inspect(temp);
    require(headerOnly.opened && !headerOnly.sampleScanCompleted,
            "header inspect stays lightweight");
    require(headerOnly.info.numChannels == 2,
            "stereo metadata is reported");
    require(headerOnly.info.channelLayout.selectedRoles.size() == 2,
            "stereo roles are reported");

    const auto scanned = AudioInspectionService::inspect(temp, { true, 2 });
    require(scanned.opened && scanned.sampleScanCompleted,
            "sample scan completes in bounded blocks");
    require(scanned.samplesScannedPerChannel == 4,
            "all samples are counted once");
    require(std::abs(scanned.channels[0].peak - 1.0) < 0.001,
            "left peak is exact within PCM tolerance");
    require(std::abs(scanned.channels[0].rms - std::sqrt(0.375)) < 0.001,
            "left RMS is correct");
    require(std::abs(scanned.channels[1].rms - 0.25) < 0.001,
            "right RMS is correct");
    require(!scanned.hasNonFiniteSamples(),
            "valid PCM has no non-finite samples");

    const auto json = scanned.toJson();
    require(static_cast<int>(json["num_channels"]) == 2,
            "JSON exposes channel count");
    require(json["channel_layout"]["roles"].size() == 2,
            "JSON exposes channel roles");
    require(json["channels"].size() == 2,
            "JSON exposes per-channel statistics");

    const auto missing = AudioInspectionService::inspect(temp.getSiblingFile("missing.wav"));
    require(!missing.opened && missing.errorMessage.isNotEmpty(),
            "missing file fails explicitly");
    require(missing.toJson()["input"].toString().endsWith("missing.wav"),
            "missing input keeps its requested path");
    temp.deleteFile();

    const auto largeTemp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("single-editor-cli-large-mapped",
                                                        ".wav",
                                                        false);
    const auto largeSamples = writeLargeMappedWavFile(largeTemp);
    require(largeSamples > 0 && largeTemp.getSize() > 64LL * 1024LL * 1024LL,
            "large fixture exceeds eager mmap threshold");
    const auto largeScan = AudioInspectionService::inspect(largeTemp,
                                                            { true, 65536 });
    require(largeScan.opened && largeScan.sampleScanCompleted,
            "section-mapped reader scans every bounded block");
    require(largeScan.samplesScannedPerChannel == largeSamples,
            "large section-mapped scan reaches the final sample");
    largeTemp.deleteFile();
}

void runBuiltInOperationTests()
{
    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    require(registry.descriptors().size() == 8,
            "CLI registers read-only inspection plus F2 edit analysis operations");

    const auto* inspect = registry.find("audio.inspect.v1");
    const auto* trimSilence = registry.find("trim-silence.v1");
    const auto* validate = registry.find("audio.validate.v1");
    const auto* routing = registry.find("audio.routing.inspect.v1");
    const auto* gain = registry.find("gain.v1");
    const auto* fade = registry.find("fade.v1");
    const auto* normalize = registry.find("normalize.peak.v1");
    const auto* pipeline = registry.find("edit-pipeline.v1");
    require(inspect != nullptr && trimSilence != nullptr
                && validate != nullptr && routing != nullptr
                && gain != nullptr && fade != nullptr
                && normalize != nullptr && pipeline != nullptr,
            "inspection and F2 operation ids are stable");

    require(gain->descriptor().mode == AudioOperationMode::analyze
                && fade->descriptor().mode == AudioOperationMode::analyze
                && normalize->descriptor().mode == AudioOperationMode::analyze
                && pipeline->descriptor().mode == AudioOperationMode::analyze,
            "F2 edit operations remain read-only during analyze");

    const auto trimSchema = trimSilence->descriptor().parameterSchema;
    require(trimSilence->descriptor().mode == AudioOperationMode::analyze
                && static_cast<double>(
                       trimSchema["properties"]["threshold_dbfs"]["default"])
                       == -60.0
                && static_cast<bool>(
                       trimSchema["properties"]["head_only"]["default"]),
            "trim-silence exposes its versioned analyze contract");

    const auto routingSchema = routing->descriptor().parameterSchema;
    require(routingSchema["properties"]["output_channels"]["type"].toString() == "integer"
                && routingSchema["properties"]["output_channels"]["enum"].size() == 6,
            "routing output channel schema is machine-readable");
    require(routingSchema["properties"]["source_preset"]["default"].toString() == "auto"
                && !static_cast<bool>(routingSchema["properties"]["include_lfe"]["default"]),
            "routing optional parameter defaults are machine-readable");
    require(routingSchema["properties"]["output_map"]["type"].toString() == "array"
                && routingSchema["properties"]["mute_channels"]["type"].toString() == "array"
                && routingSchema["properties"]["solo_channels"]["type"].toString() == "array",
            "routing F3 mapping and solo-mute schemas are machine-readable");
    require(routingSchema["properties"]["ambisonic_yaw"]["type"].toString() == "number"
                && static_cast<double>(routingSchema["properties"]["ambisonic_pitch"]["minimum"])
                       == -90.0
                && static_cast<double>(routingSchema["properties"]["ambisonic_roll"]["maximum"])
                       == 180.0,
            "routing schema exposes bounded Ambisonic orientation parameters");
    require(routingSchema["properties"]["ambisonic_monitor"]["type"].toString() == "string"
                && routingSchema["properties"]["ambisonic_monitor"]["enum"].size() == 2
                && routingSchema["properties"]["ambisonic_monitor"]["default"].toString()
                       == "speaker",
            "routing schema exposes the lazy speaker or binaural Ambisonic monitor mode");
    require(routingSchema["properties"]["runtime_probe"]["type"].toString() == "boolean"
                && !static_cast<bool>(routingSchema["properties"]["runtime_probe"]["default"])
                && static_cast<double>(routingSchema["properties"]["probe_start_seconds"]["minimum"])
                       == 0.0,
            "routing schema exposes an opt-in headless runtime render probe");

    AudioOperationRequest empty;
    require(inspect->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "inspect rejects empty input");
    require(validate->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "validate rejects empty input");
    require(trimSilence->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "trim-silence rejects empty input");
    require(routing->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "routing rejects empty input");
    require(gain->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "gain analysis rejects empty input");
    require(fade->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "fade analysis rejects empty input");
    require(normalize->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "normalize analysis rejects empty input");
    require(pipeline->execute(empty).exitCode == AutomationExitCode::invalidArguments,
            "pipeline analysis rejects empty input");
}

void runCliArgumentTests()
{
    auto parsed = parseCliArguments({ "operations", "--json" });
    require(parsed.isValid()
                && parsed.command == CliCommand::operations
                && parsed.json,
            "operations JSON command parses");

    parsed = parseCliArguments(
        { "inspect", "--input", "test.wav", "--sample-stats", "--json" });
    require(parsed.isValid() && parsed.command == CliCommand::inspect,
            "inspect command parses");
    require(parsed.inputs.size() == 1 && parsed.sampleStats,
            "inspect options parse");

    parsed = parseCliArguments(
        { "validate", "--input", "a.wav", "--input", "b.wav" });
    require(parsed.isValid() && parsed.inputs.size() == 2,
            "validate accepts repeated inputs");

    parsed = parseCliArguments(
        { "analyze", "trim-silence", "--input", "test.wav", "--head-only",
          "--threshold-dbfs", "-54", "--plan", "trim-plan.json", "--dry-run",
          "--json" });
    require(parsed.isValid() && parsed.command == CliCommand::analyze,
            "analyze trim-silence command parses");
    require(parsed.operationId == "trim-silence.v1"
                && parsed.headOnly
                && std::abs(parsed.thresholdDbfs + 54.0) < 0.000001
                && parsed.planFile.getFileName() == "trim-plan.json"
                && parsed.dryRun,
            "analyze plan and dry-run options parse");

    parsed = parseCliArguments({ "analyze", "--input", "test.wav" });
    require(!parsed.isValid(), "analyze requires an operation id");

    parsed = parseCliArguments(
        { "analyze", "trim-silence", "--input", "test.wav",
          "--threshold-dbfs", "1" });
    require(!parsed.isValid(), "trim-silence rejects a positive threshold");

    parsed = parseCliArguments(
        { "analyze", "gain", "--input", "test.wav", "--gain-db", "-6" });
    require(parsed.isValid() && parsed.operationId == "gain.v1",
            "gain analysis command parses");

    parsed = parseCliArguments(
        { "analyze", "fade", "--input", "test.wav",
          "--fade-in-seconds", "0.1", "--fade-out-seconds", "0.2" });
    require(parsed.isValid() && parsed.operationId == "fade.v1",
            "fade analysis command parses");

    parsed = parseCliArguments(
        { "analyze", "normalize-peak", "--input", "test.wav",
          "--target-dbfs", "-1" });
    require(parsed.isValid() && parsed.operationId == "normalize.peak.v1",
            "peak normalize analysis command parses");

    parsed = parseCliArguments(
        { "analyze", "pipeline", "--input", "test.wav",
          "--pipeline", "edits.json" });
    require(parsed.isValid() && parsed.operationId == "edit-pipeline.v1",
            "versioned edit pipeline command parses");

    parsed = parseCliArguments(
        { "analyze", "gain", "--input", "test.wav", "--gain-db", "30" });
    require(!parsed.isValid(), "gain rejects values above the safe range");

    parsed = parseCliArguments(
        { "analyze", "gain", "--input", "test.wav", "--gain-db", "-6",
          "--threshold-dbfs", "-60" });
    require(!parsed.isValid(),
            "gain rejects trim-silence parameters instead of silently ignoring them");

    parsed = parseCliArguments(
        { "analyze", "fade", "--input", "test.wav" });
    require(!parsed.isValid(), "fade requires at least one non-zero duration");

    parsed = parseCliArguments(
        { "analyze", "trim-silence", "--input", "--dry-run" });
    require(!parsed.isValid(), "input never consumes the next option as a path");

    parsed = parseCliArguments(
        { "inspect", "--input", "test.wav", "--head-only" });
    require(!parsed.isValid(), "inspect rejects analyze-only flags");

    parsed = parseCliArguments(
        { "analyze", "trim-silence", "--input", "test.wav",
          "--plan", "test.wav" });
    require(!parsed.isValid(), "plan cannot overwrite its input audio file");

    const auto wildcardDirectory = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getNonexistentChildFile(
            "single-editor-cli-wildcard", {}, false);
    require(wildcardDirectory.createDirectory(), "creates wildcard fixture directory");
    require(wildcardDirectory.getChildFile("b.wav").create()
                && wildcardDirectory.getChildFile("a.wav").create()
                && wildcardDirectory.getChildFile("ignore.txt").create(),
            "creates wildcard fixture files");
    parsed = parseCliArguments(
        { "analyze", "trim-silence", "--input",
          wildcardDirectory.getChildFile("*.wav").getFullPathName() });
    require(parsed.isValid() && parsed.inputs.size() == 2
                && parsed.inputs[0].getFileName() == "a.wav"
                && parsed.inputs[1].getFileName() == "b.wav",
            "analyze expands wildcard inputs in deterministic order");
    wildcardDirectory.deleteRecursively();

    parsed = parseCliArguments(
        { "routing", "--input", "test.wav", "--output-channels", "8",
          "--source-preset", "7.1-microsoft-wave", "--include-lfe",
          "--output-map", "Lss=7", "--output-map", "Lsr=5",
          "--mute-channel", "2", "--solo-channel", "7",
          "--monitor-mode", "multichannel",
          "--ambisonic-monitor", "binaural",
          "--ambisonic-yaw", "90", "--ambisonic-pitch", "-20",
          "--ambisonic-roll", "15", "--runtime-probe",
          "--probe-start-seconds", "0.125", "--json" });
    require(parsed.isValid() && parsed.command == CliCommand::routing,
            "routing command parses");
    require(parsed.outputChannels == 8
                && parsed.sourcePresetId == "7.1-microsoft-wave"
                && parsed.includeLfe,
            "routing options parse");
    require(parsed.outputMappings.size() == 2
                && parsed.mutedSourceChannels.contains(1)
                && parsed.soloedSourceChannels.contains(6)
                && parsed.monitorMode == "multichannel"
                && parsed.ambisonicMonitorMode == "binaural"
                && parsed.ambisonicOrientationSpecified
                && std::abs(parsed.ambisonicOrientation.yawDegrees - 90.0f) < 0.001f
                && std::abs(parsed.ambisonicOrientation.pitchDegrees + 20.0f) < 0.001f
                && std::abs(parsed.ambisonicOrientation.rollDegrees - 15.0f) < 0.001f
                && parsed.runtimeProbe
                && std::abs(parsed.probeStartSeconds - 0.125) < 0.000001,
            "routing output map, mute, solo, monitor mode, orientation, and runtime probe parse");

    parsed = parseCliArguments(
        { "routing", "--input", "test.wav", "--output-channels", "2",
          "--ambisonic-monitor", "virtual" });
    require(!parsed.isValid(), "routing rejects an unknown Ambisonic monitor mode");

    parsed = parseCliArguments(
        { "routing", "--input", "test.wav", "--output-channels", "2",
          "--probe-start-seconds", "-1" });
    require(!parsed.isValid(), "routing rejects a negative runtime probe start");

    parsed = parseCliArguments(
        { "routing", "--input", "test.wav", "--output-channels", "2",
          "--ambisonic-pitch", "120" });
    require(!parsed.isValid(), "routing rejects Ambisonic pitch outside -90 to 90 degrees");

    parsed = parseCliArguments(
        { "routing", "--input", "test.wav", "--output-channels", "3" });
    require(!parsed.isValid(), "routing rejects unsupported output channel count");

    parsed = parseCliArguments(
        { "apply", "--plan", "trim-plan.json", "--in-place",
          "--dry-run", "--json" });
    require(parsed.isValid()
                && parsed.command == CliCommand::apply
                && parsed.planFile.getFileName() == "trim-plan.json"
                && parsed.dryRun
                && parsed.inPlace
                && parsed.inputs.isEmpty(),
            "apply dry-run parses an explicit in-place policy without direct inputs");

    parsed = parseCliArguments(
        { "apply", "--plan", "trim-plan.json", "--output-dir", "out",
          "--on-conflict", "rename", "--backup", "--backup-dir", "backup",
          "--allow-lossy-reencode", "--fail-fast", "--json" });
    require(parsed.isValid()
                && !parsed.inPlace
                && parsed.outputDirectory.getFileName() == "out"
                && parsed.conflictPolicy == "rename"
                && parsed.backup
                && parsed.backupDirectory.getFileName() == "backup"
                && parsed.allowLossyReencode
                && parsed.failFast,
            "writable apply parses explicit output, conflict, backup, and lossy policies");

    parsed = parseCliArguments({ "apply", "--plan", "trim-plan.json", "--json" });
    require(!parsed.isValid()
                && parsed.errorCode == AutomationExitCode::invalidArguments,
            "apply requires exactly one output policy");

    parsed = parseCliArguments(
        { "apply", "--plan", "trim-plan.json", "--in-place",
          "--output-dir", "out", "--json" });
    require(!parsed.isValid(), "apply rejects conflicting output policies");

    parsed = parseCliArguments({ "apply", "--dry-run", "--json" });
    require(!parsed.isValid()
                && parsed.errorCode == AutomationExitCode::invalidArguments,
            "apply dry-run requires a plan");

    parsed = parseCliArguments(
        { "inspect", "--output-dir", "out", "--input", "test.wav" });
    require(!parsed.isValid()
                && parsed.errorCode == AutomationExitCode::unsupported,
            "F1 write flags are explicitly unsupported");

    parsed = parseCliArguments({ "inspect" });
    require(!parsed.isValid()
                && parsed.errorCode == AutomationExitCode::invalidArguments,
            "inspect requires input");
}

void runCliDispatchTests()
{
    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);

    auto execution = executeCli(
        parseCliArguments({ "operations", "--json" }), registry);
    require(execution.exitCode == AutomationExitCode::success,
            "operations succeeds");
    auto json = juce::JSON::parse(execution.stdoutText);
    require(json["schema"].toString() == "single-editor.cli.v1",
            "schema is stable");
    require(json["command"].toString() == "operations",
            "command is stable");
    require(json["operations"].size() == 8,
            "operations lists inspection and F2 analyze descriptors");

    execution = executeCli(
        parseCliArguments({ "apply", "--json" }), registry);
    require(execution.exitCode == AutomationExitCode::invalidArguments,
            "missing apply contract preserves invalid-argument exit code");
    json = juce::JSON::parse(execution.stdoutText);
    require(json["command"].toString() == "apply",
            "error envelope preserves requested command");
    require(!static_cast<bool>(json["ok"]),
            "error envelope reports ok false");
    require(static_cast<int>(json["exit_code"]) == 2,
            "error envelope reports stable code");
}

void runAnalyzePlanTests()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-cli-trim", ".wav", false);
    const auto plan = temp.getSiblingFile(temp.getFileNameWithoutExtension() + "-plan.json");
    temp.deleteFile();
    plan.deleteFile();
    require(writeTrimSilenceWavFile(temp), "writes trim-silence fixture");

    juce::MemoryBlock originalBytes;
    require(temp.loadFileAsData(originalBytes), "reads original trim fixture bytes");

    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence", "--input",
                            temp.getFullPathName(), "--head-only", "--plan",
                            plan.getFullPathName(), "--dry-run", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "trim-silence dry-run succeeds");
    require(!plan.existsAsFile(), "dry-run never writes the requested plan file");

    auto json = juce::JSON::parse(execution.stdoutText);
    const auto generatedPlan = json["plan"];
    require(generatedPlan["schema"].toString() == "single-editor.analysis-plan.v1"
                && static_cast<int>(generatedPlan["schema_version"]) == 1,
            "analyze emits a versioned plan schema");
    require(generatedPlan["manifest"]["schema"].toString()
                == "single-editor.automation-manifest.v1"
                && generatedPlan["manifest"]["cli_version"].toString().isNotEmpty()
                && generatedPlan["manifest"]["status"].toString() == "planned",
            "analyze embeds a versioned planned manifest");
    require(generatedPlan["operation"]["id"].toString() == "trim-silence.v1"
                && static_cast<int>(generatedPlan["operation"]["schema_version"]) == 1,
            "plan records the operation contract");
    const auto item = generatedPlan["items"][0];
    require(item["status"].toString() == "planned",
            "successful analysis item is planned");
    require(item["input_identity"]["canonical_path"].toString().isNotEmpty()
                && static_cast<juce::int64>(item["input_identity"]["size_bytes"])
                       == temp.getSize(),
            "plan records stable lightweight file identity");
#if JUCE_WINDOWS
    require(item["input_identity"]["filesystem_file_id"].toString().isNotEmpty(),
            "Windows plan records the filesystem file id");
#endif
    const auto trimEnd = static_cast<juce::int64>(
        item["suggested_sample_range"]["end_sample"]);
    require(trimEnd >= 4500 && trimEnd <= 4620,
            "trim plan preserves about 5 ms before the first content");
    require(static_cast<juce::int64>(item["estimated_output"]["num_samples"])
                == 9600 - trimEnd,
            "plan estimates the post-trim sample count");

    juce::MemoryBlock dryRunBytes;
    require(temp.loadFileAsData(dryRunBytes) && dryRunBytes == originalBytes,
            "dry-run leaves the source audio byte-for-byte unchanged");

    execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence", "--input",
                            temp.getFullPathName(), "--plan", plan.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success && plan.existsAsFile(),
            "analyze atomically writes a requested plan file");
    const auto savedPlan = juce::JSON::parse(plan.loadFileAsString());
    require(savedPlan["schema"].toString() == "single-editor.analysis-plan.v1"
                && savedPlan["manifest"]["status"].toString() == "planned",
            "saved plan preserves the versioned schema and manifest state");

    juce::MemoryBlock analyzedBytes;
    require(temp.loadFileAsData(analyzedBytes) && analyzedBytes == originalBytes,
            "analyze writes only the plan and never modifies source audio");
    plan.deleteFile();
    temp.deleteFile();
}

void runPlanPreflightTests()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-cli-preflight", ".wav", false);
    const auto plan = temp.getSiblingFile(temp.getFileNameWithoutExtension()
                                          + "-plan.json");
    temp.deleteFile();
    plan.deleteFile();
    require(writeTrimSilenceWavFile(temp), "writes preflight fixture");

    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence", "--input",
                            temp.getFullPathName(), "--plan", plan.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success && plan.existsAsFile(),
            "writes a plan for preflight");

    juce::MemoryBlock sourceBefore;
    juce::MemoryBlock planBefore;
    require(temp.loadFileAsData(sourceBefore) && plan.loadFileAsData(planBefore),
            "captures source and plan before preflight");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--in-place", "--dry-run", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "current plan passes dry-run preflight");
    auto json = juce::JSON::parse(execution.stdoutText);
    require(json["command"].toString() == "apply"
                && json["operation"].toString() == "trim-silence.v1"
                && static_cast<bool>(json["dry_run"])
                && !static_cast<bool>(json["write_performed"]),
            "preflight reports apply semantics without writing");
    require(json["preflight"]["schema"].toString()
                == "single-editor.preflight.v1"
                && static_cast<bool>(json["preflight"]["ready"])
                && json["results"][0]["status"].toString() == "ready",
            "preflight emits a stable ready result");

    juce::MemoryBlock sourceAfter;
    juce::MemoryBlock planAfter;
    require(temp.loadFileAsData(sourceAfter) && sourceAfter == sourceBefore
                && plan.loadFileAsData(planAfter) && planAfter == planBefore,
            "dry-run preflight leaves source and plan byte-for-byte unchanged");

    const auto invalidPlan = plan.getSiblingFile(
        plan.getFileNameWithoutExtension() + "-invalid.json");
    invalidPlan.deleteFile();
    auto invalidPlanJson = juce::JSON::parse(plan.loadFileAsString());
    invalidPlanJson.getDynamicObject()->setProperty("schema", "unknown.plan.v9");
    require(invalidPlan.replaceWithText(juce::JSON::toString(invalidPlanJson, true)),
            "writes invalid plan fixture");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", invalidPlan.getFullPathName(),
                            "--in-place", "--dry-run", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::invalidArguments
                && !static_cast<bool>(juce::JSON::parse(execution.stdoutText)
                                          ["preflight"]["ready"]),
            "unsupported plan schema fails safely before identity checks");
    invalidPlan.deleteFile();

    const auto second = temp.getSiblingFile(
        temp.getFileNameWithoutExtension() + "-second.wav");
    const auto batchPlan = temp.getSiblingFile(
        temp.getFileNameWithoutExtension() + "-batch-plan.json");
    second.deleteFile();
    batchPlan.deleteFile();
    require(writeTrimSilenceWavFile(second), "writes second preflight fixture");
    execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence",
                            "--input", temp.getFullPathName(),
                            "--input", second.getFullPathName(),
                            "--plan", batchPlan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success
                && batchPlan.existsAsFile(),
            "writes a two-item preflight plan");
    require(second.deleteFile(), "removes one planned input externally");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", batchPlan.getFullPathName(),
                            "--in-place", "--dry-run", "--json" }),
        registry);
    json = juce::JSON::parse(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::partialFailure
                && static_cast<int>(json["summary"]["ready"]) == 1
                && static_cast<int>(json["summary"]["failed"]) == 1,
            "mixed ready and stale inputs return partial failure");
    batchPlan.deleteFile();

    require(temp.appendText("identity-change"), "changes the input identity");
    juce::MemoryBlock changedBefore;
    require(temp.loadFileAsData(changedBefore), "captures externally changed input");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--in-place", "--dry-run", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::staleInput,
            "changed input fails preflight with STALE_INPUT");
    json = juce::JSON::parse(execution.stdoutText);
    require(!static_cast<bool>(json["preflight"]["ready"])
                && json["results"][0]["status"].toString() == "stale"
                && json["results"][0]["error_code"].toString() == "STALE_INPUT",
            "stale preflight explains the identity failure");
    juce::MemoryBlock changedAfter;
    require(temp.loadFileAsData(changedAfter) && changedAfter == changedBefore,
            "failed preflight never modifies the changed source");

    plan.deleteFile();
    temp.deleteFile();
}

void runPlanApplyTests()
{
    const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("single-editor-cli-apply", ".wav", false);
    const auto plan = source.getSiblingFile(source.getFileNameWithoutExtension()
                                             + "-plan.json");
    const auto backupDirectory = source.getSiblingFile(source.getFileNameWithoutExtension()
                                                        + "-backups");
    source.deleteFile();
    plan.deleteFile();
    backupDirectory.deleteRecursively();
    require(writeTrimSilenceWavFile(source), "writes writable apply fixture");

    juce::MemoryBlock sourceBefore;
    require(source.loadFileAsData(sourceBefore), "captures source before writable apply");

    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence", "--input",
                            source.getFullPathName(), "--plan", plan.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "writes plan for writable apply");
    const auto planJson = juce::JSON::parse(plan.loadFileAsString());
    const auto expectedSamples = static_cast<juce::int64>(
        planJson["items"][0]["estimated_output"]["num_samples"]);
    juce::MemoryBlock planBefore;
    require(plan.loadFileAsData(planBefore), "captures immutable plan bytes");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--in-place", "--backup-dir",
                            backupDirectory.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "writable in-place apply succeeds");
    const auto json = juce::JSON::parse(execution.stdoutText);
    require(static_cast<bool>(json["write_performed"])
                && !static_cast<bool>(json["dry_run"])
                && json["results"][0]["status"].toString() == "committed",
            "apply reports a real atomic commit");

    const auto outputInfo = SingleTrackDocument::probeFileInfo(source);
    require(outputInfo.isValid() && outputInfo.numSamples == expectedSamples,
            "trim apply writes the sample-exact planned range");

    const auto backup = juce::File(json["results"][0]["backup"].toString());
    juce::MemoryBlock backupBytes;
    require(backup.existsAsFile()
                && backup.loadFileAsData(backupBytes)
                && backupBytes == sourceBefore,
            "explicit backup preserves the original source byte-for-byte");
    juce::MemoryBlock planAfter;
    require(plan.loadFileAsData(planAfter) && planAfter == planBefore,
            "writable apply leaves the analysis plan immutable");

    backupDirectory.deleteRecursively();
    plan.deleteFile();
    source.deleteFile();
}

void runApplyOutputPolicyTests()
{
    const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("single-editor-cli-output", ".wav", false);
    const auto plan = source.getSiblingFile(source.getFileNameWithoutExtension()
                                             + "-plan.json");
    const auto outputDirectory = source.getSiblingFile(source.getFileNameWithoutExtension()
                                                        + "-outputs");
    source.deleteFile();
    plan.deleteFile();
    outputDirectory.deleteRecursively();
    require(writeTrimSilenceWavFile(source), "writes output-policy fixture");

    juce::MemoryBlock sourceBefore;
    require(source.loadFileAsData(sourceBefore), "captures output-policy input bytes");
    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "analyze", "trim-silence", "--input",
                            source.getFullPathName(), "--plan", plan.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "writes output-policy plan");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--output-dir", outputDirectory.getFullPathName(),
                            "--json" }),
        registry);
    auto json = juce::JSON::parse(execution.stdoutText);
    const auto output = juce::File(json["results"][0]["output"].toString());
    juce::MemoryBlock sourceAfter;
    require(execution.exitCode == AutomationExitCode::success
                && output.existsAsFile()
                && source.loadFileAsData(sourceAfter)
                && sourceAfter == sourceBefore,
            "output-directory apply commits a derivative without touching input");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--output-dir", outputDirectory.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed
                && juce::JSON::parse(execution.stdoutText)["results"][0]
                       ["error_code"].toString() == "OUTPUT_EXISTS",
            "output conflict fails by default");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--output-dir", outputDirectory.getFullPathName(),
                            "--on-conflict", "rename", "--json" }),
        registry);
    json = juce::JSON::parse(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && juce::File(json["results"][0]["output"].toString()).existsAsFile()
                && juce::File(json["results"][0]["output"].toString()) != output,
            "rename conflict policy creates a distinct deterministic output");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--output-dir", source.getParentDirectory().getFullPathName(),
                            "--on-conflict", "overwrite", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed
                && juce::JSON::parse(execution.stdoutText)["results"][0]
                       ["error_code"].toString() == "OUTPUT_EQUALS_INPUT",
            "output mode never aliases the source even with overwrite requested");

    auto unsupportedPlan = juce::JSON::parse(plan.loadFileAsString());
    juce::Array<juce::var> unsupportedMetadata;
    unsupportedMetadata.add("private-unknown-chunk");
    unsupportedPlan["items"][0]["metadata"].getDynamicObject()->setProperty(
        "unsupported", juce::var(unsupportedMetadata));
    require(plan.replaceWithText(juce::JSON::toString(unsupportedPlan, true)),
            "marks metadata as unsupported in apply plan");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--in-place", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed,
            "unsupported metadata returns processing failure");
    require(juce::JSON::parse(execution.stdoutText)["results"][0]
                ["error_code"].toString() == "METADATA_UNSUPPORTED",
            "unsupported metadata reports a stable error code");
    juce::MemoryBlock metadataSourceAfter;
    require(source.loadFileAsData(metadataSourceAfter)
                && metadataSourceAfter == sourceBefore,
            "strict metadata policy leaves the source byte-for-byte unchanged");

    outputDirectory.deleteRecursively();
    plan.deleteFile();
    source.deleteFile();
}

void runEditPipelineTests()
{
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("single-editor-cli-f2", {}, false);
    const auto source = directory.getChildFile("constant.wav");
    const auto inPlaceSource = directory.getChildFile("constant-in-place.wav");
    const auto pipeline = directory.getChildFile("pipeline.json");
    const auto invalidPipeline = directory.getChildFile("invalid-pipeline.json");
    const auto plan = directory.getChildFile("pipeline-plan.json");
    const auto outputDirectory = directory.getChildFile("output");
    directory.deleteRecursively();
    require(directory.createDirectory(), "creates F2 fixture directory");
    require(writeEditPipelineWavFile(source)
                && source.copyFileTo(inPlaceSource),
            "writes F2 gain/fade/normalize fixtures");

    auto* pipelineRoot = new juce::DynamicObject();
    pipelineRoot->setProperty("schemaVersion", 1);
    juce::Array<juce::var> operations;
    auto* gain = new juce::DynamicObject();
    gain->setProperty("id", "gain.v1");
    gain->setProperty("gainDb", -6.020599913);
    operations.add(juce::var(gain));
    auto* fadeIn = new juce::DynamicObject();
    fadeIn->setProperty("id", "fade.v1");
    fadeIn->setProperty("direction", "in");
    fadeIn->setProperty("durationSeconds", 0.1);
    operations.add(juce::var(fadeIn));
    auto* fadeOut = new juce::DynamicObject();
    fadeOut->setProperty("id", "fade.v1");
    fadeOut->setProperty("direction", "out");
    fadeOut->setProperty("durationSeconds", 0.1);
    operations.add(juce::var(fadeOut));
    auto* normalize = new juce::DynamicObject();
    normalize->setProperty("id", "normalize.peak.v1");
    normalize->setProperty("targetDbfs", -6.0);
    operations.add(juce::var(normalize));
    pipelineRoot->setProperty("operations", juce::var(operations));
    require(pipeline.replaceWithText(
                juce::JSON::toString(juce::var(pipelineRoot), true)),
            "writes a versioned F2 pipeline");

    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "analyze", "fade", "--input",
                            source.getFullPathName(), "--fade-in-seconds", "2",
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed,
            "per-file analysis rejects a fade longer than the output clip");

    execution = executeCli(
        parseCliArguments({ "analyze", "pipeline", "--input",
                            source.getFullPathName(), "--pipeline",
                            pipeline.getFullPathName(), "--plan",
                            plan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success
                && plan.existsAsFile(),
            "pipeline analyze writes an immutable plan");
    auto json = juce::JSON::parse(execution.stdoutText);
    require(json["operation"].toString() == "edit-pipeline.v1"
                && json["plan"]["items"][0]["render_pipeline"]["operations"].size() == 4,
            "pipeline plan contains four resolved render stages");
    require(static_cast<double>(
                json["plan"]["items"][0]["analysis"]["measured_peak"]) > 0.12
                && static_cast<double>(
                       json["plan"]["items"][0]["analysis"]["resolved_peak"]) > 0.49,
            "normalize resolves the post-gain/fade peak during read-only analysis");

    const auto tamperedPlan = directory.getChildFile("tampered-plan.json");
    auto tamperedPlanJson = juce::JSON::parse(plan.loadFileAsString());
    tamperedPlanJson["items"][0]["render_pipeline"]["operations"][0]
        .getDynamicObject()->setProperty("unknown_future_parameter", 1);
    require(tamperedPlan.replaceWithText(
                juce::JSON::toString(tamperedPlanJson, true)),
            "writes a tampered resolved pipeline fixture");
    auto tamperedExecution = executeCli(
        parseCliArguments({ "apply", "--plan", tamperedPlan.getFullPathName(),
                            "--in-place", "--dry-run", "--json" }),
        registry);
    require(tamperedExecution.exitCode == AutomationExitCode::invalidArguments,
            "preflight rejects unknown fields in a tampered resolved pipeline");

    execution = executeCli(
        parseCliArguments({ "apply", "--plan", plan.getFullPathName(),
                            "--output-dir", outputDirectory.getFullPathName(),
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "out-of-place F2 pipeline apply succeeds");
    json = juce::JSON::parse(execution.stdoutText);
    const auto output = juce::File(json["results"][0]["output"].toString());
    const auto rendered = readAudioFile(output);
    require(rendered.getNumChannels() == 1 && rendered.getNumSamples() == 1000,
            "F2 pipeline preserves format and duration");
    require(std::abs(rendered.getSample(0, 0)) < 0.0001f
                && std::abs(rendered.getSample(0, 999)) < 0.0001f,
            "F2 pipeline renders sample-exact fade endpoints");
    require(std::abs(rendered.getMagnitude(0, rendered.getNumSamples())
                     - juce::Decibels::decibelsToGain(-6.0f)) < 0.001f,
            "F2 pipeline reaches the analyzed peak normalization target");

    const auto inPlacePlan = directory.getChildFile("in-place-plan.json");
    execution = executeCli(
        parseCliArguments({ "analyze", "gain", "--input",
                            inPlaceSource.getFullPathName(), "--gain-db", "-6",
                            "--plan", inPlacePlan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "single gain analysis writes a reusable F2 plan");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", inPlacePlan.getFullPathName(),
                            "--in-place", "--json" }),
        registry);
    const auto inPlaceRendered = readAudioFile(inPlaceSource);
    require(execution.exitCode == AutomationExitCode::success
                && std::abs(inPlaceRendered.getMagnitude(0, inPlaceRendered.getNumSamples())
                            - 0.125f) < 0.001f,
            "in-place F2 gain apply uses the existing atomic save path");

    const auto noOpSource = directory.getChildFile("constant-no-op.wav");
    const auto noOpPlan = directory.getChildFile("no-op-plan.json");
    require(source.copyFileTo(noOpSource), "copies an F2 no-op fixture");
    juce::MemoryBlock noOpBefore;
    require(noOpSource.loadFileAsData(noOpBefore), "captures no-op source bytes");
    execution = executeCli(
        parseCliArguments({ "analyze", "gain", "--input",
                            noOpSource.getFullPathName(), "--gain-db", "0",
                            "--plan", noOpPlan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success
                && juce::JSON::parse(execution.stdoutText)["plan"]["items"][0]
                       ["status"].toString() == "skipped",
            "zero-gain analysis records a no-change plan item");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", noOpPlan.getFullPathName(),
                            "--in-place", "--json" }),
        registry);
    juce::MemoryBlock noOpAfter;
    json = juce::JSON::parse(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && !static_cast<bool>(json["write_performed"])
                && json["results"][0]["status"].toString() == "skipped"
                && noOpSource.loadFileAsData(noOpAfter)
                && noOpAfter == noOpBefore,
            "no-change apply skips the writer and preserves source bytes");

    const auto batchA = directory.getChildFile("batch-a.wav");
    const auto batchB = directory.getChildFile("batch-b.wav");
    const auto batchPlan = directory.getChildFile("batch-plan.json");
    const auto batchOutput = directory.getChildFile("batch-output");
    require(source.copyFileTo(batchA) && source.copyFileTo(batchB),
            "copies two F2 batch fixtures");
    execution = executeCli(
        parseCliArguments({ "analyze", "gain", "--input", batchA.getFullPathName(),
                            "--input", batchB.getFullPathName(), "--gain-db", "-12",
                            "--plan", batchPlan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "F2 batch analysis plans every input");
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", batchPlan.getFullPathName(),
                            "--output-dir", batchOutput.getFullPathName(), "--json" }),
        registry);
    json = juce::JSON::parse(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && static_cast<int>(json["summary"]["committed"]) == 2
                && batchOutput.getChildFile("batch-a.wav").existsAsFile()
                && batchOutput.getChildFile("batch-b.wav").existsAsFile(),
            "F2 out-of-place batch commits each file independently");

    const auto trimSource = directory.getChildFile("trim-and-gain.wav");
    const auto trimPipeline = directory.getChildFile("trim-pipeline.json");
    const auto trimPlan = directory.getChildFile("trim-pipeline-plan.json");
    const auto trimOutput = directory.getChildFile("trim-output");
    require(writeTrimSilenceWavFile(trimSource),
            "writes a combined trim and gain pipeline fixture");
    auto* trimRoot = new juce::DynamicObject();
    trimRoot->setProperty("schemaVersion", 1);
    juce::Array<juce::var> trimOperations;
    auto* trim = new juce::DynamicObject();
    trim->setProperty("id", "trim-silence.v1");
    trim->setProperty("headOnly", true);
    trim->setProperty("thresholdDbfs", -60.0);
    trimOperations.add(juce::var(trim));
    auto* trimGain = new juce::DynamicObject();
    trimGain->setProperty("id", "gain.v1");
    trimGain->setProperty("gainDb", -6.020599913);
    trimOperations.add(juce::var(trimGain));
    trimRoot->setProperty("operations", juce::var(trimOperations));
    require(trimPipeline.replaceWithText(
                juce::JSON::toString(juce::var(trimRoot), true)),
            "writes a combined trim and gain pipeline");
    execution = executeCli(
        parseCliArguments({ "analyze", "pipeline", "--input",
                            trimSource.getFullPathName(), "--pipeline",
                            trimPipeline.getFullPathName(), "--plan",
                            trimPlan.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "combined trim and gain analysis succeeds");
    const auto trimPlanJson = juce::JSON::parse(trimPlan.loadFileAsString());
    const auto expectedTrimmedSamples = static_cast<juce::int64>(
        trimPlanJson["items"][0]["estimated_output"]["num_samples"]);
    execution = executeCli(
        parseCliArguments({ "apply", "--plan", trimPlan.getFullPathName(),
                            "--output-dir", trimOutput.getFullPathName(), "--json" }),
        registry);
    const auto trimmed = readAudioFile(trimOutput.getChildFile("trim-and-gain.wav"));
    require(execution.exitCode == AutomationExitCode::success
                && trimmed.getNumSamples() == expectedTrimmedSamples
                && std::abs(trimmed.getMagnitude(0, trimmed.getNumSamples()) - 0.125f)
                       < 0.002f,
            "combined pipeline trims and gains in one document transaction");

    auto* invalidRoot = new juce::DynamicObject();
    invalidRoot->setProperty("schemaVersion", 1);
    juce::Array<juce::var> invalidOperations;
    invalidOperations.add(juce::var(new juce::DynamicObject()));
    invalidOperations.getReference(0).getDynamicObject()->setProperty(
        "id", "normalize.peak.v1");
    invalidOperations.getReference(0).getDynamicObject()->setProperty(
        "targetDbfs", -1.0);
    auto* postNormalizeGain = new juce::DynamicObject();
    postNormalizeGain->setProperty("id", "gain.v1");
    postNormalizeGain->setProperty("gainDb", -3.0);
    invalidOperations.add(juce::var(postNormalizeGain));
    invalidRoot->setProperty("operations", juce::var(invalidOperations));
    require(invalidPipeline.replaceWithText(
                juce::JSON::toString(juce::var(invalidRoot), true)),
            "writes an invalid normalize ordering fixture");
    execution = executeCli(
        parseCliArguments({ "analyze", "pipeline", "--input",
                            source.getFullPathName(), "--pipeline",
                            invalidPipeline.getFullPathName(), "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::invalidArguments
                && !static_cast<bool>(juce::JSON::parse(execution.stdoutText)["ok"]),
            "pipeline validator rejects processing after peak normalize");

    directory.deleteRecursively();
}

void runRoutingCliTests()
{
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-cli-routing", ".wav", false);
    temp.deleteFile();
    require(writeRoutingWavFile(temp), "writes 0x63F routing fixture");

    OperationRegistry registry;
    registerBuiltInReadOnlyOperations(registry);
    auto execution = executeCli(
        parseCliArguments({ "routing", "--input", temp.getFullPathName(),
                            "--output-channels", "8", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "routing command succeeds without an audio device");

    auto json = juce::JSON::parse(execution.stdoutText);
    require(json["operation"].toString() == "audio.routing.inspect.v1",
            "routing operation id is stable");
    const auto result = json["results"][0];
    const auto ordinaryDecodeMatrix = result["ambisonic_decode_matrix"];
    require(ordinaryDecodeMatrix["state"].toString() == "not_required"
                && ordinaryDecodeMatrix["kind"].toString() == "not_required"
                && ordinaryDecodeMatrix["artifact_id"].isVoid()
                && ordinaryDecodeMatrix["cache_key"].isVoid()
                && !static_cast<bool>(ordinaryDecodeMatrix["audio_device_required"]),
            "ordinary routing reports Ambisonic matrix work as not required");
    require(result["source_layout"]["order"].toString() == "L R C LFE Lsr Rsr Lss Rss",
            "routing auto-detects Microsoft WAVE source order");
    require(static_cast<int>(result["source_layout"]["interpretation_schema_version"]) == 1
                && result["source_layout"]["interpretation_origin"].toString()
                       == "wave_channel_mask"
                && static_cast<int64_t>(result["source_layout"]["wave_channel_mask"]) == 1599,
            "routing reports versioned WAV source interpretation provenance");
    require(result["target_layout"]["order"].toString() == "L R C LFE Lss Rss Lsr Rsr",
            "routing uses Windows device output order");
    require(static_cast<int>(result["output_meter"]["channel_count"]) == 8
                && result["output_meter"]["sampling_point"].toString()
                       == "final_device_output_after_ping",
            "CLI exposes the final eight-channel output meter topology");
    require(result["output_meter"]["channels"][3]["role"].toString() == "LFE"
                && !static_cast<bool>(result["output_meter"]["channels"][3]["included_in_global_lufs"]),
            "CLI marks LFE as visible but excluded from standard LUFS");
    require(std::abs(static_cast<double>(result["output_meter"]["channels"][4]["position_weight"])
                     - 1.41) < 0.001,
            "CLI reports the BS.1770 side-surround position weight");
    const auto rearLeftRoute = result["routes"][4];
    require(rearLeftRoute["source_role"].toString() == "Lsr"
                && static_cast<int>(rearLeftRoute["outputs"][0]["output_channel"]) == 7,
            "source ch5 rear-left routes to device output 7");
    const auto sideLeftRoute = result["routes"][6];
    require(sideLeftRoute["source_role"].toString() == "Lss"
                && static_cast<int>(sideLeftRoute["outputs"][0]["output_channel"]) == 5,
            "source ch7 side-left routes to device output 5");
    require(result["downmix_profile"].toString() == "wwise-ac3-preview-v1"
                && static_cast<int>(result["physical_output_matrix"]["schema_version"]) == 1
                && static_cast<int>(result["physical_output_matrix"]["active_output_channels"])
                       == 8,
            "routing exposes the locked downmix profile and physical matrix contract");
    require(result["silent_route_probe"]["method"].toString() == "unit_impulse_matrix"
                && !static_cast<bool>(result["silent_route_probe"]["audio_device_required"])
                && !static_cast<bool>(result["silent_route_probe"]["audible_output_emitted"])
                && result["silent_route_probe"]["role_pings"][4]["source_role"].toString()
                       == "Lsr"
                && static_cast<int>(result["silent_route_probe"]["role_pings"][4]
                                          ["outputs"][0]["physical_output_channel"]) == 7,
            "CLI emits a deterministic silent per-role route proof");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", temp.getFullPathName(),
                            "--output-channels", "8",
                            "--output-map", "Lsr=5", "--output-map", "Rsr=6",
                            "--output-map", "Lss=7", "--output-map", "Rss=8",
                            "--solo-channel", "5", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "routing F3 output map and solo command succeeds");
    json = juce::JSON::parse(execution.stdoutText);
    const auto mappedRearLeft = json["results"][0]["routes"][4];
    require(static_cast<int>(mappedRearLeft["outputs"][0]["output_channel"]) == 5,
            "CLI output map patches rear-left to physical output 5");
    require(!static_cast<bool>(json["results"][0]["routes"][6]["routed"]),
            "CLI solo leaves non-soloed source channels silent");
    require(static_cast<bool>(json["results"][0]["output_mapping_valid"]),
            "CLI reports valid output mapping");
    require(json["results"][0]["physical_output_matrix"]["routes"][4]["role"].toString()
                == "Lss"
                && static_cast<int>(json["results"][0]["physical_output_matrix"]["routes"][4]
                                          ["physical_output_channel"]) == 7,
            "CLI physical matrix preserves canonical role while patching the device output");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", temp.getFullPathName(),
                            "--output-channels", "8", "--source-preset", "7.1-smpte",
                            "--json" }),
        registry);
    json = juce::JSON::parse(execution.stdoutText);
    const auto smpteRoute = json["results"][0]["routes"][4];
    require(smpteRoute["source_role"].toString() == "Lss"
                && static_cast<int>(smpteRoute["outputs"][0]["output_channel"]) == 5,
            "source preset override simulates UI SMPTE selection");
    require(json["results"][0]["source_layout"]["interpretation_origin"].toString()
                == "explicit_preset"
                && static_cast<int64_t>(json["results"][0]["source_layout"]
                                             ["wave_channel_mask"]) == 1599
                && static_cast<int>(json["results"][0]["physical_output_matrix"]["routes"][4]
                                          ["physical_output_channel"]) == 5,
            "source preset override keeps metadata provenance without rewriting physical outputs");
    temp.deleteFile();

    const auto heightTemp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getNonexistentChildFile("single-editor-cli-height-routing", ".wav", false);
    heightTemp.deleteFile();
    require(writeHeightRoutingWavFile(heightTemp), "writes WAVE 7.1.4 routing fixture");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", heightTemp.getFullPathName(),
                            "--output-channels", "12", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "routing command supports a twelve-output device");
    json = juce::JSON::parse(execution.stdoutText);
    const auto heightResult = json["results"][0];
    require(heightResult["source_layout"]["order"].toString()
                == "L R C LFE Lsr Rsr Lss Rss Ltf Rtf Ltr Rtr",
            "routing auto-detects WAVE 7.1.4 source order");
    require(heightResult["target_layout"]["order"].toString()
                == "L R C LFE Lss Rss Lsr Rsr Ltf Rtf Ltr Rtr",
            "routing exposes the twelve-output device order");
    require(static_cast<int>(heightResult["output_meter"]["channel_count"]) == 12
                && heightResult["output_meter"]["channels"][11]["role"].toString() == "Rtr",
            "CLI exposes all twelve physical output meter channels");
    require(heightResult["routes"].size() == 12
                && heightResult["unexpected_unrouted_source_channels"].size() == 0,
            "CLI reports all twelve 7.1.4 source channels as routed");
    require(heightResult["routes"][8]["source_role"].toString() == "Ltf"
                && static_cast<int>(heightResult["routes"][8]["outputs"][0]["output_channel"]) == 9,
            "CLI routes top-front-left to physical output 9");
    require(heightResult["routes"][10]["source_role"].toString() == "Ltr"
                && static_cast<int>(heightResult["routes"][10]["outputs"][0]["output_channel"]) == 11,
            "CLI routes top-rear-left to physical output 11");
    require(heightResult["routes"][11]["source_role"].toString() == "Rtr"
                && static_cast<int>(heightResult["routes"][11]["outputs"][0]["output_channel"]) == 12,
            "CLI routes top-rear-right to physical output 12");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", heightTemp.getFullPathName(),
                            "--output-channels", "8", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "routing command folds 7.1.4 into an eight-output device");
    json = juce::JSON::parse(execution.stdoutText);
    const auto foldedTopRear = json["results"][0]["routes"][10]["outputs"][0];
    require(static_cast<int>(foldedTopRear["output_channel"]) == 7
                && std::abs(static_cast<double>(foldedTopRear["gain"]) - std::sqrt(0.5)) < 0.00001,
            "CLI reports Wwise top-rear to rear-left fold-down at -3 dB");
    const auto foldedTopRearRight = json["results"][0]["routes"][11]["outputs"][0];
    require(static_cast<int>(foldedTopRearRight["output_channel"]) == 8
                && std::abs(static_cast<double>(foldedTopRearRight["gain"]) - std::sqrt(0.5)) < 0.00001,
            "CLI reports symmetric Wwise top-rear to rear-right fold-down");
    heightTemp.deleteFile();

    const auto ambisonicTemp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getNonexistentChildFile("single-editor-cli-foa", ".wav", false);
    ambisonicTemp.deleteFile();
    require(writeFoaFrontWavFile(ambisonicTemp), "writes front-facing FOA fixture");
    execution = executeCli(
        parseCliArguments({ "routing", "--input", ambisonicTemp.getFullPathName(),
                            "--output-channels", "2", "--source-preset", "foa-acn-sn3d",
                            "--ambisonic-yaw", "90", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "explicit FOA routing inspection succeeds with the on-demand decoder");
    json = juce::JSON::parse(execution.stdoutText);
    const auto ambisonicResult = json["results"][0];
    require(static_cast<bool>(ambisonicResult["source_layout"]["ambisonic"])
                && static_cast<int>(ambisonicResult["source_layout"]["ambisonic_order"]) == 1
                && ambisonicResult["source_layout"]["normalization"].toString() == "SN3D",
            "CLI exposes explicit FOA ACN/SN3D semantics");
    require(ambisonicResult["source_layout"]["monitor_decode_status"].toString() == "ready_on_demand",
            "CLI reports the FOA monitor decoder as ready on demand");
    require(ambisonicResult["source_layout"]["monitor_decode_mode"].toString()
                == "canonical_stage_graph_max_re",
            "CLI names the canonical max-rE stage graph instead of the legacy projection");
    require(static_cast<int>(ambisonicResult["source_layout"]["ambisonic_stage_graph_schema"]) == 1
                && ambisonicResult["source_layout"]["ambisonic_stage_order"].toString()
                       == "validate-convert>rotate>analyze>decode-max-re>canonical-speaker-bus>role-mapping>physical-device>meter"
                && ambisonicResult["source_layout"]["ambisonic_decoder_profile"].toString()
                       == "max-re-137.9deg-fixed-reference-v1",
            "CLI exposes versioned stage ordering and decoder policy for verification");
    require(ambisonicResult["source_layout"]["monitor_decode_capability"].toString()
                == "supported"
                && ambisonicResult["source_layout"]["monitor_decode_runtime_state"].toString()
                       == "active",
            "CLI separates decoder capability from the active routing instance");
    const auto standardDecodeMatrix = ambisonicResult["ambisonic_decode_matrix"];
    require(standardDecodeMatrix["state"].toString() == "active"
                && standardDecodeMatrix["kind"].toString() == "standard_offline"
                && static_cast<int>(standardDecodeMatrix["artifact_schema_version"]) == 1
                && standardDecodeMatrix["artifact_id"].toString().isNotEmpty()
                && standardDecodeMatrix["geometry_profile_id"].toString()
                       == "standard-stereo-v1"
                && static_cast<int64_t>(standardDecodeMatrix["geometry_profile_revision"]) == 1
                && standardDecodeMatrix["geometry_profile_hash"].toString().isNotEmpty()
                && standardDecodeMatrix["cache_key"].toString().isNotEmpty()
                && static_cast<bool>(standardDecodeMatrix["generated_offline"])
                && !static_cast<bool>(standardDecodeMatrix["contains_physical_output_binding"]),
            "CLI exposes the immutable standard matrix and route-neutral geometry identity");
    require(static_cast<double>(ambisonicResult["source_layout"]["ambisonic_orientation"]["yaw_degrees"])
                    == 90.0
                && static_cast<double>(ambisonicResult["source_layout"]["ambisonic_orientation"]["pitch_degrees"])
                       == 0.0,
            "CLI reports the active Ambisonic orientation for agent verification");
    require(ambisonicResult["source_layout"]["candidates"].size() == 3
                && ambisonicResult["source_layout"]["candidates"][1]["preset_id"].toString()
                       == "foa-acn-sn3d",
            "CLI exposes selectable channel-order candidates for agent control");
    require(ambisonicResult["unexpected_unrouted_source_channels"].size() == 0
                && static_cast<bool>(ambisonicResult["routes"][0]["routed"])
                && !static_cast<bool>(ambisonicResult["routes"][0]["expected_drop"]),
            "FOA ACN components expose real decoded monitor routes");
    require(static_cast<double>(ambisonicResult["routes"][3]["outputs"][0]["gain"])
                > static_cast<double>(ambisonicResult["routes"][3]["outputs"][1]["gain"]),
            "CLI yaw rotates front ACN3 toward the left stereo output");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", ambisonicTemp.getFullPathName(),
                            "--output-channels", "8", "--source-preset", "foa-acn-sn3d",
                            "--ambisonic-yaw", "90", "--runtime-probe", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "FOA runtime render probe succeeds without opening an audio device");
    json = juce::JSON::parse(execution.stdoutText);
    const auto runtimeProbe = json["results"][0]["runtime_render_probe"];
    require(static_cast<bool>(runtimeProbe["requested"])
                && static_cast<bool>(runtimeProbe["ok"])
                && static_cast<bool>(runtimeProbe["finite"])
                && static_cast<int>(runtimeProbe["channel_count"]) == 8
                && runtimeProbe["render_path"].toString() == "transport"
                && static_cast<int>(runtimeProbe["transport_channel_capacity"]) == 12,
            "runtime probe reports the real transport path and its Ambisonic channel capacity");
    require(static_cast<double>(runtimeProbe["channels"][3]["rms_linear"]) < 0.000001,
            "runtime probe confirms Ambisonics never feeds LFE");
    require(static_cast<double>(runtimeProbe["channels"][4]["rms_linear"])
                > static_cast<double>(runtimeProbe["channels"][5]["rms_linear"]) * 1.25,
            "runtime probe confirms positive yaw moves real speaker output toward the left side");
    const auto fieldProbe = runtimeProbe["ambisonic_field"];
    require(static_cast<bool>(fieldProbe["valid"])
                && static_cast<int>(fieldProbe["order"]) == 1
                && static_cast<double>(fieldProbe["left_energy"])
                       > static_cast<double>(fieldProbe["right_energy"]) * 20.0
                && std::abs(static_cast<double>(fieldProbe["dominant_azimuth_degrees"]) - 90.0) <= 15.0,
            "runtime probe exposes the yawed Ambisonic field used by the sphere visualizer");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", ambisonicTemp.getFullPathName(),
                            "--output-channels", "8", "--source-preset", "foa-acn-sn3d",
                            "--probe-start-seconds", "99", "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed
                && juce::JSON::parse(execution.stdoutText)["results"][0]
                       ["runtime_render_probe"]["error"].toString().containsIgnoreCase("range"),
            "runtime probe rejects a start beyond the playable file range");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", ambisonicTemp.getFullPathName(),
                            "--output-channels", "8", "--source-preset", "foa-acn-sn3d",
                            "--ambisonic-monitor", "binaural", "--ambisonic-yaw", "90",
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::success,
            "explicit FOA binaural inspection succeeds without an audio device");
    json = juce::JSON::parse(execution.stdoutText);
    const auto binauralResult = json["results"][0];
    require(binauralResult["target_layout"]["order"].toString() == "L R"
                && binauralResult["device_output_layout"]["order"].toString()
                       == "L R C LFE Lss Rss Lsr Rsr",
            "CLI separates the binaural decode target from the physical device layout");
    require(static_cast<int>(binauralResult["output_meter"]["channel_count"]) == 8
                && binauralResult["output_meter"]["channels"][7]["role"].toString() == "Rsr",
            "binaural monitoring preserves all physical output meter channels");
    require(binauralResult["source_layout"]["monitor_decode_mode"].toString()
                == "binaural_hrtf"
                && binauralResult["source_layout"]["monitor_decode_runtime_state"].toString()
                       == "active",
            "CLI reports the active HRTF path instead of the speaker reference matrix");
    require(static_cast<bool>(binauralResult["binaural_resource"]["ready"])
                && static_cast<int>(binauralResult["binaural_resource"]["sample_rate"]) == 48000
                && static_cast<int>(binauralResult["binaural_resource"]["tap_count"]) == 256
                && binauralResult["binaural_resource"]["source_md5"].toString()
                       == "05aaa22e2d5654cbdee5cbe83ebfb1ef",
            "CLI exposes the pinned SADIE resource identity and shape");
    require(static_cast<bool>(binauralResult["binaural_self_test"]["passed"])
                && static_cast<double>(binauralResult["binaural_self_test"]["left_energy"])
                       > static_cast<double>(binauralResult["binaural_self_test"]["right_energy"]),
            "CLI verifies a yawed FOA impulse through the real renderer in memory");

    execution = executeCli(
        parseCliArguments({ "routing", "--input", ambisonicTemp.getFullPathName(),
                            "--output-channels", "2", "--ambisonic-monitor", "binaural",
                            "--json" }),
        registry);
    require(execution.exitCode == AutomationExitCode::processingFailed
                && juce::JSON::parse(execution.stdoutText)["results"][0]["error"]
                       .toString().containsIgnoreCase("Ambisonic"),
            "CLI rejects HRTF monitoring for an ordinary speaker-layout source");

    const auto standardGeometry =
        AmbisonicStandardMatrixCatalog::standardProfile("standard-5.1-v1");
    require(standardGeometry != nullptr, "loads a standard geometry as custom profile source");
    auto customGeometry = *standardGeometry;
    customGeometry.profileId = "cli-custom-room-v1";
    customGeometry.revision = 3;
    customGeometry.displayName = "CLI Custom Room";
    customGeometry.parentTemplateId = standardGeometry->profileId;
    customGeometry.builtInReadOnly = false;
    SpeakerGeometryGenerationRequest generationRequest;
    generationRequest.requestId = 1;
    generationRequest.draftRevision = customGeometry.revision;
    generationRequest.ambisonicOrder = 1;
    generationRequest.profile = customGeometry;
    const auto generated = AmbisonicSpeakerGeometryGenerator::generate(generationRequest);
    require(generated.succeeded(), "generates a valid custom geometry artifact for inspection");

    RoutingInspectionOptions customOptions;
    customOptions.outputChannels = 6;
    customOptions.sourcePresetId = "foa-acn-sn3d";
    customOptions.customDecodeArtifact = generated.artifact;
    const auto customInspection = RoutingInspectionService::inspect(ambisonicTemp,
                                                                    customOptions);
    require(customInspection.succeeded(), "automation inspects a custom geometry path in memory");
    const auto customDecodeMatrix = customInspection.toJson()["ambisonic_decode_matrix"];
    require(customDecodeMatrix["state"].toString() == "active"
                && customDecodeMatrix["kind"].toString() == "custom_runtime"
                && customDecodeMatrix["artifact_id"].toString()
                       == generated.artifact->artifactId
                && customDecodeMatrix["geometry_profile_id"].toString()
                       == customGeometry.profileId
                && static_cast<int64_t>(customDecodeMatrix["geometry_profile_revision"])
                       == customGeometry.revision
                && customDecodeMatrix["geometry_profile_hash"].toString()
                       == generated.artifact->profileHash
                && customDecodeMatrix["cache_key"].toString()
                       == generated.artifact->cacheKey
                && !static_cast<bool>(customDecodeMatrix["generated_offline"]),
            "automation distinguishes custom runtime generation from standard offline artifacts");
    ambisonicTemp.deleteFile();
}

void runM3SingleFileAudioSuiteTests()
{
    auto runM3 = [](const juce::StringArray& arguments)
    {
        const auto execution = tryExecuteM3SingleFileCli(arguments);
        require(execution.has_value(), "M3 command is claimed before legacy registry dispatch");
        return *execution;
    };

    require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "Reference Gain starts with zero live instances");
    auto execution = runM3({ "discover", "--jsonl" });
    auto events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && events.size() == 2
                && terminalEventCount(events) == 1
                && events[0]["type"].toString() == "started"
                && events[1]["type"].toString() == "result"
                && static_cast<int>(events[0]["sequence"]) == 1
                && static_cast<int>(events[1]["sequence"]) == 2,
            "cold discovery emits deterministic started/result JSONL");
    const auto discovery = events[1]["payload"];
    require(discovery["schema"].toString() == CliColdManifest::discoverySchema
                && discovery["schema_version"].toString()
                       == CliColdManifest::discoverySchemaVersion
                && discovery["manifest_sha256"].toString()
                       == CliColdManifest::manifestSha256()
                && discovery["operations"].size() == 3,
            "cold discovery exposes a versioned deterministic manifest identity");
    const auto discoveredOperation = discovery["operations"][0];
    require(discoveredOperation["id"].toString() == "singleeditor.reference-gain"
                && discoveredOperation["parameters"].size() == 1
                && discoveredOperation["formats"]["input"].size() == 1
                && discoveredOperation["channel_targeting"]["modes"].size() == 2,
            "discovery exposes operation, parameter, format and channel targeting");
    require(discovery["operations"][1]["id"].toString()
                == CliColdManifest::parametricEq().id
                && discovery["operations"][2]["id"].toString()
                       == CliColdManifest::truePeakLimiter().id
                && static_cast<int>(discovery["batch"]["maximum_items"])
                       == CliColdManifest::maximumBatchItems
                && static_cast<int>(discovery["batch"]["maximum_concurrency"]) == 1,
            "cold discovery exposes EQ, Limiter and truthful bounded Batch capability");
    require(CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "cold discovery constructs no AudioSuite processor");

    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-m3-17", {}, true);
    require(root.createDirectory(), "creates M3-17 fixture directory");
    const auto source = root.getChildFile("source.wav");
    const auto guiSource = root.getChildFile("gui-source.wav");
    const auto cliOutput = root.getChildFile("cli-output.wav");
    const auto guiOutput = root.getChildFile("gui-output.wav");
    const auto planA = root.getChildFile("plan-a.json");
    const auto planB = root.getChildFile("plan-b.json");
    require(writeReferenceGainWavFile(source), "writes Reference Gain fixture");
    require(source.copyFileTo(guiSource), "copies GUI parity source fixture");

    const juce::StringArray analyzeArguments {
        "analyze", "reference-gain",
        "--input", source.getFullPathName(),
        "--plan", planA.getFullPathName(),
        "--gain-db", "-6.0",
        "--start-sample", "100",
        "--end-sample", "900",
        "--channel", "0",
        "--destination-mode", "create_new",
        "--destination", cliOutput.getFullPathName(),
        "--jsonl"
    };
    execution = runM3(analyzeArguments);
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && planA.existsAsFile()
                && terminalEventCount(events) == 1
                && !static_cast<bool>(events.getLast()["payload"]["write_performed"]),
            "Analyze writes an immutable Plan without touching destination audio");
    const auto firstPlanHash = events.getLast()["payload"]["plan_sha256"].toString();
    const auto analyzedIdentity = events.getLast()["payload"]["document_identity"];
    require(static_cast<int>(analyzedIdentity["version"]) == 1
                && analyzedIdentity["lineage"].toString().isNotEmpty()
                && analyzedIdentity["runtime_instance"].toString().isNotEmpty()
                && static_cast<int>(analyzedIdentity["event_sequence"]) == 1,
            "Analyze receipt consumes the document opened identity event");
    auto repeatedArguments = analyzeArguments;
    repeatedArguments.set(repeatedArguments.indexOf(planA.getFullPathName()),
                          planB.getFullPathName());
    execution = runM3(repeatedArguments);
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && events.getLast()["payload"]["plan_sha256"].toString()
                       == firstPlanHash
                && planA.loadFileAsString() == planB.loadFileAsString(),
            "identical Analyze requests produce byte-identical canonical Plans");
    require(events.getLast()["payload"]["document_identity"]["runtime_instance"].toString()
                != analyzedIdentity["runtime_instance"].toString(),
            "separate Analyze executions retain distinct runtime ownership without changing the immutable Plan");
    require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "Analyze releases its lazy Reference Gain processor");

    auto tamperedPlan = juce::JSON::parse(planB.loadFileAsString());
    tamperedPlan["parameters"].getDynamicObject()->setProperty("gain_db", -3.0);
    require(planB.replaceWithText(juce::JSON::toString(tamperedPlan, true)),
            "writes tampered plan fixture");
    execution = runM3({ "apply", "--plan", planB.getFullPathName(), "--jsonl" });
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::validationFailed
                && terminalEventCount(events) == 1
                && events.getLast()["error"]["code"].toString() == "PLAN_INVALID"
                && !cliOutput.exists(),
            "Apply rejects a plan whose canonical content no longer matches its hash");

    execution = runM3({ "apply", "--plan", planA.getFullPathName(), "--jsonl" });
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && cliOutput.existsAsFile()
                && terminalEventCount(events) == 1
                && events.getLast()["type"].toString() == "result"
                && static_cast<bool>(events.getLast()["payload"]["write_performed"]),
            "Apply validates, renders and atomically publishes create_new output");
    const auto appliedIdentity = events.getLast()["payload"]["document_identity"];
    require(static_cast<int>(appliedIdentity["event_sequence"]) >= 2
                && static_cast<juce::int64>(appliedIdentity["generation"]) > 1
                && appliedIdentity["runtime_instance"].toString() != analyzedIdentity["runtime_instance"].toString(),
            "Apply receipt observes committed mutation identity in its own reopened document");
    int previousSequence = 0;
    int progressEvents = 0;
    for (const auto& event : events)
    {
        const auto sequence = static_cast<int>(event["sequence"]);
        require(sequence == previousSequence + 1,
                "Apply event sequence is monotonic without gaps");
        previousSequence = sequence;
        if (event["type"].toString() == "progress")
            ++progressEvents;
    }
    require(progressEvents >= 3,
            "Apply reports typed validation, render and publication progress");
    execution = runM3({ "apply", "--plan", planA.getFullPathName(), "--jsonl" });
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::conflict
                && events.getLast()["error"]["code"].toString()
                       == "DESTINATION_EXISTS",
            "successful create_new Plan cannot silently overwrite on replay");

    SingleTrackDocument guiDocument;
    require(guiDocument.loadFile(guiSource), "loads GUI parity source");
    AudioSelectionRequest guiSelection;
    guiSelection.timeRangeSeconds = juce::Range<double>(0.1, 0.9);
    guiSelection.sourceChannels.push_back(SourceChannelIdentity::fromSourceIndex(0));
    auto guiProcessor = AudioSuiteReferenceProcessor::describe().create();
    const auto guiPlan = guiProcessor->analyze(
        guiDocument.getState(), 1, guiSelection,
        static_cast<float>(std::pow(10.0, -6.0 / 20.0)));
    require(guiPlan.succeeded(), "GUI reference path analyzes the parity selection");
    require(guiProcessor->apply(guiDocument, guiPlan.plan, 1).succeeded(),
            "GUI reference path applies the parity selection");
    require(guiDocument.saveAsToFile(guiOutput),
            "GUI reference path saves the parity output");
    guiProcessor.reset();

    const auto cliAudio = readAudioFile(cliOutput);
    const auto guiAudio = readAudioFile(guiOutput);
    const auto sourceAudio = readAudioFile(source);
    require(cliAudio.getNumChannels() == 2
                && cliAudio.getNumSamples() == 1000
                && guiAudio.getNumChannels() == cliAudio.getNumChannels()
                && guiAudio.getNumSamples() == cliAudio.getNumSamples(),
            "GUI and CLI outputs preserve reference source shape");
    double maximumParityError = 0.0;
    double maximumUntargetedError = 0.0;
    for (int channel = 0; channel < cliAudio.getNumChannels(); ++channel)
    {
        for (int sample = 0; sample < cliAudio.getNumSamples(); ++sample)
        {
            maximumParityError = std::max(
                maximumParityError,
                std::abs(static_cast<double>(cliAudio.getSample(channel, sample)
                                             - guiAudio.getSample(channel, sample))));
            if (channel == 1)
            {
                maximumUntargetedError = std::max(
                    maximumUntargetedError,
                    std::abs(static_cast<double>(cliAudio.getSample(channel, sample)
                                                 - sourceAudio.getSample(channel, sample))));
            }
        }
    }
    require(maximumParityError <= 2.0e-6,
            "CLI and GUI Reference Gain outputs are sample-equivalent");
    require(maximumUntargetedError <= 2.0e-6,
            "explicit source-channel targeting leaves untargeted audio unchanged");

    const auto staleSource = root.getChildFile("stale-source.wav");
    const auto staleOutput = root.getChildFile("stale-output.wav");
    const auto stalePlan = root.getChildFile("stale-plan.json");
    require(source.copyFileTo(staleSource), "copies stale source fixture");
    execution = runM3({
        "analyze", "reference-gain", "--input", staleSource.getFullPathName(),
        "--plan", stalePlan.getFullPathName(), "--gain-db", "-1",
        "--destination-mode", "create_new", "--destination",
        staleOutput.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success,
            "writes stale-source plan fixture");
    require(staleSource.appendText("identity-change"),
            "changes source identity after Analyze");
    execution = runM3({ "apply", "--plan", stalePlan.getFullPathName(), "--jsonl" });
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::conflict
                && events.getLast()["error"]["code"].toString() == "STALE_SOURCE"
                && !staleOutput.exists(),
            "Apply fails closed when source identity changes");

    const auto overwriteSource = root.getChildFile("overwrite-source.wav");
    const auto overwriteTarget = root.getChildFile("overwrite-target.wav");
    const auto overwritePlan = root.getChildFile("overwrite-plan.json");
    require(source.copyFileTo(overwriteSource)
                && source.copyFileTo(overwriteTarget),
            "writes overwrite authorization fixtures");
    execution = runM3({
        "analyze", "reference-gain", "--input", overwriteSource.getFullPathName(),
        "--plan", overwritePlan.getFullPathName(), "--gain-db", "-2",
        "--destination-mode", "overwrite_existing", "--destination",
        overwriteTarget.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success,
            "Analyze binds existing destination identity");
    execution = runM3({ "apply", "--plan", overwritePlan.getFullPathName(),
                         "--jsonl" });
    require(execution.exitCode == AutomationExitCode::conflict,
            "overwrite Plan requires a second Apply authorization");
    execution = runM3({ "apply", "--plan", overwritePlan.getFullPathName(),
                         "--allow-overwrite-existing", "--jsonl" });
    require(execution.exitCode == AutomationExitCode::success,
            "explicit overwrite authorization publishes atomically");

    const auto inPlaceSource = root.getChildFile("in-place-source.wav");
    const auto inPlacePlan = root.getChildFile("in-place-plan.json");
    require(source.copyFileTo(inPlaceSource), "writes in-place authorization fixture");
    execution = runM3({
        "analyze", "reference-gain", "--input", inPlaceSource.getFullPathName(),
        "--plan", inPlacePlan.getFullPathName(), "--gain-db", "-2",
        "--destination-mode", "in_place", "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success,
            "Analyze binds source identity as in-place destination");
    execution = runM3({ "apply", "--plan", inPlacePlan.getFullPathName(),
                         "--jsonl" });
    require(execution.exitCode == AutomationExitCode::conflict,
            "in-place Plan requires a second Apply authorization");
    execution = runM3({ "apply", "--plan", inPlacePlan.getFullPathName(),
                         "--allow-in-place", "--jsonl" });
    require(execution.exitCode == AutomationExitCode::success,
            "explicit in-place authorization publishes atomically");
    require(CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "all M3 CLI invocations release AudioSuite processor instances");

    require(root.deleteRecursively(), "cleans M3-17 fixture directory");
}

ParametricEqParameters cliEqParameters()
{
    auto parameters = ParametricEqParameters::flat();
    parameters.presetId = "cli-parity";
    parameters.bands[0].enabled = true;
    parameters.bands[0].type = ParametricEqFilterType::highPass;
    parameters.bands[0].frequencyHz = 120.0;
    parameters.bands[0].gainDb = 0.0;
    parameters.bands[0].q = 0.7071067811865476;
    parameters.bands[0].slope = ParametricEqSlope::db24;
    parameters.bands[4].enabled = true;
    parameters.bands[4].type = ParametricEqFilterType::bell;
    parameters.bands[4].frequencyHz = 2200.0;
    parameters.bands[4].gainDb = 3.0;
    parameters.bands[4].q = 0.9;
    return parameters;
}

juce::var cliEqParametersJson(const ParametricEqParameters& parameters)
{
    juce::Array<juce::var> bands;
    for (std::size_t index = 0; index < parameters.bands.size(); ++index)
    {
        const auto& source = parameters.bands[index];
        auto* band = new juce::DynamicObject();
        band->setProperty("index", static_cast<int>(index));
        band->setProperty("enabled", source.enabled);
        juce::String type = "bell";
        if (source.type == ParametricEqFilterType::highPass) type = "high-pass";
        else if (source.type == ParametricEqFilterType::lowShelf) type = "low-shelf";
        else if (source.type == ParametricEqFilterType::notch) type = "notch";
        else if (source.type == ParametricEqFilterType::highShelf) type = "high-shelf";
        else if (source.type == ParametricEqFilterType::lowPass) type = "low-pass";
        band->setProperty("type", type);
        band->setProperty("frequency_hz", source.frequencyHz);
        band->setProperty("gain_db", source.gainDb);
        band->setProperty("q", source.q);
        band->setProperty("slope",
                          source.slope == ParametricEqSlope::db24 ? "db24" : "db12");
        bands.add(juce::var(band));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("preset_id", parameters.presetId);
    root->setProperty("bands", juce::var(bands));
    return juce::var(root);
}

TruePeakLimiterParameters cliLimiterParameters()
{
    auto parameters = TruePeakLimiterParameters::masteringDefault();
    parameters.enabled = true;
    parameters.thresholdDb = -12.0;
    parameters.ceilingDbtp = -1.0;
    parameters.lookAheadMs = 5.0;
    parameters.releaseMode = TruePeakLimiterReleaseMode::automatic;
    parameters.releaseMs = 120.0;
    parameters.oversampling = TruePeakLimiterOversampling::sampleRateAwareFixed;
    parameters.channelsLinked = true;
    return parameters;
}

juce::var cliLimiterParametersJson(const TruePeakLimiterParameters& parameters)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("enabled", parameters.enabled);
    root->setProperty("threshold_db", parameters.thresholdDb);
    root->setProperty("ceiling_dbtp", parameters.ceilingDbtp);
    root->setProperty("lookahead_ms", parameters.lookAheadMs);
    root->setProperty("release_mode", "automatic");
    root->setProperty("release_ms", parameters.releaseMs);
    root->setProperty("oversampling", "sample-rate-aware-fixed");
    root->setProperty("channels_linked", parameters.channelsLinked);
    return juce::var(root);
}

double maximumAudioDifference(const juce::AudioBuffer<float>& left,
                              const juce::AudioBuffer<float>& right)
{
    if (left.getNumChannels() != right.getNumChannels()
        || left.getNumSamples() != right.getNumSamples())
        return std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    for (int channel = 0; channel < left.getNumChannels(); ++channel)
        for (int sample = 0; sample < left.getNumSamples(); ++sample)
            maximum = std::max(maximum,
                               std::abs(static_cast<double>(
                                   left.getSample(channel, sample)
                                   - right.getSample(channel, sample))));
    return maximum;
}

void runM3ProcessorParityTests()
{
    auto runM3 = [](const juce::StringArray& arguments)
    {
        const auto execution = tryExecuteM3SingleFileCli(arguments);
        require(execution.has_value(), "M3 processor command is claimed");
        return *execution;
    };
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-m3-18-parity", {}, true);
    require(root.createDirectory(), "creates M3-18 processor parity directory");

    const auto eqSource = root.getChildFile("eq-source.wav");
    const auto eqGuiSource = root.getChildFile("eq-gui-source.wav");
    const auto eqCliOutput = root.getChildFile("eq-cli.wav");
    const auto eqGuiOutput = root.getChildFile("eq-gui.wav");
    const auto eqPlan = root.getChildFile("eq-plan.json");
    const auto eqParametersFile = root.getChildFile("eq-parameters.json");
    const auto eqParameters = cliEqParameters();
    require(writeReferenceGainWavFile(eqSource)
                && eqSource.copyFileTo(eqGuiSource)
                && eqParametersFile.replaceWithText(
                    juce::JSON::toString(cliEqParametersJson(eqParameters), true)),
            "writes EQ CLI/GUI parity fixtures");
    auto execution = runM3({
        "analyze", "eq", "--input", eqSource.getFullPathName(),
        "--plan", eqPlan.getFullPathName(), "--parameters",
        eqParametersFile.getFullPathName(), "--destination-mode", "create_new",
        "--destination", eqCliOutput.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success
                && juce::JSON::parse(eqPlan.loadFileAsString())["operation"]["id"]
                       .toString() == CliColdManifest::parametricEq().id,
            "CLI Analyze freezes the production Parametric EQ operation/schema");
    execution = runM3({ "apply", "--plan", eqPlan.getFullPathName(), "--jsonl" });
    require(execution.exitCode == AutomationExitCode::success
                && eqCliOutput.existsAsFile(),
            "CLI applies Parametric EQ through the single-file transaction");

    SingleTrackDocument eqGuiDocument;
    require(eqGuiDocument.loadFile(eqGuiSource), "loads GUI EQ parity source");
    auto eqProcessor = ParametricEqProcessor::describe().create();
    const auto eqGuiPlan = eqProcessor->analyze(
        eqGuiDocument.getState(), 1, {}, eqParameters);
    require(eqGuiPlan.succeeded()
                && eqProcessor->apply(eqGuiDocument, eqGuiPlan.plan, 1).succeeded()
                && eqGuiDocument.saveAsToFile(eqGuiOutput),
            "GUI production EQ path renders the parity output");
    eqProcessor.reset();
    require(maximumAudioDifference(readAudioFile(eqCliOutput),
                                   readAudioFile(eqGuiOutput)) <= 2.0e-6,
            "GUI and CLI Parametric EQ outputs are sample-equivalent");

    const auto limiterSource = root.getChildFile("limiter-source.wav");
    const auto limiterGuiSource = root.getChildFile("limiter-gui-source.wav");
    const auto limiterCliOutput = root.getChildFile("limiter-cli.wav");
    const auto limiterGuiOutput = root.getChildFile("limiter-gui.wav");
    const auto limiterPlan = root.getChildFile("limiter-plan.json");
    const auto limiterParametersFile = root.getChildFile("limiter-parameters.json");
    const auto limiterParameters = cliLimiterParameters();
    require(writeReferenceGainWavFile(limiterSource)
                && limiterSource.copyFileTo(limiterGuiSource)
                && limiterParametersFile.replaceWithText(
                    juce::JSON::toString(
                        cliLimiterParametersJson(limiterParameters), true)),
            "writes Limiter CLI/GUI parity fixtures");
    execution = runM3({
        "analyze", "limiter", "--input", limiterSource.getFullPathName(),
        "--plan", limiterPlan.getFullPathName(), "--parameters",
        limiterParametersFile.getFullPathName(), "--destination-mode", "create_new",
        "--destination", limiterCliOutput.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success
                && juce::JSON::parse(limiterPlan.loadFileAsString())["operation"]["id"]
                       .toString() == CliColdManifest::truePeakLimiter().id,
            "CLI Analyze freezes the production True-Peak Limiter schema");
    execution = runM3({ "apply", "--plan", limiterPlan.getFullPathName(), "--jsonl" });
    require(execution.exitCode == AutomationExitCode::success
                && limiterCliOutput.existsAsFile(),
            "CLI applies True-Peak Limiter through the single-file transaction");

    SingleTrackDocument limiterGuiDocument;
    require(limiterGuiDocument.loadFile(limiterGuiSource),
            "loads GUI Limiter parity source");
    auto limiterProcessor = TruePeakLimiterProcessor::describe().create();
    const auto limiterGuiPlan = limiterProcessor->analyze(
        limiterGuiDocument.getState(), 1, {}, limiterParameters);
    require(limiterGuiPlan.succeeded()
                && limiterProcessor->apply(
                    limiterGuiDocument, limiterGuiPlan.plan, 1).succeeded()
                && limiterGuiDocument.saveAsToFile(limiterGuiOutput),
            "GUI production Limiter path renders the parity output");
    limiterProcessor.reset();
    require(maximumAudioDifference(readAudioFile(limiterCliOutput),
                                   readAudioFile(limiterGuiOutput)) <= 2.0e-6,
            "GUI and CLI True-Peak Limiter outputs are sample-equivalent");
    require(CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "EQ/Limiter CLI parity releases every lazy processor instance");
    require(root.deleteRecursively(), "cleans M3-18 processor parity directory");
}

void runM3BatchAutomationTests()
{
    auto runSingle = [](const juce::StringArray& arguments)
    {
        const auto execution = tryExecuteM3SingleFileCli(arguments);
        require(execution.has_value(), "Batch fixture single-file command is claimed");
        return *execution;
    };
    auto runBatch = [](const juce::StringArray& arguments)
    {
        const auto execution = tryExecuteM3BatchCli(arguments);
        require(execution.has_value(), "M3 Batch command is claimed before legacy dispatch");
        return *execution;
    };
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("single-editor-m3-18-batch", {}, true);
    require(root.createDirectory(), "creates M3-18 Batch fixture directory");
    const auto templateSource = root.getChildFile("template.wav");
    require(writeReferenceGainWavFile(templateSource), "writes Batch template source");

    auto makeReferencePlan = [&](const juce::String& stem,
                                 juce::File& source,
                                 juce::File& plan,
                                 juce::File& destination)
    {
        source = root.getChildFile(stem + "-source.wav");
        plan = root.getChildFile(stem + "-plan.json");
        destination = root.getChildFile(stem + "-output.wav");
        require(templateSource.copyFileTo(source), "copies Batch source fixture");
        const auto execution = runSingle({
            "analyze", "reference-gain", "--input", source.getFullPathName(),
            "--plan", plan.getFullPathName(), "--gain-db", "-6",
            "--destination-mode", "create_new", "--destination",
            destination.getFullPathName(), "--jsonl"
        });
        require(execution.exitCode == AutomationExitCode::success,
                "creates an immutable per-file Batch Plan");
    };

    juce::File sourceA, planA, outputA;
    juce::File sourceB, planB, outputB;
    makeReferencePlan("success-a", sourceA, planA, outputA);
    makeReferencePlan("success-b", sourceB, planB, outputB);
    const auto batchPlan = root.getChildFile("success-batch.json");
    const auto evidence = root.getChildFile("success-evidence.json");
    auto execution = runBatch({
        "batch-plan", "--output", batchPlan.getFullPathName(),
        "--item", "success-a", planA.getFullPathName(),
        "--item", "success-b", planB.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success
                && batchPlan.existsAsFile()
                && juce::JSON::parse(batchPlan.loadFileAsString())["items"].size() == 2
                && static_cast<int>(juce::JSON::parse(batchPlan.loadFileAsString())
                                        ["concurrency_limit"]) == 1,
            "Batch Plan embeds two immutable per-file Plans with a real concurrency bound");
    require(CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "Batch Plan import does not construct a processor");

    execution = runBatch({
        "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
        "--evidence", evidence.getFullPathName(), "--jsonl"
    });
    auto events = parseJsonLines(execution.stdoutText);
    auto evidenceJson = juce::JSON::parse(evidence.loadFileAsString());
    require(execution.exitCode == AutomationExitCode::success
                && terminalEventCount(events) == 1
                && outputA.existsAsFile() && outputB.existsAsFile()
                && evidenceJson["status"].toString() == "completed"
                && evidenceJson["items"][0]["status"].toString() == "succeeded"
                && evidenceJson["items"][1]["status"].toString() == "succeeded",
            "Batch applies each file atomically and persists completed evidence");
    int previousSequence = 0;
    for (const auto& event : events)
    {
        require(static_cast<int>(event["sequence"]) == ++previousSequence,
                "Batch JSONL sequence is monotonic without gaps");
    }
    execution = runBatch({
        "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
        "--evidence", evidence.getFullPathName(), "--resume", "--jsonl"
    });
    events = parseJsonLines(execution.stdoutText);
    require(execution.exitCode == AutomationExitCode::success
                && static_cast<int>(events.getLast()["payload"]["resume_skipped"]) == 2,
            "Resume revalidates completed output identities and skips exact successes");

    const auto legacyEvidence = root.getChildFile("legacy-identity-evidence.json");
    auto legacy = juce::JSON::parse(evidence.loadFileAsString());
    for (auto& item : *legacy["items"].getArray())
        item["result"].getDynamicObject()->removeProperty("document_identity");
    require(legacyEvidence.replaceWithText(juce::JSON::toString(legacy, true)), "write prior-schema receipt fixture");
    execution = runBatch({ "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
                          "--evidence", legacyEvidence.getFullPathName(), "--resume", "--jsonl" });
    require(execution.exitCode == AutomationExitCode::success, "older evidence without runtime identity remains resumable");

    const auto invalidIdentityEvidence = root.getChildFile("invalid-identity-evidence.json");
    for (const auto field : { "version", "lineage", "runtime_instance", "generation", "event_sequence", "extra" })
    {
        auto invalid = juce::JSON::parse(evidence.loadFileAsString());
        auto* identity = invalid["items"][0]["result"]["document_identity"].getDynamicObject();
        require(identity != nullptr, "runtime identity present in current evidence");
        identity->setProperty(field, -1);
        require(invalidIdentityEvidence.replaceWithText(juce::JSON::toString(invalid, true)), "write invalid runtime identity fixture");
        execution = runBatch({ "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
                              "--evidence", invalidIdentityEvidence.getFullPathName(), "--resume", "--jsonl" });
        require(execution.exitCode == AutomationExitCode::conflict, "resume rejects malformed or unknown runtime identity fields");
    }

    auto tamperedResume = juce::JSON::parse(evidence.loadFileAsString());
    auto* tamperedResult = tamperedResume["items"][0]["result"].getDynamicObject();
    require(tamperedResult != nullptr,
            "completed Batch evidence exposes one typed result for tamper testing");
    tamperedResult->setProperty("plan_sha256", "tampered-plan-hash");
    const auto tamperedEvidence = root.getChildFile("tampered-evidence.json");
    require(tamperedEvidence.replaceWithText(
                juce::JSON::toString(tamperedResume, true)),
            "writes tampered completed-evidence fixture");
    execution = runBatch({
        "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
        "--evidence", tamperedEvidence.getFullPathName(), "--resume", "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::conflict
                && outputA.existsAsFile() && outputB.existsAsFile(),
            "Resume rejects tampered result bindings without touching outputs");

    auto inconsistentCompleted = juce::JSON::parse(evidence.loadFileAsString());
    auto* inconsistentItem = inconsistentCompleted["items"][0].getDynamicObject();
    require(inconsistentItem != nullptr,
            "completed Batch evidence exposes one typed item for aggregate testing");
    inconsistentItem->setProperty("status", "pending");
    inconsistentItem->setProperty("exit_code", juce::var());
    inconsistentItem->setProperty("result", juce::var());
    inconsistentItem->setProperty("error", juce::var());
    const auto inconsistentEvidence = root.getChildFile("inconsistent-evidence.json");
    require(inconsistentEvidence.replaceWithText(
                juce::JSON::toString(inconsistentCompleted, true)),
            "writes inconsistent aggregate-evidence fixture");
    execution = runBatch({
        "batch-apply", "--batch-plan", batchPlan.getFullPathName(),
        "--evidence", inconsistentEvidence.getFullPathName(), "--resume", "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::conflict,
            "Resume rejects completed evidence containing pending items");

    juce::File staleSource, stalePlan, staleOutput;
    juce::File healthySource, healthyPlan, healthyOutput;
    makeReferencePlan("partial-stale", staleSource, stalePlan, staleOutput);
    makeReferencePlan("partial-healthy", healthySource, healthyPlan, healthyOutput);
    const auto partialBatch = root.getChildFile("partial-batch.json");
    const auto partialEvidence = root.getChildFile("partial-evidence.json");
    execution = runBatch({
        "batch-plan", "--output", partialBatch.getFullPathName(),
        "--item", "stale", stalePlan.getFullPathName(),
        "--item", "healthy", healthyPlan.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success
                && staleSource.appendText("stale-after-plan"),
            "creates a Batch with one subsequently stale source");
    execution = runBatch({
        "batch-apply", "--batch-plan", partialBatch.getFullPathName(),
        "--evidence", partialEvidence.getFullPathName(), "--jsonl"
    });
    evidenceJson = juce::JSON::parse(partialEvidence.loadFileAsString());
    require(execution.exitCode == AutomationExitCode::partialFailure
                && !staleOutput.exists()
                && healthyOutput.existsAsFile()
                && evidenceJson["status"].toString() == "partial"
                && evidenceJson["items"][0]["status"].toString() == "failed"
                && evidenceJson["items"][1]["status"].toString() == "succeeded",
            "one stale item cannot corrupt or falsely complete another Batch item");

    juce::File cancelSourceA, cancelPlanA, cancelOutputA;
    juce::File cancelSourceB, cancelPlanB, cancelOutputB;
    makeReferencePlan("cancel-a", cancelSourceA, cancelPlanA, cancelOutputA);
    makeReferencePlan("cancel-b", cancelSourceB, cancelPlanB, cancelOutputB);
    const auto cancelBatch = root.getChildFile("cancel-batch.json");
    const auto cancelEvidence = root.getChildFile("cancel-evidence.json");
    const auto cancelFile = root.getChildFile("cancel.requested");
    execution = runBatch({
        "batch-plan", "--output", cancelBatch.getFullPathName(),
        "--item", "cancel-a", cancelPlanA.getFullPathName(),
        "--item", "cancel-b", cancelPlanB.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success,
            "creates cancellable Batch fixtures");
    const auto aliasEvidence = root.getChildFile("cancel-alias-evidence.json");
    execution = runBatch({
        "batch-apply", "--batch-plan", cancelBatch.getFullPathName(),
        "--evidence", aliasEvidence.getFullPathName(),
        "--cancel-file", aliasEvidence.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::validationFailed
                && !aliasEvidence.exists(),
            "Cancel path cannot alias the evidence path or self-trigger it");
    const auto sourceAliasEvidence = root.getChildFile("source-alias-evidence.json");
    execution = runBatch({
        "batch-apply", "--batch-plan", cancelBatch.getFullPathName(),
        "--evidence", sourceAliasEvidence.getFullPathName(),
        "--cancel-file", cancelSourceA.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::validationFailed
                && !sourceAliasEvidence.exists()
                && cancelSourceA.existsAsFile(),
            "Cancel path cannot alias an item source");
    require(cancelFile.replaceWithText("cancel"),
            "writes bounded Batch cancellation request");
    execution = runBatch({
        "batch-apply", "--batch-plan", cancelBatch.getFullPathName(),
        "--evidence", cancelEvidence.getFullPathName(),
        "--cancel-file", cancelFile.getFullPathName(), "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::cancelled
                && !cancelOutputA.exists() && !cancelOutputB.exists()
                && juce::JSON::parse(cancelEvidence.loadFileAsString())
                       ["status"].toString() == "cancelled",
            "Batch cancellation stops at a truthful file boundary with durable evidence");
    require(cancelFile.deleteFile(), "clears Batch cancellation request");
    execution = runBatch({
        "batch-apply", "--batch-plan", cancelBatch.getFullPathName(),
        "--evidence", cancelEvidence.getFullPathName(), "--resume", "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::success
                && cancelOutputA.existsAsFile() && cancelOutputB.existsAsFile(),
            "cancelled Batch resumes deterministically from pending items");

    auto incompatible = juce::JSON::parse(cancelBatch.loadFileAsString());
    incompatible.getDynamicObject()->setProperty("schema_version", "99.0.0");
    const auto incompatibleBatch = root.getChildFile("incompatible-batch.json");
    require(incompatibleBatch.replaceWithText(
                juce::JSON::toString(incompatible, true)),
            "writes incompatible Batch schema fixture");
    execution = runBatch({
        "batch-apply", "--batch-plan", incompatibleBatch.getFullPathName(),
        "--evidence", root.getChildFile("incompatible-evidence.json").getFullPathName(),
        "--jsonl"
    });
    require(execution.exitCode == AutomationExitCode::validationFailed,
            "Batch schema/version mismatch fails closed before any item execution");
    require(CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "bounded Batch leaves every lazy processor instance released");
    require(root.deleteRecursively(), "cleans M3-18 Batch fixture directory");
}
} // namespace

void runDiagnosticCliTests()
{
    using namespace singleeditor::diagnostics;
    require(!tryExecuteDiagnosticsCli({ "inspect", "unused.wav" }), "unrelated CLI command does not construct diagnostics");
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-CliDiagnosticTest-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create diagnostic CLI fixture directory");
    BundleInput input;
    input.timestampMs = 1800000000000LL; input.origin = "test-fixture";
    input.identity = { "0.3.0", "c6ef07b8", juce::String::repeatedString("a", 64), "Release", "test", "x64", "x64", "10.0.26100", false };
    const juce::String body = "{\"providerId\":\"future\",\"providerSchemaMajor\":99}";
    input.entries.push_back({ "providers/future.json", EntryKind::provider, 99, juce::MemoryBlock(body.toRawUTF8(), body.getNumBytesAsUTF8()) });
    const auto bundle = root.getChildFile("fixture.zip");
    require(static_cast<bool>(DiagnosticBundle::publish(input, bundle)), "create manifest-bound CLI bundle fixture");
    const auto hash = juce::SHA256(bundle).toHexString();
    const auto execution = tryExecuteDiagnosticsCli({ "diagnostics", "validate", "--bundle", bundle.getFullPathName(), "--jsonl" });
    require(execution && execution->exitCode == AutomationExitCode::success && execution->stderrText.isEmpty(), "explicit diagnostic validator succeeds");
    auto lines = juce::StringArray::fromLines(execution->stdoutText); lines.removeEmptyStrings();
    require(lines.size() == 2, "diagnostic JSONL has started and exactly one terminal event");
    const auto terminal = juce::JSON::parse(lines[1]);
    require(terminal["terminal"] == juce::var(true) && terminal["payload"]["write_performed"] == juce::var(false)
            && terminal["payload"]["producer_authenticated"] == juce::var(false)
            && terminal["payload"]["origin"].toString() == "test-fixture", "validation reports consistency/origin without implying authenticity or GUI capture");
    for (const auto& args : std::vector<juce::StringArray> {
        { "diagnostics", "probe", "--active-probe", "--jsonl" },
        { "diagnostics", "validate", "--bundle", "relative.zip", "--jsonl" },
        { "diagnostics", "validate", "--bundle", bundle.getFullPathName(), "--extract", "--jsonl" },
        { "diagnostics", "validate", "--bundle", bundle.getFullPathName(), "--bundle", bundle.getFullPathName(), "--jsonl" } })
    {
        const auto refused = tryExecuteDiagnosticsCli(args);
        require(refused && refused->exitCode == AutomationExitCode::invalidArguments, "unknown, active, extraction, duplicate or ambiguous args fail closed");
    }
    const auto bad = root.getChildFile("private-fixture-name.zip"); require(bad.replaceWithText("bad"), "write hostile CLI archive");
    const auto rejected = tryExecuteDiagnosticsCli({ "diagnostics", "validate", "--bundle", bad.getFullPathName(), "--jsonl" });
    require(rejected && rejected->exitCode == AutomationExitCode::validationFailed && !rejected->stdoutText.contains("private-fixture-name"),
            "hostile bundle rejected without echoing source paths");
    require(juce::SHA256(bundle).toHexString() == hash && CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
            "readback preserves evidence and leaves processor owners absent");
    const auto help = tryExecuteDiagnosticsCli({ "diagnostics", "--help" });
    require(help && help->stdoutText.contains("Read-only bundle validation") && help->stdoutText.contains("not yet available"),
            "diagnostic help distinguishes available readback from pending probes without changing the AudioSuite plan manifest");
    require(root.deleteRecursively(), "clean successful diagnostic CLI fixture directory");
}

void runStandaloneDiagnosticCaptureTests()
{
    using namespace singleeditor::diagnostics;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-CliProbe-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private standalone diagnostic fixture");
    const auto source = root.getChildFile("private-codec-name.wav");
    require(writeRoutingWavFile(source), "create actual multichannel codec fixture");
    const auto sourceHash = juce::SHA256(source).toHexString();
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::String transcript;
    std::vector<juce::var> previews;
    DiagnosticCliInteraction interaction;
    interaction.workerExecutable = executable;
    interaction.confirm = [&](const juce::String& text)
    {
        transcript += text;
        auto lines = juce::StringArray::fromLines(text); lines.removeEmptyStrings();
        const auto preview = juce::JSON::parse(lines[lines.size() - 1]);
        previews.push_back(preview);
        return preview["approval_id"].toString();
    };
    const juce::StringArray probeArgs { "diagnostics", "probe", "--codec", source.getFullPathName(), "--active-probe", "--jsonl" };
    const auto probe = tryExecuteDiagnosticsCli(probeArgs, interaction);
    require(probe && probe->exitCode == AutomationExitCode::success && previews.size() == 1
        && previews.front()["type"].toString() == "effect-preview", "actual codec child runs only after one explicit effect approval");
    const auto terminal = juce::JSON::parse(probe->stdoutText), payload = terminal["payload"];
    const auto section = payload["sections"]["providers/codec.json"], fields = section["fields"];
    require(payload["origin"].toString() == "standalone-cli" && payload["gui_state_available"] == juce::var(false)
        && fields["selectedReader"]["value"].toString() == "JUCE-WAV" && static_cast<int>(fields["mediaChannels"]["value"]) == 8
        && static_cast<int>(fields["lastOperationReadyFrames"]["value"]) == 8,
        "standalone evidence comes from the actual multichannel reader and bounded sample window");
    require(payload["application"]["binary_sha256"].toString() == juce::SHA256(executable).toHexString()
        && !probe->stdoutText.contains(source.getFileName()) && !transcript.contains(root.getFullPathName()),
        "real binary identity and support-safe preview omit local source identifiers");
    require(!section.hasProperty("latestFailure") && section["operationIds"][0].toString() == previews.front()["approval_id"].toString(),
        "actual successful operation correlates with effect approval and invents no failure");
    const auto originalProbeId = previews.front()["approval_id"].toString();
    previews.clear(); transcript.clear();
    const auto bundle = root.getChildFile("reviewed.zip");
    const auto exported = tryExecuteDiagnosticsCli({ "diagnostics", "export", "--codec", source.getFullPathName(),
        "--active-probe", "--output", bundle.getFullPathName(), "--jsonl" }, interaction);
    require(exported && exported->exitCode == AutomationExitCode::success && previews.size() == 2
        && previews[0]["type"].toString() == "effect-preview" && previews[1]["type"].toString() == "snapshot-review"
        && previews[0]["approval_id"] != previews[1]["approval_id"] && previews[0]["approval_id"].toString() != originalProbeId,
        "export obtains distinct fresh operation and exact snapshot approvals");
    const auto validated = DiagnosticBundle::validate(bundle);
    require(static_cast<bool>(validated.status) && validated.manifest["origin"].toString() == "standalone-cli"
        && validated.manifest["snapshotId"] == previews[1]["approval_id"], "published bundle is the approved standalone snapshot");
    juce::ZipFile archive(bundle); const auto index = archive.getIndexOfFileName("providers/codec.json");
    std::unique_ptr<juce::InputStream> input(archive.createStreamForEntry(index));
    require(input && juce::JSON::toString(juce::JSON::parse(input->readEntireStreamAsString()), true)
        == juce::JSON::toString(previews[1]["payload"]["sections"]["providers/codec.json"], true), "published provider bytes describe the exact reviewed payload");
    const auto hash = juce::SHA256(bundle).toHexString();
    const auto conflict = tryExecuteDiagnosticsCli({ "diagnostics", "export", "--output", bundle.getFullPathName(), "--jsonl" }, interaction);
    require(conflict && conflict->exitCode == AutomationExitCode::conflict && juce::SHA256(bundle).toHexString() == hash,
        "reviewed export never overwrites a previous user-owned bundle");
    DiagnosticCliInteraction refused = interaction;
    refused.confirm = [](const juce::String&) { return juce::String("no"); };
    const auto denied = tryExecuteDiagnosticsCli(probeArgs, refused);
    require(denied && denied->exitCode == AutomationExitCode::cancelled, "declined effect does not start a probe");
    refused.confirm = [&](const juce::String& text)
    {
        auto lines = juce::StringArray::fromLines(text); lines.removeEmptyStrings();
        const auto value = juce::JSON::parse(lines[lines.size() - 1]);
        return value["type"].toString() == "effect-preview" ? value["approval_id"].toString() : juce::String("no");
    };
    const auto absent = root.getChildFile("not-approved.zip");
    const auto declinedExport = tryExecuteDiagnosticsCli({ "diagnostics", "export", "--codec", source.getFullPathName(),
        "--active-probe", "--output", absent.getFullPathName(), "--jsonl" }, refused);
    require(declinedExport && declinedExport->exitCode == AutomationExitCode::cancelled && !absent.exists(),
        "second-stage refusal publishes no file even after a successful probe");
    previews.clear();
    const auto local = tryExecuteDiagnosticsCli({ "diagnostics", "probe", "--codec", source.getFullPathName(), "--active-probe",
        "--include-file-name", "--jsonl" }, interaction);
    require(local && local->exitCode == AutomationExitCode::success && local->stdoutText.contains(source.getFileName())
        && !local->stdoutText.contains(root.getFullPathName()), "selected file-name opt-in does not imply path consent");
    const auto missing = root.getChildFile("absent-private.wav");
    const auto failure = tryExecuteDiagnosticsCli({ "diagnostics", "probe", "--codec", missing.getFullPathName(), "--active-probe", "--jsonl" }, interaction);
    require(failure && failure->exitCode == AutomationExitCode::executionFailed && !failure->stdoutText.contains(missing.getFileName())
        && juce::JSON::parse(failure->stdoutText)["payload"]["sections"]["providers/codec.json"].hasProperty("latestFailure"),
        "actual unavailable input produces typed diagnostic evidence without disclosing its private name");
    std::atomic<bool> stopped { true };
    const auto cancelled = CodecDiagnosticProbe::run(executable, source, juce::Uuid(), stopped);
    require(cancelled.cancelled && cancelled.temporaryRemoved && !cancelled.evidence.latestFailure,
        "pre-cancelled probe acquires no work and does not invent failure");
    stopped.store(false); auto policy = CodecDiagnosticProbe::limits(); policy.wallTimeMs = 1;
    const auto timedOut = CodecDiagnosticProbe::run(executable, source, juce::Uuid(), stopped, policy);
    require(!timedOut.succeeded && timedOut.temporaryRemoved && timedOut.evidence.latestFailure
        && std::any_of(timedOut.evidence.fields.begin(), timedOut.evidence.fields.end(), [](const auto& item)
            { return item.field == Field::codecFailureDetail && static_cast<int>(item.value) == static_cast<int>(media::BoundedDecodeProcess::Failure::deadline); })
        && timedOut.evidence.latestFailure->counters[4] == 1,
        "actual short deadline ends the owned child and removes its result before returning");
    previews.clear();
    const auto identityOnly = root.getChildFile("identity-only.zip");
    const auto passive = tryExecuteDiagnosticsCli({ "diagnostics", "export", "--output", identityOnly.getFullPathName(), "--jsonl" }, interaction);
    require(passive && passive->exitCode == AutomationExitCode::success && previews.size() == 1
        && previews.front()["type"].toString() == "snapshot-review" && DiagnosticBundle::validate(identityOnly).verifiedEntries == 0,
        "passive export observes only loaded CLI identity and never starts codec/device work");
    require(juce::SHA256(source).toHexString() == sourceHash && CliAudioSuiteOperationRegistry::liveProcessorInstanceCountForDiagnostics() == 0,
        "real standalone probe/export preserves source bytes and leaves unrelated processors absent");
    input.reset();
    const auto reproduction = root.getChildFile("private-reproduction.json");
    const juce::String originalContents = "{\"do-not-render-content\":\"selected opaque bytes\"}";
    require(reproduction.replaceWithText(originalContents), "write actual selected reproduction contents");
    const auto reproducedZip = root.getChildFile("reproduction.zip");
    const juce::StringArray reproductionArgs { "diagnostics", "export", "--output", reproducedZip.getFullPathName(),
        "--include-reproduction", "--attach", reproduction.getFullPathName(), "--jsonl" };
    DiagnosticCliInteraction reproductionInteraction = interaction;
    reproductionInteraction.confirm = [&](const juce::String& text)
    {
        auto lines = juce::StringArray::fromLines(text); lines.removeEmptyStrings();
        const auto review = juce::JSON::parse(lines[lines.size() - 1]); previews.push_back(review);
        require(!text.contains("do-not-render-content") && !text.contains(reproduction.getFileName()),
            "opaque CLI review does not render attachment contents or source names");
        if (review["type"].toString() == "snapshot-review")
        {
            const auto replacement = root.getChildFile("replacement.json");
            require(replacement.replaceWithText("changed after review") && replacement.replaceFileIn(reproduction),
                "real CLI approval callback can atomically replace the source without changing the snapshot");
        }
        return review["approval_id"].toString();
    };
    previews.clear();
    const auto attached = tryExecuteDiagnosticsCli(reproductionArgs, reproductionInteraction);
    require(attached && attached->exitCode == AutomationExitCode::success && previews.size() == 2
        && previews[0]["payload"]["action"].toString() == "include-reproduction-contents"
        && previews[1]["payload"]["privacy_tier"].toString() == "reproduction-material"
        && previews[1]["payload"]["attachments"].size() == 1 && previews[0]["approval_id"] != previews[1]["approval_id"],
        "actual opaque selection requires distinct contents consent and frozen snapshot approval");
    {
        juce::ZipFile zip(reproducedZip);
        std::unique_ptr<juce::InputStream> bytes(zip.createStreamForEntry(zip.getIndexOfFileName("reproduction/file-1.bin")));
        require(bytes && bytes->readEntireStreamAsString() == originalContents
            && DiagnosticBundle::validate(reproducedZip).manifest["snapshotId"] == previews[1]["approval_id"],
            "CLI publishes the exact reviewed reproduction bytes after source replacement");
    }
    for (const auto& arguments : std::vector<juce::StringArray> {
        { "diagnostics", "export", "--output", reproducedZip.getFullPathName(), "--attach", reproduction.getFullPathName(), "--jsonl" },
        { "diagnostics", "export", "--output", reproducedZip.getFullPathName(), "--include-reproduction", "--jsonl" },
        { "diagnostics", "probe", "--codec", source.getFullPathName(), "--active-probe", "--include-reproduction", "--attach", reproduction.getFullPathName(), "--jsonl" } })
        require(tryExecuteDiagnosticsCli(arguments, interaction)->exitCode == AutomationExitCode::invalidArguments,
            "reproduction requires export, explicit files and separate consent flag together");
    const auto unapproved = root.getChildFile("unapproved-contents.zip");
    auto unapprovedArgs = reproductionArgs; unapprovedArgs.set(3, unapproved.getFullPathName());
    refused.confirm = [](const juce::String&) { return juce::String("no"); };
    require(tryExecuteDiagnosticsCli(unapprovedArgs, refused)->exitCode == AutomationExitCode::cancelled && !unapproved.exists(),
        "declining contents consent publishes no attachment bundle");
    require(root.deleteRecursively(), "remove only successful owned CLI fixtures");
    std::cout << "StandaloneDiagnosticCaptureTests passed (actual child reader, dual consent, reviewed ZIP, identity/privacy, cancellation, deadline and source preservation)\n";
}

int main(int argc, char* argv[])
{
    juce::StringArray workerArguments; for (int i = 1; i < argc; ++i) workerArguments.add(juce::String::fromUTF8(argv[i]));
    if (const auto worker = singleeditor::diagnostics::CodecDiagnosticProbe::tryWorker(workerArguments)) return *worker;
    if (const auto worker = tryDeviceProbeTestWorker(workerArguments)) return *worker;
    runDiagnosticCliTests();
    runStandaloneDiagnosticCaptureTests();
    runDeviceDiagnosticProbeTests();
    runRegistryTests();
    runInspectionTests();
    runBuiltInOperationTests();
    runCliArgumentTests();
    runCliDispatchTests();
    runAnalyzePlanTests();
    runPlanPreflightTests();
    runPlanApplyTests();
    runApplyOutputPolicyTests();
    runEditPipelineTests();
    runRoutingCliTests();
    runM3SingleFileAudioSuiteTests();
    runM3ProcessorParityTests();
    runM3BatchAutomationTests();
    std::cout << "CliAutomationTests passed\n";
    return 0;
}
