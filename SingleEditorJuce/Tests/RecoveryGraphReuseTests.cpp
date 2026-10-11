#include "Recovery/RecoveryCheckpointCodec.h"
#include <cstring>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
using namespace singleeditor::recovery;
void require(bool ok, const char* message)
{ if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void good(Status status, const char* message)
{ if (!status) std::cerr << "Reuse: " << status.code << '\n'; require(static_cast<bool>(status), message); }
using Snapshot = SingleTrackDocument::CommittedStateSnapshot;
Snapshot capture(SingleTrackDocument& document)
{
    auto snapshot = document.captureCommittedState(document.getMediaIdentity().document);
    require(snapshot.has_value(), "capture actual committed version for graph reuse");
    return *snapshot;
}
}

void runRecoveryGraphReuseTests(const juce::File& root)
{
    const auto source = root.getChildFile("reuse-source.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(output,
            juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24)));
        require(writer != nullptr, "create owned independent reuse fixture");
        juce::AudioBuffer<float> pcm(2, 8192);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 8192; ++i) pcm.setSample(c, i, static_cast<float>((i * 7 + c * 11) % 127 - 63) / 128.0f);
        require(writer->writeFromAudioSampleBuffer(pcm, 0, 8192), "write owned reuse fixture");
    }
    SingleTrackDocument document;
    require(document.loadFile(source) && document.moveEnvelopePoint(0, 0.0, 0.7f), "actual first envelope mutation");
    const auto storeRoot = root.getChildFile("reuse-owner");
    const juce::Uuid owner;
    RecoveryStore store(storeRoot, owner);
    AudioGraphReuseCache reuse(storeRoot, owner);
    SafeHead head;
    std::shared_ptr<const PublishedRecoveryCommit> receipt;
    auto first = capture(document);
    CommitPacket packet;
    good(RecoveryCheckpointCodec::encode(first, {}, packet, {}, {}, &reuse), "freeze first source graph");
    require(reuse.lastCaptureWork().sourceDecodersOpened == 1 && reuse.lastCaptureWork().reusedSequences == 0
        && reuse.lastCaptureWork().sampleBytesReadOrCopied == 8192 * 2 * sizeof(float)
        && !packet.newAudio.empty(), "first capture measures actual independent source decoding");
    good(RecoveryCheckpointCodec::encode(first, {}, packet, {}, {}, &reuse), "repeat prepared but unpublished capture");
    require(reuse.lastCaptureWork().sourceDecodersOpened == 1 && reuse.lastCaptureWork().reusedSequences == 0,
        "preparation alone cannot seed durable audio reuse");
    require(!reuse.acknowledge(receipt), "missing publication proof cannot promote prepared graph");
    good(store.append(head, packet, head, &receipt), "publish first actual source graph");
    require(receipt && receipt->head() == head && receipt->documentIdentity().sameVersion(first.identity.document),
        "Store issues immutable receipt for its actual published head");
    AudioGraphReuseCache wrongOwner(storeRoot, juce::Uuid());
    CommitPacket other;
    good(RecoveryCheckpointCodec::encode(first, {}, other, {}, {}, &wrongOwner), "stage identical audio for a different owner");
    require(!wrongOwner.acknowledge(receipt), "foreign destination owner cannot seed references");
    good(reuse.acknowledge(receipt), "promote only matching durable source references");
    require(!reuse.acknowledge(receipt), "consumed receipt cannot acknowledge twice");
    const auto initialAssets = receipt->assetIds();
    std::vector<Snapshot> expected { first };
    const auto publish = [&] (const Snapshot& snapshot)
    {
        good(store.append(head, packet, head, &receipt), "append checkpoint with destination dependency verification");
        good(reuse.acknowledge(receipt), "acknowledge ordered graph publication");
        expected.push_back(snapshot);
    };
    require(document.moveEnvelopePoint(0, 0.0, 0.4f), "actual second envelope mutation");
    auto second = capture(document);
    good(RecoveryCheckpointCodec::encode(second, {}, packet, {}, {}, &reuse), "encode envelope with published source graph");
    require(reuse.lastCaptureWork().reusedSequences == 1 && reuse.lastCaptureWork().sourceDecodersOpened == 0
        && reuse.lastCaptureWork().sampleBytesReadOrCopied == 0 && packet.newAudio.empty()
        && !packet.existingAudio.empty(), "envelope metadata/history change reads or copies no PCM and allocates no new audio blocks");
    publish(second);
    require(receipt->assetIds() == initialAssets, "envelope checkpoint retains the same exact dependency closure");

    require(document.applySelectionGain({ 0.0, document.getDurationSeconds() }, 0.5f), "actual gain creates new immutable PCM");
    auto generated = capture(document);
    good(RecoveryCheckpointCodec::encode(generated, {}, packet, {}, {}, &reuse), "freeze only newly generated PCM");
    require(reuse.lastCaptureWork().reusedSequences == 1 && reuse.lastCaptureWork().sourceDecodersOpened == 0
        && reuse.lastCaptureWork().sampleBytesReadOrCopied == 8192 * 2 * sizeof(float)
        && !packet.newAudio.empty(), "new PCM is copied once while original Undo source is reused");
    publish(generated);
    const auto generatedAssets = receipt->assetIds();
    require(generatedAssets.size() > initialAssets.size(), "generated PCM adds independent durable dependencies");
    require(document.undo(), "actual Undo with newly generated PCM retained in Redo");
    auto undone = capture(document);
    good(RecoveryCheckpointCodec::encode(undone, {}, packet, {}, {}, &reuse), "reuse both original and generated audio through Undo");
    require(reuse.lastCaptureWork().reusedSequences == 2 && reuse.lastCaptureWork().sampleBytesReadOrCopied == 0
        && packet.newAudio.empty(), "Undo freezes no PCM while preserving Redo dependencies");
    publish(undone);
    require(document.redo(), "actual Redo");
    auto redone = capture(document);
    good(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, {}, &reuse), "reuse graph through Redo");
    require(reuse.lastCaptureWork().reusedSequences == 2 && packet.newAudio.empty(), "Redo reuses both immutable backings");
    publish(redone);

    auto alias = redone;
    auto state = std::make_shared<SingleTrackDocument::State>(*redone.state);
    state->audioBuffer = std::shared_ptr<juce::AudioBuffer<float>>(redone.state->audioBuffer.get(), [] (auto*) {});
    alias.state = state;
    good(RecoveryCheckpointCodec::encode(alias, {}, other, {}, {}, &reuse), "capture same address with a foreign shared owner");
    require(reuse.lastCaptureWork().sampleBytesReadOrCopied == 8192 * 2 * sizeof(float),
        "pointer address equality cannot bypass weak control-block identity");
    auto preserved = packet;
    require(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, [] { return true; }, &reuse).error == Error::cancelled
        && packet.stateJson == preserved.stateJson && !reuse.acknowledge(receipt), "cancel invalidates pending acknowledgement and preserves caller output");
    auto tight = AudioGraphLimits {}; tight.chunkSampleBytes = 256;
    require(RecoveryCheckpointCodec::encode(redone, {}, packet, tight, {}, &reuse).error == Error::budgetExceeded
        && packet.stateJson == preserved.stateJson, "cached references cannot bypass smaller chunk budgets");
    good(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, {}, &reuse), "old published cache survives failed/cancelled preparation");
    require(reuse.lastCaptureWork().sampleBytesReadOrCopied == 0, "failed preparation does not discard the last durable reuse table");

    good(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, {}, &reuse), "prepare cached references before destination corruption");
    const auto asset = storeRoot.getChildFile(owner.toString()).getChildFile("assets").getChildFile(*initialAssets.begin());
    juce::MemoryBlock saved;
    require(asset.loadFileAsData(saved) && asset.replaceWithText("owned damaged frame"), "corrupt only one owned CAS frame");
    const auto lastReceipt = receipt;
    const auto safe = head;
    require(!store.append(head, packet, head, &receipt) && head == safe && receipt == lastReceipt,
        "cached reference still verifies destination CAS; failed append preserves head and receipt");
    good(store.checkHead(safe), "corrupt dependency cannot advance last safe head");
    require(asset.replaceWithData(saved.getData(), saved.getSize()), "restore exact private CAS bytes after fault");
    publish(redone);

    const auto view = store.inspect(); good(view.status, "fully verify published reuse chain");
    require(view.commits.size() == expected.size(), "all real versions remain separately addressable");
    for (size_t i = 0; i < expected.size(); ++i)
    {
        Snapshot decoded;
        good(RecoveryCheckpointCodec::decode(view.commits[i], store, decoded), "restore reused current/Undo/Redo checkpoint");
        require(decoded.identity.document.sameVersion(expected[i].identity.document)
            && decoded.undo.size() == expected[i].undo.size() && decoded.redo.size() == expected[i].redo.size(), "exact version and history restored");
        const auto compare = [&] (const auto& actual, const auto& wanted)
        {
            require(actual->envelopePoints.size() == wanted->envelopePoints.size(), "reuse retains all envelope points");
            for (size_t point = 0; point < actual->envelopePoints.size(); ++point)
                require(actual->envelopePoints[point].gain == wanted->envelopePoints[point].gain
                    && actual->envelopePoints[point].timeSeconds == wanted->envelopePoints[point].timeSeconds, "reuse preserves exact envelope values");
            auto pcm = wanted->audioBuffer;
            if (!pcm || pcm->getNumSamples() == 0)
            {
                auto reader = SingleTrackDocument::createReaderForFile(source);
                pcm = std::make_shared<juce::AudioBuffer<float>>(2, 8192);
                require(reader && reader->read(pcm.get(), 0, 8192, 0, true, true), "decode independent source oracle");
            }
            for (int c = 0; c < 2; ++c)
                require(std::memcmp(actual->audioBuffer->getReadPointer(c), pcm->getReadPointer(c), 8192 * sizeof(float)) == 0,
                    "all reused or newly generated current/history PCM is bit-exact");
        };
        compare(decoded.state, expected[i].state);
        for (size_t j = 0; j < decoded.undo.size(); ++j) compare(decoded.undo[j], expected[i].undo[j]);
        for (size_t j = 0; j < decoded.redo.size(); ++j) compare(decoded.redo[j], expected[i].redo[j]);
    }
    {
        const juce::Uuid incompleteOwner;
        RecoveryStore incomplete(storeRoot, incompleteOwner);
        AudioGraphReuseCache guarded(storeRoot, incompleteOwner);
        CommitPacket missing;
        good(RecoveryCheckpointCodec::encode(first, {}, missing, {}, {}, &guarded), "stage complete graph for receipt closure test");
        missing.newAudio.clear(); missing.existingAudio.clear();
        SafeHead incompleteHead;
        std::shared_ptr<const PublishedRecoveryCommit> incompleteReceipt;
        good(incomplete.append({}, missing, incompleteHead, &incompleteReceipt), "generic Store publishes a record with no declared audio dependencies");
        require(guarded.acknowledge(incompleteReceipt).code == "reuse_publication_assets",
            "matching state text and owner alone cannot certify omitted audio dependencies");
    }
    // Keep source mutation last: renaming it back restores FileId but changes
    // ChangeTime, so it must not make the original witness valid again.
    const auto original = source.getSiblingFile("reuse-original-held.wav");
    preserved = packet;
    require(source.moveFileTo(original) && original.copyFileTo(source), "replace only private source with identical bytes at a new file identity");
    require(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, {}, &reuse).error == Error::sourceConflict
        && packet.stateJson == preserved.stateJson, "cached source PCM cannot hide physical source replacement");
    require(source.deleteFile() && original.moveFileTo(source), "restore only owned original source path");
    require(RecoveryCheckpointCodec::encode(redone, {}, packet, {}, {}, &reuse).error == Error::sourceConflict,
        "restoring the name and FileId does not erase the source ChangeTime fence");
    const std::weak_ptr<juce::AudioBuffer<float>> generatedLifetime = generated.state->audioBuffer;
    state.reset(); alias = {}; expected.clear(); first = {}; second = {}; generated = {}; undone = {}; redone = {};
    document.clear();
    require(generatedLifetime.expired(), "cache holds weak backing identities, not PCM/Document/reader lifetimes");
    std::cout << "RecoveryGraphReuseTests passed (real publication, zero repeat PCM reads, new PCM, history, faults, weak ownership)\n";
}
