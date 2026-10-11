#include "UI/Commands/CommandPaletteComponent.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "ModernCommandPaletteTests failed: " << message << '\n';
    std::exit(1);
}

template <typename T>
T& child(juce::Component& parent, const char* id)
{
    auto* found = dynamic_cast<T*>(parent.findChildWithID(id));
    check(found != nullptr, "the actual palette exposes its semantic controls");
    return *found;
}

class BackdropFixture final : public juce::Component
{
public:
    juce::Colour colour = juce::Colours::cornflowerblue;
    void paint(juce::Graphics& g) override
    {
        g.fillAll(RendererVisualStyle::windowBackground());
        for (int x = 0; x < getWidth(); x += 36)
        {
            g.setColour(colour.withAlpha(0.7f));
            g.fillRect(x, 0, 18, getHeight());
        }
    }
};

bool differs(const juce::Image& a, const juce::Image& b)
{
    for (int y = 8; y < a.getHeight() - 8; y += 5)
        for (int x = 8; x < a.getWidth() - 8; x += 5)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return true;
    return false;
}
}

void runModernCommandPaletteTests()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    RendererVisualStyle::setGlassMaterial(ModernGlassMaterial::liquid);
    ModernAppleLookAndFeel lookAndFeel;
    GuiCommandRegistry registry;
    GuiCommandProvider provider;
    provider.ownerId = "test.palette";
    provider.commands = {
        { "test.disabled", GuiCommandDomain::core, "Alpha unavailable", "Ctrl+Z", {} },
        { "test.enabled", GuiCommandDomain::core, "Beta enabled", "Ctrl+Shift+P", {} },
        { "test.third", GuiCommandDomain::workspace, "Gamma workspace", "", { "layout" } }
    };
    provider.resolveState = [](std::string_view id, const GuiCommandDispatchContext&)
    {
        GuiCommandState state;
        if (id == "test.disabled")
        {
            state.enabled = false;
            state.disabledReason = "No edit to undo";
        }
        return state;
    };
    provider.dispatch = [](std::string_view, const GuiCommandDispatchContext&)
    { return GuiCommandResult::committed(); };
    check(registry.registerProvider(std::move(provider)).succeeded(), "the command fixture registers normally");

    juce::Component root;
    root.setLookAndFeel(&lookAndFeel);
    root.setBounds(0, 0, 800, 550);
    root.setVisible(true);
    auto source = std::make_unique<BackdropFixture>();
    root.addAndMakeVisible(*source);
    source->setBounds(root.getLocalBounds());
    auto palette = std::make_unique<CommandPaletteComponent>(registry, GuiCommandDispatchContext{});
    root.addAndMakeVisible(*palette);
    palette->setBounds(120, 80, 560, 390);
    auto& query = child<juce::TextEditor>(*palette, "command-palette.search");
    auto& rows = child<juce::ListBox>(*palette, "command-palette.results");
    auto& run = child<juce::TextButton>(*palette, "command-palette.run");
    auto& status = child<juce::Label>(*palette, "command-palette.status");
    auto& glass = child<ModernGlassSurface>(*palette, "command-palette.material");
    std::string chosen;
    palette->onCommandChosen = [&](std::string id) { chosen = std::move(id); };
    check(rows.getListBoxModel()->getNumRows() == 3 && rows.getSelectedRow() == 0 && !run.isEnabled(), "disabled first result disables Run");
    check(status.getText() == "No edit to undo", "disabled command explains why");
    query.onReturnKey();
    check(chosen.empty(), "Return cannot invoke an unavailable command");
    query.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    check(rows.getSelectedRow() == 1 && run.isEnabled() && query.isEmpty(), "Down from the editor selects a runnable result without changing the query");
    query.onReturnKey();
    check(chosen == "test.enabled", "Return dispatches the selected command ID");
    rows.selectRow(0);
    check(!run.isEnabled(), "list selection changes update Run immediately");
    query.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    check(rows.getSelectedRow() == 0, "Up clamps at the first result");
    query.setText("layout", false);
    query.onTextChange();
    check(rows.getListBoxModel()->getNumRows() == 1 && run.isEnabled(), "search includes command keywords");
    query.setText("unmatched-xyz", false);
    query.onTextChange();
    check(rows.getListBoxModel()->getNumRows() == 0 && !run.isEnabled() && status.getText() == "0 commands", "an empty search cannot execute a stale result");
    query.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    query.setText({}, false);
    query.onTextChange();
    palette->setBackdropSource(source.get(), false);
    glass.refreshBackdropNow();
    check(!glass.hasBackdrop() && glass.captureCountForTesting() == 0, "pre-Core palette never captures a backdrop");
    palette->setBackdropSource(source.get(), true);

    // The real palette's search/list controls are siblings of its transparent
    // glass component, unlike the selection popup's direct child controls.
    const auto pointerEvent = [] (juce::Component& target)
    {
        const juce::Point<float> position { 12.0f, 8.0f };
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, {},
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, juce::Time::getCurrentTime(),
            position, juce::Time::getCurrentTime(), 0, false);
    };
    const auto savedReduceMotion = RendererVisualStyle::isReduceMotion();
    RendererVisualStyle::setReduceMotion(true);
    glass.refreshBackdropNow();
    juce::Thread::sleep(30);
    juce::Timer::callPendingTimersSynchronously();
    const auto beforeHover = palette->createComponentSnapshot(palette->getLocalBounds());
    const auto beforeHoverCaptures = glass.captureCountForTesting();
    glass.mouseEnter(pointerEvent(query));
    check(glass.hoverAmountForTesting() == 1.0f && !glass.hasScheduledUpdateForTesting(),
          "the actual sibling search field drives liquid hover under Reduce Motion");
    check(differs(beforeHover, palette->createComponentSnapshot(palette->getLocalBounds())),
          "pointer input from the real control topology changes the prepared optical pixels");
    check(glass.captureCountForTesting() == beforeHoverCaptures,
          "sibling hover does not recapture the waveform source");
    glass.mouseExit(pointerEvent(query));
    check(glass.hoverAmountForTesting() == 0.0f, "leaving the palette clears its optical hover");
    glass.mouseEnter(pointerEvent(*source));
    check(glass.hoverAmountForTesting() == 0.0f,
          "backdrop-source pointer input cannot light the foreground palette");
    glass.mouseEnter(pointerEvent(*rows.getViewport()->getViewedComponent()));
    check(glass.hoverAmountForTesting() == 1.0f, "nested list content shares the same interaction surface");
    palette->setVisible(false);
    palette->setVisible(true);
    glass.refreshBackdropNow();
    check(glass.hoverAmountForTesting() == 0.0f, "reopening the real palette does not retain its sibling hover");
    RendererVisualStyle::setReduceMotion(savedReduceMotion);

    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_PALETTE_OFFSCREEN_DIR", {});
    int renderCount = 0;
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const auto material : { ModernGlassMaterial::solid, ModernGlassMaterial::frosted, ModernGlassMaterial::liquid })
            for (const auto size : { juce::Point<int>(280, 240), {400, 320}, {560, 390}, {720, 460} })
            {
                RendererVisualStyle::setModernTheme(theme);
                RendererVisualStyle::setGlassMaterial(material);
                ModernAppleLookAndFeel::refreshTheme(root);
                palette->setBounds((800-size.x)/2, (550-size.y)/2, size.x, size.y);
                glass.refreshBackdropNow();
                check(glass.hasBackdrop() == (material != ModernGlassMaterial::solid), "solid does not retain an optical cache");
                std::array<juce::Component*, 6> controls { &query, &rows, &run, &status,
                    &child<juce::Label>(*palette, "command-palette.heading"),
                    &child<juce::TextButton>(*palette, "command-palette.close") };
                for (size_t i = 0; i < controls.size(); ++i)
                {
                    check(!controls[i]->getBounds().isEmpty() && palette->getLocalBounds().contains(controls[i]->getBounds()), "controls remain visible in compact and expanded palettes");
                    for (size_t j = i+1; j < controls.size(); ++j)
                        check(!controls[i]->getBounds().intersects(controls[j]->getBounds()), "palette controls never overlap");
                }
                check(query.findColour(juce::TextEditor::textColourId) == RendererVisualStyle::primaryText(), "the search editor follows the current theme");
                const auto captures = glass.captureCountForTesting();
                const auto rendered = root.createComponentSnapshot(root.getLocalBounds(), true, 1.5f);
                check(captures == glass.captureCountForTesting(), "rendering reuses the cache without snapshot recursion");
                if (output.isNotEmpty())
                {
                    juce::File directory(output);
                    directory.createDirectory();
                    const auto name = juce::String(theme == ModernTheme::light ? "light-" : "night-")
                        + juce::String(static_cast<int>(material)) + "-" + juce::String(size.x) + ".png";
                    juce::FileOutputStream stream(directory.getChildFile(name));
                    check(stream.openedOk() && juce::PNGImageFormat().writeImageToStream(rendered, stream), "palette render evidence writes successfully");
                }
                ++renderCount;
            }
    palette->setBounds(120, 80, 560, 390);
    for (const auto material : { ModernGlassMaterial::frosted, ModernGlassMaterial::liquid })
    {
        RendererVisualStyle::setGlassMaterial(material);
        std::array<double, 7> samples;
        for (auto& sample : samples)
        {
            const auto start = juce::Time::getHighResolutionTicks();
            glass.refreshBackdropNow();
            sample = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start) * 1000.0;
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "Palette 560x390 " << (material == ModernGlassMaterial::frosted ? "frosted" : "liquid")
                  << " capture+filter median_ms=" << samples[3] << " max_ms=" << samples.back() << '\n';
    }
    const auto before = palette->createComponentSnapshot(palette->getLocalBounds());
    source->colour = juce::Colours::orange;
    palette->invalidateBackdrop();
    glass.refreshBackdropNow();
    const auto after = palette->createComponentSnapshot(palette->getLocalBounds());
    check(differs(before, after), "the palette refracts the current sibling source, not a static mockup");
    palette->setVisible(false);
    check(!glass.hasBackdrop(), "closing visibility releases the actual nested optical cache");
    palette->setVisible(true);
    glass.refreshBackdropNow();
    check(glass.hasBackdrop(), "showing restores the allowed cache");
    // Reproduce the native core waveform: an independently owned sibling drawn
    // above the Mag host, which is absent from a snapshot of that host alone.
    auto overlay = std::make_unique<BackdropFixture>();
    root.addAndMakeVisible(*overlay);
    overlay->setBounds(240, 200, 160, 120);
    overlay->colour = juce::Colours::red;
    palette->toFront(false);
    const auto hostOnly = palette->createComponentSnapshot(palette->getLocalBounds());
    palette->setBackdropSource(source.get(), true, overlay.get());
    glass.refreshBackdropNow();
    const auto composed = palette->createComponentSnapshot(palette->getLocalBounds());
    check(differs(hostOnly, composed), "a separately hosted core surface appears in the glass");
    check(hostOnly.getPixelAt(500, 280) == composed.getPixelAt(500, 280), "overlay capture is clipped and positioned without shifting the host backdrop");
    overlay->setVisible(false);
    glass.refreshBackdropNow();
    check(!differs(hostOnly, palette->createComponentSnapshot(palette->getLocalBounds())), "a hidden core surface never appears in the sampled backdrop");
    overlay.reset();
    check(!glass.hasBackdrop(), "deleting only the overlay safely releases the composed cache");
    // Native minimisation can also release this cache from the glass timer.
    // A subsequent visible paint must recover without another mouse event or
    // explicit owner invalidation. Exercise that same empty-cache boundary.
    const auto releasedCount = glass.captureCountForTesting();
    for (int i = 0; i < 8; ++i)
        palette->createComponentSnapshot(palette->getLocalBounds());
    check(glass.captureCountForTesting() == releasedCount && glass.hasScheduledUpdateForTesting(),
          "visible paint schedules missing glass recovery without sampling inside paint");
    const auto recoveryDeadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    while (!glass.hasBackdrop() && juce::Time::getMillisecondCounterHiRes() < recoveryDeadline)
    {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    check(glass.hasBackdrop() && glass.captureCountForTesting() == releasedCount + 1
              && !glass.hasScheduledUpdateForTesting(),
          "visible cache recovery coalesces to one deferred capture and stops its timer");
    check(!differs(hostOnly, palette->createComponentSnapshot(palette->getLocalBounds())),
          "automatic recovery renders the surviving source rather than the missing overlay");
    check(glass.captureCountForTesting() == releasedCount + 1 && !glass.hasScheduledUpdateForTesting(),
          "painting recovered glass retains its cache without scheduling idle work");
    source.reset();
    check(!glass.hasBackdrop(), "source deletion releases the cache safely");
    palette->createComponentSnapshot(palette->getLocalBounds());
    check(!glass.hasScheduledUpdateForTesting(), "paint cannot recover glass without an allowed source");
    // The owner normally removes the palette while this callback is executing.
    palette->onCommandChosen = [&](std::string) { palette.reset(); };
    rows.selectRow(1);
    const auto invoke = query.onReturnKey;
    invoke();
    check(palette == nullptr, "command invocation survives owner deletion");

    RendererSelection::resetForTests(RendererKind::classic);
    CommandPaletteComponent classic(registry, {});
    check(classic.findChildWithID("command-palette.material") == nullptr, "Classic does not instantiate Modern glass");
    root.setLookAndFeel(nullptr);
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    std::cout << "ModernCommandPaletteTests passed: keyboard, disabled state, lifecycle, " << renderCount << " renders\n";
}
