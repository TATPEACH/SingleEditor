#include "UI/Skins/SkinPainters.h"

#include <cmath>

namespace singleeditor::skins
{
namespace
{
// A  Soft: one material colour. Depth comes only from paired light and dark
// shadows: raised for things you press, pressed in for wells that hold data.
constexpr ModernGlassTokens::Palette lightPalette {
    0xffe2e6eb, 0xffe2e6eb, 0xffd8dde4, 0xff3a4351, 0xff6b7585, 0xffccd2da,
    0xfff25355, 0xfff6d8d9, 0xff3a4351,
    { 0xff6a87a8, 0xff7591b0, 0xff5f7c9c, 0xff809bb8, 0xff56728f, 0xff8aa4c0 }
};
constexpr SkinExtras extras {
    0xffdde2e8, 0x14505f78, 0xff5f7fa3, 0xffd98a1e, 0xffd92d20, 0x00000000, 0xf2ffffff, 0xf2ffffff, 0x9e96a4ba
};

juce::Colour material() { return colour(pal().background); }
juce::Colour highlight(float amount = 1.0f) { return colour(ext().shadowLight).withMultipliedAlpha(amount); }
juce::Colour shade(float amount = 1.0f) { return colour(ext().shadowDark).withMultipliedAlpha(amount); }

// Light from the top left, contact shadow to the bottom right.
void raise(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float distance, float blur,
           float amount = 1.0f)
{
    softShadow(g, shape, radius, { -distance, -distance }, blur, highlight(amount));
    softShadow(g, shape, radius, { distance, distance }, blur, shade(amount));
}

// Pressed into the surface: shade under the top-left rim, light on the lower right.
void press(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float depth, float amount = 1.0f)
{
    innerShadow(g, shape, radius, { 1.0f, 1.0f }, depth, shade(0.75f * amount));
    innerShadow(g, shape, radius, { -1.0f, -1.0f }, depth * 0.8f, highlight(0.9f * amount));
}

// Connected segments shade only top and bottom, so joints stay seamless.
void pressTrack(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float depth)
{
    innerShadow(g, shape, radius, { 0.0f, 1.0f }, depth, shade(0.75f));
    innerShadow(g, shape, radius, { 0.0f, -1.0f }, depth * 0.8f, highlight(0.9f));
}

// A convex face is a touch lighter towards the light.
void face(juce::Graphics& g, juce::Rectangle<float> shape, float radius, juce::Colour fill)
{
    g.setGradientFill(juce::ColourGradient(fill.brighter(0.035f), shape.getX(), shape.getY(),
                                           fill.darker(0.025f), shape.getRight(), shape.getBottom(), false));
    g.fillRoundedRectangle(shape, radius);
}

void well(juce::Graphics& g, juce::Rectangle<float> shape, float radius, float depth)
{
    g.setColour(colour(ext().sunken));
    g.fillRoundedRectangle(shape, radius);
    press(g, shape, radius, depth);
}

void focusRing(juce::Graphics& g, juce::Rectangle<float> shape, float radius)
{
    g.setColour(colour(pal().accent).withAlpha(0.65f));
    g.drawRoundedRectangle(shape.expanded(1.5f), radius + 1.5f, 1.2f);
}

struct SoftPainter final : Painter
{
    void toolbar(juce::Graphics& g, juce::Component& surface) const override
    {
        const auto bounds = surface.getLocalBounds().toFloat();
        face(g, bounds, SkinStyle::geometry().tileRadius, material());
        const auto group = juce::Rectangle<int>::fromString(surface.getProperties()["skinTransport"].toString()).toFloat();
        if (!group.isEmpty())
        {
            // A pressed tray that holds the raised transport keys.
            const auto tray = group.expanded(5.0f, 3.0f);
            well(g, tray, tray.getHeight() * 0.5f, 5.0f);
        }
    }

    void panel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
               int icon, int trailingControls) const override
    {
        face(g, bounds.toFloat(), SkinStyle::geometry().tileRadius, material());
        const auto header = panelHeader(bounds);
        const auto box = iconBox(header);
        well(g, box, 7.0f, 3.0f);
        paintPanelGlyph(g, box.withSizeKeepingCentre(12.0f, 12.0f), icon, colour(pal().secondaryText));
        paintPanelTitle(g, header, title, trailingControls);
    }

    // Shadows stay inside the 16 px gap, so a neighbour never darkens a tile.
    void panelShadow(juce::Graphics& g, juce::Rectangle<float> tile) const override
    {
        raise(g, tile, SkinStyle::geometry().tileRadius, 5.0f, 9.0f);
    }

    void panelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live) const override
    {
        if (live)
            paintLiveRing(g, tile, SkinStyle::geometry().tileRadius);
    }

    void dataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius) const override
    {
        well(g, area, radius, 6.0f);
    }

    void waveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour signal) const override
    {
        const auto radius = juce::jmin(10.0f, lane.getHeight() * 0.25f);
        well(g, lane, radius, 7.0f);
        const auto left = lane.getX() + radius, right = lane.getRight() - radius;
        g.setColour(colour(ext().hairline));
        for (const float fraction : { 0.25f, 0.75f })
            g.drawHorizontalLine(juce::roundToInt(lane.getY() + lane.getHeight() * fraction), left, right);
        g.setColour(signal.withAlpha(0.30f));
        g.drawHorizontalLine(juce::roundToInt(lane.getCentreY()), left, right);
        setWaveformBrush(g, lane, signal);
    }

    void clock(juce::Graphics& g, juce::Rectangle<float> bounds,
               const juce::String& main, const juce::String& frames) const override
    {
        const auto display = bounds.reduced(3.0f, 1.0f);
        well(g, display, display.getHeight() * 0.5f, 5.0f);
        paintClockText(g, display, main, frames);
    }

    void iconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down, bool& onAccent) const override
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const auto enabled = button.isEnabled();
        const auto side = juce::jmin(bounds.getWidth(), bounds.getHeight()) - 6.0f;
        const auto kind = role(button);
        if (kind == "transportPrimary")
        {
            const auto disc = bounds.withSizeKeepingCentre(side, side);
            auto fill = colour(pal().accent);
            if (!enabled) fill = fill.withMultipliedAlpha(0.45f);
            else if (hovered && !down) fill = fill.brighter(0.06f);
            if (enabled && !down) raise(g, disc, side * 0.5f, 1.25f, 2.0f);
            face(g, disc, side * 0.5f, fill);
            if (down) press(g, disc, side * 0.5f, 3.0f, 0.7f);
            onAccent = true;
            return;
        }
        if (!enabled) return;
        const auto transport = kind == "transport";
        const auto shape = transport ? bounds.withSizeKeepingCentre(side, side) : bounds.reduced(3.0f);
        const auto radius = transport ? side * 0.5f : SkinStyle::geometry().controlRadius - 2.0f;
        if (down || button.getToggleState())
        {
            well(g, shape, radius, 3.0f);
            return;
        }
        raise(g, shape, radius, 1.25f, 2.0f, hovered ? 1.0f : 0.85f);
        face(g, shape, radius, hovered ? material().brighter(0.02f) : material());
    }

    bool buttonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                          juce::Colour& textColour) const override
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const auto radius = juce::jmin(SkinStyle::geometry().controlRadius, bounds.getHeight() * 0.5f);
        // Raised bodies leave room for their shadows inside the button.
        const auto body = bounds.reduced(2.5f);
        const auto bodyRadius = juce::jmax(2.0f, radius - 2.5f);
        const auto enabled = button.isEnabled();
        const auto toggled = button.getToggleState();
        textColour = enabled ? colour(pal().text) : colour(pal().secondaryText).withAlpha(0.5f);
        if (isGhostButton(button))
        {
            if (!enabled) return true;
            if (toggled || down) well(g, body, bodyRadius, 3.0f);
            else if (hovered)
            {
                raise(g, body, bodyRadius, 1.0f, 2.0f);
                face(g, body, bodyRadius, material());
            }
            return true;
        }
        if (isSegmentButton(button))
        {
            const auto track = bounds.reduced(0.5f);
            g.setColour(colour(ext().sunken));
            g.fillPath(segmentPath(track, radius, button.getConnectedEdgeFlags()));
            {
                const juce::Graphics::ScopedSaveState saved(g);
                g.reduceClipRegion(segmentPath(track, radius, button.getConnectedEdgeFlags()));
                pressTrack(g, track, radius, 4.0f);
            }
            if (toggled)
            {
                const auto pill = track.reduced(3.0f);
                raise(g, pill, juce::jmax(2.0f, radius - 3.0f), 1.0f, 1.5f);
                face(g, pill, juce::jmax(2.0f, radius - 3.0f), material().brighter(0.03f));
            }
            textColour = !enabled ? colour(pal().secondaryText).withAlpha(0.5f)
                       : toggled ? colour(pal().text) : colour(pal().secondaryText);
            return true;
        }
        if (!enabled)
        {
            g.setColour(colour(ext().hairline).withMultipliedAlpha(2.0f));
            g.drawRoundedRectangle(body, bodyRadius, 1.0f);
            return true;
        }
        const auto accent = static_cast<bool>(button.getProperties().getWithDefault("accent", false));
        if (toggled || down)
        {
            well(g, body, bodyRadius, 3.5f);
            if (toggled) textColour = colour(pal().accent).darker(0.15f);
            return true;
        }
        raise(g, body, bodyRadius, 1.5f, 2.0f);
        face(g, body, bodyRadius, accent ? colour(pal().accent) : hovered ? material().brighter(0.02f) : material());
        if (accent) textColour = juce::Colours::white;
        return true;
    }

    void switchTrack(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled, bool focused) const override
    {
        const auto radius = track.getHeight() * 0.5f;
        const auto alpha = enabled ? 1.0f : 0.5f;
        g.setColour((on ? colour(pal().accent) : colour(ext().sunken)).withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, radius);
        press(g, track, radius, 3.0f, on ? 0.55f : 1.0f);
        const auto thumb = juce::Rectangle<float>(track.getX() + (on ? track.getWidth() - track.getHeight() + 2.0f : 2.0f),
                                                  track.getY() + 2.0f, track.getHeight() - 4.0f, track.getHeight() - 4.0f);
        softShadow(g, thumb, thumb.getHeight() * 0.5f, { 0.6f, 0.8f }, 1.5f, shade(alpha));
        face(g, thumb, thumb.getHeight() * 0.5f, material().brighter(0.06f).withMultipliedAlpha(alpha));
        if (focused) focusRing(g, track, radius);
    }

    void comboBox(juce::Graphics& g, juce::ComboBox& box, bool down) const override
    {
        const auto bounds = box.getLocalBounds().toFloat();
        const auto radius = juce::jmin(SkinStyle::geometry().controlRadius, bounds.getHeight() * 0.5f);
        const auto body = bounds.reduced(2.5f);
        const auto bodyRadius = juce::jmax(2.0f, radius - 2.5f);
        if (!box.isEnabled())
        {
            g.setColour(colour(ext().hairline).withMultipliedAlpha(2.0f));
            g.drawRoundedRectangle(body, bodyRadius, 1.0f);
            return;
        }
        if (down)
        {
            well(g, body, bodyRadius, 3.5f);
            return;
        }
        raise(g, body, bodyRadius, 1.5f, 2.0f);
        face(g, body, bodyRadius, material());
    }

    void linearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position, juce::Slider& slider) const override
    {
        const auto alpha = slider.isEnabled() ? 1.0f : 0.4f;
        const auto pos = juce::jlimit(track.getX(), track.getRight(), position);
        const auto groove = juce::Rectangle<float>(track.getX(), track.getCentreY() - 3.0f, track.getWidth(), 6.0f);
        well(g, groove, 3.0f, 2.5f);
        const auto signal = colour(ext().signal).withMultipliedAlpha(alpha);
        g.setGradientFill(juce::ColourGradient(signal.brighter(0.25f), groove.getX(), 0.0f, signal, pos, 0.0f, false));
        g.fillRoundedRectangle(groove.withRight(pos).reduced(0.0f, 1.0f), 2.0f);
        const auto thumb = juce::Rectangle<float>(pos - 8.0f, track.getCentreY() - 8.0f, 16.0f, 16.0f);
        raise(g, thumb, 8.0f, 1.25f, 2.0f, alpha);
        face(g, thumb, 8.0f, material().brighter(0.04f));
        if (slider.hasKeyboardFocus(true)) focusRing(g, thumb, 8.0f);
    }

    void popupBackground(juce::Graphics& g, juce::Rectangle<float> bounds) const override
    {
        const auto body = bounds.reduced(0.5f);
        face(g, body, 12.0f, material());
        rimLight(g, body, 12.0f, highlight());
        g.setColour(colour(pal().border));
        g.drawRoundedRectangle(body, 12.0f, 1.0f);
    }

    void popupHighlight(juce::Graphics& g, juce::Rectangle<float> row) const override
    {
        well(g, row, 8.0f, 3.0f);
    }

    void knob(juce::Graphics& g, juce::Rectangle<float> dial, float angle, juce::Colour indicator,
              bool enabled, bool focused) const override
    {
        const auto body = dial.reduced(1.5f);
        const auto radius = body.getWidth() * 0.5f;
        if (enabled) raise(g, body, radius, 1.5f, 2.0f);
        face(g, body, radius, material());
        // A shallow pressed ring separates the grip from the cap.
        const auto cap = body.reduced(body.getWidth() * 0.16f);
        g.setColour(shade(0.35f));
        g.drawEllipse(cap.translated(0.5f, 0.5f), 1.0f);
        g.setColour(highlight(0.9f));
        g.drawEllipse(cap.translated(-0.5f, -0.5f), 1.0f);
        const juce::Point<float> direction(std::sin(angle), -std::cos(angle));
        const auto centre = body.getCentre();
        g.setColour(enabled ? indicator : colour(pal().secondaryText).withAlpha(0.45f));
        g.drawLine({ centre + direction * radius * 0.32f, centre + direction * radius * 0.70f },
                   juce::jmax(2.0f, body.getWidth() * 0.07f));
        if (focused) focusRing(g, body, radius);
    }

    void meterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail, bool loudness) const override
    {
        const auto radius = juce::jmin(4.0f, bar.getWidth() * 0.5f);
        well(g, bar, radius, 3.0f);
        paintMeterFill(g, bar.reduced(1.5f), radius - 1.5f, level, trail, loudness);
    }

    void meterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held) const override
    {
        const auto groove = bounds.withSizeKeepingCentre(bounds.getWidth() - 6.0f, 10.0f);
        well(g, groove, 5.0f, 3.0f);
        const auto track = groove.reduced(2.0f);
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
            g.fillRect(track.getX() + track.getWidth() * juce::jlimit(0.0f, 1.0f, held) - 0.75f, track.getY() - 1.0f,
                       1.5f, track.getHeight() + 2.0f);
        }
    }
};
} // namespace

const Definition& softSkin()
{
    static const SoftPainter painter;
    static const Definition skin { lightPalette, nullptr, extras, nullptr, { 16.0f, 10.0f, 16, 14 }, painter };
    return skin;
}
} // namespace singleeditor::skins
