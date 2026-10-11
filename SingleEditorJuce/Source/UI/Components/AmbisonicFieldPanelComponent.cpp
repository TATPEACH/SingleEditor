#include "UI/Components/AmbisonicFieldPanelComponent.h"

#include <algorithm>
#include <cmath>

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
namespace
{
constexpr auto energyFloorDb = -36.0f;
constexpr auto mapPixelWidth = 160;
constexpr auto mapPixelHeight = 80;
constexpr auto squareRootTwo = 1.4142135623730951f;

class AmbisonicAccessibilityValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit AmbisonicAccessibilityValue(const AmbisonicFieldPanelComponent& ownerToUse)
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
    const AmbisonicFieldPanelComponent& owner;
};
}

class AmbisonicFieldPanelComponent::VisibilityWatcher final : public juce::ComponentMovementWatcher
{
public:
    explicit VisibilityWatcher(AmbisonicFieldPanelComponent& panel)
        : juce::ComponentMovementWatcher(&panel), owner(panel) {}

    void componentMovedOrResized(bool, bool) override {}
    void componentPeerChanged() override { owner.refreshAnalysisLease(); }
    void componentVisibilityChanged() override { owner.refreshAnalysisLease(); }

private:
    AmbisonicFieldPanelComponent& owner;
};

AmbisonicFieldPanelComponent::AmbisonicFieldPanelComponent(AudioEngine& engineToUse)
    : engine(engineToUse),
      geometryEditor(engineToUse)
{
    setName("Ambisonic Field and Speaker Geometry");
    setTitle("Ambisonic Field and Speaker Geometry");
    setComponentID("ambisonics-field-panel");
    setDescription("Ambisonic field and staged speaker geometry, loaded only on demand.");
    fieldTab.setName("Open Ambisonic Field View");
    fieldTab.setTitle("Open Ambisonic Field View");
    fieldTab.setComponentID("ambisonics-field-tab");
    geometryTab.setName("Open Speaker Geometry Editor");
    geometryTab.setTitle("Open Speaker Geometry Editor");
    geometryTab.setComponentID("ambisonics-geometry-tab");
    fieldTab.setClickingTogglesState(false);
    geometryTab.setClickingTogglesState(false);
    fieldTab.onClick = [this] { setActiveView(ActiveView::field); };
    geometryTab.onClick = [this] { setActiveView(ActiveView::geometry); };
    addAndMakeVisible(fieldTab);
    addAndMakeVisible(geometryTab);
    geometryViewport.setComponentID("ambisonics-geometry-viewport");
    geometryViewport.setViewedComponent(&geometryEditor, false);
    geometryViewport.setScrollBarsShown(true, false);
    geometryViewport.setScrollBarThickness(8);
    addChildComponent(geometryViewport);
    visibilityWatcher = std::make_unique<VisibilityWatcher>(*this);
    setActiveView(ActiveView::field);
}

AmbisonicFieldPanelComponent::~AmbisonicFieldPanelComponent()
{
    visibilityWatcher.reset();
    stopTimer();
    analysisLease = {};
}

void AmbisonicFieldPanelComponent::refreshAnalysisLease()
{
    if (isShowing() && activeView == ActiveView::field)
    {
        if (!analysisLease)
            analysisLease = engine.acquireAmbisonicFieldAnalysisLease();
        startTimerHz(15);
        return;
    }

    stopTimer();
    analysisLease = {};
    fieldSnapshot = {};
    energyMap = {};
    energySamples.clear();
    peakEnergyDb = -120.0f;
}

void AmbisonicFieldPanelComponent::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(ModernAppleLookAndFeel::panelBackground());
    g.fillRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::cardRadius());

    g.setColour(ModernAppleLookAndFeel::borderColour().withAlpha(0.65f));
    g.drawHorizontalLine(34, 10.0f, bounds.getRight() - 10.0f);
    if (activeView == ActiveView::geometry)
        return;

    const auto tier = static_cast<RendererInformationTier>(
        std::clamp(static_cast<int>(getProperties().getWithDefault(
                       "northStarInformationTier",
                       static_cast<int>(RendererInformationTier::standard))),
                   static_cast<int>(RendererInformationTier::summary),
                   static_cast<int>(RendererInformationTier::detail)));
    const auto summary = tier == RendererInformationTier::summary
                      || getWidth() < 210 || getHeight() < 210;
    const auto detail = !summary && tier == RendererInformationTier::detail
                     && getWidth() >= 260 && getHeight() >= 300;
    auto content = getLocalBounds().reduced(summary ? 10 : 14, summary ? 10 : 16);
    content.removeFromTop(28);
    if (content.isEmpty())
        return;

    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(ModernTypography::font(RendererVisualStyle::titleTextSize(), juce::Font::bold));
    g.drawText(content.getWidth() < 150 ? "Field" : "Ambisonic Field",
               content.removeFromTop(20), juce::Justification::centredLeft);

    const auto orderText = fieldSnapshot.valid
                         ? juce::String(fieldSnapshot.order) + "OA ACN / SN3D"
                         : juce::String("Analysis idle");
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(ModernTypography::font(RendererVisualStyle::captionTextSize(), juce::Font::bold));
    g.drawText(orderText, content.removeFromTop(16), juce::Justification::centredLeft);

    if (summary)
    {
        const auto status = fieldSnapshot.valid
            ? juce::String(outputSnapshot.channelCount) + " rendered outputs"
            : juce::String("Play to analyse");
        g.setFont(ModernTypography::font(RendererVisualStyle::bodyTextSize()));
        g.drawFittedText(status, content, juce::Justification::centredLeft, 2);
        return;
    }

    if (detail && fieldSnapshot.valid)
    {
        const auto orientation = "Y " + juce::String(fieldSnapshot.orientation.yawDegrees, 0)
                               + "  P " + juce::String(fieldSnapshot.orientation.pitchDegrees, 0)
                               + "  R " + juce::String(fieldSnapshot.orientation.rollDegrees, 0);
        g.setColour(ModernAppleLookAndFeel::accentOrange());
        g.setFont(ModernTypography::font(10.0f, juce::Font::bold));
        g.drawText(orientation, content.removeFromTop(16), juce::Justification::centredLeft);
    }
    else if (detail)
    {
        content.removeFromTop(16);
    }

    content.removeFromTop(detail ? 10 : 4);
    auto legendBounds = detail ? content.removeFromBottom(48) : juce::Rectangle<int> {};
    // Keep space for the idle cue during playback too, so the map does not
    // change scale or position when its first analysis snapshot arrives.
    constexpr auto cueHeight = 28;
    const auto mapWidth = juce::jmin(content.getWidth(),
                                    (content.getHeight() - cueHeight) * 2);
    if (mapWidth < 160)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.drawFittedText("Resize for spatial map", content, juce::Justification::centred, 2);
        return;
    }
    // Hammer projection is 2:1. A short Mag must shrink both axes together.
    const auto mapHeight = mapWidth / 2;
    auto mapGroup = juce::Rectangle<int>(mapHeight * 2, mapHeight + cueHeight)
                        .withCentre(content.getCentre());
    const auto mapBounds = mapGroup.removeFromTop(mapHeight);

    drawEnergyMap(g, mapBounds);
    drawMapGrid(g, mapBounds.toFloat());
    drawOutputSpeakers(g, mapBounds.toFloat());

    if (!fieldSnapshot.valid)
    {
        g.setColour(ModernAppleLookAndFeel::secondaryText());
        g.setFont(ModernTypography::font(10.5f));
        g.drawText(mapBounds.getWidth() < 240 ? "Play to analyse" : "Play to analyse the sound field",
                   mapGroup,
                   juce::Justification::centred,
                   true);
    }

    if (detail && !legendBounds.isEmpty())
        drawLegend(g, legendBounds);
}

void AmbisonicFieldPanelComponent::timerCallback()
{
    if (!isShowing() || activeView == ActiveView::geometry)
        return;

    const auto nextField = engine.getAmbisonicFieldSnapshot();
    const auto fieldChanged = nextField.valid != fieldSnapshot.valid
                           || nextField.sequence != fieldSnapshot.sequence;
    fieldSnapshot = nextField;
    outputSnapshot = engine.getOutputMeterSnapshot();
    if (fieldChanged)
        rebuildEnergyMap();
    repaint();
}

void AmbisonicFieldPanelComponent::setActiveView(ActiveView view)
{
    activeView = view;
    const auto geometryVisible = activeView == ActiveView::geometry;
    fieldTab.setToggleState(!geometryVisible, juce::dontSendNotification);
    geometryTab.setToggleState(geometryVisible, juce::dontSendNotification);
    geometryEditor.setVisible(geometryVisible);
    geometryViewport.setVisible(geometryVisible);
    refreshAnalysisLease();
    resized();
    repaint();
}

void AmbisonicFieldPanelComponent::setAmbisonicOrder(int order)
{
    geometryEditor.setAmbisonicOrder(order);
}

void AmbisonicFieldPanelComponent::resized()
{
    auto area = getLocalBounds().reduced(10, 6);
    auto tabs = area.removeFromTop(26);
    fieldTab.setBounds(tabs.removeFromLeft(juce::jmin(68, tabs.getWidth() * 2 / 5)));
    tabs.removeFromLeft(5);
    geometryTab.setBounds(tabs.removeFromLeft(82));
    area.removeFromTop(7);
    geometryViewport.setBounds(area);
    const auto contentHeight = SpeakerGeometryMagComponent::contentHeightForViewport(
        area.getWidth(), area.getHeight(), geometryViewport.getScrollBarThickness());
    const auto needsScroll = area.getHeight() < contentHeight;
    geometryEditor.setSize(juce::jmax(1, area.getWidth()
        - (needsScroll ? geometryViewport.getScrollBarThickness() : 0)),
        contentHeight);
}

void AmbisonicFieldPanelComponent::focusOfChildComponentChanged(FocusChangeType)
{
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (activeView != ActiveView::geometry || focused == nullptr
        || !geometryEditor.isParentOf(focused))
        return;

    const auto control = geometryEditor.getLocalArea(focused, focused->getLocalBounds());
    const auto visible = geometryViewport.getViewArea();
    if (control.getY() < visible.getY())
        geometryViewport.setViewPosition(0, control.getY());
    else if (control.getBottom() > visible.getBottom())
        geometryViewport.setViewPosition(0, control.getBottom() - visible.getHeight());
}

juce::String AmbisonicFieldPanelComponent::getAccessibilityValueSummary() const
{
    if (activeView == ActiveView::geometry)
        return "Speaker geometry editor active; canonical roles and directions only; "
             + juce::String(geometryEditor.getAmbisonicOrder())
             + " order Ambisonics; physical device channels absent";

    const auto field = engine.getAmbisonicFieldSnapshot();
    if (!field.valid)
        return "Ambisonic field analysis sleeping";

    const auto output = engine.getOutputMeterSnapshot();
    return juce::String(field.order) + " order ACN SN3D; yaw "
         + juce::String(field.orientation.yawDegrees, 0) + " degrees; pitch "
         + juce::String(field.orientation.pitchDegrees, 0) + " degrees; roll "
         + juce::String(field.orientation.rollDegrees, 0) + " degrees; "
         + juce::String(output.channelCount) + " device outputs";
}

std::unique_ptr<juce::AccessibilityHandler> AmbisonicFieldPanelComponent::createAccessibilityHandler()
{
    std::unique_ptr<juce::AccessibilityValueInterface> value =
        std::make_unique<AmbisonicAccessibilityValue>(*this);
    return std::make_unique<juce::AccessibilityHandler>(
        *this,
        juce::AccessibilityRole::group,
        juce::AccessibilityActions {},
        juce::AccessibilityHandler::Interfaces(std::move(value)));
}

void AmbisonicFieldPanelComponent::rebuildEnergyMap()
{
    if (!fieldSnapshot.valid)
    {
        energyMap = {};
        energySamples.clear();
        peakEnergyDb = -120.0f;
        return;
    }

    if (!energyMap.isValid()
        || energyMap.getWidth() != mapPixelWidth
        || energyMap.getHeight() != mapPixelHeight)
    {
        energyMap = juce::Image(juce::Image::ARGB, mapPixelWidth, mapPixelHeight, true);
        energySamples.resize(static_cast<std::size_t>(mapPixelWidth * mapPixelHeight));
    }

    auto maximumEnergy = 0.0f;
    for (auto y = 0; y < mapPixelHeight; ++y)
    {
        const auto normalisedY = 1.0f - (2.0f * (static_cast<float>(y) + 0.5f)
                                        / static_cast<float>(mapPixelHeight));
        for (auto x = 0; x < mapPixelWidth; ++x)
        {
            const auto normalisedX = (2.0f * (static_cast<float>(x) + 0.5f)
                                      / static_cast<float>(mapPixelWidth)) - 1.0f;
            float azimuth = 0.0f;
            float elevation = 0.0f;
            const auto index = static_cast<std::size_t>((y * mapPixelWidth) + x);
            if (!directionForHammerPoint(normalisedX,
                                         normalisedY,
                                         azimuth,
                                         elevation))
            {
                energySamples[index] = -1.0f;
                continue;
            }

            const auto energy = AmbisonicFieldAnalyzer::energyAt(fieldSnapshot,
                                                                  azimuth,
                                                                  elevation);
            energySamples[index] = energy;
            maximumEnergy = juce::jmax(maximumEnergy, energy);
        }
    }

    peakEnergyDb = 10.0f * std::log10(juce::jmax(maximumEnergy, 1.0e-12f));
    juce::Image::BitmapData pixels(energyMap, juce::Image::BitmapData::writeOnly);
    for (auto y = 0; y < mapPixelHeight; ++y)
    {
        for (auto x = 0; x < mapPixelWidth; ++x)
        {
            const auto energy = energySamples[static_cast<std::size_t>((y * mapPixelWidth) + x)];
            if (energy < 0.0f || maximumEnergy <= 1.0e-12f)
            {
                pixels.setPixelColour(x, y, juce::Colours::transparentBlack);
                continue;
            }

            const auto relativeDb = 10.0f * std::log10(
                juce::jmax(energy / maximumEnergy, 1.0e-8f));
            const auto normalised = juce::jlimit(0.0f,
                                                  1.0f,
                                                  (relativeDb - energyFloorDb) / -energyFloorDb);
            pixels.setPixelColour(x,
                                  y,
                                  heatColour(normalised,
                                             0.10f + (0.90f * std::sqrt(normalised))));
        }
    }
}

bool AmbisonicFieldPanelComponent::directionForHammerPoint(float normalisedX,
                                                            float normalisedY,
                                                            float& azimuthDegrees,
                                                            float& elevationDegrees) noexcept
{
    // Raster inversion keeps the visualizer independent of third-party generated meshes.
    if ((normalisedX * normalisedX) + (normalisedY * normalisedY) > 1.0f)
        return false;

    const auto x = normalisedX * 2.0f * squareRootTwo;
    const auto y = normalisedY * squareRootTwo;
    const auto z = std::sqrt(juce::jmax(0.0f,
                                        1.0f - ((x * x) / 16.0f) - ((y * y) / 4.0f)));
    const auto longitude = 2.0f * std::atan2(z * x, 2.0f * ((2.0f * z * z) - 1.0f));
    const auto latitude = std::asin(juce::jlimit(-1.0f, 1.0f, z * y));
    azimuthDegrees = -juce::radiansToDegrees(longitude);
    elevationDegrees = juce::radiansToDegrees(latitude);
    return std::isfinite(azimuthDegrees) && std::isfinite(elevationDegrees);
}

juce::Point<float> AmbisonicFieldPanelComponent::hammerProject(
    float azimuthDegrees,
    float elevationDegrees,
    juce::Rectangle<float> mapBounds) noexcept
{
    const auto longitude = juce::degreesToRadians(-azimuthDegrees);
    const auto latitude = juce::degreesToRadians(elevationDegrees);
    const auto denominator = std::sqrt(juce::jmax(
        1.0e-6f,
        1.0f + (std::cos(latitude) * std::cos(longitude * 0.5f))));
    const auto x = 2.0f * squareRootTwo * std::cos(latitude)
                 * std::sin(longitude * 0.5f) / denominator;
    const auto y = squareRootTwo * std::sin(latitude) / denominator;
    return { mapBounds.getCentreX() + ((x / (2.0f * squareRootTwo)) * mapBounds.getWidth() * 0.5f),
             mapBounds.getCentreY() - ((y / squareRootTwo) * mapBounds.getHeight() * 0.5f) };
}

juce::Colour AmbisonicFieldPanelComponent::heatColour(float normalisedEnergy, float alpha)
{
    const auto amount = juce::jlimit(0.0f, 1.0f, normalisedEnergy);
    const auto cold = juce::Colour(0xff174c78);
    const auto cyan = juce::Colour(0xff1598a5);
    const auto green = juce::Colour(0xff55bd72);
    const auto hot = juce::Colour(0xffffdf58);
    auto colour = cold;
    if (amount < 0.38f)
        colour = cold.interpolatedWith(cyan, amount / 0.38f);
    else if (amount < 0.72f)
        colour = cyan.interpolatedWith(green, (amount - 0.38f) / 0.34f);
    else
        colour = green.interpolatedWith(hot, (amount - 0.72f) / 0.28f);
    return colour.withAlpha(juce::jlimit(0.0f, 1.0f, alpha));
}

bool AmbisonicFieldPanelComponent::speakerAngles(SpeakerRole role,
                                                  float& azimuthDegrees,
                                                  float& elevationDegrees) noexcept
{
    elevationDegrees = 0.0f;
    switch (role)
    {
        case SpeakerRole::L: azimuthDegrees = 30.0f; return true;
        case SpeakerRole::R: azimuthDegrees = -30.0f; return true;
        case SpeakerRole::C: azimuthDegrees = 0.0f; return true;
        case SpeakerRole::Lc: azimuthDegrees = 15.0f; return true;
        case SpeakerRole::Rc: azimuthDegrees = -15.0f; return true;
        case SpeakerRole::Lw: azimuthDegrees = 60.0f; return true;
        case SpeakerRole::Rw: azimuthDegrees = -60.0f; return true;
        case SpeakerRole::Ls:
        case SpeakerRole::Lss: azimuthDegrees = 100.0f; return true;
        case SpeakerRole::Rs:
        case SpeakerRole::Rss: azimuthDegrees = -100.0f; return true;
        case SpeakerRole::Lsr: azimuthDegrees = 145.0f; return true;
        case SpeakerRole::Rsr: azimuthDegrees = -145.0f; return true;
        case SpeakerRole::Ltf: azimuthDegrees = 30.0f; elevationDegrees = 45.0f; return true;
        case SpeakerRole::Rtf: azimuthDegrees = -30.0f; elevationDegrees = 45.0f; return true;
        case SpeakerRole::Ltm: azimuthDegrees = 90.0f; elevationDegrees = 60.0f; return true;
        case SpeakerRole::Rtm: azimuthDegrees = -90.0f; elevationDegrees = 60.0f; return true;
        case SpeakerRole::Ltr: azimuthDegrees = 145.0f; elevationDegrees = 45.0f; return true;
        case SpeakerRole::Rtr: azimuthDegrees = -145.0f; elevationDegrees = 45.0f; return true;
        default: return false;
    }
}

void AmbisonicFieldPanelComponent::drawEnergyMap(juce::Graphics& g,
                                                  juce::Rectangle<int> mapBounds) const
{
    g.setColour(juce::Colour(0xff10253f));
    g.fillEllipse(mapBounds.toFloat());
    if (energyMap.isValid())
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImageWithin(energyMap,
                          mapBounds.getX(),
                          mapBounds.getY(),
                          mapBounds.getWidth(),
                          mapBounds.getHeight(),
                          juce::RectanglePlacement::stretchToFit,
                          false);
    }
}

void AmbisonicFieldPanelComponent::drawMapGrid(juce::Graphics& g,
                                                juce::Rectangle<float> mapBounds) const
{
    const auto compact = mapBounds.getWidth() < 300.0f;
    const auto gridStep = compact ? 60 : 30;
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    g.drawEllipse(mapBounds, 1.0f);

    for (auto azimuth = compact ? -120 : -150; azimuth <= 150; azimuth += gridStep)
    {
        juce::Path path;
        for (auto elevation = -90; elevation <= 90; elevation += 3)
        {
            const auto point = hammerProject(static_cast<float>(azimuth),
                                             static_cast<float>(elevation),
                                             mapBounds);
            if (elevation == -90)
                path.startNewSubPath(point);
            else
                path.lineTo(point);
        }
        g.strokePath(path, juce::PathStrokeType(azimuth == 0 ? 0.9f : 0.45f));
    }

    for (auto elevation = -60; elevation <= 60; elevation += gridStep)
    {
        juce::Path path;
        for (auto azimuth = -180; azimuth <= 180; azimuth += 3)
        {
            const auto point = hammerProject(static_cast<float>(azimuth),
                                             static_cast<float>(elevation),
                                             mapBounds);
            if (azimuth == -180)
                path.startNewSubPath(point);
            else
                path.lineTo(point);
        }
        g.strokePath(path, juce::PathStrokeType(elevation == 0 ? 0.9f : 0.45f));
    }

    const auto labelFont = ModernTypography::font(9.5f, juce::Font::bold);
    g.setFont(labelFont);
    const auto drawLabel = [&](const juce::String& text, float centreX, float top)
    {
        const auto width = juce::GlyphArrangement::getStringWidth(labelFont, text) + 8.0f;
        const juce::Rectangle<float> label(
            juce::jlimit(mapBounds.getX() + 2.0f, mapBounds.getRight() - width - 2.0f,
                         centreX - width * 0.5f),
            juce::jmax(mapBounds.getY() + 2.0f, top), width, 13.0f);
        // Heat can reach bright yellow; white labels need their own dark ink bed.
        g.setColour(juce::Colour(0xff061220).withAlpha(0.84f));
        g.fillRoundedRectangle(label, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.94f));
        g.drawText(text, label, juce::Justification::centred, false);
    };
    const auto labelAt = [&](const juce::String& text, float azimuth, float elevation)
    {
        const auto point = hammerProject(azimuth, elevation, mapBounds);
        drawLabel(text, point.x, point.y - 17.0f);
    };
    labelAt(compact ? "F" : "FRONT", 0.0f, 0.0f);
    labelAt(compact ? "L" : "LEFT", 90.0f, 0.0f);
    labelAt(compact ? "R" : "RIGHT", -90.0f, 0.0f);
    labelAt(compact ? "T" : "TOP", 0.0f, 76.0f);
    drawLabel(compact ? "B" : "BACK", mapBounds.getX(), mapBounds.getCentreY() - 17.0f);
    drawLabel(compact ? "B" : "BACK", mapBounds.getRight(), mapBounds.getCentreY() - 17.0f);
}

void AmbisonicFieldPanelComponent::drawOutputSpeakers(
    juce::Graphics& g,
    juce::Rectangle<float> mapBounds) const
{
    for (auto channel = 0; channel < outputSnapshot.channelCount; ++channel)
    {
        const auto index = static_cast<std::size_t>(channel);
        float azimuth = 0.0f;
        float elevation = 0.0f;
        if (!speakerAngles(outputSnapshot.roles[index], azimuth, elevation))
            continue;

        const auto point = hammerProject(azimuth, elevation, mapBounds);
        const auto levelDb = juce::Decibels::gainToDecibels(outputSnapshot.rmsLinear[index], -72.0f);
        const auto level = juce::jlimit(0.0f, 1.0f, (levelDb + 60.0f) / 60.0f);
        const auto size = 4.0f + (level * 5.0f);
        const auto dotBounds = juce::Rectangle<float>(size, size).withCentre(point);

        g.setColour(ModernAppleLookAndFeel::accentOrange().withAlpha(0.45f + (level * 0.55f)));
        g.fillEllipse(dotBounds);
        g.setColour(juce::Colours::white.withAlpha(0.82f));
        g.drawEllipse(dotBounds.expanded(1.0f), 0.8f);
    }
}

void AmbisonicFieldPanelComponent::drawLegend(juce::Graphics& g,
                                               juce::Rectangle<int> legendBounds) const
{
    auto barBounds = legendBounds.removeFromTop(12).reduced(2, 2).toFloat();
    juce::ColourGradient gradient(heatColour(0.0f, 1.0f),
                                  barBounds.getX(),
                                  barBounds.getCentreY(),
                                  heatColour(1.0f, 1.0f),
                                  barBounds.getRight(),
                                  barBounds.getCentreY(),
                                  false);
    gradient.addColour(0.38, heatColour(0.38f, 1.0f));
    gradient.addColour(0.72, heatColour(0.72f, 1.0f));
    g.setGradientFill(gradient);
    g.fillRoundedRectangle(barBounds, 3.0f);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(ModernTypography::font(8.0f));
    auto scaleBounds = legendBounds.removeFromTop(13);
    g.drawText("-36", scaleBounds.removeFromLeft(30), juce::Justification::centredLeft);
    g.drawText("0 dB rel", scaleBounds.removeFromRight(44), juce::Justification::centredRight);
    g.drawText("-18", scaleBounds, juce::Justification::centred);

    const auto peakText = fieldSnapshot.valid
                        ? "PEAK " + juce::String(peakEnergyDb, 1) + " dB  /  ORANGE DOTS = OUTPUT"
                        : juce::String("FIELD HEAT / ORANGE DOTS = OUTPUT");
    g.setFont(ModernTypography::font(7.5f, juce::Font::bold));
    g.drawText(peakText, legendBounds.removeFromTop(12), juce::Justification::centredLeft);
}
} // namespace singleeditor
