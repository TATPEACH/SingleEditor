#include "UI/Components/TruePeakLimiterMagComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"
#include <iostream>
#include <cstdlib>

namespace
{
void verifyLimiterVisual(bool condition, const char* message)
{
    if (!condition) { std::cerr << "ModernLimiterPresentationTests: " << message << '\n'; std::exit(1); }
}
}

void runModernLimiterPresentationTests()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::modern);
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);
    ModernAppleLookAndFeel look;
    auto* previousDefaultLook = &juce::LookAndFeel::getDefaultLookAndFeel();
    juce::LookAndFeel::setDefaultLookAndFeel(&look);
    TruePeakLimiterParameters parameters;
    TruePeakLimiterMagComponent panel(parameters);
    panel.setLookAndFeel(&look);
    // General suppresses the optional DSP view. A later DSP visit creates a
    // fresh panel from the retained parameters, without a user toggle event.
    {
        TruePeakLimiterMagComponent firstVisit(parameters);
        firstVisit.setEnabledForTesting(true);
        const auto retained = firstVisit.getParameters();
        TruePeakLimiterMagComponent reopened(retained);
        reopened.setLookAndFeel(&look);
        for (const auto size : { juce::Point<int>(360, 320), juce::Point<int>(180, 180) })
        {
            reopened.setSize(size.x, size.y);
            reopened.applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::medium,
                                                   RendererInformationTier::standard, true);
            const auto* toggle = dynamic_cast<juce::Button*>(reopened.findChildWithID("limiter.enabled"));
            verifyLimiterVisual(toggle && toggle->getToggleState()
                && reopened.getStatusTextForTesting() == "Ready | Preview is audition-only",
                "a recreated enabled Limiter agrees with its On control at normal and compact sizes");
            verifyLimiterVisual(!reopened.isFeedbackVisibleForTesting(),
                "restoring enabled parameters never fabricates processing feedback");
        }
        reopened.setStatusText("Apply failed | Retry available");
        reopened.setSize(360, 320);
        verifyLimiterVisual(reopened.getStatusTextForTesting() == "Apply failed | Retry available",
            "layout changes preserve explicit operation outcomes");
        reopened.setLookAndFeel(nullptr);
        verifyLimiterVisual(panel.getStatusTextForTesting() == "Off | zero processing",
            "a new disabled Limiter retains its honest idle state");
    }
    const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile("out/modern-glass-limiter-renders");
    verifyLimiterVisual(folder.createDirectory().wasOk(), "create visual evidence directory");
    int count = 0;
    for (auto theme : { ModernTheme::light, ModernTheme::night })
        for (auto size : { juce::Point<int>(180, 500), juce::Point<int>(220, 360),
                           juce::Point<int>(360, 320), juce::Point<int>(520, 260) })
            for (const bool enabled : { false, true })
            {
                RendererVisualStyle::setModernTheme(theme);
                ModernAppleLookAndFeel::refreshTheme(panel);
                parameters.enabled = enabled;
                parameters.releaseMode = TruePeakLimiterReleaseMode::manual;
                panel.setParameters(parameters);
                panel.setStatusText(enabled ? "Ready | Preview is audition-only" : "Off | zero processing");
                panel.setSize(size.x, size.y);
                panel.applyWorkspaceVisualContract(RendererDensity::standard, RendererMagSize::medium,
                    RendererInformationTier::standard, true);
                verifyLimiterVisual(panel.getTransferCurveBounds().getWidth() >= 90
                    && panel.getTransferCurveBounds().getHeight() >= 68, "normal tiles expose a readable static target");
                auto* mode = dynamic_cast<juce::ComboBox*>(panel.findChildWithID("limiter.release-mode"));
                auto* release = dynamic_cast<juce::Slider*>(panel.findChildWithID("limiter.release-ms"));
                verifyLimiterVisual(mode && release && mode->isVisible() && release->isVisible()
                    && release->isEnabled() == enabled, "manual release remains an actual enabled/disabled parameter");
                verifyLimiterVisual(!panel.isFeedbackVisibleForTesting(), "static curve never fabricates live TP or GR");
                auto* threshold = dynamic_cast<juce::Slider*>(panel.findChildWithID("limiter.threshold-db"));
                verifyLimiterVisual(threshold && look.getSliderLayout(*threshold).sliderBounds.getWidth() >= 32,
                    "narrow parameter retains real slider travel beside its editable value");
                const auto* thresholdName = dynamic_cast<juce::Label*>(panel.findChildWithID("limiter.threshold-label"));
                verifyLimiterVisual(thresholdName && thresholdName->isEnabled()
                    && thresholdName->getText() == (size.x >= 220 ? "Threshold" : "Thresh."),
                    "off-state descriptions remain readable and standard columns retain the full parameter name");
                const auto nameWidth = juce::GlyphArrangement::getStringWidth(thresholdName->getFont(), thresholdName->getText());
                const auto nameSpace = thresholdName->getWidth() - thresholdName->getBorderSize().getLeftAndRight();
                if (nameWidth > nameSpace)
                    std::cerr << "Limiter " << size.x << "x" << size.y << " name=" << thresholdName->getText()
                              << " width=" << nameWidth << " available=" << nameSpace << '\n';
                verifyLimiterVisual(nameWidth <= nameSpace,
                    "the parameter name fits using the production typeface without horizontal compression");
                juce::Image frame(juce::Image::ARGB, size.x * 2, size.y * 2, true);
                { juce::Graphics g(frame); g.addTransform(juce::AffineTransform::scale(2.0f)); panel.paintEntireComponent(g, true); }
                const auto name = juce::String(theme == ModernTheme::night ? "night-" : "light-")
                    + juce::String(size.x) + "x" + juce::String(size.y) + (enabled ? "-on.png" : "-off.png");
                auto stream = folder.getChildFile(name).createOutputStream();
                verifyLimiterVisual(stream && stream->setPosition(0) && stream->truncate().wasOk(), "replace current visual evidence");
                juce::PNGImageFormat png;
                verifyLimiterVisual(png.writeImageToStream(frame, *stream), "write rendered panel");
                ++count;
            }
    parameters.releaseMode = TruePeakLimiterReleaseMode::automatic;
    panel.setParameters(parameters);
    const auto* release = panel.findChildWithID("limiter.release-ms");
    verifyLimiterVisual(release && !release->isEnabled(), "automatic release does not expose editable manual time");
    panel.setApplying(true);
    for (const auto* id : { "limiter.enabled", "limiter.threshold-db", "limiter.ceiling-dbtp", "limiter.preview", "limiter.apply" })
        verifyLimiterVisual(!panel.findChildWithID(id)->isEnabled(), "applying locks the same live controls");
    for (const auto* id : { "limiter.threshold-label", "limiter.ceiling-label", "limiter.release-label" })
        verifyLimiterVisual(panel.findChildWithID(id)->isEnabled(), "applying preserves readable parameter descriptions");
    panel.setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(previousDefaultLook);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "ModernLimiterPresentationTests passed (" << count << " renders, static target, manual/auto and applying states)\n";
}
