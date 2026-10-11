#include "Recovery/RecoveryProtectionSession.h"
#include "Recovery/RecoveryOwnerLifecycle.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
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
using namespace std::chrono_literals;
using Cancellation = AsyncRecoveryWriter::Cancellation;
using Boundary = AsyncRecoveryWriter::Boundary;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Saved cancellation: " << status.code << '\n'; require(static_cast<bool>(status), message); }
void makeSource(const juce::File& path)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output(path.createOutputStream());
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    require(writer != nullptr, "create only owned cancellation fixture audio");
    juce::AudioBuffer<float> pcm(2, 12000);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < pcm.getNumSamples(); ++i) pcm.setSample(c, i, ((i * 17 + c) % 137 - 68) / 160.0f);
    require(writer->writeFromAudioSampleBuffer(pcm, 0, pcm.getNumSamples()), "write owned cancellation fixture PCM");
}
RecoveryProtectionSession::Config settings(const juce::File& root)
{
    RecoveryProtectionSession::Config result; result.producer.root = root;
    result.producer.queue.maximumAge = 20s; result.producer.graph.chunkSampleBytes = 4096;
    return result;
}
RecoveryMutationProducer::Health epoch(const RecoveryProtectionSession& session, const juce::Uuid& owner)
{
    const auto health = session.health();
    for (const auto& old : health.retired) if (old.owner == owner) return old;
    require(health.current.owner == owner, "selected actor remains owned until explicit runtime release");
    return health.current;
}
void restampChanged(const juce::File& file)
{
#if JUCE_WINDOWS
    const auto handle = CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open only owned saved output to inject target mutation");
    FILETIME modified{}; require(GetFileTime(handle, nullptr, nullptr, &modified), "capture exact saved output mtime");
    LARGE_INTEGER position{}; position.QuadPart = 80; unsigned char byte = 0; DWORD count = 0;
    require(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) && ReadFile(handle, &byte, 1, &count, nullptr) && count == 1,
        "read owned saved sample byte"); byte ^= 1;
    require(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) && WriteFile(handle, &byte, 1, &count, nullptr) && count == 1
        && FlushFileBuffers(handle) && SetFileTime(handle, nullptr, nullptr, &modified) && CloseHandle(handle),
        "rewrite saved content under same file ID/size/exact mtime");
#else
    juce::ignoreUnused(file);
#endif
}
void savedQueueCases(const juce::File& root, const juce::File& source)
{
    for (int mode = 0; mode < 5; ++mode)
    {
        const bool running = mode == 1 || mode == 3 || mode == 4;
        const auto recoveryRoot = root.getChildFile("cancel-queue-" + juce::String(mode));
        const auto target = root.getChildFile("cancel-saved-" + juce::String(mode) + ".wav");
        auto config = settings(recoveryRoot);
        if (!running) require(recoveryRoot.replaceWithText("owned obstruction"), "create actual recovery filesystem failure");
        juce::WaitableEvent preparing, allowPrepare, checking, allowCheck, released;
        std::atomic<int> prepares { 0 };
        std::atomic<bool> targetGuarded { false };
        std::weak_ptr<const SingleTrackDocument::State> lifetime;
        const auto ownerThread = std::this_thread::get_id();
        config.producer.queue.onBoundary = [&] (Boundary boundary)
        {
            require(std::this_thread::get_id() != ownerThread, "queue work and Save target verification stay on worker");
            if (boundary == Boundary::beforePrepare && ++prepares == 1 && running)
            { preparing.signal(); require(allowPrepare.wait(10000), "bounded in-flight capture gate"); }
            if (boundary == Boundary::beforeCancellationCheck)
            { checking.signal(); require(allowCheck.wait(10000), "bounded saved authority verification gate"); }
            if (boundary == Boundary::cancellationVerified)
            {
#if JUCE_WINDOWS
                const auto write = CreateFileW(target.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                require(write == INVALID_HANDLE_VALUE, "actual saved output remains write-protected through cancellation");
#endif
                targetGuarded = true;
            }
            if (boundary == Boundary::cancellationReleased)
            {
                if (mode == 3) require(lifetime.expired(), "all captured actual State ownership releases before cancellation acknowledgement");
                released.signal();
            }
        };
        SingleTrackDocument document; require(document.loadFile(source), "load actual Document for saved cancellation");
        std::unique_ptr<RecoveryProtectionSession> session;
        good(RecoveryProtectionSession::attachPostCore(document, config, session), "attach inert actual Session");
        require(document.moveEnvelopePoint(0, 0, 0.8f), "first actual queued edit"); lifetime = document.getState();
        if (running) require(preparing.wait(10000), "hold first actual recovery capture in flight");
        else require(!session->drain(10s), "first actual storage failure stays pending");
        require(document.moveEnvelopePoint(0, 0, 0.6f), "second actual edit remains in ordered queue");
        const auto higherVersion = document.getMediaIdentity().document.generation;
        require(mode == 4 ? document.undo() : document.moveEnvelopePoint(0, 0, 0.4f), "third actual mutation or lower-version Undo");
        if (mode == 4) require(document.getMediaIdentity().document.generation < higherVersion && document.canRedo(),
            "fixture really saves a lower state number after a higher pending version");
        const auto selected = session->health().current.owner;
        auto request = document.createSaveSnapshot(target, SingleTrackDocument::SaveIntent::saveAs);
        const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
        require(saved.succeeded && session->cancelSavedQueue(selected, request, saved).code == "cancellation_save_not_current",
            "file publication without Document adoption cannot cancel pending edits");
        require(document.markSaveSnapshotCommitted(request, saved), "adopt genuine clean current Save");
        auto savedState = document.getState();
        SingleTrackDocument::SaveWriteResult fabricated { true, target, request.changeGeneration, {} };
        require(session->cancelSavedQueue(selected, request, fabricated).code == "cancellation_save_not_current"
            && session->cancelSavedQueue(juce::Uuid(), request, saved).code == "session_cancellation_unknown_owner"
            && epoch(*session, selected).writer.pendingPackets == 3, "forged success and another owner preserve every queued edit");
        good(session->cancelSavedQueue(selected, request, saved), "actual clean Save requests independent runtime-only cancellation");
        require(epoch(*session, selected).writer.cancellation == Cancellation::requested
            && session->retry(selected).code == "writer_cancellation_terminal"
            && !session->drain(0ms), "request is asynchronous, non-durable and cannot race with retry");
        if (running) allowPrepare.signal();
        require(checking.wait(10000), "worker reaches cancellation only after current append boundary");
        const auto pending = epoch(*session, selected);
        require(pending.writer.pendingPackets == (running ? 2u : 3u) && pending.writer.retainedBytes > 0
            && pending.writer.safeHead.sequence == (running ? 1 : 0), "in-flight prefix may publish, never queued tail or false protection");
        if (mode == 2) restampChanged(target);
        if (mode == 3)
        {
            document.clear(); savedState.reset(); request = {};
            session.reset();
            require(!lifetime.expired(), "detached cancelling worker still owns accepted captures");
            allowCheck.signal();
            require(released.wait(10000) && targetGuarded && lifetime.expired(), "cancellation survives Session/actor/Document lifetime without callbacks into host");
            require(RecoveryStore(recoveryRoot, selected).inspect().head.sequence == 1, "detached cancellation preserves last durable prefix");
            continue;
        }
        allowCheck.signal();
        require(!session->drain(10s), "cancelled or failed cancellation is never reported as a successful drain");
        const auto result = epoch(*session, selected);
        if (mode == 2)
        {
            require(result.writer.cancellation == Cancellation::failed && result.writer.cancellationFailure.code == "cancellation_saved_target_changed"
                && result.writer.pendingPackets == 3 && result.writer.retainedBytes > 0 && result.writer.cancelledPackets == 0
                && !targetGuarded && session->takeDrainedEpochs().empty(), "changed saved target retains all captures and permits no cancellation acknowledgement");
            require(recoveryRoot.deleteFile(), "remove only owned obstruction for explicit original-queue retry");
            good(session->retry(selected), "failed saved cancellation still permits explicit prefix-preserving retry");
            require(session->drain(10s) && epoch(*session, selected).writer.cancellation == Cancellation::none
                && RecoveryStore(recoveryRoot, selected).inspect().head.sequence == 3, "retry really publishes every retained mutation without skipping failed front");
            continue;
        }
        require(result.writer.cancellation == Cancellation::completed && result.writer.pendingPackets == 0
            && result.writer.retainedBytes == 0 && !result.writer.workerRunning && result.writer.cancelledPackets == (running ? 2u : 3u)
            && targetGuarded, "successful cancellation releases only unpublished tail and every retained capture");
        std::unique_ptr<SavedOwnerRetirement> prohibited;
        require(!session->prepareSavedRetirement(selected, request, saved, prohibited) && !prohibited,
            "cancelled/degraded prefix cannot manufacture exact drained-owner retirement authority");
        const auto completed = session->takeDrainedEpochs();
        require(completed.size() == 1 && completed[0].writer.cancellation == Cancellation::completed
            && completed[0].writer.cancelledPackets == result.writer.cancelledPackets && session->health().retired.empty(),
            "released runtime metadata retains explicit cancellation disposition and counts");
        require(document.getState() == savedState && !document.hasUnsavedChanges() && document.canUndo(),
            "cancelling protection never modifies actual saved current/Undo state");
        if (mode == 4) require(document.canRedo(), "lower-version saved Undo retains full live Redo lineage");
        require(!recoveryRoot.getChildFile(selected.toString()).getChildFile("retired").exists(), "runtime cancellation does not publish disk deletion authority");
        if (running)
        {
            require(RecoveryStore(recoveryRoot, selected).inspect().head == result.writer.safeHead, "old recoverable head remains independently valid");
            require(document.undo() && session->drain(10s) && session->health().currentProtected
                && session->health().current.owner != selected && document.canRedo(), "future real Undo starts separate protection after cancelled old runtime is released");
        }
    }
}
}

int runRecoverySavedCancellationCrashChild(const juce::File& root, int stage)
{
    juce::ScopedJuceInitialiser_GUI initialiser;
    const auto source = root.getChildFile("source.wav"); makeSource(source);
    SingleTrackDocument document; require(document.loadFile(source), "open crash cancellation source");
    auto config = settings(root.getChildFile("store"));
    config.producer.queue.onBoundary = [stage] (Boundary boundary)
    {
        if ((stage == 0 && boundary == Boundary::beforeCancellationCheck)
            || (stage == 1 && boundary == Boundary::cancellationVerified)
            || (stage == 2 && boundary == Boundary::cancellationReleased)) std::_Exit(89);
    };
    std::unique_ptr<RecoveryProtectionSession> session;
    good(RecoveryProtectionSession::attachPostCore(document, config, session), "attach owned crash cancellation Session");
    require(document.moveEnvelopePoint(0, 0, 0.7f) && session->drain(10s), "publish actual complete baseline before cancellation crash");
    const auto original = session->health().current;
    require(root.getChildFile("owner.txt").replaceWithText(original.owner.toString() + "\n" + original.writer.safeHead.hash), "persist only owned fixture identity");
#if JUCE_WINDOWS
    const auto lockFile = config.producer.root.getChildFile(original.owner.toString()).getChildFile("writer.lock");
    const auto lock = CreateFileW(lockFile.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    require(lock != INVALID_HANDLE_VALUE, "hold actual writer lock for cancellation-crash failure");
#endif
    require(document.moveEnvelopePoint(0, 0, 0.4f) && !session->drain(10s), "actual failed append retains tail behind complete head");
    const auto request = document.createSaveSnapshot(root.getChildFile("saved.wav"), SingleTrackDocument::SaveIntent::saveAs);
    const auto saved = SingleTrackDocument::writeSaveSnapshot(request);
    require(saved.succeeded && document.markSaveSnapshotCommitted(request, saved), "genuine Save authorizes crash cancellation request");
    good(session->cancelSavedQueue(original.owner, request, saved), "request worker-side cancellation with actual saved target");
    (void) session->drain(10s);
#if JUCE_WINDOWS
    CloseHandle(lock);
#endif
    return 18;
}

void runRecoverySavedCancellationTests(const juce::File& root)
{
    const auto source = root.getChildFile("saved-cancellation-source.wav"); makeSource(source);
    savedQueueCases(root, source);
    for (int stage = 0; stage < 3; ++stage)
    {
        const auto fixture = root.getChildFile("cancel-crash-" + juce::String(stage));
        require(fixture.createDirectory().wasOk(), "create only UUID-parented cancellation crash fixture");
        juce::ChildProcess child;
        require(child.start(juce::StringArray { juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            "--saved-cancel-crash", fixture.getFullPathName(), juce::String(stage) }) && child.waitForProcessToFinish(15000)
            && child.getExitCode() == 89, "real cancellation process death at saved verification/release boundary");
        const auto identity = juce::StringArray::fromLines(fixture.getChildFile("owner.txt").loadFileAsString());
        require(identity.size() == 2, "load only owned crash fixture routing facts");
        RecoveryStore store(fixture.getChildFile("store"), juce::Uuid(identity[0]));
        const auto view = store.inspect(); good(view.status, "independently verify complete original PCM/history after cancellation crash");
        require(view.head.sequence == 1 && view.head.hash == identity[1], "cancellation crash never deletes or advances original safe head");
        std::optional<OwnerRetirement> retirement; good(store.inspectRetirement(retirement), "inspect no invented retirement authority after crash");
        require(!retirement, "runtime cancellation never grants storage deletion even after actual Save");
        SingleTrackDocument saved; require(saved.loadFile(fixture.getChildFile("saved.wav"))
            && saved.getState()->info.numSamples == 12000, "genuine saved output remains readable after abrupt cancellation death");
    }
    std::cout << "RecoverySavedCancellationTests passed (real Save authority, worker guards, pending/in-flight/degraded queues, lifetime, lower-version Undo, 3 process-kill boundaries)\n";
}
