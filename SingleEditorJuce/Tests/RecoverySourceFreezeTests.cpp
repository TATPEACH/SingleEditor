#include "Recovery/RecoveryCheckpointCodec.h"
#include "Recovery/ProgressiveRecoveryAudio.h"
#include "Audio/AudioEngine.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
void require(bool value, const char* message)
{ if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Source freeze: " << status.code << '\n'; require(static_cast<bool>(status), message); }
void writeAudio(const juce::File& file, juce::AudioFormat& format, int channels)
{
    std::unique_ptr<juce::OutputStream> output(file.createOutputStream());
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(channels).withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(output, options));
    require(writer != nullptr, "create owned ordinary-audio fixture writer");
    juce::AudioBuffer<float> audio(channels, 8192);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i) audio.setSample(c, i, static_cast<float>((i * 13 + c * 7) % 113 - 56) / 128.0f);
    require(writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()), "write deterministic owned ordinary source");
}
void sameAudio(const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected)
{
    require(actual.getNumChannels() == expected.getNumChannels() && actual.getNumSamples() == expected.getNumSamples(), "frozen audio dimensions match independent decoder oracle");
    for (int c = 0; c < expected.getNumChannels(); ++c)
        require(std::memcmp(actual.getReadPointer(c), expected.getReadPointer(c), static_cast<size_t>(expected.getNumSamples()) * sizeof(float)) == 0,
            "all ordinary decoded PCM survives freezing and durable restore bit-exactly");
}
#if JUCE_WINDOWS
struct Handle final { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
void restamp(const juce::File& file, int64_t ticks)
{
    Handle handle;
    handle.value = CreateFileW(file.getFullPathName().toWideCharPointer(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle.value != INVALID_HANDLE_VALUE, "open owned fixture timestamp handle");
    const FILETIME time { static_cast<DWORD>(static_cast<uint64_t>(ticks)), static_cast<DWORD>(static_cast<uint64_t>(ticks) >> 32) };
    require(SetFileTime(handle.value, nullptr, nullptr, &time) != FALSE, "restore exact owned LastWriteTime");
}
#endif
}

void runRecoveryStreamedCaptureTests(const juce::File& root)
{
    require(!root.exists() && root.createDirectory().wasOk(), "create independent streamed-capture evidence directory");
    juce::WavAudioFormat wav;
    const auto source = root.getChildFile("source.wav");
    writeAudio(source, wav, 8);
    juce::AudioBuffer<float> oracle(8, 8192);
    {
        auto reader = SingleTrackDocument::createReaderForFile(source);
        require(reader && reader->read(&oracle, 0, 8192, 0, true, true), "read independent complete 8-channel PCM oracle");
    }
    SingleTrackDocument document;
    require(document.loadFile(source) && document.moveEnvelopePoint(0, 0, 0.4f)
        && document.applySelectionGain({ 0, document.getDurationSeconds() }, 0.5f) && document.undo(),
        "actual envelope/DSP/Undo provides original source, generated PCM and both history branches");
    const auto snapshot = *document.captureCommittedState(document.getMediaIdentity().document);
    const auto workspace = document.captureWorkspaceSnapshot();
    require(snapshot.undo.size() == 1 && snapshot.redo.size() == 1 && snapshot.redo.back()->audioBuffer,
        "streaming fixture has full current/Undo/Redo and actual DSP PCM");
    AudioGraphLimits limits; limits.chunkSampleBytes = 16384; limits.streamNewAudio = true;
    auto cancelled = std::make_shared<bool>(false);
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encode(snapshot, {}, packet, limits, [cancelled] { return *cancelled; }),
        "capture bounded replayable blocks instead of retaining all new PCM");
    require(packet.newAudio.empty() && packet.streamedAudio.size() > 2,
        "prepared streaming packet retains no vector of full PCM blocks");
    RecoveryStore store(root.getChildFile("store"), juce::Uuid()); SafeHead head;
    good(store.append({}, packet, head), "stream source and generated DSP blocks through actual Store publication");
    auto view = store.inspect(); good(view.status, "verify actual streamed graph and all durable bytes");
    SingleTrackDocument::CommittedStateSnapshot restored;
    good(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored, limits), "decode full streamed current/Undo/Redo");
    sameAudio(*restored.state->audioBuffer, oracle);
    sameAudio(*restored.undo.front()->audioBuffer, oracle);
    sameAudio(*restored.redo.back()->audioBuffer, *snapshot.redo.back()->audioBuffer);
    const auto savedHead = head;
    *cancelled = true;
    require(store.append(head, packet, head).error == Error::cancelled && head == savedHead
        && store.inspect().head == savedHead, "cancellation between capture and publication retains the protected head");
    *cancelled = false;
    const auto graph = juce::JSON::parse(packet.stateJson)["audio"].toString();
    const auto scratch = root.getChildFile("scratch"); require(scratch.createDirectory().wasOk(), "create private progressive scratch");
    std::unique_ptr<ProgressiveRecoveryAudio> progressive;
    good(ProgressiveRecoveryAudio::create(graph, 0, scratch, progressive, limits, &store), "create real progressive restore reader");
    good(progressive->loadNext(store), "load only first verified recovery chunk");
    require(!progressive->isComplete(), "the remaining current audio is still unprepared in the mapping");
    auto partial = snapshot;
    good(progressive->bindMetadata(*restored.state, partial.state), "bind current metadata to actual partial recovery reader");
    CommitPacket recaptured;
    good(RecoveryCheckpointCodec::encode(partial, {}, recaptured, limits),
        "stream all durable CAS chunks, including the unprepared recovery tail and original Undo/DSP Redo");
    require(recaptured.newAudio.empty() && !recaptured.streamedAudio.empty(), "recovered source also uses bounded packet sources");
    RecoveryStore destination(root.getChildFile("recaptured"), juce::Uuid()); SafeHead copied;
    good(destination.append({}, recaptured, copied), "publish independent complete graph from partially restored audio");
    view = destination.inspect(); good(view.status, "verify recaptured complete CAS closure");
    good(RecoveryCheckpointCodec::decode(view.commits.back(), destination, restored, limits), "decode full recaptured current/history");
    sameAudio(*restored.state->audioBuffer, oracle);
    sameAudio(*restored.undo.front()->audioBuffer, oracle);
    sameAudio(*restored.redo.back()->audioBuffer, *snapshot.redo.back()->audioBuffer);
    require(document.captureWorkspaceSnapshot()->sameContents(*workspace), "all streaming work preserves the live Document and exact full history");
    std::cout << "RecoveryStreamedCaptureTests passed (8-channel source, real DSP, full current/Undo/Redo, partial recovery CAS, cancellation and unchanged live Document)\n";
}

void runRecoverySourceFreezeTests(const juce::File& root, singleeditor::recovery::AudioGraphLimits limits)
{
    using namespace singleeditor;
    using namespace singleeditor::recovery;
    juce::WavAudioFormat wav;
    juce::FlacAudioFormat flac;
    juce::OggVorbisAudioFormat ogg;
    juce::AiffAudioFormat aiff;
    struct Case { juce::AudioFormat* format; const char* extension; int channels; };
    for (const auto& item : { Case { &wav, "wav", 8 }, Case { &flac, "flac", 2 }, Case { &ogg, "ogg", 2 }, Case { &aiff, "aiff", 2 } })
    {
        const auto file = root.getChildFile(juce::String("freeze-") + item.extension + "." + item.extension);
        writeAudio(file, *item.format, item.channels);
        juce::AudioBuffer<float> expected(item.channels, 8192);
        {
            auto oracle = SingleTrackDocument::createReaderForFile(file);
            require(oracle && oracle->read(&expected, 0, 8192, 0, true, true), "decode independent complete PCM oracle before source admission");
        }
        SingleTrackDocument document;
        require(document.loadFile(file), "actual ordinary Document open succeeds");
        const auto initial = document.getState();
        require(initial->sourceFileWitness && initial->sourceFileWitness->file == file
            && initial->sourceFileWitness->volumeId.length() == 16 && initial->sourceFileWitness->fileId.length() == 32
            && initial->sourceFileWitness->frames == 8192 && initial->sourceFileWitness->channels == item.channels
            && initial->sourceFileWitness->sampleRate == 48000 && initial->sourceFileWitness->readerFormat.isNotEmpty(),
            "production reader admission retains original parsed facts and guarded physical witness");
        require(initial->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::streamingReader
            || initial->audioBackingStore == SingleTrackDocument::State::AudioBackingStore::memoryMappedFile, "ordinary fixture remains reader-backed without eager audio materialization");
        const auto cursor = initial->streamingReader && initial->streamingReader->input ? initial->streamingReader->input->getPosition() : -1;
        require(document.moveEnvelopePoint(0, 0.0, 0.4f), "commit actual envelope while retaining ordinary reader source");
        const auto captured = *document.captureCommittedState(document.getMediaIdentity().document);
        const auto destination = root.getChildFile(juce::String("freeze-store-") + item.extension);
        require(!destination.exists(), "read and mutation alone have no registered recovery writer or directory");
        CommitPacket packet;
        good(RecoveryCheckpointCodec::encode(captured, {}, packet, limits), "freeze ordinary current and Undo through independent guarded decoder");
        require(!destination.exists() && document.getState() == captured.state
            && captured.state->audioBackingStore == initial->audioBackingStore && captured.state->sourceFileWitness == initial->sourceFileWitness,
            "worker capture does not change live backing, generation or ordinary reader facts");
        if (cursor >= 0) require(initial->streamingReader->input->getPosition() == cursor, "independent capture never advances shared playback decoder input position");
        const auto graph = juce::JSON::parse(juce::JSON::parse(packet.stateJson)["audio"].toString());
        require(graph["states"].size() == 2 && graph["sequences"].size() == 1, "envelope/current Undo share one frozen physical backing");
        RecoveryStore store(destination, juce::Uuid()); SafeHead head;
        good(store.append({}, packet, head), "publish actual ordinary frozen checkpoint");
        const auto view = store.inspect(); good(view.status, "verify all ordinary frozen assets");
        SingleTrackDocument::CommittedStateSnapshot restored;
        good(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored, limits), "restore independently frozen ordinary checkpoint");
        require(restored.undo.size() == 1 && restored.state->envelopePoints.front().gain == 0.4f, "ordinary freeze preserves actual mutation and ordered Undo");
        sameAudio(*restored.state->audioBuffer, expected); sameAudio(*restored.undo.front()->audioBuffer, expected);
        auto unchanged = packet; int polls = 0;
        require(RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits, [&] { return ++polls > 8; }).error == Error::cancelled
            && unchanged.stateJson == packet.stateJson && store.inspect().head == head, "cancelled source freeze preserves old caller output and durable head");
        // The scoped read lease must be released even on cancellation.
        std::unique_ptr<media::SourceFileLease> released;
        require(media::SourceFileLease::open(file, released).wasOk(), "source lease can be acquired after cancelled freeze");
        released.reset();
#if JUCE_WINDOWS
        if (juce::String(item.extension) == "wav")
        {
            const auto old = file.getSiblingFile("freeze-original-held.wav");
            require(file.moveFileTo(old) && old.copyFileTo(file), "replace only owned same-path source with byte-identical different file identity");
            restamp(file, initial->sourceFileWitness->modifiedTicks);
            require(RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits).error == Error::sourceConflict
                && unchanged.stateJson == packet.stateJson, "same name/size/mtime/content replacement cannot satisfy the opened physical witness");
            require(file.deleteFile() && old.moveFileTo(file), "restore only private replaced source fixture");
        }
        if (juce::String(item.extension) == "flac")
        {
            const auto before = *initial->sourceFileWitness;
            const auto size = file.getSize(); char oldByte = 0;
            { juce::FileInputStream input(file); require(input.openedOk() && input.setPosition(size - 1), "read owned restamp fixture tail"); oldByte = input.readByte(); }
            { juce::FileOutputStream output(file); require(output.openedOk() && output.setPosition(size - 1) && output.writeByte(static_cast<char>(oldByte ^ 1)), "edit private source with same size"); output.flush(); }
            restamp(file, before.modifiedTicks);
            require(RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits).error == Error::sourceConflict
                && unchanged.stateJson == packet.stateJson, "actual ChangeTime catches a same-size edit with restored LastWriteTime before decoder access");
        }
        if (juce::String(item.extension) == "ogg")
        {
            Handle writer;
            writer.value = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            require(writer.value != INVALID_HANDLE_VALUE, "open owned external-writer contention fixture");
            const auto blocked = RecoveryCheckpointCodec::encode(captured, {}, unchanged, limits);
            require(blocked.error == Error::resourceOrEnvironmentFault && unchanged.stateJson == packet.stateJson,
                "busy writer prevents unsafe freeze but preserves prior protection");
            SingleTrackDocument busyReader;
            require(busyReader.loadFile(file) && !busyReader.getState()->sourceFileWitness, "ordinary playback admission remains available without claiming a frozen witness");
            AudioEngine engine(busyReader); juce::AudioBuffer<float> rendered(2, 32);
            require(engine.renderDocumentBlockForDiagnostics(0, rendered), "real playback remains available when optional physical witness is unavailable");
        }
#endif
    }
    std::cout << "RecoverySourceFreezeTests passed (ordinary WAV/AIFF/FLAC/OGG, independent cursor, full PCM/history, replaced/restamped/busy source, cancellation)\n";
}
