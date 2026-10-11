#include "UI/Components/WaveformPlotGeometry.h"
#include "UI/LookAndFeel/ModernWaveformPainter.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/Renderer/RendererSelection.h"
#include <iostream>
#include <cstdlib>

namespace
{
void runSpectralPresentationTests()
{
    using namespace singleeditor;
    const auto check = [](bool ok, const char* message)
    {
        if (!ok) { std::cerr << "SpectralPresentationTests failed: " << message << '\n'; std::exit(1); }
    };
    // Frozen v1 raster arithmetic is compatibility data: factoring the mapping
    // for a ruler must not change a single row's FFT-bin selection.
    for (int row = 0; row < 256; ++row)
    {
        const auto y = static_cast<float>(row) / 255.0f;
        const auto original = (1.0f - std::pow(y, 2.6f)) * 510.0f;
        check(SpectrogramMapping::binAtNormalisedY(y) == original,
              "frequency presentation preserves every v1 FFT-bin coordinate");
    }
    int combinations = 0;
    const auto font = ModernTypography::font(10.0f);
    for (const double sampleRate : { 8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const int width : { 180, 433, 1600 })
            for (const int height : { 58, 100, 300, 1200 })
            {
                const juce::Rectangle<float> plot(17, 31, static_cast<float>(width), static_cast<float>(height));
                const auto lane = WaveformPlotGeometry::content(plot, true);
                const auto unit = WaveformPlotGeometry::spectralUnitBounds(lane);
                const auto ticks = WaveformPlotGeometry::frequencyTicks(lane, sampleRate);
                check(!ticks.empty() && plot.contains(unit), "valid spectral view has a contained frequency scale");
                for (const auto& tick : ticks)
                {
                    check(plot.contains(tick.label) && !lane.intersects(tick.label), "Hz values occupy the existing gutter");
                    check(!unit.intersects(tick.label), "frequency unit never collides with a value");
                    check(juce::GlyphArrangement::getStringWidth(font, tick.text) <= tick.label.getWidth()
                        && font.getHeight() <= tick.label.getHeight(), "whole frequency values fit at the actual em size");
                    check(tick.frequency <= sampleRate * 0.5 && tick.y > lane.getY() && tick.y < lane.getBottom(),
                          "no label claims a frequency above Nyquist or beyond the raster");
                    for (const auto& other : ticks)
                        if (&other != &tick) check(!tick.label.intersects(other.label), "frequency values never overlap");
                }
                ++combinations;
            }
    check(WaveformPlotGeometry::frequencyTicks({0, 0, 400, 200}, 0).empty(), "unknown rate has no invented frequency scale");
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_SPECTRAL_AXES_OFFSCREEN_DIR", {});
    int renders = 0;
    RendererSelection::resetForTests(RendererKind::modern);
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const auto size : { juce::Point<int>(104, 60), juce::Point<int>(180, 74),
                                juce::Point<int>(433, 360), juce::Point<int>(900, 620) })
            for (const double sampleRate : { 44100.0, 96000.0 })
                for (const int presentation : { 0, 1, 2 })
                {
                    const auto preparing = presentation == 1;
                    const auto failed = presentation == 2;
                    RendererVisualStyle::setModernTheme(theme);
                    const juce::Rectangle<float> plot(9, 11, static_cast<float>(size.x), static_cast<float>(size.y));
                    const auto lane = WaveformPlotGeometry::content(plot, true);
                    const auto status = ModernWaveformPainter::spectralStatusLayout(lane, failed);
                    check(lane.contains(status.background) && status.background.contains(status.icon), "preparation badge/icon are contained");
                    if (!status.text.isEmpty())
                    {
                        check(status.background.contains(status.text) && !status.text.intersects(status.icon), "badge text stays separate from its icon");
                        const auto statusFont = ModernTypography::font(status.textEm);
                        check(juce::GlyphArrangement::getStringWidth(statusFont, status.message) <= status.text.getWidth()
                            && statusFont.getHeight() <= status.text.getHeight(), "preparation text is never squeezed or clipped");
                    }
                    juce::Image picture(juce::Image::ARGB, size.x + 20, size.y + 24, true);
                    {
                        juce::Graphics g(picture);
                        g.setColour(RendererVisualStyle::panelBackground()); g.fillRect(plot);
                        // Calibration background, not fake native spectrum evidence.
                        for (int y = 0; y < static_cast<int>(lane.getHeight()); ++y)
                        {
                            g.setColour(juce::Colour::fromHSV(0.65f, 0.5f, 0.15f + 0.5f * y / lane.getHeight(), 1.0f));
                            g.drawHorizontalLine(juce::roundToInt(lane.getY()) + y, lane.getX(), lane.getRight());
                        }
                        if (preparing || failed) ModernWaveformPainter::paintSpectralStatus(g, lane, failed);
                        else ModernWaveformPainter::paintSpectralAxis(g, plot, sampleRate);
                    }
                    check(picture.getPixelAt(0, 0).isTransparent()
                        && picture.getPixelAt(size.x + 19, size.y + 23).isTransparent(), "spectral presentation cannot paint outside its plot");
                    if (output.isNotEmpty())
                    {
                        const juce::File directory(output);
                        check(directory.createDirectory().wasOk(), "create spectral presentation evidence directory");
                        const auto name = juce::String(theme == ModernTheme::light ? "light" : "night")
                            + "-" + juce::String(size.x) + "x" + juce::String(size.y) + "-"
                            + juce::String(juce::roundToInt(sampleRate))
                            + (failed ? "-failed.png" : preparing ? "-preparing.png" : "-axis.png");
                        auto stream = directory.getChildFile(name).createOutputStream();
                        check(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                            && juce::PNGImageFormat().writeImageToStream(picture, *stream), "write spectral painter evidence");
                    }
                    ++renders;
                }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "SpectralPresentationTests passed (" << combinations << " frequency layouts, " << renders << " renders)\n";
}
}

void runWaveformPlotGeometryTests()
{
    using namespace singleeditor;
    using Geometry = WaveformPlotGeometry;
    const auto check = [](bool ok, const char* message)
    {
        if (!ok) { std::cerr << "WaveformPlotGeometryTests failed: " << message << '\n'; std::exit(1); }
    };
    int combinations = 0;
    for (const float width : { 140.0f, 180.0f, 220.0f, 433.0f, 720.0f, 1600.0f })
        for (const float height : { 42.0f, 64.0f, 100.0f, 140.0f, 280.0f })
            for (const float zoom : { 0.5f, 1.0f, 2.0f, 12.0f })
            {
                const juce::Rectangle<float> plot(8, 56, width, height);
                const auto lane = Geometry::content(plot, true);
                check(plot.contains(lane), "waveform stays inside the plot");
                check(Geometry::content(plot, false) == plot.reduced(18, 12), "Classic geometry is unchanged");
                if (width >= 180)
                {
                    const auto ticks = Geometry::amplitudeTicks(lane, zoom);
                    for (const auto& tick : ticks)
                    {
                        check(plot.contains(tick.label) && !lane.intersects(tick.label), "dB labels occupy their reserved gutter");
                        check(tick.label.getHeight() >= ModernTypography::font(10.0f).getHeight(),
                              "dB label bounds accommodate the actual em font height");
                        for (const auto& other : ticks)
                            if (&tick != &other) check(!tick.label.intersects(other.label), "dB labels never overlap at compact sizes");
                        if (tick.zero) check(tick.y == lane.getCentreY(), "zero amplitude is the centre line");
                        else
                        {
                            const auto amplitude = static_cast<float>(std::pow(10.0, tick.db / 20.0));
                            const auto value = tick.y < lane.getCentreY() ? amplitude : -amplitude;
                            check(std::abs(Geometry::sampleToY(value, lane, zoom) - tick.y) < 0.0001f,
                                  "each dB mark coincides with the real peak mapping after zoom");
                            check(tick.y != lane.getCentreY(), "0 dBFS cannot be confused with zero amplitude");
                        }
                    }
                }
                const juce::Range<double> visible(52.831, 97.156);
                for (const auto fraction : { 0.0, 0.1, 0.5, 0.9, 1.0 })
                {
                    const auto time = visible.getStart() + visible.getLength() * fraction;
                    const auto x = Geometry::timeToX(time, lane, visible);
                    check(std::abs(Geometry::xToTime(x, lane, visible) - time) < 0.00001,
                          "time drawing and selection hit coordinates round-trip with the axis gutter");
                }
                check(Geometry::xToTime(lane.getRight() + 20, lane, visible) == visible.getEnd(), "right edge remains clamped");
                ++combinations;
            }
    const juce::Rectangle<float> lane(0, 0, 400, 200);
    check(Geometry::sampleToY(1.5f, lane, 0.5f) < Geometry::sampleToY(1.0f, lane, 0.5f),
          "amplitude zoom-out reveals float or gained peaks above full scale rather than clipping them before zoom");
    check(Geometry::sampleToY(3.0f, lane, 1.0f) == lane.getY(), "only the physical viewport clips tall peaks");
    for (const double span : { 0.0000104, 0.002, 0.1, 1.0, 239.353, 3600.0, 2160000.0 })
        for (const float width : { 140.0f, 400.0f, 1600.0f, 7680.0f })
        {
            const auto step = Geometry::rulerStep(span, width);
            check(step > 0 && std::isfinite(step) && step / span * width >= 99.99,
                  "major time ticks reserve readable label spacing from tiny clips to long recordings");
        }
    check(Geometry::formatTime(52.831, 0.001) == "00:52.831", "selection millisecond format remains exact");
    check(Geometry::formatTime(0.0012, 0.0001) == "00:00.0012", "deep zoom labels do not repeat at sub-millisecond intervals");
    check(Geometry::formatTime(59.99996, 0.0001) == "01:00.0000", "time rounding carries across minute boundaries");
    check(Geometry::formatTime(2160000.125, 0.001) == "600:00:00.125", "long recordings do not overflow a 32-bit millisecond counter");
    const auto rulerFont = ModernTypography::font(11.0f);
    const juce::Rectangle<float> ruler(10, 8, 320, 22), wave(20, 40, 294, 100);
    for (const auto text : { "00:00", "03:59", "00:00.0012", "600:00:00.125" })
    {
        const auto textWidth = juce::GlyphArrangement::getStringWidth(rulerFont, text);
        const auto bounds = Geometry::rulerLabelBounds(30, ruler, wave, textWidth);
        check(!bounds.isEmpty() && bounds.getWidth() >= textWidth && ruler.contains(bounds),
              "complete time labels fit their reserved ruler cells at the real em size");
        check(Geometry::rulerLabelBounds(wave.getRight() - textWidth + 2, ruler, wave, textWidth).isEmpty(),
              "terminal time labels are omitted rather than painting partial values");
        check(!Geometry::rulerLabelBounds(wave.getRight() - textWidth - 3, ruler, wave, textWidth).isEmpty(),
              "a complete terminal value remains visible when it exactly fits");
    }
    for (const auto text : { "+12", "-36", "0", "\xe2\x88\x92\xe2\x88\x9e" })
        check(juce::GlyphArrangement::getStringWidth(ModernTypography::font(10.0f), juce::String::fromUTF8(text)) <= 21.0f,
              "full dB and negative-infinity labels fit the existing amplitude gutter");

    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_WAVEFORM_AXES_OFFSCREEN_DIR", {});
    RendererSelection::resetForTests(RendererKind::modern);
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const int height : { 64, 160, 300 })
            for (const float zoom : { 0.5f, 1.0f, 2.0f })
            {
                RendererVisualStyle::setModernTheme(theme);
                juce::Image image(juce::Image::RGB, 500, height, true);
                {
                juce::Graphics g(image);
                g.fillAll(RendererVisualStyle::panelBackground());
                const auto plot = Geometry::content(image.getBounds().toFloat(), true);
                ModernWaveformPainter::prepareLane(g, plot, RendererVisualStyle::waveformBlue());
                for (int x = 0; x < static_cast<int>(plot.getWidth()); ++x)
                {
                    const float amplitude = 0.1f + static_cast<float>(x) / plot.getWidth() * 1.4f;
                    g.drawVerticalLine(juce::roundToInt(plot.getX()) + x,
                        Geometry::sampleToY(amplitude, plot, zoom), Geometry::sampleToY(-amplitude, plot, zoom));
                }
                g.setColour(RendererVisualStyle::secondaryText());
                g.setFont(ModernTypography::font(10.0f));
                for (const auto& tick : Geometry::amplitudeTicks(plot, zoom))
                {
                    g.drawLine(plot.getRight(), tick.y, plot.getRight() + 3, tick.y, 1);
                    g.drawText(tick.zero ? juce::String::fromUTF8("−∞") : juce::String(juce::roundToInt(tick.db)),
                        tick.label, juce::Justification::centredLeft, false);
                }
                }
                if (output.isNotEmpty())
                {
                    const juce::File directory(output);
                    check(directory.createDirectory().wasOk(), "create axis render directory");
                    auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light" : "night")
                        + "-" + juce::String(height) + "-zoom" + juce::String(zoom, 1) + ".png").createOutputStream();
                    check(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                        && juce::PNGImageFormat().writeImageToStream(image, *stream), "write axis render evidence");
                }
            }
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    runSpectralPresentationTests();
    std::cout << "WaveformPlotGeometryTests passed (" << combinations << " layouts, real dB mapping, time transforms, 18 renders)\n";
}
