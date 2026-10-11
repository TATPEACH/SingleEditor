#include "CodecDiagnosticProbe.h"
#include "DiagnosticTestActivity.h"
#include "Audio/SourceFileLease.h"
#include "Core/NativeFilePath.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace singleeditor::diagnostics
{
namespace
{
enum class Outcome { success, sourceUnavailable, readerUnavailable, dimensions, sampleRead, readBudget, worker, cancelled, cleanup, temporaryStorage };
struct ReadState { int64_t bytes = 0; bool exhausted = false; };
struct ProbeFiles final
{
    juce::File directory, output;
    bool created = false;
    bool remove()
    {
        if (!created) return true;
        const auto removed = (!output.exists() || output.deleteFile()) && directory.deleteFile();
        if (removed) created = false;
        return removed;
    }
    ~ProbeFiles() { remove(); }
};
class BoundedInput final : public juce::InputStream
{
public:
    BoundedInput(const juce::File& file, std::shared_ptr<media::SourceFileLease> lease, std::shared_ptr<ReadState> counter)
        : guard(std::move(lease)), input(files::nativeFile(file).createInputStream()), state(std::move(counter)) {}
    juce::int64 getTotalLength() override { return input ? input->getTotalLength() : 0; }
    juce::int64 getPosition() override { return input ? input->getPosition() : 0; }
    bool setPosition(juce::int64 position) override { return input && position >= 0 && position <= getTotalLength() && input->setPosition(position); }
    bool isExhausted() override { return !input || state->exhausted || input->isExhausted(); }
    int read(void* destination, int requested) override
    {
        if (!input || requested <= 0) return 0;
        const auto remaining = CodecDiagnosticProbe::readBudget - state->bytes;
        if (remaining <= 0) { state->exhausted = true; return 0; }
        const auto count = input->read(destination, static_cast<int>(std::min<int64_t>(remaining, requested)));
        if (count > 0) state->bytes += count;
        if (requested > remaining) state->exhausted = true;
        return count;
    }
private:
    // Ordinary admission leases pin the current path; the historical retained
    // stream API belongs only to an input preserved across real Save replacement.
    std::shared_ptr<media::SourceFileLease> guard;
    std::unique_ptr<juce::InputStream> input;
    std::shared_ptr<ReadState> state;
};
const char* reason(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::success: return "bounded-probe-completed";
        case Outcome::sourceUnavailable: return "source-unavailable";
        case Outcome::readerUnavailable: return "native-reader-unavailable";
        case Outcome::dimensions: return "reader-dimensions-refused";
        case Outcome::sampleRead: return "initial-window-read-failed";
        case Outcome::readBudget: return "probe-read-budget";
        case Outcome::worker: return "probe-worker-failed";
        case Outcome::cancelled: return "probe-cancelled";
        case Outcome::cleanup: return "probe-cleanup-failed";
        case Outcome::temporaryStorage: return "probe-temporary-unavailable";
    }
    return "probe-worker-failed";
}
const char* knownFormat(const juce::String& name)
{
    for (const auto* format : { "WAV", "AIFF", "FLAC", "Ogg-Vorbis", "MP3" })
        if (name.equalsIgnoreCase(format) || name.equalsIgnoreCase(juce::String(format) + " file")) return format;
    return nullptr;
}
juce::var observe(const juce::File& source, const juce::Uuid& operation)
{
    auto* object = new juce::DynamicObject(); juce::var report(object);
    object->setProperty("operation", operation.toString());
    auto outcome = Outcome::sourceUnavailable;
    auto counter = std::make_shared<ReadState>();
    int observedFrames = 0;
    std::unique_ptr<media::SourceFileLease> opened;
    if (media::SourceFileLease::open(source, opened).wasOk())
    {
        std::shared_ptr<media::SourceFileLease> lease(std::move(opened));
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(std::make_unique<BoundedInput>(source, lease, counter)));
        outcome = Outcome::readerUnavailable;
        if (reader)
        {
            outcome = Outcome::dimensions;
            const auto* format = knownFormat(reader->getFormatName());
            if (format && std::isfinite(reader->sampleRate) && reader->sampleRate > 0.0 && reader->sampleRate <= 768000.0
                && reader->numChannels > 0 && reader->numChannels <= 64 && reader->lengthInSamples >= 0)
            {
                object->setProperty("reader", format);
                object->setProperty("rate", reader->sampleRate);
                object->setProperty("channels", static_cast<int>(reader->numChannels));
                object->setProperty("frames", reader->lengthInSamples);
                const auto frames = static_cast<int>(std::min<int64_t>(reader->lengthInSamples, CodecDiagnosticProbe::windowFrames));
                juce::AudioBuffer<float> samples(static_cast<int>(reader->numChannels), std::max(1, frames));
                outcome = frames == 0 || reader->read(&samples, 0, frames, 0, true, true) ? Outcome::success : Outcome::sampleRead;
                if (outcome == Outcome::success) observedFrames = frames;
            }
        }
        if (counter->exhausted) outcome = Outcome::readBudget;
        if (lease->recheck().failed()) outcome = Outcome::sourceUnavailable;
    }
    object->setProperty("outcome", static_cast<int>(outcome));
    object->setProperty("readBytes", counter->bytes);
    object->setProperty("observedFrames", observedFrames);
    return report;
}
}
media::BoundedDecodeProcess::Limits CodecDiagnosticProbe::limits()
{
    media::BoundedDecodeProcess::Limits result;
    result.wallTimeMs = 5000; result.memoryBytes = 512ULL * 1024 * 1024;
    result.outputBytes = 16384; result.processes = 1;
    return result;
}
std::optional<int> CodecDiagnosticProbe::tryWorker(const juce::StringArray& args)
{
    if (args.isEmpty() || args[0] != "--diagnostic-codec-worker") return std::nullopt;
    if (args.size() != 5 || args[4] != "--active-probe" || !juce::File::isAbsolutePath(args[1]) || !juce::File::isAbsolutePath(args[2])
        || args[3].length() != 32 || !args[3].containsOnly("0123456789abcdef") || juce::Uuid(args[3]).isNull()) return 2;
    const juce::File output(args[2]);
    if (output.exists()) return 2;
    const auto report = observe(juce::File(args[1]), juce::Uuid(args[3]));
    const auto text = juce::JSON::toString(report, true);
    if (text.getNumBytesAsUTF8() > 16384) return 3;
    auto stream = files::nativeFile(output).createOutputStream();
    if (!stream || !stream->writeText(text, false, false, "\n")) return 5;
    stream->flush();
    return stream->getStatus().wasOk() ? 0 : 5;
}
CodecProbeResult CodecDiagnosticProbe::run(const juce::File& executable, const juce::File& source,
                                          const juce::Uuid& operation, const std::atomic<bool>& cancelled,
                                          media::BoundedDecodeProcess::Limits policy, Origin origin)
{
    SINGLE_EDITOR_DIAGNOSTIC_ACTIVITY(codecProbe);
    CodecProbeResult result;
    const auto* module = origin == Origin::gui ? "gui-codec-probe" : "standalone-codec-probe";
    result.evidence.operationIds.push_back(operation);
    result.evidence.identifiers = { { LocalIdentifierKind::path, source.getFullPathName() }, { LocalIdentifierKind::fileName, source.getFileName() } };
    result.evidence.fields = { { Field::dependencyIdentity, juce::SystemStats::getJUCEVersion().replaceCharacter(' ', '-') },
        { Field::fallbackReason, "external-decoder-not-requested" }, { Field::lastOperationModule, module } };
    auto outcome = cancelled.load() ? Outcome::cancelled : Outcome::sourceUnavailable;
    media::BoundedDecodeProcess::Result worker;
    int64_t observedBytes = 0, frames = 0;
    std::unique_ptr<media::SourceFileLease> lease;
    if (!cancelled.load() && !operation.isNull() && media::SourceFileLease::open(source, lease).wasOk())
    {
        ProbeFiles temporary;
        temporary.directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("SingleEditor-codec-probe-" + juce::Uuid().toString());
        temporary.output = temporary.directory.getChildFile("result.json");
        const auto& directory = temporary.directory; const auto& output = temporary.output;
        outcome = Outcome::temporaryStorage;
        if (directory.createDirectory().wasOk())
        {
            temporary.created = true;
            // Only stricter internal policies are accepted, including focused tests.
            const auto maximum = limits();
            policy.wallTimeMs = std::min(policy.wallTimeMs, maximum.wallTimeMs);
            policy.memoryBytes = std::min(policy.memoryBytes, maximum.memoryBytes);
            policy.outputBytes = std::min(policy.outputBytes, maximum.outputBytes); policy.processes = 1;
            worker = media::BoundedDecodeProcess::run(executable,
                { "--diagnostic-codec-worker", source.getFullPathName(), output.getFullPathName(), operation.toString(), "--active-probe" }, output, policy, cancelled
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                , {}, 0, OptionalLifecycleOwner::diagnostics
#endif
                );
            outcome = worker.failure == media::BoundedDecodeProcess::Failure::cancelled ? Outcome::cancelled : Outcome::worker;
            if (worker && output.existsAsFile() && output.getSize() <= 16384)
            {
                const auto report = juce::JSON::parse(output.loadFileAsString());
                const auto code = static_cast<int>(report["outcome"]);
                if (report["operation"].toString() == operation.toString() && report.hasProperty("outcome")
                    && code >= 0 && code <= static_cast<int>(Outcome::readBudget))
                {
                    outcome = static_cast<Outcome>(code);
                    observedBytes = static_cast<int64_t>(report["readBytes"]);
                    frames = static_cast<int64_t>(report["observedFrames"]);
                    if (const auto* format = knownFormat(report["reader"].toString()))
                        result.evidence.fields.insert(result.evidence.fields.end(), { { Field::container, format },
                            { Field::selectedReader, juce::String("JUCE-") + format }, { Field::mediaRate, report["rate"] },
                            { Field::mediaChannels, report["channels"] }, { Field::mediaFrames, report["frames"] } });
                }
            }
            if (lease->recheck().failed()) outcome = Outcome::sourceUnavailable;
            // Exact fresh owned paths only; no broad temporary-directory sweep.
            result.temporaryRemoved = temporary.remove();
            if (!result.temporaryRemoved) outcome = Outcome::cleanup;
        }
    }
    result.succeeded = outcome == Outcome::success;
    result.cancelled = outcome == Outcome::cancelled;
    result.evidence.fields.insert(result.evidence.fields.end(), {
        { Field::policyOutcome, reason(outcome) }, { Field::lastOperationState, result.succeeded ? "ready" : result.cancelled ? "cancelled" : "failed" },
        { Field::lastOperationReadyFrames, frames }, { Field::lastOperationProfile, "bounded-initial-window" } });
    if (!result.succeeded && !result.cancelled)
    {
        result.evidence.fields.insert(result.evidence.fields.end(), { { Field::lastFailureReason, reason(outcome) },
            { Field::codecFailureSource, module }, { Field::codecFailureDetail, static_cast<int>(worker.failure) } });
        const auto stage = outcome == Outcome::sourceUnavailable ? FailureStage::admission
            : outcome == Outcome::temporaryStorage ? FailureStage::write
            : outcome == Outcome::cleanup ? FailureStage::cleanup : FailureStage::decode;
        result.evidence.latestFailure = FailureCapsule { operation, stage, static_cast<int64_t>(outcome),
            { worker.exitCode, observedBytes, readBudget, worker.elapsedMilliseconds, worker.jobCleanupConfirmed ? 1 : 0, 0 } };
    }
    return result;
}
}
