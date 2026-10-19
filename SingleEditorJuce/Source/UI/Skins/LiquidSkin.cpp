#include "UI/Skins/SkinPainters.h"

#include <cmath>

namespace singleeditor::skins
{
namespace
{
// C  Liquid Glass: an aurora backdrop under translucent tiles with rim light.
constexpr ModernGlassTokens::Palette lightPalette {
    0xffeef1f8, 0xfff7f8fc, 0xffffffff, 0xff1a2238, 0xff56607a, 0xffd5dae6,
    0xfff25355, 0xfffbd9dd, 0xff1a2238,
    { 0xff1f6f8b, 0xff2a7d99, 0xff17617a, 0xff3489a5, 0xff11566d, 0xff3f95b0 }
};
constexpr ModernGlassTokens::Palette nightPalette {
    0xff0b1020, 0xff1c2336, 0xff252d42, 0xffffffff, 0xffb5bdd0, 0xff39425a,
    0xffff6b6e, 0xff4a2a3a, 0xffffffff,
    { 0xffa6eef3, 0xffb5f1f5, 0xff95e6ec, 0xffc2f4f7, 0xff86dde4, 0xffcff7f9 }
};
constexpr SkinExtras lightExtras {
    0x0f1a2238, 0x241a2238, 0xff1f6f8b, 0xffd98a1e, 0xffd92d20, 0x8cffffff, 0xe6ffffff, 0x00000000, 0x261a2238
};
constexpr SkinExtras nightExtras {
    0x47000000, 0x2effffff, 0xffa6eef3, 0xffffc15a, 0xffff6b5b, 0x1cffffff, 0x75ffffff, 0x00000000, 0x59000000
};

juce::Colour white(float alpha) { return juce::Colours::white.withAlpha(alpha); }

// Glass body: tint a little denser at the top, then a specular wash.
void glass(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float strength = 1.0f)
{
    const auto tint = colour(ext().glassTint).withMultipliedAlpha(strength);
    g.setGradientFill(juce::ColourGradient(tint, 0.0f, shape.getY(),
                                           tint.withMultipliedAlpha(0.82f), 0.0f, shape.getBottom(), false));
    g.fillRoundedRectangle(shape, radius);
    const auto sheen = white((isNight() ? 0.07f : 0.30f) * juce::jmin(1.0f, strength));
    g.setGradientFill(juce::ColourGradient(sheen, 0.0f, shape.getY(), sheen.withAlpha(0.0f), 0.0f,
                                           shape.getY() + juce::jmin(shape.getHeight() * 0.5f, 60.0f), false));
    g.fillRoundedRectangle(shape, radius);
}

// The edge of a sheet: a hairline all round and light caught on the top rim.
void edge(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float strength = 1.0f)
{
    g.setColour(colour(ext().hairline).withMultipliedAlpha(strength));
    g.drawRoundedRectangle(shape.reduced(0.5f), juce::jmax(0.0f, radius - 0.5f), 1.0f);
    rimLight(g, shape, radius, colour(ext().glassRim).withMultipliedAlpha(strength));
}

// Kept outside the shape, so a shadow never shows through its own glass.
void dropShadow(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float offsetY, float blur,
                float amount = 1.0f)
{
    const juce::Graphics::ScopedSaveState saved(g);
    juce::Path outside;
    outside.addRectangle(shape.expanded(blur + offsetY + 2.0f));
    outside.addRoundedRectangle(shape, radius);
    outside.setUsingNonZeroWinding(false);
    g.reduceClipRegion(outside);
    softShadow(g, shape, radius, { 0.0f, offsetY }, blur, colour(ext().shadowDark).withMultipliedAlpha(amount));
}

// A channel cut into the glass: darker tint, shade under the top lip, light on the lower lip.
void sunken(juce::Graphics& g, juce::Rectangle<float> shape, float radius)
{
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(shape, radius);
    innerShadow(g, shape, radius, { 0.0f, 1.0f }, 4.0f, juce::Colours::black.withAlpha(isNight() ? 0.28f : 0.07f));
    const auto lip = white(isNight() ? 0.12f : 0.60f);
    g.setGradientFill(juce::ColourGradient(lip.withAlpha(0.0f), 0.0f, shape.getBottom() - juce::jmin(14.0f, shape.getHeight()),
                                           lip, 0.0f, shape.getBottom(), false));
    g.drawRoundedRectangle(shape.reduced(0.5f), juce::jmax(0.0f, radius - 0.5f), 1.0f);
}

juce::Colour accentTint() { return colour(pal().accent).withAlpha(isNight() ? 0.30f : 0.16f); }
juce::Colour accentInk() { return isNight() ? colour(pal().accent).brighter(0.25f) : colour(pal().accent).darker(0.30f); }

// Coral drop of liquid: lit from the top left, glossy cap, soft glow.
void gem(juce::Graphics& g, juce::Rectangle<float> shape, float radius, juce::Colour fill, bool glow)
{
    if (glow) softShadow(g, shape, radius, { 0.0f, 1.0f }, 2.5f, fill.withAlpha(0.55f));
    g.setGradientFill(juce::ColourGradient(fill.brighter(0.22f), shape.getX(), shape.getY(),
                                           fill.darker(0.06f), shape.getRight(), shape.getBottom(), false));
    g.fillRoundedRectangle(shape, radius);
    const auto cap = shape.reduced(shape.getWidth() * 0.12f, 1.5f).withHeight(shape.getHeight() * 0.48f);
    g.setGradientFill(juce::ColourGradient(white(0.38f), 0.0f, cap.getY(), white(0.0f), 0.0f, cap.getBottom(), false));
    g.fillRoundedRectangle(cap, juce::jmin(radius, cap.getHeight() * 0.5f));
    rimLight(g, shape, radius, white(0.45f));
}

void focusRing(juce::Graphics& g, juce::Rectangle<float> shape, float radius)
{
    g.setColour(colour(pal().accent).withAlpha(0.70f));
    g.drawRoundedRectangle(shape.expanded(1.5f), radius + 1.5f, 1.2f);
}

struct LiquidPainter final : Painter
{
    void windowBackground(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> origin,
                          juce::Rectangle<float> window) const override
    {
        const auto night = isNight();
        g.setGradientFill(juce::ColourGradient(colour(night ? 0xff0b1020 : 0xfff4f6fb), 0.0f, window.getY() - origin.y,
                                               colour(night ? 0xff121a38 : 0xffe4e9f5), 0.0f,
                                               window.getBottom() - origin.y, false));
        g.fillRect(area);
        // Colour fields are placed in window coordinates so the caption and the
        // editor share one continuous backdrop.
        struct Field { float x, y, radius; std::uint32_t day, dark; };
        static constexpr Field fields[] {
            { 0.08f, 0.04f, 0.42f, 0x4df25355, 0x57ff5e61 },
            { 0.90f, 0.16f, 0.38f, 0x4a2bb3c0, 0x5227c3cf },
            { 0.55f, 1.02f, 0.55f, 0x427b61ff, 0x667b61ff },
            { 0.00f, 0.95f, 0.30f, 0x2e2bb3c0, 0x3827c3cf },
            { 0.62f, 0.42f, 0.34f, 0x1f5b7cff, 0x1f5b7cff },
        };
        const auto reach = juce::jmax(window.getWidth(), window.getHeight());
        for (const auto& field : fields)
        {
            const juce::Point<float> centre(window.getX() + window.getWidth() * field.x - origin.x,
                                            window.getY() + window.getHeight() * field.y - origin.y);
            const auto tint = colour(night ? field.dark : field.day);
            juce::ColourGradient wash(tint, centre, tint.withAlpha(0.0f), centre.translated(reach * field.radius, 0.0f), true);
            wash.addColour(0.45, tint.withMultipliedAlpha(0.45f));
            g.setGradientFill(wash);
            g.fillRect(area);
        }
    }

    void toolbar(juce::Graphics& g, juce::Component& surface) const override
    {
        const auto bounds = surface.getLocalBounds().toFloat();
        const auto radius = SkinStyle::geometry().tileRadius;
        glass(g, bounds, radius);
        edge(g, bounds, radius);
        const auto group = juce::Rectangle<int>::fromString(surface.getProperties()["skinTransport"].toString()).toFloat();
        if (!group.isEmpty())
        {
            const auto tray = group.expanded(4.0f, 3.0f);
            sunken(g, tray, tray.getHeight() * 0.5f);
        }
    }

    void panel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
               int icon, int trailingControls) const override
    {
        glass(g, bounds.toFloat(), SkinStyle::geometry().tileRadius);
        const auto header = panelHeader(bounds);
        const auto box = iconBox(header);
        g.setColour(white(isNight() ? 0.10f : 0.55f));
        g.fillEllipse(box);
        g.setColour(colour(ext().hairline));
        g.drawEllipse(box.reduced(0.5f), 1.0f);
        paintPanelGlyph(g, box.withSizeKeepingCentre(12.0f, 12.0f), icon, colour(pal().secondaryText));
        paintPanelTitle(g, header, title, trailingControls);
    }

    // Reaches no further than the 10 px gap, so it never tints a neighbour's glass.
    void panelShadow(juce::Graphics& g, juce::Rectangle<float> tile) const override
    {
        dropShadow(g, tile, SkinStyle::geometry().tileRadius, 3.0f, 7.0f);
    }

    void panelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live) const override
    {
        const auto radius = SkinStyle::geometry().tileRadius;
        edge(g, tile, radius);
        if (live)
            paintLiveRing(g, tile, radius);
    }

    void dataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius) const override
    {
        sunken(g, area, radius);
    }

    void waveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour signal) const override
    {
        const auto radius = juce::jmin(10.0f, lane.getHeight() * 0.25f);
        sunken(g, lane, radius);
        const auto left = lane.getX() + radius, right = lane.getRight() - radius;
        g.setColour(colour(ext().hairline));
        for (const float fraction : { 0.25f, 0.75f })
            g.drawHorizontalLine(juce::roundToInt(lane.getY() + lane.getHeight() * fraction), left, right);
        g.setColour(signal.withAlpha(0.34f));
        g.drawHorizontalLine(juce::roundToInt(lane.getCentreY()), left, right);
        setWaveformBrush(g, lane, signal);
    }

    void clock(juce::Graphics& g, juce::Rectangle<float> bounds,
               const juce::String& main, const juce::String& frames) const override
    {
        const auto display = bounds.reduced(2.0f, 1.0f);
        sunken(g, display, display.getHeight() * 0.5f);
        paintClockText(g, display, main, frames);
    }

    void iconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down, bool& onAccent) const override
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const auto enabled = button.isEnabled();
        const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight());
        const auto kind = role(button);
        if (kind == "transportPrimary")
        {
            const auto disc = bounds.withSizeKeepingCentre(side - 4.0f, side - 4.0f);
            auto fill = colour(pal().accent);
            if (!enabled) fill = fill.withMultipliedAlpha(0.45f);
            else if (down) fill = fill.darker(0.12f);
            else if (hovered) fill = fill.brighter(0.06f);
            gem(g, disc, disc.getHeight() * 0.5f, fill, enabled && !down);
            onAccent = true;
            return;
        }
        if (!enabled) return;
        const auto transport = kind == "transport";
        const auto shape = transport ? bounds.withSizeKeepingCentre(side - 2.0f, side - 2.0f) : bounds.reduced(1.0f);
        const auto radius = shape.getHeight() * 0.5f;
        if (button.getToggleState())
        {
            g.setColour(accentTint());
            g.fillRoundedRectangle(shape, radius);
            g.setColour(colour(pal().accent).withAlpha(0.45f));
            g.drawRoundedRectangle(shape.reduced(0.5f), radius - 0.5f, 1.0f);
        }
        else if (down)
            sunken(g, shape, radius);
        else if (hovered || !transport)
        {
            glass(g, shape, radius, hovered ? 1.2f : 0.9f);
            edge(g, shape, radius);
        }
    }

    bool buttonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                          juce::Colour& textColour) const override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto radius = juce::jmin(SkinStyle::geometry().controlRadius, bounds.getHeight() * 0.5f);
        const auto enabled = button.isEnabled();
        const auto toggled = button.getToggleState();
        textColour = enabled ? colour(pal().text) : colour(pal().secondaryText).withAlpha(0.5f);
        if (isGhostButton(button))
        {
            if (!enabled) return true;
            if (toggled)
            {
                g.setColour(accentTint());
                g.fillRoundedRectangle(bounds, radius);
            }
            else if (down) sunken(g, bounds, radius);
            else if (hovered)
            {
                glass(g, bounds, radius);
                edge(g, bounds, radius);
            }
            return true;
        }
        if (isSegmentButton(button))
        {
            const auto track = segmentPath(bounds, radius, button.getConnectedEdgeFlags());
            g.setColour(colour(ext().sunken));
            g.fillPath(track);
            if (toggled)
            {
                const auto pill = bounds.reduced(2.5f);
                const auto pillRadius = juce::jmax(2.0f, radius - 2.5f);
                softShadow(g, pill, pillRadius, { 0.0f, 1.0f }, 2.0f, colour(ext().shadowDark));
                g.setColour(white(isNight() ? 0.18f : 0.92f));
                g.fillRoundedRectangle(pill, pillRadius);
                edge(g, pill, pillRadius);
            }
            textColour = !enabled ? colour(pal().secondaryText).withAlpha(0.5f)
                       : toggled ? colour(pal().text) : colour(pal().secondaryText);
            return true;
        }
        if (!enabled)
        {
            glass(g, bounds, radius, 0.45f);
            edge(g, bounds, radius, 0.5f);
            return true;
        }
        if (static_cast<bool>(button.getProperties().getWithDefault("accent", false)))
        {
            gem(g, bounds, radius, down ? colour(pal().accent).darker(0.12f) : colour(pal().accent), false);
            textColour = juce::Colours::white;
            return true;
        }
        if (toggled)
        {
            g.setColour(accentTint());
            g.fillRoundedRectangle(bounds, radius);
            g.setColour(colour(pal().accent).withAlpha(0.50f));
            g.drawRoundedRectangle(bounds, radius, 1.0f);
            textColour = accentInk();
            return true;
        }
        if (down)
        {
            sunken(g, bounds, radius);
            return true;
        }
        glass(g, bounds, radius, hovered ? 1.25f : 1.0f);
        edge(g, bounds, radius);
        return true;
    }

    void switchTrack(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled, bool focused) const override
    {
        const auto radius = track.getHeight() * 0.5f;
        const auto alpha = enabled ? 1.0f : 0.5f;
        if (on)
            gem(g, track, radius, colour(pal().accent).withMultipliedAlpha(alpha), false);
        else
            sunken(g, track, radius);
        const auto thumb = juce::Rectangle<float>(track.getX() + (on ? track.getWidth() - track.getHeight() + 2.0f : 2.0f),
                                                  track.getY() + 2.0f, track.getHeight() - 4.0f, track.getHeight() - 4.0f);
        softShadow(g, thumb, thumb.getHeight() * 0.5f, { 0.0f, 0.8f }, 1.5f, juce::Colours::black.withAlpha(0.25f * alpha));
        g.setColour(white(alpha));
        g.fillEllipse(thumb);
        if (focused) focusRing(g, track, radius);
    }

    void comboBox(juce::Graphics& g, juce::ComboBox& box, bool down) const override
    {
        const auto bounds = box.getLocalBounds().toFloat().reduced(0.5f);
        const auto radius = juce::jmin(SkinStyle::geometry().controlRadius, bounds.getHeight() * 0.5f);
        if (down)
        {
            sunken(g, bounds, radius);
            return;
        }
        glass(g, bounds, radius, box.isEnabled() ? 1.0f : 0.45f);
        edge(g, bounds, radius, box.isEnabled() ? 1.0f : 0.5f);
    }

    void linearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position, juce::Slider& slider) const override
    {
        const auto alpha = slider.isEnabled() ? 1.0f : 0.4f;
        const auto pos = juce::jlimit(track.getX(), track.getRight(), position);
        const auto groove = juce::Rectangle<float>(track.getX(), track.getCentreY() - 2.5f, track.getWidth(), 5.0f);
        sunken(g, groove, 2.5f);
        const auto signal = colour(ext().signal).withMultipliedAlpha(alpha);
        g.setGradientFill(juce::ColourGradient(signal.withMultipliedAlpha(0.65f), groove.getX(), 0.0f, signal, pos, 0.0f, false));
        g.fillRoundedRectangle(groove.withRight(pos), 2.5f);
        const auto thumb = juce::Rectangle<float>(pos - 8.0f, track.getCentreY() - 8.0f, 16.0f, 16.0f);
        softShadow(g, thumb, 8.0f, { 0.0f, 1.0f }, 2.5f, juce::Colours::black.withAlpha(0.22f * alpha));
        g.setColour(white(alpha));
        g.fillEllipse(thumb);
        g.setColour(colour(ext().hairline).withMultipliedAlpha(alpha));
        g.drawEllipse(thumb.reduced(0.5f), 1.0f);
        if (slider.hasKeyboardFocus(true)) focusRing(g, thumb, 8.0f);
    }

    void popupBackground(juce::Graphics& g, juce::Rectangle<float> bounds) const override
    {
        const auto body = bounds.reduced(0.5f);
        g.setColour(colour(pal().surface).withAlpha(isNight() ? 0.97f : 0.96f));
        g.fillRoundedRectangle(body, 14.0f);
        const auto sheen = white(isNight() ? 0.05f : 0.40f);
        g.setGradientFill(juce::ColourGradient(sheen, 0.0f, body.getY(), sheen.withAlpha(0.0f), 0.0f, body.getY() + 40.0f, false));
        g.fillRoundedRectangle(body, 14.0f);
        edge(g, body, 14.0f);
    }

    void popupHighlight(juce::Graphics& g, juce::Rectangle<float> row) const override
    {
        g.setColour(accentTint());
        g.fillRoundedRectangle(row, 8.0f);
    }

    void knob(juce::Graphics& g, juce::Rectangle<float> dial, float angle, juce::Colour indicator,
              bool enabled, bool focused) const override
    {
        const auto body = dial.reduced(1.0f);
        const auto radius = body.getWidth() * 0.5f;
        if (enabled) softShadow(g, body, radius, { 0.0f, 1.0f }, 2.0f, colour(ext().shadowDark));
        const auto top = isNight() ? white(0.20f) : white(0.98f);
        const auto bottom = isNight() ? white(0.05f) : colour(pal().border).withAlpha(0.85f);
        g.setGradientFill(juce::ColourGradient(top, body.getX(), body.getY(), bottom, body.getRight(), body.getBottom(), false));
        g.fillEllipse(body);
        edge(g, body, radius);
        const juce::Point<float> direction(std::sin(angle), -std::cos(angle));
        const auto centre = body.getCentre();
        g.setColour(enabled ? indicator : colour(pal().secondaryText).withAlpha(0.45f));
        g.drawLine({ centre + direction * radius * 0.30f, centre + direction * radius * 0.72f },
                   juce::jmax(2.0f, body.getWidth() * 0.07f));
        if (focused) focusRing(g, body, radius);
    }

    void meterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail, bool loudness) const override
    {
        const auto radius = bar.getWidth() * 0.5f;
        sunken(g, bar, radius);
        paintMeterFill(g, bar.reduced(1.0f), radius - 1.0f, level, trail, loudness);
        // Gloss down the left half of the column.
        g.setGradientFill(juce::ColourGradient(white(isNight() ? 0.10f : 0.28f), bar.getX(), 0.0f,
                                               white(0.0f), bar.getCentreX(), 0.0f, false));
        g.fillRoundedRectangle(bar.reduced(1.0f), juce::jmax(0.0f, radius - 1.0f));
    }

    void meterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held) const override
    {
        const auto groove = bounds.withSizeKeepingCentre(bounds.getWidth() - 6.0f, 8.0f);
        sunken(g, groove, 4.0f);
        const auto width = groove.getWidth() * juce::jlimit(0.0f, 1.0f, level);
        if (width > 0.5f)
        {
            juce::ColourGradient gradient(colour(ext().signal), groove.getX(), 0.0f, colour(ext().live), groove.getRight(), 0.0f, false);
            gradient.addColour(0.72, colour(ext().signal));
            g.setGradientFill(gradient);
            g.fillRoundedRectangle(groove.withWidth(width), 4.0f);
        }
        if (held > 0.001f)
        {
            g.setColour(colour(pal().text).withAlpha(0.55f));
            g.fillRect(groove.getX() + groove.getWidth() * juce::jlimit(0.0f, 1.0f, held) - 0.75f, groove.getY() - 2.0f,
                       1.5f, groove.getHeight() + 4.0f);
        }
    }
};
} // namespace

const Definition& liquidSkin()
{
    static const LiquidPainter painter;
    static const Definition skin { lightPalette, &nightPalette, lightExtras, &nightExtras, { 16.0f, 12.0f, 10, 12 }, painter };
    return skin;
}
} // namespace singleeditor::skins
