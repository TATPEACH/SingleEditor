#include "UI/Commands/FocusNavigationModel.h"

#include <algorithm>
#include <cstddef>

namespace singleeditor
{
TransportShortcutRoute routeTransportShortcut(const TransportShortcutInput& input) noexcept
{
    if (!input.applicationActive || !input.topLevelWindowActive || !input.isSpace
        || input.alreadyConsumed)
    {
        return TransportShortcutRoute::notHandled;
    }

    if (input.visibleModuleSuppression)
        return TransportShortcutRoute::preserveNativeMeaning;

    switch (input.focusContext)
    {
        case ShortcutFocusContext::textEditing:
        case ShortcutFocusContext::menuOrPopup:
        case ShortcutFocusContext::modalFlow:
        case ShortcutFocusContext::shortcutCapture:
        case ShortcutFocusContext::nativeButton:
        case ShortcutFocusContext::nativeToggle:
        case ShortcutFocusContext::nativeCheckbox:
            return TransportShortcutRoute::preserveNativeMeaning;
        case ShortcutFocusContext::none:
        case ShortcutFocusContext::readOnlySurface:
            return TransportShortcutRoute::dispatchTogglePlayback;
    }
    return TransportShortcutRoute::notHandled;
}

FocusRestoreTarget chooseFocusRestoreTarget(const FocusReturnToken& token,
                                            std::uint64_t activeDocumentGeneration,
                                            std::uint64_t activeWorkspaceGeneration,
                                            const FocusCandidateState& candidates) noexcept
{
    if (token.cancelled || token.documentGeneration != activeDocumentGeneration
        || token.workspaceGeneration != activeWorkspaceGeneration)
    {
        return FocusRestoreTarget::none;
    }

    if (candidates.sameNativeControl)
        return FocusRestoreTarget::sameNativeControl;
    if (candidates.sameSemanticControl)
        return FocusRestoreTarget::sameSemanticControl;
    if (candidates.activeTabHeader)
        return FocusRestoreTarget::activeTabHeader;
    if (candidates.sameMagChrome)
        return FocusRestoreTarget::sameMagChrome;
    if (candidates.nearestReadingOrderMag)
        return FocusRestoreTarget::nearestReadingOrderMag;
    if (candidates.coreWaveform)
        return FocusRestoreTarget::coreWaveform;
    if (candidates.minimalTransport)
        return FocusRestoreTarget::minimalTransport;
    return FocusRestoreTarget::none;
}

MajorRegion cycleMajorRegion(const std::vector<MajorRegionState>& regions,
                             MajorRegion current,
                             bool reverse) noexcept
{
    std::vector<MajorRegion> available;
    for (const auto& item : regions)
        if (item.visible && item.focusable)
            available.push_back(item.region);
    if (available.empty())
        return current;

    const auto found = std::find(available.begin(), available.end(), current);
    if (found == available.end())
        return reverse ? available.back() : available.front();

    const auto index = static_cast<std::size_t>(std::distance(available.begin(), found));
    if (reverse)
        return available[(index + available.size() - 1) % available.size()];
    return available[(index + 1) % available.size()];
}

EscapeOutcome escapeOutcome(EscapeContext context) noexcept
{
    switch (context)
    {
        case EscapeContext::localEdit:
            return EscapeOutcome::cancelLocalEdit;
        case EscapeContext::dragOrResize:
            return EscapeOutcome::cancelPreviewTransaction;
        case EscapeContext::menuOrPopup:
        case EscapeContext::palette:
            return EscapeOutcome::closeTransientSurface;
        case EscapeContext::none:
            return EscapeOutcome::notHandled;
    }
    return EscapeOutcome::notHandled;
}

bool AccessibilityEventBudget::shouldPublish(const std::string& surfaceId,
                                             AccessibilitySemanticEvent event,
                                             double nowMs,
                                             bool surfaceVisible,
                                             bool hasConsumer)
{
    if (!surfaceVisible || !hasConsumer
        || event == AccessibilitySemanticEvent::continuousPlayback
        || event == AccessibilitySemanticEvent::continuousMeter)
    {
        return false;
    }

    const auto highPriority = event == AccessibilitySemanticEvent::taskFailed
                           || event == AccessibilitySemanticEvent::error;
    const auto minimumInterval = highPriority ? 100.0 : 500.0;
    const auto duplicate = std::find_if(history.rbegin(), history.rend(), [&] (const PublishedEvent& item)
    {
        return item.surfaceId == surfaceId && item.event == event
            && nowMs - item.timestampMs < minimumInterval;
    });
    if (duplicate != history.rend())
        return false;

    history.push_back({ surfaceId, event, nowMs });
    if (history.size() > 64)
        history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - 64));
    return true;
}

void AccessibilityEventBudget::reset()
{
    history.clear();
}
} // namespace singleeditor
