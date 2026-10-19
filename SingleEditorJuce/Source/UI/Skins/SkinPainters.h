#pragma once

#include "UI/Skins/SkinStyle.h"

// Internal to the skin implementation files. SkinStyle dispatches to one
// Painter per skin; a skin overrides only what differs from the flat base.
namespace singleeditor::skins
{
struct Painter
{
    virtual ~Painter() = default;

    virtual void windowBackground(juce::Graphics& g, juce::Rectangle<float> area,
                                  juce::Point<float> origin, juce::Rectangle<float> window) const;
    virtual void titleBar(juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> origin,
                          juce::Rectangle<float> window) const;
    virtual void toolbar(juce::Graphics& g, juce::Component& surface) const;
    virtual void statusBar(juce::Graphics& g, juce::Rectangle<float> bounds) const;
    virtual void panel(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& title,
                       int icon, int trailingControls) const;
    virtual void panelShadow(juce::Graphics& g, juce::Rectangle<float> tile) const;
    virtual void panelOutline(juce::Graphics& g, juce::Rectangle<float> tile, bool live) const;
    virtual void dataWell(juce::Graphics& g, juce::Rectangle<float> area, float radius) const;
    virtual void waveformLane(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour colour) const;
    virtual void clock(juce::Graphics& g, juce::Rectangle<float> bounds,
                       const juce::String& main, const juce::String& frames) const;
    virtual void iconButton(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                            bool& onAccent) const;
    virtual bool buttonBackground(juce::Graphics& g, juce::Button& button, bool hovered, bool down,
                                  juce::Colour& textColour) const;
    virtual void switchTrack(juce::Graphics& g, juce::Rectangle<float> track, bool on, bool enabled,
                             bool focused) const;
    virtual void comboBox(juce::Graphics& g, juce::ComboBox& box, bool down) const;
    virtual void linearSlider(juce::Graphics& g, juce::Rectangle<float> track, float position,
                              juce::Slider& slider) const;
    virtual void popupBackground(juce::Graphics& g, juce::Rectangle<float> bounds) const;
    virtual void popupHighlight(juce::Graphics& g, juce::Rectangle<float> row) const;
    virtual bool popupHighlightUsesAccent() const { return false; }
    virtual void knob(juce::Graphics& g, juce::Rectangle<float> dial, float angle, juce::Colour indicator,
                      bool enabled, bool focused) const;
    virtual void meterBar(juce::Graphics& g, juce::Rectangle<float> bar, float level, float trail,
                          bool loudness) const;
    virtual void meterBridge(juce::Graphics& g, juce::Rectangle<float> bounds, float level, float held) const;
};

// One optional skin: palettes, geometry and painter. Light-only skins leave
// the night entries null.
struct Definition final
{
    const ModernGlassTokens::Palette& light;
    const ModernGlassTokens::Palette* night;
    const SkinExtras& lightExtras;
    const SkinExtras* nightExtras;
    SkinGeometry geometry;
    const Painter& painter;
};

const Definition& tilesSkin();
const Definition& softSkin();
const Definition& liquidSkin();

// Shared vector helpers: contour strokes instead of blurred images, so a
// repaint allocates nothing and Direct2D draws them as plain geometry.
void softShadow(juce::Graphics& g, juce::Rectangle<float> shape, float radius,
                juce::Point<float> offset, float blur, juce::Colour colour);
void innerShadow(juce::Graphics& g, juce::Rectangle<float> shape, float radius,
                 juce::Point<float> offset, float blur, juce::Colour colour);
void rimLight(juce::Graphics& g, juce::Rectangle<float> shape, float radius, juce::Colour colour,
              float thickness = 1.0f);
void paintPanelGlyph(juce::Graphics& g, juce::Rectangle<float> glyph, int icon, juce::Colour colour);
// Header pieces shared by every skin's tile: 22 px icon well and title text.
juce::Rectangle<float> iconBox(juce::Rectangle<int> header);
void paintPanelTitle(juce::Graphics& g, juce::Rectangle<int> header, const juce::String& title, int trailingControls);
juce::Rectangle<int> panelHeader(juce::Rectangle<int> bounds);
// Amber ring for a Mag whose processor is audible as a preview.
void paintLiveRing(juce::Graphics& g, juce::Rectangle<float> tile, float radius);
// Leaves the brush the waveform peaks are filled with as the current fill.
void setWaveformBrush(juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour signal);
void paintClockText(juce::Graphics& g, juce::Rectangle<float> display, const juce::String& main,
                    const juce::String& frames);
void paintMeterFill(juce::Graphics& g, juce::Rectangle<float> bar, float radius, float level, float trail,
                    bool loudness);
juce::Path segmentPath(juce::Rectangle<float> r, float radius, int connectedEdges);
bool isSegmentButton(const juce::Button& button);
bool isGhostButton(const juce::Button& button);
juce::Font uiFont(float emPixels, bool semibold = false);
juce::Colour colour(std::uint32_t argb);
bool isNight();
const ModernGlassTokens::Palette& pal();
const SkinExtras& ext();
juce::String role(const juce::Component& component);
} // namespace singleeditor::skins
