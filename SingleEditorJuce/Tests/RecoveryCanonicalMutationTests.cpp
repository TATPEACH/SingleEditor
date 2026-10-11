#include "Recovery/RecoveryCheckpointCodec.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
using Kind = SingleTrackDocument::CanonicalMutation::Kind;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Mutation: " << status.code << '\n'; require(static_cast<bool>(status), message); }
Snapshot capture(SingleTrackDocument& document)
{
    auto value = document.captureCommittedState(document.getMediaIdentity().document);
    require(value.has_value(), "capture actual canonical mutation generation");
    return *value;
}
void compare(const Snapshot& actual, const Snapshot& wanted)
{
    require(actual.identity.document.sameVersion(wanted.identity.document)
        && actual.identity.savedGeneration == wanted.identity.savedGeneration
        && actual.undo.size() == wanted.undo.size() && actual.redo.size() == wanted.redo.size(), "canonical replay restores exact identity and history counts");
    const auto state = [] (const auto& a, const auto& b)
    {
        require(a->changeGeneration == b->changeGeneration && a->audioRevision == b->audioRevision
            && a->envelopeRevision == b->envelopeRevision && a->info.numSamples == b->info.numSamples
            && a->envelopePoints.size() == b->envelopePoints.size(), "canonical replay preserves exact state revisions/dimensions");
        for (size_t i = 0; i < a->envelopePoints.size(); ++i)
            require(a->envelopePoints[i].gain == b->envelopePoints[i].gain
                && a->envelopePoints[i].timeSeconds == b->envelopePoints[i].timeSeconds, "canonical replay preserves exact sanitized envelope");
    };
    state(actual.state, wanted.state);
    for (size_t i = 0; i < actual.undo.size(); ++i) state(actual.undo[i], wanted.undo[i]);
    for (size_t i = 0; i < actual.redo.size(); ++i) state(actual.redo[i], wanted.redo[i]);
}
}

void runRecoveryCanonicalMutationTests(const juce::File& root)
{
    const auto source = root.getChildFile("canonical-source.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(output,
            juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24)));
        require(writer != nullptr, "create owned canonical mutation fixture");
        juce::AudioBuffer<float> samples(2, 8192);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 8192; ++i) samples.setSample(c, i, static_cast<float>((i * 17 + c * 7) % 127 - 63) / 128.0f);
        require(writer->writeFromAudioSampleBuffer(samples, 0, 8192), "write owned canonical PCM");
    }
    SingleTrackDocument document;
    require(document.loadFile(source), "load actual ordinary Document for canonical edits");
    const auto initial = capture(document);
    require(!initial.mutation, "ordinary open cannot fabricate a committed mutation descriptor");
    CommitPacket checkpoint;
    good(RecoveryCheckpointCodec::encode(initial, {}, checkpoint), "freeze independent durable replay starting point");
    RecoveryStore store(root.getChildFile("canonical-base"), juce::Uuid());
    SafeHead head; good(store.append({}, checkpoint, head), "publish real baseline before subsequent edits");
    auto previous = initial;
    std::vector<juce::String> records;
    std::vector<Snapshot> expected;
    const auto record = [&] (Kind kind, bool pushesUndo)
    {
        auto committed = capture(document);
        require(committed.mutation && committed.mutation->kind == kind && committed.mutation->pushesUndo == pushesUndo
            && committed.mutation->before.sameVersion(previous.identity.document)
            && committed.mutation->after.sameVersion(committed.identity.document), "Document supplies actual before/after/Undo transition facts");
        juce::String delta;
        good(RecoveryMutationCodec::encode(previous, committed, delta), "encode exact real operation as a canonical delta");
        require(delta.getNumBytesAsUTF8() < 1500 && !delta.contains("audio-graph") && !delta.contains("metadata"),
            "ordinary envelope/history delta stays small and carries no repeated audio/file metadata graph");
        Snapshot replayed;
        good(RecoveryMutationCodec::replay(delta, previous, replayed), "replay canonical delta without calling a live Document editor");
        compare(replayed, committed);
        records.push_back(delta); expected.push_back(committed); previous = committed;
    };
    document.resetEnvelope(); record(Kind::envelopeReplace, true);
    const auto inserted = document.insertEnvelopePoint(0.04);
    require(inserted > 0, "insert actual inner envelope point"); record(Kind::envelopeReplace, true);
    require(document.moveEnvelopePoint(inserted, 0.043125, 0.6f), "move actual point with nontrivial binary values"); record(Kind::envelopeReplace, true);
    require(document.removeEnvelopePoint(inserted), "remove actual inner point"); record(Kind::envelopeReplace, true);
    document.beginEnvelopeGesture();
    require(document.moveEnvelopePoint(0, 0, 0.75f), "first actual live gesture publication"); record(Kind::envelopeReplace, true);
    require(document.moveEnvelopePoint(0, 0, 0.5f), "later actual live gesture publication"); record(Kind::envelopeReplace, false);
    const auto gestureVersion = document.getMediaIdentity().document;
    document.endEnvelopeGesture();
    require(gestureVersion.sameVersion(document.getMediaIdentity().document), "gesture end does not invent an additional Document mutation");
    require(document.undo(), "undo whole gesture"); record(Kind::undo, false);
    require(document.getEnvelopePoints().front().gain == 1.0f, "one Undo restores gesture anchor rather than previous pointer position");
    require(document.redo(), "redo whole gesture"); record(Kind::redo, false);
    for (int i = 0; i < 14; ++i)
    {
        require(document.moveEnvelopePoint(0, 0, i % 2 == 0 ? 0.3f : 0.4f), "advance real bounded Undo history");
        record(Kind::envelopeReplace, true);
    }
    require(previous.undo.size() == 10, "canonical push honors actual oldest-Undo eviction");
    require(document.undo(), "move into actual Redo branch"); record(Kind::undo, false);
    require(document.moveEnvelopePoint(0, 0, 0.8f), "new actual edit replaces Redo branch"); record(Kind::envelopeReplace, true);
    require(previous.redo.empty(), "canonical edit clears exact Redo branch");

    // Restore from serialized/durable baseline, whose audio buffers/readers have
    // independent ownership from the still-open original Document.
    const auto view = store.inspect(); good(view.status, "verify baseline from disk");
    Snapshot isolated;
    good(RecoveryCheckpointCodec::decode(view.commits.front(), store, isolated), "decode independent durable baseline");
    auto pcm = isolated.state->audioBuffer;
    require(pcm && pcm->getNumSamples() == 8192, "baseline owns complete independent PCM");
    const juce::AudioBuffer<float> pcmOracle(*pcm);
    for (size_t i = 0; i < records.size(); ++i)
    {
        Snapshot next;
        good(RecoveryMutationCodec::replay(records[i], isolated, next), "replay serialized canonical sequence on independent checkpoint");
        compare(next, expected[i]);
        require(next.state->audioBuffer == pcm, "envelope/history replay reuses immutable checkpoint PCM without copying samples");
        isolated = std::move(next);
    }
    for (int c = 0; c < 2; ++c)
        require(std::memcmp(pcm->getReadPointer(c), pcmOracle.getReadPointer(c), 8192 * sizeof(float)) == 0,
            "all canonical envelope/history operations leave PCM bit-exact");
    juce::String preserved = "preserved";
    require(RecoveryMutationCodec::encode(previous, previous, preserved).error == Error::staleHead && preserved == "preserved",
        "old transition descriptor cannot represent an uncommitted no-op");
    auto bad = expected.front();
    auto changed = std::make_shared<SingleTrackDocument::State>(*bad.state);
    changed->audioRevision += 100; bad.state = changed;
    require(!RecoveryMutationCodec::encode(initial, bad, preserved) && preserved == "preserved",
        "descriptor alone cannot hide changed audio revision or incompatible state");
    bad = expected.front(); bad.undo.clear();
    require(!RecoveryMutationCodec::encode(initial, bad, preserved) && preserved == "preserved", "descriptor cannot hide changed history semantics");
    const auto expectBad = [&] (juce::var json)
    {
        auto output = isolated;
        require(!RecoveryMutationCodec::replay(juce::JSON::toString(json, true), initial, output)
            && output.state == isolated.state, "invalid canonical record preserves caller's prior complete snapshot");
    };
    auto json = juce::JSON::parse(records.front()); json.getDynamicObject()->setProperty("schema", 2); expectBad(json);
    json = juce::JSON::parse(records.front()); json.getDynamicObject()->setProperty("source_binding", juce::Uuid().toString()); expectBad(json);
    json = juce::JSON::parse(records.front()); json.getDynamicObject()->setProperty("unknown", true); expectBad(json);
    json = juce::JSON::parse(records.front()); (*json["envelope"].getArray())[0].getArray()->set(1, "7fc00000"); expectBad(json);
    json = juce::JSON::parse(records.front()); (*json["envelope"].getArray())[1].getArray()->set(0, "0000000000000000"); expectBad(json);
    auto cancelled = isolated;
    require(RecoveryMutationCodec::replay(records.front(), initial, cancelled, [] { return true; }).error == Error::cancelled
        && cancelled.state == isolated.state, "cancelled canonical replay preserves output");
    require(RecoveryMutationCodec::encode(initial, expected.front(), preserved, [] { return true; }).error == Error::cancelled
        && preserved == "preserved", "cancelled encoding preserves previous record");

    require(document.applySelectionGain({ 0.0, document.getDurationSeconds() }, 0.5f), "commit actual generated PCM outside supported delta kinds");
    auto generated = capture(document);
    require(!generated.mutation && RecoveryMutationCodec::encode(previous, generated, preserved).code == "mutation_requires_checkpoint",
        "generated PCM cannot accidentally inherit an envelope/history descriptor");
    require(document.deleteSelection({ 0.03, 0.05 }), "commit actual delete with its join fades");
    require(!capture(document).mutation, "delete is not mislabeled as envelope-only before its exact deterministic plan is implemented");
    require(document.saveAsToFile(root.getChildFile("canonical-saved.wav")), "complete actual source rebinding");
    require(!capture(document).mutation, "source rebinding cannot retain prior source mutation facts");
    const auto hidden = source.getSiblingFile("canonical-source-hidden.wav");
    require(source.moveFileTo(hidden), "hide only owned historical locator after independent baseline was decoded");
    Snapshot detached;
    good(RecoveryMutationCodec::replay(records.front(), initial, detached), "pure delta replay does not open or query historical source path");
    compare(detached, expected.front());
    require(hidden.moveFileTo(source), "restore only owned historical locator");
    std::cout << "RecoveryCanonicalMutationTests passed (actual envelope/gesture/history, bounded deltas, independent baseline, faults)\n";
}
