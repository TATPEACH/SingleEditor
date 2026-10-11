#include "DeviceDiagnosticProbeTests.h"
#include "Diagnostics/DeviceDiagnosticProbe.h"
#include "CLI/CliDiagnostics.h"
#include <iostream>
#include <cmath>
#include <thread>
#include <stdexcept>

namespace
{
using namespace singleeditor::diagnostics;
void check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
struct Observation
{
    int ping = 0, opens = 0, closes = 0;
    std::atomic<bool> incorrect { false };
    std::atomic<int> audible { 0 };
    juce::File witness;
};
class Device final : public juce::AudioIODevice
{
public:
    Device(juce::String modeIn, Observation& stateIn)
        : AudioIODevice(modeIn, "Fixture Device Probe"), mode(std::move(modeIn)), state(stateIn) {}
    ~Device() override { close(); }
    juce::StringArray getOutputChannelNames() override { return {"1", "2", "3", "4"}; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {48000.0, 96000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256}; }
    int getDefaultBufferSize() override { return 128; }
    juce::String open(const juce::BigInteger& input, const juce::BigInteger& output, double, int) override
    {
        ++state.opens; attempted = true;
        if (!input.isZero()) state.incorrect.store(true);
        if (mode == "Slow Open") { state.witness.replaceWithText("open-entered"); juce::Thread::sleep(30000); }
        if (mode == "Fail Open") return "Private driver message C:/private/device.conf";
        channels = output; if (mode == "Coerced Channels") channels.clearBit(0);
        opened = true; return {};
    }
    void close() override
    {
        stop(); if (!attempted) return;
        if (mode == "Slow Close") { state.witness.replaceWithText("close-entered"); juce::Thread::sleep(30000); }
        ++state.closes; attempted = false; opened = false;
    }
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback* callback) override
    {
        if (!opened || thread.joinable()) { state.incorrect.store(true); return; }
        active.store(true); callback->audioDeviceAboutToStart(this);
        thread = std::thread([this, callback]
        {
            if (mode != "No Callback")
            {
                const auto packed = mode != "Physical Pointer Array";
                juce::AudioBuffer<float> samples(packed ? channels.countNumberOfSetBits() : channels.getHighestBit() + 1, 64);
                std::vector<float*> pointers;
                for (int ch = 0; ch < samples.getNumChannels(); ++ch)
                    pointers.push_back(packed || channels[ch] ? samples.getWritePointer(ch) : nullptr);
                int frame = 0;
                while (active.load())
                {
                    for (int ch = 0; ch < samples.getNumChannels(); ++ch) juce::FloatVectorOperations::fill(samples.getWritePointer(ch), 1.0f, 64);
                    callback->audioDeviceIOCallbackWithContext(nullptr, 0, pointers.data(), samples.getNumChannels(), 64, {});
                    for (int ch = 0; ch < samples.getNumChannels(); ++ch) for (int i = 0; i < 64; ++i)
                    {
                        if (!pointers[static_cast<size_t>(ch)]) continue;
                        const auto selected = state.ping == 0 ? -1 : packed
                            ? channels.getBitRange(0, state.ping - 1).countNumberOfSetBits() : state.ping - 1;
                        const auto value = samples.getSample(ch, i);
                        if (!std::isfinite(value) || std::abs(value) > 0.010001f || (ch != selected && value != 0.0f)
                            || ((frame + i >= 12000 || frame + i == 0 || frame + i == 11999) && value != 0.0f)) state.incorrect.store(true);
                        if (value != 0.0f) ++state.audible;
                    }
                    frame += 64; juce::Thread::sleep(1);
                }
            }
            else while (active.load()) juce::Thread::sleep(2);
            callback->audioDeviceStopped();
        });
    }
    void stop() override { active.store(false); if (thread.joinable()) thread.join(); }
    bool isPlaying() override { return active.load(); }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 128; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveOutputChannels() const override { return channels; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 96; }
    int getInputLatencyInSamples() override { return 0; }
private:
    juce::String mode;
    Observation& state;
    juce::BigInteger channels;
    bool opened = false, attempted = false;
    std::atomic<bool> active { false };
    std::thread thread;
};
class Backend final : public juce::AudioIODeviceType
{
public:
    explicit Backend(Observation& value) : AudioIODeviceType("Fixture Device Probe"), state(value) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool input) const override
    { return input ? juce::StringArray{} : juce::StringArray{"Private Probe Speakers", "Fail Open", "No Callback", "Slow Open", "Slow Close", "Coerced Channels", "Physical Pointer Array"}; }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output, const juce::String&) override { return new Device(output, state); }
private:
    Observation& state;
};
juce::File witnessFor(const juce::Uuid& operation)
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-device-fixture-" + operation.toString() + ".txt");
}
juce::var value(const DeviceProbeResult& result, Field field)
{ for (const auto& item : result.evidence.fields) if (item.field == field) return item.value; return {}; }
}
std::optional<int> tryDeviceProbeTestWorker(const juce::StringArray& args)
{
    if (args.isEmpty() || args[0] != "--diagnostic-device-worker") return std::nullopt;
    Observation state;
    if (args.size() >= 10 && args.size() <= 12) { state.ping = args[8].getIntValue(); state.witness = witnessFor(juce::Uuid(args[2])); }
    const auto result = DeviceDiagnosticProbe::tryWorker(args, [&] { return std::make_unique<Backend>(state); });
    if (result == 0 && (state.incorrect.load() || state.opens != state.closes)) return 9;
    if (result == 0 && args.size() >= 10 && (args[4].isEmpty() || args[4] == "Physical Pointer Array")
        && state.ping != 0 && state.audible.load() == 0) return 10;
    return result;
}
void runDeviceDiagnosticProbeTests()
{
    using namespace singleeditor;
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    std::atomic<bool> cancelled { false };
    DeviceProbeRequest request { "Fixture Device Probe", {}, 96000, 256, 2, 0 };
    auto result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
    check(result.succeeded && result.temporaryRemoved && !result.evidence.latestFailure, "actual silent device callback completes and closes");
    check(value(result, Field::requestedRate) == juce::var(96000) && value(result, Field::activeRate) == juce::var(48000.0)
        && value(result, Field::requestedBuffer) == juce::var(256) && value(result, Field::activeBuffer) == juce::var(128), "requested and actual device configuration remain distinct");
    request.pingChannel = 2;
    result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
    check(result.succeeded && static_cast<int>(value(result, Field::deviceProbeFrames)) >= 12000,
        "actual second physical output receives only bounded ramped -40dBFS samples, others silent; callback stopped before destruction");
    request.physicalOutputs.setBit(1); request.physicalOutputs.setBit(3); request.pingChannel = 4;
    for (const auto& name : { "", "Physical Pointer Array" })
    {
        request.outputDevice = name;
        result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
        const auto mapping = value(result, Field::channelMapping);
        check(result.succeeded && mapping.size() == 2 && static_cast<int>(mapping[0]) == 1 && static_cast<int>(mapping[1]) == 3,
            "physical output four maps correctly for compact WASAPI and physical/null callback arrays with only outputs two/four enabled");
        const BundleIdentity identity { "0.3.0", "device-fixture", juce::String::repeatedString("a", 64),
            "Release", "test", "x64", "x64", "test", false };
        const auto prepared = DiagnosticSnapshot::capture(identity, "test-fixture", { { ProviderKind::audio, &result } });
        check(static_cast<bool>(prepared.status()) && prepared.content().entries.size() == 1,
            "noncontiguous probe passes actual public snapshot schema");
        const auto fields = juce::JSON::parse(prepared.content().entries.front().bytes.toString())["fields"];
        check(fields["requestedChannelPolicy"]["value"].toString() == "exact-physical-mask-a",
            "exact physical request survives actual serialized report instead of becoming invalid-value");
    }
    request.pingChannel = 3;
    check(!DeviceDiagnosticProbe::valid(request), "ping refuses a physical channel outside the exact mask");
    request.physicalOutputs.clear(); request.pingChannel = 2;
    request.pingChannel = 0; request.outputDevice = "Private Probe Speakers"; request.expectedOutputEndpoint = "different-private-endpoint";
    result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
    check(!result.succeeded && value(result, Field::negotiationStage).toString() == "requested-output-unavailable",
        "same display name cannot authorize a different endpoint or emit a ping after devices are re-enumerated");
    request.expectedOutputEndpoint.clear(); request.pingChannel = 2;
    request.outputDevice = "Coerced Channels";
    result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
    check(!result.succeeded && value(result, Field::negotiationStage).toString() == "exact-physical-channel-unavailable", "coerced channel mapping cannot silently redirect ping");
    request.pingChannel = 0;
    for (const auto& mode : {"Fail Open", "No Callback", "Missing Device"})
    {
        request.outputDevice = mode;
        result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
        check(!result.succeeded && result.temporaryRemoved && result.evidence.latestFailure, "actual failed open/callback/selection produces typed evidence and releases worker");
    }
    request.backend = "Missing Backend"; request.outputDevice.clear();
    result = DeviceDiagnosticProbe::run(executable, request, juce::Uuid(), cancelled);
    check(!result.succeeded && value(result, Field::negotiationStage).toString() == "requested-backend-unavailable", "no other backend fallback");
    request.backend = "Fixture Device Probe";
    for (const auto& mode : {"Slow Open", "Slow Close"})
    {
        request.outputDevice = mode; const juce::Uuid operation; auto policy = DeviceDiagnosticProbe::limits(); policy.wallTimeMs = 1000;
        result = DeviceDiagnosticProbe::run(executable, request, operation, cancelled, policy);
        const auto witness = witnessFor(operation);
        check(witness.existsAsFile(), "actual controlled driver stall was entered before deadline");
        check(!result.succeeded && result.temporaryRemoved && value(result, Field::deviceProbeWorkerFailure)
            == juce::var(static_cast<int>(media::BoundedDecodeProcess::Failure::deadline))
            && value(result, Field::deviceProbeCleanup).toString() == "confirmed", "deadline reclaims actual stalled open/close child");
        check(witness.deleteFile(), "remove only this test operation witness");
    }
    request.outputDevice = "Slow Open"; const juce::Uuid operation; const auto witness = witnessFor(operation);
    std::thread abort([&]
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + 4000;
        while (!witness.existsAsFile() && juce::Time::getMillisecondCounterHiRes() < end) juce::Thread::sleep(5);
        cancelled.store(true);
    });
    result = DeviceDiagnosticProbe::run(executable, request, operation, cancelled); abort.join();
    check(witness.existsAsFile() && result.cancelled && result.temporaryRemoved && !result.evidence.latestFailure
        && value(result, Field::deviceProbeCleanup).toString() == "confirmed", "cancel during actual driver open releases child without failure capsule");
    check(witness.deleteFile(), "remove cancelled operation witness"); cancelled.store(false);

    std::vector<juce::var> previews;
    DiagnosticCliInteraction interaction;
    interaction.confirm = [&](const juce::String& text)
    {
        auto lines = juce::StringArray::fromLines(text); lines.removeEmptyStrings();
        previews.push_back(juce::JSON::parse(lines[lines.size() - 1])); return previews.back()["approval_id"].toString();
    };
    const juce::StringArray args {"diagnostics", "probe", "--device", "Fixture Device Probe", "--active-probe", "--ping-channel", "2", "--jsonl"};
    const auto execution = tryExecuteDiagnosticsCli(args, interaction);
    const auto payload = juce::JSON::parse(execution->stdoutText)["payload"], section = payload["sections"]["providers/audio.json"];
    check(execution->exitCode == AutomationExitCode::success && previews.size() == 1
        && previews[0]["payload"]["emits_sound"] == juce::var(true) && previews[0]["payload"]["peak_dbfs"] == juce::var(-40)
        && previews[0]["payload"]["physical_channel_one_based"] == juce::var(2), "actual CLI effect discloses sound/channel/level before exact operation approval");
    check(!execution->stdoutText.contains("Private Probe Speakers") && section["operationIds"][0] == previews[0]["approval_id"]
        && payload["gui_state_available"] == juce::var(false), "common support-safe audio provider identity is independent of GUI");
    auto local = args; local.add("--include-device-name");
    check(tryExecuteDiagnosticsCli(local, interaction)->stdoutText.contains("Private Probe Speakers"), "device name requires explicit identifier consent");
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("SingleEditor-DeviceCliTest-" + juce::Uuid().toString());
    check(root.createDirectory().wasOk(), "create owned device CLI fixture");
    const auto bundle = root.getChildFile("device.zip"); previews.clear();
    const auto exported = tryExecuteDiagnosticsCli({"diagnostics", "export", "--device", "Fixture Device Probe", "--active-probe", "--output", bundle.getFullPathName(), "--jsonl"}, interaction);
    check(exported->exitCode == AutomationExitCode::success && previews.size() == 2
        && previews[0]["approval_id"] != previews[1]["approval_id"] && DiagnosticBundle::validate(bundle).manifest["snapshotId"] == previews[1]["approval_id"], "device export uses separate immutable snapshot approval and common ZIP");
    for (const auto& bad : std::vector<juce::StringArray>{
        {"diagnostics", "probe", "--device", "Fixture Device Probe", "--jsonl"},
        {"diagnostics", "probe", "--device", "Fixture Device Probe", "--active-probe", "--ping-channel", "3", "--jsonl"},
        {"diagnostics", "probe", "--device", "Fixture Device Probe", "--active-probe", "--sample-rate", "48000x", "--jsonl"},
        {"diagnostics", "export", "--output", bundle.getFullPathName(), "--ping-channel", "1", "--jsonl"}})
        check(tryExecuteDiagnosticsCli(bad, interaction)->exitCode == AutomationExitCode::invalidArguments, "device effects require complete explicit valid request");
    interaction.confirm = [](const juce::String&) { return juce::String("no"); };
    check(tryExecuteDiagnosticsCli(args, interaction)->exitCode == AutomationExitCode::cancelled, "declined device effect launches no probe");
    check(root.deleteRecursively(), "remove only successful owned device CLI fixtures");
    std::cout << "DeviceDiagnosticProbeTests passed (actual controlled device callbacks/ping, close, coercion, errors, deadline/abort, CLI consent, privacy and ZIP)\n";
}
