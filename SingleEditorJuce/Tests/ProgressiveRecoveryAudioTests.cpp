#include "Recovery/ProgressiveRecoveryAudio.h"
#include "Audio/AudioEngine.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <thread>
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void requireStatus(Status status, const char* message)
{
    if (!status) std::cerr << "Recovery status: " << status.code << '\n';
    require(static_cast<bool>(status), message);
}
void checkSamples(const juce::AudioBuffer<float>& result, const juce::AudioBuffer<float>& expected, int start)
{
    for (int channel = 0; channel < result.getNumChannels(); ++channel)
        for (int frame = 0; frame < result.getNumSamples(); ++frame)
            require(std::bit_cast<uint32_t>(result.getSample(channel, frame)) == std::bit_cast<uint32_t>(expected.getSample(channel, start + frame)),
                    "mapped recovery reader preserves the original sample bits");
}
#if JUCE_WINDOWS
void installOwnedJunction(const juce::File& link, const juce::File& target, const juce::File& root)
{
    require(link.isAChildOf(root) && target.isAChildOf(root) && link.createDirectory().wasOk() && target.createDirectory().wasOk(),
        "junction fixture stays inside this test UUID root");
    const auto substitute = juce::String("\\??\\") + target.getFullPathName();
    const auto display = target.getFullPathName();
    const auto substituteBytes = static_cast<USHORT>(substitute.length() * 2);
    const auto displayBytes = static_cast<USHORT>(display.length() * 2);
    juce::MemoryBlock buffer(static_cast<size_t>(20 + substituteBytes + displayBytes), true);
    auto* raw = static_cast<unsigned char*>(buffer.getData());
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT dataLength = static_cast<USHORT>(12 + substituteBytes + displayBytes);
    const USHORT displayOffset = static_cast<USHORT>(substituteBytes + 2);
    std::memcpy(raw, &tag, 4); std::memcpy(raw + 4, &dataLength, 2);
    std::memcpy(raw + 10, &substituteBytes, 2); std::memcpy(raw + 12, &displayOffset, 2); std::memcpy(raw + 14, &displayBytes, 2);
    std::memcpy(raw + 16, substitute.toWideCharPointer(), substituteBytes);
    std::memcpy(raw + 16 + displayOffset, display.toWideCharPointer(), displayBytes);
    const auto handle = CreateFileW(link.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open owned junction directory");
    DWORD returned = 0;
    const auto installed = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.getData(), static_cast<DWORD>(buffer.getSize()),
        nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    require(installed != FALSE, "install real scratch-directory junction fixture");
}
#endif
}

void runProgressiveRecoveryAudioTests(const SingleTrackDocument::CommittedStateSnapshot& snapshot,
    const CapturedAudioGraph& captured, const RecoveryStore& store, const juce::File& root, AudioGraphLimits limits)
{
    const auto scratch = root.getChildFile("progressive-scratch");
    require(scratch.createDirectory().wasOk(), "create private progressive test scratch directory");
    AudioGraphDescriptor descriptor;
    requireStatus(RecoveryAudioGraph::describe(captured.json, descriptor, limits), "validate frame-addressable graph");
    const auto& sequence = descriptor.sequences[descriptor.states[0]];
    const auto firstEnd = static_cast<int>(sequence.chunks.front().frames);
    require(sequence.chunks.size() > 2 && firstEnd > 64, "fixture needs multiple independently recoverable chunks");
    std::unique_ptr<ProgressiveRecoveryAudio> candidate;
    requireStatus(ProgressiveRecoveryAudio::create(captured.json, 0, scratch, candidate, limits), "create independent bounded scratch mapping");
    require(candidate->readyFrames() == 0 && !candidate->isComplete()
        && candidate->getAvailability().getPlayableSampleRange(sequence.frames).isEmpty(), "creation does not advertise any unverified samples");
    auto reader = candidate->getReader();
    require(!ProgressiveRecoveryAudio::getDurableReference(reader), "unbound mapping alone has no durable CAS authority");
    juce::AudioBuffer<float> block(sequence.channels, 64);
    require(!reader->read(&block, 0, 64, 0, true, true) && block.getMagnitude(0, 64) == 0.0f,
            "unprepared audio is a failed read, never silent success");
    requireStatus(candidate->loadNext(store), "first chunk is verified and prepared separately");
    const auto durable = ProgressiveRecoveryAudio::getDurableReference(reader);
    require(durable && durable->store.ownerId() == store.ownerId() && durable->store.rootDirectory() == store.rootDirectory()
        && durable->sequence.frames == sequence.frames, "first owner load publishes immutable durable provenance separately from readiness");
    require(candidate->readyFrames() == firstEnd && !candidate->isComplete()
        && candidate->getAvailability().getPlayableSampleRange(sequence.frames).getEnd() == firstEnd,
        "first real prefix is playable while the remaining sequence has not been read");
    require(reader->read(&block, 0, 64, 32, true, true), "read prepared prefix through JUCE public reader");
    checkSamples(block, *snapshot.state->audioBuffer, 32);
    require(!reader->read(&block, 0, 64, firstEnd - 32, true, true) && block.getMagnitude(0, 64) == 0.0f,
        "crossing an unprepared boundary fails without leaking partial or uninitialised samples");

    // Exercise the production streaming-reader playback path, not Recovery UI
    // admission (which remains separate). Only this private fixture is adopted.
    {
        auto state = std::make_shared<SingleTrackDocument::State>(*snapshot.state);
        state->audioBuffer.reset(); state->memoryMappedReader.reset();
        state->streamingReader = reader; state->readerSourceFile = {};
        state->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::streamingReader;
        state->playbackAvailability = candidate->getAvailability();
        SingleTrackDocument document;
        require(document.adoptLoadedState(state), "admit private prefix fixture to actual streaming playback path");
        AudioEngine engine(document);
        require(engine.renderDocumentBlockForDiagnostics(firstEnd - 32, block), "render actual transport source at the preparation boundary");
        for (int channel = 0; channel < sequence.channels; ++channel)
            for (int frame = 0; frame < 64; ++frame)
            {
                const auto sourceFrame = firstEnd - 32 + frame;
                const auto expected = frame < 32 ? snapshot.state->audioBuffer->getSample(channel, sourceFrame)
                    * SingleTrackDocument::evaluateEnvelope(state->envelopePoints, sourceFrame / sequence.sampleRate) : 0.0f;
                require(std::abs(block.getSample(channel, frame) - expected) < 1.0e-6f, "actual playback clamps to the verified prefix and applies envelope");
            }
        require(!engine.getRuntimeMetricsForDiagnostics().has_value(), "progressive playback test never opens or changes the audio device");
    }

    // Keep reading already published samples while a real worker publishes the
    // remaining chunks. It never rewrites bytes inside the visible prefix.
    std::atomic<bool> finished { false }, succeeded { true };
    std::thread worker([&]
    {
        while (!candidate->isComplete()) if (!candidate->loadNext(store)) { succeeded.store(false); break; }
        finished.store(true, std::memory_order_release);
    });
    do
    {
        require(reader->read(&block, 0, 64, 32, true, true), "concurrent prepared-prefix read succeeds");
        checkSamples(block, *snapshot.state->audioBuffer, 32);
        std::this_thread::yield();
    } while (!finished.load(std::memory_order_acquire));
    worker.join();
    require(succeeded.load() && candidate->isComplete() && candidate->getAvailability().isRandomAccess(),
        "only the final verified chunk promotes random access");
    juce::AudioBuffer<float> complete(sequence.channels, static_cast<int>(sequence.frames));
    require(reader->read(&complete, 0, complete.getNumSamples(), 0, true, true), "read entire completed mapping");
    checkSamples(complete, *snapshot.state->audioBuffer, 0);
    candidate.reset();
    require(reader->read(&block, 0, 64, 32, true, true), "reader retains mapping ownership after control object retirement");
    checkSamples(block, *snapshot.state->audioBuffer, 32);
    reader.reset();
    require(scratch.getNumberOfChildFiles(juce::File::findFiles) == 0, "last reader release closes and deletes only its own scratch file");

    // Cancellation after the copy but before publication must leave zero ready
    // bytes. A later call cannot silently restart a cancelled candidate.
    requireStatus(ProgressiveRecoveryAudio::create(captured.json, 0, scratch, candidate, limits), "create cancellation fixture");
    int polls = 0;
    require(candidate->loadNext(store, [&] { return ++polls == 3; }).error == Error::cancelled
        && candidate->readyFrames() == 0 && candidate->loadNext(store).error == Error::cancelled, "cancelled candidate never publishes or silently retries");
    candidate.reset();

    const auto corruptRoot = root.getChildFile("progressive-corruption");
    const juce::Uuid corruptOwner;
    RecoveryStore corruptStore(corruptRoot, corruptOwner);
    CommitPacket packet;
    packet.identity = snapshot.identity; packet.operationId = "progressive-test"; packet.planJson = "{}";
    packet.stateJson = captured.json; packet.newAudio = captured.newAudio;
    SafeHead head;
    requireStatus(corruptStore.append({}, packet, head), "write independent corruption fixture");
    requireStatus(ProgressiveRecoveryAudio::create(captured.json, 0, scratch, candidate, limits), "create late-corruption candidate");
    requireStatus(candidate->loadNext(corruptStore), "prepare prefix before later disk corruption");
    const auto secondAsset = corruptRoot.getChildFile(corruptOwner.toString()).getChildFile("assets").getChildFile(sequence.chunks[1].id);
    require(secondAsset.replaceWithText("corrupt private fixture"), "corrupt only an unprepared private CAS chunk");
    require(candidate->loadNext(corruptStore).error == Error::missingOrCorruptAsset && candidate->readyFrames() == firstEnd
        && !candidate->isComplete() && candidate->loadNext(store).error == Error::missingOrCorruptAsset,
        "late corruption preserves prefix and requires explicit fresh restore even if a good store becomes available");
    reader = candidate->getReader();
    require(reader->read(&block, 0, 64, 32, true, true), "earlier prepared samples survive later corruption");
    checkSamples(block, *snapshot.state->audioBuffer, 32);
    candidate->getAvailability().markRandomAccess();
    require(!reader->read(&block, 0, 64, firstEnd, true, true), "external availability mutation cannot bypass the private prepared-byte fence");
    reader.reset(); candidate.reset();

    requireStatus(ProgressiveRecoveryAudio::create(captured.json, 0, scratch, candidate, limits), "preserve candidate across malformed create");
    const auto* preserved = candidate.get();
    auto invalid = juce::JSON::parse(captured.json);
    invalid.getDynamicObject()->setProperty("schema", 1);
    require(ProgressiveRecoveryAudio::create(juce::JSON::toString(invalid, true), 0, scratch, candidate, limits).error == Error::unsupportedSchema
        && candidate.get() == preserved, "old pilot schema is explicitly rejected without replacing candidate");
    invalid = juce::JSON::parse(captured.json);
    invalid["sequences"][0]["chunks"][0].getDynamicObject()->setProperty("frames", 0);
    require(!ProgressiveRecoveryAudio::create(juce::JSON::toString(invalid, true), 0, scratch, candidate, limits)
        && candidate.get() == preserved, "zero-length chunk rejected before mapping allocation");
    require(!ProgressiveRecoveryAudio::create(captured.json, descriptor.states.size(), scratch, candidate, limits)
        && candidate.get() == preserved, "invalid state index cannot replace a candidate");
    require(!ProgressiveRecoveryAudio::create(captured.json, 0, scratch.getChildFile("missing"), candidate, limits)
        && !scratch.getChildFile("missing").exists() && candidate.get() == preserved, "factory never creates arbitrary ancestor directories");
    candidate.reset();

    // A structurally valid plan with false boundaries must still match each
    // verified CAS body's frame count; total-length equality is insufficient.
    invalid = juce::JSON::parse(captured.json);
    const auto chunks = invalid["sequences"][0]["chunks"];
    chunks[0].getDynamicObject()->setProperty("frames", static_cast<int>(chunks[0]["frames"]) - 1);
    chunks[1].getDynamicObject()->setProperty("frames", static_cast<int>(chunks[1]["frames"]) + 1);
    auto wider = limits; wider.chunkSampleBytes *= 2;
    const auto falseBoundaries = juce::JSON::toString(invalid, true);
    requireStatus(ProgressiveRecoveryAudio::create(falseBoundaries, 0, scratch, candidate, wider), "structural bounds alone do not certify sample bodies");
    require(candidate->loadNext(store).error == Error::missingOrCorruptAsset && candidate->readyFrames() == 0,
        "CAS length mismatch refuses the first publication despite matching aggregate length");
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> full;
    require(RecoveryAudioGraph::materialise(falseBoundaries, store, full, wider).error == Error::missingOrCorruptAsset && full.empty(),
        "full-memory decoder applies the same exact CAS frame contract");
    candidate.reset();

    auto multichannel = snapshot;
    auto multiState = std::make_shared<SingleTrackDocument::State>(*snapshot.state);
    multiState->info.numChannels = 12; multiState->info.numSamples = 4096;
    multiState->audioBuffer = std::make_shared<juce::AudioBuffer<float>>(12, 4096);
    for (int channel = 0; channel < 12; ++channel)
        for (int frame = 0; frame < 4096; ++frame)
            multiState->audioBuffer->setSample(channel, frame, static_cast<float>(((frame + channel * 17) % 127) - 63) / 128.0f);
    multichannel.state = multiState; multichannel.undo.clear(); multichannel.redo.clear();
    CapturedAudioGraph multiAudio;
    requireStatus(RecoveryAudioGraph::capture(multichannel, {}, multiAudio, limits), "capture generated 12-channel PCM without a hardware device");
    RecoveryStore multiStore(root.getChildFile("progressive-12ch"), juce::Uuid());
    packet.stateJson = multiAudio.json; packet.newAudio = multiAudio.newAudio;
    requireStatus(multiStore.append({}, packet, head), "persist independent 12-channel fixture");
    requireStatus(ProgressiveRecoveryAudio::create(multiAudio.json, 0, scratch, candidate, limits), "create 12-channel mapped recovery source");
    while (!candidate->isComplete()) requireStatus(candidate->loadNext(multiStore), "load each 12-channel chunk");
    complete.setSize(12, 4096);
    require(candidate->getReader()->read(&complete, 0, 4096, 0, true, true), "JUCE reader carries every recovered channel");
    checkSamples(complete, *multiState->audioBuffer, 0);
    candidate.reset();

#if JUCE_WINDOWS
    const auto junction = root.getChildFile("progressive-junction");
    const auto other = root.getChildFile("progressive-untouched");
    installOwnedJunction(junction, other, root);
    const auto redirected = ProgressiveRecoveryAudio::create(captured.json, 0, junction, candidate, limits);
    // Remove only the owned junction itself before any normal fixture cleanup.
    require(RemoveDirectoryW(junction.getFullPathName().toWideCharPointer()) != FALSE, "remove owned junction without traversing its target");
    require(!redirected && !candidate && other.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0,
        "scratch creation refuses a junction and never writes into the redirected directory");
#endif
    require(scratch.getNumberOfChildFiles(juce::File::findFiles) == 0, "all scratch handles retire without stale temporary files");
    std::cout << "ProgressiveRecoveryAudioTests passed\n";
}
