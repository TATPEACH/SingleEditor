#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernNorthStarVisualContract.h"
#include "UI/LookAndFeel/ModernGlassTokens.h"
#include "UI/LookAndFeel/ModernGlassPainter.h"
#include "UI/Components/ModernTransportLayout.h"
#include "UI/Components/ModernTransportWidgets.h"
#include "UI/Components/ModernIconButton.h"
#include "UI/Components/SaveEncodingOptionsDialog.h"
#include "UI/Components/CardSurface.h"
#include "UI/Components/WaveformLaneControls.h"
#include "UI/LookAndFeel/ModernEqKnobPainter.h"
#include "UI/LookAndFeel/ModernWaveformPainter.h"
#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/Renderer/CoreActionIcons.h"
#include "UI/Skins/SkinStyle.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "ModernVisualSystemTests failed: " << message << '\n';
    std::exit(1);
}

class ControlSurface final : public juce::Component
{
public:
    void paint(juce::Graphics& g) override
    {
        g.fillAll(singleeditor::ModernAppleLookAndFeel::windowBackground());
    }
};

void runOptionalSkinThemeTest()
{
    using namespace singleeditor;
    RendererVisualStyle::setModernTheme(ModernTheme::night);
    SkinStyle::setActive(ModernSkin::soft);
    require(!RendererVisualStyle::isNight(), "Soft resolves the saved Night request to Light");
    SkinStyle::setActive(ModernSkin::liquid);
    require(RendererVisualStyle::isNight(), "switching away from Soft restores the saved Night request");

    ModernAppleLookAndFeel lookAndFeel;
    for (const auto skin : { ModernSkin::original, ModernSkin::soft, ModernSkin::liquid, ModernSkin::tiles })
    {
        SkinStyle::setActive(skin);
        for (const auto theme : { ModernTheme::light, ModernTheme::night })
        {
            RendererVisualStyle::setModernTheme(theme);
            juce::Image picture(juce::Image::ARGB, 320, 200, true);
            juce::Graphics g(picture);
            lookAndFeel.drawPopupMenuBackground(g, picture.getWidth(), picture.getHeight());
            require(picture.getPixelAt(160, 100).isOpaque(),
                    "every skin gives detached popup menus an opaque reading surface");
        }
    }
    SkinStyle::setActive(ModernSkin::liquid);
    RendererSelection::resetForTests(RendererKind::classic);
    require(SkinStyle::isOriginal(), "Classic keeps its own paint path with an optional skin saved");
    RendererSelection::resetForTests(RendererKind::modern);
    require(SkinStyle::active() == ModernSkin::liquid, "returning to Modern retains the selected skin");
    SkinStyle::setActive(ModernSkin::original);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "OptionalSkinThemeTests passed (Night restoration, eight popup surfaces, Classic isolation)\n";
}

void runModernRegionOutlineTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    ModernAppleLookAndFeel lookAndFeel;
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const float scale : { 1.0f, 1.5f, 2.0f })
            for (const auto size : { juce::Point<int>(140, 110), juce::Point<int>(940, 700) })
            {
                RendererVisualStyle::setModernTheme(theme);
                ControlSurface root;
                juce::Component region;
                root.setLookAndFeel(&lookAndFeel);
                // Explicit opt-in only: JUCE requires an actual peer for isShowing().
                // This render fixture stays offscreen and cannot take input or focus.
                root.setBounds(-10000, -10000, size.x + 40, size.y + 40);
                root.addToDesktop(juce::ComponentPeer::windowIsTemporary
                    | juce::ComponentPeer::windowIgnoresMouseClicks
                    | juce::ComponentPeer::windowIgnoresKeyPresses);
                root.setVisible(true);
                root.addAndMakeVisible(region);
                region.setBounds(10, 10, size.x, size.y);
                const auto idle = root.createComponentSnapshot(root.getLocalBounds(), true, scale);
                auto outline = lookAndFeel.createFocusOutlineForComponent(region);
                outline->setOwner(&region);
                require(root.getNumChildComponents() == 2, "region focus installs one real overlay sibling");
                require(root.getComponentAt(region.getBounds().getCentre()) == &region,
                        "focus overlay never intercepts the region's pointer input");
                const auto focused = root.createComponentSnapshot(root.getLocalBounds(), true, scale);
                int changed = 0;
                for (int y = 0; y < idle.getHeight(); ++y)
                    for (int x = 0; x < idle.getWidth(); ++x)
                        if (idle.getPixelAt(x, y) != focused.getPixelAt(x, y))
                        {
                            ++changed;
                            const juce::Point<float> point((x + 0.5f) / scale, (y + 0.5f) / scale);
                            const auto bounds = region.getBounds().toFloat();
                            require(bounds.contains(point) && !bounds.reduced(6.0f).contains(point),
                                    "region focus remains on its own perimeter without covering data or adjacent Mags");
                        }
                require(changed > size.x, "actual JUCE outline is visible in each theme and scale");
                region.setBounds(18, 16, size.x - 10, size.y - 10);
                require(root.getChildComponent(1)->getBounds() == region.getBounds(),
                        "focus outline tracks owner resize and movement");
                region.setVisible(false);
                require(root.getNumChildComponents() == 1, "hidden Mag removes its focus overlay");
                region.setVisible(true);
                require(root.getNumChildComponents() == 2, "visible owner can restore its outline");
                outline.reset();
                require(root.getNumChildComponents() == 1, "focus departure destroys the previous outline");
                root.setVisible(false);
                root.removeFromDesktop();
                root.setLookAndFeel(nullptr);
                ++renders;
            }
    RendererVisualStyle::setModernTheme(savedTheme);
    std::cout << "ModernRegionOutlineTests passed (" << renders
              << " real outline renders, hit testing, move/resize/hide/release)\n";
}

void runModernWaveformPlaceholderTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    ModernAppleLookAndFeel lookAndFeel;
    auto* previousDefault = &juce::LookAndFeel::getDefaultLookAndFeel();
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_PLACEHOLDER_OFFSCREEN_DIR", {});
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        for (const auto size : { juce::Point<int>(104, 60), {160, 110}, {360, 140}, {760, 348}, {1530, 1000} })
            for (const auto state : { ModernWaveformPainter::PlaceholderState::empty,
                                     ModernWaveformPainter::PlaceholderState::loading,
                                     ModernWaveformPainter::PlaceholderState::failed })
            {
                const auto loading = state == ModernWaveformPainter::PlaceholderState::loading;
                const auto failed = state == ModernWaveformPainter::PlaceholderState::failed;
                const juce::Rectangle<float> available(7, 11, static_cast<float>(size.x), static_cast<float>(size.y));
                const auto layout = ModernWaveformPainter::placeholderLayout(available);
                const std::array<juce::Rectangle<float>, 3> parts { layout.icon, layout.title, layout.detail };
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    if (parts[i].isEmpty()) continue;
                    require(available.contains(parts[i]), "empty/loading content remains inside the real plot");
                    for (size_t j = i + 1; j < parts.size(); ++j)
                        require(parts[j].isEmpty() || !parts[i].intersects(parts[j]),
                                "placeholder illustration and text never overlap");
                }
                const auto title = failed ? "Could not open audio" : loading ? "Opening audio" : "Open audio";
                require(juce::GlyphArrangement::getStringWidth(ModernTypography::font(layout.titleEm, juce::Font::bold), title)
                            <= layout.title.getWidth() * 2 && layout.title.getHeight() >= 28,
                        "narrow placeholder retains normal-size title space");
                juce::Image picture(juce::Image::ARGB, size.x + 14, size.y + 22, true);
                {
                    juce::Graphics g(picture);
                    g.setColour(RendererVisualStyle::waveformSurface());
                    g.fillRect(available);
                    ModernWaveformPainter::paintPlaceholder(g, available, state,
                        juce::String::fromUTF8("朴树 - 平凡之路 - a long source name.wav"),
                        failed ? "No supported audio reader could open this file."
                               : "Metadata is ready. Drawing the first waveform...");
                }
                require(picture.getPixelAt(0, 0).isTransparent(), "placeholder cannot spill outside its plot");
                if (output.isNotEmpty())
                {
                    const juce::File directory(output);
                    require(directory.createDirectory().wasOk(), "create placeholder evidence directory");
                    auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                        + (failed ? "failed-" : loading ? "loading-" : "empty-") + juce::String(size.x) + "x" + juce::String(size.y) + ".png").createOutputStream();
                    require(stream != nullptr && stream->setPosition(0)
                                && juce::PNGImageFormat().writeImageToStream(picture, *stream) && stream->truncate().wasOk(),
                            "write actual placeholder painter evidence");
                }
                ++renders;
            }
    }
    RendererVisualStyle::setModernTheme(savedTheme);
    juce::LookAndFeel::setDefaultLookAndFeel(previousDefault);
    std::cout << "ModernWaveformPlaceholderTests passed (" << renders << " empty/loading/failed theme and size cases)\n";
}

void runModernAlertPresentationTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_ALERT_OFFSCREEN_DIR", {});
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        ModernAppleLookAndFeel lookAndFeel;
        auto* previousDefault = &juce::LookAndFeel::getDefaultLookAndFeel();
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        juce::Component owner;
        owner.setBounds(0, 0, 802, 600);
        owner.setLookAndFeel(&lookAndFeel);
        std::unique_ptr<juce::AlertWindow> alert(lookAndFeel.createAlertWindow(
            "Float Audio Waveform",
            "The waveform will move to a separate native window. The main window keeps a lightweight return anchor, "
            "and closing the waveform window redocks it. Continue?",
            "Float Waveform", "Cancel", {}, juce::MessageBoxIconType::NoIcon, 2, &owner));
        require(alert != nullptr && alert->getWidth() < 802 && alert->getHeight() < 400,
                "the real confirmation layout fits the compact application window");
        std::vector<juce::Rectangle<int>> buttons;
        for (auto* child : alert->getChildren())
            if (dynamic_cast<juce::TextButton*>(child) != nullptr)
            {
                require(alert->getLocalBounds().contains(child->getBounds()), "confirmation buttons stay inside their dialog");
                buttons.push_back(child->getBounds());
            }
        require(buttons.size() == 2 && !buttons[0].intersects(buttons[1]), "confirmation actions remain separate and reachable");
        for (const float scale : { 1.0f, 1.5f, 2.0f })
        {
            const auto picture = alert->createComponentSnapshot(alert->getLocalBounds(), true, scale);
            require(picture.isValid() && picture.getPixelAt(juce::roundToInt(10 * scale), juce::roundToInt(10 * scale))
                        == ModernAppleLookAndFeel::elevatedPanelBackground(),
                    "confirmation paints the current Modern theme rather than the inherited dark V4 surface");
            if (output.isNotEmpty())
            {
                const juce::File directory(output);
                require(directory.createDirectory().wasOk(), "create alert render directory");
                auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                    + juce::String(juce::roundToInt(scale * 100)) + ".png").createOutputStream();
                require(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                    && juce::PNGImageFormat().writeImageToStream(picture, *stream), "write actual alert component render");
            }
            ++renders;
        }
        alert.reset();
        owner.setLookAndFeel(nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel(previousDefault);
    }
    RendererVisualStyle::setModernTheme(savedTheme);
    std::cout << "ModernAlertPresentationTests passed (" << renders << " themed/DPI component renders; native response tested separately)\n";
}

void runModernAlertIconPresentationTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_ALERT_OFFSCREEN_DIR", {});
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        ModernAppleLookAndFeel lookAndFeel;
        auto* previousDefault = &juce::LookAndFeel::getDefaultLookAndFeel();
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        juce::Component owner;
        owner.setBounds(0, 0, 800, 600);
        owner.setLookAndFeel(&lookAndFeel);
        for (const auto type : { juce::MessageBoxIconType::WarningIcon,
                                 juce::MessageBoxIconType::InfoIcon,
                                 juce::MessageBoxIconType::QuestionIcon })
        {
            std::unique_ptr<juce::AlertWindow> alert(lookAndFeel.createAlertWindow(
                "Unsaved Changes", "Save changes to Save read-only fixture.wav before closing Single Editor?",
                "Save", "Discard", "Cancel", type, 3, &owner));
            std::vector<juce::Rectangle<int>> buttons;
            for (auto* child : alert->getChildren())
                if (dynamic_cast<juce::TextButton*>(child) != nullptr)
                {
                    require(alert->getLocalBounds().contains(child->getBounds()), "alert actions fit inside the dialog");
                    for (const auto bounds : buttons)
                        require(!bounds.intersects(child->getBounds()), "alert actions never overlap");
                    buttons.push_back(child->getBounds());
                }
            require(buttons.size() == 3, "all three unsaved document choices remain available");
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                const auto picture = alert->createComponentSnapshot(alert->getLocalBounds(), true, scale);
                if (output.isNotEmpty())
                {
                    const juce::File directory(output);
                    require(directory.createDirectory().wasOk(), "create alert icon render directory");
                    auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                        + "icon-" + juce::String(static_cast<int>(type)) + "-" + juce::String(juce::roundToInt(scale * 100))
                        + ".png").createOutputStream();
                    require(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                                && juce::PNGImageFormat().writeImageToStream(picture, *stream), "write alert icon render");
                }
                // The old V4 icon starts outside the window and spills into this inset.
                // Check the real painter, not a copy of its intended rectangle.
                for (int y = 20; y < 100; ++y)
                    require(picture.getPixelAt(juce::roundToInt(12 * scale), juce::roundToInt(y * scale))
                                == ModernAppleLookAndFeel::elevatedPanelBackground(),
                            "alert icon stays inset from the window edge");
                ++renders;
            }
        }
        owner.setLookAndFeel(nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel(previousDefault);
    }
    RendererVisualStyle::setModernTheme(savedTheme);
    std::cout << "ModernAlertIconPresentationTests passed (" << renders << " actual icon/theme/DPI renders)\n";
}

void runSaveEncodingDialogPresentationTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_ALERT_OFFSCREEN_DIR", {});
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        ModernAppleLookAndFeel lookAndFeel;
        juce::Component owner;
        owner.setBounds(0, 0, 800, 600);
        owner.setLookAndFeel(&lookAndFeel);
        for (const auto* name : { "MP3 bitrate", "Ogg Vorbis quality", "FLAC compression" })
        {
            const juce::String fieldName(name);
            const juce::StringArray choices = fieldName == "MP3 bitrate"
                ? juce::StringArray { "96 kbps", "128 kbps", "160 kbps", "192 kbps", "256 kbps", "320 kbps" }
                : fieldName == "Ogg Vorbis quality"
                    ? juce::StringArray { "Quality 0", "Quality 1", "Quality 10" }
                    : juce::StringArray { "Level 0", "Level 5", "Level 8" };
            SaveEncodingOptionsDialog dialog(owner, fieldName, choices, choices.size() - 1);
            auto& choice = dialog.getChoice();
            const auto fieldBounds = dialog.getLocalArea(&choice, choice.getLocalBounds());
            require(dialog.getWidth() <= 560 && dialog.getHeight() < 400,
                    "real encoding form fits the compact native workspace");
            require(choice.getHeight() >= 32 && dialog.getLocalBounds().contains(fieldBounds),
                    "encoding choice keeps a full-height reachable control");
            require(juce::GlyphArrangement::getStringWidth(lookAndFeel.getComboBoxFont(choice), choice.getText())
                        <= choice.getWidth() - 38,
                    "longest encoding value fits without shrinking or clipping");
            std::vector<juce::Rectangle<int>> buttons;
            for (auto* child : dialog.getChildren())
                if (dynamic_cast<juce::TextButton*>(child) != nullptr)
                {
                    require(dialog.getLocalBounds().contains(child->getBounds())
                                && child->getY() >= fieldBounds.getBottom() + 8,
                            "encoding actions stay below the field with visible separation");
                    buttons.push_back(child->getBounds());
                }
            require(buttons.size() == 2 && !buttons[0].intersects(buttons[1]),
                    "encoding Continue and Cancel remain separate");
            for (const float scale : { 1.0f, 1.5f, 2.0f })
            {
                const auto picture = dialog.createComponentSnapshot(dialog.getLocalBounds(), true, scale);
                require(picture.isValid() && picture.getPixelAt(juce::roundToInt(12 * scale), juce::roundToInt(24 * scale))
                            == ModernAppleLookAndFeel::elevatedPanelBackground(),
                        "encoding form uses its owner theme, including an independently constructed alert");
                if (output.isNotEmpty())
                {
                    const juce::File directory(output);
                    require(directory.createDirectory().wasOk(), "create encoding form render directory");
                    auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                        + "encoding-" + fieldName.upToFirstOccurrenceOf(" ", false, false).toLowerCase()
                        + "-" + juce::String(juce::roundToInt(scale * 100)) + ".png").createOutputStream();
                    require(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                                && juce::PNGImageFormat().writeImageToStream(picture, *stream), "write real encoding form render");
                }
                ++renders;
            }
        }
        owner.setLookAndFeel(nullptr);
    }
    RendererVisualStyle::setModernTheme(savedTheme);
    std::cout << "SaveEncodingDialogPresentationTests passed (" << renders << " actual format/theme/DPI renders)\n";
}

void runModernFocusOutlineTest()
{
    using namespace singleeditor;
    const auto savedTheme = RendererVisualStyle::modernTheme();
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_FOCUS_OFFSCREEN_DIR", {});
    int cases = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const float scale : { 1.0f, 1.5f, 2.0f })
            for (const int width : { 20, 32, 96, 180 })
                for (const bool selected : { false, true })
                    for (const bool enabled : { false, true })
                    {
                        RendererVisualStyle::setModernTheme(theme);
                        const auto height = width == 20 ? 20 : 32;
                        const juce::Rectangle<float> bounds(8.0f, 8.0f, float(width), float(height));
                        const auto render = [&](bool focused)
                        {
                            juce::Image image(juce::Image::ARGB, juce::roundToInt((width + 16) * scale),
                                              juce::roundToInt((height + 16) * scale), true);
                            juce::Graphics g(image);
                            g.addTransform(juce::AffineTransform::scale(scale));
                            const auto tint = !enabled ? RendererVisualStyle::mutedSurface()
                                : selected ? RendererVisualStyle::accentBlue()
                                           : RendererVisualStyle::elevatedPanelBackground();
                            ModernGlassPainter::surface(g, bounds, tint, 8.0f);
                            ModernGlassPainter::focusOutline(g, bounds, 8.0f, focused, enabled);
                            return image;
                        };
                        const auto idle = render(false), focused = render(true);
                        int changed = 0;
                        for (int y = 0; y < idle.getHeight(); ++y)
                            for (int x = 0; x < idle.getWidth(); ++x)
                                if (idle.getPixelAt(x, y) != focused.getPixelAt(x, y))
                                {
                                    ++changed;
                                    const juce::Point<float> logical((x + 0.5f) / scale, (y + 0.5f) / scale);
                                    require(bounds.contains(logical), "focus pixels stay inside the control hit bounds");
                                    require(!bounds.reduced(6.0f).contains(logical),
                                            "focus does not paint over the central text or icon region");
                                }
                        require(enabled ? changed > width : changed == 0,
                                "enabled focus is visible on normal/selected fills; disabled controls never gain a ring");
                        if (output.isNotEmpty() && width == 96 && scale == 2.0f && enabled)
                        {
                            const juce::File directory(output);
                            require(directory.createDirectory().wasOk(), "create focus render directory");
                            const auto name = juce::String(theme == ModernTheme::light ? "light" : "night")
                                + (selected ? "-selected-focus.png" : "-normal-focus.png");
                            auto stream = directory.getChildFile(name).createOutputStream();
                            require(stream != nullptr && stream->setPosition(0) && stream->truncate().wasOk()
                                && juce::PNGImageFormat().writeImageToStream(focused, *stream), "write focus painter evidence");
                        }
                        ++cases;
                    }
    RendererVisualStyle::setModernTheme(savedTheme);
    std::cout << "ModernFocusOutlineTests passed (" << cases
              << " painter states; native keyboard traversal remains separate)\n";
}

void runTypedTokenAndAssetTest()
{
    using Contract = singleeditor::ModernNorthStarVisualContract;
    using Asset = singleeditor::ModernNorthStarAssetId;

    require(Contract::Foundation::backgroundArgb == 0xfff4f0e7
                && Contract::Semantic::focusArgb == Contract::Foundation::accentArgb
                && Contract::Component::waveformLaneArgb[0] == Contract::Foundation::accentArgb
                && Contract::Component::meterFillArgb == Contract::Foundation::accentArgb
                && Contract::Spacing::standardGap == 10
                && Contract::Radius::mag == 4.0f
                && Contract::Elevation::settledShadowAlpha == 0.0f
                && Contract::Motion::postCoreDurationMs == 140.0f,
            "foundation, semantic, spacing, radius, elevation and motion layers are typed and locked");

    constexpr std::array<Asset, 21> coreAssets {
        Asset::openFiles, Asset::remove, Asset::preferences, Asset::save,
        Asset::skipBack, Asset::previousFrame, Asset::nextFrame, Asset::play,
        Asset::pause, Asset::stop, Asset::toggle, Asset::loop, Asset::fadeIn,
        Asset::fadeOut, Asset::undo, Asset::redo, Asset::reset, Asset::zoomIn,
        Asset::zoomOut, Asset::waveform, Asset::spectrum
    };
    for (const auto id : coreAssets)
    {
        const auto token = Contract::assetToken(id);
        require(token.id == id
                    && token.owner == singleeditor::ModernNorthStarAssetOwner::core
                    && token.fileName[0] != '\0'
                    && Contract::assetIdForFileName(token.fileName) == id,
                "every approved core asset has a typed, reversible renderer mapping");
        const auto bytes = singleeditor::coreActionIconBytes(id);
        require(bytes.data != nullptr && bytes.size > 0, "every approved Core icon is available without a file lookup");
        const auto drawable = juce::Drawable::createFromImageData(bytes.data, static_cast<size_t>(bytes.size));
        require(drawable != nullptr && !drawable->getDrawableBounds().isEmpty(),
                "every embedded Core SVG decodes to a drawable with real geometry");
    }
    require(singleeditor::coreActionIconBytes(Asset::unknown).data == nullptr,
            "unknown icon identifiers do not resolve a resource");
    require(Contract::assetIdForFileName("unapproved-runtime-icon.svg") == Asset::unknown,
            "unapproved string assets cannot silently enter the Modern renderer");
}

void runResponsivePolicyTest()
{
    using Contract = singleeditor::ModernNorthStarVisualContract;
    using Density = singleeditor::ModernNorthStarDensity;
    using Size = singleeditor::ModernNorthStarMagSize;
    using Tier = singleeditor::ModernNorthStarInformationTier;

    require(Contract::resolvedInformationTier(Density::expanded, Size::large, { 279, 170 })
                == Tier::standard,
            "a large Mag cannot claim detail one pixel below its real width sweet spot");
    require(Contract::resolvedInformationTier(Density::expanded, Size::large, { 280, 170 })
                == Tier::detail,
            "a large Mag exposes detail exactly at its typed content sweet spot");
    require(Contract::resolvedInformationTier(Density::compact, Size::large, { 800, 600 })
                == Tier::summary,
            "global Compact remains a truthful summary tier even in generous geometry");
    require(!Contract::sweetSpotSatisfied(Size::medium, { 149, 90 })
                && Contract::sweetSpotSatisfied(Size::medium, { 150, 90 }),
            "responsive sweet spots use the content rectangle rather than window labels");
}

void runMotionLifecycleTest()
{
    singleeditor::ModernNorthStarMotionController motion;
    motion.start(0.0f, 1.0f, true, false);
    require(motion.isActive() && motion.value() == 0.0f,
            "post-Core motion starts without claiming completion");
    motion.advance(70.0f);
    const auto interruptedValue = motion.value();
    require(interruptedValue > 0.0f && interruptedValue < 1.0f,
            "the bounded transition has an observable intermediate state");
    motion.interrupt(0.25f, true, false);
    require(motion.isActive() && motion.value() == interruptedValue,
            "interruption preserves the current visual state without a jump");
    motion.advance(200.0f);
    require(!motion.isActive() && motion.value() == 0.25f,
            "a settled transition owns no residual active state");
    motion.start(0.0f, 1.0f, true, true);
    require(!motion.isActive() && motion.value() == 1.0f,
            "Reduce Motion applies the truthful target directly");
    motion.start(0.0f, 1.0f, false, false);
    require(!motion.isActive() && motion.value() == 1.0f,
            "motion-off applies the truthful target directly");
}

void runWaveformLaneControlsTest()
{
    using singleeditor::WaveformLaneControls;
    for (const float width : { 48.0f, 112.0f, 500.0f })
        for (const float height : { 12.0f, 24.0f, 80.0f, 320.0f })
        {
            const juce::Rectangle<float> lane(19.0f, 37.0f, width, height);
            const auto controls = WaveformLaneControls::calculate(lane, true);
            for (const auto bounds : { controls.label, controls.solo, controls.mute })
                require(bounds.isEmpty() || lane.contains(bounds),
                        "lane controls stay inside short and narrow channel lanes");
            require(controls.solo.getHeight() <= 20.0f && controls.mute.getHeight() <= 20.0f,
                    "Solo and Mute do not stretch into tall waveform-covering bars");
            require(!controls.label.intersects(controls.solo) && !controls.solo.intersects(controls.mute),
                    "channel selection and Solo/Mute targets do not overlap");
            if (height >= 80.0f)
                require(!controls.solo.contains(juce::Point<float>(controls.solo.getCentreX(), lane.getCentreY())),
                        "a selection drag through the old tall Solo area remains a waveform gesture");
        }
    const auto classic = WaveformLaneControls::calculate({ 0, 0, 500, 100 }, false);
    require(classic.label == juce::Rectangle<float>(0, 0, 50, 100)
                && classic.solo == juce::Rectangle<float>(53, 18, 20, 64),
            "Classic channel controls retain their previous geometry");
    const auto modern = WaveformLaneControls::calculate({ 0, 0, 500, 100 }, true);
    for (const auto text : { "Ch 1 L", "Ch 2 R", "Ch 12 Ltf", "Ch 64 Lss", "Ch 128" })
        require(juce::GlyphArrangement::getStringWidth(singleeditor::ModernTypography::font(11.0f), text)
                    <= modern.label.getWidth() - 6,
                "common source indices and speaker roles fit their full em label without touching Solo/Mute");
}

void runModernTransportLayoutTest()
{
    using singleeditor::ModernTransportLayout;
    for (int width = 640; width <= 2560; ++width)
    {
        const auto layout = ModernTransportLayout::calculate(width);
        const juce::Rectangle<int> surface(0, 0, width, layout.height);
        std::vector<juce::Rectangle<int>> controls {
            layout.open, layout.clock, layout.more, layout.undo, layout.redo,
            layout.meterMode, layout.meter, layout.peak, layout.monitorLabel, layout.monitor,
            layout.generalScene, layout.simpleView, layout.lightTheme, layout.nightTheme
        };
        controls.insert(controls.end(), layout.transport.begin(), layout.transport.end());
        for (size_t i = 0; i < controls.size(); ++i)
        {
            require(surface.contains(controls[i]) && controls[i].getHeight() == 32
                        && controls[i].getWidth() >= 32,
                    "all transport controls retain a usable size inside width " + std::to_string(width));
            for (size_t j = i + 1; j < controls.size(); ++j)
                require(!controls[i].intersects(controls[j]),
                        "transport/time/meter/monitor groups must not overlap at width " + std::to_string(width));
        }
        require(layout.clock.getWidth() == 156 && layout.monitor.getWidth() == 96,
                "time and monitor retain their reserved widths across the breakpoint at " + std::to_string(width));
        const auto twoRows = layout.open.getY() != layout.monitor.getY();
        require((twoRows && layout.open.getY() == 10 && surface.getBottom() - layout.monitor.getBottom() == 10)
                    || (!twoRows && layout.open.getY() == 12 && surface.getBottom() - layout.open.getBottom() == 12),
                "single and double rows preserve equal top and bottom breathing room");
        require(layout.generalScene.getY() == layout.simpleView.getY()
                    && layout.lightTheme.getY() == layout.nightTheme.getY(),
                "scene and theme pairs never break across rows");
        if (width >= 706)
            require(layout.generalScene.getY() == layout.clock.getY(),
                    "scenes stay beside playback whenever the whole group fits");
    }

    using TimeLabel = singleeditor::ModernTransportTimeLabel;
    for (const auto text : { "00:01:23:14", "123:59:59:29" })
    {
        const auto parts = TimeLabel::splitTimecode(text);
        require(parts.has_value() && parts->main + parts->frames == text,
                "visual timecode tiers preserve every original digit and separator");
    }
    for (const auto text : { "Loading song.wav", "BNK Preview Stopped 00:00:00:00", "00:01:23", "00::00:12", "" })
        require(!TimeLabel::splitTimecode(text), "non-timecode status uses the original Label presentation");

    singleeditor::ModernAppleLookAndFeel lookAndFeel;
    juce::Image menuHeader(juce::Image::RGB, 180, 32, false);
    {
        juce::Graphics g(menuHeader);
        g.fillAll(singleeditor::ModernAppleLookAndFeel::panelBackground());
        lookAndFeel.drawPopupMenuSectionHeader(g, menuHeader.getBounds(), "File");
    }
    int darkHeaderPixels = 0;
    for (int y = 0; y < menuHeader.getHeight(); ++y)
        for (int x = 0; x < menuHeader.getWidth(); ++x)
            if (menuHeader.getPixelAt(x, y).getBrightness() < 0.6f) ++darkHeaderPixels;
    require(darkHeaderPixels > 10, "Light popup section headers remain readable on the real painted surface");
    ControlSurface surface;
    surface.setLookAndFeel(&lookAndFeel);
    singleeditor::CardSurface chrome;
    chrome.getProperties().set("glassChrome", true);
    surface.addAndMakeVisible(chrome);
    std::array<singleeditor::ModernIconButton, 8> buttons;
    constexpr std::array<singleeditor::RendererAssetId, 8> assets {
        singleeditor::RendererAssetId::skipBack, singleeditor::RendererAssetId::previousFrame,
        singleeditor::RendererAssetId::nextFrame, singleeditor::RendererAssetId::play,
        singleeditor::RendererAssetId::pause, singleeditor::RendererAssetId::stop,
        singleeditor::RendererAssetId::toggle, singleeditor::RendererAssetId::loop
    };
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        buttons[i].getProperties().set("glassControl", true);
        buttons[i].setIconName(singleeditor::RendererVisualStyle::assetToken(assets[i]).fileName);
        surface.addAndMakeVisible(buttons[i]);
    }
    buttons[4].setEnabled(false);
    buttons[7].setToggleState(true, juce::dontSendNotification);
    singleeditor::ModernIconButton open, undo, redo;
    open.setIconName(singleeditor::RendererVisualStyle::assetToken(singleeditor::RendererAssetId::openFiles).fileName);
    open.getProperties().set("glassControl", true);
    undo.setIconName(singleeditor::RendererVisualStyle::assetToken(singleeditor::RendererAssetId::undo).fileName);
    redo.setIconName(singleeditor::RendererVisualStyle::assetToken(singleeditor::RendererAssetId::redo).fileName);
    juce::TextButton more("More");
    juce::TextButton general("General"), dsp("DSP");
    general.setToggleState(true, juce::dontSendNotification);
    singleeditor::ModernTransportTimeLabel clock;
    clock.setModernPresentation(true);
    singleeditor::ModernThemeButton light(false), night(true);
    light.setToggleState(!singleeditor::RendererVisualStyle::isNight(), juce::dontSendNotification);
    night.setToggleState(singleeditor::RendererVisualStyle::isNight(), juce::dontSendNotification);
    singleeditor::ModernTransportMeterLabel peak;
    juce::Label monitorLabel, meter;
    clock.setText("00:01:23:14", juce::dontSendNotification);
    clock.setFont(juce::Font(juce::FontOptions(18.0f)));
    clock.setColour(juce::Label::textColourId, singleeditor::ModernAppleLookAndFeel::accentBlue());
    peak.setText("TP -120.0 dBTP", juce::dontSendNotification);
    monitorLabel.setText("Monitor", juce::dontSendNotification);
    monitorLabel.setFont(singleeditor::ModernTypography::font(11.0f));
    monitorLabel.setBorderSize(juce::BorderSize<int>(0, 2, 0, 2));
    meter.setText("L / R", juce::dontSendNotification);
    juce::ComboBox mode;
    mode.addItem("TP", 1);
    mode.addItem("RMS", 2);
    mode.addItem("LUFS", 3);
    mode.setSelectedId(1, juce::dontSendNotification);
    mode.setSize(52, 32);
    std::cout << "Monitor width=" << juce::GlyphArrangement::getStringWidth(monitorLabel.getFont(), "Monitor") << " available=44\n";
    require(juce::GlyphArrangement::getStringWidth(monitorLabel.getFont(), "Monitor") <= 44.0f,
            "Monitor remains legible at its full em size in the production 48 px label");
    for (const auto meterText : { "-120.0 dBTP", "-120.0 dBFS", "-18.4", "+12.3 dBTP" })
        require(juce::GlyphArrangement::getStringWidth(singleeditor::ModernTypography::font(11.0f), meterText) <= 72.0f,
                "full numeric meter value and unit fit the production readout without ellipsis");
    juce::Label modeText;
    lookAndFeel.positionComboBoxText(mode, modeText);
    std::cout << "Compact combo font height=" << modeText.getFont().getHeight()
              << " em=" << modeText.getFont().getHeightInPoints()
              << " LUFS width=" << juce::GlyphArrangement::getStringWidth(modeText.getFont(), "LUFS")
              << " available=" << modeText.getWidth() - modeText.getBorderSize().getLeftAndRight() << '\n';
    for (const auto name : { "TP", "RMS", "LUFS" })
        require(juce::GlyphArrangement::getStringWidth(modeText.getFont(), name)
                    <= modeText.getWidth() - modeText.getBorderSize().getLeftAndRight(),
                "every meter mode fits the real compact combo label without ellipsis");
    juce::Slider monitor;
    monitor.setRange(0, 100);
    monitor.setValue(80);
    monitor.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    for (auto* child : std::initializer_list<juce::Component*> {
             &open, &more, &general, &dsp, &light, &night, &undo, &redo, &clock, &peak, &monitorLabel, &meter, &mode, &monitor })
        surface.addAndMakeVisible(*child);

    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_MODERN_OFFSCREEN_DIR", {});
    if (output.isNotEmpty())
        require(juce::File(output).createDirectory().wasOk(), "offscreen artifact directory is writable");
    for (const int width : { 640, 705, 706, 772, 783, 784, 1223, 1224, 1672 })
    {
        const auto layout = ModernTransportLayout::calculate(width);
        surface.setSize(width, layout.height);
        mode.setSelectedId(width == 640 ? 3 : width == 706 ? 2 : 1, juce::dontSendNotification);
        peak.setText(width == 640 ? "LUFS -18.4" : width == 706 ? "RMS -20.3 dBFS" : "TP -120.0 dBTP",
                     juce::dontSendNotification);
        chrome.setBounds(surface.getLocalBounds());
        open.setBounds(layout.open); more.setBounds(layout.more);
        general.setBounds(layout.generalScene); dsp.setBounds(layout.simpleView);
        light.setBounds(layout.lightTheme); night.setBounds(layout.nightTheme);
        undo.setBounds(layout.undo); redo.setBounds(layout.redo);
        clock.setBounds(layout.clock); mode.setBounds(layout.meterMode);
        meter.setBounds(layout.meter); peak.setBounds(layout.peak);
        monitorLabel.setBounds(layout.monitorLabel); monitor.setBounds(layout.monitor);
        for (size_t i = 0; i < buttons.size(); ++i) buttons[i].setBounds(layout.transport[i]);
        for (const float scale : { 1.0f, 1.5f, 2.0f })
        {
            const auto snapshot = surface.createComponentSnapshot(surface.getLocalBounds(), true, scale);
            require(snapshot.isValid() && snapshot.getWidth() == juce::roundToInt(width * scale)
                        && snapshot.getHeight() == juce::roundToInt(layout.height * scale),
                    "real JUCE controls render offscreen at 100/150/200 percent scale");
            require(snapshot.getPixelAt(0, 0) == singleeditor::ModernAppleLookAndFeel::windowBackground(),
                    "offscreen render uses the Modern Glass surface");
            if (output.isNotEmpty())
            {
                const auto file = juce::File(output).getChildFile(juce::String(singleeditor::RendererVisualStyle::isNight() ? "night-toolbar-" : "light-toolbar-") + juce::String(width)
                    + "-" + juce::String(juce::roundToInt(scale * 100)) + ".png");
                auto stream = file.createOutputStream();
                require(stream != nullptr && stream->setPosition(0)
                            && juce::PNGImageFormat().writeImageToStream(snapshot, *stream)
                            && stream->truncate().wasOk(),
                        "offscreen component screenshot is written");
            }
        }
    }
    surface.setLookAndFeel(nullptr);

    ControlSurface statusSurface;
    statusSurface.setLookAndFeel(&lookAndFeel);
    singleeditor::ModernStatusLabel file, status, details;
    const juce::String fullDetails = "Cursor: 00:01:23:14 | Start: 00:00:00:00 | Monitor 100% | TP -120.0 dBTP | Undo -- | Redo --";
    for (auto* label : { &file, &status, &details })
    {
        label->setModernPresentation(true);
        label->setColour(juce::Label::textColourId, singleeditor::RendererVisualStyle::secondaryText());
        statusSurface.addAndMakeVisible(*label);
    }
    file.setText("Modified | Song.wav", juce::dontSendNotification);
    details.setJustificationType(juce::Justification::centredRight);
    details.setText(fullDetails, juce::dontSendNotification);
    details.setModernText("Monitor 100%");
    require(details.textForPresentation() == "Monitor 100%"
                && details.getText() == fullDetails && details.getTooltip() == fullDetails,
            "concise Modern status keeps complete original source and tooltip text");
    details.setModernPresentation(false);
    require(details.textForPresentation() == fullDetails, "startup status retains the original full text");
    details.setModernPresentation(true);
    details.setModernText({});
    require(details.textForPresentation() == fullDetails, "loading/BNK can restore the original status presentation");
    for (const int width : { 640, 772, 1224, 1672 })
    {
        const auto layout = singleeditor::ModernStatusLayout::calculate(width);
        statusSurface.setSize(width, singleeditor::ModernStatusLayout::height);
        file.setBounds(layout.file); status.setBounds(layout.status); details.setBounds(layout.details);
        require(statusSurface.getLocalBounds().contains(layout.file)
                    && statusSurface.getLocalBounds().contains(layout.status)
                    && statusSurface.getLocalBounds().contains(layout.details)
                    && !layout.file.intersects(layout.status) && !layout.status.intersects(layout.details),
                "status regions remain separate and inside the 28 px native footer");
        for (const auto compact : { "Monitor 100%", "Waveform tail loading" })
            require(juce::GlyphArrangement::getStringWidth(singleeditor::ModernTypography::font(11.0f), compact)
                        <= layout.details.getWidth() - 4,
                    "current monitor/loading status fits without compressing glyphs");
        int sample = 0;
        for (const auto message : { "96.0 kHz / Stereo / 16-bit",
                                   "Workspace preset is corrupt; using the safe default layout.",
                                   "Save failed: target is read-only; original audio preserved." })
        {
            status.setText(message, juce::dontSendNotification);
            details.setModernText(sample == 0 ? "Waveform tail loading" : "Monitor 100%");
            require(status.textForPresentation() == message && status.getTooltip() == message,
                    "critical statuses are not replaced by the concise transport summary");
            const auto snapshot = statusSurface.createComponentSnapshot(statusSurface.getLocalBounds(), true, 2.0f);
            require(snapshot.isValid() && snapshot.getHeight() == 56, "native status labels render at 200 percent");
            if (output.isNotEmpty())
            {
                const auto filePath = juce::File(output).getChildFile(juce::String(singleeditor::RendererVisualStyle::isNight() ? "night-status-" : "light-status-")
                    + juce::String(width) + "-" + juce::String(sample) + ".png");
                auto stream = filePath.createOutputStream();
                require(stream != nullptr && stream->setPosition(0)
                            && juce::PNGImageFormat().writeImageToStream(snapshot, *stream) && stream->truncate().wasOk(),
                        "actual status label evidence is written");
            }
            ++sample;
        }
    }
    statusSurface.setLookAndFeel(nullptr);
}

void runDeferredIconPreparationTest()
{
    struct CountedRectangle final : juce::DrawableRectangle
    {
        CountedRectangle(int& copiesToUse, juce::Colour colour) : copies(copiesToUse)
        {
            setRectangle(juce::Rectangle<float>(0.0f, 0.0f, 16.0f, 16.0f));
            setFill(juce::FillType(colour));
        }
        std::unique_ptr<juce::Drawable> createCopy() const override
        {
            ++copies;
            return juce::DrawableRectangle::createCopy();
        }
        int& copies;
    };

    int copies = 0;
    singleeditor::ModernIconButton button;
    button.setSize(32, 32);
    button.setIconDrawable(std::make_unique<CountedRectangle>(copies, juce::Colours::red));
    button.setTintColours(juce::Colours::white, juce::Colours::grey);
    require(copies == 0, "hidden icon setup does not clone drawables before any paint");
    auto first = button.createComponentSnapshot(button.getLocalBounds());
    require(copies == 2 && first.getPixelAt(16, 16) == juce::Colours::red,
            "first paint prepares both icon states and includes the complete glyph immediately");
    button.setEnabled(false);
    auto disabled = button.createComponentSnapshot(button.getLocalBounds());
    require(copies == 2 && disabled.getPixelAt(16, 16) == first.getPixelAt(16, 16),
            "enablement reuses prepared custom icon states without extra copies");
    button.setIconDrawable(std::make_unique<CountedRectangle>(copies, juce::Colours::blue));
    button.setIconDrawable(std::make_unique<CountedRectangle>(copies, juce::Colours::green));
    require(copies == 2, "unpainted replacements coalesce without preparing discarded icons");
    auto replaced = button.createComponentSnapshot(button.getLocalBounds());
    require(copies == 4 && replaced.getPixelAt(16, 16) == juce::Colours::green,
            "the next paint shows the latest replacement with no stale or empty frame");
    button.setIconDrawable(nullptr);
    auto cleared = button.createComponentSnapshot(button.getLocalBounds());
    require(copies == 4 && cleared.getPixelAt(16, 16).isTransparent(),
            "clearing an icon removes its previous pixels without preparing resources");
}

void runThemeSwitchTest()
{
    using namespace singleeditor;
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    ModernAppleLookAndFeel lookAndFeel;
    ControlSurface root;
    root.setLookAndFeel(&lookAndFeel);
    root.setSize(420, 100);
    juce::Label label;
    label.setText("Selection: 0:45.123 - 1:52.789", juce::dontSendNotification);
    ModernAppleLookAndFeel::setLabelRole(label, ModernAppleLookAndFeel::LabelRole::secondary);
    ModernIconButton button;
    button.setIconName(RendererVisualStyle::assetToken(RendererAssetId::play).fileName);
    button.getProperties().set("glassControl", true);
    button.setToggleState(true, juce::dontSendNotification);
    juce::ComboBox combo;
    combo.addItem("TP", 1); combo.addItem("PCM", 2);
    combo.setSelectedId(2, juce::dontSendNotification);
    juce::TextEditor editor;
    for (auto* child : std::initializer_list<juce::Component*> { &label, &button, &combo, &editor })
        root.addAndMakeVisible(*child);
    editor.setText("Uncommitted input");
    label.setBounds(6, 6, 400, 24); button.setBounds(6, 40, 36, 32);
    combo.setBounds(50, 40, 96, 32); editor.setBounds(156, 40, 240, 32);
    const auto light = root.createComponentSnapshot(root.getLocalBounds());

    RendererVisualStyle::setModernTheme(ModernTheme::night);
    ModernAppleLookAndFeel::refreshTheme(root);
    const auto night = root.createComponentSnapshot(root.getLocalBounds());
    require(night.getPixelAt(0, 0) == RendererVisualStyle::windowBackground()
                && night.getPixelAt(0, 0).getBrightness() < 0.2f,
            "Night repaints the existing component tree with the dark surface");
    require(label.findColour(juce::Label::textColourId) == RendererVisualStyle::secondaryText()
                && editor.findColour(juce::TextEditor::textColourId) == RendererVisualStyle::primaryText()
                && lookAndFeel.findColour(juce::PopupMenu::textColourId) == RendererVisualStyle::primaryText(),
            "existing labels, editors and popup text rebind with the theme");
    require(combo.getSelectedId() == 2 && button.getToggleState() && editor.getText() == "Uncommitted input",
            "theme refresh preserves selected controls and uncommitted text");
    // Colour IDs can update while JUCE's stored text runs retain the old ink.
    // Compare actual glyph pixels with text created under the current theme.
    juce::TextEditor freshEditor;
    freshEditor.setLookAndFeel(&lookAndFeel);
    freshEditor.setSize(editor.getWidth(), editor.getHeight());
    freshEditor.setText(editor.getText());
    const auto existingInk = editor.createComponentSnapshot(editor.getLocalBounds());
    const auto freshInk = freshEditor.createComponentSnapshot(freshEditor.getLocalBounds());
    for (int y = 3; y < existingInk.getHeight() - 3; ++y)
        for (int x = 3; x < existingInk.getWidth() - 3; ++x)
            require(existingInk.getPixelAt(x, y) == freshInk.getPixelAt(x, y),
                    "existing editor glyphs match text created in Night without retyping");
    freshEditor.setLookAndFeel(nullptr);
    require(night.getPixelAt(24, 56) != light.getPixelAt(24, 56),
            "an already cached transport icon changes with the theme");
    int brightIconPixels = 0;
    for (int y = 48; y < 64; ++y)
        for (int x = 16; x < 32; ++x)
            if (night.getPixelAt(x, y).getBrightness() > 0.8f) ++brightIconPixels;
    require(brightIconPixels > 3,
            "Night retints the cached glyph itself, not only the button background");
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    ModernAppleLookAndFeel::refreshTheme(root);
    const auto restored = root.createComponentSnapshot(root.getLocalBounds());
    for (int y = 0; y < light.getHeight(); ++y)
        for (int x = 0; x < light.getWidth(); ++x)
            require(restored.getPixelAt(x, y) == light.getPixelAt(x, y),
                    "Light -> Night -> Light restores the same offscreen controls without stale colours");
    root.setLookAndFeel(nullptr);

    RendererSelection::resetForTests(RendererKind::classic);
    const auto classic = RendererVisualStyle::windowBackground();
    RendererVisualStyle::setModernTheme(ModernTheme::night);
    require(!RendererVisualStyle::isNight() && RendererVisualStyle::windowBackground() == classic,
            "Modern theme does not modify Classic renderer colours");
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
}

void runCoreControlRendererTest()
{
    using Contract = singleeditor::ModernNorthStarVisualContract;
    singleeditor::ModernAppleLookAndFeel::setSkinMode(
        singleeditor::ModernAppleLookAndFeel::SkinMode::full);
    singleeditor::ModernAppleLookAndFeel lookAndFeel;
    require(singleeditor::ModernAppleLookAndFeel::windowBackground().getARGB()
                == singleeditor::ModernGlassTokens::background
                && singleeditor::ModernAppleLookAndFeel::panelBackground().getARGB()
                       == singleeditor::ModernGlassTokens::surface
                && singleeditor::ModernAppleLookAndFeel::accentBlue().getARGB()
                       == singleeditor::ModernGlassTokens::accent
                && singleeditor::ModernAppleLookAndFeel::outerPadding()
                       == Contract::Spacing::outerPadding,
            "JUCE LookAndFeel maps to the approved Round 8 Light tokens and preserves spacing policy");

    ControlSurface surface;
    surface.setLookAndFeel(&lookAndFeel);
    surface.setBounds(0, 0, 980, 180);
    juce::TextButton primary("Play");
    primary.getProperties().set("accent", true);
    juce::TextButton ghost("Undo");
    ghost.getProperties().set("ghostButton", true);
    juce::ToggleButton toggle("Reduce Motion");
    juce::ComboBox combo;
    combo.addItem("Standard", 1);
    combo.setSelectedId(1, juce::dontSendNotification);
    juce::TextEditor editor;
    editor.setText("Modern Glass");
    for (auto* component : std::array<juce::Component*, 5> { &primary, &ghost, &toggle, &combo, &editor })
        surface.addAndMakeVisible(*component);
    primary.setBounds(10, 20, 120, 34);
    ghost.setBounds(140, 20, 120, 34);
    toggle.setBounds(270, 20, 180, 34);
    combo.setBounds(460, 20, 180, 34);
    editor.setBounds(650, 20, 280, 34);

    for (auto index = 0; index < surface.getNumChildComponents(); ++index)
        require(surface.getLocalBounds().contains(surface.getChildComponent(index)->getBounds()),
                "representative core controls remain inside the 980 px Compact surface");
    const auto snapshot100 = surface.createComponentSnapshot(surface.getLocalBounds(), true, 1.0f);
    const auto snapshot150 = surface.createComponentSnapshot(surface.getLocalBounds(), true, 1.5f);
    require(snapshot100.isValid() && snapshot100.getWidth() == 980 && snapshot100.getHeight() == 180
                && snapshot150.isValid() && snapshot150.getWidth() == 1470 && snapshot150.getHeight() == 270,
            "core controls paint at 100 and 150 percent DPI without clipping or renderer fallback");
    surface.setLookAndFeel(nullptr);
}

void runModernPopupPresentationTest()
{
    using namespace singleeditor;
    ModernAppleLookAndFeel lookAndFeel;
    juce::ComboBox box;
    juce::Label label;
    box.addItem("Flat", 1);
    box.addItem("Vocal Clarity", 2);
    box.setSelectedId(2, juce::dontSendNotification);
    label.setSize(1400, 22);
    box.setSize(1444, 26);
    const auto options = lookAndFeel.getOptionsForComboBoxPopupMenu(box, label);
    require(options.getMinimumWidth() <= 360 && options.getTargetComponent() == &box
                && options.getInitiallySelectedItemId() == 2 && options.getItemThatMustBeVisible() == 2,
            "wide popup does not inherit the oversized control, while its anchor and selected item survive");
    int longWidth = 0, rowHeight = 0;
    lookAndFeel.getIdealPopupMenuItemSize("A very long audio device name with an important channel suffix", false,
                                         30, longWidth, rowHeight);
    require(longWidth > options.getMinimumWidth() && rowHeight >= 28,
            "long item names still determine natural popup width instead of being capped by control policy");
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile("out/modern-glass-popup-renders");
    require(output.createDirectory().wasOk(), "popup render output can be created");
    int renders = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        lookAndFeel.refreshColours();
        require(!lookAndFeel.findColour(juce::PopupMenu::backgroundColourId).isOpaque(),
                "native popup peer allows transparent rounded corners");
        for (const auto width : { 160, 300, 440 })
            for (const auto scale : { 1.0f, 1.5f })
            {
                juce::Image image(juce::Image::ARGB, juce::roundToInt(width * scale), juce::roundToInt(198 * scale), true);
                {
                juce::Graphics g(image);
                g.addTransform(juce::AffineTransform::scale(scale));
                lookAndFeel.drawPopupMenuBackground(g, width, 198);
                lookAndFeel.drawPopupMenuSectionHeader(g, { 6, 6, width - 12, 26 }, "Workspace");
                const auto item = [&](int y, bool active, bool highlighted, bool ticked, bool submenu,
                                      const juce::String& text, const juce::String& shortcut)
                {
                    lookAndFeel.drawPopupMenuItem(g, { 6, y, width - 12, 30 }, false, active, highlighted,
                                                  ticked, submenu, text, shortcut, nullptr, nullptr);
                };
                item(32, true, true, true, false, "General", "Ctrl+1");
                item(62, true, false, false, false, "Command Palette...", "Ctrl+Shift+P");
                item(92, false, true, false, false, "Apply unavailable", "");
                lookAndFeel.drawPopupMenuItem(g, { 6, 122, width - 12, 9 }, true, true, false,
                                              false, false, {}, {}, nullptr, nullptr);
                item(131, true, false, false, true, "View", "");
                item(161, true, false, false, false, "A long item keeps one readable line", "");
                } // Finish the native drawing context before reading image pixels.
                const auto sample = [&](int x, int y) { return image.getPixelAt(juce::roundToInt(x * scale), juce::roundToInt(y * scale)); };
                require(sample(0, 0).getAlpha() == 0 && sample(width / 2, 9).getAlpha() > 240,
                        "rounded menu corners stay clear while its central body is opaque: corner="
                            + std::to_string(sample(0, 0).getAlpha()) + " body="
                            + std::to_string(sample(width / 2, 9).getAlpha()));
                require(sample(width - 18, 40) != sample(width - 18, 100),
                        "disabled pointer highlight cannot paint the enabled selection fill");
                const auto name = juce::String(theme == ModernTheme::night ? "night-" : "light-")
                    + juce::String(width) + "-" + juce::String(juce::roundToInt(scale * 100)) + ".png";
                auto stream = output.getChildFile(name).createOutputStream();
                require(stream != nullptr && stream->setPosition(0)
                            && juce::PNGImageFormat().writeImageToStream(image, *stream)
                            && stream->truncate().wasOk(),
                        "popup fixture written");
                ++renders;
            }
    }
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::startup);
    require(lookAndFeel.getOptionsForComboBoxPopupMenu(box, label).getMinimumWidth() == box.getWidth(),
            "startup keeps its existing lightweight popup policy");
    ModernAppleLookAndFeel::setSkinMode(ModernAppleLookAndFeel::SkinMode::full);
    RendererSelection::resetForTests(RendererKind::classic);
    require(lookAndFeel.getOptionsForComboBoxPopupMenu(box, label).getMinimumWidth() == box.getWidth(),
            "Modern popup layout does not change Classic policy");
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "ModernPopupPresentationTests passed (" << renders << " renders, anchor, selection, long names and corners)\n";
}
}

void runModernGlassTests();
void runRuntimePlaybackProbeTests();
void runModernGeneralPanelTests();
void runLocalArtworkAsyncTests();
void runTimelineMarkersTests();
void runModernLimiterPresentationTests();
void runWaveformPeakLevelTests();
void runWaveformOverviewTests();
void runWaveformPlotGeometryTests();
void runWindowChromeTests();
void runModernOutputMeterTests();
void runModernCommandPaletteTests();
void runNativeInputProbe();

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    if (argc == 2 && std::string(argv[1]) == "--native-region-outline")
    {
        singleeditor::RendererSelection::resetForTests(singleeditor::RendererKind::modern);
        runModernRegionOutlineTest();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--local-artwork-async")
    {
        runLocalArtworkAsyncTests();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--native-input-probe")
    {
        runNativeInputProbe();
        return 0;
    }
    singleeditor::RendererSelection::resetForTests(singleeditor::RendererKind::modern);
    runOptionalSkinThemeTest();
    runWindowChromeTests();
    runWaveformPeakLevelTests();
    runWaveformOverviewTests();
    runWaveformPlotGeometryTests();
    runModernWaveformPlaceholderTest();
    runTypedTokenAndAssetTest();
    runResponsivePolicyTest();
    runMotionLifecycleTest();
    runCoreControlRendererTest();
    runModernFocusOutlineTest();
    runModernAlertPresentationTest();
    runModernAlertIconPresentationTest();
    runSaveEncodingDialogPresentationTest();
    runModernPopupPresentationTest();
    runWaveformLaneControlsTest();
    for (const int extent : { 5, 12, 24, 40, 96 })
    {
        const juce::Rectangle<float> available(3, 7, static_cast<float>(extent), static_cast<float>(extent));
        const auto dial = singleeditor::ModernEqKnobPainter::dialBounds(available, 38);
        require(available.contains(dial) && dial.getWidth() <= 38,
                "Modern pearl knob adapts to its real drawing area without overflowing tiny cells");
    }
    runModernTransportLayoutTest();
    runDeferredIconPreparationTest();
    runThemeSwitchTest();
    singleeditor::RendererVisualStyle::setModernTheme(singleeditor::ModernTheme::night);
    runModernTransportLayoutTest();
    singleeditor::RendererVisualStyle::setModernTheme(singleeditor::ModernTheme::light);
    runModernGlassTests();
    runRuntimePlaybackProbeTests();
    runModernCommandPaletteTests();
    runModernGeneralPanelTests();
    runTimelineMarkersTests();
    runModernLimiterPresentationTests();
    runModernOutputMeterTests();
    runLocalArtworkAsyncTests();
    std::cout << "ModernVisualSystemTests passed\n";
    return 0;
}
