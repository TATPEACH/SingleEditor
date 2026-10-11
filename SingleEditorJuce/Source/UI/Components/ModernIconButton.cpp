#include "UI/Components/ModernIconButton.h"
#include "UI/LookAndFeel/ModernGlassPainter.h"

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/CoreActionIcons.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
namespace
{
juce::CriticalSection& getIconCacheLock()
{
    static juce::CriticalSection lock;
    return lock;
}

std::map<juce::String, std::shared_ptr<juce::Drawable>>& getTintedDrawableCache()
{
    static std::map<juce::String, std::shared_ptr<juce::Drawable>> cache;
    return cache;
}

juce::String colourToSvgString(juce::Colour colour)
{
    return juce::String::formatted("#%02X%02X%02X", colour.getRed(), colour.getGreen(), colour.getBlue());
}

bool isShapeTag(const juce::String& tagName)
{
    static const juce::StringArray shapeTags { "path", "rect", "circle", "ellipse", "polygon", "polyline", "line", "text", "use" };
    return shapeTags.contains(tagName);
}

void tintSvgElement(juce::XmlElement& element, const juce::String& tint)
{
    const auto tagName = element.getTagName().toLowerCase();
    const auto fill = element.getStringAttribute("fill");
    const auto stroke = element.getStringAttribute("stroke");

    if (fill.isNotEmpty() && fill != "none")
        element.setAttribute("fill", tint);

    if (stroke.isNotEmpty() && stroke != "none")
        element.setAttribute("stroke", tint);

    if (element.hasAttribute("color"))
        element.setAttribute("color", tint);

    if (fill.isEmpty() && stroke.isEmpty() && isShapeTag(tagName))
    {
        if (tagName == "line" || tagName == "polyline")
            element.setAttribute("stroke", tint);
        else
            element.setAttribute("fill", tint);
    }

    for (auto* child = element.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        tintSvgElement(*child, tint);
}

std::unique_ptr<juce::Drawable> createTintedSvgDrawable(const juce::String& svgText, juce::Colour tint)
{
    auto xml = juce::parseXML(svgText);
    if (xml == nullptr)
        return {};

    const auto tintString = colourToSvgString(tint);
    tintSvgElement(*xml, tintString);
    return juce::Drawable::createFromSVG(*xml);
}

std::unique_ptr<juce::Drawable> cloneDrawable(const juce::Drawable& drawable)
{
    return drawable.createCopy();
}

juce::String makeTintedDrawableCacheKey(RendererAssetId assetId, juce::Colour tint)
{
    return juce::String(static_cast<int>(assetId))
        + "|"
        + juce::String::toHexString(static_cast<juce::int64>(tint.getARGB()));
}

std::unique_ptr<juce::Drawable> createCachedTintedDrawable(RendererAssetId assetId, juce::Colour tint)
{
    const auto cacheKey = makeTintedDrawableCacheKey(assetId, tint);
    {
        const juce::ScopedLock sl(getIconCacheLock());
        if (const auto it = getTintedDrawableCache().find(cacheKey); it != getTintedDrawableCache().end()
            && it->second != nullptr)
        {
            return cloneDrawable(*it->second);
        }
    }

    const auto bytes = coreActionIconBytes(assetId);
    if (bytes.data == nullptr || bytes.size <= 0)
        return {};

    auto drawable = createTintedSvgDrawable(juce::String::fromUTF8(bytes.data, bytes.size), tint);
    if (drawable == nullptr)
        return {};

    auto cachedDrawable = std::shared_ptr<juce::Drawable>(drawable.release());
    {
        const juce::ScopedLock sl(getIconCacheLock());
        getTintedDrawableCache()[cacheKey] = cachedDrawable;
    }

    return cloneDrawable(*cachedDrawable);
}

} // namespace

ModernIconButton::ModernIconButton(const juce::String& componentName)
    : juce::Button(componentName),
      normalTint(ModernAppleLookAndFeel::primaryText()),
      disabledTint(ModernAppleLookAndFeel::disabledText())
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTriggeredOnMouseDown(false);
}

void ModernIconButton::setIconName(const juce::String& newIconName)
{
    iconName = newIconName;
    sourceDrawable.reset();
    refreshIcons();
}

void ModernIconButton::setIconDrawable(std::unique_ptr<juce::Drawable> newDrawable)
{
    iconName.clear();
    sourceDrawable = std::move(newDrawable);
    refreshIcons();
}

void ModernIconButton::setIconPadding(int newPadding)
{
    iconPadding = juce::jlimit(0, 16, newPadding);
    repaint();
}

void ModernIconButton::setNominalIconSize(int newSize)
{
    nominalIconSize = juce::jlimit(10, 36, newSize);
    repaint();
}

void ModernIconButton::setCornerRadius(float newCornerRadius)
{
    cornerRadius = juce::jlimit(0.0f, 20.0f, newCornerRadius);
    repaint();
}

void ModernIconButton::setTintColours(juce::Colour normalColour, juce::Colour disabledColour)
{
    customTint = true;
    normalTint = normalColour;
    disabledTint = disabledColour;
    refreshIcons();
}

void ModernIconButton::lookAndFeelChanged()
{
    if (customTint) return;
    const auto normal = ModernAppleLookAndFeel::primaryText();
    const auto disabled = ModernAppleLookAndFeel::disabledText();
    if (normalTint == normal && disabledTint == disabled) return;
    normalTint = normal;
    disabledTint = disabled;
    refreshIcons();
}

void ModernIconButton::paintButton(juce::Graphics& g, bool hovered, bool down)
{
    auto bounds = getLocalBounds().toFloat();

    const bool glassControl = !ModernAppleLookAndFeel::isStartupSkin()
        && static_cast<bool>(getProperties().getWithDefault("glassControl", false));
    if (glassControl)
    {
        ModernGlassPainter::control(g, bounds, isEnabled(), hovered, down, getToggleState());
    }

    // Draw hover/pressed background - only for enabled buttons
    if (!glassControl && isEnabled() && (hovered || down))
    {
        auto fill = down ? ModernAppleLookAndFeel::ghostPressedFill()
                         : (getToggleState() ? ModernAppleLookAndFeel::accentBlue().withAlpha(0.16f)
                                             : ModernAppleLookAndFeel::ghostHoverFill());
        g.setColour(fill);
        g.fillRoundedRectangle(bounds.reduced(0.5f), cornerRadius);
    }

    if (!ModernAppleLookAndFeel::isStartupSkin() && !RendererVisualStyle::isClassic())
        ModernGlassPainter::focusOutline(g, bounds, cornerRadius, hasKeyboardFocus(false), isEnabled());

    // Calculate icon placement
    const auto paddedBounds = bounds.reduced(static_cast<float>(iconPadding));
    const auto maxSide = juce::jmax(0.0f, juce::jmin(paddedBounds.getWidth(), paddedBounds.getHeight()));
    const auto targetSide = juce::jmin(maxSide, static_cast<float>(nominalIconSize));
    auto targetBounds = paddedBounds.withSizeKeepingCentre(targetSide, targetSide);
    if (targetBounds.isEmpty())
        return;

    prepareIconsIfNeeded();
    auto* drawable = isEnabled() ? normalIcon.get() : disabledIcon.get();
    if (drawable != nullptr)
    {
        // Apply press scale via Graphics transform (not component transform - that breaks hit testing)
        juce::Graphics::ScopedSaveState state(g);
        if (down && isEnabled())
        {
            auto scale = 0.95f;
            auto cx = bounds.getCentreX();
            auto cy = bounds.getCentreY();
            g.addTransform(juce::AffineTransform::scale(scale, scale, cx, cy));
        }

        const auto sourceBounds = drawable->getDrawableBounds();
        g.addTransform(juce::RectanglePlacement(juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize)
                           .getTransformToFit(sourceBounds, targetBounds));
        drawable->draw(g, 1.0f);
        return;
    }

    if (getButtonText().isNotEmpty())
    {
        juce::Graphics::ScopedSaveState state(g);
        if (down && isEnabled())
        {
            auto scale = 0.95f;
            auto cx = bounds.getCentreX();
            auto cy = bounds.getCentreY();
            g.addTransform(juce::AffineTransform::scale(scale, scale, cx, cy));
        }

        g.setColour(isEnabled() ? normalTint : disabledTint);
        g.setFont(juce::Font(juce::FontOptions(13.0f, getToggleState() ? juce::Font::bold : juce::Font::plain)));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(6, 2), juce::Justification::centred, 1);
    }
}

void ModernIconButton::enablementChanged()
{
    repaint();
}

void ModernIconButton::refreshIcons()
{
    // Startup installs controls behind the preview. Resolve/parse only when a
    // glyph is actually drawn, coalescing setup and theme changes beforehand.
    iconsNeedPreparation = true;
    repaint();
}

void ModernIconButton::prepareIconsIfNeeded()
{
    if (!iconsNeedPreparation)
        return;
    iconsNeedPreparation = false;
    normalIcon.reset();
    disabledIcon.reset();

    if (sourceDrawable != nullptr)
    {
        normalIcon = cloneDrawable(*sourceDrawable);
        disabledIcon = cloneDrawable(*sourceDrawable);
        return;
    }

    if (iconName.isEmpty())
    {
        return;
    }

    const auto assetId = RendererVisualStyle::assetIdForFileName(iconName.toRawUTF8());
    if (assetId == RendererAssetId::unknown)
        return;
    normalIcon = createCachedTintedDrawable(assetId, normalTint);
    disabledIcon = createCachedTintedDrawable(assetId, disabledTint);

}
} // namespace singleeditor
