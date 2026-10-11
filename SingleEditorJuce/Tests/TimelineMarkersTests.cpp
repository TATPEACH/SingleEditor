#include "UI/Components/TimelineMarkersComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace
{
void requireMarker(bool result, const char* message)
{
    if (result) return;
    std::cerr << "TimelineMarkersTests failed: " << message << '\n';
    std::exit(1);
}
}

void runTimelineMarkersTests()
{
    using namespace singleeditor;
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile("out/modern-glass-marker-renders");
    requireMarker(output.createDirectory().wasOk(), "create marker evidence directory");
    const auto fixture = output.getNonexistentChildFile("timeline-markers-fixture", ".wav", false);
    std::unordered_map<juce::String, juce::String> metadata {
        { "NumCuePoints", "2" }, { "Cue0Identifier", "41" }, { "Cue0Offset", "132000" },
        { "Cue1Identifier", "17" }, { "Cue1Offset", "60000" }, { "NumCueLabels", "2" },
        { "CueLabel0Identifier", "17" }, { "CueLabel0Text", "First phrase" },
        { "CueLabel1Identifier", "41" }, { "CueLabel1Text", "Second phrase" }
    };
    juce::WavAudioFormat format;
    {
        auto fileStream = fixture.createOutputStream();
        requireMarker(fileStream != nullptr && fileStream->setPosition(0) && fileStream->truncate().wasOk(), "replace isolated WAV fixture");
        std::unique_ptr<juce::OutputStream> stream(std::move(fileStream));
        auto options = juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(1)
                         .withBitsPerSample(16).withMetadataValues(metadata);
        auto writer = format.createWriterFor(stream, options);
        requireMarker(writer != nullptr, "create real WAV writer with embedded cues");
        juce::AudioBuffer<float> samples(1, 192000);
        for (int i = 0; i < samples.getNumSamples(); ++i)
            samples.setSample(0, i, static_cast<float>(0.06 * std::sin(i * 0.07) * std::sin(i * 0.00002)));
        requireMarker(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples()), "write real cue fixture");
    }
    auto input = fixture.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(input.release(), true));
    requireMarker(reader != nullptr, "read embedded cue metadata through the real format reader");
    auto source = std::make_shared<SingleTrackDocument::State>();
    source->audioRevision = 1;
    source->info.file = fixture;
    source->info.sampleRate = reader->sampleRate;
    source->info.numSamples = reader->lengthInSamples;
    source->info.durationSeconds = reader->lengthInSamples / reader->sampleRate;
    source->info.numChannels = static_cast<int>(reader->numChannels);
    source->info.bitsPerSample = static_cast<int>(reader->bitsPerSample);
    source->info.metadata = reader->metadataValues;
    reader.reset();
    std::cout << "TimelineMarkerFixture=" << fixture.getFullPathName() << '\n';

    ModernAppleLookAndFeel lookAndFeel;
    TimelineMarkersComponent panel;
    panel.setLookAndFeel(&lookAndFeel);
    panel.setDocument(source);
    requireMarker(panel.getNumRows() == 7 && !panel.hasUnreadableCues(), "two actual cues plus five timeline references");
    requireMarker(panel.rowAt(1).label == "First phrase" && panel.rowAt(1).seconds == 1.25
                      && panel.rowAt(2).label == "Second phrase" && panel.rowAt(2).seconds == 2.75,
                  "cue labels join by identifier and rows sort by real sample offsets");
    requireMarker(!panel.rowAt(3).available && !panel.rowAt(5).available && panel.rowAt(6).seconds == 4.0,
                  "absent selection remains unavailable while file end is exact");
    panel.setTransport(1.234, juce::Range<double>(0.5, 3.0));
    requireMarker(panel.rowAt(3).seconds == 0.5 && panel.rowAt(4).seconds == 1.234 && panel.rowAt(5).seconds == 3.0,
                  "live references derive from the actual selection and playhead");
    double seek = -1;
    panel.onSeek = [&](double value) { seek = value; };
    panel.returnKeyPressed(1);
    requireMarker(seek == 1.25, "activation routes the real cue time");
    panel.returnKeyPressed(5);
    requireMarker(seek == 3.0, "selection end can be activated without changing its range");
    panel.setTransport(100, juce::Range<double>(-2, 5));
    requireMarker(panel.rowAt(3).seconds == 0 && panel.rowAt(4).seconds == 4 && panel.rowAt(5).seconds == 4,
                  "transport and selection cannot expose times outside the file");
    panel.setTransport(std::numeric_limits<double>::quiet_NaN(), {});
    panel.returnKeyPressed(3);
    requireMarker(seek == 3.0 && !panel.rowAt(3).available && panel.rowAt(4).seconds == 0,
                  "invalid transport is sanitized and disabled selection never dispatches");

    auto changed = std::make_shared<SingleTrackDocument::State>(*source);
    // A new immutable state must refresh even if a reopened file repeats a revision number.
    changed->info.metadata.set("Cue1Offset", "24000");
    panel.setDocument(changed);
    requireMarker(panel.rowAt(1).seconds == 0.5, "new source identity refreshes embedded offsets even at equal revision");
    changed = std::make_shared<SingleTrackDocument::State>(*source);
    changed->info.metadata.set("Cue1Offset", "invalid");
    panel.setDocument(changed);
    requireMarker(panel.hasUnreadableCues() && panel.getNumRows() == 6 && panel.rowAt(1).seconds == 2.75,
                  "malformed cue is not invented at zero and other real cues survive");
    changed = std::make_shared<SingleTrackDocument::State>(*source);
    changed->info.metadata.set("Cue1BlockStart", "128");
    panel.setDocument(changed);
    requireMarker(panel.hasUnreadableCues() && panel.getNumRows() == 6, "unsupported block-relative cue is not treated as a file offset");
    changed = std::make_shared<SingleTrackDocument::State>(*source);
    changed->info.metadata.set("NumCuePoints", "9223372036854775807");
    panel.setDocument(changed);
    requireMarker(panel.hasUnreadableCues() && panel.getNumRows() == 5, "hostile declared count never drives an unbounded loop");
    panel.setDocument({});
    panel.returnKeyPressed(0);
    requireMarker(seek == 3.0 && panel.getNumRows() == 5 && !panel.rowAt(0).available && !panel.rowAt(4).available,
                  "clearing a document clears stale marker actions");

    panel.setDocument(source);
    panel.setTransport(1.234, juce::Range<double>(0.5, 3.0));
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        lookAndFeel.refreshColours();
        for (const int width : { 140, 240, 360 })
            for (const int height : { 180, 320 })
            {
                panel.setSize(width, height);
                const auto image = panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.5f);
                auto stream = output.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                    + juce::String(width) + "x" + juce::String(height) + ".png").createOutputStream();
                requireMarker(stream != nullptr && stream->setPosition(0)
                                  && juce::PNGImageFormat().writeImageToStream(image, *stream) && stream->truncate().wasOk(),
                              "replace current marker visual evidence");
                ++renders;
            }
    }
    panel.setLookAndFeel(nullptr);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "TimelineMarkersTests passed (real cue WAV, live selection, source changes, invalid metadata, " << renders << " renders)\n";
}
