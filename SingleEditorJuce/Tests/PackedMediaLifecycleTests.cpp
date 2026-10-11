#include "Audio/VgmstreamDecodeService.h"
#include "Audio/AudioEngine.h"
#include "Recovery/RecoveryCheckpointCodec.h"
#include "Waveform/WaveformModel.h"
#include "DecoderProcessWitness.h"
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
using namespace singleeditor::media;
using namespace singleeditor::recovery;
using Policy = OptionalMediaFailureState;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
template <typename F> void await(F condition, const char* message)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (!condition() && juce::Time::getMillisecondCounter() - start < 10000) juce::Thread::sleep(5);
    require(condition(), message);
}
juce::File gate(const juce::File& source) { return source.getSiblingFile(source.getFileName() + ".finish"); }
juce::File pids(const juce::File& source) { return source.getSiblingFile(source.getFileName() + ".pids"); }
std::shared_ptr<OptionalMediaOperation> request(const std::shared_ptr<Policy>& policy,
    Policy::Intent intent = Policy::Intent::initial, std::optional<Policy::Key> key = {})
{ return std::make_shared<OptionalMediaOperation>(policy, intent, key, Policy::Module::packedAudio); }
void awaitExit(const juce::File& source)
{
   #if JUCE_WINDOWS
    for (const auto& text : juce::StringArray::fromLines(pids(source).loadFileAsString()))
    {
        if (text.isEmpty()) continue;
        require(tests::decoderProcessExited(text, 5000), "actual owned decoder process identity exits");
    }
   #else
    juce::ignoreUnused(source);
   #endif
}
void clearCache(const juce::File& source)
{
    const auto wave = VgmstreamDecodeService::getDecodedWaveFile(source);
    const auto path = wave.getFullPathName().toLowerCase();
    juce::InterProcessLock lock("SingleEditor-vgmstream-" + juce::SHA256(path.toRawUTF8(), path.getNumBytesAsUTF8()).toHexString());
    require(lock.enter(5000), "actual decoder worker has released cache ownership");
    require(wave.getParentDirectory() == VgmstreamDecodeService::getDecodeCacheDirectory()
        && wave.deleteFile() && DecodedWaveCache::sealFile(wave).deleteFile()
        && wave.getSiblingFile(wave.getFileName() + ".decoding").deleteFile(), "remove only fixture-derived cache after owners release");
}
}

// Test executable acts as the actual bounded child; no real codec claim. Its
// growing WAV has a declared three-second payload but initially only one second.
int runPackedDecoderFixture(int argc, char* argv[])
{
   #if JUCE_WINDOWS
    if (argc != 7 && argc != 5) return 70;
    const juce::File output(juce::String::fromUTF8(argv[argc - 2])), source(juce::String::fromUTF8(argv[argc - 1]));
    const auto mode = source.loadFileAsString();
    if (!mode.startsWith("packed-fixture:")) return 71;
    const auto witness = tests::currentDecoderProcessWitness();
    if (witness.isEmpty() || !pids(source).appendText(witness + "\n")) return 72;
    {
        juce::FileOutputStream stream(output);
        if (!stream.openedOk()) return 73;
        stream.write("RIFF", 4); stream.writeInt(36 + 144000 * 2); stream.write("WAVEfmt ", 8);
        stream.writeInt(16); stream.writeShort(1); stream.writeShort(1); stream.writeInt(48000);
        stream.writeInt(96000); stream.writeShort(2); stream.writeShort(16); stream.write("data", 4); stream.writeInt(144000 * 2);
        for (int frame = 0; frame < 48000; ++frame) stream.writeShort(8192);
        stream.flush();
    }
    while (!gate(source).existsAsFile()) juce::Thread::sleep(5);
    if (mode.contains("late-exit")) return 37;
    auto invocations = juce::StringArray::fromLines(pids(source).loadFileAsString());
    invocations.removeEmptyStrings();
    if (mode.contains("quota") && invocations.size() == 1)
    {
        const auto handle = CreateFileW(output.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return 74;
        DWORD returned = 0;
        const auto sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        LARGE_INTEGER size; size.QuadPart = 1024LL * 1024 * 1024 + 4096;
        const auto sized = SetFilePointerEx(handle, size, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
        CloseHandle(handle);
        return sparse && sized ? 0 : 75;
    }
    juce::FileOutputStream stream(output);
    if (!stream.openedOk()) return 76;
    for (int frame = 48000; frame < 144000; ++frame) stream.writeShort(8192);
    stream.flush();
    return 0;
   #else
    juce::ignoreUnused(argc, argv); return 77;
   #endif
}

int runPackedCacheObserver(const juce::File& source, bool cancel)
{
    if (!source.getSiblingFile("observer-started").replaceWithText("started")) return 80;
    auto operation = request(std::make_shared<Policy>());
    std::thread cancellation;
    if (cancel) cancellation = std::thread([operation] { juce::Thread::sleep(100); operation->requestCancel(); });
    auto prepared = SingleTrackDocument::prepareAudioFileForOpen(source, operation);
    if (cancellation.joinable()) cancellation.join();
    if (cancel) return !prepared.hasReader() && operation->report().cancelled && !operation->report().process ? 0 : 82;
    const auto* backing = dynamic_cast<const IndependentAudioBacking*>(prepared.getReader());
    if (!prepared.hasReader() || !operation->report().succeeded || !backing || !backing->validatedBacking()) return 81;
    return 0;
}

void runPackedMediaLifecycleTests()
{
   #if JUCE_WINDOWS
    const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto root = parent.getChildFile("SingleEditor-PackedLifecycle-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create isolated packed lifecycle fixture");
    const auto prior = juce::SystemStats::getEnvironmentVariable("SINGLE_EDITOR_VGMSTREAM_PATH", {});
    require(SetEnvironmentVariableW(L"SINGLE_EDITOR_VGMSTREAM_PATH", juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getFullPathName().toWideCharPointer()) != 0, "own process uses actual bounded fixture child");
    const auto source = root.getChildFile("late-exit.wem");
    require(source.replaceWithText("packed-fixture:late-exit"), "write owned compressed-source stand-in");
    auto policy = std::make_shared<Policy>();
    auto first = request(policy);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(source, first);
        require(prepared.hasReader(), "actual child publishes first playable second before gate opens");
        auto state = SingleTrackDocument::createStateFromPreparedFile(std::move(prepared));
        SingleTrackDocument document;
        require(document.adoptLoadedState(state), "adopt actual growing reader");
        const auto* backing = dynamic_cast<const IndependentAudioBacking*>(state->streamingReader.get());
        require(backing && !backing->validatedBacking() && !first->report().terminal && !first->report().decoderComplete,
            "prefix is not a completion or independent recovery credential");
        AudioEngine engine(document); juce::AudioBuffer<float> pcm(1, 64);
        require(engine.renderDocumentBlockForDiagnostics(32, pcm) && std::abs(pcm.getSample(0, 2) - 0.25f) < 1.0e-6f,
            "real growing reader renders actual prefix through production engine");
        require(document.moveEnvelopePoint(0, 0.0, 0.7f) && document.moveEnvelopePoint(0, 0.0, 0.4f)
            && document.undo(), "create real Undo and Redo while the immutable reader is growing");
        const auto before = document.captureCommittedState(document.getMediaIdentity().document);
        require(before && !before->undo.empty() && !before->redo.empty() && !before->sourceVersion,
            "prefix document preserves both history stacks without original source credential");
        CapturedAudioGraph graph;
        require(!RecoveryAudioGraph::capture(*before, {}, graph), "partial backing cannot be frozen for recovery");
        require(gate(source).replaceWithText("finish"), "release actual child to late exit");
        await([&] { return first->report().terminal && !state->playbackAvailability.canAdvanceContiguousPrefix(); }, "late failure reaches real shared availability");
        require(first->report().failure && first->report().failure->reason == Policy::Reason::decoderExit
            && first->report().process && first->report().process->exitCode == 37 && first->report().process->jobCleanupConfirmed
            && first->report().readyFrames >= 48000 && !first->report().succeeded && !backing->validatedBacking(),
            "late failure retains actual reason, prefix progress and confirmed Job cleanup");
        require(document.getState() == before->state && document.undo() && document.redo(), "late failure does not replace Document or erase history");
        require(!RecoveryAudioGraph::capture(*before, {}, graph), "failed history still has no reusable backing proof");
    }
    awaitExit(source); clearCache(source);
    auto retry = request(policy, Policy::Intent::retryOnce, first->report().key);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(source, retry);
        await([&] { return retry->report().terminal; }, "explicit retry settles");
    }
    require(retry->report().failure && retry->policyView().circuitOpen, "two actual decoder failures fuse this session identity");
    auto denied = request(policy);
    require(!SingleTrackDocument::prepareAudioFileForOpen(source, denied).hasReader()
        && denied->report().denied == Policy::Denied::circuitOpen, "fused explicit reopen launches no third child");
    auto invocations = juce::StringArray::fromLines(pids(source).loadFileAsString());
    invocations.removeEmptyStrings();
    require(invocations.size() == 2,
        "exactly two actual decoder invocations");
    awaitExit(source); clearCache(source);

    const auto cancelledSource = root.getChildFile("cancel.wem");
    require(cancelledSource.replaceWithText("packed-fixture:hang"), "create cancellation fixture");
    auto cancelled = request(policy);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(cancelledSource, cancelled);
        require(prepared.hasReader() && !cancelled->report().terminal, "cancellation fixture reaches actual prefix");
        // Last reader destruction, with no explicit UI or operation cancel call.
    }
    await([&] { return cancelled->report().terminal; }, "dropping the last reader cancels the independently owned worker");
    require(cancelled->report().cancelled && cancelled->report().process && cancelled->report().process->jobCleanupConfirmed
        && cancelled->policyView().failures == 0, "reader destruction drains actual child without spending retry");
    awaitExit(cancelledSource); clearCache(cancelledSource);

    const auto quotaSource = root.getChildFile("quota.wem");
    require(quotaSource.replaceWithText("packed-fixture:quota"), "create actual sparse output quota fixture");
    auto limited = request(policy);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(quotaSource, limited);
        require(prepared.hasReader() && gate(quotaSource).replaceWithText("finish"), "release prefix child to real output limit");
        await([&] { return limited->report().terminal; }, "actual output bound settles");
        require(limited->report().failure && limited->report().failure->reason == Policy::Reason::outputQuota
            && limited->report().process && limited->report().process->observedOutputBytes > limited->packedLimits().outputBytes
            && limited->report().process->jobCleanupConfirmed && limited->policyView().expansionAvailable,
            "actual bounded output result authorizes a reviewed finite expansion");
    }
    awaitExit(quotaSource); clearCache(quotaSource);
    auto expanded = request(policy, Policy::Intent::expandBudget, limited->report().key);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(quotaSource, expanded);
        require(prepared.hasReader(), "reviewed expansion creates actual reader");
        await([&] { return expanded->report().terminal; }, "expanded decoder completes");
        require(expanded->report().succeeded && expanded->packedLimits().wallTimeMs == 120000
            && expanded->packedLimits().outputBytes == 4ULL * 1024 * 1024 * 1024
            && expanded->policyView().failures == 0, "expanded profile is fixed and independent from conservative failure counter");
    }
    awaitExit(quotaSource); clearCache(quotaSource);

    const auto good = root.getChildFile("success.wem");
    require(good.replaceWithText("packed-fixture:success"), "create successful progressive fixture");
    auto complete = request(policy);
    {
        auto prepared = SingleTrackDocument::prepareAudioFileForOpen(good, complete);
        require(prepared.hasReader(), "success path also opens before decoder completion");
        auto state = SingleTrackDocument::createStateFromPreparedFile(std::move(prepared));
        SingleTrackDocument document;
        require(document.adoptLoadedState(state) && document.moveEnvelopePoint(0, 0.0, 0.7f)
            && document.moveEnvelopePoint(0, 0.0, 0.4f) && document.undo(), "retain full edited history before seal");
        const auto before = document.captureCommittedState(document.getMediaIdentity().document);
        require(before && !before->undo.empty() && !before->redo.empty(), "both stacks precede completed backing");
        juce::ChildProcess cancelledObserver;
        require(cancelledObserver.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--packed-cache-observer-cancel", good.getFullPathName() }), "launch separate-process cancellable cache waiter");
        const auto cancelledObserverFinished = cancelledObserver.waitForProcessToFinish(10000);
        if (!cancelledObserverFinished) cancelledObserver.kill();
        require(cancelledObserverFinished && cancelledObserver.getExitCode() == 0 && !complete->report().terminal
            && state->readerSourceFile.getSize() == 44 + 48000 * 2, "cancelled waiter launches no decoder and cannot delete live producer data");
        require(root.getChildFile("observer-started").deleteFile(), "remove only completed observer's readiness fixture");
        juce::ChildProcess observer;
        require(observer.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--packed-cache-observer", good.getFullPathName() }), "launch actual separate-process cache consumer");
        await([&] { return root.getChildFile("observer-started").existsAsFile(); }, "separate process reaches cache admission");
        juce::Thread::sleep(100);
        require(observer.isRunning() && state->readerSourceFile.getSize() == 44 + 48000 * 2,
            "separate process cannot overwrite the first producer's live prefix");
        require(gate(good).replaceWithText("finish"), "allow actual decoder completion");
        await([&] { return complete->report().succeeded && state->playbackAvailability.isRandomAccess(); }, "actual sealed completion publishes full availability");
        const auto observerFinished = observer.waitForProcessToFinish(10000);
        if (!observerFinished) observer.kill();
        require(observerFinished && observer.getExitCode() == 0, "separate process reuses the final sealed cache with a real backing witness");
        auto goodInvocations = juce::StringArray::fromLines(pids(good).loadFileAsString()); goodInvocations.removeEmptyStrings();
        require(goodInvocations.size() == 1, "two actual consumer processes launch only one decoder");
        const auto* backing = dynamic_cast<const IndependentAudioBacking*>(state->streamingReader.get());
        require(backing && backing->validatedBacking() && backing->validatedBacking()->file == state->readerSourceFile
            && backing->validatedBacking()->file != good && !state->sourceFileWitness && !before->sourceVersion,
            "immutable decoded backing proof never impersonates compressed source facts");
        require(document.getState() == before->state && document.getMediaIdentity().document.sameVersion(before->identity.document),
            "seal completion mutates neither Document generation nor retained State identity");
        RecoveryStore store(root.getChildFile("recovery"), juce::Uuid());
        AudioGraphReuseCache reuse(store.rootDirectory(), store.ownerId());
        CommitPacket packet;
        const auto encoded = RecoveryCheckpointCodec::encode(*before, {}, packet, {}, {}, &reuse);
        if (!encoded) std::cerr << "Packed capture: " << encoded.code << '\n';
        require(static_cast<bool>(encoded) && reuse.lastCaptureWork().sourceDecodersOpened == 1,
            "existing pre-seal current/Undo/Redo capture one independent sealed PCM backing");
        SafeHead head; std::shared_ptr<const PublishedRecoveryCommit> receipt;
        require(static_cast<bool>(store.append({}, packet, head, &receipt)) && static_cast<bool>(reuse.acknowledge(receipt)),
            "publish actual complete checkpoint and seed reuse only from receipt");
        require(document.redo() && document.undo(), "post-seal real Redo and Undo remain available");
        const auto again = document.captureCommittedState(document.getMediaIdentity().document);
        require(again && static_cast<bool>(RecoveryCheckpointCodec::encode(*again, {}, packet, {}, {}, &reuse))
            && reuse.lastCaptureWork().sourceDecodersOpened == 0 && reuse.lastCaptureWork().reusedSequences == 1,
            "retained dynamic witness also participates in receipt-gated reuse");
        const auto view = store.inspect();
        SingleTrackDocument::CommittedStateSnapshot restored;
        require(view.status && view.commits.size() == 1
            && static_cast<bool>(RecoveryCheckpointCodec::decode(view.commits.back(), store, restored)), "decode actual committed current and history");
        juce::String error;
        auto recovered = SingleTrackDocument::createRecoveredCopy(restored, error);
        require(recovered && recovered->canUndo() && recovered->canRedo(), "actual isolated recovered Document retains both history stacks");
        AudioEngine engine(*recovered); juce::AudioBuffer<float> pcm(1, 64);
        const auto check = [&]
        {
            require(engine.renderDocumentBlockForDiagnostics(96000, pcm), "recovered engine plays beyond original prefix using independent PCM");
            const auto gain = SingleTrackDocument::evaluateEnvelope(recovered->getState()->envelopePoints, 96002.0 / 48000.0);
            require(std::abs(pcm.getSample(0, 2) - 0.25f * gain) < 1.0e-6f, "recovered current/history has actual correct decoded audio and envelope");
        };
        check(); require(recovered->redo(), "real recovered Redo"); check();
        require(recovered->undo() && recovered->undo(), "real recovered Undo reaches retained original"); check();
        {
            auto unavailable = std::make_shared<SingleTrackDocument::State>(*state);
            unavailable->readerSourceFile = root.getChildFile("missing-decoded-backing.wav");
            auto beforeCalls = pids(good).loadFileAsString();
            const auto preview = WaveformModel::buildLeadingPreviewPeakCacheForState(unavailable, 128, 1.0);
            require(!preview && pids(good).loadFileAsString() == beforeCalls,
                "missing declared waveform backing cannot reopen compressed source or borrow mutable playback reader");
            SingleTrackDocument missing;
            require(missing.adoptLoadedState(unavailable), "adopt owned missing-backing diagnostic state");
            const auto save = missing.createSaveSnapshot(root.getChildFile("missing-backing-export.wav"), SingleTrackDocument::SaveIntent::exportCopy);
            require(save.isValid() && !SingleTrackDocument::writeSaveSnapshot(save).succeeded
                && pids(good).loadFileAsString() == beforeCalls, "export cannot silently re-decode the original optional source when declared backing is missing");
        }
        require(state->readerSourceFile.appendData("x", 1), "change actual decoded backing after durable publication");
        const auto changed = RecoveryCheckpointCodec::encode(*again, {}, packet, {}, {}, &reuse);
        require(!changed && changed.error == Error::sourceConflict, "reuse still reacquires physical dynamic backing and rejects actual mutation");
    }
    awaitExit(good); clearCache(good);
    require(SetEnvironmentVariableW(L"SINGLE_EDITOR_VGMSTREAM_PATH", prior.isEmpty() ? nullptr : prior.toWideCharPointer()) != 0,
        "restore only this process decoder override");
    require(root.getParentDirectory() == parent && root.getFileName().startsWith("SingleEditor-PackedLifecycle-")
        && root.deleteRecursively(), "remove only uniquely owned completed packed fixture");
    std::cout << "PackedMediaLifecycleTests passed (actual progressive child, late failure/fuse, last-reader cancellation, sealed full history/reuse; no compressed codec/device claim)\n";
   #endif
}
