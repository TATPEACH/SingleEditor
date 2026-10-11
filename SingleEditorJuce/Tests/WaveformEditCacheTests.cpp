#include "Waveform/WaveformModel.h"
#include "Waveform/SpectrogramMapping.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void check(bool ok, const char* message)
{
    if (!ok)
    {
        std::cerr << "WaveformEditCacheTests failed: " << message << '\n';
        std::exit(1);
    }
}

template <typename Predicate>
void waitFor(Predicate ready, const char* message)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000.0;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    check(ready(), message);
}

uint64_t spectralPixels(const juce::Image& image)
{
    uint64_t hash = 1469598103934665603ULL;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            hash = (hash ^ image.getPixelAt(x, y).getARGB()) * 1099511628211ULL;
    return hash;
}
}

void runWaveformEditCacheTests()
{
    using namespace singleeditor;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("SingleEditorWaveformEditCacheTests")
        .getNonexistentChildFile("source", {});
    check(root.createDirectory(), "creates isolated fixture directory");
    const auto file = root.getChildFile("constant.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
            .withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
        check(writer != nullptr, "creates real WAV writer");
        juce::AudioBuffer<float> samples(2, 4800);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < samples.getNumSamples(); ++sample)
                samples.setSample(channel, sample, channel == 0 ? 0.5f : 0.25f);
        check(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples()), "writes fixture");
    }
    {
        SingleTrackDocument document;
        check(document.loadFile(file), "loads actual source document");
        const auto original = document.getState();
        WaveformModel model(document);
        const auto ready = [&] { return model.isPeakCacheReady() && !model.getPeakCache()->isPreview; };
        waitFor(ready, "initial full cache becomes ready");
        // This model-only suite explicitly admits persistence after peaks are
        // ready. The native suite exercises MainComponent's real CoreReady gate.
        waitFor([&]
        {
            model.dispatchPendingPeakCachePersistence();
            return WaveformModel::loadPersistentPeakCache(original->info) != nullptr;
        },
                "original peaks reach the persistent cache before editing");
        const auto firstPeak = [&] { return model.getPeakCache()->channelBins.front().front().maxValue; };
        check(std::abs(firstPeak() - 0.5f) < 0.001f, "original cached peaks reflect actual PCM");

        const auto spectralReady = [&] { return model.isSpectrogramCacheReady(); };
        model.ensureSpectrogramReady();
        waitFor(spectralReady, "original spectrogram becomes ready on explicit request");
        const auto originalSpectrum = spectralPixels(model.getSpectrogramCache()->image);
        waitFor([&] { return WaveformModel::loadPersistentSpectrogramCache(original) != nullptr; },
                "original spectrogram reaches disk before editing");

        check(document.applyFadeIn({ 0.0, 0.1 }), "performs a real same-length PCM edit");
        const auto faded = document.getState();
        check(faded->audioRevision != original->audioRevision && faded->info.numSamples == original->info.numSamples,
              "edit changes audio identity without changing file name or dimensions");
        waitFor(ready, "same-length fade rebuilds peaks instead of leaving an indefinite blank waveform");
        check(firstPeak() < 0.01f && model.getPeakCache()->channelBins.front().back().maxValue > 0.49f,
              "rebuilt peaks represent the fade, not relabelled original cached samples");
        check(WaveformModel::loadPersistentPeakCache(faded) == nullptr,
              "an unsaved working buffer cannot load the original file cache as its own peaks");
        model.ensureSpectrogramReady();
        waitFor(spectralReady, "fade rebuilds spectrogram for its own PCM revision");
        const auto fadedSpectrum = spectralPixels(model.getSpectrogramCache()->image);
        check(fadedSpectrum != originalSpectrum, "fade changes actual spectral pixels");
        check(WaveformModel::loadPersistentSpectrogramCache(faded) == nullptr,
              "an edited buffer cannot load the source file spectrum as its own");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1500);
        const auto diskAfterEdit = WaveformModel::loadPersistentPeakCache(original->info);
        check(diskAfterEdit != nullptr && std::abs(diskAfterEdit->channelBins.front().front().maxValue - 0.5f) < 0.001f,
              "edited peaks do not overwrite the original file's persistent peaks");
        const auto diskSpectrumAfterEdit = WaveformModel::loadPersistentSpectrogramCache(original);
        check(diskSpectrumAfterEdit != nullptr
                  && spectralPixels(diskSpectrumAfterEdit->image) == originalSpectrum,
              "edited spectral pixels do not overwrite the original file cache");

        check(document.undo(), "undoes the fade");
        waitFor(ready, "undo restores visible peaks for the restored audio revision");
        check(std::abs(firstPeak() - 0.5f) < 0.001f && !document.hasUnsavedChanges(),
              "undo restores actual original amplitude and clean document state");
        model.ensureSpectrogramReady();
        waitFor(spectralReady, "undo restores spectrum for the current revision");
        check(spectralPixels(model.getSpectrogramCache()->image) == originalSpectrum,
              "undo restores the original spectral pixels");
        check(document.redo(), "redoes the fade");
        waitFor(ready, "redo restores visible edited peaks");
        check(firstPeak() < 0.01f, "redo does not reuse the undo state's original peaks");
        model.ensureSpectrogramReady();
        waitFor(spectralReady, "redo restores edited spectrum");
        check(spectralPixels(model.getSpectrogramCache()->image) == fadedSpectrum,
              "redo restores the fade spectrum instead of cached original pixels");
        check(document.applySelectionGain({ 0.0, 0.1 }, 0.25f), "applies another equal-length edit");
        waitFor(ready, "successive gain edit rebuilds its own peaks");
        check(model.getPeakCache()->channelBins.front().back().maxValue < 0.126f,
              "successive same-dimension edits retain current sample truth");
        model.ensureSpectrogramReady();
        waitFor(spectralReady, "successive gain edit rebuilds spectral pixels");
        check(spectralPixels(model.getSpectrogramCache()->image) != fadedSpectrum,
              "gain changes the current spectrum");
    }

    // Validate the display scale against fresh production FFT images, not an
    // inverse-formula-only test. Neither document load nor ordinary peaks may
    // eagerly build the optional spectrum.
    for (const double sampleRate : { 44100.0, 96000.0 })
        for (const double toneHz : { 1000.0, 5000.0, 10000.0 })
        {
            const auto toneFile = root.getChildFile("tone-" + juce::String(sampleRate, 0)
                                                    + "-" + juce::String(toneHz, 0) + ".wav");
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::OutputStream> stream(toneFile.createOutputStream());
                auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
                    .withSampleRate(sampleRate).withNumChannels(1).withBitsPerSample(24));
                check(writer != nullptr, "creates real frequency-calibration WAV");
                juce::AudioBuffer<float> samples(1, 8192);
                for (int sample = 0; sample < samples.getNumSamples(); ++sample)
                    samples.setSample(0, sample, 0.6f * static_cast<float>(std::sin(
                        juce::MathConstants<double>::twoPi * toneHz * sample / sampleRate)));
                check(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples()), "writes calibration PCM");
            }
            SingleTrackDocument toneDocument;
            check(toneDocument.loadFile(toneFile), "loads actual calibration document");
            WaveformModel toneModel(toneDocument);
            check(toneModel.getSpectrogramCache() == nullptr, "ordinary audio still does not eagerly request a spectrum");
            toneModel.ensureSpectrogramReady();
            waitFor([&] { return toneModel.isSpectrogramCacheReady(); }, "calibration FFT becomes ready on demand");
            const auto cache = toneModel.getSpectrogramCache();
            const auto& image = cache->image;
            int peakRow = 0;
            float brightest = -1.0f;
            for (int row = 0; row < image.getHeight(); ++row)
            {
                const auto energyColour = image.getPixelAt(image.getWidth() / 2, row).getBrightness();
                if (energyColour > brightest) { brightest = energyColour; peakRow = row; }
            }
            const auto labelRow = SpectrogramMapping::normalisedYForFrequency(toneHz, cache->sampleRate)
                * image.getHeight() - 0.5f;
            if (std::abs(labelRow - peakRow) > 2.0f)
                std::cerr << "Frequency mapping: rate=" << sampleRate << " tone=" << toneHz
                          << " label row=" << labelRow << " FFT peak row=" << peakRow << '\n';
            check(brightest > 0.65f && std::abs(labelRow - peakRow) <= 2.0f,
                  "frequency labels align with the actual tone ridge within the FFT/raster resolution");
            check(!toneDocument.hasUnsavedChanges(), "presentation verification does not edit the audio document");
        }

    {
        // Drive a real terminal source-availability failure, not a mocked cache
        // result. Existing peaks remain usable and retry runs the production FFT.
        using Status = WaveformModel::SpectrogramStatus;
        const auto failureFile = root.getChildFile("availability-failure.wav");
        check(file.copyFileTo(failureFile), "creates an uncached owned failure fixture");
        SingleTrackDocument document;
        check(document.loadFile(failureFile), "opens real failure fixture");
        WaveformModel model(document);
        waitFor([&] { return model.isPeakCacheReady() && !model.getPeakCache()->isPreview; },
                "ordinary waveform is ready before the optional failure");
        const auto peaks = model.getPeakCache();
        const auto state = document.getState();
        check(model.getSpectrogramStatus() == Status::notRequested, "spectrum starts unrequested");
        state->playbackAvailability.markUnavailable();
        model.ensureSpectrogramReady();
        waitFor([&] { return model.getSpectrogramStatus() == Status::failed; },
                "unavailable source produces a terminal spectral failure");
        check(model.getSpectrogramCache() == nullptr && model.getPeakCache() == peaks && model.hasWaveform(),
              "failed spectrum retains the real ordinary waveform and has no false image");
        state->playbackAvailability.markRandomAccess();
        for (int i = 0; i < 8; ++i)
        {
            model.ensureSpectrogramReady();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        }
        check(model.getSpectrogramStatus() == Status::failed, "notifications do not create an automatic retry loop");
        const auto originalRevision = state->audioRevision;
        document.insertEnvelopePoint(0.03);
        check(document.getState()->audioRevision == originalRevision, "envelope notification keeps PCM identity");
        model.ensureSpectrogramReady();
        check(model.getSpectrogramStatus() == Status::failed, "envelope notifications retain terminal failure");
        model.ensureSpectrogramReady(true);
        waitFor([&] { return model.getSpectrogramStatus() == Status::ready && model.isSpectrogramCacheReady(); },
                "explicit retry produces the real spectrum after source recovery");
        check(model.getSpectrogramCache()->audioRevision == originalRevision,
              "retry publishes only the matching audio revision");

        // A pending optional job must not keep a cleared/replaced document alive
        // as a visible loading/error state. Reuse real peaks to avoid gating the
        // optional worker behind an unrelated progressive peak build.
        const auto pendingFile = root.getChildFile("availability-pending.wav");
        check(file.copyFileTo(pendingFile), "creates a fresh pending fixture");
        auto pending = SingleTrackDocument::createStateFromFile(pendingFile);
        check(pending != nullptr, "prepares pending source");
        pending->playbackAvailability = SingleTrackDocument::PlaybackAvailability::contiguousPrefix(1000);
        model.prepareIncomingCaches(std::make_shared<WaveformModel::PeakCache>(*peaks));
        check(document.adoptLoadedState(pending), "adopts progressive source with real incoming peaks");
        model.ensureSpectrogramReady();
        check(model.getSpectrogramStatus() == Status::preparing, "progressive request remains pending truthfully");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        document.clear();
        pending->playbackAvailability.markUnavailable();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
        check(model.getSpectrogramStatus() == Status::notRequested && model.getSpectrogramCache() == nullptr,
              "obsolete failure cannot repopulate a cleared document");
        check(document.loadFile(file), "opens a replacement after cancellation");
        model.ensureSpectrogramReady();
        waitFor([&] { return model.getSpectrogramStatus() == Status::ready && model.isSpectrogramCacheReady(); },
                "replacement spectrum remains usable after cancellation");
        check(model.getSpectrogramCache()->audioRevision == document.getState()->audioRevision,
              "replacement never receives the previous job's result");
        check(!document.hasUnsavedChanges(), "replacement workflow did not edit audio");
    }

    // Cross the real 64 MiB eager-map threshold using bounded writer blocks.
    // This exercises a worker-owned reader with an initially empty mapping.
    const auto largeFile = root.getChildFile("section-mapped.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(largeFile.createOutputStream());
        auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions()
            .withSampleRate(96000.0).withNumChannels(2).withBitsPerSample(16));
        check(writer != nullptr, "creates section-mapped fixture writer");
        juce::AudioBuffer<float> block(2, 8192);
        for (int channel = 0; channel < block.getNumChannels(); ++channel)
            for (int sample = 0; sample < block.getNumSamples(); ++sample)
                block.setSample(channel, sample, 0.5f * std::sin(juce::MathConstants<float>::twoPi
                                                               * static_cast<float>(sample) / 64.0f));
        for (int i = 0; i < 2049; ++i)
            check(writer->writeFromAudioSampleBuffer(block, 0, block.getNumSamples()), "writes large fixture block");
    }
    {
        SingleTrackDocument document;
        check(document.loadFile(largeFile), "loads the real section-mapped source");
        const auto state = document.getState();
        check(state->memoryMappedReader != nullptr && state->memoryMappedReader->getMappedSection().isEmpty(),
              "large source initially has no eagerly mapped PCM section");
        WaveformModel model(document);
        model.ensureSpectrogramReady();
        waitFor([&] { return model.isSpectrogramCacheReady(); },
                "large-file spectrum maps requested FFT windows and becomes ready");
        check(state->memoryMappedReader->getMappedSection().isEmpty(),
              "spectral worker does not remap the playback reader");
    }
    check(root.deleteRecursively(), "removes only the owned fixture directory");
    std::cout << "WaveformEditCacheTests passed (PCM/spectral fade/gain, persistent cache, undo/redo, section mapping)\n";
}
