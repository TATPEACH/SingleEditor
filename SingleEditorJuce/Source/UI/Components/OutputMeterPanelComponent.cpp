#include "UI/Components/OutputMeterPanelComponent.h"

#include <algorithm>

#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernOutputMeterPainter.h"

namespace singleeditor
{
namespace
{
class OutputMeterAccessibilityValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit OutputMeterAccessibilityValue(const OutputMeterPanelComponent& ownerToUse)
        : owner(ownerToUse)
    {
    }

    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override
    {
        return owner.getAccessibilityValueSummary();
    }
    void setValueAsString(const juce::String&) override {}

private:
    const OutputMeterPanelComponent& owner;
};
}

class OutputMeterPanelComponent::VisibilityWatcher final : public juce::ComponentMovementWatcher
{
public:
    explicit VisibilityWatcher(OutputMeterPanelComponent& value) : ComponentMovementWatcher(&value), owner(value) {}
    void componentMovedOrResized(bool, bool) override {}
    void componentPeerChanged() override { owner.refreshActivity(); }
    void componentVisibilityChanged() override { owner.refreshActivity(); }
private:
    OutputMeterPanelComponent& owner;
};

OutputMeterPanelComponent::OutputMeterPanelComponent(AudioEngine& engineToUse)
    : engine(engineToUse)
{
    setInterceptsMouseClicks(false, true);
    setDescription("Current device output levels. TP and RMS readouts show the highest channel; "
                   "LUFS shows global momentary loudness. The toolbar RMS averages all device outputs, "
                   "including silent channels.");
    if (!RendererVisualStyle::isClassic())
        getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
    addChildComponent(resetHoldButton);
    resetHoldButton.setComponentID("output-meter.reset-hold");
    resetHoldButton.setTitle("Reset output peak hold");
    resetHoldButton.setTooltip("Reset displayed peak hold; does not change audio or measurement history.");
    resetHoldButton.onClick = [this] { display.resetHold(); repaint(); };
    watcher = std::make_unique<VisibilityWatcher>(*this);
}

OutputMeterPanelComponent::~OutputMeterPanelComponent()
{ watcher.reset(); stopTimer(); lease = {}; }
void OutputMeterPanelComponent::setCoreReady(bool ready)
{ coreReady = ready; refreshActivity(); }

void OutputMeterPanelComponent::setMeterMode(AudioEngine::MeterMode newMode) noexcept
{
    if (mode == newMode)
        return;

    mode = newMode;
    display.resetBallistics();
    repaint();
}

void OutputMeterPanelComponent::resized()
{
    const auto geometry = ModernOutputMeterPainter::layout(getLocalBounds(), display.snapshot.channelCount);
    resetHoldButton.setBounds(geometry.reset);
    resetHoldButton.setVisible(!RendererVisualStyle::isClassic() && !geometry.reset.isEmpty());
}

void OutputMeterPanelComponent::paint(juce::Graphics& g)
{
    if (!RendererVisualStyle::isClassic())
    {
        ModernOutputMeterPainter::paint(g, getLocalBounds(), display, mode);
        return;
    }
    const auto& latestSnapshot = display.snapshot;
    const auto& displayedLevels = display.levels;
    const auto& heldLevels = display.trails;
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(RendererVisualStyle::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), RendererVisualStyle::cardRadius());

    const auto tier = static_cast<RendererInformationTier>(
        std::clamp(static_cast<int>(getProperties().getWithDefault(
                       "northStarInformationTier",
                       static_cast<int>(RendererInformationTier::standard))),
                   static_cast<int>(RendererInformationTier::summary),
                   static_cast<int>(RendererInformationTier::detail)));
    const auto summary = tier == RendererInformationTier::summary;
    const auto detail = tier == RendererInformationTier::detail;

    auto content = getLocalBounds().reduced(summary ? 10 : 14, summary ? 10 : 16);
    if (content.isEmpty())
        return;
    g.setColour(RendererVisualStyle::primaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::titleTextSize(),
                                          juce::Font::bold)));
    g.drawText(summary ? "Output Meter" : "Output",
               content.removeFromTop(std::min(20, content.getHeight())),
               juce::Justification::centredLeft);

    g.setColour(RendererVisualStyle::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::captionTextSize(),
                                          juce::Font::bold)));
    const auto topology = latestSnapshot.channelCount > 0
                        ? juce::String(latestSnapshot.channelCount) + " DEVICE OUTPUTS"
                        : juce::String("DEVICE OUTPUTS SLEEPING");
    if (content.getHeight() > 0)
        g.drawText(topology,
                   content.removeFromTop(std::min(16, content.getHeight())),
                   juce::Justification::centredLeft);
    content.removeFromTop(std::min(summary ? 3 : 8, content.getHeight()));

    auto readout = juce::String("--");
    if (latestSnapshot.channelCount > 0)
    {
        if (mode == AudioEngine::MeterMode::lufs)
        {
            readout = "M " + juce::String(latestSnapshot.momentaryLufs, 1) + " LUFS";
        }
        else
        {
            auto maximum = -120.0f;
            for (int channel = 0; channel < latestSnapshot.channelCount; ++channel)
            {
                const auto linear = mode == AudioEngine::MeterMode::truePeak
                                  ? latestSnapshot.truePeakLinear[static_cast<std::size_t>(channel)]
                                  : latestSnapshot.rmsLinear[static_cast<std::size_t>(channel)];
                maximum = juce::jmax(maximum, juce::Decibels::gainToDecibels(linear, -120.0f));
            }
            readout = "MAX " + juce::String(maximum, 1)
                    + (mode == AudioEngine::MeterMode::truePeak ? " dBTP" : " dBFS");
        }
    }

    g.setColour(RendererVisualStyle::accentInk());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::monoTextSize(),
                                          juce::Font::bold)));
    if (content.getHeight() > 0)
        g.drawText(readout,
                   content.removeFromTop(std::min(18, content.getHeight())),
                   juce::Justification::centredLeft);
    content.removeFromTop(std::min(8, content.getHeight()));

    if (summary || content.getHeight() < 20 || content.getWidth() < 20)
        return;

    auto meterArea = content;
    const auto channelCount = juce::jmax(1, latestSnapshot.channelCount);
    const auto gap = channelCount > 8 ? 2 : 3;
    const auto totalGap = gap * (channelCount - 1);
    const auto barWidth = juce::jmax(4, (meterArea.getWidth() - totalGap) / channelCount);
    const auto labelHeight = detail ? 24 : 8;
    const auto bars = meterArea.withTrimmedBottom(labelHeight);

    for (int channel = 0; channel < channelCount; ++channel)
    {
        auto bar = juce::Rectangle<int>(meterArea.getX() + channel * (barWidth + gap),
                                        bars.getY(),
                                        barWidth,
                                        bars.getHeight()).toFloat();
        g.setColour(RendererVisualStyle::mutedSurface());
        g.fillRoundedRectangle(bar, RendererVisualStyle::componentRadius());

        const auto level = displayedLevels[static_cast<std::size_t>(channel)];
        const auto fill = bar.withTop(bar.getBottom() - (bar.getHeight() * level));
        if (fill.getHeight() > 0.5f)
        {
            g.setColour(RendererVisualStyle::meterFill());
            g.fillRoundedRectangle(fill, RendererVisualStyle::componentRadius());
        }

        const auto held = heldLevels[static_cast<std::size_t>(channel)];
        if (held > 0.002f)
        {
            const auto y = bar.getBottom() - (bar.getHeight() * held);
            g.setColour(RendererVisualStyle::accentInk().withAlpha(0.55f));
            g.drawHorizontalLine(juce::roundToInt(y), bar.getX(), bar.getRight());
        }

        const auto label = latestSnapshot.channelCount > 0
                         ? ChannelLayoutCatalog::roleToAbbreviation(
                               latestSnapshot.roles[static_cast<std::size_t>(channel)])
                         : juce::String("-");
        if (detail)
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(juce::Font(juce::FontOptions(channelCount > 8 ? 8.0f : 9.0f)));
            g.drawText(label,
                       juce::Rectangle<int>(juce::roundToInt(bar.getX()),
                                            bars.getBottom() + 4,
                                            juce::roundToInt(bar.getWidth()),
                                            labelHeight - 4),
                       juce::Justification::centredTop,
                       false);
        }
    }

    if (detail)
    {
        g.setColour(RendererVisualStyle::secondaryText().withAlpha(0.72f));
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText(modeLabel(mode), getLocalBounds().reduced(14, 8).removeFromBottom(12),
                   juce::Justification::centredRight);
    }
}

juce::String OutputMeterPanelComponent::modeLabel(AudioEngine::MeterMode meterMode)
{
    switch (meterMode)
    {
        case AudioEngine::MeterMode::truePeak: return "PER CHANNEL TP";
        case AudioEngine::MeterMode::rms: return "PER CHANNEL RMS";
        case AudioEngine::MeterMode::lufs: return "K CONTRIBUTION + GLOBAL M";
    }
    return {};
}

juce::String OutputMeterPanelComponent::getAccessibilityValueSummary() const
{
    const auto snapshot = engine.getOutputMeterSnapshot();
    if (snapshot.channelCount <= 0)
        return "Device outputs sleeping";

    auto summary = juce::String(snapshot.channelCount) + " device outputs; " + modeLabel(mode);
    if (mode == AudioEngine::MeterMode::lufs)
        return summary + "; momentary " + juce::String(snapshot.momentaryLufs, 1) + " LUFS";

    for (int channel = 0; channel < snapshot.channelCount; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        const auto linear = mode == AudioEngine::MeterMode::truePeak
            ? snapshot.truePeakLinear[index]
            : snapshot.rmsLinear[index];
        summary += "; " + ChannelLayoutCatalog::roleToAbbreviation(snapshot.roles[index])
                 + " " + juce::String(juce::Decibels::gainToDecibels(linear, -120.0f), 1)
                 + (mode == AudioEngine::MeterMode::truePeak ? " dBTP" : " dBFS");
    }
    return summary;
}

std::unique_ptr<juce::AccessibilityHandler> OutputMeterPanelComponent::createAccessibilityHandler()
{
    std::unique_ptr<juce::AccessibilityValueInterface> value =
        std::make_unique<OutputMeterAccessibilityValue>(*this);
    return std::make_unique<juce::AccessibilityHandler>(
        *this,
        juce::AccessibilityRole::group,
        juce::AccessibilityActions {},
        juce::AccessibilityHandler::Interfaces(std::move(value)));
}

void OutputMeterPanelComponent::refreshActivity()
{
    if (coreReady && isShowing())
    {
        if (!lease) lease = engine.acquireOutputMeterAnalysisLease();
        if (lease) startTimerHz(30);
    }
    else
    {
        stopTimer();
        lease = {};
        display = {};
        repaint();
    }
}

void OutputMeterPanelComponent::timerCallback()
{
    if (!coreReady || !isShowing()) { refreshActivity(); return; }
    if (display.advance(engine.getOutputMeterSnapshot(), mode))
        repaint();
}
} // namespace singleeditor
