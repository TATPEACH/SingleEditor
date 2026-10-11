#include "Audio/AudioEngine.h"
#include "AudioSuite/TruePeakLimiterProcessor.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Components/TruePeakLimiterMagComponent.h"
#include "UI/Renderer/RendererSelection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>

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

bool near(double actual, double expected, double tolerance = 1.0e-4)
{
    return std::abs(actual - expected) <= tolerance;
}

bool writeFixture(const juce::File& file, int numSamples = 8192)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;
    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(2)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto phase = 2.0f * juce::MathConstants<float>::pi
                         * 997.0f * static_cast<float>(sample) / 48000.0f;
        buffer.setSample(0, sample, 0.92f * std::sin(phase));
        buffer.setSample(1, sample, 0.35f * std::sin(phase + 0.3f));
    }
    buffer.setSample(0, 3071, -0.9f);
    buffer.setSample(0, 3072, 0.9f);
    buffer.setSample(0, 3073, 0.9f);
    buffer.setSample(0, 3074, -0.9f);
    buffer.setSample(1, 3072, 0.25f);
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

struct DocumentFixture
{
    DocumentFixture()
    {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("SingleEditorTruePeakLimiterTests")
                   .getNonexistentChildFile("limiter", {});
        root.createDirectory();
        source = root.getChildFile("source.wav");
        require(writeFixture(source), "writes limiter fixture");
        require(document.loadFile(source), "loads limiter fixture");
    }

    ~DocumentFixture() { root.deleteRecursively(); }

    juce::File root;
    juce::File source;
    SingleTrackDocument document;
};

TruePeakLimiterParameters enabledParameters()
{
    auto parameters = TruePeakLimiterParameters::masteringDefault();
    parameters.enabled = true;
    parameters.thresholdDb = -6.0;
    parameters.ceilingDbtp = -1.0;
    parameters.releaseMode = TruePeakLimiterReleaseMode::manual;
    parameters.releaseMs = 120.0;
    return parameters;
}

float readDocumentSample(SingleTrackDocument& document,
                         int channel,
                         int64_t sample)
{
    AudioEngine engine(document);
    const auto state = document.getState();
    juce::AudioBuffer<float> block(state->info.numChannels, 1);
    require(engine.renderDocumentBlockForDiagnostics(sample, block),
            "document reader supplies a diagnostic sample");
    return block.getSample(channel, 0);
}

void runLazyLockedSchemaTest()
{
    require(TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "ordinary startup owns no limiter processor");
    const auto descriptor = TruePeakLimiterProcessor::describe();
    require(TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "cold limiter descriptor performs no processor allocation");

    const auto ids = TruePeakLimiterProcessor::automationSafeParameterIds();
    require(ids.size() == 8, "locked mastering schema exposes eight stable IDs");
    const std::set<juce::String> unique(ids.begin(), ids.end());
    require(unique.size() == ids.size()
                && unique.contains("limiter.threshold-db")
                && unique.contains("limiter.lookahead-ms-fixed")
                && unique.contains("limiter.channels-linked-fixed"),
            "limiter parameter IDs preserve locked semantics");
    require(TruePeakLimiterProcessor::oversamplingFactorForSampleRate(48000.0) == 4
                && TruePeakLimiterProcessor::oversamplingFactorForSampleRate(96000.0) == 4
                && TruePeakLimiterProcessor::oversamplingFactorForSampleRate(192000.0) == 2
                && TruePeakLimiterProcessor::oversamplingFactorForSampleRate(384000.0) == 1,
            "detector factor is fixed and sample-rate aware");

    {
        auto processor = descriptor.create();
        require(processor != nullptr
                    && TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 1,
                "explicit demand creates exactly one limiter processor");
    }
    require(TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "limiter processor releases after demand ends");
}

void runMagTruthTest()
{
    require(TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "Limiter Mag test starts without a processor");
    TruePeakLimiterMagComponent mag(TruePeakLimiterParameters::masteringDefault());
    mag.setSize(520, 320);
    mag.applyWorkspaceVisualContract(RendererDensity::standard,
                                     RendererMagSize::medium,
                                     RendererInformationTier::standard,
                                     true);
    require(!mag.getParameters().enabled
                && !mag.isFeedbackVisibleForTesting()
                && TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "cold Mag is Off, feedback-free and processor-free");

    auto emittedEnabled = false;
    mag.onParametersChanged = [&emittedEnabled](const auto& parameters)
    {
        emittedEnabled = parameters.enabled;
    };
    mag.setEnabledForTesting(true);
    require(emittedEnabled && mag.getParameters().enabled,
            "explicit On gesture emits the locked limiter model");

    TruePeakLimiterFeedbackSnapshot snapshot;
    snapshot.inputTruePeakDbtp = 1.4;
    snapshot.maximumGainReductionDb = 4.2;
    snapshot.processedSamples = 2048;
    mag.setFeedback(snapshot);
    require(mag.isFeedbackVisibleForTesting()
                && mag.getStatusTextForTesting().contains("TP 1.4 dBTP")
                && mag.getStatusTextForTesting().contains("GR 4.2 dB"),
            "requested processing exposes truthful TP and GR even when only the compact status fits");
    mag.clearFeedback();
    require(!mag.isFeedbackVisibleForTesting(),
            "feedback disappears when processing is no longer requested");

    mag.setBounds(0, 0, 320, 70);
    mag.applyWorkspaceVisualContract(RendererDensity::compact,
                                     RendererMagSize::small,
                                     RendererInformationTier::summary,
                                     true);
    require(mag.areSizeControlsUsableForTesting(),
            "Compact limiter keeps all S/M/L controls visible and inside the Mag so it can be expanded again");

    RendererSelection::resetForTests(RendererKind::modern);
    for (const int width : { 180, 240, 360, 520 })
        for (const int height : { 180, 240, 320, 500 })
            for (auto tier : { RendererInformationTier::standard, RendererInformationTier::detail })
            {
                mag.setSize(width, height);
                mag.applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::medium, tier, true);
                require(mag.areSizeControlsUsableForTesting(), "Modern limiter keeps S/M/L at every tested size");
                for (const auto* id : { "limiter.threshold-db", "limiter.ceiling-dbtp", "limiter.preview",
                                       "limiter.apply", "limiter.recall-last-applied" })
                {
                    auto* child = mag.findChildWithID(id);
                    require(child != nullptr && child->isVisible() && child->getWidth() >= 40
                                && child->getHeight() >= 22 && mag.getLocalBounds().contains(child->getBounds()),
                            "Modern limiter parameters and actions remain usable inside the compact tile");
                }
                for (int i = 0; i < mag.getNumChildComponents(); ++i)
                {
                    const auto* child = mag.getChildComponent(i);
                    require(!child->isVisible() || !mag.getTransferCurveBounds().intersects(child->getBounds()),
                            "Static transfer plot never covers a live control");
                    for (int j = i + 1; j < mag.getNumChildComponents(); ++j)
                    {
                        const auto* first = mag.getChildComponent(i);
                        const auto* second = mag.getChildComponent(j);
                        require(!first->isVisible() || !second->isVisible()
                                    || !first->getBounds().intersects(second->getBounds()),
                                "Modern limiter visible controls never overlap");
                    }
                }
            }
    require(TruePeakLimiterProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "Responsive limiter layout never creates audio processing");
    auto target = TruePeakLimiterParameters::masteringDefault();
    require(near(TruePeakLimiterMagComponent::nominalOutputDb(target, -18.0), -18.0),
            "Off transfer target is unity");
    target.enabled = true;
    require(near(TruePeakLimiterMagComponent::nominalOutputDb(target, -18.0), -12.0)
                && near(TruePeakLimiterMagComponent::nominalOutputDb(target, -7.0), -1.0)
                && near(TruePeakLimiterMagComponent::nominalOutputDb(target, 0.0), -1.0),
            "Static public target shows input gain then ceiling, without implying measured true peak");
    RendererSelection::resetForTests(RendererKind::classic);
}

void runValidationAndIntersampleTest()
{
    DocumentFixture fixture;
    auto processor = TruePeakLimiterProcessor::describe().create();
    auto off = TruePeakLimiterParameters::masteringDefault();
    require(!processor->analyze(fixture.document.getState(), 20, {}, off).succeeded(),
            "Off performs zero audio processing");

    auto invalid = enabledParameters();
    invalid.lookAheadMs = 4.0;
    require(!processor->analyze(fixture.document.getState(), 20, {}, invalid).succeeded(),
            "lookahead cannot drift from the locked 5 ms profile");
    invalid = enabledParameters();
    invalid.channelsLinked = false;
    require(!processor->analyze(fixture.document.getState(), 20, {}, invalid).succeeded(),
            "channel unlinking is not a hidden v1 mode");

    const float intersample[] { 0.9f, 0.9f, 0.9f, -0.9f };
    const auto samplePeak = 0.9;
    const auto estimated = TruePeakLimiterProcessor::estimateTruePeakLinearForDiagnostics(
        intersample, 4, 4);
    require(estimated > samplePeak + 0.05,
            "oversampled detector finds a peak between stored samples");
}

void runLinkedLookAheadTransientTest()
{
    DocumentFixture fixture;
    auto processor = TruePeakLimiterProcessor::describe().create();
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             31,
                                             {},
                                             enabledParameters());
    require(analyzed.succeeded(), "limiter Analyze creates an immutable plan");
    require(analyzed.plan->getOversamplingFactor() == 4
                && analyzed.plan->getLookAheadSamples() == 240
                && near(analyzed.plan->getGuardedCeilingDbtp(), -1.1),
            "plan freezes factor, five millisecond lookahead and internal guard");

    const auto validSamples = 2048;
    const auto lookAhead = analyzed.plan->getLookAheadSamples();
    juce::AudioBuffer<float> buffer(2, validSamples + lookAhead + 2);
    buffer.clear();
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        buffer.setSample(0, sample, 0.25f);
        buffer.setSample(1, sample, 0.25f);
    }
    const auto transient = 1000;
    buffer.setSample(0, transient, 1.0f);
    require(TruePeakLimiterProcessor::renderPlanForDiagnostics(
                *analyzed.plan,
                buffer,
                validSamples,
                0,
                { 0, validSamples }),
            "bounded lookahead render completes");

    const auto ratioLeft = buffer.getSample(0, transient) / 1.0f;
    const auto ratioRight = buffer.getSample(1, transient) / 0.25f;
    require(near(ratioLeft, ratioRight, 0.002),
            "one linked envelope preserves multichannel image at a transient");
    require(buffer.getSample(1, transient - lookAhead / 2) < 0.35f,
            "gain reduction begins before the transient without shifting audio");

    const auto feedback = analyzed.plan->getFeedbackSnapshot();
    require(feedback.processedSamples == validSamples
                && feedback.maximumGainReductionDb > 3.0
                && feedback.inputTruePeakDbtp > 5.0,
            "TP and gain-reduction feedback comes from requested processing");

    const auto outputPeak = TruePeakLimiterProcessor::estimateTruePeakLinearForDiagnostics(
        buffer.getReadPointer(0), validSamples, 4);
    require(20.0 * std::log10(outputPeak) <= -0.98,
            "guarded render remains below the public true-peak ceiling");
}

void runSilenceAndStressTest()
{
    DocumentFixture fixture;
    auto processor = TruePeakLimiterProcessor::describe().create();
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             42,
                                             {},
                                             enabledParameters());
    require(analyzed.succeeded(), "limiter plan resolves for silence and stress evidence");

    const auto validSamples = 1024;
    juce::AudioBuffer<float> silence(2, validSamples + analyzed.plan->getLookAheadSamples() + 2);
    silence.clear();
    require(TruePeakLimiterProcessor::renderPlanForDiagnostics(
                *analyzed.plan, silence, validSamples, 0, { 0, validSamples }),
            "silence render completes");
    for (int channel = 0; channel < silence.getNumChannels(); ++channel)
        for (int sample = 0; sample < validSamples; ++sample)
            require(silence.getSample(channel, sample) == 0.0f,
                    "silence remains exact silence");

    juce::AudioBuffer<float> hot(2, validSamples + analyzed.plan->getLookAheadSamples() + 2);
    for (int channel = 0; channel < hot.getNumChannels(); ++channel)
        for (int sample = 0; sample < hot.getNumSamples(); ++sample)
            hot.setSample(channel, sample, channel == 0 ? 4.0f : -3.0f);
    require(TruePeakLimiterProcessor::renderPlanForDiagnostics(
                *analyzed.plan, hot, validSamples, 0, { 0, validSamples }),
            "over-range render completes");
    for (int channel = 0; channel < hot.getNumChannels(); ++channel)
        for (int sample = 0; sample < validSamples; ++sample)
            require(std::isfinite(hot.getSample(channel, sample))
                        && std::abs(hot.getSample(channel, sample)) < 1.0f,
                    "stress render cannot create clipping, NaN or infinity");
}

void runPreviewApplyVerificationUndoSaveTest()
{
    DocumentFixture fixture;
    auto processor = TruePeakLimiterProcessor::describe().create();
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             77,
                                             {},
                                             enabledParameters());
    require(analyzed.succeeded(), "whole-document limiter plan resolves");

    auto preview = processor->createPreview(analyzed.plan);
    require(preview.isValid()
                && preview.getState()->getRequiredLookAheadSamples() == 242,
            "Preview exposes the same bounded detector lookahead");
    AudioEngine engine(fixture.document);
    engine.setAudioSuitePreview(preview.getState());
    juce::AudioBuffer<float> audition(2, 4096);
    require(engine.renderDocumentBlockForDiagnostics(0, audition),
            "main playback reader supplies bounded future samples to Preview");
    const auto previewSample = audition.getSample(0, 1000);
    engine.clearAudioSuitePreview();

    const auto sourceState = fixture.document.getState();
    const auto applied = processor->apply(fixture.document,
                                          analyzed.plan,
                                          77,
                                          {},
                                          512);
    require(applied.succeeded() && applied.renderedBlocks > 1,
            "Limiter Apply reuses the streaming shared transaction");
    require(applied.verification.performed
                && applied.verification.passed
                && applied.verification.verifiedChannels == 2
                && applied.verification.verifiedSamples == sourceState->info.numSamples
                && applied.verification.nonFiniteSamples == 0
                && applied.verification.clippedSamples == 0
                && applied.verification.maximumTruePeakDbtp <= -0.98,
            "independent second pass verifies TP, clipping, finite output and shape");

    const auto appliedState = fixture.document.getState();
    require(near(appliedState->audioBuffer->getSample(0, 1000),
                 previewSample,
                 0.003),
            "Preview and Apply use the same linked lookahead kernel");
    require(appliedState->audioRevision != sourceState->audioRevision
                && fixture.document.hasUnsavedChanges()
                && fixture.document.canUndo(),
            "verified Apply atomically publishes waveform, dirty and Undo truth");
    require(fixture.document.saveOverSourceFile(),
            "Save works immediately after verified limiter Apply");
    require(fixture.document.undo(),
            "Undo works after limiter Apply and save-over-source");
    require(fixture.document.getState()->audioBuffer != nullptr,
            "Undo remains playable without reader reload");

    const auto freshPlan = processor->analyze(fixture.document.getState(),
                                              78,
                                              {},
                                              enabledParameters());
    require(freshPlan.succeeded(), "fresh plan resolves after Undo");
    AudioSuiteCancellationToken cancelled;
    cancelled.cancel();
    const auto beforeCancel = fixture.document.getState();
    const auto cancelledResult = processor->apply(fixture.document,
                                                   freshPlan.plan,
                                                   78,
                                                   cancelled,
                                                   512);
    require(cancelledResult.status == AudioSuiteApplyStatus::cancelled
                && fixture.document.getState() == beforeCancel,
            "cancellation cannot publish or verify partial audio");
}

void runBlockBoundaryAndLocalSelectionTest()
{
    DocumentFixture smallBlocks;
    DocumentFixture largeBlocks;
    auto smallProcessor = TruePeakLimiterProcessor::describe().create();
    auto largeProcessor = TruePeakLimiterProcessor::describe().create();
    const auto smallPlan = smallProcessor->analyze(smallBlocks.document.getState(),
                                                   91,
                                                   {},
                                                   enabledParameters());
    const auto largePlan = largeProcessor->analyze(largeBlocks.document.getState(),
                                                   91,
                                                   {},
                                                   enabledParameters());
    require(smallPlan.succeeded() && largePlan.succeeded(),
            "block-boundary comparison plans resolve");
    const auto smallResult = smallProcessor->apply(smallBlocks.document,
                                                   smallPlan.plan,
                                                   91,
                                                   {},
                                                   64);
    const auto largeResult = largeProcessor->apply(largeBlocks.document,
                                                   largePlan.plan,
                                                   91,
                                                   {},
                                                   4096);
    require(smallResult.succeeded() && largeResult.succeeded()
                && smallResult.verification.passed
                && largeResult.verification.passed,
            "small and large streaming blocks both verify before publication");
    require(near(smallResult.verification.maximumTruePeakDbtp,
                 largeResult.verification.maximumTruePeakDbtp,
                 1.0e-4),
            "verification retains the real previous sample across block boundaries");

    const auto smallState = smallBlocks.document.getState();
    const auto largeState = largeBlocks.document.getState();
    auto maximumDifference = 0.0;
    for (int channel = 0; channel < smallState->audioBuffer->getNumChannels(); ++channel)
    {
        for (int sample = 0; sample < smallState->audioBuffer->getNumSamples(); ++sample)
        {
            maximumDifference = std::max(
                maximumDifference,
                std::abs(static_cast<double>(
                    smallState->audioBuffer->getSample(channel, sample)
                    - largeState->audioBuffer->getSample(channel, sample))));
        }
    }
    require(maximumDifference <= 1.0e-6,
            "bounded streaming output is invariant to render block size");

    DocumentFixture localFixture;
    auto localProcessor = TruePeakLimiterProcessor::describe().create();
    AudioSelectionRequest localSelection;
    localSelection.timeRangeSeconds = {
        3072.0 / 48000.0,
        5000.0 / 48000.0
    };
    const auto localPlan = localProcessor->analyze(localFixture.document.getState(),
                                                   92,
                                                   localSelection,
                                                   enabledParameters());
    require(localPlan.succeeded(), "loud-boundary local selection resolves");
    const auto selectionRange = localPlan.plan->getAudioSuitePlan()->getSelection().sampleRange;
    const auto beforeBoundary = readDocumentSample(localFixture.document,
                                                   0,
                                                   selectionRange.getStart());
    const auto beforeOutsideLeft = readDocumentSample(localFixture.document,
                                                      0,
                                                      selectionRange.getStart() - 1);
    const auto beforeOutsideRight = readDocumentSample(localFixture.document,
                                                       0,
                                                       selectionRange.getEnd());
    const auto localResult = localProcessor->apply(localFixture.document,
                                                   localPlan.plan,
                                                   92,
                                                   {},
                                                   64);
    require(localResult.succeeded()
                && localResult.verification.passed
                && localResult.verification.maximumTruePeakDbtp <= -0.98,
            "fully wet local selection preserves the true-peak ceiling");
    require(near(readDocumentSample(localFixture.document,
                                    0,
                                    selectionRange.getStart() - 1),
                 beforeOutsideLeft,
                 1.0e-7)
                && near(readDocumentSample(localFixture.document,
                                           0,
                                           selectionRange.getEnd()),
                        beforeOutsideRight,
                        1.0e-7),
            "local limiter Apply leaves samples outside the selection untouched");
    require(std::abs(readDocumentSample(localFixture.document,
                                        0,
                                        selectionRange.getStart()))
                < std::abs(beforeBoundary),
            "unsafe dry boundary is attenuated instead of falsely passing verification");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    runLazyLockedSchemaTest();
    runMagTruthTest();
    runValidationAndIntersampleTest();
    runLinkedLookAheadTransientTest();
    runSilenceAndStressTest();
    runPreviewApplyVerificationUndoSaveTest();
    runBlockBoundaryAndLocalSelectionTest();
    std::cout << "True-Peak Limiter tests passed\n";
    return 0;
}
