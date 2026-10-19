#include "UI/Components/MeterBridgeComponent.h"

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Skins/SkinStyle.h"

namespace singleeditor
{
namespace
{
float clamp01(float value) noexcept
{
    return juce::jlimit(0.0f, 1.0f, value);
}
} // namespace

class MeterBridgeComponent::VisibilityWatcher final : public juce::ComponentMovementWatcher
{
public:
    explicit VisibilityWatcher(MeterBridgeComponent& value) : ComponentMovementWatcher(&value), owner(value) {}
    void componentMovedOrResized(bool, bool) override {}
    void componentPeerChanged() override { owner.refreshActivity(); }
    void componentVisibilityChanged() override { owner.refreshActivity(); }
private:
    MeterBridgeComponent& owner;
};

MeterBridgeComponent::MeterBridgeComponent(AudioEngine& engineToUse)
    : engine(engineToUse)
{
    setInterceptsMouseClicks(false, false);
    watcher = std::make_unique<VisibilityWatcher>(*this);
}

MeterBridgeComponent::~MeterBridgeComponent()
{ watcher.reset(); stopTimer(); lease = {}; }
void MeterBridgeComponent::setCoreReady(bool ready)
{ coreReady = ready; refreshActivity(); }
void MeterBridgeComponent::refreshActivity()
{
    if (coreReady && isShowing())
    {
        if (!lease) lease = engine.acquireOutputMeterAnalysisLease();
        if (lease) startTimerHz(30);
    }
    else
    {
        stopTimer(); lease = {}; resetPeakHold();
    }
}

void MeterBridgeComponent::setMeterMode(AudioEngine::MeterMode newMode) noexcept
{
    if (mode == newMode)
        return;

    mode = newMode;
    resetPeakHold();
    repaint();
}

void MeterBridgeComponent::resetPeakHold() noexcept
{
    displayedLevel = 0.0f;
    heldLevel = 0.0f;
    repaint();
}

void MeterBridgeComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    if (!SkinStyle::isOriginal() && !ModernAppleLookAndFeel::isStartupSkin())
    {
        SkinStyle::paintMeterBridge(g, bounds, clamp01(displayedLevel), clamp01(heldLevel));
        return;
    }
    g.setColour(ModernAppleLookAndFeel::mutedSurface().brighter(0.02f));
    g.fillRoundedRectangle(bounds, 12.0f);

    auto meterBounds = bounds.reduced(8.0f, 7.0f);
    g.setColour(RendererVisualStyle::isClassic() ? juce::Colour(0xffd8dde6)
        : ModernAppleLookAndFeel::primaryText().withAlpha(RendererVisualStyle::isNight() ? 0.07f : 0.08f));
    g.fillRoundedRectangle(meterBounds, 7.0f);

    const auto levelWidth = meterBounds.getWidth() * clamp01(displayedLevel);
    if (levelWidth > 0.5f)
    {
        juce::ColourGradient gradient(ModernAppleLookAndFeel::accentBlue(),
                                      meterBounds.getX(),
                                      meterBounds.getCentreY(),
                                      ModernAppleLookAndFeel::accentOrange(),
                                      meterBounds.getRight(),
                                      meterBounds.getCentreY(),
                                      false);
        gradient.addColour(0.55, juce::Colour(0xffb38cff));
        g.setGradientFill(gradient);
        g.fillRoundedRectangle(meterBounds.withWidth(levelWidth), 7.0f);
    }

    g.setColour(ModernAppleLookAndFeel::primaryText().withAlpha(0.18f));
    for (const auto tick : { 0.25f, 0.5f, 0.75f })
    {
        const auto x = meterBounds.getX() + (meterBounds.getWidth() * tick);
        g.drawVerticalLine(juce::roundToInt(x), meterBounds.getY() + 1.5f, meterBounds.getBottom() - 1.5f);
    }

    if (heldLevel > 0.001f)
    {
        const auto holdX = meterBounds.getX() + (meterBounds.getWidth() * clamp01(heldLevel));
        g.setColour(ModernAppleLookAndFeel::primaryText().withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(holdX), meterBounds.getY(), meterBounds.getBottom());
    }

}

float MeterBridgeComponent::normaliseMeterValue(AudioEngine::MeterMode meterMode, const AudioEngine& engine) noexcept
{
    switch (meterMode)
    {
        case AudioEngine::MeterMode::truePeak:
        {
            const auto dB = juce::Decibels::gainToDecibels(engine.getTruePeakLinear(), -120.0f);
            return clamp01((dB + 60.0f) / 66.0f);
        }

        case AudioEngine::MeterMode::rms:
        {
            const auto dB = juce::Decibels::gainToDecibels(engine.getOutputRmsLinear(), -120.0f);
            return clamp01((dB + 60.0f) / 60.0f);
        }

        case AudioEngine::MeterMode::lufs:
        {
            const auto lufs = engine.getMomentaryLufs();
            return clamp01((lufs + 54.0f) / 54.0f);
        }
    }

    return 0.0f;
}

void MeterBridgeComponent::timerCallback()
{
    if (!coreReady || !isShowing()) { refreshActivity(); return; }
    const auto previousLevel = displayedLevel;
    const auto previousHold = heldLevel;
    const auto targetLevel = normaliseMeterValue(mode, engine);
    const auto attack = 0.62f;
    const auto release = 0.18f;
    displayedLevel += (targetLevel - displayedLevel) * (targetLevel > displayedLevel ? attack : release);
    displayedLevel = clamp01(displayedLevel);
    if (displayedLevel < 0.0001f && targetLevel == 0.0f) displayedLevel = 0.0f;

    if (displayedLevel >= heldLevel)
        heldLevel = displayedLevel;
    else
        heldLevel = juce::jmax(displayedLevel, heldLevel - 0.018f);

    if (std::abs(previousLevel - displayedLevel) > 0.00001f
        || std::abs(previousHold - heldLevel) > 0.00001f)
        repaint();
}
} // namespace singleeditor
