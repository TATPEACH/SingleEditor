#include "UI/Components/WaveformOverviewComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

void runWaveformOverviewTests()
{
    using namespace singleeditor;
    const auto check = [](bool ok, const char* message)
    {
        if (!ok) { std::cerr << "WaveformOverviewTests failed: " << message << '\n'; std::exit(1); }
    };
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel lookAndFeel;
    WaveformOverviewComponent overview;
    overview.setLookAndFeel(&lookAndFeel);
    overview.setVisible(true);
    auto cache = std::make_shared<WaveformModel::PeakCache>();
    cache->durationSeconds = 100;
    cache->sampleRate = 48000;
    cache->totalSamples = 4800000;
    cache->loadedSampleRange = { 0, 960000 };
    cache->audioRevision = 7;
    cache->isPreview = true;
    cache->binCount = 200;
    for (int i = 0; i < 200; ++i)
        cache->mixedBins.push_back({ -0.8f, 0.8f, i * 0.1, (i + 1) * 0.1 });
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_OVERVIEW_OFFSCREEN_DIR", {});
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const int width : { 180, 220, 360, 720, 1024, 1800 })
        {
            RendererVisualStyle::setModernTheme(theme);
            ModernAppleLookAndFeel::refreshTheme(overview);
            overview.setBounds(0, 0, width, 26);
            overview.setState(cache, 7, 100, { 20, 40 }, 5);
            const auto track = overview.getTrackBounds();
            const auto viewport = overview.getViewportBounds();
            check(track.getWidth() >= 52 && track.getHeight() >= 18, "compact overview retains a usable track");
            check(std::abs(viewport.getWidth() / track.getWidth() - 0.2f) < 0.001,
                  "viewport is scaled to the complete source duration, not loaded preview length");
            check(std::abs((viewport.getX() - track.getX()) / track.getWidth() - 0.2f) < 0.001,
                  "viewport position uses the same full-file coordinate system");
            for (auto* child : overview.getChildren())
            {
                check(overview.getLocalBounds().contains(child->getBounds()) && child->getHeight() >= 22,
                      "all three navigation buttons fit and remain usable");
                check(!child->getBounds().toFloat().intersects(track), "buttons do not overlap overview");
                for (auto* other : overview.getChildren())
                    if (other != child) check(!child->getBounds().intersects(other->getBounds()), "buttons never overlap");
            }
            const auto preview = overview.createComponentSnapshot(overview.getLocalBounds());
            overview.setState(cache, 8, 100, { 20, 40 }, 5);
            const auto stale = overview.createComponentSnapshot(overview.getLocalBounds());
            const int knownX = juce::roundToInt(track.getX() + track.getWidth() * 0.1f);
            const int unknownX = juce::roundToInt(track.getX() + track.getWidth() * 0.8f);
            const int y = juce::roundToInt(track.getCentreY() - 4);
            check(preview.getPixelAt(knownX, y) != stale.getPixelAt(knownX, y),
                  "real loaded peaks are visible and stale revisions are cleared");
            overview.setState(cache, 7, 100, { 20, 40 }, 5);
            auto zero = std::make_shared<WaveformModel::PeakCache>(*cache);
            zero->mixedBins.clear();
            overview.setState(zero, 7, 100, { 20, 40 }, 5);
            const auto empty = overview.createComponentSnapshot(overview.getLocalBounds());
            check(preview.getPixelAt(unknownX, y) == empty.getPixelAt(unknownX, y),
                  "unknown preview tail contains no fabricated or stretched waveform");
            if (output.isNotEmpty())
            {
                juce::File directory(output);
                check(directory.createDirectory().wasOk(), "create overview render directory");
                auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light" : "night")
                    + "-" + juce::String(width) + ".png").createOutputStream();
                check(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                    && juce::PNGImageFormat().writeImageToStream(preview, *stream), "write overview evidence");
            }
        }
    overview.setSize(720, 26);
    overview.setState(cache, 7, 100, { 20, 40 }, 5);
    int pans = 0;
    double start = -1;
    overview.onPanRequested = [&](double value) { ++pans; start = value; };
    check(overview.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && start == 22,
          "keyboard pan advances ten percent of the visible range");
    overview.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
    check(start == 80, "End clamps the viewport to the source end");
    overview.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
    check(start == 0, "Home returns to the source start");
    const auto track = overview.getTrackBounds();
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto eventAt = [&](float x)
    {
        const juce::Point<float> point(x, track.getCentreY());
        return juce::MouseEvent(source, point, juce::ModifierKeys::leftButtonModifier, 1.0f,
            0, 0, 0, 0, &overview, &overview, juce::Time::getCurrentTime(), point,
            juce::Time::getCurrentTime(), 1, false);
    };
    overview.mouseDown(eventAt(track.getX() + track.getWidth() * 0.7f));
    check(std::abs(start - 60) < 0.001, "clicking outside viewport centres it at the chosen time");
    overview.mouseDrag(eventAt(track.getRight() + 100));
    check(start == 80, "captured drag past the right edge remains bounded");
    overview.mouseDrag(eventAt(track.getX() - 100));
    check(start == 0, "captured drag past the left edge remains bounded");
    overview.mouseUp(eventAt(track.getX()));
    const auto count = pans;
    overview.mouseDrag(eventAt(track.getRight()));
    check(pans == count, "mouse release ends the gesture");
    overview.setNavigationEnabled(false);
    check(!overview.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)), "command-disabled view cannot navigate");
    overview.mouseDown(eventAt(track.getRight() - 1));
    check(pans == count, "disabled overview cannot dispatch mouse navigation");
    overview.setLookAndFeel(nullptr);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "WaveformOverviewTests passed (12 renders, truthful prefix, stale cache, geometry, mouse and keyboard navigation)\n";
}
