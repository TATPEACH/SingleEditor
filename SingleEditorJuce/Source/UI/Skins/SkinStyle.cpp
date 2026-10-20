#include "UI/Skins/SkinStyle.h"

#include <atomic>
#include <cmath>

#include "UI/LookAndFeel/ModernPanelPainter.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Skins/SkinPainters.h"

namespace singleeditor
{
namespace
{
std::atomic<ModernSkin> selectedSkin { ModernSkin::original };

// Each optional skin lives in its own file and registers one line under its
// label here, so a single skin can be reverted or cherry-picked on its own.
const skins::Definition* definition(ModernSkin skin) noexcept
{
    switch (skin)
    {
        // D  Layered Tiles
        case ModernSkin::tiles: return &skins::tilesSkin();
        // A  Soft
        case ModernSkin::soft: return &skins::softSkin();
        // C  Liquid Glass
        case ModernSkin::liquid: return &skins::liquidSkin();
        case ModernSkin::original: break;
    }
    return nullptr;
}

const skins::Definition* activeDefinition() noexcept { return definition(SkinStyle::active()); }

// Only reached if a hook runs while Original is active; mirrors Modern Glass.
constexpr SkinExtras originalExtras {
    0x0f000000, 0x1a000000, 0xff0e71fe, 0xffff9f0a, 0xffff3b30, 0x00000000, 0x33ffffff, 0x00000000, 0x14000000
};

const skins::Painter& painter()
{
    if (const auto* skin = activeDefinition())
        return skin->painter;
    static const skins::Painter flat;
    return flat;
}
} // namespace

ModernSkin SkinStyle::active() noexcept
{
    if (RendererSelection::selected() == RendererKind::classic)
        return ModernSkin::original;
    return selectedSkin.load(std::memory_order_relaxed);
}

void SkinStyle::setActive(ModernSkin skin) noexcept
{
    selectedSkin.store(isAvailable(skin) ? skin : ModernSkin::original, std::memory_order_relaxed);
}

bool SkinStyle::isAvailable(ModernSkin skin) noexcept
{
    return skin == ModernSkin::original || definition(skin) != nullptr;
}

bool SkinStyle::supportsNight(ModernSkin skin) noexcept
{
    const auto* skinDefinition = definition(skin);
    return skinDefinition == nullptr || skinDefinition->night != nullptr;
}

ModernSkin SkinStyle::parse(std::string_view value, ModernSkin fallback) noexcept
{
    for (const auto skin : { ModernSkin::original, ModernSkin::soft, ModernSkin::liquid, ModernSkin::tiles })
        if (value == id(skin))
            return skin;
    return fallback;
}

std::string_view SkinStyle::id(ModernSkin skin) noexcept
{
    switch (skin)
    {
        case ModernSkin::soft: return "soft";
        case ModernSkin::liquid: return "liquid";
        case ModernSkin::tiles: return "tiles";
        case ModernSkin::original: break;
    }
    return "original";
}

const char* SkinStyle::menuName(ModernSkin skin) noexcept
{
    switch (skin)
    {
        case ModernSkin::soft: return "Soft (light only)";
        case ModernSkin::liquid: return "Liquid Glass";
        case ModernSkin::tiles: return "Layered Tiles";
        case ModernSkin::original: break;
    }
    return "Modern Glass (default)";
}

const ModernGlassTokens::Palette& SkinStyle::palette(bool night) noexcept
{
    if (const auto* skin = activeDefinition())
        return night && skin->night != nullptr ? *skin->night : skin->light;
    return night ? ModernGlassTokens::night : ModernGlassTokens::light;
}

const SkinExtras& SkinStyle::extras(bool night) noexcept
{
    if (const auto* skin = activeDefinition())
        return night && skin->nightExtras != nullptr ? *skin->nightExtras : skin->lightExtras;
    return originalExtras;
}

SkinGeometry SkinStyle::geometry() noexcept
{
    if (const auto* skin = activeDefinition())
        return skin->geometry;
    return { 10.0f, 7.0f, 10, 12 };
}

juce::Colour SkinStyle::signal() { return juce::Colour(extras(RendererVisualStyle::isNight()).signal); }

void SkinStyle::paintWindowBackground(juce::Graphics& g, juce::Rectangle<float> area,
                                      juce::Point<float> origin, juce::Rectangle<float> window)
{ painter().windowBackground(g, area, origin, window); }
void SkinStyle::paintTitleBar(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> origin,
                              juce::Rectangle<float> window)
{ painter().titleBar(g, area, origin, window); }
void SkinStyle::paintToolbar(juce::Graphics& g, juce::Component& surface) { painter().toolbar(g, surface); }
void SkinStyle::paintStatusBar(juce::Graphics& g, juce::Rectangle<float> bounds) { painter().statusBar(g, bounds); }
void SkinStyle::paintPanel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
                           int icon, int trailingControls)
{ painter().panel(g, bounds, title, icon, trailingControls); }
void SkinStyle::paintPanelShadow(juce::Graphics& g, juce::Rectangle<float> tile) { painter().panelShadow(g, tile); }
void SkinStyle::paintPanelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live)
{ painter().panelOutline(g, tile, live); }
void SkinStyle::paintDataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius)
{ painter().dataWell(g, area, radius); }
void SkinStyle::prepareWaveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour colour)
{ painter().waveformLane(g, lane, colour); }
void SkinStyle::paintClock(juce::Graphics& g, juce::Rectangle<float> bounds,
                           const juce::String& main, const juce::String& frames)
{ painter().clock(g, bounds, main, frames); }
void SkinStyle::paintIconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down, bool& onAccent)
{ painter().iconButton(g, button, hovered, down, onAccent); }
bool SkinStyle::paintButtonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                                      juce::Colour& textColour)
{ return painter().buttonBackground(g, button, hovered, down, textColour); }
void SkinStyle::paintSwitch(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled, bool focused)
{ painter().switchTrack(g, track, on, enabled, focused); }
void SkinStyle::paintComboBox(juce::Graphics& g, juce::ComboBox& box, bool down) { painter().comboBox(g, box, down); }
void SkinStyle::paintLinearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position, juce::Slider& slider)
{ painter().linearSlider(g, track, position, slider); }
void SkinStyle::paintPopupBackground(juce::Graphics& g, juce::Rectangle<float> bounds)
{ painter().popupBackground(g, bounds); }
void SkinStyle::paintPopupHighlight(juce::Graphics& g, juce::Rectangle<float> row) { painter().popupHighlight(g, row); }
bool SkinStyle::popupHighlightUsesAccent() noexcept { return painter().popupHighlightUsesAccent(); }
void SkinStyle::paintKnob(juce::Graphics& g, juce::Rectangle<float> dial, float angle, juce::Colour indicator,
                          bool enabled, bool focused)
{ painter().knob(g, dial, angle, indicator, enabled, focused); }
void SkinStyle::paintMeterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail, bool loudness)
{ painter().meterBar(g, bar, level, trail, loudness); }
void SkinStyle::paintMeterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held)
{ painter().meterBridge(g, bounds, level, held); }

namespace skins
{
juce::Colour colour(std::uint32_t argb) { return juce::Colour(argb); }
bool isNight() { return RendererVisualStyle::isNight(); }
const ModernGlassTokens::Palette& pal() { return SkinStyle::palette(isNight()); }
const SkinExtras& ext() { return SkinStyle::extras(isNight()); }
juce::String role(const juce::Component& component) { return component.getProperties()["skinRole"].toString(); }
juce::Font uiFont(float emPixels, bool semibold)
{
    return ModernTypography::font(emPixels, semibold ? juce::Font::bold : juce::Font::plain);
}

void softShadow(juce::Graphics& g, juce::Rectangle<float> shape, float radius,
                juce::Point<float> offset, float blur, juce::Colour shadow)
{
    if (shape.isEmpty() || shadow.getAlpha() == 0) return;
    const auto base = shape.translated(offset.x, offset.y);
    // Meters and playheads repaint small regions many times a second.
    if (!g.getClipBounds().toFloat().intersects(base.expanded(blur + 1.0f))) return;
    const auto steps = juce::jmax(1, juce::roundToInt(blur));
    for (int i = 1; i <= steps; ++i)
    {
        const auto t = static_cast<float>(i) / static_cast<float>(steps + 1);
        const auto falloff = (1.0f - t) * (1.0f - t);
        g.setColour(shadow.withMultipliedAlpha(falloff * 2.2f / static_cast<float>(steps)));
        g.drawRoundedRectangle(base.expanded(static_cast<float>(i) - 0.5f),
                               radius + static_cast<float>(i) - 0.5f, 1.0f);
    }
}

void innerShadow(juce::Graphics& g, juce::Rectangle<float> shape, float radius,
                 juce::Point<float> offset, float blur, juce::Colour shadow)
{
    if (shape.isEmpty() || shadow.getAlpha() == 0) return;
    const juce::Graphics::ScopedSaveState saved(g);
    juce::Path clip;
    clip.addRoundedRectangle(shape, radius);
    g.reduceClipRegion(clip);
    const auto transparent = shadow.withAlpha(0.0f);
    const auto band = juce::jmax(1.0f, blur);
    if (offset.y > 0.0f)
    {
        g.setGradientFill(juce::ColourGradient(shadow, 0.0f, shape.getY(), transparent, 0.0f, shape.getY() + band, false));
        g.fillRect(shape.withHeight(band));
    }
    else if (offset.y < 0.0f)
    {
        g.setGradientFill(juce::ColourGradient(shadow, 0.0f, shape.getBottom(), transparent, 0.0f, shape.getBottom() - band, false));
        g.fillRect(shape.withTop(shape.getBottom() - band));
    }
    if (offset.x > 0.0f)
    {
        g.setGradientFill(juce::ColourGradient(shadow, shape.getX(), 0.0f, transparent, shape.getX() + band, 0.0f, false));
        g.fillRect(shape.withWidth(band));
    }
    else if (offset.x < 0.0f)
    {
        g.setGradientFill(juce::ColourGradient(shadow, shape.getRight(), 0.0f, transparent, shape.getRight() - band, 0.0f, false));
        g.fillRect(shape.withLeft(shape.getRight() - band));
    }
}

void rimLight(juce::Graphics& g, juce::Rectangle<float> shape, float radius, juce::Colour light, float thickness)
{
    if (shape.isEmpty() || light.getAlpha() == 0) return;
    const auto inner = shape.reduced(thickness * 0.5f);
    g.setGradientFill(juce::ColourGradient(light, 0.0f, inner.getY(), light.withAlpha(0.0f), 0.0f,
                                           inner.getY() + juce::jmin(inner.getHeight(), 28.0f), false));
    g.drawRoundedRectangle(inner, juce::jmax(0.0f, radius - thickness * 0.5f), thickness);
}

void paintPanelGlyph(juce::Graphics& g, juce::Rectangle<float> glyph, int icon, juce::Colour ink)
{
    g.setColour(ink);
    ModernPanelPainter::paintIcon(g, glyph, static_cast<ModernPanelPainter::Icon>(icon));
}

juce::Rectangle<float> iconBox(juce::Rectangle<int> header)
{
    return { static_cast<float>(header.getX() + 6), static_cast<float>(header.getCentreY()) - 11.0f, 22.0f, 22.0f };
}

void paintPanelTitle(juce::Graphics& g, juce::Rectangle<int> header, const juce::String& title, int trailingControls)
{
    // Starts where the Waveform tile's own title Label draws its text.
    g.setColour(colour(pal().text));
    g.setFont(uiFont(13.0f, true));
    g.drawText(title, header.withTrimmedLeft(35).withTrimmedRight(30 + trailingControls),
               juce::Justification::centredLeft, true);
}

juce::Rectangle<int> panelHeader(juce::Rectangle<int> bounds)
{
    return bounds.removeFromTop(juce::jmin(ModernPanelPainter::headerHeight, bounds.getHeight()));
}

void paintLiveRing(juce::Graphics& g, juce::Rectangle<float> tile, float radius)
{
    const auto amber = colour(ext().live);
    g.setColour(amber.withAlpha(0.85f));
    g.drawRoundedRectangle(tile.reduced(0.75f), radius - 0.75f, 1.5f);
    for (int i = 1; i <= 4; ++i)
    {
        g.setColour(amber.withAlpha(0.10f - static_cast<float>(i) * 0.02f));
        g.drawRoundedRectangle(tile.reduced(1.5f + static_cast<float>(i)),
                               juce::jmax(0.0f, radius - 1.5f - static_cast<float>(i)), 1.0f);
    }
}

void setWaveformBrush(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour signal)
{
    juce::ColourGradient peaks(signal.withAlpha(0.42f), lane.getCentreX(), lane.getY(),
                               signal.withAlpha(0.42f), lane.getCentreX(), lane.getBottom(), false);
    peaks.addColour(0.40, signal.withAlpha(0.92f));
    peaks.addColour(0.60, signal.withAlpha(0.92f));
    g.setGradientFill(peaks);
}

void paintClockText(juce::Graphics& g, juce::Rectangle<float> display, const juce::String& main,
                    const juce::String& frames)
{
    auto major = uiFont(22.0f, true);
    const auto minor = uiFont(13.0f, true);
    const auto minorWidth = juce::GlyphArrangement::getStringWidth(minor, frames);
    const auto available = juce::jmax(1.0f, display.getWidth() - 16.0f - minorWidth);
    const auto natural = juce::GlyphArrangement::getStringWidth(major, main);
    if (natural > available) major.setHeight(major.getHeight() * available / natural);
    const auto majorWidth = juce::GlyphArrangement::getStringWidth(major, main);
    const auto x = display.getX() + juce::jmax(0.0f, (display.getWidth() - majorWidth - minorWidth - 2.0f) * 0.5f);
    const auto baseline = display.getCentreY() + major.getAscent() * 0.5f - major.getDescent() * 0.5f;
    g.setFont(major);
    g.setColour(colour(pal().text));
    g.drawSingleLineText(main, juce::roundToInt(x), juce::roundToInt(baseline));
    g.setFont(minor);
    g.setColour(colour(pal().secondaryText));
    g.drawSingleLineText(frames, juce::roundToInt(x + majorWidth + 2.0f), juce::roundToInt(baseline));
}

void paintMeterFill(juce::Graphics& g, juce::Rectangle<float> bar, float radius, float level, float trail,
                    bool loudness)
{
    juce::ColourGradient zones(colour(ext().signal), bar.getBottomLeft(), colour(ext().danger), bar.getTopLeft(), false);
    zones.addColour(loudness ? 0.70 : 0.78, colour(ext().signal));
    zones.addColour(loudness ? 0.78 : 0.86, colour(ext().live));
    zones.addColour(loudness ? 0.90 : 0.94, colour(ext().live));
    const auto fill = bar.withTop(bar.getBottom() - bar.getHeight() * juce::jlimit(0.0f, 1.0f, level));
    if (fill.getHeight() >= 0.5f)
    {
        g.setGradientFill(zones);
        g.fillRoundedRectangle(fill, juce::jmax(0.0f, radius));
    }
    if (trail > 0.002f)
    {
        g.setColour(colour(pal().text).withAlpha(0.72f));
        g.fillRect(bar.getX(), static_cast<float>(juce::roundToInt(bar.getBottom() - bar.getHeight() * trail)) - 0.5f,
                   bar.getWidth(), 1.5f);
    }
}

juce::Path segmentPath(juce::Rectangle<float> r, float radius, int connectedEdges)
{
    juce::Path path;
    const bool left = (connectedEdges & juce::Button::ConnectedOnLeft) == 0;
    const bool right = (connectedEdges & juce::Button::ConnectedOnRight) == 0;
    path.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius,
                             left, right, left, right);
    return path;
}

bool isSegmentButton(const juce::Button& button)
{
    return button.getConnectedEdgeFlags() != 0
        || button.getComponentID().startsWith("appearance.theme.");
}

bool isGhostButton(const juce::Button& button)
{
    const auto& properties = button.getProperties();
    const auto iconOnly = static_cast<bool>(properties.getWithDefault("iconOnly", false));
    return static_cast<bool>(properties.getWithDefault("ghostButton", false))
        || (iconOnly && properties["iconName"].toString().isNotEmpty());
}

// ---------------- Flat base: the Layered Tiles look ----------------

void Painter::windowBackground(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float>,
                               juce::Rectangle<float>) const
{
    g.setColour(colour(pal().background));
    g.fillRect(area);
}

void Painter::titleBar(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> origin,
                       juce::Rectangle<float> window) const
{
    windowBackground(g, area, origin, window);
}

void Painter::toolbar(juce::Graphics& g, juce::Component& surface) const
{
    const auto bounds = surface.getLocalBounds().toFloat().reduced(0.5f);
    const auto radius = SkinStyle::geometry().tileRadius;
    g.setColour(colour(pal().surface));
    g.fillRoundedRectangle(bounds, radius);
    if (!isNight())
    {
        g.setColour(colour(ext().hairline));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
    }
    const auto group = juce::Rectangle<int>::fromString(surface.getProperties()["skinTransport"].toString()).toFloat();
    if (!group.isEmpty())
    {
        const auto capsule = group.expanded(3.0f, 3.0f);
        g.setColour(colour(ext().sunken));
        g.fillRoundedRectangle(capsule, capsule.getHeight() * 0.5f);
        g.setColour(colour(ext().hairline));
        g.drawRoundedRectangle(capsule.reduced(0.5f), capsule.getHeight() * 0.5f - 0.5f, 1.0f);
    }
}

void Painter::statusBar(juce::Graphics&, juce::Rectangle<float>) const {}

void Painter::panel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
                    int icon, int trailingControls) const
{
    const auto radius = SkinStyle::geometry().tileRadius;
    const auto body = bounds.toFloat().reduced(0.5f);
    g.setColour(colour(pal().surface));
    g.fillRoundedRectangle(body, radius);
    const auto header = panelHeader(bounds);
    const auto box = iconBox(header);
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(box, 6.0f);
    paintPanelGlyph(g, box.withSizeKeepingCentre(12.0f, 12.0f), icon, colour(pal().secondaryText));
    paintPanelTitle(g, header, title, trailingControls);
}

void Painter::panelShadow(juce::Graphics&, juce::Rectangle<float>) const {}

void Painter::panelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live) const
{
    const auto radius = SkinStyle::geometry().tileRadius;
    if (!isNight())
    {
        g.setColour(colour(ext().hairline));
        g.drawRoundedRectangle(tile, radius, 1.0f);
    }
    if (live)
        paintLiveRing(g, tile, radius);
}

void Painter::dataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius) const
{
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(area, radius);
}

void Painter::waveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour signal) const
{
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(lane, juce::jmin(6.0f, lane.getHeight() * 0.25f));
    g.setColour(colour(ext().hairline));
    for (const float fraction : { 0.25f, 0.75f })
        g.drawHorizontalLine(juce::roundToInt(lane.getY() + lane.getHeight() * fraction), lane.getX(), lane.getRight());
    g.setColour(signal.withAlpha(0.32f));
    g.drawHorizontalLine(juce::roundToInt(lane.getCentreY()), lane.getX(), lane.getRight());
    setWaveformBrush(g, lane, signal);
}

void Painter::clock(juce::Graphics& g, juce::Rectangle<float> bounds,
                    const juce::String& main, const juce::String& frames) const
{
    const auto well = bounds.reduced(2.0f, 0.0f);
    const auto radius = SkinStyle::geometry().controlRadius;
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(well, radius);
    innerShadow(g, well, radius, { 0.0f, 1.0f }, 3.0f, juce::Colours::black.withAlpha(isNight() ? 0.35f : 0.06f));
    g.setColour(colour(ext().hairline));
    g.drawRoundedRectangle(well.reduced(0.5f), radius - 0.5f, 1.0f);
    paintClockText(g, well, main, frames);
}

void Painter::iconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down, bool& onAccent) const
{
    const auto bounds = button.getLocalBounds().toFloat();
    const auto kind = role(button);
    const auto enabled = button.isEnabled();
    if (kind == "transportPrimary")
    {
        // Painting is clipped to the 32 px key, so the disc leaves room for its glow.
        const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight()) - 3.0f;
        const auto circle = bounds.withSizeKeepingCentre(side, side).translated(0.0f, -0.5f);
        auto fill = colour(pal().accent);
        if (!enabled) fill = fill.withMultipliedAlpha(0.45f);
        else if (down) fill = fill.darker(0.12f);
        else if (hovered) fill = fill.brighter(0.08f);
        if (enabled && !down) softShadow(g, circle, side * 0.5f, { 0.0f, 1.0f }, 2.0f, fill.withAlpha(0.55f));
        g.setColour(fill);
        g.fillEllipse(circle);
        rimLight(g, circle, side * 0.5f, juce::Colours::white.withAlpha(0.30f));
        onAccent = true;
        return;
    }
    const auto transport = kind == "transport";
    const auto shape = transport ? bounds.withSizeKeepingCentre(juce::jmin(bounds.getWidth(), bounds.getHeight()),
                                                                juce::jmin(bounds.getWidth(), bounds.getHeight()))
                                 : bounds.reduced(1.0f);
    const auto radius = transport ? shape.getHeight() * 0.5f : SkinStyle::geometry().controlRadius;
    if (enabled && button.getToggleState())
    {
        g.setColour(colour(pal().accent).withAlpha(isNight() ? 0.22f : 0.13f));
        g.fillRoundedRectangle(shape, radius);
    }
    else if (enabled && (hovered || down))
    {
        g.setColour(colour(pal().control).withAlpha(down ? 1.0f : 0.85f));
        g.fillRoundedRectangle(shape, radius);
    }
}

bool Painter::buttonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                               juce::Colour& textColour) const
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    const auto radius = SkinStyle::geometry().controlRadius;
    const auto& properties = button.getProperties();
    const auto ghost = isGhostButton(button);
    const auto enabled = button.isEnabled();
    textColour = enabled ? colour(pal().text) : colour(pal().secondaryText).withAlpha(0.52f);
    if (ghost)
    {
        if (!enabled) return true;
        if (button.getToggleState() || hovered || down)
        {
            g.setColour(button.getToggleState() ? colour(pal().accent).withAlpha(isNight() ? 0.22f : 0.13f)
                                                : colour(pal().control).withAlpha(down ? 1.0f : 0.85f));
            g.fillRoundedRectangle(bounds, radius);
        }
        return true;
    }

    const auto segment = isSegmentButton(button);
    const auto toggled = button.getToggleState();
    const auto accent = static_cast<bool>(properties.getWithDefault("accent", false));
    const auto top = colour(pal().surface);
    const auto bottom = colour(pal().control);
    if (segment)
    {
        g.setColour(colour(ext().sunken));
        g.fillPath(segmentPath(bounds, radius, button.getConnectedEdgeFlags()));
        if (toggled)
        {
            const auto pill = bounds.reduced(2.0f);
            softShadow(g, pill, radius - 2.0f, { 0.0f, 1.0f }, 2.0f, colour(ext().shadowDark));
            g.setGradientFill(juce::ColourGradient(top, 0.0f, pill.getY(), top.interpolatedWith(bottom, 0.35f),
                                                   0.0f, pill.getBottom(), false));
            g.fillRoundedRectangle(pill, radius - 2.0f);
            g.setColour(colour(ext().hairline));
            g.drawRoundedRectangle(pill, radius - 2.0f, 1.0f);
        }
        textColour = !enabled ? colour(pal().secondaryText).withAlpha(0.52f)
                   : toggled ? colour(pal().text) : colour(pal().secondaryText);
        return true;
    }
    if (accent && enabled)
    {
        g.setColour(down ? colour(pal().text).withMultipliedAlpha(0.85f) : colour(pal().text));
        g.fillRoundedRectangle(bounds, radius);
        textColour = colour(pal().surface);
        return true;
    }
    if (!enabled)
    {
        g.setColour(colour(pal().control).withMultipliedAlpha(0.6f));
        g.fillRoundedRectangle(bounds, radius);
        return true;
    }
    if (toggled)
    {
        g.setColour(colour(pal().accent).withAlpha(isNight() ? 0.24f : 0.14f));
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(colour(pal().accent).withAlpha(0.45f));
        g.drawRoundedRectangle(bounds, radius, 1.0f);
        textColour = isNight() ? colour(pal().text) : colour(pal().accent).darker(0.35f);
        return true;
    }
    if (!down) softShadow(g, bounds, radius, { 0.0f, 1.0f }, 2.0f, colour(ext().shadowDark));
    const auto lift = hovered && !down ? 0.04f : 0.0f;
    g.setGradientFill(juce::ColourGradient(down ? bottom : top.brighter(lift), 0.0f, bounds.getY(),
                                           down ? bottom : top.interpolatedWith(bottom, 0.6f).brighter(lift),
                                           0.0f, bounds.getBottom(), false));
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(isNight() ? colour(ext().glassRim) : colour(ext().hairline).withMultipliedAlpha(1.6f));
    g.drawRoundedRectangle(bounds, radius, 1.0f);
    return true;
}

void Painter::switchTrack(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled, bool focused) const
{
    const auto alpha = enabled ? 1.0f : 0.4f;
    const auto radius = track.getHeight() * 0.5f;
    g.setColour((on ? colour(pal().accent) : colour(pal().control)).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(track, radius);
    if (!on)
    {
        g.setColour(colour(ext().hairline));
        g.drawRoundedRectangle(track.reduced(0.5f), radius - 0.5f, 1.0f);
    }
    const auto thumb = juce::Rectangle<float>(track.getX() + (on ? track.getWidth() - track.getHeight() + 2.0f : 2.0f),
                                              track.getY() + 2.0f, track.getHeight() - 4.0f, track.getHeight() - 4.0f);
    softShadow(g, thumb, thumb.getHeight() * 0.5f, { 0.0f, 0.8f }, 2.0f, juce::Colours::black.withAlpha(0.22f * alpha));
    g.setColour(juce::Colours::white.withAlpha(alpha));
    g.fillEllipse(thumb);
    if (focused)
    {
        g.setColour(colour(pal().accent).withAlpha(0.7f));
        g.drawRoundedRectangle(track.expanded(2.0f), radius + 2.0f, 1.0f);
    }
}

void Painter::comboBox(juce::Graphics& g, juce::ComboBox& box, bool down) const
{
    const auto bounds = box.getLocalBounds().toFloat().reduced(0.5f);
    const auto radius = SkinStyle::geometry().controlRadius;
    const auto top = colour(pal().surface);
    const auto bottom = colour(pal().control);
    if (!box.isEnabled())
    {
        g.setColour(bottom.withMultipliedAlpha(0.6f));
        g.fillRoundedRectangle(bounds, radius);
        return;
    }
    if (!down) softShadow(g, bounds, radius, { 0.0f, 1.0f }, 2.0f, colour(ext().shadowDark));
    g.setGradientFill(juce::ColourGradient(down ? bottom : top, 0.0f, bounds.getY(),
                                           down ? bottom : top.interpolatedWith(bottom, 0.6f), 0.0f, bounds.getBottom(), false));
    g.fillRoundedRectangle(bounds, radius);
    g.setColour(isNight() ? colour(ext().glassRim) : colour(ext().hairline).withMultipliedAlpha(1.6f));
    g.drawRoundedRectangle(bounds, radius, 1.0f);
}

void Painter::linearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position, juce::Slider& slider) const
{
    const auto alpha = slider.isEnabled() ? 1.0f : 0.35f;
    const auto centre = track.getCentreY();
    const auto left = track.getX(), right = track.getRight();
    const auto pos = juce::jlimit(left, right, position);
    const auto line = juce::Rectangle<float>(left, centre - 2.0f, right - left, 4.0f);
    g.setColour(colour(pal().control).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(line, 2.0f);
    g.setColour(colour(ext().hairline).withMultipliedAlpha(alpha));
    g.drawRoundedRectangle(line, 2.0f, 1.0f);
    g.setColour(colour(pal().secondaryText).withMultipliedAlpha(alpha));
    g.fillRoundedRectangle(line.withRight(pos), 2.0f);
    const auto thumb = juce::Rectangle<float>(pos - 7.0f, centre - 7.0f, 14.0f, 14.0f);
    softShadow(g, thumb, 7.0f, { 0.0f, 1.0f }, 3.0f, juce::Colours::black.withAlpha(0.25f * alpha));
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(alpha), 0.0f, thumb.getY(),
                                           juce::Colour(0xffe9ecf0).withAlpha(alpha), 0.0f, thumb.getBottom(), false));
    g.fillEllipse(thumb);
    g.setColour(juce::Colours::black.withAlpha(0.14f * alpha));
    g.drawEllipse(thumb.reduced(0.5f), 1.0f);
    if (slider.hasKeyboardFocus(true))
    {
        g.setColour(colour(pal().accent).withAlpha(0.6f));
        g.drawEllipse(thumb.expanded(2.5f), 1.2f);
    }
}

void Painter::popupBackground(juce::Graphics& g, juce::Rectangle<float> bounds) const
{
    const auto body = bounds.reduced(0.5f);
    g.setColour(colour(pal().surface));
    g.fillRoundedRectangle(body, 12.0f);
    g.setColour(isNight() ? colour(ext().glassRim) : colour(pal().border));
    g.drawRoundedRectangle(body, 12.0f, 1.0f);
}

void Painter::popupHighlight(juce::Graphics& g, juce::Rectangle<float> row) const
{
    // Neutral, but strong enough to follow the keyboard through a menu in both themes.
    g.setColour(colour(pal().text).withAlpha(isNight() ? 0.11f : 0.075f));
    g.fillRoundedRectangle(row, 6.0f);
}

void Painter::knob(juce::Graphics& g, juce::Rectangle<float> dial, float angle, juce::Colour indicator,
                   bool enabled, bool focused) const
{
    const auto radius = dial.getWidth() * 0.5f;
    const auto centre = dial.getCentre();
    softShadow(g, dial, radius, { 0.0f, 1.5f }, 3.0f, colour(ext().shadowDark).withMultipliedAlpha(enabled ? 1.0f : 0.4f));
    const auto top = colour(pal().surface);
    const auto bottom = colour(pal().control);
    g.setGradientFill(juce::ColourGradient(top.brighter(0.04f), centre.x, dial.getY(),
                                           bottom.interpolatedWith(colour(pal().border), 0.35f), centre.x, dial.getBottom(), false));
    g.fillEllipse(dial);
    g.setColour(isNight() ? colour(ext().glassRim) : colour(pal().border));
    g.drawEllipse(dial.reduced(0.5f), 1.0f);
    const juce::Point<float> direction(std::sin(angle), -std::cos(angle));
    // The band colour identifies the band; it matches the numbered curve badges.
    g.setColour(enabled ? indicator : colour(pal().secondaryText).withAlpha(0.45f));
    g.drawLine({ centre + direction * radius * 0.30f, centre + direction * radius * 0.74f },
               juce::jmax(1.6f, dial.getWidth() * 0.06f));
    if (focused)
    {
        g.setColour(colour(pal().accent).withAlpha(0.8f));
        g.drawEllipse(dial.expanded(1.5f), 1.2f);
    }
}

void Painter::meterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail, bool loudness) const
{
    const auto radius = juce::jmin(3.0f, bar.getWidth() * 0.5f);
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(bar, radius);
    paintMeterFill(g, bar, radius, level, trail, loudness);
}

void Painter::meterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held) const
{
    const auto track = bounds.withSizeKeepingCentre(bounds.getWidth() - 6.0f, 6.0f);
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(track, 3.0f);
    g.setColour(colour(ext().hairline));
    g.drawRoundedRectangle(track, 3.0f, 1.0f);
    const auto width = track.getWidth() * juce::jlimit(0.0f, 1.0f, level);
    if (width > 0.5f)
    {
        juce::ColourGradient gradient(colour(ext().signal), track.getX(), 0.0f, colour(ext().live), track.getRight(), 0.0f, false);
        gradient.addColour(0.72, colour(ext().signal));
        g.setGradientFill(gradient);
        g.fillRoundedRectangle(track.withWidth(width), 3.0f);
    }
    if (held > 0.001f)
    {
        g.setColour(colour(pal().text).withAlpha(0.55f));
        g.fillRect(track.getX() + track.getWidth() * juce::jlimit(0.0f, 1.0f, held) - 0.75f, track.getY() - 2.0f, 1.5f,
                   track.getHeight() + 4.0f);
    }
}
} // namespace skins
} // namespace singleeditor
