#include "UI/Components/WaveformViewComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace singleeditor
{
struct WaveformPresentationTestAccess
{
    static void deliverCacheNotification(WaveformViewComponent& view)
    {
        view.changeListenerCallback(nullptr);
    }
};

int runWaveformPresentationTests(const juce::File& artifacts)
{
    if (artifacts.exists() || artifacts.createDirectory().failed())
        throw std::runtime_error("Use a new waveform presentation artifact directory");
    RendererSelection::select(RendererKind::modern);
    juce::Array<juce::var> results;
    int failures = 0;
    const auto check = [&](bool ok, const char* name)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", name); row->setProperty("status", ok ? "pass" : "failed");
        results.add(juce::var(row));
        if (!ok) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    };

    SingleTrackDocument document;
    auto state = std::make_shared<SingleTrackDocument::State>();
    state->info.sourceAdmitted = true;
    state->info.sampleRate = 48000.0;
    state->info.numChannels = 2;
    state->info.numSamples = 48000;
    state->info.durationSeconds = 1.0;
    state->playbackAvailability = SingleTrackDocument::PlaybackAvailability::randomAccess();
    auto pcm = std::make_shared<juce::AudioBuffer<float>>(2, 48000);
    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < 48000; ++i)
            pcm->setSample(channel, i, static_cast<float>(0.6 * std::sin(i * 0.041 + channel)));
    state->audioBuffer = pcm;
    if (!document.adoptLoadedState(state)) throw std::runtime_error("Adopt actual in-memory PCM fixture");
    const auto original = document.getState();
    WaveformModel model(document);
    auto peaks = WaveformModel::buildCompletePeakCacheForState(original);
    if (peaks == nullptr || peaks->isPreview || peaks->channelBins.empty())
        throw std::runtime_error("Build actual source peaks");
    model.adoptPeakCache(peaks);
    AudioEngine engine(document); // No device initialization or route changes.
    ModernAppleLookAndFeel lookAndFeel;
    WaveformViewComponent view(engine, document, model);
    view.setLookAndFeel(&lookAndFeel);
    view.setBounds(0, 0, 960, 420);
    view.setVisible(true);
    view.beginPresentationGeneration();
    int presentations = 0;
    view.onWaveformPresentation = [&](bool) { ++presentations; };

    // Deliver the actual component's cache callback without painting. Cache
    // completion cannot stand in for a source waveform drawn by the view.
    WaveformPresentationTestAccess::deliverCacheNotification(view);
    check(presentations == 0, "full cache arrival before paint does not report first waveform");

    view.beginPresentationGeneration(); presentations = 0;
    view.setViewMode(WaveformViewComponent::ViewMode::spectral);
    check(model.getSpectrogramStatus() == WaveformModel::SpectrogramStatus::notRequested,
        "a retained spectral request waits for the new document CoreReady");
    WaveformPresentationTestAccess::deliverCacheNotification(view);
    check(presentations == 0, "spectral cache notification cannot fabricate a waveform presentation");
    view.beginPresentationGeneration(); presentations = 0;
    const auto image = view.createComponentSnapshot(view.getLocalBounds());
    check(image.isValid() && presentations == 1, "the initial real view paint presents source waveform before optional spectrum");
    view.createComponentSnapshot(view.getLocalBounds());
    check(presentations == 1, "repainting the same waveform is a one-shot milestone");
    check(view.getViewMode() == WaveformViewComponent::ViewMode::spectral,
        "the requested spectral mode is retained across the core presentation");
    view.notifyCoreReady();
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!model.isSpectrogramCacheReady() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::Thread::sleep(5);
    check(model.isSpectrogramCacheReady(), "actual spectrum becomes ready after the owning CoreReady notification");
    const auto spectralImage = view.createComponentSnapshot(view.getLocalBounds());
    check(spectralImage.isValid() && presentations == 1, "spectral painting does not fabricate another waveform milestone");
    view.beginPresentationGeneration(); presentations = 0;
    WaveformPresentationTestAccess::deliverCacheNotification(view);
    check(presentations == 0, "a new generation with retained caches still waits for its own paint");
    view.createComponentSnapshot(view.getLocalBounds());
    check(presentations == 1 && view.getViewMode() == WaveformViewComponent::ViewMode::spectral,
        "reopened retained audio paints the core waveform and preserves requested spectral mode");
    check(document.getState() == original, "presentation leaves document and Undo ownership unchanged");
    auto stream = artifacts.getChildFile("initial-presentation.png").createOutputStream();
    if (!stream || !juce::PNGImageFormat().writeImageToStream(image, *stream))
        throw std::runtime_error("Write presentation snapshot");
    stream.reset();
    view.setLookAndFeel(nullptr);
    auto* report = new juce::DynamicObject();
    report->setProperty("schema", "singleeditor.waveform-presentation-tests");
    report->setProperty("status", failures ? "failed" : "pass");
    report->setProperty("results", results);
    if (!artifacts.getChildFile("receipt.json").replaceWithText(juce::JSON::toString(juce::var(report), true)))
        throw std::runtime_error("Write waveform presentation receipt");
    std::cout << "Waveform presentation checks: " << results.size() << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}
}
