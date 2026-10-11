#include "Audio/AudioEngine.h"
#include "Audio/AmbisonicMonitorDecoder.h"
#include "Audio/AmbisonicSpeakerGeometry.h"
#include "Audio/ChannelRouter.h"
#include "Audio/WindowsMediaAudioService.h"
#include "Audio/VgmstreamDecodeService.h"
#include "Document/DocumentSaveCoordinator.h"
#include "Document/SingleTrackDocument.h"
#include "Recovery/DocumentRecoveryCapture.h"
#include "Bnk/BnkParser.h"
#include "Bnk/BnkSidecarParser.h"
#include "Bnk/BoundedSidecarInput.h"
#include "Bnk/BnkWemPlaybackBackend.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <array>
#include <cstring>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#endif

void runRecoveryAudioGraphTests();
int runPackedDecoderFixture(int argc, char* argv[]);
int runPackedCacheObserver(const juce::File&, bool cancel);
void runPackedMediaLifecycleTests();
void runBankDecodeOperationTests();
void runRealBankCharacterization(const juce::File&, bool);
void runBankPreviewRequestTests();
void runBnkRuntimeSessionTests();
void runBankOpenRequestTests();
void runBankParserHardeningTests();
void runBnkSidecarSemanticsTests();
void runBnkSidecarTargetTests();
void runBnkStateGroupTests();
void runBnkRtpcIdentityTests();
void runBnkMediaLocatorTests();
void runBnkDerivedPathTests();
void runBankParserCorpusTests(const juce::File&, int onlyCase = -1);
int runBankCorpusAsanWitness();
void runWorkspaceSnapshotTests(const juce::File&);
void runRecoverySessionHostTests(const juce::File&);
void runRecoveryServiceTests(const juce::File&);
void runRecoveryOrdinaryAudioTests(const juce::File&, bool burst = false);
void runRecoveryCapacityPilot(const juce::File&);
void runRecoveryLargeAudioPilot(const juce::File&, const juce::File&, bool applyDsp = false);
void runRecoveryStreamedCaptureTests(const juce::File&);
void runRecoveryMutationProducerTests(const juce::File&);
void runRecoveryExpiryTests(const juce::File&);
void runRecoveryRetentionTests(const juce::File&);
int runRecoveryExpiryLeaseChild(const juce::File&, const juce::Uuid&);
int runRecoveryCompactionCrashChild(const juce::File&, juce::Uuid, int);

namespace singleeditor
{
// This executable does not link AudioDeviceDiagnosticsTests.cpp. Use the existing
// friend seam to retain one real transport lifetime without opening a device.
struct AudioEngineDeviceTestAccess
{
    class TransportSession
    {
    public:
        TransportSession(AudioEngine& audioEngine, int channels, int blockSize, double sampleRate)
            : engine(audioEngine)
        {
            engine.playbackRateSource->setRenderChannelCount(channels);
            engine.transportSource.prepareToPlay(blockSize, sampleRate);
            engine.transportSource.setPosition(0.0);
            engine.transportSource.start();
        }

        ~TransportSession()
        {
            engine.transportSource.stop();
            engine.transportSource.releaseResources();
        }

        void render(juce::AudioBuffer<float>& buffer)
        {
            buffer.clear();
            engine.transportSource.getNextAudioBlock({ &buffer, 0, buffer.getNumSamples() });
        }

        TransportSession(const TransportSession&) = delete;
        TransportSession& operator=(const TransportSession&) = delete;

    private:
        AudioEngine& engine;
    };
};
}

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

bool writeTestWavFile(const juce::File& file, double sampleRate = 48000.0)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(1)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(1, 4800);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        buffer.setSample(0, sample, sample < 960 ? 0.75f : 0.25f);

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeTestFlacFile(const juce::File& file)
{
    juce::FlacAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(1)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(1, 4800);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        buffer.setSample(0, sample, sample < 960 ? 0.75f : 0.25f);
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeBwfTestWavFile(const juce::File& file, bool useNonZeroCueBlockStart = false)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    std::unordered_map<juce::String, juce::String> metadata;
    metadata[juce::WavAudioFormat::bwavDescription] = "Single Editor metadata audit";
    metadata[juce::WavAudioFormat::bwavTimeReference] = "96000";
    metadata["NumCuePoints"] = "3";
    metadata["Cue0Identifier"] = "101";
    metadata["Cue0Offset"] = "240";
    metadata["Cue1Identifier"] = "102";
    metadata["Cue1Offset"] = "960";
    if (useNonZeroCueBlockStart)
        metadata["Cue1BlockStart"] = "128";
    metadata["Cue2Identifier"] = "103";
    metadata["Cue2Offset"] = "4000";
    metadata["NumCueLabels"] = "2";
    metadata["CueLabel0Identifier"] = "101";
    metadata["CueLabel0Text"] = "Removed head marker";
    metadata["CueLabel1Identifier"] = "102";
    metadata["CueLabel1Text"] = "Surviving marker";
    metadata["NumSampleLoops"] = "2";
    metadata["Loop0Identifier"] = "201";
    metadata["Loop0Start"] = "100";
    metadata["Loop0End"] = "300";
    metadata["Loop1Identifier"] = "202";
    metadata["Loop1Start"] = "1000";
    metadata["Loop1End"] = "2000";
    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(1)
                       .withBitsPerSample(16)
                       .withMetadataValues(metadata);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(1, 4800);
    buffer.clear();
    buffer.applyGain(0.25f);
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeStereoPatternWavFile(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(2)
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(2, 6000);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto left = (static_cast<float>(sample % 997) / 997.0f) - 0.5f;
        const auto right = (static_cast<float>((sample * 7) % 991) / 991.0f) - 0.5f;
        buffer.setSample(0, sample, left);
        buffer.setSample(1, sample, right);
    }

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
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

    juce::AudioBuffer<float> buffer(4, 1024);
    buffer.clear();
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        buffer.setSample(0, sample, 0.25f); // ACN0 W
        buffer.setSample(3, sample, 0.25f); // ACN3 X (front)
    }

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool createGrowingStereoWav(const juce::File& file, int totalSamples, int initialSamples)
{
    file.deleteFile();
    juce::FileOutputStream output(file);
    if (!output.openedOk())
        return false;

    constexpr int channels = 2;
    constexpr int bytesPerSample = 2;
    constexpr int sampleRate = 48000;
    const auto dataBytes = totalSamples * channels * bytesPerSample;
    output.write("RIFF", 4);
    output.writeInt(36 + dataBytes);
    output.write("WAVEfmt ", 8);
    output.writeInt(16);
    output.writeShort(1);
    output.writeShort(channels);
    output.writeInt(sampleRate);
    output.writeInt(sampleRate * channels * bytesPerSample);
    output.writeShort(channels * bytesPerSample);
    output.writeShort(bytesPerSample * 8);
    output.write("data", 4);
    output.writeInt(dataBytes);
    for (int sample = 0; sample < initialSamples; ++sample)
    {
        output.writeShort(8192);
        output.writeShort(-8192);
    }
    output.flush();
    return output.getStatus().wasOk();
}

bool appendGrowingStereoSamples(const juce::File& file, int samples)
{
    juce::FileOutputStream output(file);
    if (!output.openedOk() || !output.setPosition(file.getSize()))
        return false;

    for (int sample = 0; sample < samples; ++sample)
    {
        output.writeShort(16384);
        output.writeShort(-16384);
    }
    output.flush();
    return output.getStatus().wasOk();
}

bool writeMaskedWavFile(const juce::File& file, int channelCount, juce::uint32 channelMask)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withChannelLayout(juce::AudioChannelSet::fromWaveChannelMask(static_cast<juce::int32>(channelMask)))
                       .withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(channelCount, 4800);
    for (int channel = 0; channel < channelCount; ++channel)
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample(channel, sample, (channel + 1) * 0.01f);
    }

    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeMaskedRoutingImpulseWavFile(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withChannelLayout(juce::AudioChannelSet::fromWaveChannelMask(0x63f))
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(8, 16);
    buffer.clear();
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        buffer.setSample(channel, channel, 0.1f * static_cast<float>(channel + 1));
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool writeMaskedHeightRoutingImpulseWavFile(const juce::File& file)
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

    juce::AudioBuffer<float> buffer(12, 16);
    buffer.clear();
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        buffer.setSample(channel, channel, 0.05f * static_cast<float>(channel + 1));
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

juce::uint32 readWaveChannelMask(const juce::File& file)
{
    auto reader = SingleTrackDocument::createReaderForFile(file);
    if (reader == nullptr)
        return 0;

    const auto value = reader->metadataValues.getValue("ChannelMask", {}).trim();
    return static_cast<juce::uint32>(value.startsWithIgnoreCase("0x")
                                         ? value.fromFirstOccurrenceOf("x", false, true).getHexValue64()
                                         : value.getLargeIntValue());
}

struct CountingListener final : SingleTrackDocument::Listener
{
    void documentChanged(SingleTrackDocument::StatePtr) override
    {
        ++count;
    }

    int count = 0;
};

class AdmissionTestReader final : public juce::AudioFormatReader
{
public:
    AdmissionTestReader(double rate, unsigned int channels, int64_t samples, bool readSucceeds = true)
        : juce::AudioFormatReader(nullptr, "Admission fixture"), succeeds(readSucceeds)
    {
        sampleRate = rate;
        numChannels = channels;
        lengthInSamples = samples;
        bitsPerSample = 32;
        usesFloatingPointData = true;
    }
    bool readSamples(int* const* channels, int count, int offset, juce::int64, int samples) override
    {
        ++reads;
        if (!succeeds) return false;
        for (int i = 0; i < count; ++i)
            if (channels[i] != nullptr) std::fill_n(channels[i] + offset, samples, 0);
        return true;
    }
    int reads = 0;
    bool succeeds;
};

void runReaderAdmissionTest()
{
    using media::ReaderFailure;
    for (const auto rate : { 0.0, -1.0, std::numeric_limits<double>::infinity(), 768001.0 })
        require(media::admitReader(rate, 2, 16).failure == ReaderFailure::sampleRate, "rate admission boundary");
    require(media::admitReader(768000.0, 32, 16).floatBytes == 2048, "maximum supported facts use checked byte count");
    for (const auto channels : { 0ULL, 33ULL, std::numeric_limits<uint64_t>::max() })
        require(media::admitReader(48000.0, channels, 16).failure == ReaderFailure::channels, "channel bound before narrowing");
    require(media::admitReader(48000.0, 2, -1).failure == ReaderFailure::sampleLength
                && media::admitReader(48000.0, 2, std::numeric_limits<int64_t>::max()).failure == ReaderFailure::byteOverflow,
            "invalid lengths and byte multiplication overflow rejected");
    require(media::admitMaterialisation(1, static_cast<int64_t>(std::numeric_limits<int>::max()) + 1).failure
                == ReaderFailure::sampleNarrowing, "frame narrowing precedes allocation");
    const auto oversized = static_cast<int64_t>(media::maximumMaterialisedBytes / 4 + 1);
    require(media::admitMaterialisation(1, oversized).failure == ReaderFailure::materialisationLimit,
            "full materialisation has a distinct ceiling from streaming admission");
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-admission-" + juce::Uuid().toString() + ".bin");
    require(file.replaceWithText("private synthetic reader fixture"), "create private reader admission file");
    AdmissionTestReader nanReader(std::numeric_limits<double>::quiet_NaN(), 2, 16);
    require(!SingleTrackDocument::buildFileInfoForReader(file, nanReader).isValid() && nanReader.reads == 0,
            "post-reader guard rejects nonfinite sample rate without reading audio");
    AdmissionTestReader failedReader(48000.0, 2, 16, false);
    require(SingleTrackDocument::createStateFromReader(file, failedReader) == nullptr && failedReader.reads == 1,
            "reader failure cannot publish a silent usable document");
    auto reader = std::make_unique<AdmissionTestReader>(48000.0, 1, oversized);
    auto* readerObserver = reader.get();
    auto state = SingleTrackDocument::createStateFromOwnedReader(file, std::move(reader));
    require(state != nullptr && state->hasPlaybackSource() && readerObserver->reads == 0 && state->audioBuffer == nullptr,
            "valid oversized media stays stream-backed without materialisation");
    SingleTrackDocument document;
    require(document.adoptLoadedState(state), "oversized stream-backed document remains openable");
    const auto version = document.getMediaIdentity().document;
    require(!document.applySelectionGain({ 0.0, 0.01 }, 0.5f)
                && !document.beginEqualLengthAudioMutation(document.getState()) && readerObserver->reads == 0
                && document.getMediaIdentity().document.sameVersion(version) && !document.canUndo(),
            "unsupported bulk edit refuses before allocation/read and preserves document");
    SingleTrackDocument::PreparedFileLoad inconsistent;
    inconsistent.ownedReader = std::make_unique<AdmissionTestReader>(48000.0, 2, 16);
    inconsistent.info = SingleTrackDocument::buildFileInfoForReader(file, *inconsistent.ownedReader);
    inconsistent.info.numSamples = 32;
    require(!SingleTrackDocument::createStateFromPreparedFile(std::move(inconsistent)),
            "prepared dimensions must match the reader they describe");
    document.clear();
    state.reset();
    require(file.deleteFile(), "remove only owned admission fixture");
    std::cout << "ReaderAdmissionTests passed\n";
}

void runEnvelopeGestureIdentityTest()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-envelope-identity-" + juce::Uuid().toString() + ".wav");
    require(writeTestWavFile(file), "create private envelope identity fixture");
    SingleTrackDocument document;
    require(document.loadFile(file), "open envelope identity fixture");
    const auto initial = document.getMediaIdentity().document;
    std::vector<recovery::DocumentRecoveryCapture::Capture> captures;
    recovery::DocumentRecoveryCapture capture(document, [&] (auto value) { captures.push_back(std::move(value)); });
    document.beginEnvelopeGesture();
    require(!document.moveEnvelopePoint(0, 0.0, 1.0f) && !document.hasUnsavedChanges() && captures.empty(),
            "no-op gesture point creates no dirty state or recovery record");
    require(document.moveEnvelopePoint(0, 0.0, 0.75f), "first live envelope mutation");
    const auto first = document.getMediaIdentity().document;
    require(document.hasUnsavedChanges() && !first.sameVersion(initial) && first.sameDocument(initial),
            "live envelope gesture advances document generation and dirty state");
    require(captures.size() == 1 && captures.front().document.has_value()
                && captures.front().document->undo.size() == 1
                && captures.front().document->identity.document.sameVersion(first),
            "post-Core consumer receives one atomic identity/state/Undo capture");
    const auto preserved = *captures.front().document;
    const auto snapshot = document.createSaveSnapshot(file);
    require(document.moveEnvelopePoint(0, 0.0, 0.5f), "second live envelope mutation");
    const auto savedGesture = SingleTrackDocument::writeSaveSnapshot(snapshot);
    require(savedGesture.succeeded && document.markSaveSnapshotCommitted(snapshot, savedGesture), "commit actual earlier gesture save publication");
    require(preserved.state->envelopePoints.front().gain == 0.75f
                && preserved.identity.document.sameVersion(first)
                && !document.captureCommittedState(first).has_value(),
            "queued capture remains immutable and stale capture requests fail closed");
    require(document.hasUnsavedChanges(), "save of earlier gesture point cannot clean a newer point");
    document.endEnvelopeGesture();
    require(document.undo() && std::abs(document.getEnvelopePoints().front().gain - 1.0f) < 0.0001f,
            "entire gesture retains one Undo step");
    require(!document.canUndo(), "gesture does not create per-point Undo steps");
    require(document.redo() && std::abs(document.getEnvelopePoints().front().gain - 0.5f) < 0.0001f,
            "Redo restores final gesture point");
    require(static_cast<bool>(capture.latestFailure()) && captures.size() == 5,
            "recovery consumes gesture generations, checkpoint, Undo and Redo in order");
    int failedConsumerCalls = 0;
    recovery::DocumentRecoveryCapture brokenConsumer(document, [&] (auto)
    {
        ++failedConsumerCalls;
        throw std::runtime_error("injected consumer failure");
    });
    document.resetEnvelope();
    document.resetEnvelope();
    require(failedConsumerCalls == 1 && !brokenConsumer.latestFailure(),
            "consumer failure cannot throw through committed edit or silently resume after a gap");
    document.clear();
    require(file.deleteFile(), "remove only owned envelope fixture");
    std::cout << "EnvelopeGestureIdentityTests passed\n";
}

void runMediaIdentityTest()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("SingleEditorIdentity-" + juce::Uuid().toString(), {}, false);
    require(root.createDirectory().wasOk(), "create private identity fixture directory");
    const auto a = root.getChildFile("a.wav"), b = root.getChildFile("b.wav");
    const auto saved = root.getChildFile("saved.wav"), exported = root.getChildFile("export.wav");
    require(writeTestWavFile(a) && a.copyFileTo(b), "create byte-identical distinct source files");
    {
        SingleTrackDocument document, other;
        struct IdentityListener final : SingleTrackDocument::Listener
        {
            std::vector<media::IdentityEvent> events;
            void documentIdentityChanged(const media::IdentityEvent& event) override { events.push_back(event); }
        } listener;
        document.addListener(&listener);
        require(!document.getMediaIdentity().document.isValid() && !document.getMediaIdentity().asset,
                "empty document has no media lineage or catalog reference");
        require(document.loadFile(a) && other.loadFile(a), "open same location in independent documents");
        const auto opened = document.getMediaIdentity();
        require(opened.document.isValid() && opened.source.fastStampValid && !opened.asset
                    && opened.document.generation == other.getMediaIdentity().document.generation
                    && !opened.document.sameDocument(other.getMediaIdentity().document)
                    && !opened.source.sameBinding(other.getMediaIdentity().source),
                "matching file/path/generation never merges runtime document or source bindings");
        const auto staleSaveAs = document.createSaveSnapshot(saved, SingleTrackDocument::SaveIntent::saveAs);
        require(document.loadFile(b), "replace current document before old save completion");
        const auto replacement = document.getMediaIdentity();
        const auto staleResult = SingleTrackDocument::writeSaveSnapshot(staleSaveAs);
        require(staleResult.succeeded, "old snapshot can finish its requested output");
        document.markSaveSnapshotCommitted(staleSaveAs, staleResult);
        require(document.getFileInfo().file == b && document.getMediaIdentity().document.sameVersion(replacement.document),
                "stale Save As completion cannot rebind a newly opened document");
        other.markSaveSnapshotCommitted(staleSaveAs, staleResult);
        require(other.getFileInfo().file == a, "save from another runtime instance cannot rebind this document");
        document.clear();
        document.markSaveSnapshotCommitted(staleSaveAs, staleResult);
        require(!document.getMediaIdentity().document.isValid() && document.getFileInfo().file == juce::File(),
                "old completion cannot resurrect a cleared document");
        require(document.loadFile(a), "reopen same path creates an explicit new lineage");
        require(!document.getMediaIdentity().document.sameDocument(opened.document), "reopen is not a metadata-based identity merge");
        const auto beforeEdit = document.getMediaIdentity();
        require(document.applySelectionGain({ 0.0, 0.05 }, 0.5f), "first identity mutation");
        const auto edited = document.getMediaIdentity();
        require(edited.document.sameDocument(beforeEdit.document) && !edited.document.sameVersion(beforeEdit.document),
                "mutation advances generation within one lineage/runtime");
        const auto checkpoint = document.createSaveSnapshot();
        require(document.applySelectionGain({ 0.0, 0.05 }, 0.5f), "second identity mutation");
        const auto latest = document.getMediaIdentity();
        const auto checkpointResult = SingleTrackDocument::writeSaveSnapshot(checkpoint);
        if (!checkpointResult.succeeded) std::cerr << "Save publication: " << checkpointResult.errorMessage << '\n';
        require(checkpointResult.succeeded, "write captured generation while newer edits exist");
        document.markSaveSnapshotCommitted(checkpoint, checkpointResult);
        require(document.hasUnsavedChanges() && document.getMediaIdentity().document.sameVersion(latest.document)
                    && document.getMediaIdentity().savedGeneration == edited.document.generation,
                "checkpoint marks only saved generation, preserving newer dirty state");
        require(document.undo() && !document.hasUnsavedChanges()
                    && document.getMediaIdentity().document.sameVersion(edited.document)
                    && document.redo() && document.hasUnsavedChanges(),
                "Undo/Redo restore content generation while preserving document lineage");
        auto stagedMutation = document.beginEqualLengthAudioMutation(document.getState());
        require(stagedMutation != nullptr, "prepare mutation before source rebind");
        juce::AudioBuffer<float> staging;
        const auto stagingSamples = static_cast<int>(stagedMutation->getNumSamples());
        require(stagedMutation->readSourceBlock(0, stagingSamples, staging)
                    && stagedMutation->writeRenderedBlock(0, staging, 0, stagingSamples),
                "fully render staged mutation before Save As");
        const auto saveAs = document.createSaveSnapshot(saved, SingleTrackDocument::SaveIntent::saveAs);
        const auto beforeSaveAs = document.getMediaIdentity();
        const auto saveAsResult = SingleTrackDocument::writeSaveSnapshot(saveAs);
        require(saveAsResult.succeeded, "Save As current lineage");
        document.markSaveSnapshotCommitted(saveAs, saveAsResult);
        const auto rebound = document.getMediaIdentity();
        require(rebound.document.sameVersion(beforeSaveAs.document) && !rebound.source.sameBinding(beforeSaveAs.source)
                    && rebound.source.location.file == saved && !document.hasUnsavedChanges(),
                "Save As atomically rebinds source without discarding live document lineage");
        require(!document.commitEqualLengthAudioMutation(std::move(stagedMutation))
                    && document.getMediaIdentity().document.sameVersion(rebound.document),
                "source rebind invalidates staged mutation even when state pointer and document generation are unchanged");
        document.markSaveSnapshotCommitted(checkpoint, checkpointResult);
        require(document.getMediaIdentity().source.sameBinding(rebound.source)
                    && document.getMediaIdentity().savedGeneration == rebound.savedGeneration,
                "late checkpoint for previous source cannot overwrite the rebound source identity or clean head");
        const auto exportCopy = document.createSaveSnapshot(exported, SingleTrackDocument::SaveIntent::exportCopy);
        const auto eventCount = listener.events.size();
        const auto exportResult = SingleTrackDocument::writeSaveSnapshot(exportCopy);
        require(exportResult.succeeded, "export snapshot");
        document.markSaveSnapshotCommitted(exportCopy, exportResult);
        require(document.getMediaIdentity().source.sameBinding(rebound.source) && document.getFileInfo().file == saved
                    && listener.events.size() == eventCount, "Export does not rebind or checkpoint the live document");
        const std::vector<media::IdentityEventKind> expectedKinds {
            media::IdentityEventKind::opened, media::IdentityEventKind::opened,
            media::IdentityEventKind::cleared, media::IdentityEventKind::opened,
            media::IdentityEventKind::generationChanged, media::IdentityEventKind::generationChanged,
            media::IdentityEventKind::saveCheckpoint, media::IdentityEventKind::generationChanged,
            media::IdentityEventKind::generationChanged, media::IdentityEventKind::sourceRebound };
        require(listener.events.size() == expectedKinds.size(), "no identity events for rejected completion or Export");
        for (size_t i = 0; i < listener.events.size(); ++i)
            require(listener.events[i].sequence == i + 1 && listener.events[i].kind == expectedKinds[i],
                    "identity event kinds and sequence are deterministic without gaps");
        document.removeListener(&listener);
    }
    for (const auto& file : { a, b, saved, exported }) require(file.deleteFile(), "remove owned identity fixture");
    require(root.deleteFile(), "remove empty private fixture directory");
    std::cout << "MediaIdentityTests passed (ownership, stale completion, generation, save/rebind/export, ordered events)\n";
}

void runSavePublicationTest()
{
#if JUCE_WINDOWS
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-SavePublication-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private save publication fixture root");
    const auto source = root.getChildFile("source.wav"), saved = root.getChildFile("saved.wav");
    const auto newerTarget = root.getChildFile("newer.wav"), substitute = root.getChildFile("substitute.wav");
    {
        require(writeTestWavFile(source), "write independent save publication source");
        {
            require(writeTestWavFile(substitute), "write owned replacement-lease probe staging");
            std::unique_ptr<media::SourceFileLease> original, staged;
            require(media::SourceFileLease::open(source, original).wasOk()
                && media::SourceFileLease::openStagedWrite(substitute, staged).wasOk(), "hold both physical objects for save lease probe");
            const auto replaced = staged->publishTo(source);
            std::cout << "Save target lease probe: " << (replaced.wasOk() ? "replaced" : replaced.getErrorMessage()) << '\n';
            if (replaced.wasOk())
                require(original->recheck().failed(), "old handle cannot certify a pathname replaced under POSIX semantics");
            else
                require(original->recheck().wasOk() && substitute.exists(), "failed replacement preserves original source and staging");
            staged.reset(); original.reset();
            if (substitute.exists()) require(substitute.deleteFile(), "remove only unused owned probe staging");
        }
        SingleTrackDocument document;
        require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.6f), "create actual dirty document for save proofs");
        const auto request = document.createSaveSnapshot(saved, SingleTrackDocument::SaveIntent::saveAs);
        SingleTrackDocument::SaveWriteResult fake { true, saved, request.changeGeneration, {} };
        require(!document.markSaveSnapshotCommitted(request, fake) && document.hasUnsavedChanges()
            && !saved.exists(), "fabricated success without actual publication cannot clean or rebind a document");
        const auto result = SingleTrackDocument::writeSaveSnapshot(request);
        require(result.succeeded && result.publication && result.publication->documentIdentity().sameVersion(request.documentIdentity)
            && result.publication->targetVersion()->file == saved, "actual writer issues physical publication bound to requested document version");
        auto wrong = request; wrong.requestSerial++;
        require(!document.markSaveSnapshotCommitted(wrong, result), "receipt cannot be reused for a different save request");
        wrong = request; wrong.targetFile = newerTarget;
        require(!document.markSaveSnapshotCommitted(wrong, result), "receipt cannot certify another target");
        wrong = request; wrong.sourceBinding = juce::Uuid();
        require(!document.markSaveSnapshotCommitted(wrong, result), "receipt cannot certify a different captured source binding");
        wrong = request; wrong.state = std::make_shared<SingleTrackDocument::State>(*request.state);
        require(!document.markSaveSnapshotCommitted(wrong, result), "copied metadata with a different state owner cannot borrow publication");
        fake = result; fake.publication.reset();
        require(!document.markSaveSnapshotCommitted(request, fake), "public success flags cannot replace the private issued receipt");
        const auto savedFacts = result.publication->targetVersion();
        // Same physical file, same length and exact LastWriteTime; only actual
        // content/ChangeTime changes. Millisecond restamping is not sufficient.
        HANDLE altered = CreateFileW(saved.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(altered != INVALID_HANDLE_VALUE, "open only saved fixture for exact restamp fault");
        LARGE_INTEGER position; position.QuadPart = savedFacts->sizeBytes - 1;
        DWORD count = 0; unsigned char byte = 0;
        require(SetFilePointerEx(altered, position, nullptr, FILE_BEGIN) && ReadFile(altered, &byte, 1, &count, nullptr) && count == 1,
            "read owned saved sample byte");
        byte ^= 1;
        require(SetFilePointerEx(altered, position, nullptr, FILE_BEGIN) && WriteFile(altered, &byte, 1, &count, nullptr) && count == 1,
            "modify owned file in place without changing file identity or size");
        FILETIME stamp { static_cast<DWORD>(savedFacts->modifiedTicks), static_cast<DWORD>(static_cast<uint64_t>(savedFacts->modifiedTicks) >> 32) };
        require(FlushFileBuffers(altered) && SetFileTime(altered, nullptr, nullptr, &stamp) && CloseHandle(altered),
            "flush modification and restore exact original LastWriteTime");
        std::unique_ptr<media::SourceFileLease> changed;
        require(media::SourceFileLease::open(saved, changed).wasOk()
            && changed->witness()->fileId == savedFacts->fileId && changed->witness()->sizeBytes == savedFacts->sizeBytes
            && changed->witness()->modifiedTicks == savedFacts->modifiedTicks && changed->witness()->changedTicks != savedFacts->changedTicks,
            "fault isolates ChangeTime with physical ID, size and modification time unchanged");
        changed.reset();
        require(!document.markSaveSnapshotCommitted(request, result) && document.hasUnsavedChanges(),
            "restamped saved-output mutation cannot clean the live document");
        const auto repaired = SingleTrackDocument::writeSaveSnapshot(request);
        require(repaired.succeeded && saved.copyFileTo(substitute), "render exact retry and copy equal bytes to a different object");
        require(substitute.replaceFileIn(saved), "replace only owned output with equal bytes under a different physical identity");
        require(!document.markSaveSnapshotCommitted(request, repaired), "equal output bytes at same path do not certify the earlier published object");
        const auto verified = SingleTrackDocument::writeSaveSnapshot(request);
        require(verified.succeeded && document.markSaveSnapshotCommitted(request, verified)
            && !document.hasUnsavedChanges(), "only intact actual publication cleans requested generation");
        const auto rebound = document.getMediaIdentity();
        require(!document.markSaveSnapshotCommitted(request, verified)
            && document.getMediaIdentity().source.sameBinding(rebound.source), "duplicate save completion does not create another source epoch");
        require(document.moveEnvelopePoint(0, 0, 0.4f), "continue editing after validated publication");
        const auto earlier = document.createSaveSnapshot(saved, SingleTrackDocument::SaveIntent::saveAs);
        const auto later = document.createSaveSnapshot(newerTarget, SingleTrackDocument::SaveIntent::saveAs);
        const auto earlierResult = SingleTrackDocument::writeSaveSnapshot(earlier);
        const auto laterResult = SingleTrackDocument::writeSaveSnapshot(later);
        require(earlierResult.succeeded && laterResult.succeeded && document.markSaveSnapshotCommitted(later, laterResult),
            "newer requested Save As completes first for the same live generation");
        require(!document.markSaveSnapshotCommitted(earlier, earlierResult) && document.getFileInfo().file == newerTarget,
            "older genuine Save As receipt cannot rewind the current target after a later save");
        require(document.moveEnvelopePoint(0, 0, 0.2f), "create dirty state before unverified async writer");
        bool notified = false, succeeded = true;
        DocumentSaveCoordinator coordinator(document, [] (const auto& pending)
        { return SingleTrackDocument::SaveWriteResult { true, pending.targetFile, pending.changeGeneration, {} }; });
        require(coordinator.requestSave(document.createSaveSnapshot(), [&] (const auto& completed)
        { notified = true; succeeded = completed.succeeded; }), "dispatch actual coordinator with an unverified completion");
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;
        while (!notified && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(notified && !succeeded && document.hasUnsavedChanges(), "native coordinator reports rejected proof and keeps current edits dirty");
    }
    for (const auto& file : { source, saved, newerTarget }) require(file.deleteFile(), "remove only owned save publication fixture file");
    require(!substitute.exists() && root.deleteFile(), "release all publication leases and remove empty owned directory");
    std::cout << "SavePublicationTests passed (actual flush/replace, private receipts, restamp, physical substitution, save ordering, async rejection)\n";
#endif
}

void runRetainedSaveInputTest()
{
#if JUCE_WINDOWS
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-RetainedSave-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create independent retained-input fixture root");
    const auto file = root.getChildFile("source.wav");
    const auto alternate = root.getChildFile("alternate.wav");
    std::weak_ptr<media::SourceFileLease> lifetime;
    std::shared_ptr<const SingleTrackDocument::SavePublication> savedProof;
    {
        require(writeTestWavFile(file), "write known source bytes for retained-input comparison");
        juce::MemoryBlock originalBytes;
        require(file.loadFileAsData(originalBytes), "capture independent original-file byte oracle before save");
        juce::AudioBuffer<float> originalSample(1, 1);
        {
            auto beforeSave = SingleTrackDocument::createReaderForFile(file);
            require(beforeSave && beforeSave->read(&originalSample, 0, 1, 0, true, false)
                && std::abs(originalSample.getSample(0, 0) - 0.75f) < 0.00004f,
                "decode independent original PCM oracle with the fixture's 16-bit quantization");
        }
        SingleTrackDocument document, unrelated;
        require(document.loadFile(file) && unrelated.loadFile(file), "open separate documents over the same original physical file");
        const auto source = document.getState()->sourceFileWitness;
        require(source && !source->retainedInput(), "ordinary read-only admission creates no retained file lease");
        require(document.moveEnvelopePoint(0, 0, 0.5f), "commit envelope before original-file replacement");
        const auto request = document.createSaveSnapshot();
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && saved.publication && saved.publication->replacedSourceVersion()
            && document.markSaveSnapshotCommitted(request, saved), "publish and adopt real original-source replacement");
        savedProof = saved.publication;
        auto retained = source->retainedInput(); lifetime = retained;
        require(retained && retained->recheck().wasOk() && !unrelated.getState()->sourceFileWitness->retainedInput(),
            "only matching immutable source ownership receives retained physical input");
        auto substitutedFacts = std::make_shared<media::SourceFileWitness>(*source);
        substitutedFacts->file = alternate;
        require(!substitutedFacts->retainedInput() && !SingleTrackDocument::createReaderForRetainedSource(substitutedFacts),
            "copied source facts cannot borrow a retained handle after changing its provenance");
        auto reader = SingleTrackDocument::createReaderForRetainedSource(source);
        auto output = SingleTrackDocument::createReaderForFile(file);
        juce::AudioBuffer<float> oldSample(1, 1), savedSample(1, 1);
        require(reader && output && reader->read(&oldSample, 0, 1, 0, true, false)
            && output->read(&savedSample, 0, 1, 0, true, false)
            && oldSample.getSample(0, 0) == originalSample.getSample(0, 0)
            && std::abs(savedSample.getSample(0, 0) - originalSample.getSample(0, 0) * 0.5f) < 0.00004f,
            "retained decoder reads original audio while the same pathname exposes rendered saved audio");
        std::atomic<bool> correct { true };
        const auto inspect = [&] (int seed)
        {
            auto stream = media::SourceFileLease::createRetainedInputStream(retained);
            if (!stream || stream->getTotalLength() != static_cast<int64_t>(originalBytes.getSize())) { correct = false; return; }
            std::array<char, 79> block;
            for (int index = 0; index < 100; ++index)
            {
                const auto offset = (index * 97 + seed * 113) % (originalBytes.getSize() - block.size());
                if (!stream->setPosition(static_cast<int64_t>(offset)) || stream->read(block.data(), static_cast<int>(block.size())) != static_cast<int>(block.size())
                    || std::memcmp(block.data(), static_cast<const char*>(originalBytes.getData()) + offset, block.size()) != 0)
                    correct = false;
            }
        };
        std::thread first(inspect, 1), second(inspect, 2);
        first.join(); second.join();
        require(correct.load(), "concurrent independent decoder streams never share a logical cursor or observe saved-path bytes");
        require(document.getState()->sourceFileWitness == source && !document.hasUnsavedChanges(),
            "retained input admission preserves live state identity and saved generation");
    }
    require(lifetime.expired() && savedProof, "saved receipt alone retains no original input handle after Document/history/readers die");
    savedProof.reset();
    {
        SingleTrackDocument document;
        require(file.copyFileTo(alternate) && document.loadFile(alternate), "open independent playback backing for target-version fault");
        const auto saveAs = document.createSaveSnapshot(file, SingleTrackDocument::SaveIntent::saveAs);
        const auto published = SingleTrackDocument::writeSaveSnapshot(saveAs);
        require(published.succeeded && document.markSaveSnapshotCommitted(saveAs, published)
            && document.moveEnvelopePoint(0, 0, 0.7f), "continue editing after real Save As leaves the target without a playback reader");
        const auto request = document.createSaveSnapshot();
        require(request.sourceVersion != nullptr, "actual Save As publication supplies the current target's physical version");
        const auto& facts = *request.sourceVersion;
        HANDLE changed = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(changed != INVALID_HANDLE_VALUE, "open owned current source for a same-version-metadata fault");
        LARGE_INTEGER offset; offset.QuadPart = facts.sizeBytes - 1;
        unsigned char byte; DWORD count = 0;
        require(SetFilePointerEx(changed, offset, nullptr, FILE_BEGIN) && ReadFile(changed, &byte, 1, &count, nullptr) && count == 1,
            "read owned current source byte before external mutation");
        byte ^= 1;
        require(SetFilePointerEx(changed, offset, nullptr, FILE_BEGIN) && WriteFile(changed, &byte, 1, &count, nullptr) && count == 1,
            "mutate source under same physical identity and length");
        FILETIME stamp { static_cast<DWORD>(facts.modifiedTicks), static_cast<DWORD>(static_cast<uint64_t>(facts.modifiedTicks) >> 32) };
        require(FlushFileBuffers(changed) && SetFileTime(changed, nullptr, nullptr, &stamp) && CloseHandle(changed),
            "restore exact source LastWriteTime without hiding its changed ChangeTime");
        require(request.sourceIdentity.matchesCurrentFile(), "fault passes the previous path/size/millisecond save check");
        juce::MemoryBlock beforeAttempt, afterAttempt;
        require(file.loadFileAsData(beforeAttempt), "capture externally changed source before rejected save");
        const auto rejected = SingleTrackDocument::writeSaveSnapshot(request);
        require(!rejected.succeeded && rejected.failureReason == SingleTrackDocument::SaveFailureReason::staleSource
            && !rejected.publication && file.loadFileAsData(afterAttempt) && beforeAttempt == afterAttempt
            && document.hasUnsavedChanges(), "strong physical save fence refuses restamped source change and preserves its actual bytes");
    }
    require(file.deleteFile() && alternate.deleteFile() && root.deleteFile(), "release retained-input and failed-save resources and remove only owned fixtures");
    std::cout << "RetainedSaveInputTests passed (independent original audio, parallel cursors, ownership/lifetime, strong source fence)\n";
#endif
}

void runAsyncSaveIdentityFenceTest()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("SingleEditorAsyncIdentity-" + juce::Uuid().toString(), {}, false);
    require(root.createDirectory().wasOk(), "create private async identity directory");
    const auto a = root.getChildFile("a.wav"), b = root.getChildFile("b.wav"), output = root.getChildFile("saved-a.wav");
    require(writeTestWavFile(a) && a.copyFileTo(b), "create async identity sources");
    {
        SingleTrackDocument document;
        require(document.loadFile(a), "load document A for async save");
        const auto snapshot = document.createSaveSnapshot(output, SingleTrackDocument::SaveIntent::saveAs);
        juce::WaitableEvent started, releaseWriter, idle;
        bool completed = false;
        DocumentSaveCoordinator coordinator(document, [&] (const auto& request)
        {
            started.signal();
            require(releaseWriter.wait(5000), "release blocked fixture writer");
            return SingleTrackDocument::writeSaveSnapshot(request);
        });
        require(coordinator.requestSave(snapshot, [&] (const auto& result) { completed = result.succeeded; }),
                "dispatch real async save snapshot");
        require(started.wait(2000), "async writer is in flight");
        require(document.loadFile(b) && document.applySelectionGain({ 0.0, 0.05 }, 0.5f),
                "open and edit document B while A saves");
        const auto bIdentity = document.getMediaIdentity();
        coordinator.whenIdle([&] (bool succeeded) { require(succeeded, "requested A output succeeds"); idle.signal(); });
        releaseWriter.signal();
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;
        while (!idle.wait(0) && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        require(completed && output.existsAsFile() && document.getFileInfo().file == b
                    && document.hasUnsavedChanges() && document.getMediaIdentity().document.sameVersion(bIdentity.document),
                "real async completion saves A without rebinding or cleaning edited B");
    }
    for (const auto& file : { a, b, output }) require(file.deleteFile(), "remove owned async fixture");
    require(root.deleteFile(), "remove empty async fixture directory");
    std::cout << "AsyncSaveIdentityFenceTests passed\n";
}

void runStatePreservingSaveTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates save test directory");

    const auto sourceFile = testRoot.getChildFile("source.wav");
    require(writeTestWavFile(sourceFile), "writes initial WAV");

    SingleTrackDocument document;
    CountingListener listener;
    document.addListener(&listener);

    require(document.loadFile(sourceFile), "loads source WAV");
    require(document.deleteSelection({ 0.0, 0.02 }), "deletes first 20 ms");
    require(document.hasUnsavedChanges(), "edit marks document dirty");

    const auto stateBeforeSave = document.getState();
    const auto revisionBeforeSave = stateBeforeSave->audioRevision;
    const auto listenerCountBeforeSave = listener.count;

    require(document.saveOverSourceFile(), "save over source succeeds");
    require(document.getState() == stateBeforeSave, "save preserves live state identity");
    require(document.getState()->audioRevision == revisionBeforeSave, "save preserves audio revision");
    require(listener.count == listenerCountBeforeSave, "save does not publish documentChanged");
    require(document.canUndo(), "save preserves undo history");
    require(!document.hasUnsavedChanges(), "saved generation is clean");

    require(document.undo(), "undo remains available after save");
    require(document.hasUnsavedChanges(), "undo before checkpoint becomes dirty");
    require(document.redo(), "redo remains available after save");
    require(!document.hasUnsavedChanges(), "redo to checkpoint becomes clean");

    document.removeListener(&listener);
    testRoot.deleteRecursively();
}

void runLatestWinsSaveTest()
{
    SingleTrackDocument document;
    auto state = std::make_shared<SingleTrackDocument::State>();
    state->info.file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("single-editor-coordinator-" + juce::Uuid().toString() + ".wav");
    require(state->info.file.create().wasOk(), "creates coordinator target file");
    state->info.sampleRate = 48000.0;
    state->info.numChannels = 1;
    state->info.numSamples = 1;
    state->info.durationSeconds = 1.0 / 48000.0;
    state->audioBuffer = std::make_shared<juce::AudioBuffer<float>>(1, 1);
    state->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::inMemoryBuffer;
    state->changeGeneration = 1;

    juce::WaitableEvent firstWriterStarted;
    juce::WaitableEvent releaseFirstWriter;
    juce::WaitableEvent coordinatorIdle(true);
    juce::CriticalSection writesLock;
    std::vector<int64_t> writtenGenerations;

    DocumentSaveCoordinator coordinator(document, [&](const auto& snapshot)
    {
        {
            const juce::ScopedLock sl(writesLock);
            writtenGenerations.push_back(snapshot.changeGeneration);
        }

        if (snapshot.changeGeneration == 1)
        {
            firstWriterStarted.signal();
            releaseFirstWriter.wait(5000);
        }

        return SingleTrackDocument::SaveWriteResult { true,
                                                       snapshot.targetFile,
                                                       snapshot.changeGeneration,
                                                       {} };
    });

    auto first = SingleTrackDocument::SaveSnapshot { state, state->info.file, 1 };
    auto second = SingleTrackDocument::SaveSnapshot { state, state->info.file, 2 };
    auto latest = SingleTrackDocument::SaveSnapshot { state, state->info.file, 3 };

    require(coordinator.requestSave(first), "starts first asynchronous save");
    require(firstWriterStarted.wait(2000), "first save reaches writer");
    require(coordinator.requestSave(second), "queues second save");
    require(coordinator.requestSave(latest), "replaces pending save with latest generation");

    coordinator.whenIdle([&](bool succeeded)
    {
        require(succeeded, "latest asynchronous save succeeds");
        coordinatorIdle.signal();
    });

    releaseFirstWriter.signal();
    const auto timeoutAt = juce::Time::getMillisecondCounter() + 5000;
    while (!coordinatorIdle.wait(0) && juce::Time::getMillisecondCounter() < timeoutAt)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);

    require(coordinatorIdle.wait(0), "coordinator becomes idle");

    const juce::ScopedLock sl(writesLock);
    require(writtenGenerations.size() == 2, "coordinator writes active and latest snapshots only");
    require(writtenGenerations[0] == 1, "coordinator writes active generation first");
    require(writtenGenerations[1] == 3, "coordinator drops superseded pending generation");
    state->info.file.deleteFile();
}

void runPlaybackContinuityAfterSaveTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates playback continuity test directory");

    const auto sourceFile = testRoot.getChildFile("playback-source.wav");
    require(writeStereoPatternWavFile(sourceFile), "writes patterned stereo WAV");

    SingleTrackDocument document;
    AudioEngine audioEngine(document);
    require(document.loadFile(sourceFile), "loads patterned stereo WAV");
    require(document.deleteSelection({ 0.0, 0.01 }), "materialises edit state by deleting the head");
    require(document.saveOverSourceFile(), "saves materialised edit over source");

    const auto state = document.getState();
    require(state != nullptr && state->audioBuffer != nullptr, "edited state remains buffer-backed");

    constexpr int64_t renderStart = 1400;
    constexpr int renderSamples = 256;
    juce::AudioBuffer<float> rendered(2, renderSamples);
    require(audioEngine.renderDocumentBlockForDiagnostics(renderStart, rendered),
            "renders the actual document playback source without an audio device");

    for (int channel = 0; channel < rendered.getNumChannels(); ++channel)
    {
        for (int sample = 0; sample < rendered.getNumSamples(); ++sample)
        {
            const auto expected = state->audioBuffer->getSample(channel,
                                                                 static_cast<int>(renderStart) + sample);
            const auto actual = rendered.getSample(channel, sample);
            require(std::abs(actual - expected) < 1.0e-6f,
                    "post-save playback reads the requested in-memory sample range");
        }
    }

    testRoot.deleteRecursively();
}

void runSaveAsAndExportSemanticsTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-save-as", {}, false);
    require(testRoot.createDirectory(), "creates Save As test directory");
    const auto sourceFile = testRoot.getChildFile("source.wav");
    const auto saveAsFile = testRoot.getChildFile("renamed.wav");
    const auto exportFile = testRoot.getChildFile("derivative.flac");
    require(writeStereoPatternWavFile(sourceFile), "writes Save As source fixture");

    SingleTrackDocument document;
    CountingListener listener;
    document.addListener(&listener);
    require(document.loadFile(sourceFile), "loads Save As source fixture");
    require(document.deleteSelection({ 0.0, 0.01 }), "edits before Save As");
    const auto stateBeforeSaveAs = document.getState();
    const auto listenerCountBeforeSaveAs = listener.count;

    const auto saveAsSnapshot = document.createSaveSnapshot(
        saveAsFile, SingleTrackDocument::SaveIntent::saveAs);
    const auto saveAsResult = SingleTrackDocument::writeSaveSnapshot(saveAsSnapshot);
    require(saveAsResult.succeeded, "writes Save As snapshot");
    document.markSaveSnapshotCommitted(saveAsSnapshot, saveAsResult);
    require(document.getFileInfo().file == saveAsFile,
            "Save As adopts the new current document path");
    require(document.getState() == stateBeforeSaveAs
                && listener.count == listenerCountBeforeSaveAs,
            "Save As preserves state identity and does not publish documentChanged");
    require(!document.hasUnsavedChanges(), "Save As commits the saved generation");

    require(document.deleteSelection({ 0.01, 0.02 }), "edits again before Export");
    const auto exportSnapshot = document.createSaveSnapshot(
        exportFile, SingleTrackDocument::SaveIntent::exportCopy);
    const auto exportResult = SingleTrackDocument::writeSaveSnapshot(exportSnapshot);
    require(exportResult.succeeded, "writes Export snapshot");
    document.markSaveSnapshotCommitted(exportSnapshot, exportResult);
    require(document.getFileInfo().file == saveAsFile,
            "Export does not change the current document path");
    require(document.hasUnsavedChanges(),
            "Export does not mark the current document checkpoint clean");
    require(exportFile.existsAsFile(), "Export creates the requested derivative");

    document.removeListener(&listener);
    testRoot.deleteRecursively();
}

void runOrdinaryAudioDoesNotLoadMediaFoundationTest()
{
    require(!WindowsMediaAudioService::isRuntimeLoadedForTests(),
            "Media Foundation starts unloaded in an ordinary-audio process");

    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-lazy-media", {}, false);
    require(testRoot.createDirectory(), "creates lazy media test directory");
    const auto sourceFile = testRoot.getChildFile("ordinary.wav");
    require(writeTestWavFile(sourceFile), "writes ordinary WAV lazy-media fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "opens ordinary WAV without media backend");
    require(!WindowsMediaAudioService::isRuntimeLoadedForTests(),
            "ordinary WAV open does not load Media Foundation");
    testRoot.deleteRecursively();
}

void runVideoOpenFailureProvidesDiagnosticTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-video-diagnostic", {}, false);
    require(testRoot.createDirectory(), "creates video diagnostic test directory");
    const auto sourceFile = testRoot.getChildFile("invalid.mp4");
    require(sourceFile.replaceWithText("not a media container"), "writes invalid video fixture");

    const auto prepared = SingleTrackDocument::prepareAudioFileForOpen(sourceFile);
    require(!prepared.hasReader(), "invalid video remains unopened");
    require(prepared.errorMessage.isNotEmpty(), "video open failure preserves a backend diagnostic");
    require(!WindowsMediaAudioService::isRuntimeLoadedForTests(), "hostile video never loads Media Foundation in this process");

    testRoot.deleteRecursively();
}

void runMp3SaveAsTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-mp3-save", {}, false);
    require(testRoot.createDirectory(), "creates MP3 Save As test directory");
    const auto sourceFile = testRoot.getChildFile("source.wav");
    const auto mp3File = testRoot.getChildFile("encoded.mp3");
    require(writeStereoPatternWavFile(sourceFile), "writes MP3 source fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads MP3 source fixture");
    require(document.canSaveToFile(mp3File), "declares MP3 as a writable target");
    SingleTrackDocument::SaveEncodingOptions encodingOptions;
    encodingOptions.mp3BitrateKbps = 256;
    const auto snapshot = document.createSaveSnapshot(mp3File,
                                                      SingleTrackDocument::SaveIntent::saveAs,
                                                      encodingOptions);
    const auto result = SingleTrackDocument::writeSaveSnapshot(snapshot);
    if (!result.succeeded)
        std::cerr << "MP3 save error: " << result.errorMessage << "\n";
    require(result.succeeded, "writes MP3 Save As target");
    require(result.encodingDescription == "MP3 256 kbps",
            "reports the immutable MP3 encoding settings");
    document.markSaveSnapshotCommitted(snapshot, result);
    require(mp3File.existsAsFile() && mp3File.getSize() > 0, "creates non-empty MP3 output");

    auto reader = SingleTrackDocument::createReaderForFile(mp3File);
    require(reader != nullptr, "reopens encoded MP3 through the normal reader");
    require(static_cast<int>(reader->numChannels) == 2, "preserves MP3 channel count");
    require(std::abs(reader->sampleRate - 48000.0) < 0.001, "preserves MP3 sample rate");
    require(std::abs(reader->lengthInSamples - 6000) <= 4096,
            "keeps MP3 duration within codec delay tolerance");

    juce::AudioBuffer<float> decoded(2, 1024);
    require(reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true),
            "decodes the encoded MP3 payload");
    require(decoded.getMagnitude(0, 0, decoded.getNumSamples()) > 0.01f
                || decoded.getMagnitude(1, 0, decoded.getNumSamples()) > 0.01f,
            "encoded MP3 contains non-silent audio");

    testRoot.deleteRecursively();
}

void runTimelineMetadataSavePolicyTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-metadata-save", {}, false);
    require(testRoot.createDirectory(), "creates metadata save policy test directory");
    const auto sourceFile = testRoot.getChildFile("timeline.wav");
    const auto exportFile = testRoot.getChildFile("timeline-export.flac");
    require(writeBwfTestWavFile(sourceFile), "writes BWF metadata fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads BWF metadata fixture");
    require(document.getFileInfo().metadata.containsKey(juce::WavAudioFormat::bwavTimeReference),
            "fixture exposes the BWF time reference");
    require(document.deleteSelection({ 0.0, 0.01 }), "edits BWF audio before save audit");

    const auto checkpoint = document.createSaveSnapshot();
    require(checkpoint.metadataReport.rejectedKeys.isEmpty(),
            "checkpoint accepts timeline metadata after a safe sample-offset transform");
    require(!checkpoint.metadataReport.transformedKeys.isEmpty(),
            "checkpoint reports transformed timeline metadata explicitly");
    const auto checkpointResult = SingleTrackDocument::writeSaveSnapshot(checkpoint);
    require(checkpointResult.succeeded,
            "checkpoint writes transformed timeline metadata over the source");
    document.markSaveSnapshotCommitted(checkpoint, checkpointResult);

    std::unique_ptr<juce::AudioFormatReader> reopened(
        juce::WavAudioFormat().createReaderFor(sourceFile.createInputStream().release(), true));
    require(reopened != nullptr, "reopens transformed BWF fixture");
    const auto& transformed = reopened->metadataValues;
    require(transformed[juce::WavAudioFormat::bwavTimeReference] == "96480",
            "head deletion advances BWF time reference by deleted samples");
    require(transformed["NumCuePoints"] == "2"
                && transformed["Cue0Identifier"] == "102"
                && transformed["Cue0Offset"] == "480"
                && transformed["Cue1Identifier"] == "103"
                && transformed["Cue1Offset"] == "3520",
            "cue points inside the deletion are removed and later points shift forward");
    require(transformed["NumCueLabels"] == "1"
                && transformed["CueLabel0Identifier"] == "102",
            "labels for removed cue points are removed without orphaning LIST entries");
    require(transformed["NumSampleLoops"] == "1"
                && transformed["Loop0Identifier"] == "202"
                && transformed["Loop0Start"] == "520"
                && transformed["Loop0End"] == "1520",
            "sampler loops are removed or shifted with the edited sample timeline");

    const auto exportSnapshot = document.createSaveSnapshot(
        exportFile,
        SingleTrackDocument::SaveIntent::exportCopy);
    require(exportSnapshot.metadataReport.rejectedKeys.isEmpty()
                && !exportSnapshot.metadataReport.omittedKeys.isEmpty(),
            "export reports unsupported timeline metadata as omitted rather than hidden loss");
    const auto exportResult = SingleTrackDocument::writeSaveSnapshot(exportSnapshot);
    require(exportResult.succeeded
                && exportResult.metadataDescription.containsIgnoreCase("omitted"),
            "export succeeds with an explicit metadata omission report");

    testRoot.deleteRecursively();
}

void runAmplitudeEditPreservesBwfTimelineTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-bwf-gain", {}, false);
    require(testRoot.createDirectory(), "creates BWF gain test directory");
    const auto sourceFile = testRoot.getChildFile("gain.wav");
    require(writeBwfTestWavFile(sourceFile), "writes BWF gain fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads BWF gain fixture");
    require(document.applySelectionGain({ 0.02, 0.04 }, 0.5f),
            "applies amplitude-only edit");
    const auto snapshot = document.createSaveSnapshot();
    require(snapshot.metadataReport.rejectedKeys.isEmpty(),
            "amplitude-only edit does not stale timeline metadata");
    require(snapshot.metadataReport.transformedKeys.isEmpty(),
            "amplitude-only edit preserves rather than transforms timeline metadata");
    require(SingleTrackDocument::writeSaveSnapshot(snapshot).succeeded,
            "amplitude-only checkpoint preserves BWF metadata");

    std::unique_ptr<juce::AudioFormatReader> reopened(
        juce::WavAudioFormat().createReaderFor(sourceFile.createInputStream().release(), true));
    require(reopened != nullptr
                && reopened->metadataValues[juce::WavAudioFormat::bwavTimeReference] == "96000",
            "amplitude-only checkpoint leaves BWF time reference unchanged");

    testRoot.deleteRecursively();
}

void runComplexCueMetadataRejectsTimelineEditTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-complex-cue", {}, false);
    require(testRoot.createDirectory(), "creates complex cue test directory");
    const auto sourceFile = testRoot.getChildFile("complex-cue.wav");
    require(writeBwfTestWavFile(sourceFile, true), "writes non-zero cue block-offset fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads non-zero cue block-offset fixture");
    require(document.deleteSelection({ 0.0, 0.01 }), "edits complex cue fixture");
    const auto snapshot = document.createSaveSnapshot();
    require(!snapshot.metadataReport.rejectedKeys.isEmpty(),
            "checkpoint rejects cue offsets whose block-relative semantics cannot be transformed safely");
    const auto result = SingleTrackDocument::writeSaveSnapshot(snapshot);
    require(!result.succeeded
                && result.failureReason == SingleTrackDocument::SaveFailureReason::metadataRejected,
            "complex cue rejection leaves the source untouched");

    testRoot.deleteRecursively();
}

void runExternalSourceIdentityConflictTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-stale-save", {}, false);
    require(testRoot.createDirectory(), "creates stale source test directory");
    const auto sourceFile = testRoot.getChildFile("external-change.flac");
    require(writeTestFlacFile(sourceFile), "writes stale source fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads stale source fixture");
    require(document.applySelectionGain({ 0.02, 0.04 }, 0.5f),
            "edits stale source fixture");
    const auto staleSnapshot = document.createSaveSnapshot();
    const auto externallyChangedTime = sourceFile.getLastModificationTime() + juce::RelativeTime::seconds(5.0);
    require(sourceFile.setLastModificationTime(externallyChangedTime),
            "simulates an external source modification");

    const auto staleResult = SingleTrackDocument::writeSaveSnapshot(staleSnapshot);
    require(!staleResult.succeeded
                && staleResult.failureReason == SingleTrackDocument::SaveFailureReason::staleSource,
            "checkpoint rejects a source whose lightweight identity changed after load");
    require(sourceFile.getLastModificationTime() == externallyChangedTime,
            "stale checkpoint leaves the externally changed source untouched");

    SingleTrackDocument reloaded;
    require(reloaded.loadFile(sourceFile), "reloads the externally changed source");
    require(reloaded.deleteSelection({ 0.0, 0.01 }), "edits reloaded source");
    const auto firstSnapshot = reloaded.createSaveSnapshot();
    const auto firstResult = SingleTrackDocument::writeSaveSnapshot(firstSnapshot);
    require(firstResult.succeeded,
            "fresh source identity permits checkpoint");
    reloaded.markSaveSnapshotCommitted(firstSnapshot, firstResult);
    require(reloaded.applySelectionGain({ 0.01, 0.02 }, 0.5f),
            "edits again after checkpoint");
    require(SingleTrackDocument::writeSaveSnapshot(reloaded.createSaveSnapshot()).succeeded,
            "successful checkpoint refreshes the source identity baseline");

    testRoot.deleteRecursively();
}

void runNativeWriteFormatRoundTripTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-native-formats", {}, false);
    require(testRoot.createDirectory(), "creates native format test directory");
    const auto sourceFile = testRoot.getChildFile("source.wav");
    require(writeStereoPatternWavFile(sourceFile), "writes native format source fixture");

    for (const auto& extension : juce::StringArray { ".wav", ".aiff", ".flac", ".ogg" })
    {
        SingleTrackDocument document;
        require(document.loadFile(sourceFile), "loads native format source fixture");
        const auto targetFile = testRoot.getChildFile("roundtrip" + extension);
        require(document.canSaveToFile(targetFile), "declares native target format writable");
        require(document.saveToFile(targetFile), "writes native target format");

        auto reader = SingleTrackDocument::createReaderForFile(targetFile);
        require(reader != nullptr, "reopens native target format");
        require(static_cast<int>(reader->numChannels) == 2, "preserves native format channel count");
        require(std::abs(reader->sampleRate - 48000.0) < 0.001,
                "preserves native format sample rate");
        const auto tolerance = extension == ".ogg" ? 4800 : 0;
        require(std::abs(reader->lengthInSamples - 6000) <= tolerance,
                "preserves native format duration within codec tolerance");
        if (extension == ".flac")
        {
            SingleTrackDocument reopenedDocument;
            require(reopenedDocument.loadFile(targetFile), "loads ordinary FLAC for metadata audit");
            require(reopenedDocument.createSaveSnapshot().metadataReport.canProceed(),
                    "does not mistake structural FLAC blocks for lossy user metadata");
        }
    }

    testRoot.deleteRecursively();
}

void runMp3RejectsImplicitMultichannelDownmixTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("single-editor-mp3-multichannel", {}, false);
    require(testRoot.createDirectory(), "creates multichannel MP3 test directory");
    const auto sourceFile = testRoot.getChildFile("foa.wav");
    const auto targetFile = testRoot.getChildFile("foa.mp3");
    require(writeFoaFrontWavFile(sourceFile), "writes four-channel MP3 source fixture");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads four-channel MP3 source fixture");
    require(!document.canSaveToFile(targetFile), "does not advertise multichannel MP3 as writable");
    const auto result = SingleTrackDocument::writeSaveSnapshot(
        document.createSaveSnapshot(targetFile, SingleTrackDocument::SaveIntent::exportCopy));
    require(!result.succeeded && result.errorMessage.containsIgnoreCase("no implicit downmix"),
            "rejects multichannel MP3 instead of silently downmixing");
    require(!targetFile.existsAsFile(), "multichannel MP3 rejection leaves no output");
    testRoot.deleteRecursively();
}

void runPlaybackAvailabilityStateTest()
{
    SingleTrackDocument::State emptyState;
    require(emptyState.getPlayableSampleRange().isEmpty(),
            "empty state exposes no playable samples");
    require(!emptyState.canPlaySample(0),
            "empty state rejects playback");

    SingleTrackDocument::FileInfo previewInfo;
    previewInfo.file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("single-editor-preview-" + juce::Uuid().toString() + ".wav");
    require(previewInfo.file.create().wasOk(), "creates preview identity file");
    previewInfo.sampleRate = 48000.0;
    previewInfo.numChannels = 2;
    previewInfo.numSamples = 48000;
    previewInfo.durationSeconds = 1.0;
    const auto previewState = SingleTrackDocument::createPreviewState(previewInfo);
    require(previewState != nullptr && previewState->getPlayableSampleRange().isEmpty(),
            "visual preview does not pretend to be playable");
    previewInfo.file.deleteFile();

    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates availability test directory");
    const auto sourceFile = testRoot.getChildFile("random-access.wav");
    require(writeStereoPatternWavFile(sourceFile), "writes random-access WAV");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads random-access WAV");
    const auto loadedState = document.getState();
    require(loadedState->playbackAvailability.getAccess()
                == SingleTrackDocument::PlaybackAvailability::Access::randomAccess,
            "ordinary reader is explicitly random-access playable");
    require(loadedState->getPlayableSampleRange()
                == juce::Range<juce::int64>(0, loadedState->info.numSamples),
            "ordinary reader exposes its complete duration on demand");
    require(loadedState->canPlaySample(loadedState->info.numSamples - 1),
            "ordinary reader permits seeking to the final sample");
    require(!loadedState->canPlaySample(loadedState->info.numSamples),
            "sample immediately after the file is not playable");

    testRoot.deleteRecursively();
}

void runContiguousPlaybackBoundaryTest()
{
    constexpr int totalSamples = 512;
    constexpr int readySamples = 128;

    auto state = std::make_shared<SingleTrackDocument::State>();
    state->info.file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("single-editor-prefix-" + juce::Uuid().toString() + ".wav");
    require(state->info.file.create().wasOk(), "creates prefix state identity file");
    state->info.sampleRate = 48000.0;
    state->info.numChannels = 2;
    state->info.numSamples = totalSamples;
    state->info.durationSeconds = static_cast<double>(totalSamples) / state->info.sampleRate;
    state->info.channelLayout = ChannelLayoutCatalog::defaultLayoutForChannelCount(2);
    state->audioBuffer = std::make_shared<juce::AudioBuffer<float>>(2, totalSamples);
    state->audioBuffer->clear();
    for (int channel = 0; channel < state->audioBuffer->getNumChannels(); ++channel)
        for (int sample = 0; sample < totalSamples; ++sample)
            state->audioBuffer->setSample(channel, sample, 0.5f);
    state->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::inMemoryBuffer;
    state->playbackAvailability =
        SingleTrackDocument::PlaybackAvailability::contiguousPrefix(readySamples);

    auto sharedAvailability = state->playbackAvailability;
    sharedAvailability.advanceContiguousPrefix(readySamples + 32);
    require(state->getPlayableSampleRange().getEnd() == readySamples + 32,
            "shared provider publishes a larger ready prefix to the document state");
    sharedAvailability.advanceContiguousPrefix(readySamples);
    require(state->getPlayableSampleRange().getEnd() == readySamples + 32,
            "ready prefix never moves backwards");
    state->playbackAvailability =
        SingleTrackDocument::PlaybackAvailability::contiguousPrefix(readySamples);

    SingleTrackDocument document;
    AudioEngine audioEngine(document);
    require(document.adoptLoadedState(state), "adopts synthetic prefix-ready state");
    require(audioEngine.getPlayableSampleRange()
                == juce::Range<juce::int64>(0, readySamples),
            "audio engine reports the document ready prefix");

    juce::AudioBuffer<float> crossingBoundary(2, 128);
    require(audioEngine.renderDocumentBlockForDiagnostics(64, crossingBoundary),
            "renders a block crossing the ready boundary");
    for (int channel = 0; channel < crossingBoundary.getNumChannels(); ++channel)
    {
        require(crossingBoundary.getMagnitude(channel, 0, 64) > 0.1f,
                "ready samples render normally");
        require(crossingBoundary.getMagnitude(channel, 64, 64) == 0.0f,
                "unready samples remain silent");
    }

    audioEngine.seekToSeconds(256.0 / state->info.sampleRate);
    require(audioEngine.getCurrentPositionSeconds() == 0.0,
            "seek outside the ready prefix is rejected");

    auto liveAvailability = state->playbackAvailability;
    liveAvailability.markRandomAccess();
    require(audioEngine.getPlayableSampleRange()
                == juce::Range<juce::int64>(0, totalSamples),
            "completed provider upgrades the same live document state to random access");

    liveAvailability.markUnavailable();
    require(audioEngine.getPlayableSampleRange().isEmpty(),
            "failed provider immediately withdraws the playable range");

    state->info.file.deleteFile();
}

void runGrowingFilePlaybackTest()
{
    constexpr int totalSamples = 512;
    constexpr int initialSamples = 128;
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates growing WAV test directory");
    const auto sourceFile = testRoot.getChildFile("growing.wav");
    require(createGrowingStereoWav(sourceFile, totalSamples, initialSamples),
            "writes a WAV header and first decoded prefix");

    {
        juce::WavAudioFormat format;
        auto stream = std::make_unique<juce::FileInputStream>(sourceFile);
        require(stream->openedOk(), "opens growing WAV input stream");
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            format.createReaderFor(stream.release(), true));
        require(reader != nullptr && reader->lengthInSamples == totalSamples,
                "growing WAV header exposes final duration before decode completes");

        auto state = SingleTrackDocument::createStateFromOwnedReader(sourceFile, std::move(reader));
        require(state != nullptr, "creates streaming state for growing WAV");
        state->playbackAvailability =
            SingleTrackDocument::PlaybackAvailability::contiguousPrefix(initialSamples);
        auto availability = state->playbackAvailability;

        SingleTrackDocument document;
        AudioEngine audioEngine(document);
        require(document.adoptLoadedState(state), "adopts growing WAV state");

        juce::AudioBuffer<float> rendered(2, 64);
        require(audioEngine.renderDocumentBlockForDiagnostics(64, rendered),
                "renders first on-disk prefix");
        require(rendered.getMagnitude(0, 0, 64) > 0.2f,
                "first decoded prefix contains expected audio");

        require(appendGrowingStereoSamples(sourceFile, 128),
                "appends the next decoded PCM prefix while reader stays open");
        availability.advanceContiguousPrefix(256);
        rendered.clear();
        require(audioEngine.renderDocumentBlockForDiagnostics(128, rendered),
                "renders newly published growing-file prefix");
        require(rendered.getMagnitude(0, 0, 64) > 0.4f,
                "open reader observes PCM appended after initial playback became ready");
    }

    testRoot.deleteRecursively();
}

void runWaveChannelMaskPreservedAfterSaveTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates channel mask save test directory");

    constexpr juce::uint32 smpteSevenOneMask = 0x63fu;
    const auto sourceFile = testRoot.getChildFile("smpte-7.1.wav");
    require(writeMaskedWavFile(sourceFile, 8, smpteSevenOneMask), "writes 7.1 SMPTE WAV fixture");
    require(readWaveChannelMask(sourceFile) == smpteSevenOneMask, "fixture starts with 7.1 SMPTE channel mask");

    SingleTrackDocument document;
    require(document.loadFile(sourceFile), "loads 7.1 SMPTE WAV");
    require(document.deleteSelection({ 0.0, 0.01 }), "materialises edit before channel mask save");
    require(document.saveOverSourceFile(), "saves edited 7.1 WAV over source");

    require(readWaveChannelMask(sourceFile) == smpteSevenOneMask,
            "destructive save preserves WAV channel mask");

    testRoot.deleteRecursively();
}

void runHeadlessMultichannelRoutingTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates headless routing test directory");

    const auto sourceFile = testRoot.getChildFile("wave-7.1-routing.wav");
    require(writeMaskedRoutingImpulseWavFile(sourceFile),
            "writes WAVE 0x63F routing impulse fixture");

    SingleTrackDocument document;
    AudioEngine audioEngine(document);
    require(document.loadFile(sourceFile), "loads WAVE 0x63F routing fixture");

    juce::AudioBuffer<float> rendered(8, 16);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, rendered),
            "renders routed multichannel playback without an audio device");

    constexpr float tolerance = 0.0001f;
    require(std::abs(rendered.getSample(4, 4) - 0.5f) < tolerance,
            "WAVE source ch5 rear-left reaches Windows device output 5");
    require(std::abs(rendered.getSample(6, 6) - 0.7f) < tolerance,
            "WAVE source ch7 side-left reaches Windows device output 7");
    require(std::abs(rendered.getSample(6, 4)) < tolerance
                && std::abs(rendered.getSample(4, 6)) < tolerance,
            "side and rear impulses remain isolated in their Windows output slots");

    ChannelOutputMapping outputMapping;
    require(outputMapping.setOutputChannel(SpeakerRole::Lsr, 6)
                && outputMapping.setOutputChannel(SpeakerRole::Rsr, 7)
                && outputMapping.setOutputChannel(SpeakerRole::Lss, 4)
                && outputMapping.setOutputChannel(SpeakerRole::Rss, 5),
            "creates physical side-rear output patch");
    audioEngine.setOutputMapping(outputMapping);
    rendered.clear();
    require(audioEngine.renderDocumentBlockForDiagnostics(0, rendered),
            "renders patched multichannel playback without an audio device");
    require(std::abs(rendered.getSample(6, 4) - 0.5f) < tolerance,
            "explicit output patch moves rear-left to physical output 7");
    require(std::abs(rendered.getSample(4, 6) - 0.7f) < tolerance,
            "explicit output patch moves side-left to physical output 5");

    testRoot.deleteRecursively();
}

void runHeadlessSevenOneFourRoutingTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates 7.1.4 headless routing test directory");

    const auto sourceFile = testRoot.getChildFile("wave-7.1.4-routing.wav");
    require(writeMaskedHeightRoutingImpulseWavFile(sourceFile),
            "writes WAVE 7.1.4 routing impulse fixture");

    SingleTrackDocument document;
    AudioEngine audioEngine(document);
    require(document.loadFile(sourceFile), "loads WAVE 7.1.4 routing fixture");

    constexpr float tolerance = 0.0001f;
    const auto requireSample = [&](const juce::AudioBuffer<float>& rendered,
                                   int outputChannel,
                                   int sourceImpulseSample,
                                   float expected,
                                   const char* message)
    {
        require(std::abs(rendered.getSample(outputChannel, sourceImpulseSample) - expected) < tolerance,
                message);
    };

    juce::AudioBuffer<float> twelveOutputs(12, 16);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, twelveOutputs),
            "renders 7.1.4 to twelve outputs without an audio device");
    constexpr int expectedDeviceOutputByWaveSource[12] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
    };
    for (int sourceChannel = 0; sourceChannel < 12; ++sourceChannel)
    {
        const auto expectedAmplitude = 0.05f * static_cast<float>(sourceChannel + 1);
        for (int outputChannel = 0; outputChannel < 12; ++outputChannel)
        {
            const auto expected = outputChannel == expectedDeviceOutputByWaveSource[sourceChannel]
                                ? expectedAmplitude
                                : 0.0f;
            requireSample(twelveOutputs,
                          outputChannel,
                          sourceChannel,
                          expected,
                          "all WAVE 7.1.4 impulses route to exactly one semantic device output");
        }
    }
    requireSample(twelveOutputs, 4, 4, 0.25f,
                  "WAVE rear-left source reaches Windows rear-left output 5");
    requireSample(twelveOutputs, 6, 6, 0.35f,
                  "WAVE side-left source reaches Windows side-left output 7");
    requireSample(twelveOutputs, 8, 8, 0.45f,
                  "top-front-left reaches output 9 at unity");
    requireSample(twelveOutputs, 11, 11, 0.60f,
                  "top-rear-right reaches output 12 at unity");
    requireSample(twelveOutputs, 6, 4, 0.0f,
                  "rear-left does not leak into the side-left output");
    requireSample(twelveOutputs, 4, 6, 0.0f,
                  "side-left does not leak into the rear-left output");

    juce::AudioBuffer<float> sevenOneOutputs(8, 16);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, sevenOneOutputs),
            "renders 7.1.4 Wwise fold-down to 7.1");
    requireSample(sevenOneOutputs, 0, 8, 0.45f * ChannelRouter::minusThreeDbGain(),
                  "top-front-left folds to front-left at -3 dB");
    requireSample(sevenOneOutputs, 1, 9, 0.50f * ChannelRouter::minusThreeDbGain(),
                  "top-front-right folds to front-right at -3 dB");
    requireSample(sevenOneOutputs, 4, 10, 0.55f * ChannelRouter::minusThreeDbGain(),
                  "top-rear-left folds to rear-left at Wwise -3 dB");
    requireSample(sevenOneOutputs, 5, 11, 0.60f * ChannelRouter::minusThreeDbGain(),
                  "top-rear-right folds to rear-right at Wwise -3 dB");

    juce::AudioBuffer<float> fiveOneOutputs(6, 16);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, fiveOneOutputs),
            "renders 7.1.4 Wwise fold-down to 5.1");
    requireSample(fiveOneOutputs, 4, 10, 0.55f * ChannelRouter::minusThreeDbGain(),
                  "top-rear-left folds to 5.1 surround-left at Wwise -3 dB");
    requireSample(fiveOneOutputs, 5, 11, 0.60f * ChannelRouter::minusThreeDbGain(),
                  "top-rear-right folds to 5.1 surround-right at Wwise -3 dB");

    juce::AudioBuffer<float> stereoOutputs(2, 16);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, stereoOutputs),
            "renders 7.1.4 Wwise fold-down to stereo");
    requireSample(stereoOutputs, 0, 8, 0.45f * ChannelRouter::minusThreeDbGain(),
                  "top-front-left folds to stereo left at -3 dB");
    requireSample(stereoOutputs, 0, 10, 0.55f * ChannelRouter::minusSixDbGain(),
                  "top-rear-left folds to stereo left at -6 dB");

    testRoot.deleteRecursively();
}

void runLazyFoaMonitorDecodeTest()
{
    const auto testRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("SingleEditorDocumentTests")
                              .getChildFile(juce::Uuid().toString());
    require(testRoot.createDirectory(), "creates FOA monitor test directory");

    const auto sourceFile = testRoot.getChildFile("foa-front.wav");
    require(writeFoaFrontWavFile(sourceFile), "writes ACN/SN3D FOA fixture");

    SingleTrackDocument document;
    AudioEngine audioEngine(document);
    require(document.loadFile(sourceFile), "loads FOA fixture as ordinary four-channel audio");
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "ordinary four-channel load does not initialise the Ambisonics stage graph");
    require(document.getState()->ambisonicBinauralFilterBank == nullptr
                && document.getAmbisonicMonitorMode() == AmbisonicMonitorMode::speakerProjection,
            "ordinary four-channel load does not touch HRTF resources");
    require(!audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "ordinary four-channel load does not create the Ambisonic field analyzer");

    const auto templateProfile =
        AmbisonicStandardMatrixCatalog::standardProfile("standard-5.1-v1");
    require(templateProfile != nullptr, "loads geometry template only after explicit user action");
    auto customProfile = *templateProfile;
    customProfile.profileId = "document-test-room-v1";
    customProfile.displayName = "Document Test Room";
    customProfile.parentTemplateId = templateProfile->profileId;
    customProfile.builtInReadOnly = false;
    SpeakerGeometryGenerationRequest customRequest;
    customRequest.requestId = 1;
    customRequest.draftRevision = customProfile.revision;
    customRequest.ambisonicOrder = 1;
    customRequest.profile = customProfile;
    auto customResult = AmbisonicSpeakerGeometryGenerator::generate(customRequest);
    require(customResult.succeeded(), "generates a valid custom FOA speaker matrix off the callback");
    const auto customArtifact = customResult.artifact;
    require(audioEngine.activateAmbisonicDecodeMatrixArtifact(customArtifact)
                && audioEngine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == customArtifact
                && !audioEngine.hasAmbisonicStageGraphForDiagnostics()
                && !audioEngine.isCustomAmbisonicDecodeMatrixActiveForDiagnostics(),
            "activating geometry for ordinary Quad stores policy but creates no spatial runtime");

    auto fieldAnalysisLease = audioEngine.acquireAmbisonicFieldAnalysisLease();
    require(fieldAnalysisLease
                && audioEngine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 1
                && !audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "a visible field consumer does not initialise Ambisonics for ordinary Quad");
    require(document.setChannelLayoutPreset("foa-acn-sn3d"), "selects FOA explicitly");
    require(AmbisonicMonitorDecoder::statusFor(document.getState()->info.channelLayout)
                == AmbisonicMonitorDecodeStatus::ready,
            "explicit FOA selection validates without allocating a decoder graph");
    require(audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "an active field-view lease creates the analyzer after explicit FOA selection");
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "explicit FOA interpretation alone does not acquire the playback stage graph");

    juce::AudioBuffer<float> customSpeakerOutputs(6, 256);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, customSpeakerOutputs)
                && audioEngine.hasAmbisonicStageGraphForDiagnostics()
                && audioEngine.isCustomAmbisonicDecodeMatrixActiveForDiagnostics(),
            "playback lease applies the activated custom canonical speaker bus on demand");
    require(customSpeakerOutputs.getMagnitude(3, 0, customSpeakerOutputs.getNumSamples())
                < 0.0001f,
            "custom AudioEngine decode keeps LFE silent");
    auto previewProfile = customProfile;
    previewProfile.revision += 1;
    previewProfile.speakers[0].azimuthDegrees += 12.0f;
    auto previewRequest = customRequest;
    previewRequest.requestId += 1;
    previewRequest.draftRevision = previewProfile.revision;
    previewRequest.profile = previewProfile;
    auto previewResult = AmbisonicSpeakerGeometryGenerator::generate(previewRequest);
    require(previewResult.succeeded()
                && audioEngine.previewAmbisonicDecodeMatrixArtifact(previewResult.artifact)
                && audioEngine.getAmbisonicDecodeMatrixPreviewForDiagnostics()
                       == previewResult.artifact
                && audioEngine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == customArtifact,
            "audition publishes a temporary graph without replacing active geometry");
    audioEngine.clearAmbisonicDecodeMatrixPreview();
    require(audioEngine.getAmbisonicDecodeMatrixPreviewForDiagnostics() == nullptr
                && audioEngine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == customArtifact
                && audioEngine.isCustomAmbisonicDecodeMatrixActiveForDiagnostics(),
            "ending audition restores the last-known-good active custom matrix");
    auto corruptArtifact = std::make_shared<AmbisonicDecodeMatrixArtifact>(*customArtifact);
    corruptArtifact->decodeMatrix.gain(0, 0) += 0.001f;
    require(!audioEngine.activateAmbisonicDecodeMatrixArtifact(corruptArtifact)
                && audioEngine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == customArtifact
                && audioEngine.isCustomAmbisonicDecodeMatrixActiveForDiagnostics(),
            "invalid activation is rejected without replacing the active custom matrix");
    audioEngine.clearAmbisonicDecodeMatrixArtifact();
    require(audioEngine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == nullptr
                && !audioEngine.isCustomAmbisonicDecodeMatrixActiveForDiagnostics()
                && audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "clearing custom geometry atomically falls back to the standard decode graph");

    auto secondFieldAnalysisLease = audioEngine.acquireAmbisonicFieldAnalysisLease();
    fieldAnalysisLease = {};
    require(audioEngine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 1
                && audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "releasing an older field lease cannot invalidate a newer consumer");
    require(document.getState()->ambisonicBinauralFilterBank == nullptr,
            "speaker projection does not load the optional HRTF resource");
    require(audioEngine.getTransportChannelCapacityForDiagnostics() == OutputMeterSnapshot::maxChannels,
            "FOA interpretation expands the transport channel capacity before playback");

    juce::AudioBuffer<float> speakerOutputs(8, 256);
    require(audioEngine.renderTransportBlockForDiagnostics(0, speakerOutputs),
            "renders FOA through the same transport chain used by device playback");
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "the bounded transport probe releases its playback stage-graph lease");
    require(speakerOutputs.getMagnitude(3, 0, speakerOutputs.getNumSamples()) < 0.0001f,
            "FOA speaker decode leaves LFE silent");
    for (int outputChannel = 4; outputChannel < speakerOutputs.getNumChannels(); ++outputChannel)
    {
        const auto magnitude = speakerOutputs.getMagnitude(outputChannel, 0, speakerOutputs.getNumSamples());
        require(magnitude > 0.0001f,
                "FOA transport decode reaches every non-LFE 7.1 surround output");
    }
    const auto fieldSnapshot = audioEngine.getAmbisonicFieldSnapshot();
    require(fieldSnapshot.valid && fieldSnapshot.order == 1,
            "transport playback publishes an Ambisonic field snapshot on demand");
    require(AmbisonicFieldAnalyzer::energyAt(fieldSnapshot, 0.0f, 0.0f)
                > AmbisonicFieldAnalyzer::energyAt(fieldSnapshot, 180.0f, 0.0f) * 20.0f,
            "transport field snapshot preserves the source direction");
    secondFieldAnalysisLease = {};
    require(!audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics()
                && audioEngine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "closing the final field consumer fully releases the analyzer");

    audioEngine.setSourceChannelSoloed(3, true);
    juce::AudioBuffer<float> unrotatedPreview(8, 256);
    juce::AudioBuffer<float> yawPreview(8, 256);
    {
        // A bounded diagnostic probe prepares/releases on every call. Live
        // preview instead spans consecutive blocks of the same transport.
        AudioEngineDeviceTestAccess::TransportSession playback(audioEngine, 8, 256, 48000.0);
        playback.render(unrotatedPreview);
        require(audioEngine.hasAmbisonicStageGraphForDiagnostics(),
                "continuous FOA playback retains its stage-graph lease");
        require(audioEngine.setAmbisonicOrientationPreview({ 90.0f, 0.0f, 0.0f }),
                "starts a live speaker-orientation preview without committing document state");
        playback.render(yawPreview);
    }
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "ending continuous FOA playback releases its stage-graph lease");
    require(std::abs(document.getAmbisonicOrientation().yawDegrees) < 0.001f,
            "live orientation preview leaves the document orientation unchanged");
    require(std::abs(yawPreview.getSample(0, 0) - unrotatedPreview.getSample(0, 255)) < 0.001f,
            "live orientation preview starts the audio block from the previous routing matrix");
    require(std::abs(yawPreview.getSample(0, 255) - yawPreview.getSample(0, 0)) > 0.001f,
            "live orientation preview reaches the new routing matrix by the end of the block");
    require(std::abs(unrotatedPreview.getRMSLevel(0, 0, 256)
                     - yawPreview.getRMSLevel(0, 0, 256)) > 0.0001f,
            "live yaw preview changes physical speaker energy before mouse release");
    audioEngine.clearAmbisonicOrientationPreview();
    audioEngine.clearSourceChannelSoloMute();

    require(document.setAmbisonicMonitorMode(AmbisonicMonitorMode::binauralHrtf),
            "explicit FOA can select Binaural HRTF monitoring");
    require(document.getState()->ambisonicBinauralFilterBank != nullptr
                && document.getAmbisonicBinauralStatus().isEmpty(),
            "Binaural selection loads and validates the HRTF resource on demand");

    require(audioEngine.setAmbisonicOrientationPreview({ 45.0f, 0.0f, 0.0f }),
            "Binaural monitoring accepts an in-place orientation preview while dragging");
    require(std::abs(document.getAmbisonicOrientation().yawDegrees) < 0.001f,
            "Binaural orientation preview leaves the document state uncommitted");
    audioEngine.clearAmbisonicOrientationPreview();

    auto unrotatedState = document.getState();
    require(document.setAmbisonicOrientation({ 90.0f, 0.0f, 0.0f }),
            "explicit FOA accepts a yaw orientation update");
    require(document.getState() != unrotatedState
                && std::abs(document.getAmbisonicOrientation().yawDegrees - 90.0f) < 0.001f,
            "orientation update publishes immutable document state without prebuilding playback");
    unrotatedState.reset();

    juce::AudioBuffer<float> stereo(2, 256);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, stereo),
            "renders FOA through the headless playback path");
    require(audioEngine.hasAmbisonicStageGraphForDiagnostics(),
            "prepared FOA playback owns the canonical stage graph independently of the view");
    require(audioEngine.isAmbisonicBinauralRendererActive(),
            "headless playback activates the same on-demand HRTF renderer as device playback");
    require(stereo.getMagnitude(0, 0, stereo.getNumSamples()) > 0.01f,
            "FOA HRTF renderer produces audible left-ear output");
    require(stereo.getMagnitude(1, 0, stereo.getNumSamples()) > 0.01f,
            "FOA HRTF renderer produces audible right-ear output");
    require(stereo.getRMSLevel(0, 0, stereo.getNumSamples())
                > stereo.getRMSLevel(1, 0, stereo.getNumSamples()) * 1.25f,
            "headless HRTF playback applies positive yaw toward the left ear");

    auto persistentFieldLease = audioEngine.acquireAmbisonicFieldAnalysisLease();
    require(audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "a new visible field consumer can reacquire analysis independently of playback");
    const std::weak_ptr<const AmbisonicBinauralFilterBank> renderedBank = document.getState()->ambisonicBinauralFilterBank;
    require(document.setChannelLayoutPreset("quad-lrlsrs"), "switches back to ordinary Quad");
    require(document.getState()->ambisonicBinauralFilterBank == nullptr
                && document.getAmbisonicMonitorMode() == AmbisonicMonitorMode::speakerProjection,
            "leaving FOA releases HRTF state and restores speaker mode");
    require(renderedBank.expired(),
            "leaving FOA after real HRTF rendering releases drained renderer and filter-bank leases");
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics()
                && !audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "leaving Ambisonics releases playback and view stages even while the view lease survives");
    require(!document.setAmbisonicOrientation({ 45.0f, 0.0f, 0.0f })
                && document.getAmbisonicOrientation().isIdentity(),
            "ordinary Quad rejects Ambisonic rotation and retains no hidden orientation");
    juce::AudioBuffer<float> quad(4, 32);
    require(audioEngine.renderDocumentBlockForDiagnostics(0, quad),
            "ordinary Quad playback remains available after FOA monitoring");
    require(std::abs(quad.getSample(0, 0) - 0.25f) < 0.001f
                && std::abs(quad.getSample(3, 0) - 0.25f) < 0.001f,
            "switching away from FOA restores ordinary channel routing");
    require(!audioEngine.hasAmbisonicStageGraphForDiagnostics()
                && !audioEngine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "ordinary playback performs zero Ambisonics graph or analyzer work");
    persistentFieldLease = {};

    require(document.setChannelLayoutPreset("foa-acn-sn3d")
                && document.setAmbisonicMonitorMode(AmbisonicMonitorMode::binauralHrtf)
                && audioEngine.renderDocumentBlockForDiagnostics(0, stereo),
            "prepare another actual HRTF renderer for in-flight retirement");
    const std::weak_ptr<const AmbisonicBinauralFilterBank> heldBank = document.getState()->ambisonicBinauralFilterBank;
    auto heldRenderer = audioEngine.retainBinauralRendererForTesting();
    require(heldRenderer != nullptr, "hold the same renderer lease that playback acquires");
    require(document.setAmbisonicMonitorMode(AmbisonicMonitorMode::speakerProjection),
            "return to speakers while the old renderer lease is still in flight");
    audioEngine.collectRetiredBinauralRenderers();
    require(!heldBank.expired(), "control maintenance preserves the in-flight renderer");
    std::thread consumer([lease = std::move(heldRenderer)]() mutable { lease.reset(); });
    consumer.join();
    require(!heldBank.expired(), "the final consumer does not destroy the retired renderer on its thread");
    audioEngine.collectRetiredBinauralRenderers();
    require(heldBank.expired(), "the control thread releases the drained renderer and bank");

    testRoot.deleteRecursively();
}

} // namespace

void runBnkContainerBoundsTest()
{
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-BnkBounds-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create private BNK fixture");
    const auto bank = root.getChildFile("test.bnk");
    auto header = [](juce::MemoryOutputStream& out)
    {
        out.write("BKHD", 4); out.writeInt(8); out.writeInt(128); out.writeInt(7);
    };
    auto index = [](juce::MemoryOutputStream& out, int offset)
    {
        out.write("DIDX", 4); out.writeInt(12); out.writeInt(5); out.writeInt(offset); out.writeInt(4);
    };
    auto data = [](juce::MemoryOutputStream& out)
    {
        out.write("DATA", 4); out.writeInt(8); out.write("ABCDEFGH", 8);
    };
    auto parse = [&](juce::MemoryOutputStream& out)
    {
        require(bank.replaceWithData(out.getData(), out.getDataSize()), "write owned BNK");
        return BnkParser::parseFile(bank, root);
    };
    juce::MemoryOutputStream valid;
    header(valid); index(valid, 2); data(valid);
    auto state = parse(valid);
    require(state != nullptr && state->dataChunkStart == 44 && state->dataChunkSize == 8,
            "BNK DATA offset points to payload start without loading its body");
    {
        auto admitted = BnkParser::parse(bank, root);
        require(admitted && admitted.inputLease && admitted.inputLease->recheck().wasOk(),
            "typed bank result retains the actual input lease until caller publication");
#if JUCE_WINDOWS
        require(!bank.appendData("x", 1), "successful unpublished bank result pins its exact physical input");
#endif
        admitted = {};
        require(bank.replaceWithData(valid.getData(), valid.getDataSize()), "discarded result releases its input read lease");
        bool sawObjects = false;
        const auto cancelled = BnkParser::parse(bank, root, [&](const BnkParser::Progress& progress)
        { sawObjects = sawObjects || progress.stage == BnkParser::Stage::coreObjects; return !sawObjects; });
        require(!cancelled && cancelled.failure == BnkParser::Failure::cancelled && !cancelled.inputLease && sawObjects,
            "cancellation during actual core indexing publishes no bank and drains its lease");
        juce::MemoryOutputStream large;
        header(large); large.write("HIRC", 4); large.writeInt(4 + 9 + 192 * 1024); large.writeInt(1);
        large.writeByte(2); large.writeInt(4 + 192 * 1024); large.writeInt(99);
        juce::MemoryBlock payload(192 * 1024, true); large.write(payload.getData(), payload.getSize());
        require(bank.replaceWithData(large.getData(), large.getDataSize()), "create actual multi-block HIRC input");
        uint64_t observedRead = 0;
        const auto partialRead = BnkParser::parse(bank, root, [&](const BnkParser::Progress& progress)
        {
            observedRead = progress.bytesRead;
            return progress.bytesRead < 65536;
        });
        require(!partialRead && partialRead.failure == BnkParser::Failure::cancelled && !partialRead.inputLease
            && observedRead == 8 + 65536, "actual bank table read cancels after one 64 KiB block without publishing partial objects");
        require(bank.replaceWithData(valid.getData(), valid.getDataSize()), "restore valid fixture after whole-parser cancellation");
    }
    {
        juce::FileInputStream payload(bank);
        require(payload.setPosition(state->dataChunkStart + state->mediaEntries[0].offset)
                && payload.readByte() == 'C', "embedded media offset resolves actual bytes");
    }
    juce::MemoryOutputStream truncated;
    header(truncated); truncated.write("DIDX", 4); truncated.writeInt(static_cast<int>(0x80000000u));
    require(parse(truncated) == nullptr, "oversized/truncated chunk refuses the whole bank");
    juce::MemoryOutputStream range;
    header(range); index(range, 7); data(range);
    require(parse(range) == nullptr, "DIDX span cannot leave DATA");
    juce::MemoryOutputStream repeated;
    header(repeated); index(repeated, 2); index(repeated, 2); data(repeated);
    require(parse(repeated) == nullptr, "duplicate structural chunks fail closed");
    juce::MemoryOutputStream odd;
    header(odd); odd.write("UNKN", 4); odd.writeInt(3); odd.write("abc", 3); index(odd, 2); data(odd);
    state = parse(odd);
    require(state != nullptr && state->dataChunkStart == 55, "BNK unknown odd chunks have no invented RIFF padding");
    juce::MemoryOutputStream hirc;
    header(hirc); hirc.write("HIRC", 4); hirc.writeInt(4); hirc.writeInt(0x7fffffff);
    require(parse(hirc) == nullptr, "HIRC object count is bounded by its payload and policy");
    const auto hardFailure = BnkParser::parse(bank, root);
    require(!hardFailure && hardFailure.failure == BnkParser::Failure::hardLimit && !hardFailure.inputLease,
        "actual hard core limit stays distinguishable from malformed data and retains no input lease");
    require(bank.replaceWithData(valid.getData(), valid.getDataSize()), "restore valid container");
    {
        BnkDocument retained;
        require(retained.loadFile(bank, root) && retained.selectMediaById(5), "select actual parsed bank media");
        const auto parsed = retained.getState();
        const auto image = retained.captureSelection();
        const auto moved = root.getChildFile("moved.bnk");
        require(image && bank.moveFileTo(moved), "retain selection before actual bank relocation");
        require(retained.captureSelection() != nullptr, "already missing bank still has a parsed return point");
        retained.clear();
        require(retained.restoreSelection(*image) && retained.getState() == parsed && retained.getSelectedMediaId() == uint32_t{5}
            && !retained.hasBank() && !retained.getState()->isSourceCurrent(),
            "restore exact parsed selection without granting authority over missing source");
        require(moved.moveFileTo(bank), "return private bank fixture for remaining bounds checks");
    }
    require(root.getChildFile("SoundbanksInfo.xml").replaceWithText(
        "<SoundBanksInfo><SoundBanks><SoundBank><Path>test.bnk</Path><IncludedEvents>"
        "<Event Id=\"2\" Name=\"new\"/><Event Id=\"1\" Name=\"updated\"/>"
        "</IncludedEvents></SoundBank></SoundBanks></SoundBanksInfo>"), "write sidecar growth fixture");
    BnkDocument::State sidecarState;
    BnkDocument::EventDescriptor oldEvent; oldEvent.id = 1; oldEvent.displayName = "old";
    sidecarState.events.push_back(oldEvent); sidecarState.events.shrink_to_fit();
    BnkSidecarParser::enrichStateFromSidecars(bank, sidecarState, root);
    require(sidecarState.events.size() == 2 && sidecarState.events[0].id == 1
            && sidecarState.events[0].displayName == "updated", "sidecar lookup survives event-vector reallocation");
    {
        const auto infoFile = root.getChildFile("SoundbanksInfo.xml");
        const auto originalXml = infoFile.loadFileAsString();
        const auto textFile = bank.withFileExtension(".txt");
        require(textFile.replaceWithText("Switch\tID\tName\n11\tPartial switch\tPartial group\n")
            && infoFile.replaceWithText("<!DOCTYPE SoundBanksInfo [<!ENTITY forbidden 'x'>]><SoundBanksInfo/>"),
            "create actual valid text metadata followed by rejected XML");
        BnkDocument::State coreOnly;
        coreOnly.info.file = bank; coreOnly.info.eventCount = 1;
        coreOnly.events.push_back(oldEvent); coreOnly.mediaEntries.push_back({ 5, 2, 4 });
        const auto rejected = BnkSidecarParser::enrichStateFromSidecars(bank, coreOnly, root);
        require(!rejected && rejected.failure == BoundedSidecarInput::Failure::invalidData
            && rejected.inputs.size() == 2, "sidecar rejection reports typed failure and the actual physical inputs");
        require(coreOnly.switchValues.empty() && coreOnly.projectRoot == juce::File()
            && coreOnly.events.size() == 1 && coreOnly.events[0].displayName == "old"
            && coreOnly.mediaEntries.size() == 1 && coreOnly.info.eventCount == 1,
            "failed optional sidecar capability preserves proven core bank without partial text metadata");
        require(!coreOnly.info.diagnostics.isEmpty(), "failed optional metadata remains explicitly degraded");
        {
            const auto degraded = BnkParser::parse(bank, root);
            require(degraded && degraded.failure == BnkParser::Failure::none
                && degraded.sidecar.failure == BoundedSidecarInput::Failure::invalidData
                && degraded.state->switchValues.empty() && degraded.state->mediaEntries.size() == 1,
                "whole parser returns proven core plus independent typed sidecar failure");
        }
        require(infoFile.replaceWithText(originalXml), "restore real XML before cancelling after the text prefix");
#if JUCE_WINDOWS
        bool sawXmlLease = false;
        const auto cancelled = BnkSidecarParser::enrichStateFromSidecars(bank, coreOnly, root, [&]
        {
            const auto handle = CreateFileW(infoFile.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) { sawXmlLease = true; return false; }
            CloseHandle(handle); return true;
        });
        require(sawXmlLease && !cancelled && cancelled.failure == BoundedSidecarInput::Failure::cancelled
            && coreOnly.switchValues.empty() && coreOnly.projectRoot == juce::File()
            && coreOnly.events.size() == 1 && coreOnly.events[0].displayName == "old",
            "actual cancellation after text enrichment and XML input pin discards the entire private metadata prefix");
        bool sawSidecar = false;
        const auto wholeCancelled = BnkParser::parse(bank, root, [&](const BnkParser::Progress& progress)
        { sawSidecar = sawSidecar || progress.stage == BnkParser::Stage::sidecar; return !sawSidecar; });
        require(sawSidecar && !wholeCancelled && wholeCancelled.failure == BnkParser::Failure::cancelled
            && !wholeCancelled.inputLease, "whole-operation cancellation after valid core refuses all late bank publication");
#endif
        require(textFile.deleteFile() && infoFile.replaceWithText(originalXml), "restore only owned valid sidecar fixtures");
    }
    {
        const auto directory = root.getChildFile("streamed-paths"), outside = root.getChildFile("outside.wem");
        const auto localBank = directory.getChildFile("local.bnk"), localMedia = directory.getChildFile("local.wem");
        require(directory.createDirectory().wasOk() && localBank.replaceWithData(valid.getData(), valid.getDataSize())
            && localMedia.replaceWithText("local") && outside.replaceWithText("outside"), "create private streamed sidecar path fixtures");
        BnkDocument::State core;
        BnkDocument::EventDescriptor event; event.id = 8;
        BnkDocument::EventDescriptor::MediaReference item; item.mediaId = 5;
        event.media.push_back(item); core.events.push_back(event);
        require(localBank.withFileExtension(".txt").replaceWithText("Streamed Audio\tID\tName\n5\tstream\tunused\t../outside.wem\n"),
            "write actual escaped streamed path");
        const auto rejected = BnkSidecarParser::enrichStateFromSidecars(localBank, core, directory);
        require(!rejected && rejected.failure == BoundedSidecarInput::Failure::invalidData
            && core.events[0].media[0].mediaFile == juce::File() && outside.loadFileAsString() == "outside",
            "streamed text cannot resolve a real file outside its bank root or publish partial metadata");
        require(localBank.withFileExtension(".txt").replaceWithText("Streamed Audio\tID\tName\n5\tstream\tunused\tlocal.wem\n"),
            "write actual permitted streamed path");
        const auto permitted = BnkSidecarParser::enrichStateFromSidecars(localBank, core, directory);
        require(permitted && core.events[0].media[0].mediaFile == localMedia, "root-local streamed sidecar mapping remains supported");
    }
    const auto mediaFile = root.getChildFile("preview.wav");
    require(writeTestWavFile(mediaFile), "write real BNK preview fixture");
    BnkWemPlaybackBackend backend;
    BnkDocument::PlayableMediaDescriptor media;
    media.mediaId = 5; media.displayName = "fixture"; media.mediaFile = mediaFile;
    sidecarState.info.file = bank;
    auto preview = backend.prepareMediaPreview(sidecarState, media);
    require(preview.isValid() && preview.state->audioBuffer->getNumSamples() == 4800,
            "bounded BNK raw-media preview uses the real reader");
    {
        SingleTrackDocument playable;
        require(playable.adoptLoadedState(preview.state), "adopt complete bank preview through actual Document");
        AudioEngine player(playable);
        juce::AudioBuffer<float> rendered(2, 64);
        require(player.renderDocumentBlockForDiagnostics(32, rendered), "bank preview renders through real engine");
        const auto expected = 0.75f * std::sqrt(0.5f); // Existing mono-to-stereo -3 dB routing.
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < rendered.getNumSamples(); ++sample)
                require(std::abs(rendered.getSample(channel, sample) - expected) < 0.001f,
                    "bank preview preserves actual mono-to-stereo routing and exact non-silent PCM");
    }
    BnkDocument::EventDescriptor sequence;
    sequence.id = 1; sequence.playbackHint = BnkDocument::PlaybackHint::sequence;
    BnkDocument::EventDescriptor::MediaReference reference;
    reference.mediaId = 5; reference.mediaFile = mediaFile;
    sequence.media = { reference, reference };
    preview = backend.preparePreview(sidecarState, sequence, {});
    require(preview.isValid() && preview.state->audioBuffer->getNumSamples() == 9600
            && std::abs(preview.state->audioBuffer->getSample(0, 4800) - 0.75f) < 0.001f,
            "bounded sequence preserves sample placement");
    auto missing = reference; missing.mediaId = 999; missing.mediaFile = root.getChildFile("missing-required.wav");
    sequence.media = { reference, missing };
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "required failed sequence member cannot publish a partial preview");
    sequence.media.assign(17, reference);
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "sequence limit rejects the whole operation instead of truncating to 16 steps");
    const auto otherRateFile = root.getChildFile("preview-other-rate.wav");
    require(writeTestWavFile(otherRateFile, 44100.0), "write actual differing-rate preview member");
    auto otherRate = reference; otherRate.mediaFile = otherRateFile; otherRate.mediaId = 6;
    sequence.media = { reference, otherRate };
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "actual differing-rate sequence refuses the whole preview");
    sequence.media = { reference, reference };
    sequence.playbackHint = BnkDocument::PlaybackHint::layered;
    preview = backend.preparePreview(sidecarState, sequence, {});
    require(preview.isValid() && preview.state->audioBuffer->getNumSamples() == 4800
            && std::abs(preview.state->audioBuffer->getSample(0, 0) - 0.75f) < 0.001f,
            "bounded layered preview preserves existing normalisation");
    sequence.media = { reference, missing };
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "required failed layer cannot publish a partial mix");
    sequence.media.assign(9, reference);
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "layer limit rejects the whole operation instead of truncating to 8 layers");
    sequence.media = { reference, otherRate };
    require(!backend.preparePreview(sidecarState, sequence, {}).isValid(), "actual differing-rate layer refuses the whole mix");
    preview = {};
    juce::MemoryBlock embeddedWave;
    require(mediaFile.loadFileAsData(embeddedWave), "load owned embedded WEM fixture");
    juce::MemoryOutputStream embeddedBank;
    header(embeddedBank);
    embeddedBank.write("DIDX", 4); embeddedBank.writeInt(12); embeddedBank.writeInt(99);
    embeddedBank.writeInt(0); embeddedBank.writeInt(static_cast<int>(embeddedWave.getSize()));
    embeddedBank.write("DATA", 4); embeddedBank.writeInt(static_cast<int>(embeddedWave.getSize()));
    embeddedBank.write(embeddedWave.getData(), embeddedWave.getSize());
    auto embeddedState = parse(embeddedBank);
    require(embeddedState != nullptr && embeddedState->isSourceCurrent(), "capture stable bank source version");
    BnkDocument embeddedDocument;
    require(embeddedDocument.adoptParsedState(embeddedState), "adopt fixture bank without audio device");
    BnkDocument::PlayableMediaDescriptor packed;
    packed.mediaId = 99; packed.displayName = "embedded fixture"; packed.extractableFromBank = true;
    preview = backend.prepareMediaPreview(*embeddedState, packed);
    require(preview.isValid() && preview.state->audioBuffer->getNumSamples() == 4800, "actual embedded extraction produces complete preview");
    const auto extracted = embeddedDocument.getEmbeddedMediaCacheFile(99);
    require(extracted.existsAsFile() && extracted.getSize() == static_cast<int64_t>(embeddedWave.getSize()), "published extracted size");
    auto corrupted = embeddedWave;
    static_cast<unsigned char*>(corrupted.getData())[corrupted.getSize() - 1] ^= 0x5a;
    require(extracted.replaceWithData(corrupted.getData(), corrupted.getSize()), "simulate same-size embedded cache corruption");
    preview = backend.prepareMediaPreview(*embeddedState, packed);
    juce::MemoryBlock rebuilt;
    require(preview.isValid() && extracted.loadFileAsData(rebuilt) && rebuilt == embeddedWave,
            "same-size corrupt extracted cache is rebuilt from verified source span");
    require(extracted.replaceWithData(embeddedWave.getData(), embeddedWave.getSize() - 2), "simulate incomplete old extraction");
    preview = backend.prepareMediaPreview(*embeddedState, packed);
    require(preview.isValid() && extracted.getSize() == static_cast<int64_t>(embeddedWave.getSize()), "partial extraction is replaced, never appended");
    require(bank.appendData("x", 1), "modify bank after index capture");
    require(!embeddedState->isSourceCurrent() && !backend.prepareMediaPreview(*embeddedState, packed).isValid(),
            "stale bank index cannot extract from a replacement source");
    preview = {};
    require(extracted.deleteFile(), "remove only fixture-derived embedded cache");
    state.reset();
    const auto xmlFixture = root.getChildFile("bounded.wwu");
    require(BoundedSidecarInput::withinRoot(root, "../outside.wav") == juce::File()
            && BoundedSidecarInput::withinRoot(root, parent.getChildFile("outside.wav").getFullPathName()) == juce::File(),
            "sidecar relative and absolute paths cannot escape the allowed root");
    require(BoundedSidecarInput::withinRoot(root, "safe.wav") == root.getChildFile("safe.wav"), "root-local media lookup remains valid");
    {
        BoundedSidecarInput scope;
        require(xmlFixture.replaceWithText("<?xml version=\"1.0\"?><Root note=\"x &amp; y\"><!-- <ignored> --><![CDATA[<data>]]><Item/></Root>"), "write normal XML forms");
        require(BoundedSidecarInput::xml(xmlFixture) != nullptr && !scope.wasLimited(), "bounded DOM preserves legitimate XML forms");
    }
    {
        BoundedSidecarInput scope;
        require(xmlFixture.replaceWithText("<!DOCTYPE Root [<!ENTITY a \"bomb\">]><Root>&a;</Root>"), "write DTD fixture");
        require(BoundedSidecarInput::xml(xmlFixture) == nullptr && scope.wasLimited(), "DTD/entity rejected before recursive DOM parsing");
    }
    {
        BoundedSidecarInput scope;
        require(xmlFixture.replaceWithText(juce::String::repeatedString("<x>", 65)
            + juce::String::repeatedString("</x>", 65)), "write excessive-depth fixture");
        require(BoundedSidecarInput::xml(xmlFixture) == nullptr && scope.wasLimited(), "XML nesting hard ceiling");
    }
    {
        BoundedSidecarInput scope;
        require(xmlFixture.replaceWithText("<Root/>"), "restore normal XML");
        for (int i = 0; i < 128; ++i) require(BoundedSidecarInput::xml(xmlFixture) != nullptr, "file budget permitted prefix");
        require(BoundedSidecarInput::xml(xmlFixture) == nullptr && scope.wasLimited(), "one aggregate file budget spans all helpers");
    }
    {
        BoundedSidecarInput scope;
        const auto files = BoundedSidecarInput::files(root, "*.wwu");
        require(files.size() == 1 && files.front() == xmlFixture && !scope.wasLimited(), "bounded directory discovery finds supported sidecars");
    }
#if JUCE_WINDOWS
    {
        const auto link = root.getChildFile("unrelated-link"), target = root.getChildFile("junction-target");
        const auto project = root.getChildFile("owned.wproj");
        require(link.isAChildOf(root) && target.isAChildOf(root) && link.createDirectory().wasOk()
            && target.createDirectory().wasOk() && project.replaceWithText("fixture"), "create only private project-discovery fixtures");
        const auto substitute = juce::String("\\??\\") + target.getFullPathName();
        const auto display = target.getFullPathName();
        const auto substituteBytes = static_cast<USHORT>(substitute.length() * 2), displayBytes = static_cast<USHORT>(display.length() * 2);
        juce::MemoryBlock buffer(static_cast<size_t>(20 + substituteBytes + displayBytes), true);
        auto* raw = static_cast<unsigned char*>(buffer.getData());
        const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
        const USHORT dataLength = static_cast<USHORT>(12 + substituteBytes + displayBytes), displayOffset = static_cast<USHORT>(substituteBytes + 2);
        std::memcpy(raw, &tag, 4); std::memcpy(raw + 4, &dataLength, 2);
        std::memcpy(raw + 10, &substituteBytes, 2); std::memcpy(raw + 12, &displayOffset, 2); std::memcpy(raw + 14, &displayBytes, 2);
        std::memcpy(raw + 16, substitute.toWideCharPointer(), substituteBytes);
        std::memcpy(raw + 16 + displayOffset, display.toWideCharPointer(), displayBytes);
        const auto handle = CreateFileW(link.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "open exact private junction entry");
        DWORD returned = 0;
        const auto installed = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.getData(), static_cast<DWORD>(buffer.getSize()), nullptr, 0, &returned, nullptr);
        CloseHandle(handle); require(installed != FALSE && link.isSymbolicLink(), "install actual private junction");
        { BoundedSidecarInput scope;
          const auto files = BoundedSidecarInput::files(root, "*.wproj", false);
          require(!scope.wasLimited() && files.size() == 1 && files.front() == project,
              "nonrecursive project discovery ignores unrelated junction without traversing it"); }
        { BoundedSidecarInput scope;
          require(BoundedSidecarInput::files(root, "unrelated-link", false).empty()
              && scope.failureReason() == BoundedSidecarInput::Failure::invalidData,
              "a matching redirected input is still rejected"); }
        { BoundedSidecarInput scope;
          require(BoundedSidecarInput::files(root, "*.wproj", true).empty() && scope.wasLimited(),
              "recursive discovery still rejects redirected traversal"); }
        require(RemoveDirectoryW(link.getFullPathName().toWideCharPointer()) && target.isDirectory()
            && project.deleteFile() && target.deleteFile(), "remove only owned junction itself and empty private fixtures");
    }
#endif
    {
        using Budget = BoundedSidecarInput;
        const auto input = root.getChildFile("chunked.txt");
        require(input.replaceWithText(juce::String::repeatedString("guarded-sidecar-text ", 80000)), "write real multi-chunk sidecar source");
        std::atomic<bool> reached { false }, cancel { false };
        Budget::Failure failure = Budget::Failure::none;
        juce::String result;
        std::vector<media::SourceFileWitness::Ptr> witnesses;
        std::thread worker([&]
        {
            int calls = 0;
            Budget scope(nullptr, [&]
            {
                if (++calls == 3)
                {
                    reached.store(true, std::memory_order_release);
                    while (!cancel.load(std::memory_order_acquire)) juce::Thread::sleep(1);
                }
                return !cancel.load(std::memory_order_acquire);
            });
            result = Budget::text(input); failure = scope.failureReason(); witnesses = scope.inputWitnesses();
        });
        const auto stopAt = juce::Time::getMillisecondCounterHiRes() + 5000.0;
        while (!reached.load(std::memory_order_acquire) && juce::Time::getMillisecondCounterHiRes() < stopAt) juce::Thread::sleep(1);
        require(reached.load(std::memory_order_acquire), "actual sidecar worker reaches its second read checkpoint");
        require(!input.appendText("must be denied") && root.getChildFile("unrelated.txt").replaceWithText("allowed"),
            "real sidecar input lease denies mutation without locking unrelated content");
        cancel.store(true, std::memory_order_release); worker.join();
        require(result.isEmpty() && failure == Budget::Failure::cancelled && witnesses.size() == 1
            && witnesses[0]->file == input && !witnesses[0]->retainedInput(),
            "chunk cancellation rejects text prefixes and returns facts without retained Save permissions");
        require(input.appendText("allowed after drain"), "actual worker completion releases the sidecar input lease");
    }
    {
        using Budget = BoundedSidecarInput;
        require(xmlFixture.replaceWithText("<Root/>"), "prepare real profile timing source");
        { Budget scope; juce::Thread::sleep(5100);
          require(!Budget::xml(xmlFixture) && scope.failureReason() == Budget::Failure::deadline,
              "conservative sidecar budget expires on actual wall time before a read"); }
        { Budget scope(nullptr, {}, Budget::Profile::expanded); juce::Thread::sleep(5100);
          require(Budget::xml(xmlFixture) != nullptr && scope.finish(), "explicit bounded expanded profile survives the original five-second boundary"); }
        const auto large = root.getChildFile("large.wwu");
        require(large.replaceWithText(juce::String::repeatedString("x", 4 * 1024 * 1024 + 1)), "create actual above-hard-ceiling sidecar");
        { Budget scope(nullptr, {}, Budget::Profile::expanded);
          require(Budget::text(large).isEmpty() && scope.failureReason() == Budget::Failure::hardLimit,
              "expanded time budget never bypasses the fixed sidecar byte ceiling"); }
    }
    require(root.isAChildOf(parent) && root.getParentDirectory() == parent
            && root.getFileName().startsWith("SingleEditor-BnkBounds-"), "owned BNK fixture boundary");
    require(root.deleteRecursively(), "remove private BNK fixture");
    std::cout << "BnkContainerBoundsTests passed (offset, chunks, DATA spans, counts, sidecar growth, XML and traversal budgets)\n";
}

int runRecoveryOwnerRetirementCrashChild(const juce::File&, const juce::Uuid&, int);
int runRecoverySavedCancellationCrashChild(const juce::File&, int);
int runRecoveryQuarantineCrashChild(const juce::File&, const juce::Uuid&, int);
int runRecoveryTerminalCrashChild(const juce::File&, const juce::Uuid&, const singleeditor::recovery::SafeHead&, int);
int runRecoveryControlQuotaChild(const juce::File&, const juce::Uuid&, const juce::File&, int);
void runRecoveryControlQuotaTests(const juce::File&);
int runRecoveryServiceLayoutChild(const juce::File&, int, const juce::File&);
void runRecoveryServiceQuotaTests(const juce::File&);

int main(int argc, char* argv[])
{
    if (argc == 2 && juce::String(argv[1]) == "--bnk-session-only")
    { runBnkRuntimeSessionTests(); return 0; }
    if (argc == 3 && juce::String(argv[1]) == "--real-bank-characterization")
    { runRealBankCharacterization(juce::File(juce::String::fromUTF8(argv[2])), true); return 0; }
    if (argc == 3 && juce::String(argv[1]) == "--real-wem-characterization")
    { runRealBankCharacterization(juce::File(juce::String::fromUTF8(argv[2])), false); return 0; }
    if (argc == 3 && juce::String(argv[1]) == "--recovery-expiry")
    { runRecoveryExpiryTests(juce::File(juce::String::fromUTF8(argv[2]))); return 0; }
    if (argc == 3 && juce::String(argv[1]) == "--recovery-retention")
    { runRecoveryRetentionTests(juce::File(juce::String::fromUTF8(argv[2]))); return 0; }
    if (argc == 4 && juce::String(argv[1]) == "--expiry-lease-child")
        return runRecoveryExpiryLeaseChild(juce::File(juce::String::fromUTF8(argv[2])), juce::Uuid(juce::String(argv[3])));
    if ((argc == 7 && juce::String(argv[1]) == "-i" && juce::String(argv[2]) == "-W")
        || (argc == 5 && juce::String(argv[1]) == "-i" && juce::String(argv[2]) == "-o"))
        return runPackedDecoderFixture(argc, argv);
    if (argc == 3 && (juce::String(argv[1]) == "--packed-cache-observer" || juce::String(argv[1]) == "--packed-cache-observer-cancel"))
        return runPackedCacheObserver(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[1]).endsWith("-cancel"));
    if (argc == 5 && juce::String(argv[1]) == "--service-layout-child")
        return runRecoveryServiceLayoutChild(juce::File(juce::String(argv[2])), juce::String(argv[3]).getIntValue(), juce::File(juce::String(argv[4])));
    if (argc == 6 && juce::String(argv[1]) == "--control-quota-child")
        return runRecoveryControlQuotaChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])),
            juce::File(juce::String(argv[4])), juce::String(argv[5]).getIntValue());
    if (argc == 5 && juce::String(argv[1]) == "--quarantine-crash")
        return runRecoveryQuarantineCrashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])), juce::String(argv[4]).getIntValue());
    if (argc == 4 && juce::String(argv[1]) == "--saved-cancel-crash")
        return runRecoverySavedCancellationCrashChild(juce::File(juce::String(argv[2])), juce::String(argv[3]).getIntValue());
    if (argc == 7 && juce::String(argv[1]) == "--terminal-crash")
        return runRecoveryTerminalCrashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])),
            { juce::String(argv[4]).getLargeIntValue(), juce::String(argv[5]) }, juce::String(argv[6]).getIntValue());
    if (argc == 5 && juce::String(argv[1]) == "--compaction-crash")
        return runRecoveryCompactionCrashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])), juce::String(argv[4]).getIntValue());
    if (argc == 5 && juce::String(argv[1]) == "--retirement-crash")
        return runRecoveryOwnerRetirementCrashChild(juce::File(juce::String(argv[2])), juce::Uuid(juce::String(argv[3])), juce::String(argv[4]).getIntValue());
    if (argc == 2 && juce::String(argv[1]) == "--save-publication-only")
    {
        juce::ScopedJuceInitialiser_GUI initialiser;
        runSavePublicationTest();
        runRetainedSaveInputTest();
        return 0;
    }
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    if (argc == 3 && juce::String(argv[1]) == "--recovery-producer-only")
    {
        runRecoveryMutationProducerTests(juce::File(juce::String(argv[2])));
        return 0;
    }
    if (argc == 3 && juce::String(argv[1]) == "--streamed-recovery-only")
    {
        runRecoveryStreamedCaptureTests(juce::File(juce::String(argv[2])));
        return 0;
    }
    if (argc == 4 && (juce::String(argv[1]) == "--large-recovery-pilot" || juce::String(argv[1]) == "--large-dsp-recovery-pilot"))
    {
        runRecoveryLargeAudioPilot(juce::File(juce::String(argv[2])), juce::File(juce::String(argv[3])),
                                  juce::String(argv[1]) == "--large-dsp-recovery-pilot");
        return 0;
    }
    if (argc == 3 && juce::String(argv[1]) == "--recovery-capacity-pilot")
    {
        runRecoveryCapacityPilot(juce::File(juce::String(argv[2])));
        return 0;
    }
    if (argc == 3 && (juce::String(argv[1]) == "--ordinary-recovery-only" || juce::String(argv[1]) == "--ordinary-recovery-burst"))
    {
        runRecoveryOrdinaryAudioTests(juce::File(juce::String(argv[2])), juce::String(argv[1]) == "--ordinary-recovery-burst");
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--foa-monitor-only")
    {
        runLazyFoaMonitorDecodeTest();
        std::cout << "FOA monitor tests passed\n";
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bank-decode-only")
    {
        runBankDecodeOperationTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bank-open-only")
    {
        runBankOpenRequestTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bank-request-only")
    {
        runBankPreviewRequestTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--packed-lifecycle-only")
    {
        runPackedMediaLifecycleTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--service-lifecycle-only")
    {
        runRecoveryServiceTests(juce::File::getSpecialLocation(juce::File::tempDirectory));
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--service-quota-only")
    {
        runRecoveryServiceQuotaTests(juce::File::getSpecialLocation(juce::File::tempDirectory));
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--control-quota-only")
    {
        const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto root = parent.getChildFile("single-editor-control-quota-" + juce::Uuid().toString());
        require(root.createDirectory().wasOk(), "create isolated control quota test root");
        runRecoveryControlQuotaTests(root);
        require(root.isAChildOf(parent) && root.getParentDirectory() == parent
            && root.getFileName().startsWith("single-editor-control-quota-") && root.deleteRecursively(),
            "remove only uniquely owned control quota fixture after handles release");
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--recovery-host-only")
    {
        runRecoverySessionHostTests(juce::File::getSpecialLocation(juce::File::tempDirectory));
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--workspace-snapshot-only")
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("single-editor-workspace-test-" + juce::Uuid().toString());
        require(root.createDirectory().wasOk(), "create isolated workspace test root");
        runWorkspaceSnapshotTests(root);
        require(root.deleteRecursively(), "remove only the uniquely created workspace test fixture after all owners release");
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--recovery-audio-only")
    {
        runRecoveryAudioGraphTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bnk-bounds-only")
    {
        runBnkContainerBoundsTest();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bnk-hardening-only")
    {
        runBankParserHardeningTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--sidecar-semantics-only")
    {
        runBnkSidecarSemanticsTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--state-groups-only")
    {
        runBnkStateGroupTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--media-locators-only")
    {
        runBnkMediaLocatorTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--derived-paths-only")
    {
        runBnkDerivedPathTests();
        return 0;
    }
    if ((argc == 3 || argc == 4) && juce::String(argv[1]) == "--bank-corpus")
    {
        if (argc == 4) require(juce::String(argv[3]).containsOnly("0123456789") && juce::String(argv[3]).isNotEmpty(), "corpus case ID is unsigned decimal");
        runBankParserCorpusTests(juce::File(juce::String::fromUTF8(argv[2])), argc == 4 ? juce::String(argv[3]).getIntValue() : -1);
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--bank-corpus-asan-witness") return runBankCorpusAsanWitness();
    if (argc == 2 && juce::String(argv[1]) == "--rtpc-identity-only")
    {
        runBnkRtpcIdentityTests();
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--sidecar-targets-only")
    {
        runBnkSidecarTargetTests();
        return 0;
    }
    if (argc == 4 && juce::String(argv[1]) == "--optional-media-fixture")
    {
        const juce::File video(juce::String::fromUTF8(argv[2]));
        const juce::File wem(juce::String::fromUTF8(argv[3]));
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(video);
        if (!prepared.hasReader()) std::cerr << prepared.errorMessage << '\n';
        require(prepared.hasReader() && prepared.info.numChannels == 2 && prepared.info.numSamples > 0,
                "bounded FFmpeg decodes a real valid fixture");
        require(!WindowsMediaAudioService::isRuntimeLoadedForTests(), "valid video also stays out of process");
        const auto videoCache = prepared.readerSourceFile;
        const auto stamp = videoCache.getLastModificationTime();
        auto reopened = SingleTrackDocument::prepareAudioFileForOpen(video);
        require(reopened.hasReader() && reopened.info.numSamples == prepared.info.numSamples
                && videoCache.getLastModificationTime() == stamp, "bounded video cache reopens unchanged");
        reopened = {}; prepared = {};
        require(videoCache != video && videoCache.deleteFile(), "remove only fixture-derived video cache");
        require(media::DecodedWaveCache::sealFile(videoCache).deleteFile(), "remove fixture-derived video seal");
        require(VgmstreamDecodeService::isAvailable(), "explicit smoke test requires local vgmstream runtime");
        juce::File waveCache;
        std::atomic<bool> progressiveFailure { false };
        auto growing = VgmstreamDecodeService::createProgressiveReaderForDecodedFile(wem,
            [&](juce::int64, bool, bool failed) { if (failed) progressiveFailure.store(true); }, waveCache);
        require(growing != nullptr && growing->numChannels == 2 && growing->lengthInSamples > 0,
                "bounded vgmstream produces a playable prefix");
        const auto complete = VgmstreamDecodeService::ensureDecodedWave(wem, waveCache);
        require(complete.wasOk() && !progressiveFailure.load(), "progressive decode completes and reuses validated output");
        growing.reset();
        require(waveCache != wem && waveCache.deleteFile(), "remove only fixture-derived WEM cache");
        require(media::DecodedWaveCache::sealFile(waveCache).deleteFile(), "remove fixture-derived WEM seal");
        require(VgmstreamDecodeService::ensureDecodedWave(wem, waveCache).wasOk(), "blocking vgmstream path also uses bounded launch");
        require(waveCache.deleteFile(), "remove blocking fixture cache");
        require(media::DecodedWaveCache::sealFile(waveCache).deleteFile(), "remove blocking fixture seal");
        std::cout << "OptionalMediaFixtureTests passed (real FFmpeg MP4, vgmstream PCM-WEM prefix/completion/blocking; no audio device)\n";
        return 0;
    }
    runMediaIdentityTest();
    runSavePublicationTest();
    runRetainedSaveInputTest();
    runBnkContainerBoundsTest();
    runBankOpenRequestTests();
    runBankParserHardeningTests();
    runBnkSidecarSemanticsTests();
    runBnkSidecarTargetTests();
    runBnkStateGroupTests();
    runBnkRtpcIdentityTests();
    runBnkMediaLocatorTests();
    runBnkDerivedPathTests();
    runReaderAdmissionTest();
    runPackedMediaLifecycleTests();
    runBankDecodeOperationTests();
    runBankPreviewRequestTests();
    runEnvelopeGestureIdentityTest();
    runRecoveryAudioGraphTests();
    runAsyncSaveIdentityFenceTest();
    runRecoverySessionHostTests(juce::File::getSpecialLocation(juce::File::tempDirectory));
    runRecoveryServiceTests(juce::File::getSpecialLocation(juce::File::tempDirectory));
    if (argc == 2 && juce::String(argv[1]) == "--identity-only")
    {
        runStatePreservingSaveTest();
        runLatestWinsSaveTest();
        runSaveAsAndExportSemanticsTest();
        runExternalSourceIdentityConflictTest();
        std::cout << "Document identity/save headless tests passed\n";
        return 0;
    }
    runStatePreservingSaveTest();
    runLatestWinsSaveTest();
    runPlaybackContinuityAfterSaveTest();
    runSaveAsAndExportSemanticsTest();
    runOrdinaryAudioDoesNotLoadMediaFoundationTest();
    runVideoOpenFailureProvidesDiagnosticTest();
    runMp3SaveAsTest();
    runTimelineMetadataSavePolicyTest();
    runAmplitudeEditPreservesBwfTimelineTest();
    runComplexCueMetadataRejectsTimelineEditTest();
    runExternalSourceIdentityConflictTest();
    runNativeWriteFormatRoundTripTest();
    runMp3RejectsImplicitMultichannelDownmixTest();
    runPlaybackAvailabilityStateTest();
    runContiguousPlaybackBoundaryTest();
    runGrowingFilePlaybackTest();
    runWaveChannelMaskPreservedAfterSaveTest();
    runHeadlessMultichannelRoutingTest();
    runHeadlessSevenOneFourRoutingTest();
    runLazyFoaMonitorDecodeTest();
    std::cout << "DocumentSaveTests passed\n";
    return 0;
}
