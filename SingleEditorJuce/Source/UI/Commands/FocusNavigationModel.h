#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace singleeditor
{
enum class ShortcutFocusContext
{
    none,
    readOnlySurface,
    textEditing,
    menuOrPopup,
    modalFlow,
    shortcutCapture,
    nativeButton,
    nativeToggle,
    nativeCheckbox
};

enum class TransportShortcutRoute { notHandled, preserveNativeMeaning, dispatchTogglePlayback };

struct TransportShortcutInput
{
    bool applicationActive = false;
    bool topLevelWindowActive = false;
    bool isSpace = false;
    bool alreadyConsumed = false;
    bool visibleModuleSuppression = false;
    ShortcutFocusContext focusContext = ShortcutFocusContext::none;
};

TransportShortcutRoute routeTransportShortcut(const TransportShortcutInput& input) noexcept;

struct FocusReturnToken
{
    std::uint64_t documentGeneration = 0;
    std::uint64_t workspaceGeneration = 0;
    std::string sourceScope;
    std::string semanticAnchor;
    std::string placementId;
    std::string tabStackId;
    std::string tabInstanceId;
    std::uintptr_t weakNativeHandle = 0;
    bool cancelled = false;
};

struct FocusCandidateState
{
    bool sameNativeControl = false;
    bool sameSemanticControl = false;
    bool activeTabHeader = false;
    bool sameMagChrome = false;
    bool nearestReadingOrderMag = false;
    bool coreWaveform = false;
    bool minimalTransport = false;
};

enum class FocusRestoreTarget
{
    none,
    sameNativeControl,
    sameSemanticControl,
    activeTabHeader,
    sameMagChrome,
    nearestReadingOrderMag,
    coreWaveform,
    minimalTransport
};

FocusRestoreTarget chooseFocusRestoreTarget(const FocusReturnToken& token,
                                            std::uint64_t activeDocumentGeneration,
                                            std::uint64_t activeWorkspaceGeneration,
                                            const FocusCandidateState& candidates) noexcept;

enum class MajorRegion { transport, coreWaveform, workspace, inspector };

struct MajorRegionState
{
    MajorRegion region = MajorRegion::transport;
    bool visible = false;
    bool focusable = false;
};

MajorRegion cycleMajorRegion(const std::vector<MajorRegionState>& regions,
                             MajorRegion current,
                             bool reverse) noexcept;

enum class EscapeContext { none, localEdit, dragOrResize, menuOrPopup, palette };
enum class EscapeOutcome { notHandled, cancelLocalEdit, cancelPreviewTransaction, closeTransientSurface };

EscapeOutcome escapeOutcome(EscapeContext context) noexcept;

enum class AccessibilitySemanticEvent
{
    transportEdge,
    selectionChanged,
    clipEdge,
    taskCompleted,
    taskFailed,
    error,
    continuousPlayback,
    continuousMeter
};

class AccessibilityEventBudget final
{
public:
    bool shouldPublish(const std::string& surfaceId,
                       AccessibilitySemanticEvent event,
                       double nowMs,
                       bool surfaceVisible,
                       bool hasConsumer);
    void reset();

private:
    struct PublishedEvent
    {
        std::string surfaceId;
        AccessibilitySemanticEvent event = AccessibilitySemanticEvent::transportEdge;
        double timestampMs = 0.0;
    };

    std::vector<PublishedEvent> history;
};
} // namespace singleeditor
