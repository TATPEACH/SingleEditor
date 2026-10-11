#include "Recovery/RecoveryAudioGraph.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>

void runRecoveryCheckpointCodecTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&,
                                    const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runRecoverySourceFreezeTests(const juce::File&, singleeditor::recovery::AudioGraphLimits);
void runRecoveryMutationProducerTests(const juce::File&);
void runRecoveryOwnerLifecycleTests(const juce::File&);
void runRecoverySavedCancellationTests(const juce::File&);
void runRecoveryTerminalQuarantineTests(const juce::File&);
void runWorkspaceSnapshotTests(const juce::File&);
void runRecoveryGraphReuseTests(const juce::File&);
void runRecoveryCanonicalMutationTests(const juce::File&);
void runProgressiveRecoveryAudioTests(const singleeditor::SingleTrackDocument::CommittedStateSnapshot&,
    const singleeditor::recovery::CapturedAudioGraph&, const singleeditor::recovery::RecoveryStore&,
    const juce::File&, singleeditor::recovery::AudioGraphLimits);

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
void require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
bool sameSamples(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples()) return false;
    for (int channel = 0; channel < a.getNumChannels(); ++channel)
        for (int frame = 0; frame < a.getNumSamples(); ++frame)
            if (std::bit_cast<uint32_t>(a.getSample(channel, frame)) != std::bit_cast<uint32_t>(b.getSample(channel, frame))) return false;
    return true;
}
juce::String text(const juce::var& value) { return juce::JSON::toString(value, true); }
} // namespace

void runRecoveryAudioGraphTests()
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditor-AudioGraph-" + juce::Uuid().toString());
    require(root.createDirectory().wasOk(), "create owner-only audio graph fixture");
    const auto source = root.getChildFile("source.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(source.createOutputStream());
        const std::unordered_map<juce::String, juce::String> metadata {
            { juce::WavAudioFormat::bwavDescription, "Recovery BWF fixture" },
            { juce::WavAudioFormat::bwavTimeReference, "96000" },
            { "NumCuePoints", "1" }, { "Cue0Identifier", "17" }, { "Cue0Offset", "3000" }
        };
        const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24)
            .withMetadataValues(metadata);
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
        require(writer != nullptr, "create stereo fixture writer");
        juce::AudioBuffer<float> audio(2, 4800);
        for (int frame = 0; frame < 4800; ++frame)
        {
            audio.setSample(0, frame, static_cast<float>((frame % 37) - 18) / 32.0f);
            audio.setSample(1, frame, static_cast<float>((frame % 53) - 26) / 32.0f);
        }
        require(writer->writeFromAudioSampleBuffer(audio, 0, 4800), "write stereo fixture");
    }
    SingleTrackDocument document;
    {
        auto reader = SingleTrackDocument::createReaderForFile(source);
        require(reader != nullptr, "open private fixture reader");
        require(document.adoptLoadedState(SingleTrackDocument::createStateFromReader(source, *reader)), "adopt materialised real audio");
    }
    auto transaction = document.beginEqualLengthAudioMutation(document.getState());
    require(transaction && transaction->isValid(), "prepare actual AudioSuite transaction and frozen Undo backing");
    juce::AudioBuffer<float> rendered;
    require(transaction->readSourceBlock(0, 4800, rendered), "read actual transaction source samples");
    rendered.applyGain(0.5f);
    require(transaction->writeRenderedBlock(0, rendered, 0, 4800)
                && document.commitEqualLengthAudioMutation(std::move(transaction)), "commit actual generated audio samples");
    require(document.moveEnvelopePoint(0, 0.0, 0.8f), "commit envelope sharing gain backing");
    require(document.deleteSelection({ 0.02, 0.04 }) && document.undo(), "retain ordered Undo and Redo with different lengths");
    auto snapshot = *document.captureCommittedState(document.getMediaIdentity().document);
    require(snapshot.undo.size() == 2 && snapshot.redo.size() == 1, "actual Document graph has expected stacks");
    AudioGraphLimits limits;
    limits.chunkSampleBytes = 4096;
    CapturedAudioGraph captured;
    const auto captureStatus = RecoveryAudioGraph::capture(snapshot, {}, captured, limits);
    if (!captureStatus) std::cerr << "Capture failure: " << captureStatus.code << '\n';
    require(static_cast<bool>(captureStatus), "capture all current/Undo/Redo audio");
    const auto graph = juce::JSON::parse(captured.json);
    require(graph["states"].size() == 4 && graph["sequences"].size() == 3,
            "shared envelope backing is captured once without merging distinct edited audio");
    require(captured.newAudio.size() > 3 && captured.existingAudio.empty(), "split actual samples into bounded CAS chunks");
    RecoveryStore store(root.getChildFile("recovery"), juce::Uuid());
    CommitPacket packet;
    packet.identity = snapshot.identity;
    packet.operationId = "audio-checkpoint-test";
    packet.planJson = "{}";
    packet.stateJson = captured.json;
    packet.newAudio = captured.newAudio;
    SafeHead first;
    require(static_cast<bool>(store.append({}, packet, first)) && first.sequence == 1, "durably publish graph and actual samples");
    require(source.replaceWithText("This path now contains different bytes; recovery must never reopen it."),
            "replace original path after buffers were captured");
    std::vector<std::shared_ptr<juce::AudioBuffer<float>>> restored;
    const auto view = store.inspect();
    require(static_cast<bool>(view.status) && view.commits.size() == 1, "verify durable head");
    require(static_cast<bool>(RecoveryAudioGraph::materialise(view.commits.back().record["state"].toString(), store, restored, limits)),
            "restore audio through verified store independently of replaced source path");
    std::vector<SingleTrackDocument::StatePtr> expected { snapshot.state };
    expected.insert(expected.end(), snapshot.undo.begin(), snapshot.undo.end());
    expected.insert(expected.end(), snapshot.redo.begin(), snapshot.redo.end());
    require(restored.size() == expected.size(), "restore one ordered binding per retained state");
    for (size_t index = 0; index < expected.size(); ++index)
        require(sameSamples(*restored[index], *expected[index]->audioBuffer), "bit-exact stereo samples and frame lengths for every Undo/Redo binding");
    require(restored[0] == restored[2] && restored[0] != restored[1] && restored[0] != restored[3], "restore safe shared ownership");

    std::set<juce::String> known;
    for (const auto& block : captured.newAudio)
    {
        juce::String id;
        require(static_cast<bool>(RecoveryStore::identifyAudio(block, limits.assetBytes, id)), "derive canonical store asset ID");
        known.insert(id);
    }
    CapturedAudioGraph repeat;
    require(static_cast<bool>(RecoveryAudioGraph::capture(snapshot, known, repeat, limits)) && repeat.json == captured.json
                && repeat.newAudio.empty() && repeat.existingAudio.size() == known.size(), "later checkpoint reuses durable CAS content");
    packet.newAudio.clear(); packet.existingAudio = repeat.existingAudio;
    SafeHead second;
    require(static_cast<bool>(store.append(first, packet, second)) && second.sequence == 2, "store independently verifies hinted reuse");
    RecoveryStore emptyStore(root.getChildFile("empty"), juce::Uuid());
    SafeHead rejected;
    require(!emptyStore.append({}, packet, rejected) && rejected.sequence == 0, "unverified dedupe hints cannot publish missing samples");

    const auto originalJson = captured.json;
    const auto expectCaptureFailure = [&] (const auto& altered, AudioGraphLimits bounds, Error expectedError)
    {
        const auto result = RecoveryAudioGraph::capture(altered, {}, captured, bounds);
        require(!result && result.error == expectedError && captured.json == originalJson, "failed capture preserves previous complete output");
    };
    auto alias = std::make_shared<SingleTrackDocument::State>(*snapshot.undo.back());
    alias->info.sampleRate = 96000;
    auto altered = snapshot; altered.undo.back() = alias;
    expectCaptureFailure(altered, limits, Error::invalidPacket);
    auto readerBacked = std::make_shared<SingleTrackDocument::State>(*snapshot.undo.front());
    readerBacked->audioBackingStore = SingleTrackDocument::State::AudioBackingStore::streamingReader;
    readerBacked->audioBuffer.reset();
    altered = snapshot; altered.undo.front() = readerBacked;
    const auto notFrozen = RecoveryAudioGraph::capture(altered, {}, captured, limits);
    require(!notFrozen && notFrozen.code == "audio_backing_not_frozen" && captured.json == originalJson,
            "historical live reader fails closed instead of reopening a changed path");
    auto badSamples = std::make_shared<SingleTrackDocument::State>(*snapshot.redo.front());
    badSamples->audioBuffer = std::make_shared<juce::AudioBuffer<float>>(*badSamples->audioBuffer);
    badSamples->audioBuffer->setSample(1, badSamples->audioBuffer->getNumSamples() - 1, std::numeric_limits<float>::infinity());
    altered = snapshot; altered.redo.front() = badSamples;
    expectCaptureFailure(altered, limits, Error::invalidPacket);
    auto small = limits; small.materialisedBytes = 40000;
    expectCaptureFailure(snapshot, small, Error::budgetExceeded);
    small = limits; small.chunks = 1;
    expectCaptureFailure(snapshot, small, Error::budgetExceeded);
    int capturePolls = 0;
    require(RecoveryAudioGraph::capture(snapshot, {}, captured, limits, [&] { return ++capturePolls == 4; }).error == Error::cancelled
                && captured.json == originalJson, "mid-chunk capture cancellation preserves output");

    const auto preserved = restored;
    const auto expectRestoreFailure = [&] (juce::var changed, AudioGraphLimits bounds)
    {
        require(!RecoveryAudioGraph::materialise(text(changed), store, restored, bounds) && restored == preserved,
                "invalid graph cannot replace the last complete isolated candidate");
    };
    auto changed = juce::JSON::parse(originalJson);
    changed.getDynamicObject()->setProperty("unexpected_required", true);
    expectRestoreFailure(changed, limits);
    changed = juce::JSON::parse(originalJson);
    changed["states"].getArray()->set(0, 99);
    expectRestoreFailure(changed, limits);
    changed = juce::JSON::parse(originalJson);
    (*changed["sequences"].getArray())[0].getDynamicObject()->setProperty("rate", 96000.0);
    expectRestoreFailure(changed, limits);
    changed = juce::JSON::parse(originalJson);
    (*changed["sequences"].getArray())[2]["chunks"].getArray()->removeLast();
    expectRestoreFailure(changed, limits);
    changed = juce::JSON::parse(originalJson);
    (*changed["sequences"].getArray())[2]["chunks"][0].getDynamicObject()->setProperty("id", juce::String::repeatedString("f", 64));
    expectRestoreFailure(changed, limits);
    changed = juce::JSON::parse(originalJson);
    (*changed["sequences"].getArray())[2]["chunks"][0].getDynamicObject()->setProperty("id", "../../source.wav");
    expectRestoreFailure(changed, limits);
    small = limits; small.materialisedBytes = 40000;
    expectRestoreFailure(juce::JSON::parse(originalJson), small);
    int restorePolls = 0;
    require(RecoveryAudioGraph::materialise(originalJson, store, restored, limits, [&] { return ++restorePolls == 5; }).error == Error::cancelled
                && restored == preserved, "progressive restore cancellation never publishes partial buffers");
    require(!RecoveryAudioGraph::materialise(juce::String::repeatedString("[", 1000), store, restored, limits)
                && restored == preserved, "bound hostile nesting before JSON parser");
    runProgressiveRecoveryAudioTests(snapshot, captured, store, root, limits);
    runRecoveryCheckpointCodecTests(snapshot, root, limits);
    runRecoverySourceFreezeTests(root, limits);
    runRecoveryMutationProducerTests(root);
    runRecoveryOwnerLifecycleTests(root);
    runRecoverySavedCancellationTests(root);
    runRecoveryTerminalQuarantineTests(root);
    runWorkspaceSnapshotTests(root);
    runRecoveryGraphReuseTests(root);
    runRecoveryCanonicalMutationTests(root);
    document.clear();
    // This is a UUID-scoped fixture created above, never a user recovery owner.
    require(root.getParentDirectory() == juce::File::getSpecialLocation(juce::File::tempDirectory)
                && root.getFileName().startsWith("SingleEditor-AudioGraph-") && root.deleteRecursively(), "remove only owned graph fixture");
    std::cout << "RecoveryAudioGraphTests passed\n";
}
