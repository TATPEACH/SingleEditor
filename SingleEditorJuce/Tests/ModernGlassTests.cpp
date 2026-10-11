#include "UI/Components/ModernGlassSurface.h"
#include "UI/Components/ModernIconButton.h"
#include "UI/Components/SelectionPopupLayout.h"
#include "UI/LookAndFeel/ModernTypography.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using namespace singleeditor;
void check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "ModernGlassTests failed: " << message << '\n';
    std::exit(1);
}

bool different(const juce::Image& a, const juce::Image& b)
{
    if (a.getBounds() != b.getBounds()) return true;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return true;
    return false;
}

// Deliberately a material calibration pattern, not a generated audio waveform.
class MaterialFixture final : public juce::Component
{
public:
    int offset = 0;
    void paint(juce::Graphics& g) override
    {
        g.fillAll(RendererVisualStyle::panelBackground());
        for (int x = -20; x < getWidth(); x += 20)
        {
            g.setColour(RendererVisualStyle::accentBlue());
            g.fillRect(x + offset, 0, 6, getHeight());
        }
        g.setColour(RendererVisualStyle::accentPurple());
        g.fillRect(0, 95, getWidth(), 12);
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText("Native material fixture", 20, 12, 300, 24, juce::Justification::centredLeft);
    }
};

juce::Image referenceGaussian(const juce::Image& source, float radius, juce::Colour background)
{
    juce::Image result(juce::Image::RGB, source.getWidth(), source.getHeight(), false, juce::SoftwareImageType());
    { juce::Graphics g(result); g.fillAll(background); g.drawImageAt(source, 0, 0); }
    result.getPixelData()->applyGaussianBlurEffect(std::clamp(radius, 0.5f, 8.0f));
    return result;
}

void checkGaussianEquivalence()
{
    int maximumError = 0, cases = 0;
    for (auto format : { juce::Image::RGB, juce::Image::ARGB })
        for (auto size : { juce::Point<int>(1, 1), {2, 3}, {13, 7}, {64, 31}, {129, 63}, {308, 219} })
            for (const float radius : { 0.5f, 1.25f, 4.0f, 6.0f, 8.0f })
            {
                juce::Image input(format, size.x, size.y, true, juce::SoftwareImageType());
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                        input.setPixelAt(x, y, juce::Colour(static_cast<juce::uint8>((x * 83 + y * 31) % 256),
                            static_cast<juce::uint8>((x * 3 + y * 67) % 256),
                            static_cast<juce::uint8>((x * 29 + y * 11) % 256),
                            static_cast<juce::uint8>(format == juce::Image::RGB ? 255 : (x * 47 + y * 13) % 256)));
                const auto original = input.createCopy();
                const auto background = juce::Colour(0xff526374);
                const auto expected = referenceGaussian(input, radius, background);
                const auto actual = ModernGlassBackdrop::blur(input, radius, background);
                check(actual.isValid() && actual.getBounds() == expected.getBounds(), "separable Gaussian keeps exact image geometry");
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                    {
                        const auto a = actual.getPixelAt(x, y), b = expected.getPixelAt(x, y);
                        const int error = std::max({ std::abs(a.getRed() - b.getRed()),
                            std::abs(a.getGreen() - b.getGreen()), std::abs(a.getBlue() - b.getBlue()) });
                        maximumError = std::max(maximumError, error);
                        check(error <= 1, "same Gaussian kernel and boundaries differ by at most one byte level");
                    }
                check(!different(input, original), "Gaussian leaves shared source and its alpha unchanged");
                ++cases;
            }
    check(!ModernGlassBackdrop::blur({}, 6, juce::Colours::black).isValid(), "invalid source does not allocate glass");
    juce::Image overLimit(juce::Image::RGB, 1025, 1, true);
    check(!ModernGlassBackdrop::blur(overLimit, 6, juce::Colours::black).isValid(), "existing capture-size bound is retained");
    const juce::Image validTiny(juce::Image::RGB, 1, 1, true);
    check(!ModernGlassBackdrop::blur(validTiny, std::numeric_limits<float>::quiet_NaN(), juce::Colours::black).isValid(),
        "invalid radius never reaches pixel arithmetic");
    std::cout << "Gaussian reference equivalence: " << cases << " RGB/ARGB cases, max_channel_error=" << maximumError << '\n';
}

void checkOptics()
{
    juce::Image source(juce::Image::RGB, 128, 64, true);
    {
        juce::Graphics g(source);
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::white);
        g.fillRect(64, 0, 64, 64);
    }
    const auto blurred = ModernGlassBackdrop::blur(source, 4, juce::Colours::black);
    check(blurred.getPixelAt(62, 32).getRed() > 0 && blurred.getPixelAt(65, 32).getRed() < 255,
          "Gaussian blur softens both sides of an actual sharp edge");
    check(source.getPixelAt(62, 32) == juce::Colours::black,
          "backdrop processing never changes the source image");
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x)
            source.setPixelAt(x, y, juce::Colour(static_cast<juce::uint8>(x * 2), static_cast<juce::uint8>(y * 4), 90));
    const auto body = source.getBounds().reduced(12);
    const auto flat = ModernGlassBackdrop::sample(source, body, 8, ModernGlassMaterial::frosted);
    const auto liquid = ModernGlassBackdrop::sample(source, body, 8, ModernGlassMaterial::liquid);
    check(different(flat, liquid), "liquid glass spatially refracts the underlying image");
    check(flat.getPixelAt(50, 20) == liquid.getPixelAt(50, 20), "the interior stays undistorted");
    check(flat.getPixelAt(50, 3) != liquid.getPixelAt(50, 3), "the curved boundary bends the backdrop");
    check(!ModernGlassBackdrop::sample(source, body, 8, ModernGlassMaterial::solid).isValid(),
          "reduce transparency creates no optical image");
}

// Keep the original full-image sampling path as an independent reference for
// the copied interior / edge-only optimisation. Do not share its span selection.
juce::Image referenceSample(const juce::Image& blurred, juce::Rectangle<int> body,
                            float radius, ModernGlassMaterial material, float strength)
{
    juce::Image result(juce::Image::RGB, body.getWidth(), body.getHeight(), false, juce::SoftwareImageType());
    const juce::Image::BitmapData source(blurred, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData target(result, juce::Image::BitmapData::writeOnly);
    const auto local = result.getBounds().toFloat();
    for (int y = 0; y < target.height; ++y)
        for (int x = 0; x < target.width; ++x)
        {
            auto p = juce::Point<float>(static_cast<float>(x), static_cast<float>(y));
            if (material == ModernGlassMaterial::liquid)
                p = ModernGlassBackdrop::refractedPoint(p, local, radius, strength);
            p += body.getPosition().toFloat();
            p.x = juce::jlimit(0.0f, static_cast<float>(source.width - 1), p.x);
            p.y = juce::jlimit(0.0f, static_cast<float>(source.height - 1), p.y);
            const auto x0 = static_cast<int>(p.x), y0 = static_cast<int>(p.y);
            const auto x1 = juce::jmin(x0 + 1, source.width - 1), y1 = juce::jmin(y0 + 1, source.height - 1);
            const auto fx = p.x - static_cast<float>(x0), fy = p.y - static_cast<float>(y0);
            const auto top = source.getPixelColour(x0, y0).interpolatedWith(source.getPixelColour(x1, y0), fx);
            const auto bottom = source.getPixelColour(x0, y1).interpolatedWith(source.getPixelColour(x1, y1), fx);
            target.setPixelColour(x, y, top.interpolatedWith(bottom, fy));
        }
    return result;
}

void checkSamplingEquivalence()
{
    int cases = 0;
    for (auto format : { juce::Image::RGB, juce::Image::ARGB })
        for (auto size : { juce::Point<int>(1, 1), {2, 3}, {5, 4}, {13, 7}, {32, 31}, {129, 63}, {280, 195} })
        {
            juce::Image input(format, size.x + 13, size.y + 17, true, juce::SoftwareImageType());
            for (int y = 0; y < input.getHeight(); ++y)
                for (int x = 0; x < input.getWidth(); ++x)
                    input.setPixelAt(x, y, juce::Colour(static_cast<juce::uint8>((x * 83 + y * 31) % 256),
                        static_cast<juce::uint8>((x * 3 + y * 67) % 256),
                        static_cast<juce::uint8>((x * 29 + y * 11) % 256),
                        static_cast<juce::uint8>(format == juce::Image::RGB ? 255 : (x * 47 + y * 13) % 256)));
            const auto original = input.createCopy();
            const juce::Rectangle<int> body(5, 7, size.x, size.y);
            for (float radius : {0.25f, 1.0f, 2.0f, 7.25f, 8.0f, 500.0f})
                for (float strength : {-1.0f, 0.0f, 0.35f, 1.0f, 1.2f, 2.0f})
                    for (auto material : {ModernGlassMaterial::frosted, ModernGlassMaterial::liquid})
                    {
                        const auto expected = referenceSample(input, body, radius, material, strength);
                        auto actual = ModernGlassBackdrop::sample(input, body, radius, material, strength);
                        check(actual.getFormat() == juce::Image::RGB && !different(actual, expected),
                              "copied glass interior and refracted edges match every original pixel exactly");
                        actual.setPixelAt(0, 0, juce::Colours::magenta);
                        check(!different(input, original), "sample output owns its pixels and cannot modify the blurred source");
                        ++cases;
                    }
        }
    std::cout << "Glass sampling equivalence: " << cases << " RGB/ARGB cases, exact_pixels=1\n";
}

void measureSamplingCost()
{
    for (auto size : {juce::Point<int>(211, 24), {280, 195}, {720, 450}})
    {
        juce::Image input(juce::Image::RGB, size.x + 24, size.y + 24, false, juce::SoftwareImageType());
        { juce::Graphics g(input); g.fillAll(juce::Colours::cornflowerblue); }
        const juce::Rectangle<int> body(12, 12, size.x, size.y);
        for (auto material : {ModernGlassMaterial::frosted, ModernGlassMaterial::liquid})
        {
            std::array<double, 21> samples;
            unsigned int checksum = 0;
            for (int i = -2; i < static_cast<int>(samples.size()); ++i)
            {
                const auto start = juce::Time::getHighResolutionTicks();
                const auto image = ModernGlassBackdrop::sample(input, body, 8.0f, material, 1.2f);
                const auto elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start) * 1000.0;
                checksum += image.getPixelAt(0, 0).getARGB();
                if (i >= 0) samples[static_cast<size_t>(i)] = elapsed;
            }
            std::sort(samples.begin(), samples.end());
            std::cout << "Glass sampler " << size.x << 'x' << size.y
                      << (material == ModernGlassMaterial::frosted ? " frosted" : " liquid")
                      << " samples=21 median_ms=" << samples[10] << " p90_ms=" << samples[18]
                      << " max_ms=" << samples.back() << " checksum=" << checksum << '\n';
        }
    }
}
}

void runModernGlassTests()
{
    using namespace singleeditor;
    checkGaussianEquivalence();
    checkOptics();
    checkSamplingEquivalence();
    measureSamplingCost();
    for (int width = 140; width <= 422; ++width)
    {
        const auto layout = SelectionPopupLayout::calculate(width);
        const juce::Rectangle<int> bounds(0, 0, width, layout.height);
        const std::array<juce::Rectangle<int>, 6> controls { layout.label, layout.down, layout.up,
                                                         layout.slider, layout.apply, layout.cancel };
        for (size_t i = 0; i < controls.size(); ++i)
        {
            check(!controls[i].isEmpty() && bounds.contains(controls[i]), "every popup control is visible at every supported width");
            for (size_t j = i + 1; j < controls.size(); ++j)
                check(!controls[i].intersects(controls[j]), "popup controls never overlap");
        }
    }

    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel lookAndFeel;
    juce::Component root;
    root.setLookAndFeel(&lookAndFeel);
    root.setBounds(0, 0, 540, 220);
    root.setVisible(true);
    auto source = std::make_unique<MaterialFixture>();
    root.addAndMakeVisible(*source);
    source->setBounds(root.getLocalBounds());
    ModernGlassSurface glass;
    root.addAndMakeVisible(glass);
    glass.setBounds(59, 80, 422, 48);
    glass.setBackdropSource(source.get());
    glass.refreshBackdropNow();
    check(!glass.hasBackdrop() && glass.captureCountForTesting() == 0,
          "the default pre-Core state never captures or allocates a backdrop");

    juce::Label label({}, "-24.0 dB");
    label.setFont(ModernTypography::font(11.0f));
    label.setBorderSize(juce::BorderSize<int>(0, 2, 0, 2));
    label.setMinimumHorizontalScale(0.9f);
    for (const auto text : { "-60.0 dB", "+18.0 dB" })
        check(juce::GlyphArrangement::getStringWidth(label.getFont(), text) <= 54.0f,
              "selection gain values retain their complete text at the actual em size");
    std::cout << "Selection Rendering text width=" << juce::GlyphArrangement::getStringWidth(label.getFont(), "Rendering")
              << " available=54 minScale=" << label.getMinimumHorizontalScale() << '\n';
    check(juce::GlyphArrangement::getStringWidth(label.getFont(), "Rendering") * label.getMinimumHorizontalScale() <= 54.0f,
          "the transient Rendering state fits with at most ten percent horizontal compression");
    ModernIconButton down, up;
    down.setButtonText("-3"); up.setButtonText("+3");
    juce::TextButton apply("Apply"), cancel("Cancel");
    juce::Slider slider;
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setRange(-24, 24, 0.1);
    slider.setValue(0);
    apply.getProperties().set("accent", true);
    for (auto* c : std::array<juce::Component*, 6> { &label, &down, &up, &slider, &apply, &cancel })
        glass.addAndMakeVisible(c);
    glass.setEffectsEnabled(true);
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_GLASS_OFFSCREEN_DIR", {});
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const auto material : { ModernGlassMaterial::solid, ModernGlassMaterial::frosted, ModernGlassMaterial::liquid })
            for (const int width : { 140, 280, 422 })
            {
                RendererVisualStyle::setModernTheme(theme);
                RendererVisualStyle::setGlassMaterial(material);
                ModernAppleLookAndFeel::refreshTheme(root);
                const auto layout = SelectionPopupLayout::calculate(width);
                glass.setBounds((540 - width) / 2, 65, width, layout.height);
                label.setBounds(layout.label); down.setBounds(layout.down); up.setBounds(layout.up);
                slider.setBounds(layout.slider); apply.setBounds(layout.apply); cancel.setBounds(layout.cancel);
                glass.refreshBackdropNow();
                check(glass.hasBackdrop() == (material != ModernGlassMaterial::solid), "material changes respect reduce transparency");
                const auto captures = glass.captureCountForTesting();
                const auto image = root.createComponentSnapshot(root.getLocalBounds(), true, 1.5f);
                check(captures == glass.captureCountForTesting(), "paint uses prepared images and never recaptures the source");
                if (theme == ModernTheme::light && width == 422 && material == ModernGlassMaterial::frosted)
                {
                    int minimum = 255, maximum = 0;
                    for (int x = 90; x < 150; ++x)
                    {
                        const auto red = image.getPixelAt(juce::roundToInt((glass.getX() + x) * 1.5f),
                                                          juce::roundToInt((glass.getY() + 5) * 1.5f)).getRed();
                        minimum = juce::jmin(minimum, static_cast<int>(red));
                        maximum = juce::jmax(maximum, static_cast<int>(red));
                    }
                    std::cout << "Frosted fine-stripe contrast=" << maximum - minimum << '\n';
                    check(maximum - minimum < 20, "the actual surface smooths high-frequency stripes instead of just tinting them");
                }
                if (output.isNotEmpty())
                {
                    const juce::File directory(output);
                    check(directory.createDirectory().wasOk(), "create image output directory");
                    const auto name = juce::String(theme == ModernTheme::light ? "light" : "night") + "-"
                        + (material == ModernGlassMaterial::solid ? "solid" : material == ModernGlassMaterial::frosted ? "frosted" : "liquid")
                        + "-" + juce::String(width) + ".png";
                    auto stream = directory.getChildFile(name).createOutputStream();
                    check(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk(), "replace previous PNG evidence");
                    check(stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream), "write material evidence image");
                }
            }
    const auto before = glass.createComponentSnapshot(glass.getLocalBounds());
    source->offset = 9;
    glass.invalidateBackdrop();
    glass.refreshBackdropNow();
    const auto after = glass.createComponentSnapshot(glass.getLocalBounds());
    check(different(before, after), "changing the real source changes the cached glass pixels");

    // Exercise timer coalescing without adding a desktop peer or taking focus.
    juce::Thread::sleep(90);
    const auto count = glass.captureCountForTesting();
    for (int i = 0; i < 100; ++i) glass.invalidateBackdrop();
    // callPendingTimersSynchronously dispatches the countdown maintained by
    // JUCE's separate timer thread; sleeping 25 ms cannot guarantee that the
    // 20 ms timer has been marked due on a busy Windows scheduler. Bound the
    // functional wait, then still require exactly one capture and no live timer.
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 250.0;
    while (glass.captureCountForTesting() == count
           && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    check(glass.captureCountForTesting() == count + 1 && !glass.hasScheduledUpdateForTesting(),
          "a burst of changes produces one deferred capture and stops its timer");
    juce::Thread::sleep(100);
    juce::Timer::callPendingTimersSynchronously();
    check(glass.captureCountForTesting() == count + 1, "idle glass performs no recurring captures");

    // Dispatch only to this peerless test component: no native mouse input,
    // desktop window, pointer movement or focus transfer is involved.
    const auto pointerEvent = [](juce::Component& target)
    {
        const juce::Point<float> position { 12.0f, 8.0f };
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, {},
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, juce::Time::getCurrentTime(),
            position, juce::Time::getCurrentTime(), 0, false);
    };
    const auto flushGlassTimer = []
    {
        juce::Thread::sleep(30);
        juce::Timer::callPendingTimersSynchronously();
    };
    const auto restartGlass = [&]
    {
        glass.setVisible(false);
        glass.setVisible(true);
        glass.refreshBackdropNow();
        flushGlassTimer();
    };
    const auto savedMotionEnabled = RendererVisualStyle::isMotionEnabled();
    const auto savedReduceMotion = RendererVisualStyle::isReduceMotion();
    RendererVisualStyle::setGlassMaterial(ModernGlassMaterial::liquid);
    ModernNorthStarMotionPolicy::setEnabled(false);
    RendererVisualStyle::setReduceMotion(false);
    glass.mouseEnter(pointerEvent(glass));
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "disabled motion applies liquid hover directly without scheduling animation");
    const auto hoveredImage = glass.createComponentSnapshot(glass.getLocalBounds());
    restartGlass();
    check(glass.hoverAmountForTesting() == 0.0f && !glass.hasScheduledUpdateForTesting(),
          "reopening glass clears stale hover and leaves no idle timer");
    check(different(hoveredImage, glass.createComponentSnapshot(glass.getLocalBounds())),
          "clearing hover changes the actual liquid surface pixels");

    ModernNorthStarMotionPolicy::setEnabled(true);
    RendererVisualStyle::setReduceMotion(true);
    glass.mouseEnter(pointerEvent(label));
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "reduced motion also applies descendant hover directly without animation");
    restartGlass();
    RendererVisualStyle::setReduceMotion(false);
    const auto beforeHoverCaptures = glass.captureCountForTesting();
    glass.mouseEnter(pointerEvent(glass));
    check(glass.hoverAmountForTesting() == 0.0f && glass.hasScheduledUpdateForTesting(),
          "standard motion starts a bounded liquid hover transition");
    flushGlassTimer();
    check(glass.hoverAmountForTesting() > 0.0f, "the actual liquid timer advances the transition");
    RendererVisualStyle::setReduceMotion(true);
    flushGlassTimer();
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "switching Reduce Motion mid-transition settles without another pointer event");
    check(glass.captureCountForTesting() == beforeHoverCaptures,
          "hover motion renders the prepared cache without recapturing its source");

    restartGlass();
    RendererVisualStyle::setReduceMotion(false);
    glass.mouseEnter(pointerEvent(glass));
    ModernNorthStarMotionPolicy::setEnabled(false);
    flushGlassTimer();
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "disabling global motion also settles an in-flight transition");
    restartGlass();
    ModernNorthStarMotionPolicy::setEnabled(true);
    glass.mouseEnter(pointerEvent(glass));
    juce::Thread::sleep(180);
    juce::Timer::callPendingTimersSynchronously();
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "standard liquid motion reaches its endpoint and releases its timer");
    glass.setEffectsEnabled(false);
    glass.mouseEnter(pointerEvent(glass));
    check(glass.hoverAmountForTesting() == 0.0f && !glass.hasScheduledUpdateForTesting(),
          "pre-Core glass neither retains hover nor starts motion on pointer input");
    glass.setEffectsEnabled(true);
    glass.refreshBackdropNow();
    ModernNorthStarMotionPolicy::setEnabled(savedMotionEnabled);
    RendererVisualStyle::setReduceMotion(savedReduceMotion);

    for (const auto material : { ModernGlassMaterial::frosted, ModernGlassMaterial::liquid })
    {
        RendererVisualStyle::setGlassMaterial(material);
        std::vector<double> elapsed;
        for (int i = 0; i < 11; ++i)
        {
            const auto start = juce::Time::getMillisecondCounterHiRes();
            glass.refreshBackdropNow();
            if (i > 0) elapsed.push_back(juce::Time::getMillisecondCounterHiRes() - start);
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::cout << "Glass 422x48 " << (material == ModernGlassMaterial::frosted ? "frosted" : "liquid")
                  << " capture+filter median_ms=" << elapsed[elapsed.size() / 2] << " max_ms=" << elapsed.back() << '\n';
    }
    // Native floating reparents the borrowed waveform before its overlay. The
    // intermediate topology must not capture another top level or stale pixels.
    juce::Component floatingHost;
    floatingHost.setBounds(800, 200, 360, 240);
    floatingHost.setVisible(true);
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        const auto beforeTransfer = glass.captureCountForTesting();
        floatingHost.addAndMakeVisible(*source);
        source->setBounds(floatingHost.getLocalBounds());
        check(!glass.hasBackdrop() && !glass.hasScheduledUpdateForTesting(),
              "moving the source to another window immediately drops the old backdrop and timer");
        glass.refreshBackdropNow();
        check(glass.captureCountForTesting() == beforeTransfer,
              "a different top level is never sampled during float/redock transfer");
        floatingHost.addAndMakeVisible(glass);
        glass.setBounds(24, 70, 300, 80);
        glass.refreshBackdropNow();
        check(glass.hasBackdrop(), "glass resumes after joining its waveform in the floating host");
        source->setVisible(false);
        check(!glass.hasBackdrop() && !glass.hasScheduledUpdateForTesting(),
              "hiding a source Tab immediately clears its glass cache and timer");
        source->setVisible(true);
        glass.refreshBackdropNow();
        check(glass.hasBackdrop(), "showing the source Tab restores valid optics");

        // A future attachment mistake must still fail closed rather than taking
        // a snapshot containing the glass that is currently being rendered.
        source->addAndMakeVisible(glass);
        glass.refreshBackdropNow();
        check(!glass.hasBackdrop(), "reparenting below the sampled source cannot recurse through paint");
        root.addAndMakeVisible(*source);
        source->setBounds(root.getLocalBounds());
        root.addAndMakeVisible(glass);
        glass.setBounds(59, 80, 422, 48);
        glass.refreshBackdropNow();
        check(glass.hasBackdrop(), "redocking the same source and surface restores the backdrop");
    }
    glass.setVisible(false);
    check(!glass.hasBackdrop(), "hiding the surface immediately releases optical caches");
    const auto hiddenCount = glass.captureCountForTesting();
    glass.refreshBackdropNow();
    check(hiddenCount == glass.captureCountForTesting(), "hidden glass never captures");
    glass.setVisible(true);
    glass.refreshBackdropNow();
    glass.setEffectsEnabled(false);
    check(!glass.hasBackdrop(), "returning to Core startup releases caches");
    glass.setEffectsEnabled(true);
    glass.refreshBackdropNow();
    source.reset();
    check(!glass.hasBackdrop(), "source destruction releases caches without dangling access");
    glass.refreshBackdropNow();
    RendererSelection::resetForTests(RendererKind::classic);
    check(RendererVisualStyle::glassMaterial() == ModernGlassMaterial::solid, "Classic never enables Modern optics");
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    RendererVisualStyle::setGlassMaterial(ModernGlassMaterial::frosted);
    root.setLookAndFeel(nullptr);
}
