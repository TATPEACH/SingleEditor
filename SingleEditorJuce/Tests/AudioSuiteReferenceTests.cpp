#include "Audio/AudioEngine.h"
#include "AudioSuite/AudioSuiteReferenceProcessor.h"
#include "Document/SingleTrackDocument.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

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

bool near(float actual, float expected, float tolerance = 0.002f)
{
    return std::abs(actual - expected) <= tolerance;
}

bool writeStereoFixture(const juce::File& file, int numSamples = 4800, double sampleRate = 48000.0)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(2)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        buffer.setSample(0, sample, 0.5f);
        buffer.setSample(1, sample, 0.25f);
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

struct DocumentFixture
{
    explicit DocumentFixture(double sampleRate = 48000.0)
    {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("SingleEditorAudioSuiteTests")
                   .getNonexistentChildFile("reference", {});
        root.createDirectory();
        source = root.getChildFile("source.wav");
        require(writeStereoFixture(source, static_cast<int>(sampleRate * 0.1), sampleRate),
                "writes stereo AudioSuite fixture");
        require(document.loadFile(source), "loads stereo AudioSuite fixture");
    }

    ~DocumentFixture()
    {
        root.deleteRecursively();
    }

    juce::File root;
    juce::File source;
    SingleTrackDocument document;
};

AudioSelectionRequest channelOneSelection()
{
    AudioSelectionRequest request;
    request.timeRangeSeconds = juce::Range<double>(0.02, 0.06);
    request.sourceChannels = {
        SourceChannelIdentity::fromSourceIndex(1),
        SourceChannelIdentity::fromSourceIndex(1)
    };
    return request;
}

void runLazyCapabilityTest()
{
    require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "ordinary state owns no reference processor");
    const auto descriptor = AudioSuiteReferenceProcessor::describe();
    require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "cold descriptor does not allocate a processor");
    {
        auto processor = descriptor.create();
        require(processor != nullptr, "descriptor creates isolated processor");
        require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 1,
                "explicit demand owns one processor");
    }
    require(AudioSuiteReferenceProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "processor releases after explicit demand ends");
}

void runSelectionAndPlanTest()
{
    DocumentFixture fixture;
    const auto state = fixture.document.getState();
    auto processor = AudioSuiteReferenceProcessor::describe().create();

    AudioSelectionRequest wholeFile;
    const auto resolvedWhole = AudioSuiteReferenceProcessor::resolveSelection(wholeFile, *state);
    require(resolvedWhole.isValid(), "empty gesture resolves to a valid selection");
    require(resolvedWhole.wholeDuration, "empty time gesture resolves whole duration");
    require(resolvedWhole.allSourceChannels, "empty channel gesture resolves all source channels");
    require(resolvedWhole.sampleRange == juce::Range<int64_t>(0, 4800),
            "whole-file selection preserves exact sample bounds");

    AudioSelectionRequest wholeChannel;
    wholeChannel.sourceChannels = { SourceChannelIdentity::fromSourceIndex(0) };
    const auto resolvedChannel = AudioSuiteReferenceProcessor::resolveSelection(wholeChannel, *state);
    require(resolvedChannel.wholeDuration, "lane gesture selects the whole channel duration");
    require(!resolvedChannel.allSourceChannels, "lane gesture retains a source-channel subset");
    require(resolvedChannel.containsSourceChannel(0)
                && !resolvedChannel.containsSourceChannel(1),
            "source identities never become output-device roles");

    const auto analyzed = processor->analyze(state,
                                             77,
                                             channelOneSelection(),
                                             0.5f,
                                             0.01);
    require(analyzed.succeeded(), "Analyze produces an immutable plan");
    require(analyzed.plan->getProcessorId() == "singleeditor.reference-gain",
            "plan retains processor identity");
    require(analyzed.plan->getProcessorVersion() == "1.0.0",
            "plan retains processor version");
    require(analyzed.plan->getDocumentGeneration() == 77,
            "plan binds the document generation");
    require(analyzed.plan->getAudioRevision() == state->audioRevision,
            "plan binds the audio revision");
    require(analyzed.plan->getChangeGeneration() == state->changeGeneration,
            "plan binds the document change generation");
    require(analyzed.plan->getSourceIdentity().matchesCurrentFile(),
            "plan binds a live source identity");
    require(analyzed.plan->getSelection().sampleRange == juce::Range<int64_t>(960, 2880),
            "Analyze canonicalizes the time range into samples");
    require(analyzed.plan->getSelection().sourceChannels.size() == 1,
            "Analyze canonicalizes duplicate channel identities");
    require(analyzed.plan->getSelection().sourceChannels.front().stableId
                == "source-channel:1",
            "Analyze emits stable source-channel identity");
    require(analyzed.plan->getPreviewRange().getLength() == 480,
            "Preview is bounded independently of the full Apply selection");
    require(analyzed.plan->getAnalysisReadRange().getStart()
                < analyzed.plan->getSelection().sampleRange.getStart(),
            "Analyze includes bounded hidden pre-roll");
}

void runPreviewAndCancellationTest()
{
    DocumentFixture fixture;
    auto processor = AudioSuiteReferenceProcessor::describe().create();
    const auto stateBeforePreview = fixture.document.getState();
    const auto analyzed = processor->analyze(stateBeforePreview,
                                             19,
                                             channelOneSelection(),
                                             0.5f,
                                             1.0);
    require(analyzed.succeeded(), "Analyze succeeds before Preview");
    auto preview = processor->createPreview(analyzed.plan);
    require(preview.isValid(), "Preview creates a cancellable audition handle");

    AudioEngine engine(fixture.document);
    require(!engine.getRuntimeMetricsForDiagnostics().has_value(),
            "runtime diagnostics do not initialise a device for an offline engine");
    engine.setAudioSuitePreview(preview.getState());
    juce::AudioBuffer<float> audition(2, 128);
    require(engine.renderDocumentBlockForDiagnostics(1500, audition),
            "main document playback path renders Preview");
    require(!engine.getRuntimeMetricsForDiagnostics().has_value(),
            "offline Preview and repeated metrics reads stay independent of hardware startup");
    require(near(audition.getSample(0, 64), 0.5f),
            "Preview leaves an untargeted source channel unchanged");
    require(near(audition.getSample(1, 64), 0.125f),
            "Preview runs the reference kernel on the targeted source channel");
    require(fixture.document.getState() == stateBeforePreview,
            "Preview never publishes document state");
    require(!fixture.document.hasUnsavedChanges(),
            "Preview never dirties the document");

    preview.cancel();
    audition.clear();
    require(engine.renderDocumentBlockForDiagnostics(1500, audition),
            "main playback continues after Preview cancellation");
    require(near(audition.getSample(1, 64), 0.25f),
            "cancelled Preview immediately returns to source truth");
    engine.clearAudioSuitePreview();
}

void runResampledPreviewLoopTest()
{
    for (const auto rates : { std::pair { 48000.0, 48000.0 }, std::pair { 96000.0, 48000.0 },
                              std::pair { 48000.0, 96000.0 }, std::pair { 44100.0, 48000.0 } })
    {
        DocumentFixture fixture(rates.first);
        const auto state = fixture.document.getState();
        auto processor = AudioSuiteReferenceProcessor::describe().create();
        const auto analyzed = processor->analyze(state, 37, channelOneSelection(), 0.5f, 1.0);
        require(analyzed.succeeded(), "Analyze succeeds for resampled transport Preview");
        auto preview = processor->createPreview(analyzed.plan);
        AudioEngine engine(fixture.document);
        engine.setAudioSuitePreview(preview.getState());
        juce::AudioBuffer<float> block(2, 256);

        require(engine.renderTransportBlockForDiagnostics(static_cast<int64_t>(rates.first * 0.04),
                                                           block, rates.second),
                "real transport renders with independent source and output rates");
        require(engine.getCurrentPositionSeconds() > 0.04
                    && engine.getCurrentPositionSeconds() < 0.06,
                "resampled Preview does not loop before the source selection ends");

        const auto nearEnd = static_cast<int64_t>(rates.first * 0.0595);
        require(engine.renderTransportBlockForDiagnostics(nearEnd, block, rates.second),
                "real transport crosses the Preview end");
        require(std::abs(engine.getCurrentPositionSeconds() - 0.02) <= 1.0 / rates.first,
                "resampled Preview loops to the source selection start at its end");

        preview.cancel();
        require(engine.renderTransportBlockForDiagnostics(nearEnd, block, rates.second),
                "cancelled Preview keeps rendering through the transport");
        require(engine.getCurrentPositionSeconds() > 0.06,
                "cancelled Preview no longer constrains transport position");
        require(fixture.document.getState() == state && !fixture.document.hasUnsavedChanges(),
                "resampled audition leaves document and dirty state untouched");
    }
}

void runStreamingApplyUndoAndSaveTest()
{
    DocumentFixture fixture;
    auto processor = AudioSuiteReferenceProcessor::describe().create();
    const auto sourceState = fixture.document.getState();
    const auto analyzed = processor->analyze(sourceState,
                                             31,
                                             channelOneSelection(),
                                             0.5f,
                                             1.0);
    require(analyzed.succeeded(), "Analyze succeeds before Apply");

    const auto applied = processor->apply(fixture.document,
                                          analyzed.plan,
                                          31,
                                          {},
                                          257);
    require(applied.succeeded(), "Apply commits the reference processor");
    require(applied.renderedBlocks > 1 && applied.renderedSamples == 4800,
            "Apply streams bounded blocks through the mutation transaction");

    const auto appliedState = fixture.document.getState();
    require(appliedState != sourceState, "Apply atomically publishes a new document state");
    require(appliedState->audioRevision != sourceState->audioRevision,
            "Apply publishes new waveform and playback truth revision");
    require(appliedState->audioBuffer != nullptr
                && appliedState->memoryMappedReader == nullptr
                && appliedState->streamingReader == nullptr,
            "Apply publishes an immediately playable in-memory source without reader reload");
    require(fixture.document.hasUnsavedChanges(), "Apply dirties the document");
    require(fixture.document.canUndo(), "Apply adds exactly one undoable document mutation");
    require(near(appliedState->audioBuffer->getSample(0, 1500), 0.5f),
            "Apply preserves an untargeted channel");
    require(near(appliedState->audioBuffer->getSample(1, 1500), 0.125f),
            "Apply uses the same targeted-channel kernel as Preview");
    require(near(appliedState->audioBuffer->getSample(1, 960), 0.25f),
            "Apply writes only the exact selection with a boundary crossfade");

    require(fixture.document.saveOverSourceFile(),
            "Ctrl+S-equivalent save works immediately after Apply");
    require(fixture.document.getState() == appliedState,
            "save does not reload or replace the applied playback state");
    require(!fixture.document.hasUnsavedChanges(),
            "saved applied generation becomes clean");

    require(fixture.document.undo(), "Undo works immediately after Apply and Save");
    const auto undoState = fixture.document.getState();
    require(undoState->audioBuffer != nullptr
                && undoState->memoryMappedReader == nullptr
                && undoState->streamingReader == nullptr,
            "Undo source remains stable after the source file was overwritten");
    require(near(undoState->audioBuffer->getSample(1, 1500), 0.25f),
            "Undo restores original samples without stale mapped-reader cache");
    require(fixture.document.redo(), "Redo restores the applied transaction");
    require(near(fixture.document.getState()->audioBuffer->getSample(1, 1500), 0.125f),
            "Redo restores the exact applied audio without silence or noise");
}

void runStaleAndCancelledApplyTest()
{
    DocumentFixture fixture;
    auto processor = AudioSuiteReferenceProcessor::describe().create();
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             91,
                                             channelOneSelection(),
                                             0.5f);
    require(analyzed.succeeded(), "Analyze succeeds for fencing test");

    AudioSuiteCancellationToken cancelled;
    cancelled.cancel();
    const auto stateBeforeCancelledApply = fixture.document.getState();
    const auto cancelledResult = processor->apply(fixture.document,
                                                   analyzed.plan,
                                                   91,
                                                   cancelled);
    require(cancelledResult.status == AudioSuiteReferenceProcessor::ApplyStatus::cancelled,
            "cancelled Apply never begins publication");
    require(fixture.document.getState() == stateBeforeCancelledApply,
            "cancelled Apply preserves source state");

    require(fixture.document.applySelectionGain({ 0.0, 0.01 }, 0.9f),
            "creates a newer document mutation");
    const auto newerState = fixture.document.getState();
    const auto staleResult = processor->apply(fixture.document,
                                               analyzed.plan,
                                               91);
    require(staleResult.status == AudioSuiteReferenceProcessor::ApplyStatus::stalePlan,
            "Apply rejects a plan after document revision changes");
    require(fixture.document.getState() == newerState,
            "stale Apply cannot overwrite the newer document state");
    require(processor->apply(fixture.document, analyzed.plan, 92).status
                == AudioSuiteReferenceProcessor::ApplyStatus::stalePlan,
            "Apply rejects a mismatched document generation");
}
} // namespace

void runWaveformEditCacheTests();
void runAudioDeviceWarmupTests();

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    runLazyCapabilityTest();
    runSelectionAndPlanTest();
    runPreviewAndCancellationTest();
    runResampledPreviewLoopTest();
    runStreamingApplyUndoAndSaveTest();
    runStaleAndCancelledApplyTest();
    runWaveformEditCacheTests();
    runAudioDeviceWarmupTests();
    std::cout << "AudioSuite reference tests passed\n";
    return 0;
}
