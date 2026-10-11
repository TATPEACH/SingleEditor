#include "Audio/AudioEngine.h"
#include "AudioSuite/ParametricEqProcessor.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Components/ParametricEqMagComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

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

bool near(double actual, double expected, double tolerance = 1.0e-5)
{
    return std::abs(actual - expected) <= tolerance;
}

bool writeFixture(const juce::File& file, int numSamples = 8192, double sampleRate = 48000.0)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(sampleRate)
                       .withNumChannels(2)
                       .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream, options));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buffer(2, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto sine = 0.1f * std::sin(2.0f * juce::MathConstants<float>::pi
                                         * 1000.0f * static_cast<float>(sample)
                                         / static_cast<float>(sampleRate));
        buffer.setSample(0, sample, sine);
        buffer.setSample(1, sample, sine * 0.5f);
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

struct DocumentFixture
{
    explicit DocumentFixture(double sampleRate = 48000.0)
    {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("SingleEditorParametricEqTests")
                   .getNonexistentChildFile("eq", {});
        root.createDirectory();
        source = root.getChildFile("source.wav");
        require(writeFixture(source, 8192, sampleRate), "writes EQ fixture");
        require(document.loadFile(source), "loads EQ fixture");
    }

    ~DocumentFixture() { root.deleteRecursively(); }

    juce::File root;
    juce::File source;
    SingleTrackDocument document;
};

ParametricEqParameters oneKilohertzBoost()
{
    auto parameters = ParametricEqParameters::flat();
    parameters.presetId = "custom";
    auto& band = parameters.bands[4];
    band.enabled = true;
    band.type = ParametricEqFilterType::bell;
    band.frequencyHz = 1000.0;
    band.gainDb = 6.0;
    band.q = 1.0;
    return parameters;
}

void runLazySchemaAndPresetTest()
{
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "ordinary startup owns no EQ processor");
    const auto descriptor = ParametricEqProcessor::describe();
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "cold EQ descriptor performs no processor allocation");

    const auto ids = ParametricEqProcessor::automationSafeParameterIds();
    require(ids.size() == 49, "eight bands expose a complete stable parameter schema");
    std::set<juce::String> unique(ids.begin(), ids.end());
    require(unique.size() == ids.size(), "automation-safe EQ parameter IDs are unique");
    require(unique.contains("eq.band1.frequency-hz")
                && unique.contains("eq.band8.slope")
                && unique.contains("eq.preset"),
            "automation-safe IDs do not depend on Simple or Advanced view labels");

    const auto flat = ParametricEqProcessor::parametersForPreset("flat");
    const auto vocal = ParametricEqProcessor::parametersForPreset("vocal-clarity");
    require(flat.has_value() && vocal.has_value(), "named sound-only presets resolve");
    require(flat->presetId == "flat" && vocal->presetId == "vocal-clarity",
            "preset identity remains part of the shared processor model");
    require(!ParametricEqProcessor::parametersForPreset("unknown").has_value(),
            "unknown preset never silently maps to a different sound");

    {
        auto processor = descriptor.create();
        require(processor != nullptr, "explicit demand creates an EQ processor");
        require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 1,
                "EQ processor lifetime is observable only after demand");
    }
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "EQ processor releases after demand ends");
}

void runCoefficientAndCurveTest()
{
    DocumentFixture fixture;
    auto processor = ParametricEqProcessor::describe().create();
    auto parameters = oneKilohertzBoost();
    parameters.bands[7].enabled = true;
    parameters.bands[7].type = ParametricEqFilterType::lowPass;
    parameters.bands[7].frequencyHz = 100000.0;

    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             41,
                                             {},
                                             parameters);
    require(analyzed.succeeded(), "EQ Analyze produces an immutable plan");
    require(analyzed.plan->getAudioSuitePlan()->getProcessorId()
                == "singleeditor.parametric-eq-v1",
            "EQ plan binds processor identity");
    const auto& bell = analyzed.plan->getResolvedBands()[4];
    require(bell.stageCount == 1, "12 dB EQ band resolves one biquad stage");
    require(near(bell.stages[0].b0, 1.043953086990335),
            "golden bell b0 coefficient matches RBJ reference");
    require(near(bell.stages[0].b1, -1.8953207239365961),
            "golden bell b1 coefficient matches RBJ reference");
    require(near(bell.stages[0].b2, 0.8677222847598566),
            "golden bell b2 coefficient matches RBJ reference");
    require(near(bell.stages[0].a1, -1.8953207239365961)
                && near(bell.stages[0].a2, 0.9116753717501915),
            "golden bell denominator matches RBJ reference");
    require(std::abs(20.0 * std::log10(analyzed.plan->magnitudeAt(1000.0)) - 6.0)
                < 0.08,
            "static response uses the exact render coefficients");

    const auto& lowPass = analyzed.plan->getResolvedBands()[7];
    require(lowPass.requested.frequencyHz == 100000.0,
            "requested frequency preserves user intent");
    require(near(lowPass.effectiveFrequencyHz, 23520.0, 0.001),
            "effective frequency adapts explicitly below Nyquist");
}

void runImpulseSilenceAndClippingTest()
{
    DocumentFixture fixture;
    auto processor = ParametricEqProcessor::describe().create();
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             52,
                                             {},
                                             oneKilohertzBoost());
    require(analyzed.succeeded(), "EQ plan resolves for render evidence");

    juce::AudioBuffer<float> impulse(2, 4096);
    impulse.clear();
    impulse.setSample(0, 512, 1.0f);
    impulse.setSample(1, 512, 1.0f);
    require(ParametricEqProcessor::renderPlanForDiagnostics(
                *analyzed.plan, impulse, 0, { 0, 4096 }),
            "impulse render creates an isolated execution session");
    require(std::abs(impulse.getSample(0, 513)) > 1.0e-5f,
            "minimum-phase EQ impulse has a causal tail");
    require(near(impulse.getSample(0, 700), impulse.getSample(1, 700), 1.0e-6),
            "shared parameters render target channels deterministically");

    juce::AudioBuffer<float> silence(2, 1024);
    silence.clear();
    require(ParametricEqProcessor::renderPlanForDiagnostics(
                *analyzed.plan, silence, 0, { 0, 1024 }),
            "silence render succeeds");
    require(silence.getMagnitude(0, silence.getNumSamples()) == 0.0f,
            "EQ never invents signal from silence");

    auto hotParameters = oneKilohertzBoost();
    hotParameters.bands[4].gainDb = 24.0;
    const auto hot = processor->analyze(fixture.document.getState(),
                                        52,
                                        {},
                                        hotParameters);
    require(hot.succeeded(), "maximum supported EQ boost resolves");
    juce::AudioBuffer<float> hotBuffer(2, 2048);
    for (int channel = 0; channel < hotBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < hotBuffer.getNumSamples(); ++sample)
            hotBuffer.setSample(channel, sample, 4.0f);
    require(ParametricEqProcessor::renderPlanForDiagnostics(
                *hot.plan, hotBuffer, 0, { 0, 2048 }),
            "over-range input render completes");
    for (int channel = 0; channel < hotBuffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < hotBuffer.getNumSamples(); ++sample)
            require(std::isfinite(hotBuffer.getSample(channel, sample))
                        && std::abs(hotBuffer.getSample(channel, sample)) <= 32.0f,
                    "clipping stress cannot create NaN, infinity or runaway state");
}

void runPreviewApplyUndoSaveParityTest()
{
    DocumentFixture fixture;
    auto processor = ParametricEqProcessor::describe().create();
    AudioSelectionRequest channelOne;
    channelOne.sourceChannels = { SourceChannelIdentity::fromSourceIndex(1) };
    const auto analyzed = processor->analyze(fixture.document.getState(),
                                             88,
                                             channelOne,
                                             oneKilohertzBoost());
    require(analyzed.succeeded(), "channel-aware EQ plan resolves");

    auto preview = processor->createPreview(analyzed.plan);
    require(preview.isValid(), "EQ Preview uses the shared cancellable handle");
    AudioEngine engine(fixture.document);
    engine.setAudioSuitePreview(preview.getState());
    juce::AudioBuffer<float> audition(2, 4096);
    require(engine.renderDocumentBlockForDiagnostics(0, audition),
            "main playback path renders EQ Preview");
    const auto previewTarget = audition.getSample(1, 1000);
    const auto previewUntargeted = audition.getSample(0, 1000);
    engine.clearAudioSuitePreview();

    const auto sourceState = fixture.document.getState();
    const auto applied = processor->apply(fixture.document,
                                          analyzed.plan,
                                          88,
                                          {},
                                          4096);
    require(applied.succeeded() && applied.renderedBlocks > 1,
            "EQ Apply reuses the chunked atomic AudioSuite transaction");
    const auto appliedState = fixture.document.getState();
    require(near(appliedState->audioBuffer->getSample(1, 1000), previewTarget, 0.002),
            "Preview and Apply use the same coefficients and execution kernel");
    require(near(appliedState->audioBuffer->getSample(0, 1000), previewUntargeted, 0.002),
            "time-by-channel Apply preserves an untargeted channel");
    require(appliedState->audioRevision != sourceState->audioRevision
                && fixture.document.hasUnsavedChanges()
                && fixture.document.canUndo(),
            "EQ Apply atomically publishes waveform, dirty and Undo truth");
    require(fixture.document.saveOverSourceFile(),
            "Ctrl+S-equivalent save works immediately after EQ Apply");
    require(fixture.document.undo(), "Undo works after EQ Apply and save-over-source");
    require(fixture.document.getState()->audioBuffer != nullptr,
            "Undo remains playable without reloading the overwritten reader");
}

void runSharedSimpleAdvancedMagTest()
{
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "EQ Mag test begins without a processor instance");
    ParametricEqMagComponent mag(ParametricEqParameters::flat());
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "requesting the descriptor-only EQ Mag does not create the processor");

    // The session owns this preference while optional Mag components are gone.
    auto retainedMode = ParametricEqMagComponent::ViewMode::simple;
    std::size_t retainedBand = 0;
    auto bandNotifications = 0;
    auto modeNotifications = 0;
    auto sizeRequests = 0;
    auto audioParameterChanges = 0;
    const auto recreate = [&]
    {
        auto view = std::make_unique<ParametricEqMagComponent>(ParametricEqParameters::flat());
        view->onMagSizeRequested = [&] (RendererMagSize) { ++sizeRequests; };
        view->setViewMode(retainedMode, false);
        view->setSelectedAdvancedBand(retainedBand);
        view->onSelectedAdvancedBandChanged = [&] (std::size_t band)
        {
            retainedBand = band;
            ++bandNotifications;
        };
        view->onViewModeChanged = [&] (ParametricEqMagComponent::ViewMode mode)
        {
            retainedMode = mode;
            ++modeNotifications;
        };
        view->onParametersChanged = [&] (const ParametricEqParameters&) { ++audioParameterChanges; };
        return view;
    };
    auto firstVisit = recreate();
    firstVisit->setViewMode(ParametricEqMagComponent::ViewMode::advanced);
    firstVisit->setViewMode(ParametricEqMagComponent::ViewMode::advanced);
    require(modeNotifications == 1 && sizeRequests == 1,
            "a real view choice notifies once and requests its existing sweet spot once");
    auto* bandChoice = dynamic_cast<juce::ComboBox*>(firstVisit->findChildWithID("eq.advanced.band"));
    require(bandChoice != nullptr, "the real selected-band control is available");
    bandChoice->setSelectedId(5, juce::sendNotificationSync);
    firstVisit->setSelectedAdvancedBand(4);
    require(retainedBand == 4 && bandNotifications == 1,
            "a real Band 5 choice notifies once without duplicate programmatic notification");
    firstVisit.reset();
    auto restoredView = recreate();
    for (const auto size : { juce::Point<int>(1018, 320), juce::Point<int>(420, 180) })
    {
        restoredView->setSize(size.x, size.y);
        restoredView->applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::large,
                                                   RendererInformationTier::detail, true);
        require(restoredView->getViewMode() == ParametricEqMagComponent::ViewMode::advanced,
                "recreated and resized EQ views retain the explicit Advanced choice");
        const auto* restoredChoice = dynamic_cast<juce::ComboBox*>(restoredView->findChildWithID("eq.advanced.band"));
        require(restoredView->getSelectedAdvancedBand() == 4 && restoredChoice
                    && restoredChoice->getSelectedId() == 5,
                "recreated and resized inspectors retain the selected band and actual ComboBox label");
    }
    require(modeNotifications == 1 && bandNotifications == 1 && sizeRequests == 1 && audioParameterChanges == 0,
            "restoration never re-requests layout or edits audio parameters");
    restoredView->setViewMode(ParametricEqMagComponent::ViewMode::simple);
    restoredView.reset();
    auto restoredSimple = recreate();
    require(restoredSimple->getViewMode() == ParametricEqMagComponent::ViewMode::simple
                && modeNotifications == 2 && sizeRequests == 2 && audioParameterChanges == 0,
            "an explicit return to Simple also survives optional-view recreation");
    restoredSimple->setViewMode(ParametricEqMagComponent::ViewMode::advanced, false);
    require(restoredSimple->getSelectedAdvancedBand() == 4 && audioParameterChanges == 0,
            "Simple does not discard the Advanced editing position");
    restoredSimple->setSelectedAdvancedBand(ParametricEqParameters::bandCount + 10);
    const auto* clampedChoice = dynamic_cast<juce::ComboBox*>(restoredSimple->findChildWithID("eq.advanced.band"));
    require(restoredSimple->getSelectedAdvancedBand() == ParametricEqParameters::bandCount - 1
                && clampedChoice && clampedChoice->getSelectedId() == static_cast<int>(ParametricEqParameters::bandCount)
                && audioParameterChanges == 0,
            "out-of-range restored indexes clamp safely without changing audio parameters");

    auto parameterNotifications = 0;
    mag.onParametersChanged = [&] (const ParametricEqParameters&)
    {
        ++parameterNotifications;
    };
    mag.setBounds(0, 0, 720, 520);
    mag.applyWorkspaceVisualContract(RendererDensity::standard,
                                     RendererMagSize::medium,
                                     RendererInformationTier::standard,
                                     true);
    mag.setSimpleGainForTesting(0, 3.5);
    require(parameterNotifications == 1
                && mag.getParameters().bands[1].enabled
                && near(mag.getParameters().bands[1].gainDb, 3.5),
            "Simple view edits the shared low-shelf band parameter");

    mag.setViewMode(ParametricEqMagComponent::ViewMode::advanced, false);
    mag.applyWorkspaceVisualContract(RendererDensity::expanded,
                                     RendererMagSize::large,
                                     RendererInformationTier::detail,
                                     true);
    require(mag.isBandControlVisibleForTesting(0)
                && near(mag.getParameters().bands[1].gainDb, 3.5),
            "Advanced view exposes the same preserved parameter at its large sweet spot");
    mag.setViewMode(ParametricEqMagComponent::ViewMode::simple, false);
    require(near(mag.getParameters().bands[1].gainDb, 3.5),
            "switching views never creates a second EQ parameter model");
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "Simple and Advanced view changes remain processor-free before Preview or Apply");

    mag.setBounds(0, 0, 320, 70);
    mag.applyWorkspaceVisualContract(RendererDensity::compact,
                                     RendererMagSize::small,
                                     RendererInformationTier::summary,
                                     true);
    require(mag.areSizeControlsUsableForTesting(),
            "Compact EQ keeps all S/M/L controls visible and inside the Mag so it can be expanded again");
    require(mag.visibleCompactBandGainCountForTesting() == 8
                && mag.compactBandGainsFitBoundsForTesting(),
            "Compact wide EQ keeps all eight real band gains visible and inside the Mag");
    mag.setCompactBandGainForTesting(7, 2.4);
    require(parameterNotifications == 2
                && mag.getParameters().bands[7].enabled
                && near(mag.getParameters().bands[7].gainDb, 2.4),
            "a compact gain knob edits and enables the shared eighth EQ band");

    mag.setBounds(0, 0, 220, 420);
    mag.applyWorkspaceVisualContract(RendererDensity::compact,
                                     RendererMagSize::small,
                                     RendererInformationTier::summary,
                                     true);
    require(mag.visibleCompactBandGainCountForTesting() == 8
                && mag.compactBandGainsFitBoundsForTesting()
                && mag.areSizeControlsUsableForTesting(),
            "Compact portrait EQ reflows the same eight live gain knobs instead of dropping controls");
}
void runCoefficientVisualizationTest()
{
    require(ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0, "curve test starts processor-free");
    auto parameters = oneKilohertzBoost();
    const auto curve = ParametricEqProcessor::magnitudeResponse(parameters, 96000, 20, 20000);
    require(curve.size() == 512 && ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "coefficient visualization works without creating any processor");
    require(ParametricEqProcessor::magnitudeResponse(parameters, 0, 20, 20000).empty()
                && ParametricEqProcessor::magnitudeResponse(parameters, 22050, 20, 20000).empty()
                && ParametricEqProcessor::magnitudeResponse(parameters, 48000, 20, 20000, 5000).empty(),
            "invalid source geometry cannot create a misleading response or unbounded render workload");
    for (const auto sampleRate : { 22050.0, 44100.0, 96000.0, 192000.0 })
    {
        DocumentFixture fixture(sampleRate);
        auto processor = ParametricEqProcessor::describe().create();
        auto variants = ParametricEqProcessor::builtInPresets();
        for (int i = 0; i < 8; ++i)
        {
            parameters.bands[static_cast<std::size_t>(i)].enabled = i != 3;
            parameters.bands[static_cast<std::size_t>(i)].type = static_cast<ParametricEqFilterType>(i % 6);
            parameters.bands[static_cast<std::size_t>(i)].slope = ParametricEqSlope::db24;
            parameters.bands[static_cast<std::size_t>(i)].frequencyHz = 30.0 * std::pow(2.7, i);
        }
        variants.push_back({ "mixed", "Mixed types and slopes", parameters });
        for (const auto& preset : variants)
        {
            const auto plan = processor->analyze(fixture.document.getState(), 81, {}, preset.parameters);
            require(plan.succeeded(), "response parity uses a real valid audio edit plan");
            const auto maximumHz = std::min(20000.0, sampleRate * 0.49);
            const auto response = ParametricEqProcessor::magnitudeResponse(preset.parameters, sampleRate, 20, maximumHz);
            require(response.size() == 512, "valid response has the requested sampling density");
            for (int point = 0; point < 512; ++point)
            {
                const auto hz = std::exp(std::log(20.0) + std::log(maximumHz / 20.0) * point / 511.0);
                const auto expected = juce::Decibels::gainToDecibels(plan.plan->magnitudeAt(hz), -120.0);
                require(std::isfinite(response[static_cast<std::size_t>(point)])
                            && std::abs(response[static_cast<std::size_t>(point)] - expected) < 0.0001,
                        "display response exactly follows audio coefficients across sample rates, presets and slopes");
            }
        }
    }
}

void runIndividualBandVisualizationTest()
{
    RendererSelection::resetForTests(RendererKind::modern);
    auto parameters = ParametricEqParameters::flat();
    for (std::size_t i = 0; i < parameters.bands.size(); ++i)
    {
        auto& band = parameters.bands[i];
        band.enabled = i != 3;
        band.type = static_cast<ParametricEqFilterType>(i % 6);
        band.frequencyHz = 30.0 * std::pow(2.7, static_cast<double>(i));
        band.gainDb = i % 2 == 0 ? 6.0 : -3.0;
        band.slope = i % 2 == 0 ? ParametricEqSlope::db24 : ParametricEqSlope::db12;
    }
    ParametricEqMagComponent mag(parameters);
    auto changes = 0;
    mag.onParametersChanged = [&changes] (const auto&) { ++changes; };
    for (const auto sampleRate : { 22050.0, 48000.0, 192000.0 })
    {
        mag.setSourceSampleRate(sampleRate);
        DocumentFixture fixture(sampleRate);
        auto processor = ParametricEqProcessor::describe().create();
        const auto plan = processor->analyze(fixture.document.getState(), 82, {}, parameters);
        require(plan.succeeded(), "individual traces are checked against resolved audio-plan stages");
        for (const auto minimumHz : { 20.0, 100.0 })
        {
            const auto maximumHz = std::min(20000.0, sampleRate * 0.49);
            if (minimumHz == 100.0)
                mag.setExactMagnitudeCurve(ParametricEqProcessor::magnitudeResponse(
                    parameters, sampleRate, minimumHz, maximumHz, 96), minimumHz, maximumHz);
            const auto& curves = mag.bandResponsesForTesting();
            for (std::size_t i = 0; i < curves.size(); ++i)
            {
                const auto& resolved = plan.plan->getResolvedBands()[i];
                require(resolved.stageCount == 0 ? curves[i].empty() : curves[i].size() == 512,
                        "only enabled bands have an individual response, also after Preview changes the range");
                for (std::size_t point = 0; point < curves[i].size(); ++point)
                {
                    const auto hz = std::exp(std::log(minimumHz)
                        + std::log(maximumHz / minimumHz) * static_cast<double>(point) / 511.0);
                    auto magnitude = 1.0;
                    for (int stage = 0; stage < resolved.stageCount; ++stage)
                        magnitude *= resolved.stages[static_cast<std::size_t>(stage)].magnitudeAt(hz, sampleRate);
                    const auto expected = juce::Decibels::gainToDecibels(magnitude, -120.0);
                    require(std::isfinite(curves[i][point]) && near(curves[i][point], expected, 0.0001),
                            "each displayed band follows its actual audio stages, source limit and slope");
                }
            }
        }
    }
    auto disabled = parameters;
    for (auto& band : disabled.bands) band.enabled = false;
    mag.setParameters(disabled);
    require(std::all_of(mag.bandResponsesForTesting().begin(), mag.bandResponsesForTesting().end(),
                       [] (const auto& curve) { return curve.empty(); }),
            "disabling a band removes its stale trace immediately");
    mag.setParameters(parameters);
    mag.setSourceSampleRate(0.0);
    require(std::all_of(mag.bandResponsesForTesting().begin(), mag.bandResponsesForTesting().end(),
                       [] (const auto& curve) { return curve.empty(); }),
            "losing the source clears every coefficient trace");
    require(changes == 0 && ParametricEqProcessor::getLiveInstanceCountForDiagnostics() == 0,
            "display updates do not emit sound edits or retain a processor");
    mag.setSourceSampleRate(48000.0);
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int step = 0; step < 200; ++step)
    {
        parameters.bands[2].gainDb = step % 24 - 12.0;
        mag.setParameters(parameters);
    }
    std::cout << "EQ coefficient/cache/control update mean ms="
              << (juce::Time::getMillisecondCounterHiRes() - start) / 200.0 << '\n';
}

void runModernMagVisualTest()
{
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);
    auto parameters = oneKilohertzBoost();
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_EQ_OFFSCREEN_DIR", {});
    if (output.isNotEmpty()) require(juce::File(output).createDirectory().wasOk(), "EQ screenshot directory is writable");
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        ModernAppleLookAndFeel lookAndFeel;
        ParametricEqMagComponent mag(parameters);
        mag.setLookAndFeel(&lookAndFeel);
        mag.setSourceSampleRate(48000.0);
        auto changes = 0;
        mag.onParametersChanged = [&changes] (const auto&) { ++changes; };
        for (const auto size : { juce::Point<int>(320, 70), { 220, 420 }, { 433, 185 }, { 480, 320 }, { 720, 520 }, { 935, 342 }, { 936, 342 }, { 940, 342 }, { 944, 342 }, { 1000, 342 }, { 1000, 500 } })
            for (const auto view : { ParametricEqMagComponent::ViewMode::simple, ParametricEqMagComponent::ViewMode::advanced })
            {
                mag.setSize(size.x, size.y);
                mag.setViewMode(view, false);
                const bool summary = size.y < 170 || size.x < 280;
                mag.applyWorkspaceVisualContract(RendererDensity::standard,
                    summary ? RendererMagSize::small : RendererMagSize::large,
                    summary ? RendererInformationTier::summary : RendererInformationTier::detail, true);
                const auto* bandPicker = mag.findChildWithID("eq.advanced.band");
                const auto visibleGains = mag.visibleCompactBandGainCountForTesting();
                require((visibleGains == 8 && mag.compactBandGainsFitBoundsForTesting())
                            || (visibleGains == 1 && bandPicker != nullptr && bandPicker->isVisible()
                                && view == ParametricEqMagComponent::ViewMode::advanced),
                        "Advanced retains either all gains or a complete selected-band editor with access to every band");
                if (size == juce::Point<int>(433, 185) && view == ParametricEqMagComponent::ViewMode::advanced)
                    for (const auto* parameter : { ".enabled", ".type", ".frequency-hz", ".gain-db", ".q", ".slope" })
                    {
                        auto* control = mag.findChildWithID("eq.band1" + juce::String(parameter));
                        require(control != nullptr && control->isVisible() && control->getHeight() >= 24,
                                "short DSP Advanced exposes all six selected-band controls without enlargement");
                    }
                require(mag.areSizeControlsUsableForTesting(), "Modern EQ always exposes its size recovery controls");
                if (size == juce::Point<int>(433, 185) && view == ParametricEqMagComponent::ViewMode::simple)
                    for (int band = 1; band <= 8; ++band)
                    {
                        auto* gain = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band" + juce::String(band) + ".gain-db"));
                        require(gain != nullptr && gain->getTextBoxPosition() == juce::Slider::TextBoxBelow
                                    && gain->getHeight() >= 34,
                                "Short wide DSP scene retains every real EQ gain readout");
                    }
                for (auto* child : mag.getChildren())
                    if (child->isVisible() && !child->getBounds().isEmpty())
                        require(mag.getLocalBounds().contains(child->getBounds()), "visible EQ controls stay inside the actual Mag");
                if (size.x >= 936 && view == ParametricEqMagComponent::ViewMode::advanced)
                {
                    require(bandPicker != nullptr && !bandPicker->isVisible(),
                            "ordinary wide DSP exposes all eight inspectors without a selected-band fallback");
                    require(mag.curveBoundsForTesting().getHeight() >= 120,
                            "all-band Advanced retains a readable response curve in the ordinary DSP tile");
                    auto* numeric = mag.findChildWithID("eq.band1.frequency-hz");
                    juce::Label* value = nullptr;
                    for (auto* child : numeric->getChildren())
                        if (auto* label = dynamic_cast<juce::Label*>(child)) value = label;
                    require(value != nullptr, "all-band frequency exposes its actual editable value label");
                    std::cout << "Inline EQ " << size.x << 'x' << size.y << " slider=" << numeric->getBounds().toString()
                              << " label=" << value->getBounds().toString() << " font=" << value->getFont().getHeight()
                              << " scale=" << value->getMinimumHorizontalScale() << " available="
                              << value->getWidth() - value->getBorderSize().getLeftAndRight()
                              << " sixDigits=" << juce::GlyphArrangement::getStringWidth(value->getFont(), "192000") << '\n';
                    for (const auto* text : { "30", "1000", "20000", "99999", "192000" })
                        require(juce::GlyphArrangement::getStringWidth(value->getFont(), text) * value->getMinimumHorizontalScale()
                                    <= value->getWidth() - value->getBorderSize().getLeftAndRight(),
                                "all-band frequency retains complete integer Hz rather than ellipsis");
                    auto* gain = mag.findChildWithID("eq.band1.gain-db");
                    juce::Label* gainValue = nullptr;
                    for (auto* child : gain->getChildren())
                        if (auto* label = dynamic_cast<juce::Label*>(child)) gainValue = label;
                    require(gainValue != nullptr, "all-band gain retains its actual editable value label");
                    std::cout << "Inline gain label=" << gainValue->getBounds().toString()
                              << " em=" << gainValue->getFont().getHeightInPoints() << " scale=" << gainValue->getMinimumHorizontalScale()
                              << " available=" << gainValue->getWidth() - gainValue->getBorderSize().getLeftAndRight()
                              << " signed=" << juce::GlyphArrangement::getStringWidth(gainValue->getFont(), "-18.0 dB") << '\n';
                    for (const auto* text : { "-18.0 dB", "0.0 dB", "18.0 dB" })
                        require(juce::GlyphArrangement::getStringWidth(gainValue->getFont(), text) * gainValue->getMinimumHorizontalScale()
                                    <= gainValue->getWidth() - gainValue->getBorderSize().getLeftAndRight(),
                                "inline gain shows the complete signed value and dB unit");
                    for (int band = 1; band <= 8; ++band)
                        for (const auto* parameter : { ".enabled", ".type", ".frequency-hz", ".gain-db", ".q", ".slope" })
                        {
                            auto* child = mag.findChildWithID("eq.band" + juce::String(band) + parameter);
                            const auto minimumWidth = juce::String(parameter) == ".enabled" ? 22 : 30;
                            require(child != nullptr && child->isVisible() && child->getHeight() >= 22 && child->getWidth() >= minimumWidth,
                                    "Wide Advanced exposes all six controls for each of the eight real bands");
                        }
                }
                if (size.x == 935 && view == ParametricEqMagComponent::ViewMode::advanced)
                    require(bandPicker != nullptr && bandPicker->isVisible(),
                            "below the readable eight-column width Advanced retains selected-band editing");
                for (int i = 0; i < mag.getNumChildComponents(); ++i)
                    for (int j = i + 1; j < mag.getNumChildComponents(); ++j)
                    {
                        const auto* a = mag.getChildComponent(i);
                        const auto* b = mag.getChildComponent(j);
                        require(!a->isVisible() || !b->isVisible() || !a->getBounds().intersects(b->getBounds()),
                                "Modern EQ visible sibling controls do not overlap");
                    }
                auto* frequency = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band1.frequency-hz"));
                require(frequency != nullptr && near(frequency->getValueFromText("2.5 kHz"), 2500.0)
                            && near(frequency->getValueFromText("250"), 250.0)
                            && frequency->getTextFromValue(2500.0) == "2500",
                        "compact frequency values preserve explicit Hz and kHz editing semantics");
                for (const double hz : { 1001.0, 10001.0, 99999.0 })
                    require(near(frequency->getValueFromText(frequency->getTextFromValue(hz)), hz),
                            "editable frequency display roundtrips every integer Hz without kHz rounding loss");
                const auto snapshot = mag.createComponentSnapshot(mag.getLocalBounds(), true, 1.5f);
                require(snapshot.isValid(), "real native EQ component renders offscreen");
                if (output.isNotEmpty())
                {
                    const auto name = juce::String(theme == ModernTheme::night ? "night-" : "light-")
                        + (view == ParametricEqMagComponent::ViewMode::simple ? "simple-" : "advanced-")
                        + juce::String(size.x) + "x" + juce::String(size.y) + ".png";
                    auto stream = juce::File(output).getChildFile(name).createOutputStream();
                    require(stream != nullptr && stream->setPosition(0)
                        && juce::PNGImageFormat().writeImageToStream(snapshot, *stream) && stream->truncate().wasOk(),
                        "native EQ visual evidence is written");
                }
            }
        require(changes == 0 && near(mag.getParameters().bands[4].gainDb, 6.0),
                "view, theme and geometry changes do not mutate sound parameters");
        auto* frequency = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band5.frequency-hz"));
        require(frequency != nullptr, "fifth band has a real editable Slider");
        juce::Label* valueLabel = nullptr;
        for (auto* child : frequency->getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child)) valueLabel = label;
        require(valueLabel != nullptr, "numeric stepper owns a real JUCE value label");
        valueLabel->showEditor();
        auto* editor = valueLabel->getCurrentTextEditor();
        require(editor != nullptr && editor->findColour(juce::TextEditor::textColourId) == RendererVisualStyle::primaryText()
                    && editor->findColour(juce::TextEditor::backgroundColourId) == RendererVisualStyle::mutedSurface(),
                "the actual numeric TextEditor overrides inherited V4 editing colours in both themes");
        editor->setText("2.5 kHz", false);
        valueLabel->hideEditor(false);
        require(near(mag.getParameters().bands[4].frequencyHz, 2500.0) && changes == 1,
                "actual value-label commit parses kHz and emits one real band parameter change");
        for (const auto compact : { juce::Point<int>(433, 185), {220, 420}, {320, 270}, {480, 320} })
        {
            mag.setParameters(parameters);
            mag.setSize(compact.x, compact.y);
            mag.setViewMode(ParametricEqMagComponent::ViewMode::advanced, false);
            mag.applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::medium,
                                             RendererInformationTier::standard, true);
            auto* picker = dynamic_cast<juce::ComboBox*>(mag.findChildWithID("eq.advanced.band"));
            require(picker != nullptr && picker->isVisible(), "standard-tier compact Advanced has a band selector");
            const int beforeSelection = changes;
            for (int band = 1; band <= 8; ++band)
            {
                picker->setSelectedId(band, juce::sendNotificationSync);
                for (const auto* parameter : { ".enabled", ".type", ".frequency-hz", ".gain-db", ".q", ".slope" })
                {
                    auto* field = mag.findChildWithID("eq.band" + juce::String(band) + parameter);
                    require(field != nullptr && field->isVisible() && field->getHeight() >= 24
                                && mag.getLocalBounds().contains(field->getBounds()),
                            "every selected band exposes all six actual parameter controls");
                }
                for (int i = 0; i < mag.getNumChildComponents(); ++i)
                    for (int j = i + 1; j < mag.getNumChildComponents(); ++j)
                    {
                        const auto* a = mag.getChildComponent(i);
                        const auto* b = mag.getChildComponent(j);
                        require(!a->isVisible() || !b->isVisible() || !a->getBounds().intersects(b->getBounds()),
                                "selected-band inspection does not overlap any visible control");
                    }
            }
            require(changes == beforeSelection, "inspecting all eight bands does not change sound parameters");
            auto* gain = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band8.gain-db"));
            gain->setValue(3.0, juce::sendNotificationSync);
            require(changes == beforeSelection + 1 && near(mag.getParameters().bands[7].gainDb, 3.0)
                        && near(mag.getParameters().bands[4].gainDb, parameters.bands[4].gainDb),
                    "compact editing changes only the selected real band once");
            if (output.isNotEmpty())
            {
                const auto name = juce::String(theme == ModernTheme::night ? "night-" : "light-")
                    + "selected-" + juce::String(compact.x) + "x" + juce::String(compact.y) + ".png";
                const auto snapshot = mag.createComponentSnapshot(mag.getLocalBounds(), true, 1.5f);
                auto stream = juce::File(output).getChildFile(name).createOutputStream();
                require(stream != nullptr && stream->setPosition(0)
                            && juce::PNGImageFormat().writeImageToStream(snapshot, *stream) && stream->truncate().wasOk(),
                        "selected-band evidence is written");
            }
            RendererVisualStyle::setModernTheme(theme == ModernTheme::light ? ModernTheme::night : ModernTheme::light);
            ModernAppleLookAndFeel::refreshTheme(mag);
            for (const auto* suffix : { ".frequency-hz", ".q", ".gain-db" })
            {
                auto* number = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band8" + juce::String(suffix)));
                if (number->getSliderStyle() != juce::Slider::IncDecButtons) continue;
                juce::Label* label = nullptr;
                for (auto* child : number->getChildren())
                    if (auto* candidate = dynamic_cast<juce::Label*>(child)) label = candidate;
                require(label != nullptr && label->findColour(juce::Label::backgroundColourId) == RendererVisualStyle::mutedSurface()
                            && label->findColour(juce::Label::textColourId) == RendererVisualStyle::primaryText(),
                        "existing numeric labels adopt the new theme without resizing or reopening the Mag");
            }
            RendererVisualStyle::setModernTheme(theme);
            ModernAppleLookAndFeel::refreshTheme(mag);
            mag.setViewMode(ParametricEqMagComponent::ViewMode::simple, false);
            require(mag.visibleCompactBandGainCountForTesting() == 8 && mag.compactBandGainsFitBoundsForTesting(),
                    "returning to Simple restores all eight shared gains");
        }
        if (output.isNotEmpty())
            for (const auto& preset : ParametricEqProcessor::builtInPresets())
                for (const auto size : { juce::Point<int>(480, 320), {1000, 342} })
                {
                    mag.setParameters(preset.parameters);
                    mag.setSize(size.x, size.y);
                    mag.setViewMode(ParametricEqMagComponent::ViewMode::simple, false);
                    mag.applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::medium,
                                                     RendererInformationTier::standard, true);
                    const auto snapshot = mag.createComponentSnapshot(mag.getLocalBounds());
                    const auto name = juce::String(theme == ModernTheme::night ? "night-" : "light-")
                        + preset.stableId + "-" + juce::String(size.x) + "x" + juce::String(size.y) + ".png";
                    auto stream = juce::File(output).getChildFile(name).createOutputStream();
                    require(stream != nullptr && stream->setPosition(0)
                        && juce::PNGImageFormat().writeImageToStream(snapshot, *stream) && stream->truncate().wasOk(),
                        "native preset curves and numbered bands have reviewable render evidence");
                }
        mag.setLookAndFeel(nullptr);
    }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
}
void runNumericEditorKeyboardTest()
{
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const auto size : { juce::Point<int>(480, 320), {940, 342}, {1000, 342} })
        {
            RendererVisualStyle::setModernTheme(theme);
            ModernAppleLookAndFeel lookAndFeel;
            ParametricEqMagComponent mag(ParametricEqParameters::flat());
            mag.setLookAndFeel(&lookAndFeel);
            mag.setSize(size.x, size.y);
            int changes = 0;
            mag.onParametersChanged = [&] (const auto&) { ++changes; };
            for (const auto* suffix : { ".gain-db", ".frequency-hz", ".q" })
            {
                const bool gain = juce::String(suffix) == ".gain-db";
                mag.setViewMode(gain ? ParametricEqMagComponent::ViewMode::simple
                                    : ParametricEqMagComponent::ViewMode::advanced, false);
                auto* picker = dynamic_cast<juce::ComboBox*>(mag.findChildWithID("eq.advanced.band"));
                picker->setSelectedId(3, juce::sendNotificationSync);
                auto* slider = dynamic_cast<juce::Slider*>(mag.findChildWithID("eq.band3" + juce::String(suffix)));
                require(slider != nullptr && slider->isVisible(), "numeric keyboard target is an actual visible EQ field");
                auto* listener = dynamic_cast<juce::KeyListener*>(slider);
                require(listener != nullptr, "EQ slider supplies its editor key listener");
                auto findLabel = [&]() -> juce::Label*
                {
                    for (auto* child : slider->getChildren())
                        if (auto* label = dynamic_cast<juce::Label*>(child)) return label;
                    return nullptr;
                };
                auto* label = findLabel();
                require(label != nullptr && label->onEditorShow != nullptr, "live and rebuilt labels attach editor routing");
                const auto original = slider->getValue();
                const auto changesBefore = changes;
                label->showEditor();
                auto* editor = label->getCurrentTextEditor();
                require(editor != nullptr, "numeric text editor opens");
                const juce::Component::SafePointer<juce::TextEditor> stableEditor(editor);
                require(listener->keyPressed(juce::KeyPress(juce::KeyPress::upKey), editor), "Up is consumed by the numeric editor");
                require(near(slider->getValueFromText(editor->getText()), original + slider->getInterval()),
                        "Up steps by gain, frequency or Q's actual parameter interval");
                require(listener->keyPressed(juce::KeyPress(juce::KeyPress::downKey), editor)
                            && near(slider->getValueFromText(editor->getText()), original),
                        "Down reverses exactly one step");
                require(stableEditor != nullptr && label->getCurrentTextEditor() == stableEditor.getComponent()
                            && near(slider->getValue(), original) && changes == changesBefore,
                        "stepping preserves editor identity and native uncommitted draft semantics");
                require(!listener->keyPressed(juce::KeyPress(juce::KeyPress::leftKey), editor)
                            && !listener->keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0), editor)
                            && !listener->keyPressed(juce::KeyPress(juce::KeyPress::returnKey), editor),
                        "caret, selection and Return retain JUCE editor behavior");
                editor->setText(slider->getTextFromValue(slider->getMaximum()), false);
                listener->keyPressed(juce::KeyPress(juce::KeyPress::upKey), editor);
                require(near(slider->getValueFromText(editor->getText()), slider->getMaximum()), "upper limit clamps draft");
                editor->setText(slider->getTextFromValue(slider->getMinimum()), false);
                listener->keyPressed(juce::KeyPress(juce::KeyPress::downKey), editor);
                require(near(slider->getValueFromText(editor->getText()), slider->getMinimum()), "lower limit clamps draft");
                editor->setText("", false);
                listener->keyPressed(juce::KeyPress(juce::KeyPress::upKey), editor);
                require(near(slider->getValueFromText(editor->getText()), original + slider->getInterval()), "empty draft steps from committed value");
                require(!listener->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey), editor),
                        "Escape restores the draft then leaves editor closure to JUCE");
                label->hideEditor(true);
                require(near(slider->getValue(), original) && changes == changesBefore, "cancel discards all numeric draft changes");

                label->showEditor();
                editor = label->getCurrentTextEditor();
                const bool frequency = juce::String(suffix) == ".frequency-hz";
                if (frequency) editor->setText("2k", false);
                listener->keyPressed(juce::KeyPress(juce::KeyPress::upKey), editor);
                const auto expected = (frequency ? 2000.0 : original) + slider->getInterval();
                label->hideEditor(false);
                require(near(slider->getValue(), expected) && changes == changesBefore + 1,
                        "commit uses production slider callback once and preserves Hz shorthand parsing");
                const auto& band = mag.getParameters().bands[2];
                require(near(gain ? band.gainDb : (frequency ? band.frequencyHz : band.q), expected),
                        "numeric commit reaches the shared EQ parameter model");
            }
            mag.setLookAndFeel(nullptr);
        }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
}
void runLiveNumericDraftTest()
{
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);
    DocumentFixture fixture;
    const auto documentState = fixture.document.getState();
    auto processor = ParametricEqProcessor::describe().create();
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const auto size : { juce::Point<int>(480, 320), {940, 342}, {1000, 342} })
            for (const auto* suffix : { ".gain-db", ".frequency-hz", ".q" })
            {
                RendererVisualStyle::setModernTheme(theme);
                ModernAppleLookAndFeel lookAndFeel;
                const auto initial = oneKilohertzBoost();
                auto mag = std::make_unique<ParametricEqMagComponent>(initial);
                mag->setLookAndFeel(&lookAndFeel);
                mag->setSourceSampleRate(48000.0);
                mag->setSize(size.x, size.y);
                const bool gain = juce::String(suffix) == ".gain-db";
                const bool frequency = juce::String(suffix) == ".frequency-hz";
                mag->setViewMode(gain ? ParametricEqMagComponent::ViewMode::simple
                                     : ParametricEqMagComponent::ViewMode::advanced, false);
                dynamic_cast<juce::ComboBox*>(mag->findChildWithID("eq.advanced.band"))->setSelectedId(5, juce::sendNotificationSync);
                auto* slider = dynamic_cast<juce::Slider*>(mag->findChildWithID("eq.band5" + juce::String(suffix)));
                auto* updater = dynamic_cast<juce::AsyncUpdater*>(slider);
                auto* listener = dynamic_cast<juce::KeyListener*>(slider);
                require(slider != nullptr && updater != nullptr && listener != nullptr && slider->isVisible(), "live edit target is a real visible production control");
                auto labelFor = [&]() -> juce::Label*
                {
                    for (auto* child : slider->getChildren())
                        if (auto* label = dynamic_cast<juce::Label*>(child)) return label;
                    return nullptr;
                };
                int changes = 0;
                ParametricEqPlanPtr previewPlan;
                AudioSuitePreviewHandle audition;
                mag->onParametersChanged = [&](const auto& parameters)
                {
                    ++changes;
                    const auto analyzed = processor->analyze(documentState, 91, {}, parameters);
                    require(analyzed.succeeded(), "live parameters resolve through the real EQ analyzer");
                    audition.cancel();
                    previewPlan = analyzed.plan;
                    audition = processor->createPreview(previewPlan);
                    require(audition.isValid(), "live feedback can replace a real audition handle without editing audio");
                };
                auto* label = labelFor();
                label->showEditor();
                auto* editor = label->getCurrentTextEditor();
                const juce::Component::SafePointer<juce::TextEditor> identity(editor);
                const auto originalCurve = mag->bandResponsesForTesting();
                const auto setDraft = [&](const juce::String& value)
                {
                    editor->setText(value, false);
                    require(editor->onTextChange != nullptr, "native editor text changes schedule live feedback");
                    editor->onTextChange();
                };
                for (const auto* invalid : { "", "-", "1e", "nan", "2garbage", "0x10" })
                {
                    setDraft(invalid);
                    updater->handleUpdateNowIfNeeded();
                    require(changes == 0 && mag->getParameters() == initial, "incomplete or invalid input leaves the last valid state intact");
                }
                setDraft("8");
                setDraft("9");
                setDraft(frequency ? "2.5 kHz" : (gain ? "12" : "2.5"));
                require(changes == 0 && updater->isUpdatePending(), "a rapid input burst queues only one pending update");
                updater->handleUpdateNowIfNeeded();
                const auto expected = frequency ? 2500.0 : (gain ? 12.0 : 2.5);
                const auto& live = mag->getParameters().bands[4];
                require(changes == 1 && near(gain ? live.gainDb : (frequency ? live.frequencyHz : live.q), expected)
                            && mag->bandResponsesForTesting() != originalCurve,
                        "latest valid draft updates the real parameter model and coefficient curve before commit");
                require(identity != nullptr && label->getCurrentTextEditor() == identity.getComponent()
                            && previewPlan->getParameters() == mag->getParameters(),
                        "live draft keeps the same editor and sends identical parameters to real Preview");
                listener->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey), editor);
                label->hideEditor(true);
                require(changes == 2 && mag->getParameters() == initial
                            && mag->bandResponsesForTesting() == originalCurve && previewPlan->getParameters() == initial,
                        "Escape restores parameters, preset identity, coefficient curves and active Preview together");

                label->showEditor();
                editor = label->getCurrentTextEditor();
                setDraft(frequency ? "2.5k" : (gain ? "12" : "2.5"));
                updater->handleUpdateNowIfNeeded();
                const auto liveChanges = changes;
                label->hideEditor(false);
                require(changes == liveChanges && near(slider->getValue(), expected), "commit synchronises Slider without a duplicate Preview rebuild");

                label = labelFor();
                label->showEditor();
                editor = label->getCurrentTextEditor();
                setDraft(frequency ? "3k" : (gain ? "10" : "3"));
                updater->handleUpdateNowIfNeeded();
                const auto beforeTheme = mag->getParameters();
                const auto beforeThemeChanges = changes;
                RendererVisualStyle::setModernTheme(theme == ModernTheme::light ? ModernTheme::night : ModernTheme::light);
                ModernAppleLookAndFeel::refreshTheme(*mag);
                require(mag->getParameters() == beforeTheme && changes == beforeThemeChanges,
                        "theme reconstruction commits latest draft without reverting or duplicating callbacks");

                label = labelFor();
                label->showEditor();
                editor = label->getCurrentTextEditor();
                setDraft("7");
                const auto beforeReplace = changes;
                mag->setParameters(ParametricEqParameters::flat());
                updater->handleUpdateNowIfNeeded();
                require(changes == beforeReplace && mag->getParameters() == ParametricEqParameters::flat(),
                        "external source/model replacement abandons queued old drafts without resurrecting old parameters");
                label = labelFor();
                label->showEditor();
                editor = label->getCurrentTextEditor();
                setDraft("8");
                mag.reset();
                require(changes == beforeReplace && fixture.document.getState() == documentState,
                        "destruction cancels pending editor callbacks and all draft feedback leaves document audio unchanged");
                audition.cancel();
            }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    runLazySchemaAndPresetTest();
    runCoefficientAndCurveTest();
    runImpulseSilenceAndClippingTest();
    runPreviewApplyUndoSaveParityTest();
    runSharedSimpleAdvancedMagTest();
    runModernMagVisualTest();
    runNumericEditorKeyboardTest();
    runLiveNumericDraftTest();
    runCoefficientVisualizationTest();
    runIndividualBandVisualizationTest();
    std::cout << "Parametric EQ tests passed\n";
    return 0;
}
