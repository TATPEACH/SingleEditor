#include "UI/Commands/CommandPaletteModel.h"
#include "UI/Commands/FocusNavigationModel.h"
#include "UI/Commands/GuiCommandModel.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace singleeditor;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

GuiCommandProvider makeCoreProvider(int& dispatchCount,
                                    GuiCommandSource& lastSource,
                                    bool& playbackEnabled)
{
    GuiCommandProvider provider;
    provider.ownerId = "core";
    provider.commands = {
        { "core.transport.toggle-playback", GuiCommandDomain::core, "Play or Pause", "Space", { "transport" } },
        { "document.save", GuiCommandDomain::document, "Save", "Ctrl+S", { "write" } }
    };
    provider.resolveState = [&playbackEnabled] (std::string_view id, const GuiCommandDispatchContext&)
    {
        if (id == "core.transport.toggle-playback")
            return GuiCommandState { playbackEnabled, false, true, "Play or Pause",
                                     playbackEnabled ? "" : "No playable audio is ready." };
        return GuiCommandState { true, false, true, "Save", {} };
    };
    provider.dispatch = [&] (std::string_view, const GuiCommandDispatchContext& context)
    {
        ++dispatchCount;
        lastSource = context.source;
        return GuiCommandResult::committed();
    };
    return provider;
}

void runRegistryContractTest()
{
    GuiCommandRegistry registry;
    registry.setActiveDocumentGeneration(7);
    int dispatchCount = 0;
    GuiCommandSource source = GuiCommandSource::shortcut;
    bool playbackEnabled = true;
    require(registry.registerProvider(makeCoreProvider(dispatchCount, source, playbackEnabled)).succeeded(),
            "a valid explicit owner provider registers");
    require(registry.providerCount() == 1 && registry.commandCount() == 2,
            "registry exposes bounded provider and command counts");
    require(GuiCommandRegistry::isValidCommandId("workspace.module.open")
                && !GuiCommandRegistry::isValidCommandId("Workspace Open"),
            "command IDs use a stable lower-case namespace");

    GuiCommandDispatchContext context { 7, GuiCommandSource::toolbar, "main", {} };
    for (const auto invocationSource : { GuiCommandSource::toolbar,
                                         GuiCommandSource::menu,
                                         GuiCommandSource::shortcut,
                                         GuiCommandSource::mag,
                                         GuiCommandSource::palette,
                                         GuiCommandSource::accessibility })
    {
        context.source = invocationSource;
        require(registry.dispatch("core.transport.toggle-playback", context).succeeded()
                    && source == invocationSource,
                "every GUI surface reaches the same command handler and context");
    }
    require(dispatchCount == 6, "surface parity dispatches exactly once per invocation");

    playbackEnabled = false;
    const auto disabled = registry.dispatch("core.transport.toggle-playback", context);
    require(disabled.status == GuiCommandStatus::disabled
                && disabled.feedback == "No playable audio is ready."
                && dispatchCount == 6,
            "one state source blocks every surface with the same disabled reason");

    context.documentGeneration = 6;
    require(registry.dispatch("document.save", context).status == GuiCommandStatus::staleGeneration,
            "old generation dispatch is rejected before its owner runs");
    context.documentGeneration = 7;
    auto cancellation = std::make_shared<GuiCommandCancellation>();
    cancellation->cancel();
    context.cancellation = cancellation;
    require(registry.dispatch("document.save", context).status == GuiCommandStatus::cancelled,
            "cancelled dispatch is rejected before its owner runs");
}

void runRegistrationAtomicityTest()
{
    GuiCommandRegistry registry;
    int count = 0;
    GuiCommandSource source {};
    bool enabled = true;
    require(registry.registerProvider(makeCoreProvider(count, source, enabled)).succeeded(),
            "fixture core provider registers");

    auto duplicate = makeCoreProvider(count, source, enabled);
    duplicate.ownerId = "workspace";
    const auto result = registry.registerProvider(std::move(duplicate));
    require(result.status == GuiCommandRegistrationStatus::duplicateCommand
                && registry.providerCount() == 1 && registry.commandCount() == 2,
            "duplicate command rejects the entire provider transaction");
    require(!registry.unregisterProvider("missing") && registry.unregisterProvider("core")
                && registry.commandCount() == 0,
            "provider ownership controls bounded unregister without an event bus");
}

void runLazyPaletteTest()
{
    GuiCommandRegistry registry;
    registry.setActiveDocumentGeneration(3);
    int count = 0;
    GuiCommandSource source {};
    bool enabled = true;
    require(registry.registerProvider(makeCoreProvider(count, source, enabled)).succeeded(),
            "palette fixture registers core commands");

    CommandPaletteModel palette;
    require(!palette.isOpen() && palette.indexSize() == 0 && palette.buildCount() == 0,
            "closed Palette owns no View-equivalent search index or startup work");
    GuiCommandDispatchContext context { 3, GuiCommandSource::palette, "palette", {} };
    palette.open(registry, context);
    const auto matches = palette.search("play");
    require(palette.isOpen() && palette.buildCount() == 1 && palette.indexSize() == 2
                && matches.size() == 1
                && matches.front().command.descriptor.id == "core.transport.toggle-playback",
            "first invocation builds a deterministic descriptor-only search index");
    require(registry.dispatch(matches.front().command.descriptor.id, context).succeeded()
                && source == GuiCommandSource::palette,
            "Palette invokes the same registered command handler");
    palette.close();
    require(!palette.isOpen() && palette.indexSize() == 0 && palette.search("save").empty(),
            "closing Palette releases optional search state immediately");

    GuiCommandProvider module;
    module.ownerId = "module-meter";
    module.commands = {
        { "module.meter.read", GuiCommandDomain::module, "Read Output Meter", {}, { "levels" } }
    };
    module.resolveState = [] (std::string_view, const GuiCommandDispatchContext&)
    {
        return GuiCommandState { true, false, true, "Read Output Meter", {} };
    };
    module.dispatch = [] (std::string_view, const GuiCommandDispatchContext&)
    {
        return GuiCommandResult::committed();
    };
    require(registry.registerProvider(std::move(module)).succeeded()
                && palette.buildCount() == 1 && palette.indexSize() == 0,
            "late lightweight module registration does not eagerly rebuild a closed Palette");
    palette.open(registry, context);
    require(palette.indexSize() == 3 && palette.search("meter").size() == 1,
            "next explicit invocation discovers the available module descriptor");
}

void runTransportRoutingTest()
{
    TransportShortcutInput input { true, true, true, false, false, ShortcutFocusContext::none };
    require(routeTransportShortcut(input) == TransportShortcutRoute::dispatchTogglePlayback,
            "title bar or empty focus routes Space to Core transport");
    input.focusContext = ShortcutFocusContext::readOnlySurface;
    require(routeTransportShortcut(input) == TransportShortcutRoute::dispatchTogglePlayback,
            "waveform and read-only surfaces route unconsumed Space to Core transport");
    for (const auto context : { ShortcutFocusContext::textEditing,
                                ShortcutFocusContext::menuOrPopup,
                                ShortcutFocusContext::modalFlow,
                                ShortcutFocusContext::shortcutCapture,
                                ShortcutFocusContext::nativeButton,
                                ShortcutFocusContext::nativeToggle,
                                ShortcutFocusContext::nativeCheckbox })
    {
        input.focusContext = context;
        require(routeTransportShortcut(input) == TransportShortcutRoute::preserveNativeMeaning,
                "exclusive and standard native controls preserve their Space meaning");
    }
    input.focusContext = ShortcutFocusContext::none;
    input.visibleModuleSuppression = true;
    require(routeTransportShortcut(input) == TransportShortcutRoute::preserveNativeMeaning,
            "a visible bounded module suppression scope wins before Core routing");
    input.visibleModuleSuppression = false;
    input.applicationActive = false;
    require(routeTransportShortcut(input) == TransportShortcutRoute::notHandled,
            "transport routing is not a system-global hotkey");
}

void runFocusFallbackTest()
{
    FocusReturnToken token { 9, 4, "workspace", "module.output-meter.read", "output-meter",
                             "stack-1", "output-meter-1", 123, false };
    FocusCandidateState candidates { true, true, true, true, true, true, true };
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::sameNativeControl,
            "a live native opener is the first focus return target");
    candidates.sameNativeControl = false;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::sameSemanticControl,
            "semantic anchor replaces an invalid weak native handle");
    candidates.sameSemanticControl = false;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::activeTabHeader,
            "tab switch returns to the new active header before hidden descendants");
    candidates.activeTabHeader = false;
    candidates.sameMagChrome = false;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::nearestReadingOrderMag,
            "removed Mag falls back to the nearest surviving reading-order Mag");
    candidates.nearestReadingOrderMag = false;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::coreWaveform,
            "workspace exhaustion falls back to the privileged waveform");
    candidates.coreWaveform = false;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates)
                == FocusRestoreTarget::minimalTransport,
            "minimal transport is the final valid focus surface");
    require(chooseFocusRestoreTarget(token, 10, 4, candidates) == FocusRestoreTarget::none,
            "old document generation token never steals focus");
    require(chooseFocusRestoreTarget(token, 9, 5, candidates) == FocusRestoreTarget::none,
            "old workspace generation token never targets a redocked or replaced View");
    token.cancelled = true;
    require(chooseFocusRestoreTarget(token, 9, 4, candidates) == FocusRestoreTarget::none,
            "cancelled command token is discarded");
}

void runNavigationAndAccessibilityBudgetTest()
{
    const std::vector<MajorRegionState> regions {
        { MajorRegion::transport, true, true },
        { MajorRegion::coreWaveform, true, true },
        { MajorRegion::workspace, false, true },
        { MajorRegion::inspector, true, false }
    };
    require(cycleMajorRegion(regions, MajorRegion::transport, false) == MajorRegion::coreWaveform
                && cycleMajorRegion(regions, MajorRegion::coreWaveform, false) == MajorRegion::transport
                && cycleMajorRegion(regions, MajorRegion::transport, true) == MajorRegion::coreWaveform,
            "F6 skips hidden/sleeping regions and cycles only the current native window");
    require(escapeOutcome(EscapeContext::dragOrResize) == EscapeOutcome::cancelPreviewTransaction
                && escapeOutcome(EscapeContext::palette) == EscapeOutcome::closeTransientSurface
                && escapeOutcome(EscapeContext::none) == EscapeOutcome::notHandled,
            "Escape cancels local/transient work and never means Remove");

    AccessibilityEventBudget budget;
    require(!budget.shouldPublish("meter", AccessibilitySemanticEvent::continuousMeter,
                                  1000.0, true, true),
            "per-frame meter changes never enter live accessibility events");
    require(!budget.shouldPublish("meter", AccessibilitySemanticEvent::clipEdge,
                                  1000.0, false, true),
            "hidden surfaces have zero accessibility event work");
    require(!budget.shouldPublish("meter", AccessibilitySemanticEvent::clipEdge,
                                  1000.0, true, false),
            "no consumer means zero high-frequency accessibility producer");
    require(budget.shouldPublish("meter", AccessibilitySemanticEvent::clipEdge,
                                 1000.0, true, true)
                && !budget.shouldPublish("meter", AccessibilitySemanticEvent::clipEdge,
                                         1200.0, true, true)
                && budget.shouldPublish("meter", AccessibilitySemanticEvent::clipEdge,
                                        1600.0, true, true),
            "semantic edges are coalesced and rate-limited on demand");
}
}

int main()
{
    try
    {
        runRegistryContractTest();
        runRegistrationAtomicityTest();
        runLazyPaletteTest();
        runTransportRoutingTest();
        runFocusFallbackTest();
        runNavigationAndAccessibilityBudgetTest();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Command/accessibility test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "Command and accessibility model tests passed.\n";
    return EXIT_SUCCESS;
}
