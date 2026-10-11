#include "DeviceDiagnosticProbe.h"
#include "DiagnosticTestActivity.h"
#include "Core/NativeFilePath.h"
#include "Audio/NativeAudioDeviceBinding.h"
#include "Audio/SystemAudioDeviceManager.h"
#include <cmath>

namespace singleeditor::diagnostics
{
namespace
{
enum class Outcome { success, backend, device, open, dimensions, channel, callback, close,
                     invalidRequest, storage, worker, cancelled, cleanup };
struct ProbeFiles final
{
    juce::File directory, output;
    bool created = false;
    bool remove()
    {
        if (!created) return true;
        if ((!output.exists() || output.deleteFile()) && directory.deleteFile()) { created = false; return true; }
        return false;
    }
    ~ProbeFiles() { remove(); }
};
const char* reason(Outcome value)
{
    switch (value)
    {
        case Outcome::success: return "bounded-device-probe-completed";
        case Outcome::backend: return "requested-backend-unavailable";
        case Outcome::device: return "requested-output-unavailable";
        case Outcome::open: return "device-open-failed";
        case Outcome::dimensions: return "negotiated-dimensions-refused";
        case Outcome::channel: return "exact-physical-channel-unavailable";
        case Outcome::callback: return "device-callback-incomplete";
        case Outcome::close: return "device-close-incomplete";
        case Outcome::invalidRequest: return "invalid-device-probe-request";
        case Outcome::storage: return "probe-temporary-unavailable";
        case Outcome::worker: return "probe-worker-failed";
        case Outcome::cancelled: return "probe-cancelled";
        case Outcome::cleanup: return "probe-cleanup-failed";
    }
    return "probe-worker-failed";
}
bool integer(const juce::String& text, int& output)
{
    if (text.isEmpty() || text.length() > 6 || !text.containsOnly("0123456789")) return false;
    output = text.getIntValue(); return true;
}
class Callback final : public juce::AudioIODeviceCallback
{
public:
    Callback(double sampleRate, int channel, juce::BigInteger active) : rate(sampleRate), selected(channel - 1),
        activeChannels(std::move(active)),
        target(static_cast<int64_t>(sampleRate * DeviceDiagnosticProbe::durationMs / 1000.0)) {}
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs,
        int count, int samples, const juce::AudioIODeviceCallbackContext&) override
    {
        const auto start = frames.load(std::memory_order_relaxed);
        // WASAPI packs enabled physical channels; other JUCE backends may
        // expose physical-index arrays with null entries. Accept only these
        // explicit shapes, never redirect a ping by assuming physical == array.
        auto selectedBuffer = selected;
        if (selected >= 0)
        {
            if (count == activeChannels.countNumberOfSetBits())
                selectedBuffer = activeChannels.getBitRange(0, selected).countNumberOfSetBits();
            else if (count <= activeChannels.getHighestBit()) selectedBuffer = -1;
            if (selectedBuffer < 0 || selectedBuffer >= count || !outputs[selectedBuffer])
                failed.store(true, std::memory_order_relaxed);
        }
        for (int channel = 0; channel < count; ++channel)
        {
            if (!outputs[channel]) continue;
            juce::FloatVectorOperations::clear(outputs[channel], samples);
            if (channel != selectedBuffer) continue;
            for (int i = 0; i < samples && start + i < target; ++i)
            {
                const auto frame = start + i;
                const auto ramp = std::min({1.0, static_cast<double>(frame) / (rate * 0.01),
                    static_cast<double>(target - 1 - frame) / (rate * 0.01)});
                outputs[channel][i] = static_cast<float>(0.01 * ramp
                    * std::sin(juce::MathConstants<double>::twoPi * DeviceDiagnosticProbe::toneHz * frame / rate));
            }
        }
        frames.store(start + samples, std::memory_order_release);
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError(const juce::String&) override { failed.store(true); }
    const double rate;
    const int selected;
    const juce::BigInteger activeChannels;
    const int64_t target;
    std::atomic<int64_t> frames { 0 };
    std::atomic<bool> failed { false };
};
juce::var observe(const DeviceProbeRequest& request, const juce::Uuid& operation,
                  const DeviceDiagnosticProbe::BackendFactory& factory)
{
    auto* object = new juce::DynamicObject(); juce::var report(object);
    object->setProperty("operation", operation.toString());
    auto outcome = Outcome::backend;
    std::unique_ptr<juce::AudioDeviceManager> manager;
    std::unique_ptr<juce::AudioIODeviceType> controlled;
    juce::AudioIODeviceType* type = nullptr;
    if (factory) { controlled = factory(); type = controlled.get(); }
    else
    {
        manager = std::make_unique<SystemAudioDeviceManager>();
        for (auto* candidate : manager->getAvailableDeviceTypes())
            if (candidate->getTypeName() == request.backend) { type = candidate; break; }
    }
    if (type && type->getTypeName() == request.backend)
    {
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        const auto index = request.outputDevice.isEmpty() ? type->getDefaultDeviceIndex(false) : names.indexOf(request.outputDevice);
        outcome = Outcome::device;
        if (index >= 0 && index < names.size())
        {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(names[index], {}));
            if (device && (request.expectedOutputEndpoint.isEmpty()
                || copyWasapiOutputEndpoint(device.get()) == request.expectedOutputEndpoint))
            {
                object->setProperty("outputName", names[index]);
                const auto available = device->getOutputChannelNames().size();
                outcome = Outcome::channel;
                const auto channels = request.requestedOutputs();
                if (channels.getHighestBit() < available)
                {
                    const auto rates = device->getAvailableSampleRates();
                    const auto rate = request.sampleRate != 0 ? static_cast<double>(request.sampleRate)
                        : rates.contains(48000.0) ? 48000.0 : rates.isEmpty() ? 0.0 : rates[0];
                    const auto buffer = request.bufferSamples != 0 ? request.bufferSamples : device->getDefaultBufferSize();
                    outcome = Outcome::open;
                    if (device->open({}, channels, rate, buffer).isEmpty() && device->isOpen())
                    {
                        const auto actualRate = device->getCurrentSampleRate();
                        const auto actualBuffer = device->getCurrentBufferSizeSamples();
                        const auto active = device->getActiveOutputChannels();
                        outcome = Outcome::dimensions;
                        if (std::isfinite(actualRate) && actualRate >= 8000.0 && actualRate <= 768000.0
                            && actualBuffer > 0 && actualBuffer <= 65536 && active.countNumberOfSetBits() > 0
                            && active.getHighestBit() < 32)
                        {
                            object->setProperty("rate", actualRate); object->setProperty("buffer", actualBuffer);
                            object->setProperty("latency", device->getOutputLatencyInSamples());
                            juce::Array<juce::var> mapping;
                            for (int i = 0; i <= active.getHighestBit(); ++i) if (active[i]) mapping.add(i);
                            object->setProperty("mapping", mapping);
                            outcome = Outcome::channel;
                            // Never reinterpret a physical ping index after driver channel coercion.
                            if (request.pingChannel == 0 || active == channels)
                            {
                                Callback callback(actualRate, request.pingChannel, active);
                                {
                                    struct StopBeforeCallbackDestruction
                                    { juce::AudioIODevice& device; ~StopBeforeCallbackDestruction() { device.stop(); } } stop { *device };
                                    device->start(&callback);
                                    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
                                    while (callback.frames.load(std::memory_order_acquire) < callback.target
                                        && !callback.failed.load() && juce::Time::getMillisecondCounterHiRes() < deadline)
                                        juce::Thread::sleep(5);
                                }
                                object->setProperty("frames", callback.frames.load());
                                outcome = !callback.failed.load() && callback.frames.load() >= callback.target
                                    ? Outcome::success : Outcome::callback;
                            }
                        }
                    }
                }
                device->stop();
                device->close();
                if (device->isOpen()) outcome = Outcome::close;
            }
        }
    }
    object->setProperty("outcome", static_cast<int>(outcome));
    return report;
}
}
bool DeviceDiagnosticProbe::valid(const DeviceProbeRequest& request) noexcept
{
    return request.backend.isNotEmpty() && request.backend.length() <= 128
        && !request.backend.containsAnyOf("\r\n") && request.outputDevice.length() <= 512
        && !request.outputDevice.containsAnyOf("\r\n")
        && request.expectedOutputEndpoint.length() <= 2048 && !request.expectedOutputEndpoint.containsAnyOf("\r\n")
        && (request.sampleRate == 0 || (request.sampleRate >= 8000 && request.sampleRate <= 768000))
        && request.bufferSamples >= 0 && request.bufferSamples <= 65536
        && request.outputChannels >= 1 && request.outputChannels <= 32
        && !request.physicalOutputs.isNegative() && request.physicalOutputs.getHighestBit() < 32
        && (request.physicalOutputs.isZero() || request.physicalOutputs.countNumberOfSetBits() == request.outputChannels)
        && request.pingChannel >= 0 && request.pingChannel <= 32
        && (request.pingChannel == 0 || request.requestedOutputs()[request.pingChannel - 1]);
}
media::BoundedDecodeProcess::Limits DeviceDiagnosticProbe::limits()
{
    media::BoundedDecodeProcess::Limits result;
    result.wallTimeMs = 5000; result.memoryBytes = 512ULL * 1024 * 1024;
    result.outputBytes = 16384; result.processes = 1; return result;
}
std::optional<int> DeviceDiagnosticProbe::tryWorker(const juce::StringArray& args, const BackendFactory& factory)
{
    if (args.isEmpty() || args[0] != "--diagnostic-device-worker") return std::nullopt;
    if ((args.size() != 10 && args.size() != 11 && args.size() != 12) || args[9] != "--active-probe" || !juce::File::isAbsolutePath(args[1])
        || args[2].length() != 32 || !args[2].containsOnly("0123456789abcdef") || juce::Uuid(args[2]).isNull()) return 2;
    DeviceProbeRequest request; request.backend = args[3]; request.outputDevice = args[4];
    if (args.size() >= 11)
    {
        if (args[10].isEmpty() || args[10].length() > 8 || !args[10].containsOnly("0123456789abcdef")) return 2;
        request.physicalOutputs.parseString(args[10], 16);
        if (request.physicalOutputs.isZero()) return 2;
    }
    if (args.size() == 12)
    {
        request.expectedOutputEndpoint = args[11];
        if (request.expectedOutputEndpoint.isEmpty()) return 2;
    }
    if (!integer(args[5], request.sampleRate) || !integer(args[6], request.bufferSamples)
        || !integer(args[7], request.outputChannels) || !integer(args[8], request.pingChannel) || !valid(request)) return 2;
    const juce::File output(args[1]); if (output.exists()) return 2;
    juce::ScopedJuceInitialiser_GUI runtime;
    try
    {
        const auto text = juce::JSON::toString(observe(request, juce::Uuid(args[2]), factory), true);
        if (text.getNumBytesAsUTF8() > 16384) return 3;
        auto stream = files::nativeFile(output).createOutputStream();
        if (!stream || !stream->writeText(text, false, false, "\n")) return 5;
        stream->flush(); return stream->getStatus().wasOk() ? 0 : 5;
    }
    catch (...) { return 4; }
}
DeviceProbeResult DeviceDiagnosticProbe::run(const juce::File& executable, const DeviceProbeRequest& request,
    const juce::Uuid& operation, const std::atomic<bool>& cancelled, media::BoundedDecodeProcess::Limits policy)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(deviceProbe);
    DeviceProbeResult result;
    result.evidence.operationIds.push_back(operation);
    result.evidence.fields = { { Field::requestedBackend, request.backend }, { Field::requestedRate, request.sampleRate },
        { Field::requestedBuffer, request.bufferSamples }, { Field::requestedChannels, request.outputChannels },
        { Field::requestedRateMode, request.sampleRate == 0 ? "backend-default" : "explicit" },
        { Field::requestedBufferMode, request.bufferSamples == 0 ? "backend-default" : "explicit" },
        { Field::requestedChannelPolicy, request.physicalOutputs.isZero() ? juce::String("first-physical-outputs")
            : "exact-physical-mask-" + request.physicalOutputs.toString(16) },
        { Field::negotiationRequestKind, "standalone-active-probe" },
        { Field::deviceProbeAction, request.pingChannel == 0 ? "silent-negotiation" : "physical-channel-ping" },
        { Field::deviceProbeChannel, request.pingChannel }, { Field::deviceProbePeakDbfs, peakDbfs },
        { Field::deviceProbeDurationMs, durationMs } };
    auto outcome = cancelled.load() ? Outcome::cancelled : Outcome::invalidRequest;
    media::BoundedDecodeProcess::Result worker;
    if (!cancelled.load() && valid(request) && !operation.isNull())
    {
        ProbeFiles temporary;
        temporary.directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("SingleEditor-device-probe-" + juce::Uuid().toString());
        temporary.output = temporary.directory.getChildFile("result.json");
        const auto& directory = temporary.directory; const auto& output = temporary.output;
        outcome = Outcome::storage;
        if (directory.createDirectory().wasOk())
        {
            temporary.created = true;
            const auto maximum = limits();
            policy.wallTimeMs = std::min(policy.wallTimeMs, maximum.wallTimeMs);
            policy.memoryBytes = std::min(policy.memoryBytes, maximum.memoryBytes);
            policy.outputBytes = std::min(policy.outputBytes, maximum.outputBytes); policy.processes = 1;
            juce::StringArray arguments { "--diagnostic-device-worker", output.getFullPathName(), operation.toString(), request.backend,
                    request.outputDevice, juce::String(request.sampleRate), juce::String(request.bufferSamples),
                    juce::String(request.outputChannels), juce::String(request.pingChannel), "--active-probe" };
            if (!request.physicalOutputs.isZero() || request.expectedOutputEndpoint.isNotEmpty()) arguments.add(request.requestedOutputs().toString(16));
            if (request.expectedOutputEndpoint.isNotEmpty()) arguments.add(request.expectedOutputEndpoint);
            worker = media::BoundedDecodeProcess::run(executable, arguments, output, policy, cancelled
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                , {}, 0, OptionalLifecycleOwner::diagnostics
#endif
                );
            result.workerReleased = worker.failure != media::BoundedDecodeProcess::Failure::cleanup;
            outcome = worker.failure == media::BoundedDecodeProcess::Failure::cancelled ? Outcome::cancelled : Outcome::worker;
            if (worker && output.existsAsFile() && output.getSize() <= 16384)
            {
                const auto report = juce::JSON::parse(output.loadFileAsString());
                const auto code = static_cast<int>(report["outcome"]);
                if (report["operation"].toString() == operation.toString() && report["outcome"].isInt()
                    && code >= 0 && code <= static_cast<int>(Outcome::close))
                {
                    outcome = static_cast<Outcome>(code);
                    result.evidence.fields.push_back({ Field::backend, request.backend });
                    if (report.hasProperty("rate")) result.evidence.fields.insert(result.evidence.fields.end(), {
                        { Field::activeRate, report["rate"] }, { Field::activeBuffer, report["buffer"] },
                        { Field::driverLatency, report["latency"] }, { Field::channelMapping, report["mapping"] },
                        { Field::activeChannels, report["mapping"].size() } });
                    if (report.hasProperty("frames")) result.evidence.fields.push_back({ Field::deviceProbeFrames, report["frames"] });
                    if (report["outputName"].isString()) result.evidence.identifiers.push_back({ LocalIdentifierKind::deviceName, report["outputName"].toString() });
                }
            }
            result.temporaryRemoved = temporary.remove();
            if (!result.temporaryRemoved) outcome = Outcome::cleanup;
        }
    }
    result.succeeded = outcome == Outcome::success; result.cancelled = outcome == Outcome::cancelled;
    result.evidence.fields.insert(result.evidence.fields.end(), {
        { Field::deviceState, worker.jobCleanupConfirmed ? "temporary-worker-released" : "probe-not-started-or-unconfirmed" }, { Field::negotiationStage, reason(outcome) },
        { Field::negotiationOutcome, result.succeeded ? "succeeded" : result.cancelled ? "cancelled" : "failed" },
        { Field::deviceProbeWorkerFailure, static_cast<int>(worker.failure) },
        { Field::deviceProbeCleanup, worker.jobCleanupConfirmed ? "confirmed" : "not-launched-or-unconfirmed" } });
    if (!result.succeeded && !result.cancelled)
    {
        result.evidence.fields.push_back({ Field::lastNegotiationFailure, reason(outcome) });
        result.evidence.latestFailure = FailureCapsule { operation,
            outcome == Outcome::cleanup ? FailureStage::cleanup : FailureStage::negotiation,
            static_cast<int64_t>(outcome), { worker.exitCode, worker.elapsedMilliseconds, worker.jobCleanupConfirmed ? 1 : 0, 0, 0, 0 } };
    }
    return result;
}
}
