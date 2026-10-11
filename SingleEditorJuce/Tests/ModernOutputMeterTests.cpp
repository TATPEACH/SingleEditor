#include "UI/LookAndFeel/ModernOutputMeterPainter.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "ModernOutputMeterTests failed: " << message << '\n';
    std::exit(1);
}
class MeterFixture final : public juce::Component
{
public:
    OutputMeterDisplayState display;
    AudioEngine::MeterMode mode = AudioEngine::MeterMode::truePeak;
    juce::TextButton reset { "Reset" };
    MeterFixture() { addAndMakeVisible(reset); }
    void resized() override { reset.setBounds(ModernOutputMeterPainter::layout(getLocalBounds(), display.snapshot.channelCount).reset); }
    void paint(juce::Graphics& g) override { ModernOutputMeterPainter::paint(g, getLocalBounds(), display, mode); }
};
}

void runModernOutputMeterTests()
{
    using Mode = AudioEngine::MeterMode;
    OutputMeterDisplayState meter;
    OutputMeterSnapshot source;
    source.channelCount = 2;
    source.roles[0] = SpeakerRole::L;
    source.roles[1] = SpeakerRole::R;
    source.truePeakLinear[0] = 0.5f;
    source.truePeakLinear[1] = 0.25f;
    source.rmsLinear[0] = 0.25f;
    source.momentaryLufs = -22.0f;
    source.kWeightedContributionDb[0] = -26.0f;
    check(std::abs(OutputMeterDisplayState::maximum(source, true) + 6.0206f) < 0.001f
              && std::abs(OutputMeterDisplayState::maximum(source, false) + 12.0412f) < 0.001f,
          "TP and RMS present their actual separate measurements");
    meter.advance(source, Mode::truePeak);
    check(meter.levels[0] > meter.levels[1] && meter.peakSinceResetDb < -6.0f, "channels remain independent");
    const auto originalPeak = meter.peakSinceResetDb;
    source.truePeakLinear.fill(0.0f);
    source.rmsLinear.fill(0.0f);
    source.kWeightedContributionDb.fill(-120.0f);
    source.momentaryLufs = -120.0f;
    for (int i = 0; i < 250; ++i) meter.advance(source, Mode::truePeak);
    check(meter.levels[0] == 0 && meter.trails[0] == 0 && meter.peakSinceResetDb == originalPeak,
          "bars settle at silence while resettable peak retains the real maximum");
    check(!meter.advance(source, Mode::truePeak), "unchanged silence requests no repaint");
    meter.resetHold();
    check(meter.peakSinceResetDb == -120 && meter.snapshot.channelCount == 2,
          "reset only clears visual hold, not the audio/device snapshot");
    source.truePeakLinear[0] = 0.125f;
    meter.advance(source, Mode::truePeak);
    check(meter.peakSinceResetDb < -18.0f && meter.peakSinceResetDb > -18.1f,
          "new peaks after reset start a new hold interval");
    source.momentaryLufs = -8.0f;
    source.kWeightedContributionDb[0] = -40.0f;
    meter.resetBallistics();
    meter.advance(source, Mode::lufs);
    check(std::abs(meter.levels[0] - (14.0f / 54.0f) * 0.62f) < 0.0001f,
          "loudness bars use per-channel K contribution, not global momentary LUFS copied into each bar");

    for (int width = 100; width <= 420; ++width)
        for (int height : { 55, 80, 120, 150, 260, 360, 600 })
            for (int channels = 0; channels <= 12; ++channels)
            {
                const juce::Rectangle<int> bounds(0, 0, width, height);
                const auto layout = ModernOutputMeterPainter::layout(bounds, channels);
                if (height >= 120)
                    check(layout.bars.size() == static_cast<std::size_t>(channels),
                          "compact meter retains every channel bar before optional device detail");
                for (const auto area : { layout.primary, layout.reset, layout.secondary, layout.topology })
                    check(area.isEmpty() || bounds.contains(area), "meter readouts remain inside compact bounds");
                check(!layout.primary.intersects(layout.reset) && !layout.secondary.intersects(layout.topology),
                      "readouts and reset do not overlap");
                for (std::size_t ch = 0; ch < layout.bars.size(); ++ch)
                {
                    const auto bar = layout.bars[ch];
                    check(bounds.toFloat().contains(bar) && bar.getWidth() <= 12.01f,
                          "slender bars fit every supported physical channel count");
                    if (ch > 0) check(!bar.intersects(layout.bars[ch - 1]), "adjacent bars do not overlap");
                }
                if (!layout.bars.empty())
                {
                    check(std::abs(layout.channelLabels.getCentreX() - bounds.getCentreX()) <= 1,
                          "scale and channel group remain centred beneath the readouts");
                    for (const auto bar : layout.bars)
                        check(bar.getY() == layout.bars.front().getY()
                                  && bar.getHeight() == layout.bars.front().getHeight(),
                              "every channel uses the same vertical baseline and extent");
                    if (channels == 8 && width >= 300)
                        check(layout.bars.back().getRight() - layout.bars.front().getX() > 230,
                              "eight outputs spread across a wide Mag instead of parking in one corner");
                }
            }
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel look;
    const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile("out/modern-glass-output-meter");
    check(directory.createDirectory().wasOk(), "create output-meter visual evidence folder");
    for (auto theme : { ModernTheme::light, ModernTheme::night })
        for (auto mode : { Mode::truePeak, Mode::rms, Mode::lufs })
            for (int channels : { 2, 8, 12 })
                for (auto size : { juce::Point<int>(140, 120), { 240, 160 }, { 300, 400 }, { 360, 600 } })
                {
                    RendererVisualStyle::setModernTheme(theme);
                    MeterFixture fixture;
                    fixture.setLookAndFeel(&look);
                    fixture.mode = mode;
                    auto& snapshot = fixture.display.snapshot;
                    snapshot.channelCount = channels;
                    snapshot.roles[0] = SpeakerRole::L; snapshot.roles[1] = SpeakerRole::R;
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        snapshot.truePeakLinear[ch] = juce::Decibels::decibelsToGain(-2.0f - static_cast<float>(ch) * 3.0f);
                        snapshot.rmsLinear[ch] = snapshot.truePeakLinear[ch] * 0.5f;
                        snapshot.kWeightedContributionDb[ch] = -12.0f - static_cast<float>(ch) * 3.0f;
                        fixture.display.levels[ch] = 0.9f - static_cast<float>(ch) * 0.05f;
                        fixture.display.trails[ch] = fixture.display.levels[ch] + 0.03f;
                    }
                    snapshot.momentaryLufs = -11.4f;
                    fixture.display.peakSinceResetDb = -0.6f;
                    fixture.setSize(size.x, size.y);
                    const auto image = fixture.createComponentSnapshot(fixture.getLocalBounds(), true, 1.5f);
                    auto stream = directory.getChildFile(juce::String(theme == ModernTheme::night ? "night" : "light")
                        + "-" + juce::String(static_cast<int>(mode)) + "-" + juce::String(channels)
                        + "-" + juce::String(size.x) + ".png").createOutputStream();
                    check(stream && stream->setPosition(0) && juce::PNGImageFormat().writeImageToStream(image, *stream)
                        && stream->truncate().wasOk(), "write meter fixture evidence");
                    fixture.setLookAndFeel(nullptr);
                }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "ModernOutputMeterTests passed (29211 layouts, hold/settling/mode semantics, 72 renders)\n";
}
