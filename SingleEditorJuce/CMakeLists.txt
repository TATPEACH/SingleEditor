cmake_minimum_required(VERSION 3.24)

project(SingleEditor VERSION 0.1.0 LANGUAGES C CXX)

execute_process(COMMAND git rev-parse --verify HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE SINGLE_EDITOR_DIAGNOSTIC_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SINGLE_EDITOR_DIAGNOSTIC_COMMIT MATCHES "^[0-9a-f]+$")
    set(SINGLE_EDITOR_DIAGNOSTIC_COMMIT "not-reported")
else()
    execute_process(COMMAND git status --porcelain --untracked-files=normal WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE diagnostic_worktree_status ERROR_QUIET)
    if(NOT diagnostic_worktree_status STREQUAL "")
        string(APPEND SINGLE_EDITOR_DIAGNOSTIC_COMMIT "-dirty")
    endif()
endif()
configure_file(cmake/DiagnosticBuildIdentity.h.in generated/DiagnosticBuildIdentity.h @ONLY)

option(SINGLE_EDITOR_ENABLE_OPEN_TIMING "Enable startup/open timing instrumentation" OFF)
option(SINGLE_EDITOR_DOCUMENT_TEST_ASAN "Instrument only the Document test target with MSVC AddressSanitizer" OFF)
option(SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE "Diagnostic Release without GL, ICF or string pooling; not evidence for an existing binary" OFF)

if (SINGLE_EDITOR_ENABLE_OPEN_TIMING)
    set(SINGLE_EDITOR_PRODUCT_NAME "Single Editor Timing")
    set(SINGLE_EDITOR_BUNDLE_ID "com.singleeditor.app.timing")
else()
    set(SINGLE_EDITOR_PRODUCT_NAME "Single Editor")
    set(SINGLE_EDITOR_BUNDLE_ID "com.singleeditor.app")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE's recommended Release /Ox does not include /Gy. Keep functions and
# globals independently removable by the existing /OPT:REF and /OPT:ICF link
# settings, including in unity-built JUCE modules, without editing shared JUCE.
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Gy /Gw")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT SingleEditor)

include(FetchContent)
include(cmake/SingleEditorSampleRate.cmake)

set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)

set(SINGLE_EDITOR_JUCE_ROOT "" CACHE PATH "Optional shared JUCE source tree root")

if (SINGLE_EDITOR_JUCE_ROOT)
    get_filename_component(JUCE_LOCAL_ROOT
        "${SINGLE_EDITOR_JUCE_ROOT}"
        ABSOLUTE
    )
elseif (DEFINED ENV{SINGLE_EDITOR_JUCE_ROOT} AND NOT "$ENV{SINGLE_EDITOR_JUCE_ROOT}" STREQUAL "")
    get_filename_component(JUCE_LOCAL_ROOT
        "$ENV{SINGLE_EDITOR_JUCE_ROOT}"
        ABSOLUTE
    )
else()
    get_filename_component(JUCE_LOCAL_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../JUCE_ENV/JUCE"
        ABSOLUTE
    )
endif()

if (EXISTS "${JUCE_LOCAL_ROOT}/CMakeLists.txt")
    message(STATUS "Using local JUCE source tree: ${JUCE_LOCAL_ROOT}")
    add_subdirectory("${JUCE_LOCAL_ROOT}" "${CMAKE_BINARY_DIR}/_deps/juce-build")
    set(SINGLE_EDITOR_RESOLVED_JUCE_ROOT "${JUCE_LOCAL_ROOT}" CACHE INTERNAL
        "Actual JUCE provider used by the configured product" FORCE)
else()
    message(STATUS "Local JUCE source tree not found, falling back to GitHub archive download.")
    FetchContent_Declare(
        juce
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.12.tar.gz
        URL_HASH SHA256=dec1a8baee5aaec4502b717a421b850e257ab7e9c2efd03766185ec1957cf102
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(juce)
    set(SINGLE_EDITOR_RESOLVED_JUCE_ROOT "${juce_SOURCE_DIR}" CACHE INTERNAL
        "Actual JUCE provider used by the configured product" FORCE)
endif()

include(cmake/SingleEditorJuceGraphics.cmake)
single_editor_configure_juce_graphics()
include(cmake/SingleEditorJuceAudioDiagnostics.cmake)
single_editor_configure_juce_audio_diagnostics()
include(cmake/SingleEditorJuceWavMetadata.cmake)
single_editor_configure_juce_wav_metadata()
include(cmake/SingleEditorJuceFftRegistration.cmake)
single_editor_configure_juce_fft_registration()
include(cmake/SingleEditorAsio.cmake)

include(cmake/SingleEditorWindowShellResources.cmake)
include(cmake/SingleEditorCoreActionIcons.cmake)
include(cmake/SingleEditorAmbisonicResources.cmake)

if (WIN32)
    juce_add_gui_app(SingleEditor
        PRODUCT_NAME "${SINGLE_EDITOR_PRODUCT_NAME}"
        COMPANY_NAME "SingleEditor"
        BUNDLE_ID "${SINGLE_EDITOR_BUNDLE_ID}"
    )
else()
    juce_add_gui_app(SingleEditor
        PRODUCT_NAME "${SINGLE_EDITOR_PRODUCT_NAME}"
        COMPANY_NAME "SingleEditor"
        BUNDLE_ID "${SINGLE_EDITOR_BUNDLE_ID}"
        ICON_BIG "Resources/AppIcon.png"
        ICON_SMALL "Resources/AppIcon.png"
    )
endif()

target_include_directories(SingleEditor
    PRIVATE
        Source
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# Ordered source roles shared by product and test targets. Keep target-specific
# interleaving below: moving a role across another source changes link order.
include(cmake/SingleEditorCodeIntent.cmake)

# Definition coordinates follow the actual JUCE provider, including FetchContent.
# Each use below lists exact files and retains that unit's existing selectors.
get_target_property(intent_definition_core_sources juce_core INTERFACE_SOURCES)
foreach(intent_definition_source IN LISTS intent_definition_core_sources)
    if(intent_definition_source MATCHES "/juce_core\\.cpp$")
        get_filename_component(intent_definition_core_root "${intent_definition_source}" DIRECTORY)
        get_filename_component(intent_definition_modules_root "${intent_definition_core_root}" DIRECTORY)
    endif()
endforeach()
if(NOT intent_definition_modules_root)
    message(FATAL_ERROR "Actual JUCE module root required for included definition coordinates")
endif()


set(SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE
    Source/AudioSuite/AudioSuiteReferenceProcessor.cpp
)

set(SINGLE_EDITOR_AMBISONIC_SOURCES
    Source/Audio/AmbisonicBinauralFilterBank.cpp
    Source/Audio/AmbisonicBinauralRenderer.cpp
    Source/Audio/AmbisonicFieldAnalyzer.cpp
    Source/Audio/AmbisonicMonitorDecoder.cpp
    Source/Audio/AmbisonicStageGraph.cpp
    Source/Audio/AmbisonicSpeakerGeometry.cpp
)

single_editor_declare_code_unit(
    ID singleeditor.ambisonics.binaural-bank OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AmbisonicBinauralFilterBank.cpp
    SCOPES singleeditor::AmbisonicBinauralFilterBank TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit GUI or CLI Binaural / HRTF selection for Ambisonic audio"
    RETENTION "Document or explicit CLI inspection lease")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.binaural-renderer OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AmbisonicBinauralRenderer.cpp
    WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Prepared bank and explicit HRTF playback or CLI inspection"
    RETENTION "Published transport renderer and in-flight consumer leases, retired on control thread")
foreach(source IN ITEMS Source/Audio/AmbisonicFieldAnalyzer.cpp
        Source/Audio/AmbisonicStageGraph.cpp
        Source/Audio/AmbisonicMonitorDecoder.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.ambisonics.processing.${unit_name}" OWNER singleeditor.ambisonics
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
        TRIGGER "Admitted Ambisonic interpretation, leased field analysis/playback, or explicit CLI monitor inspection"
        RETENTION "Published graph/decoder and consumer-owned analyzer for the selected Ambisonic document")
endforeach()
# Geometry's standard catalog also serves the selected output backend, while
# profile editing and generation belong to explicit geometry actions. Keep
# these slices separate instead of classifying their mixed source as one phase.
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.geometry-catalog OWNER singleeditor.ambisonics
    LIFECYCLE BackendLazy SOURCE Source/Audio/AmbisonicSpeakerGeometry.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::AmbisonicStandardMatrixCatalog
    MEMBERS "singleeditor::`anonymous namespace'::profileFromGenerated"
        "singleeditor::`anonymous namespace'::findGeneratedGeometry"
        "singleeditor::`anonymous namespace'::findGeneratedGeometryForLayout"
        "singleeditor::`anonymous namespace'::makeCacheKey"
        "singleeditor::`anonymous namespace'::matrixHashFromPointer"
        "singleeditor::`anonymous namespace'::sha256ForUtf8"
        "singleeditor::`anonymous namespace'::profileIdentity"
        "singleeditor::`anonymous namespace'::dimensionalityToString"
        "singleeditor::`anonymous namespace'::weightPolicyToString"
    MEMBERS "singleeditor::generated::standardMatrices"
        "singleeditor::generated::standardGeometries"
    MEMBERS "singleeditor::standardLookupCount"
    TRIGGER "Selected output restoration/settings, Ambisonic decode, leased spatial layout or explicit diagnostic/CLI inspection"
    RETENTION "Immutable built-in profile/matrix views and caller-owned profile copies")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.geometry-actions OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AmbisonicSpeakerGeometry.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::SpeakerGeometryProfileCodec
        singleeditor::SpeakerGeometryValidationResult
        singleeditor::AmbisonicDecodeMatrixArtifact
        singleeditor::SpeakerGeometryGenerationResult
        singleeditor::AmbisonicSpeakerGeometryGenerator
        singleeditor::AmbisonicSpeakerGeometrySession
        "singleeditor::`anonymous-namespace'::GeometryProfileProperties"
    MEMBERS "singleeditor::`anonymous namespace'::geometryProfileProperties"
        "singleeditor::`anonymous namespace'::dimensionalityFromString"
        "singleeditor::`anonymous namespace'::weightPolicyFromString"
        "singleeditor::`anonymous namespace'::cloneObjectOrCreate"
        "singleeditor::`anonymous namespace'::addDiagnostic"
        "singleeditor::`anonymous namespace'::finite"
        "singleeditor::`anonymous namespace'::directionFor"
        "singleeditor::`anonymous namespace'::angularSeparationDegrees"
        "singleeditor::`anonymous namespace'::determinant"
        "singleeditor::`anonymous namespace'::activeDirectionalSpeakers"
    MEMBERS "singleeditor::customGenerationCount"
    TRIGGER "Explicit geometry import/export, validation/generation, audition or activation of its matrix artifact"
    RETENTION "Geometry editor/session cache and activated artifact leases, with codec properties initialized only on first read/write")

set(SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES
    Source/Audio/ChannelLayout.cpp
    Source/Audio/ChannelRouter.cpp
    Source/Audio/OutputMeterAnalyzer.cpp
)
single_editor_declare_code_unit(
    ID singleeditor.channel-layout.catalog OWNER singleeditor.channel-layout
    LIFECYCLE CoreEager SOURCE Source/Audio/ChannelLayout.cpp WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Ordinary file role interpretation, startup channel preferences and selected playback target layout"
    RETENTION "Shared role and preset descriptions. Ambisonic labels do not activate a decoder or analyzer")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.channel-routing OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE Source/Audio/ChannelRouter.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::RoutingMatrix
    MEMBERS singleeditor::ChannelRouter::buildRoutingMatrix singleeditor::ChannelRouter::outputChannelForRole
        singleeditor::ChannelRouter::outputMappingIsValid singleeditor::ChannelRouter::sourceChannelIsAudible
        singleeditor::ChannelRouter::findRole singleeditor::ChannelRouter::addGainForRole
        singleeditor::ChannelRouter::addStereoFallback singleeditor::ChannelRouter::addMonoFallback
        singleeditor::ChannelRouter::addFoldDownToTarget
    TRIGGER "Ordinary selected-output routing, source mute and solo, and role-aware fold-down"
    RETENTION "Current render matrix and stateless routing helpers reused by explicit route inspection")
single_editor_declare_code_unit(
    ID singleeditor.cli-automation.route-inspection OWNER singleeditor.cli-automation
    LIFECYCLE ModuleLazy SOURCE Source/Audio/ChannelRouter.cpp TARGETS SingleEditorCLI
    MEMBERS singleeditor::ChannelRouter::buildPhysicalOutputMatrix singleeditor::ChannelRouter::buildSilentRouteProbes
    TRIGGER "Explicit CLI RoutingInspectionService report for the selected output mapping and rendered source routes"
    RETENTION "Inspection result only. No output device activation by these matrix description functions")
single_editor_declare_code_unit(
    ID singleeditor.output-meter.analyzer OWNER singleeditor.output-meter
    LIFECYCLE ModuleLazy SOURCE Source/Audio/OutputMeterAnalyzer.cpp WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Visible post-Core meter consumer lease or explicit CLI routing inspection loudness-role query"
    RETENTION "Lease-owned analyzer filters/history, with final consumer release destroying state off the audio callback")

set(SINGLE_EDITOR_PACKED_DECODE_SOURCES
    Source/Audio/VgmstreamDecodeService.cpp
    Source/Audio/WindowsMediaAudioService.cpp
)

set(SINGLE_EDITOR_WAVEFORM_SOURCES
    Source/Waveform/WaveformModel.cpp
    Source/Waveform/WaveformPeakLevel.cpp
)

# Mixed classes use exact named-member slices. Do not classify the full waveform
# model/view or the optional lifecycle coordinator as one eager/lazy block.
single_editor_declare_code_unit(
    ID singleeditor.core-lifecycle.milestones OWNER singleeditor.core-lifecycle
    LIFECYCLE CoreEager SOURCE Source/Core/CoreLifecycleCoordinator.cpp
    MEMBERS singleeditor::CoreLifecycleCoordinator::beginGeneration
        singleeditor::CoreLifecycleCoordinator::isCurrent
        singleeditor::CoreLifecycleCoordinator::markFirstWaveform
        singleeditor::CoreLifecycleCoordinator::markPlayable
        singleeditor::CoreLifecycleCoordinator::isCoreReady
        singleeditor::CoreLifecycleCoordinator::markCoreMilestone
    TARGETS SingleEditor
    TRIGGER "Current ordinary document generation and actual waveform/playable milestones"
    RETENTION "Minimal generation and CoreReady state owned by the editor")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.appearance-factory OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/UI/Renderer/RendererLookAndFeelFactory.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Application creates the selected shared look and feel before initial native window construction"
    RETENTION "Application-owned selected look and feel without admitting optional resource preparation")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.appearance-binding OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernAppleLookAndFeel::ModernAppleLookAndFeel
        singleeditor::ModernAppleLookAndFeel::choosePreferredFont
        singleeditor::ModernAppleLookAndFeel::getTypefaceForFont
        singleeditor::ModernAppleLookAndFeel::refreshColours
        singleeditor::ModernAppleLookAndFeel::setSkinMode
        singleeditor::ModernAppleLookAndFeel::getSkinMode
        singleeditor::ModernAppleLookAndFeel::isStartupSkin
        singleeditor::ModernAppleLookAndFeel::windowBackground
        singleeditor::ModernAppleLookAndFeel::panelBackground
        singleeditor::ModernAppleLookAndFeel::elevatedPanelBackground
        singleeditor::ModernAppleLookAndFeel::workspaceSurface
        singleeditor::ModernAppleLookAndFeel::mutedSurface
        singleeditor::ModernAppleLookAndFeel::titleBarBackground
        singleeditor::ModernAppleLookAndFeel::borderColour
        singleeditor::ModernAppleLookAndFeel::primaryText
        singleeditor::ModernAppleLookAndFeel::secondaryText
        singleeditor::ModernAppleLookAndFeel::disabledText
        singleeditor::ModernAppleLookAndFeel::accentOrange
        singleeditor::ModernAppleLookAndFeel::accentBlue
        singleeditor::ModernAppleLookAndFeel::accentPurple
        singleeditor::ModernAppleLookAndFeel::waveformBlue
        singleeditor::ModernAppleLookAndFeel::waveformCyan
        singleeditor::ModernAppleLookAndFeel::ghostHoverFill
        singleeditor::ModernAppleLookAndFeel::ghostPressedFill
        singleeditor::ModernAppleLookAndFeel::cardRadius
        singleeditor::ModernAppleLookAndFeel::componentRadius
        singleeditor::ModernAppleLookAndFeel::outerPadding
        singleeditor::ModernAppleLookAndFeel::interPanelGap
        singleeditor::ModernAppleLookAndFeel::topBarHeight
        singleeditor::ModernAppleLookAndFeel::bottomBarHeight
    TRIGGER "Existing initial window and ordinary waveform need shared fonts, palette and geometry"
    RETENTION "Application appearance and passive presentation values without blanket ownership of interaction painting")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.appearance-controls OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernAppleLookAndFeel::drawButtonBackground
        "singleeditor::ModernAppleLookAndFeel::getSliderLayout"
        "singleeditor::ModernAppleLookAndFeel::drawLinearSlider"
        singleeditor::ModernAppleLookAndFeel::createFocusOutlineForComponent
        singleeditor::ModernGlassPainter::focusOutline
        singleeditor::ModernAppleLookAndFeel::drawButtonText
        singleeditor::ModernAppleLookAndFeel::getTextButtonFont
        singleeditor::ModernAppleLookAndFeel::drawToggleButton
        singleeditor::ModernAppleLookAndFeel::drawComboBox
        singleeditor::ModernAppleLookAndFeel::positionComboBoxText
        singleeditor::ModernAppleLookAndFeel::getComboBoxFont
    MEMBERS "singleeditor::ModernAppleLookAndFeel::createSliderTextBox"
    TRIGGER "Existing ordinary editor controls share the selected renderer before and after real CoreReady"
    RETENTION "Shared control paint and font layout without claiming menu, dialog or optional icon decoding")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.appearance-interaction OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernAppleLookAndFeel::drawAlertBox
        singleeditor::ModernAppleLookAndFeel::getOptionsForComboBoxPopupMenu
        singleeditor::ModernAppleLookAndFeel::getPopupMenuFont
        singleeditor::ModernAppleLookAndFeel::getPopupMenuBorderSize
        singleeditor::ModernAppleLookAndFeel::getMenuWindowFlags
        singleeditor::ModernAppleLookAndFeel::getIdealPopupMenuItemSize
        singleeditor::ModernAppleLookAndFeel::drawPopupMenuBackground
        singleeditor::ModernAppleLookAndFeel::drawPopupMenuItem
        singleeditor::ModernAppleLookAndFeel::drawPopupMenuSectionHeader
        singleeditor::ModernAppleLookAndFeel::fillTextEditorBackground
        singleeditor::ModernAppleLookAndFeel::drawTextEditorOutline
    TRIGGER "Explicit menu, editable field or dialog presentation after an editor action or reported open result"
    RETENTION "Requested interaction surface through dismissal without changing first-window appearance")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.glass-consumer OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/ModernGlassSurface.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernGlassSurface::refreshBackdropNow
        singleeditor::ModernGlassSurface::paint
        singleeditor::ModernGlassSurface::renderPreparedBackdrop
        singleeditor::ModernGlassSurface::timerCallback
        singleeditor::ModernGlassSurface::updateHover
        singleeditor::ModernGlassSurface::mouseMove
        singleeditor::ModernGlassSurface::mouseEnter
        singleeditor::ModernGlassSurface::mouseExit
        singleeditor::ModernGlassSurface::mouseDrag
        singleeditor::ModernGlassSurface::mouseDown
        singleeditor::ModernGlassSurface::mouseUp
        singleeditor::ModernGlassSurface::mouseWheelMove
    TRIGGER "Visible consumer enables sampling after CoreReady or an explicit pointer interaction requests hover state"
    RETENTION "Bounded sampled and prepared images while the visible consumer is enabled, released on hide or disable")

# Existing minimal controls are constructed and may paint before CoreReady.
# SVG decoding remains a distinct requested backend, not a whole control scope.
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.icon-controls OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/ModernIconButton.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernIconButton::ModernIconButton
        singleeditor::ModernIconButton::setIconName singleeditor::ModernIconButton::setIconDrawable
        singleeditor::ModernIconButton::setIconPadding singleeditor::ModernIconButton::setNominalIconSize
        singleeditor::ModernIconButton::setCornerRadius singleeditor::ModernIconButton::setTintColours
        singleeditor::ModernIconButton::lookAndFeelChanged singleeditor::ModernIconButton::enablementChanged
        singleeditor::ModernIconButton::refreshIcons singleeditor::ModernIconButton::paintButton
    TRIGGER "Existing initial open and transport controls bind passive state and paint the minimal editor"
    RETENTION "MainComponent-owned core controls through editor lifetime, with separately admitted drawable decoding")
single_editor_declare_code_unit(
    ID singleeditor.image-codecs.icon-drawables OWNER singleeditor.image-codecs
    LIFECYCLE BackendLazy SOURCE Source/UI/Components/ModernIconButton.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernIconButton::prepareIconsIfNeeded
        "singleeditor::`anonymous namespace'::getIconCacheLock"
        "singleeditor::`anonymous namespace'::getTintedDrawableCache"
        "singleeditor::`anonymous namespace'::colourToSvgString"
        "singleeditor::`anonymous namespace'::isShapeTag"
        "singleeditor::`anonymous namespace'::tintSvgElement"
        "singleeditor::`anonymous namespace'::createTintedSvgDrawable"
        "singleeditor::`anonymous namespace'::cloneDrawable"
        "singleeditor::`anonymous namespace'::makeTintedDrawableCacheKey"
        "singleeditor::`anonymous namespace'::createCachedTintedDrawable"
    TRIGGER "First actual glyph paint requests a built-in drawable or a supplied drawable copy"
    RETENTION "Requested per-control drawable and shared built-in tint cache, without eager preparation in setters")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.header-widgets OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::CardSurface singleeditor::ModernTransportTimeLabel
    MEMBERS singleeditor::ModernIconButton::~ModernIconButton
        singleeditor::ModernThemeButton::ModernThemeButton singleeditor::ModernThemeButton::~ModernThemeButton
    MEMBERS "singleeditor::MainComponent::SidebarResizeHandle::SidebarResizeHandle"
        "singleeditor::ModernStatusLabel::ModernStatusLabel"
    TRIGGER "MainComponent owns passive toolbar and clock objects and paints ordinary startup status and card surfaces"
    PDB_VFTABLE_CLASSES
        "singleeditor::CardSurface" ".?AVCardSurface@singleeditor@@"
        "singleeditor::ModernTransportTimeLabel" ".?AVModernTransportTimeLabel@singleeditor@@"
    RETENTION "Existing core member objects and time label with a later Modern presentation branch")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.theme-button-paint OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernThemeButton::paintButton singleeditor::ModernGlassPainter::surface
    TRIGGER "Admitted full Modern chrome shows Light and Night buttons and enables glassChrome on the action bar"
    RETENTION "Vector paint only while full chrome is presented, no backdrop sampling implied")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.static-glass-painter OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernGlassPainter::control
    TRIGGER "Existing full Modern controls outside the startup skin draw vector glass accents"
    RETENTION "Paint-stack vector geometry without sampled backdrop allocation")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.glass-binding OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/ModernGlassSurface.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ModernGlassSurface::ModernGlassSurface singleeditor::ModernGlassSurface::~ModernGlassSurface
        singleeditor::ModernGlassSurface::setBackdropSource singleeditor::ModernGlassSurface::setBackdropOverlay
        singleeditor::ModernGlassSurface::setInteractionHost singleeditor::ModernGlassSurface::setEffectsEnabled
        singleeditor::ModernGlassSurface::canSample singleeditor::ModernGlassSurface::releaseBackdrop
        singleeditor::ModernGlassSurface::invalidateBackdrop singleeditor::ModernGlassSurface::moved
        singleeditor::ModernGlassSurface::resized singleeditor::ModernGlassSurface::visibilityChanged
        singleeditor::ModernGlassSurface::parentHierarchyChanged singleeditor::ModernGlassSurface::lookAndFeelChanged
        singleeditor::ModernGlassSurface::componentMovedOrResized
        singleeditor::ModernGlassSurface::componentParentHierarchyChanged
        singleeditor::ModernGlassSurface::componentVisibilityChanged singleeditor::ModernGlassSurface::componentBeingDeleted
    TRIGGER "Passive selection surface is an ordinary editor member with effects disabled until admitted visibility"
    RETENTION "Weak component bindings and guarded invalidation or cleanup, excluding actual snapshot and blur work")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-overview OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/WaveformOverviewComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::WaveformOverviewComponent
    TRIGGER "Full Modern waveform layout outside the startup skin creates the ordinary overview and navigation controls"
    PDB_VFTABLE_CLASSES
        "singleeditor::WaveformOverviewComponent" ".?AVWaveformOverviewComponent@singleeditor@@"
    RETENTION "View-owned peak path and navigation state, released when the full Modern presentation is removed")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-accessibility OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/WaveformViewComponent.cpp TARGETS SingleEditor
    SCOPES "singleeditor::`anonymous-namespace'::WaveformAccessibilityValue"
    MEMBERS singleeditor::WaveformViewComponent::createAccessibilityHandler
        singleeditor::WaveformViewComponent::getAccessibilityValueSummary
    TRIGGER "Native accessibility requests the existing ordinary waveform group's value and actions"
    RETENTION "Waveform-owned accessibility handler with passive current document and selection facts")
single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Metadata/LocalArtworkLoader.h"
    ID singleeditor.metadata.sidebar-presentation OWNER singleeditor.metadata
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::LocalArtworkLoader
    MEMBERS singleeditor::MetadataSidebarComponent::MetadataSidebarComponent
        singleeditor::MetadataSidebarComponent::~MetadataSidebarComponent
        singleeditor::MetadataSidebarComponent::beginTypedMetadataGeneration
        singleeditor::MetadataSidebarComponent::clearTypedMetadataInspector
        singleeditor::MetadataSidebarComponent::setBnkPreviewContext
        singleeditor::MetadataSidebarComponent::paint
        singleeditor::MetadataSidebarComponent::resized
        singleeditor::MetadataSidebarComponent::refreshChannelControls
        singleeditor::MetadataSidebarComponent::layoutModernInspector
    MEMBERS "singleeditor::`anonymous namespace'::drawInspectorItems"
    TRIGGER "Empty-workspace construction or current-generation real CoreReady creates the sidebar, later generations reuse and withdraw its presentation"
    PDB_VFTABLE_CLASSES
        "singleeditor::LocalArtworkLoader" ".?AVLocalArtworkLoader@singleeditor@@"
    RETENTION "Main-view-owned sidebar borrowed by each workspace host, without admitting bank, device or binaural work on ordinary startup")
single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Components/ModernFileSummaryComponent.h"
    ID singleeditor.metadata.file-summary OWNER singleeditor.metadata
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::ModernFileSummaryComponent
    TRIGGER "The deferred metadata sidebar paints already admitted file facts after CoreReady or empty-workspace presentation"
    PDB_VFTABLE_CLASSES
        "singleeditor::ModernFileSummaryComponent" ".?AVModernFileSummaryComponent@singleeditor@@"
    RETENTION "Sidebar-owned compact file summary without a reader, artwork extractor or metadata provider")
single_editor_declare_code_unit(
    ID singleeditor.metadata.sidebar-activation OWNER singleeditor.metadata
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MetadataSidebarComponent::activateTypedMetadataInspector
        singleeditor::MetadataSidebarComponent::refreshTypedMetadataFacts
    TRIGGER "Current ordinary generation reaches real waveform and playable CoreReady, or refreshes its admitted inspector"
    RETENTION "Generation-bound typed inspector and artwork request through replacement or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.bnk.sidebar-content OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::MetadataSidebarComponent::BankInspectorContent
    MEMBERS singleeditor::MetadataSidebarComponent::refreshBankInspector
    PDB_METHODS "`singleeditor::MetadataSidebarComponent::refreshBankInspector'::`2'::<lambda_1>::operator()"
    TRIGGER "Successful explicit BNK open creates the bank inspector content"
    PDB_VFTABLE_CLASSES
        "singleeditor::MetadataSidebarComponent::BankInspectorContent::Field" ".?AUField@BankInspectorContent@MetadataSidebarComponent@singleeditor@@"
        "singleeditor::MetadataSidebarComponent::BankInspectorContent" ".?AVBankInspectorContent@MetadataSidebarComponent@singleeditor@@"
    RETENTION "Bank inspector view while bank presentation is active, released when returning to ordinary media")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.sidebar-actions OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::MetadataSidebarComponent::BinauralPreparation
    MEMBERS singleeditor::MetadataSidebarComponent::requestBinauralMonitor
        singleeditor::MetadataSidebarComponent::showAmbisonicOrientationDialog
    SCOPES "singleeditor::`anonymous-namespace'::AmbisonicOrientationComponent"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MetadataSidebarComponent::requestBinauralMonitor'::`2'::<lambda_1>,void,std::shared_ptr<singleeditor::AmbisonicBinauralFilterBank const >,juce::String>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MetadataSidebarComponent::showAmbisonicOrientationDialog'::`2'::<lambda_1>,void>::_Do_call"
    TRIGGER "Explicit Ambisonic monitor or orientation action from the metadata controls"
    PDB_VFTABLE_CLASSES
        "singleeditor::MetadataSidebarComponent::BinauralPreparation" ".?AVBinauralPreparation@MetadataSidebarComponent@singleeditor@@"
    RETENTION "Requested dialog or preparation worker with exact pending document state until completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.audio-output.sidebar-mapping OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/UI/Components/MetadataSidebarComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MetadataSidebarComponent::showOutputMappingDialog
    SCOPES "singleeditor::`anonymous-namespace'::OutputMappingMatrixComponent"
    TRIGGER "Explicit physical output mapping request for the selected output capability"
    RETENTION "Requested output mapping dialog without creating unrelated backends")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.recent-file-actions OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/Document/WorkspaceSessionSupport.cpp TARGETS SingleEditor
    MEMBERS singleeditor::WorkspaceSessionSupport::persistRecentFiles
        singleeditor::WorkspaceSessionSupport::rememberOpenedFiles
        singleeditor::WorkspaceSessionSupport::getValidRecentFiles
        singleeditor::WorkspaceSessionSupport::clearRecentFiles
        singleeditor::WorkspaceSessionSupport::getDefaultBrowseDirectory
    TRIGGER "Explicit file or recent-menu action, admitted post-Core bookkeeping or settings flush"
    RETENTION "Existing recent-file and browse-directory state without claiming constructor or initial settings reads lazy")
single_editor_declare_code_unit(
    ID singleeditor.core-file.retained-workspace-actions OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/Document/WorkspaceSessionSupport.cpp TARGETS SingleEditor
    SCOPES singleeditor::WorkspaceSessionSupport::RetainedAudioDocument
    MEMBERS singleeditor::WorkspaceSessionSupport::collectWarmableFiles
        singleeditor::WorkspaceSessionSupport::clearRetainedPlaylistAudio
        singleeditor::WorkspaceSessionSupport::discardRetainedAudioDocument
        singleeditor::WorkspaceSessionSupport::pruneRetainedPlaylistAudio
        singleeditor::WorkspaceSessionSupport::retainCurrentAudioDocumentIfNeeded
    TRIGGER "Explicit playlist or document switch, removal, retention preference or admitted cache warming"
    RETENTION "Exact workspace history snapshot and matching waveform caches until release, not a path-only reload")
single_editor_declare_code_unit(
    ID singleeditor.recovery.workspace-adoption OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE Source/Document/WorkspaceSessionSupport.cpp TARGETS SingleEditor
    MEMBERS singleeditor::WorkspaceSessionSupport::restoreRecoveredDocument
        singleeditor::WorkspaceSessionSupport::acceptRecoveredPeaks
    TRIGGER "Explicit activation of a recovered playlist entry or completion of its generation-bound peak worker"
    RETENTION "Complete recovered workspace snapshot and exact attempt identity including Undo and Redo history")
single_editor_declare_code_unit(
    ID singleeditor.recovery.playlist-adoption OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE "Source/Document/PlaylistModel.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::PlaylistModel::addRecoveredDocument"
    TRIGGER "Explicit recovery selection adopts a verified recovered document into the playlist"
    RETENTION "Playlist-owned independent recovered image, complete history and CAS readers until row release")
single_editor_declare_code_unit(
    ID singleeditor.core-file.cache-warm-admission OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::MainComponent::PendingCacheWarm
    MEMBERS singleeditor::MainComponent::scheduleCacheWarm
        singleeditor::MainComponent::scheduleWaveformCacheWarm
        singleeditor::MainComponent::scheduleAdjacentPlaylistCacheWarm
        singleeditor::MainComponent::cancelCacheWarm
        singleeditor::MainComponent::dispatchPendingCacheWarmIfReady
    TRIGGER "Initial or replacement open records a request and existing UI timer checks current generation and true CoreReady"
    RETENTION "Pending filenames and deadline only before admission with cache inspection and worker construction delayed until ready")
single_editor_declare_code_unit(
    ID singleeditor.core-file.cache-warm-worker OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    SCOPES "singleeditor::`anonymous-namespace'::AsyncCacheWarmJob"
    MEMBERS singleeditor::MainComponent::scheduleRecentCacheWarm
    TRIGGER "Current waveform and playable milestones or visible initially empty workspace admit due nonessential warming"
    RETENTION "Lazy one-thread pool until editor destruction with jobs cancelled on replacement or close and checked between files")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.preview-cache-writer OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/Waveform/WaveformModel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::WaveformModel::ensurePersistentPreviewCache
    TRIGGER "Admitted nonessential cache worker prepares a bounded preview for another audio file"
    RETENTION "Per-file reader and preview until cache publication while ordinary first-waveform primitives retain eager ownership")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.peak-cache-publication OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/Waveform/WaveformModel.cpp TARGETS SingleEditor
    SCOPES singleeditor::WaveformModel::PeakCachePersistenceJob
    MEMBERS singleeditor::WaveformModel::dispatchPendingPeakCachePersistence
        singleeditor::WaveformModel::persistStartupPreviewCache
        "singleeditor::`anonymous namespace'::persistPeakCacheToDisk"
        "singleeditor::`anonymous namespace'::persistStartupPreviewToDisk"
        "singleeditor::`anonymous namespace'::writePeakLevel"
    TRIGGER "Current real CoreReady admits due source-backed cache persistence on the existing waveform pool and explicit admitted warmer shares the preview serializer"
    PDB_VFTABLE_CLASSES
        "singleeditor::WaveformModel::PeakCachePersistenceJob" ".?AVPeakCachePersistenceJob@WaveformModel@singleeditor@@"
    RETENTION "Immutable peak snapshot and exact source state until completion or owner cancellation with chunk checks and locked final publication rejecting old epochs")
single_editor_declare_code_unit(
    ID singleeditor.document-save.workspace-inventory OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/Document/WorkspaceSessionSupport.cpp TARGETS SingleEditor
    SCOPES singleeditor::WorkspaceSessionSupport::UnsavedDocument
    MEMBERS singleeditor::WorkspaceSessionSupport::collectUnsavedDocuments
    TRIGGER "Explicit close or Save flow gathers current and retained unsaved workspace documents"
    RETENTION "In-memory exact revision inventory for the pending close or Save decision")
single_editor_declare_code_unit(
    ID singleeditor.bnk.workspace-project-map OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/Document/WorkspaceSessionSupport.cpp TARGETS SingleEditor
    MEMBERS singleeditor::WorkspaceSessionSupport::getMappedBnkProjectRoot
        singleeditor::WorkspaceSessionSupport::setMappedBnkProjectRoot
        singleeditor::WorkspaceSessionSupport::makeBnkProjectMappingKey
    TRIGGER "Explicit bank open or user selection of the associated BNK project directory"
    RETENTION "Existing bank-to-project setting while preserving the user selected project mapping")

single_editor_declare_code_unit(
    ID singleeditor.window-shell.startup-core OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/App/StartupPresentationCoordinator.cpp
    MEMBERS singleeditor::StartupPresentationCoordinator::beginGeneration
        singleeditor::StartupPresentationCoordinator::isCurrent
        singleeditor::StartupPresentationCoordinator::snapshot
        singleeditor::StartupPresentationCoordinator::markFileNameVisible
        singleeditor::StartupPresentationCoordinator::markTruthfulWaveformVisible
        singleeditor::StartupPresentationCoordinator::markPlayable
        singleeditor::StartupPresentationCoordinator::noteHostMutation
        singleeditor::StartupPresentationCoordinator::mark
        singleeditor::StartupPresentationCoordinator::refreshCoreReady
    TARGETS SingleEditor
    TRIGGER "Minimal startup window and current Core presentation state"
    RETENTION "Startup host lifetime, renderer attachment members remain separate")
# MainWindow remains a mixed host. The timer and queue admission execute before
# CoreReady; the settings writers also run on explicit early shutdown.
single_editor_declare_code_unit(
    ID singleeditor.window-shell.main-host OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/App/MainWindow.cpp TARGETS SingleEditor
    SCOPES "singleeditor::`anonymous-namespace'::StartupPlaceholderComponent"
        "singleeditor::`anonymous-namespace'::StartupAudioPrepareJob"
    MEMBERS singleeditor::MainWindow::MainWindow singleeditor::MainWindow::~MainWindow
        singleeditor::MainWindow::getMainComponent singleeditor::MainWindow::openFileWhenReady
        singleeditor::MainWindow::findControlAtPoint singleeditor::MainWindow::requestStartupKeyboardFocus
        singleeditor::MainWindow::activeWindowStatusChanged singleeditor::MainWindow::keyPressed
        singleeditor::MainWindow::moved singleeditor::MainWindow::resized
        singleeditor::MainWindow::beginStartupAudioPrepare singleeditor::MainWindow::completeStartupAudioPreview
        singleeditor::MainWindow::completeStartupAudioPrepare
        singleeditor::MainWindow::scheduleMainContentInstallAfterStartupPreview
        singleeditor::MainWindow::initialiseMainContentAsync singleeditor::MainWindow::installMainContent
        singleeditor::MainWindow::restoreWindowState singleeditor::MainWindow::requestWindowStatePersist
        singleeditor::MainWindow::timerCallback
    TRIGGER "Ordinary initial native host, source preparation, main-content handoff and shared window callbacks"
    RETENTION "Window lifetime and ordinary startup workers with shared pre-Core persistence admission")
# Reviewed existing bootstrap and playback bodies. These declarations account
# for already-shipping mandatory code; they do not add eager product behavior.
single_editor_declare_code_unit(
    ID singleeditor.window-shell.application-entry OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/App/SingleEditorApplication.cpp TARGETS SingleEditor
    MEMBERS singleeditor::SingleEditorApplication::SingleEditorApplication
        singleeditor::SingleEditorApplication::getApplicationName
        singleeditor::SingleEditorApplication::getApplicationVersion
        singleeditor::SingleEditorApplication::moreThanOneInstanceAllowed
        singleeditor::SingleEditorApplication::initialise
        singleeditor::SingleEditorApplication::shutdown
        singleeditor::SingleEditorApplication::configureAppProperties
        singleeditor::SingleEditorApplication::parseInitialFile
        singleeditor::SingleEditorApplication::selectRenderer
    TRIGGER "Existing GUI application entry, selected ordinary source and minimal window setup"
    RETENTION "Application root through shutdown with initial output and document ownership")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.chrome-painter OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/App/SingleEditorApplication.cpp TARGETS SingleEditor
    PDB_METHODS "singleeditor::WindowChromeLookAndFeel::CaptionButton::paintButton"
        "singleeditor::WindowChromeLookAndFeel::WindowChromeLookAndFeel"
        "singleeditor::WindowChromeLookAndFeel::createDocumentWindowButton"
        "singleeditor::WindowChromeLookAndFeel::createDropShadowerForComponent"
        "singleeditor::WindowChromeLookAndFeel::drawDocumentWindowTitleBar"
        "singleeditor::WindowChromeLookAndFeel::drawResizableFrame"
        "singleeditor::WindowChromeLookAndFeel::drawResizableWindowBorder"
        "singleeditor::WindowChromeLookAndFeel::palette"
        "singleeditor::WindowChromeLookAndFeel::positionDocumentWindowButtons"
        "singleeditor::WindowChromeLookAndFeel::~WindowChromeLookAndFeel"
    TRIGGER "Initial application constructs the minimal WindowChromeLookAndFeel before MainWindow"
    RETENTION "Neutral caption controls, palette and frame methods shared by the initial window and the later renderer")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.application-callback-storage OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/App/SingleEditorApplication.cpp
    TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Copy"
        "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Delete_this"
        "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Get"
        "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Move"
        "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Target_type"
    TRIGGER "Initial application creates and retains the prepared-renderer callback without invoking the full renderer"
    RETENTION "Function wrapper storage and lifetime management only with invocation declared separately")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.application-actions OWNER singleeditor.window-shell
    LIFECYCLE CoreDeferred SOURCE Source/App/SingleEditorApplication.cpp TARGETS SingleEditor
    MEMBERS singleeditor::SingleEditorApplication::anotherInstanceStarted
        singleeditor::SingleEditorApplication::systemRequestedQuit
    TRIGGER "Explicit additional open request or operating-system quit request"
    RETENTION "Request delegated to the existing window and close coordinator")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.application-renderer OWNER singleeditor.window-shell
    LIFECYCLE PostCoreWorkspace SOURCE Source/App/SingleEditorApplication.cpp TARGETS SingleEditor
    MEMBERS singleeditor::SingleEditorApplication::ensureLookAndFeelReady
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::SingleEditorApplication::initialise'::`2'::<lambda_1>,void>::_Do_call"
    TRIGGER "Existing prepared-renderer callback after core presentation or empty/error workspace admission"
    RETENTION "Selected full look and feel through application shutdown")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.gui-entry OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/Main.cpp TARGETS SingleEditor WHOLE_SOURCE
    TRIGGER "Native GUI process entry creates the sole application root"
    RETENTION "Existing JUCE application entry and root destruction")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.title-controls OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/UI/Components/CustomTitleBarComponent.cpp TARGETS SingleEditor WHOLE_SOURCE
    TRIGGER "Existing main-view title/menu/window controls and initial layout"
    RETENTION "Window-owned controls and callbacks without constructing menu content or optional modules")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.rate-source OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE Source/Audio/PlaybackRateSource.cpp TARGETS SingleEditor SingleEditorCLI WHOLE_SOURCE
    TRIGGER "Admitted ordinary document transport prepares for the selected output rate"
    RETENTION "Transport-owned rate adapter, bypassing conversion at equal rates, released with the source")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.window-actions OWNER singleeditor.window-shell
    LIFECYCLE CoreDeferred SOURCE Source/App/MainWindow.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainWindow::restoreFromBackground singleeditor::MainWindow::requestUserCloseAction
        singleeditor::MainWindow::requestActualQuit singleeditor::MainWindow::closeButtonPressed
        singleeditor::MainWindow::shouldCloseToBackground singleeditor::MainWindow::hideToBackground
        singleeditor::MainWindow::ensureTrayIconVisible
    TRIGGER "Explicit window close, quit or background restore action and its existing close-to-tray preference"
    RETENTION "Requested window action and tray ownership without declaring the mixed tray implementation")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.state-writer OWNER singleeditor.window-shell
    LIFECYCLE CoreDeferred SOURCE Source/App/MainWindow.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainWindow::persistWindowState singleeditor::MainWindow::flushPendingWindowStatePersist
    TRIGGER "Due window state after current CoreReady or idle empty admission, and explicit shutdown flush"
    RETENTION "Pending window geometry write through completion or window destruction")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.session-settings-admission OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::restoreSessionSettings singleeditor::MainComponent::restoreStartupCriticalSettings
        singleeditor::MainComponent::persistSessionSettings singleeditor::MainComponent::requestSessionSettingsPersist
        singleeditor::MainComponent::canPersistDeferredSettings
    TRIGGER "Initial empty or ordinary-file settings reads and shared pre-Core queue admission"
    RETENTION "Editor settings values and pending request without declaring its writer eager")
single_editor_declare_code_unit(
    ID singleeditor.core-lifecycle.post-core-promotion-admission OWNER singleeditor.core-lifecycle
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp
    TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::schedulePostCorePromotionIfNeeded"
    TRIGGER "Real waveform or playable milestone checks current CoreReady and schedules one generation-bound continuation"
    RETENTION "Admission guard and queued callback storage without treating creation as execution")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.post-core-promotion OWNER singleeditor.core-edit-ui
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/MainComponent.cpp
    TARGETS SingleEditor
    PDB_METHODS "`juce::MessageManager::callAsync<`singleeditor::MainComponent::schedulePostCorePromotionIfNeeded'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
    TRIGGER "Queued promotion rechecks exact current generation and real CoreReady before workspace work"
    RETENTION "One retained message invocation through current sidebar, renderer and recovery promotion")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.empty-workspace-continuation OWNER singleeditor.core-edit-ui
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/MainComponent.cpp
    TARGETS SingleEditor
    PDB_METHODS "`juce::MessageManager::callAsync<`singleeditor::MainComponent::MainComponent'::`16'::<lambda_57> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::MainComponent'::`16'::<lambda_57>::operator()'::`5'::<lambda_1>,void>::_Do_call"
    TRIGGER "The no-initial-file constructor posts playlist restoration and delayed cache admission for the shown empty workspace"
    RETENTION "Safe-pointer continuation while a restored audio file must reach its own actual CoreReady")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.session-settings-writer OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::flushPendingSessionSettingsPersist
    TRIGGER "Due session state after current CoreReady or idle empty admission, and explicit shutdown flush"
    RETENTION "Pending session settings write through completion or main-content destruction")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.menu-controller OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::showRecentFilesMenu singleeditor::MainComponent::showTitleBarMenu
    PDB_CLASSES
        "singleeditor::MainComponent::showRecentFilesMenu::__l2::<lambda_1>"
        "singleeditor::MainComponent::showTitleBarMenu::__l2::<lambda_2>"
        "singleeditor::MainComponent::showTitleBarMenu::__l2::<lambda_1>"
        "singleeditor::MainComponent::{ctor}::__l2::<lambda_18>::()::__l2::<lambda_2>"
        "singleeditor::MainComponent::{ctor}::__l2::<lambda_18>"
        "singleeditor::MainComponent::{ctor}::__l2::<lambda_18>::()::__l2::<lambda_1>"
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::MainComponent'::`2'::<lambda_18>::operator()'::`2'::<lambda_2>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecentFilesMenu'::`2'::<lambda_1>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showTitleBarMenu'::`2'::<lambda_2>,void,int>::_Do_call"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_18>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_1>,void,enum singleeditor::CustomTitleBarComponent::MenuKind,juce::Rectangle<int> >::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_20>,void>::_Do_call"
    TRIGGER "Explicit recent-files button, custom title menu or menu command request"
    RETENTION "Requested menu and command selection while optional handlers retain their own admission")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-paint OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/WaveformViewComponent.cpp
    MEMBERS singleeditor::WaveformViewComponent::drawTimeRuler
        singleeditor::WaveformViewComponent::drawBackgroundGrid
        singleeditor::WaveformViewComponent::drawWaveforms
        singleeditor::WaveformViewComponent::drawEmptyState
        singleeditor::WaveformViewComponent::notifyWaveformPresentationIfNeeded
        singleeditor::ModernWaveformPainter::placeholderLayout
        singleeditor::ModernWaveformPainter::paintPlaceholder
    TARGETS SingleEditor
    MEMBERS "singleeditor::`anonymous namespace'::formatRulerLabel"
    PDB_METHODS "singleeditor::WaveformPlotGeometry::content"
    PDB_FUNCTIONS "std::_Med3_unchecked<singleeditor::WaveformPlotGeometry::AmplitudeTick *,`singleeditor::WaveformPlotGeometry::amplitudeTicks'::`2'::<lambda_2> >"
        "std::_Pop_heap_hole_by_index<singleeditor::WaveformPlotGeometry::AmplitudeTick *,singleeditor::WaveformPlotGeometry::AmplitudeTick,`singleeditor::WaveformPlotGeometry::amplitudeTicks'::`2'::<lambda_2> >"
        "std::_Sort_unchecked<singleeditor::WaveformPlotGeometry::AmplitudeTick *,`singleeditor::WaveformPlotGeometry::amplitudeTicks'::`2'::<lambda_2> >"
    TRIGGER "Actual ordinary waveform surface paint using source peak data"
    RETENTION "Core waveform component, no spectrum worker or cache preparation")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-paint-closures OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/WaveformViewComponent.cpp TARGETS SingleEditor
    PDB_CLASSES "singleeditor::WaveformViewComponent::drawWaveforms::__l2::<lambda_2>"
        "singleeditor::WaveformViewComponent::drawWaveforms::__l21::<lambda_3>"
    TRIGGER "Ordinary waveform lane and channel-control paint within drawWaveforms"
    RETENTION "Stack-local paint closures, exact compiler class parents only")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-presentation OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/WaveformViewComponent.cpp
    MEMBERS singleeditor::WaveformViewComponent::WaveformViewComponent
        singleeditor::WaveformViewComponent::~WaveformViewComponent
        singleeditor::WaveformViewComponent::paint
        singleeditor::WaveformViewComponent::resized
        singleeditor::WaveformViewComponent::repaintAndNotify
        singleeditor::WaveformViewComponent::refreshNow
        singleeditor::WaveformViewComponent::setLoadingHint
        singleeditor::WaveformViewComponent::setOpenFailureHint
        singleeditor::WaveformViewComponent::setCommandAvailability
        singleeditor::WaveformViewComponent::getSelectionRange
        singleeditor::WaveformViewComponent::getSelectionBoundsInLocal
        singleeditor::WaveformViewComponent::getViewMode
        singleeditor::WaveformViewComponent::getDisplayMode
        singleeditor::WaveformViewComponent::getVisibleTimeRange
        singleeditor::WaveformViewComponent::ensureTimeVisible
        singleeditor::WaveformViewComponent::getHeaderBounds
        singleeditor::WaveformViewComponent::getRulerBounds
        singleeditor::WaveformViewComponent::getPlotBounds
        singleeditor::WaveformViewComponent::getOverviewBounds
        singleeditor::WaveformViewComponent::getInteractiveBounds
        singleeditor::WaveformViewComponent::secondsToX
        singleeditor::WaveformViewComponent::gainToY
        singleeditor::WaveformViewComponent::drawEnvelope
        singleeditor::WaveformViewComponent::beginPresentationGeneration
        singleeditor::WaveformViewComponent::notifySelectionChanged
        singleeditor::WaveformViewComponent::changeListenerCallback
        singleeditor::WaveformViewComponent::timerCallback
        singleeditor::WaveformViewComponent::clearSelection
        singleeditor::WaveformViewComponent::clearSelectionPreviewGain
        singleeditor::WaveformViewComponent::setSelectedSourceChannels
        singleeditor::WaveformViewComponent::getLoadedPreviewTimeRange
        singleeditor::WaveformViewComponent::refreshOverview
        singleeditor::WaveformViewComponent::setSimplePresentation
    TARGETS SingleEditor
    TRIGGER "Ordinary waveform construction, first paint, generation and shared presentation state"
    RETENTION "Existing startup and ordinary waveform callbacks. Exact members only, no spectral worker or whole view scope")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-interaction OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/Components/WaveformViewComponent.cpp
    MEMBERS singleeditor::WaveformViewComponent::mouseDown
        singleeditor::WaveformViewComponent::mouseDrag
        singleeditor::WaveformViewComponent::mouseUp
        singleeditor::WaveformViewComponent::mouseWheelMove
        singleeditor::WaveformViewComponent::mouseDoubleClick
        singleeditor::WaveformViewComponent::keyPressed
        singleeditor::WaveformViewComponent::selectWholeFile
        singleeditor::WaveformViewComponent::setSelectionPreviewGain
        singleeditor::WaveformViewComponent::zoomHorizontal
        singleeditor::WaveformViewComponent::zoomVertical
        singleeditor::WaveformViewComponent::seekToTime
        singleeditor::WaveformViewComponent::xToSeconds
        singleeditor::WaveformViewComponent::yToGain
        singleeditor::WaveformViewComponent::findEnvelopePointAt
        singleeditor::WaveformViewComponent::hitTestChannelButton
        singleeditor::WaveformViewComponent::hitTestChannelLaneLabel
        singleeditor::WaveformViewComponent::updateSelectionFromPosition
        singleeditor::WaveformViewComponent::updateEnvelopePointFromMouse
        singleeditor::WaveformViewComponent::panVisibleRangeBy
        singleeditor::WaveformViewComponent::resetView
    TARGETS SingleEditor
    TRIGGER "Explicit waveform pointer, keyboard, navigation or envelope editing gesture"
    RETENTION "Ordinary waveform interaction callbacks retained with the view. Registration does not execute the gesture")
single_editor_declare_code_unit(
    ID singleeditor.spectral.view OWNER singleeditor.spectral
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/WaveformViewComponent.cpp
    MEMBERS singleeditor::WaveformViewComponent::drawSpectrogram
    TARGETS SingleEditor
    PDB_METHODS "singleeditor::ModernWaveformPainter::paintSpectralAxis"
        "singleeditor::ModernWaveformPainter::spectralStatusLayout"
        "singleeditor::WaveformPlotGeometry::frequencyTicks"
    PDB_FUNCTIONS "std::_Med3_unchecked<singleeditor::WaveformPlotGeometry::FrequencyTick *,`singleeditor::WaveformPlotGeometry::frequencyTicks'::`2'::<lambda_1> >"
        "std::_Pop_heap_hole_by_index<singleeditor::WaveformPlotGeometry::FrequencyTick *,singleeditor::WaveformPlotGeometry::FrequencyTick,`singleeditor::WaveformPlotGeometry::frequencyTicks'::`2'::<lambda_1> >"
        "std::_Sort_unchecked<singleeditor::WaveformPlotGeometry::FrequencyTick *,`singleeditor::WaveformPlotGeometry::frequencyTicks'::`2'::<lambda_1> >"
    TRIGGER "Requested Spectral mode after current document CoreReady"
    RETENTION "Requested spectral presentation of the current document")
single_editor_declare_code_unit(
    ID singleeditor.spectral.preparation OWNER singleeditor.spectral
    LIFECYCLE ModuleLazy SOURCE Source/Waveform/WaveformModel.cpp
    MEMBERS singleeditor::WaveformModel::ensureSpectrogramReady
        singleeditor::WaveformModel::scheduleSpectrogramBuild
        singleeditor::WaveformModel::publishSpectrogramCache
        singleeditor::WaveformModel::loadPersistentSpectrogramCache
        singleeditor::WaveformModel::isSpectrogramCacheReady
        "singleeditor::`anonymous namespace'::persistSpectrogramToDisk"
        "singleeditor::`anonymous namespace'::getSpectrogramCacheFile"
        "singleeditor::`anonymous namespace'::colourFromSpectralEnergy"
    SCOPES singleeditor::WaveformModel::SpectrogramBuildJob
    PDB_FUNCTIONS "singleeditor::`anonymous namespace'::buildSpectrogramForState<`singleeditor::WaveformModel::SpectrogramBuildJob::runJob'::`5'::<lambda_1> >"
    TARGETS SingleEditor
    TRIGGER "Current CoreReady and explicit or retained Spectral view request"
    PDB_VFTABLE_CLASSES
        "singleeditor::WaveformModel::SpectrogramBuildJob" ".?AVSpectrogramBuildJob@WaveformModel@singleeditor@@"
    RETENTION "Waveform model generation, late worker results checked against current audio revision")
single_editor_declare_code_unit(
    ID singleeditor.workspace.host OWNER singleeditor.workspace
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/Workspace/WorkspaceModuleHostComponent.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "ensureWorkspaceModuleHost after current CoreReady and startup chrome deferral ends"
    RETENTION "Current workspace host and its explicit module/view leases, released on generation change or shutdown")

set(SINGLE_EDITOR_RENDERER_STYLE_SOURCES
    Source/UI/Renderer/RendererSelection.cpp
    Source/UI/Renderer/RendererVisualStyle.cpp
    # Optional Modern skins (More > Appearance). Each skin is one file under
    # its label, so it can be reverted or cherry-picked on its own.
    Source/UI/Skins/SkinStyle.cpp
    # D  Layered Tiles
    Source/UI/Skins/TilesSkin.cpp
    # A  Soft
    Source/UI/Skins/SoftSkin.cpp
    # C  Liquid Glass
)

set(SINGLE_EDITOR_CLI_APPLICATION_SOURCES
    Source/CLI/CliArguments.cpp
    Source/CLI/CliApplication.cpp
    Source/CLI/CliAudioSuiteOperationRegistry.cpp
    Source/CLI/CliBatchAutomation.cpp
    Source/CLI/CliColdManifest.cpp
    Source/CLI/CliSingleFileAudioSuite.cpp
)

# The external command surface is absent from the GUI product. This owns only
# its entry/dispatch/controllers, not the shared document, DSP or device sources.
foreach(source IN ITEMS Source/CLI/SingleEditorCliMain.cpp ${SINGLE_EDITOR_CLI_APPLICATION_SOURCES})
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.cli-automation.source.${unit_name}" OWNER singleeditor.cli-automation
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditorCLI
        TRIGGER "Explicit external CLI discovery or command invocation, with operation creation gated by command dispatch"
        RETENTION "Command-owned arguments, cold declarations and bounded execution state until command completion")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.diagnostics.source.clidiagnostics OWNER singleeditor.diagnostics
    LIFECYCLE ModuleLazy SOURCE Source/CLI/CliDiagnostics.cpp WHOLE_SOURCE TARGETS SingleEditorCLI
    TRIGGER "Explicit CLI diagnostics command after main argument dispatch"
    RETENTION "Diagnostic command event and reviewed export state until completion or cancellation")

set(SINGLE_EDITOR_AUTOMATION_PLAN_SOURCES
    Source/Automation/AutomationPlanApply.cpp
    Source/Automation/AutomationPlanIO.cpp
    Source/Automation/AutomationPlanPreflight.cpp
    Source/Automation/AudioInspectionService.cpp
    Source/Automation/BuiltInOperations.cpp
    Source/Automation/EditPipelineAnalysisService.cpp
    Source/Automation/FileIdentity.cpp
    Source/Automation/RoutingInspectionService.cpp
    Source/Automation/TrimSilenceAnalysisService.cpp
)

# These plan/inspection implementations are selected by explicit CLI commands.
# Shared document, DSP and routing primitives keep their own source declarations.
foreach(source IN ITEMS Source/Automation/OperationRegistry.cpp ${SINGLE_EDITOR_AUTOMATION_PLAN_SOURCES})
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.cli-automation.source.${unit_name}" OWNER singleeditor.cli-automation
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditorCLI
        TRIGGER "Explicit CLI inspect, analyze, plan, preflight or apply command after external command dispatch"
        RETENTION "Command-owned registry, plan, source identity and bounded analysis or apply state until completion")
endforeach()

set(SINGLE_EDITOR_AUDIO_SUITE_SOURCES
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    Source/AudioSuite/ParametricEqProcessor.cpp
    Source/AudioSuite/TruePeakLimiterProcessor.cpp
)

# Shared transaction and render contracts have their own bounded owner. Plain
# selection/parameter values and cold preview handles are deliberately excluded.
# Each actual processor retains its independent module identity below.
single_editor_declare_code_unit(
    ID singleeditor.audio-suite.transaction OWNER singleeditor.audio-suite
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/AudioSuiteReferenceProcessor.cpp
    SCOPES singleeditor::AudioSuitePlan singleeditor::AudioSuitePlanBuilder
        singleeditor::AudioSuiteTransactionExecutor singleeditor::AudioSuiteRenderSession
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Admitted GUI DSP analyze, preview or apply action, or explicit CLI processing operation"
    RETENTION "Shared immutable processing plan and transaction-owned renderer through completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.audio-suite.reference-render OWNER singleeditor.audio-suite
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/AudioSuiteReferenceProcessor.cpp
    SCOPES "singleeditor::`anonymous-namespace'::ReferenceGainRenderFactory"
        "singleeditor::`anonymous-namespace'::ReferenceGainRenderSession"
    MEMBERS "singleeditor::`anonymous namespace'::referenceGainForSample"
        singleeditor::AudioSuiteReferenceProcessor::resolveSelection
        singleeditor::AudioSuiteReferenceProcessor::analyze singleeditor::AudioSuiteReferenceProcessor::createPreview
        singleeditor::AudioSuiteReferenceProcessor::apply singleeditor::AudioSuiteReferenceProcessor::renderBlockInPlace
        singleeditor::AudioSuiteReferenceProcessor::gainForSample
    TARGETS SingleEditor SingleEditorCLI
    MEMBERS "singleeditor::AudioSuiteReferenceProcessor::liveInstanceCount"
    TRIGGER "CoreReady-gated reference DSP request or explicit CLI reference-gain operation"
    RETENTION "Requested reference plan, bounded preview or transactional rendered document")
single_editor_declare_code_unit(
    ID singleeditor.audio-suite.reference-controller OWNER singleeditor.audio-suite
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::ensureReferenceAudioSuiteProcessor"
        "singleeditor::MainComponent::updateReferenceAudioSuitePreview"
        "singleeditor::MainComponent::applySelectionGain"
        "singleeditor::MainComponent::finishReferenceAudioSuiteApply"
    SCOPES "singleeditor::MainComponent::ReferenceAudioSuiteApplyThread"
    PDB_CLASSES "singleeditor::MainComponent::applySelectionGain::__l2::<lambda_1>"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::applySelectionGain'::`2'::<lambda_1>,void,singleeditor::AudioSuiteApplyResult>::_Do_call"
        "`juce::MessageManager::callAsync<`singleeditor::MainComponent::ReferenceAudioSuiteApplyThread::run'::`5'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_34>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_35>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_48>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_49>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_50>,void>::_Do_call"
    TRIGGER "Explicit selection-gain preview or Apply, with current-generation real CoreReady admission before processor creation"
    PDB_VFTABLE_CLASSES
        "singleeditor::MainComponent::ReferenceAudioSuiteApplyThread" ".?AVReferenceAudioSuiteApplyThread@MainComponent@singleeditor@@"
    RETENTION "Selection preview lease or apply worker and completion until cancellation, generation replacement or completion")
single_editor_declare_code_unit(
    ID singleeditor.parametric-eq-v1.render-support OWNER singleeditor.parametric-eq-v1
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/ParametricEqProcessor.cpp
    SCOPES "singleeditor::`anonymous-namespace'::BiquadState"
        "singleeditor::`anonymous-namespace'::ParametricEqRenderFactory"
        "singleeditor::`anonymous-namespace'::ParametricEqRenderSession"
    MEMBERS "singleeditor::`anonymous namespace'::normalize" "singleeditor::`anonymous namespace'::makeCoefficients"
        "singleeditor::`anonymous namespace'::resolveBands" "singleeditor::`anonymous namespace'::parametersAreValid"
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit EQ analysis, preview or apply uses its requested plan and render factory"
    RETENTION "Plan-owned coefficient factory and independent processing session for requested EQ work")
single_editor_declare_code_unit(
    ID singleeditor.true-peak-limiter-v1.render-support OWNER singleeditor.true-peak-limiter-v1
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/TruePeakLimiterProcessor.cpp
    SCOPES "singleeditor::`anonymous-namespace'::TruePeakLimiterRenderFactory"
        "singleeditor::`anonymous-namespace'::TruePeakLimiterRenderSession"
    MEMBERS "singleeditor::`anonymous namespace'::dbToGain" "singleeditor::`anonymous namespace'::gainToDb"
        "singleeditor::`anonymous namespace'::atomicMaximum" "singleeditor::`anonymous namespace'::cubicInterpolate"
        "singleeditor::`anonymous namespace'::sampleAt" "singleeditor::`anonymous namespace'::oversampledFramePeak"
        "singleeditor::`anonymous namespace'::parametersAreValid"
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit Limiter analysis, preview, apply or requested rendered-output verification"
    RETENTION "Plan-owned render factory, bounded lookahead session and post-render verification state")

# Plain parameter structs are also used by cold UI/document state. Do not
# classify their constructors or unrelated TU helpers as lazy processor code.
single_editor_declare_code_unit(
    ID singleeditor.parametric-eq-v1.processor OWNER singleeditor.parametric-eq-v1
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/ParametricEqProcessor.cpp
    SCOPES singleeditor::ParametricEqProcessor singleeditor::ParametricEqPlan
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Post-Core explicit EQ Mag activation or explicit CLI EQ capability / operation request"
    RETENTION "Mag processor, analysis plan and preview or apply consumers")
single_editor_declare_code_unit(
    ID singleeditor.true-peak-limiter-v1.processor OWNER singleeditor.true-peak-limiter-v1
    LIFECYCLE ModuleLazy SOURCE Source/AudioSuite/TruePeakLimiterProcessor.cpp
    SCOPES singleeditor::TruePeakLimiterProcessor singleeditor::TruePeakLimiterPlan singleeditor::TruePeakLimiterFeedbackState
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Post-Core explicit Limiter Mag activation or explicit CLI Limiter capability / operation request"
    RETENTION "Mag processor, analysis plan, feedback and preview or apply consumers")

single_editor_declare_code_unit(
    ID singleeditor.parametric-eq-v1.view OWNER singleeditor.parametric-eq-v1
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/ParametricEqMagComponent.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit post-Core EQ Mag view factory"
    RETENTION "Owned workspace view, released by host releaseView, generation replacement or shutdown")
single_editor_declare_code_unit(
    ID singleeditor.true-peak-limiter-v1.view OWNER singleeditor.true-peak-limiter-v1
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/TruePeakLimiterMagComponent.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit post-Core Limiter Mag view factory"
    RETENTION "Owned workspace view, released by host releaseView, generation replacement or shutdown")

single_editor_declare_code_unit(
    ID singleeditor.document-save.passive-state OWNER singleeditor.document-save
    LIFECYCLE CoreEager SOURCE Source/Document/DocumentSaveCoordinator.cpp
    MEMBERS singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator
        singleeditor::DocumentSaveCoordinator::~DocumentSaveCoordinator
    PDB_METHODS
        "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Copy"
        "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Move"
        "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Get"
        "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Target_type"
        "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Delete_this"
    TARGETS SingleEditor
    TRIGGER "Existing MainComponent member construction before ordinary CoreReady stores the default writer without invoking it"
    RETENTION "Passive coordinator and writer callback storage until editor destruction. Worker allocation and writer invocation remain deferred")
single_editor_declare_code_unit(
    ID singleeditor.document-save.execution OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/Document/DocumentSaveCoordinator.cpp
    MEMBERS singleeditor::DocumentSaveCoordinator::requestSave
        singleeditor::DocumentSaveCoordinator::startRequest
        singleeditor::DocumentSaveCoordinator::finishFromWorker
        singleeditor::DocumentSaveCoordinator::handleAsyncUpdate
        singleeditor::DocumentSaveCoordinator::whenIdle
        singleeditor::DocumentSaveCoordinator::isSaveInProgress
        singleeditor::DocumentSaveCoordinator::hasPendingSave
    SCOPES singleeditor::DocumentSaveCoordinator::SaveJob
        singleeditor::DocumentSaveCoordinator::Request
        singleeditor::DocumentSaveCoordinator::CompletedRequest
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::DocumentSaveCoordinator::DocumentSaveCoordinator'::`5'::<lambda_1>,singleeditor::SingleTrackDocument::SaveWriteResult,singleeditor::SingleTrackDocument::SaveSnapshot const &>::_Do_call"
    TARGETS SingleEditor
    TRIGGER "Explicit Save or Save As with an owned snapshot, or the idle decision for a user close or document replacement"
    RETENTION "Active and coalesced pending save snapshots until worker completion and publication"
    PDB_VFTABLE_CLASSES
        "singleeditor::DocumentSaveCoordinator::SaveJob" ".?AVSaveJob@DocumentSaveCoordinator@singleeditor@@")

# Exact ordinary-audio slices of mixed document, transport and waveform sources.
# These shared entry points do not grant ownership to callees, anonymous helpers,
# spectral workers, diagnostics or device configuration commands.
single_editor_declare_code_unit(
    ID singleeditor.core-file.document-input OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::SingleTrackDocument::createReaderForFile
        singleeditor::SingleTrackDocument::prepareAudioFileForOpen
        singleeditor::SingleTrackDocument::buildFileInfoForReader
        singleeditor::SingleTrackDocument::probeFileInfo
        singleeditor::SingleTrackDocument::createPreviewState
        singleeditor::SingleTrackDocument::createStateFromFile
        singleeditor::SingleTrackDocument::createStateFromPreparedFile
        singleeditor::SingleTrackDocument::createStateFromOwnedReader
        singleeditor::SingleTrackDocument::createStateFromReader
        singleeditor::SingleTrackDocument::loadFile
        singleeditor::SingleTrackDocument::adoptPreviewState
        singleeditor::SingleTrackDocument::adoptLoadedState
        singleeditor::SingleTrackDocument::clear
        singleeditor::SingleTrackDocument::getMediaIdentity
        singleeditor::SingleTrackDocument::getState
        singleeditor::SingleTrackDocument::getFileInfo
        singleeditor::SingleTrackDocument::hasAudio
        singleeditor::SingleTrackDocument::hasUnsavedChanges
        singleeditor::SingleTrackDocument::getDurationSeconds
        singleeditor::SingleTrackDocument::getNumSamples
        singleeditor::SingleTrackDocument::getSampleRate
        singleeditor::SingleTrackDocument::getPlayableSampleRange
        singleeditor::SingleTrackDocument::getPlayableTimeRange
        singleeditor::SingleTrackDocument::makeDefaultEnvelope
        singleeditor::SingleTrackDocument::makeEmptyState
        singleeditor::SingleTrackDocument::sanitiseEnvelope
        singleeditor::SingleTrackDocument::notifyDocumentChanged
        singleeditor::SingleTrackDocument::notifyIdentityChanged
        singleeditor::SingleTrackDocument::nextAudioRevisionNumber
        singleeditor::SingleTrackDocument::nextEnvelopeRevisionNumber
        singleeditor::SingleTrackDocument::nextChangeGenerationNumber
    PDB_FUNCTIONS "std::_Sort_unchecked<singleeditor::SingleTrackDocument::EnvelopePoint *,`singleeditor::SingleTrackDocument::sanitiseEnvelope'::`2'::<lambda_1> >"
    TRIGGER "Ordinary GUI or explicit CLI reader admission, document state and playable range publication"
    RETENTION "Document reader and immutable state until replacement, retained workspace or Undo consumers release them")
single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Document/SingleTrackDocument.h"
    ID singleeditor.core-file.document-state OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::SingleTrackDocument::FileInfo singleeditor::SingleTrackDocument::State
        singleeditor::SingleTrackDocument::PreparedFileLoad singleeditor::SingleTrackDocument::SourceFileIdentity
        singleeditor::SingleTrackDocument::CommittedStateSnapshot singleeditor::SingleTrackDocument::WorkspaceSnapshot
    MEMBERS singleeditor::SingleTrackDocument::SingleTrackDocument
        singleeditor::SingleTrackDocument::~SingleTrackDocument
        singleeditor::SingleTrackDocument::addListener singleeditor::SingleTrackDocument::removeListener
        singleeditor::SingleTrackDocument::canUndo singleeditor::SingleTrackDocument::canRedo
        singleeditor::SingleTrackDocument::captureWorkspaceSnapshot
        singleeditor::SingleTrackDocument::requiresRecoverySaveAs singleeditor::SingleTrackDocument::getRecoveryOrigin
        singleeditor::SingleTrackDocument::getEnvelopePoints
        singleeditor::SingleTrackDocument::getAmbisonicOrientation
        singleeditor::SingleTrackDocument::getAmbisonicMonitorMode
        singleeditor::SingleTrackDocument::getAmbisonicBinauralStatus
    TRIGGER "Ordinary document state and readiness queries including initial load ownership and passive command state"
    RETENTION "Shared immutable state and snapshots through playback, retained workspace and history consumers. No recovery or DSP service activation")
single_editor_declare_code_unit(
    ID singleeditor.document-edit.mutation OWNER singleeditor.document-edit
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::SingleTrackDocument::OfflineAmplitudeEdit singleeditor::SingleTrackDocument::OfflineEditPipeline
        singleeditor::SingleTrackDocument::EqualLengthAudioMutationTransaction
        singleeditor::SingleTrackDocument::RenderedAudioStorage
    MEMBERS singleeditor::SingleTrackDocument::setChannelLayoutPreset
        singleeditor::SingleTrackDocument::resetEnvelope singleeditor::SingleTrackDocument::insertEnvelopePoint
        singleeditor::SingleTrackDocument::beginEnvelopeGesture singleeditor::SingleTrackDocument::endEnvelopeGesture
        singleeditor::SingleTrackDocument::moveEnvelopePoint singleeditor::SingleTrackDocument::removeEnvelopePoint
        singleeditor::SingleTrackDocument::applySelectionGain singleeditor::SingleTrackDocument::applyFadeIn
        singleeditor::SingleTrackDocument::applyFadeOut singleeditor::SingleTrackDocument::deleteSelection
        singleeditor::SingleTrackDocument::applyOfflineEditPipeline
        singleeditor::SingleTrackDocument::beginEqualLengthAudioMutation
        singleeditor::SingleTrackDocument::commitEqualLengthAudioMutation
        singleeditor::SingleTrackDocument::undo singleeditor::SingleTrackDocument::redo
        singleeditor::SingleTrackDocument::secondsToSampleRange singleeditor::SingleTrackDocument::copyBuffer
        singleeditor::SingleTrackDocument::materialiseAudioBufferIfNeeded
        singleeditor::SingleTrackDocument::pushUndoState singleeditor::SingleTrackDocument::commitState
    TRIGGER "Explicit GUI or CLI edit, layout selection, Undo or Redo with current document admission"
    RETENTION "Admitted transaction and immutable audio states through commit, cancellation or retained history"
    PDB_VFTABLE_CLASSES
        "singleeditor::SingleTrackDocument::RenderedAudioStorage::Reader" ".?AVReader@RenderedAudioStorage@SingleTrackDocument@singleeditor@@")
single_editor_declare_code_unit(
    ID singleeditor.core-file.workspace-restore OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor
    MEMBERS singleeditor::SingleTrackDocument::adoptWorkspaceSnapshot
    TRIGGER "Explicit retained-document activation or restoration of the complete previous workspace after a failed load"
    RETENTION "Restored document state, source identity, Undo and Redo until replacement or remaining readers release them")
single_editor_declare_code_unit(
    ID singleeditor.recovery.document-snapshot OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor
    MEMBERS singleeditor::SingleTrackDocument::captureCommittedState singleeditor::SingleTrackDocument::createRecoveredCopy
    TRIGGER "Post-Core recovery protection captures an exact committed head or an explicit verified restore creates a copy"
    RETENTION "Recovery capture or candidate owns complete state and history until publication, cancellation or adoption")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.document-monitor OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::SingleTrackDocument::setAmbisonicOrientation
        singleeditor::SingleTrackDocument::setAmbisonicMonitorMode
        singleeditor::SingleTrackDocument::applyPreparedBinauralMonitor
    TRIGGER "Explicit Ambisonic orientation or monitor selection and exact-state prepared binaural publication"
    RETENTION "Selected document monitor and filter bank until state replacement or last playback consumer release")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.document-envelope OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::SingleTrackDocument::getEnvelopeGainAt singleeditor::SingleTrackDocument::evaluateEnvelope
        singleeditor::SingleTrackDocument::isEnvelopeFlatUnity
    TRIGGER "Ordinary playback, waveform and source rendering evaluate the current immutable envelope"
    RETENTION "Stateless evaluation over document-owned envelope points shared with later editing and Save")
single_editor_declare_code_unit(
    ID singleeditor.document-save.eligibility OWNER singleeditor.document-save
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::SingleTrackDocument::canSaveToFile singleeditor::SingleTrackDocument::canSaveOverSourceFile
        singleeditor::SingleTrackDocument::canWriteExtension
    TRIGGER "Passive Save command eligibility and supported-extension queries for the current document"
    RETENTION "No writer or save transaction. Shared current state queries only")
single_editor_declare_code_unit(
    ID singleeditor.document-save.document-write OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::SingleTrackDocument::SaveSnapshot singleeditor::SingleTrackDocument::SaveWriteResult
        singleeditor::SingleTrackDocument::SavePublication singleeditor::SingleTrackDocument::SaveMetadataReport
    MEMBERS singleeditor::SingleTrackDocument::createSaveSnapshot
        singleeditor::SingleTrackDocument::writeSaveSnapshot
        singleeditor::SingleTrackDocument::markSaveSnapshotCommitted
        singleeditor::SingleTrackDocument::saveAsToFile
        singleeditor::SingleTrackDocument::saveToFile
        singleeditor::SingleTrackDocument::saveOverSourceFile
        singleeditor::SingleTrackDocument::writeStateAudioToFile
        singleeditor::SingleTrackDocument::createWriterFormatForExtension
        singleeditor::SingleTrackDocument::getDefaultSaveExtension
        singleeditor::SingleTrackDocument::markCurrentStateClean
    TRIGGER "Explicit Save, Save As or Export snapshot and verified publication"
    RETENTION "Owned save snapshot, source witness, writer and publication result through completion")
# Reader admission and later Save or history consumers share implementation.
# Only the concrete entry points below determine lifecycle, not the whole source.
single_editor_declare_code_unit(
    ID singleeditor.core-file.document-reader-helpers OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS "singleeditor::`anonymous namespace'::allocateMaterialisedBuffer"
        "singleeditor::`anonymous namespace'::createReaderWithFormat" "singleeditor::`anonymous namespace'::sourceFactsForReader"
        "singleeditor::`anonymous namespace'::createMappedReaderWithFormat" "singleeditor::`anonymous namespace'::createDirectStandardAudioReaderForFile"
        "singleeditor::`anonymous namespace'::createDirectMappedReaderForFile" "singleeditor::`anonymous namespace'::parseWaveChannelMask"
        "singleeditor::`anonymous namespace'::ambiXPresetIdForMetadata" "singleeditor::`anonymous namespace'::makeChannelLayoutForMetadata"
        "singleeditor::`anonymous namespace'::makeFileInfoFromReader" "singleeditor::`anonymous namespace'::setPreparedReaderInfo"
        "singleeditor::`anonymous namespace'::supportsMemoryMappedOpen" "singleeditor::`anonymous namespace'::shouldMapEntireFileOnOpen"
        "singleeditor::`anonymous namespace'::createMemoryMappedReaderForFile"
    TRIGGER "Ordinary source admission, direct reader selection and plain source/channel facts"
    RETENTION "Reader-owned audio and facts shared with later Save and retained-history consumers")
single_editor_declare_code_unit(
    ID singleeditor.core-file.fallback-registry OWNER singleeditor.core-file
    LIFECYCLE BackendLazy SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES "singleeditor::`anonymous-namespace'::ThreadLocalReadFormatRegistry"
    MEMBERS "singleeditor::`anonymous namespace'::registerReadFormats" "singleeditor::`anonymous namespace'::getThreadLocalReadFormatManager"
    TRIGGER "Format fallback after direct reader failure or explicit retained-source reader request"
    RETENTION "Thread-local format descriptors constructed only on first fallback-registry use")
single_editor_declare_code_unit(
    ID singleeditor.metadata.document-timeline OWNER singleeditor.metadata
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES "singleeditor::`anonymous-namespace'::CuePointMetadata"
    PDB_CLASSES "singleeditor::`anonymous-namespace'::transformCueMetadata::__l2::<lambda_1>"
    MEMBERS "singleeditor::`anonymous namespace'::isTimelineMetadata" "singleeditor::`anonymous namespace'::parseNonNegativeMetadataValue"
        "singleeditor::`anonymous namespace'::removeIndexedMetadata" "singleeditor::`anonymous namespace'::markTimelineMetadata"
        "singleeditor::`anonymous namespace'::transformCueMetadata" "singleeditor::`anonymous namespace'::transformSamplerLoopMetadata"
        "singleeditor::`anonymous namespace'::transformTimelineMetadataForDeletion"
    TRIGGER "Explicit timeline edit or Save metadata classification on the current document"
    RETENTION "Transformed metadata values retained by the resulting immutable document state and history")
single_editor_declare_code_unit(
    ID singleeditor.document-save.metadata-helpers OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS "singleeditor::`anonymous namespace'::readFourCC" "singleeditor::`anonymous namespace'::addMetadataLabel"
        "singleeditor::`anonymous namespace'::inspectContainerMetadata" "singleeditor::`anonymous namespace'::isOggMetadataKey"
        "singleeditor::`anonymous namespace'::buildSaveMetadataReport" "singleeditor::`anonymous namespace'::makeWriterChannelLayoutForWaveMask"
        "singleeditor::`anonymous namespace'::shouldPreserveWaveChannelMaskOnWrite"
    TRIGGER "Explicit Save snapshot metadata inspection and writer channel-mask preparation"
    RETENTION "Request-local container read and metadata report with immutable Save snapshot ownership")
single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Audio/SourceFileLease.h"
    ID singleeditor.core-file.source-lease OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Audio/SourceFileLease.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::media::SourceFileWitness singleeditor::media::SourceFileLease::Impl
        "singleeditor::media::`anonymous-namespace'::Handle"
    MEMBERS singleeditor::media::SourceFileLease::SourceFileLease
        singleeditor::media::SourceFileLease::~SourceFileLease
        singleeditor::media::SourceFileLease::open
        singleeditor::media::SourceFileLease::openInternal
        singleeditor::media::SourceFileLease::recheck
        singleeditor::media::SourceFileLease::protectsInput
        "singleeditor::media::`anonymous namespace'::observe"
        "singleeditor::media::`anonymous namespace'::ticks"
    TRIGGER "Ordinary physical reader admission and shared source version checks for later explicit consumers"
    RETENTION "Admission lease closes after reader preparation. Immutable witness follows document state and shared history")
single_editor_declare_code_unit(
    ID singleeditor.core-file.native-path-gui OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Diagnostics/ReproductionFiles.cpp TARGETS SingleEditor
    MEMBERS singleeditor::files::nativePath
    TRIGGER "Shared Unicode and extended-path spelling required by ordinary physical reader admission"
    RETENTION "Call-local I/O path string with actual GUI header emission in ReproductionFiles, not diagnostic-only behavior")
single_editor_declare_code_unit(
    ID singleeditor.core-file.native-path-cli OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditorCLI
    MEMBERS singleeditor::files::nativePath
    TRIGGER "Shared Unicode and extended-path spelling required by explicit CLI ordinary file admission"
    RETENTION "Call-local I/O path string emitted from the actual CLI document source")
single_editor_declare_code_unit(
    ID singleeditor.document-save.source-publication OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/Audio/SourceFileLease.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::media::SourceFileLease::openStagedWrite
        singleeditor::media::SourceFileLease::openReplacementSource
        singleeditor::media::SourceFileLease::preserveReplacedInput
        singleeditor::media::SourceFileLease::publishTo
    TRIGGER "Explicit Save worker opens staged output and publishes the verified replacement"
    RETENTION "Save transaction until publication. Replaced input remains only while real document history retains its witness")
single_editor_declare_code_unit(
    ID singleeditor.core-file.retained-input OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/Audio/SourceFileLease.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::media::SourceFileLease::RetainedStream
    MEMBERS singleeditor::media::SourceFileLease::createRetainedInputStream
        singleeditor::media::SourceFileLease::readAt
    TRIGGER "Independent waveform, save or recovery reading of a source retained after explicit replacement"
    PDB_VFTABLE_CLASSES
        "singleeditor::media::SourceFileLease::RetainedStream" ".?AVRetainedStream@SourceFileLease@media@singleeditor@@"
    RETENTION "Independent cursor and shared retained file handle through the consuming reader lifetime")
single_editor_declare_code_unit(
    ID singleeditor.core-file.retained-reader OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/Document/SingleTrackDocument.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::SingleTrackDocument::createReaderForRetainedSource
    TRIGGER "Explicit history-backed read after source replacement for waveform, save or recovery"
    RETENTION "Independent decoder reader owns its retained source stream until that consumer releases it")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.binding OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::AudioEngine::AudioEngine singleeditor::AudioEngine::~AudioEngine
        singleeditor::AudioEngine::addListener singleeditor::AudioEngine::removeListener
        singleeditor::AudioEngine::loadFile singleeditor::AudioEngine::updateFromDocumentState
        singleeditor::AudioEngine::documentChanged
        singleeditor::AudioEngine::documentIdentityChanged
        singleeditor::AudioEngine::clearSourceChannelSoloMute
        singleeditor::AudioEngine::clampSeconds
        singleeditor::AudioEngine::getLoadedFileInfo
        singleeditor::AudioEngine::hasLoadedFile
        singleeditor::AudioEngine::getCurrentPositionSeconds
        singleeditor::AudioEngine::getDurationSeconds
        singleeditor::AudioEngine::getPlaybackState
        singleeditor::AudioEngine::getPlayableSampleRange
        singleeditor::AudioEngine::getPlayableTimeRange
        singleeditor::AudioEngine::canPlayAtSeconds
        singleeditor::AudioEngine::isSelectedOutputReady
        singleeditor::AudioEngine::warmUpAudioDeviceAsync
        singleeditor::AudioEngine::initialiseAudioDevice
        singleeditor::AudioEngine::ensureAudioDeviceReady
        singleeditor::AudioEngine::reserveExplicitStartupOutput
        singleeditor::AudioEngine::setPlaybackState
        singleeditor::AudioEngine::updatePositionFromTransport
        singleeditor::AudioEngine::play
        singleeditor::AudioEngine::stop
        singleeditor::AudioEngine::seekToSeconds
        singleeditor::AudioEngine::isPlaying
    TRIGGER "Ordinary document transport binding, actual selected output readiness and admitted initial playback"
    RETENTION "Current document transport and selected audio output through route replacement or shutdown")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.controls OWNER singleeditor.core-playback
    LIFECYCLE CoreDeferred SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor
    MEMBERS singleeditor::AudioEngine::pause singleeditor::AudioEngine::seekBySeconds singleeditor::AudioEngine::stepByFrames
    TRIGGER "Explicit GUI transport pause, relative seek or frame-step command after document admission"
    RETENTION "Existing transport and current playback position. No new optional analysis consumer")
single_editor_declare_code_unit(
    ID singleeditor.audio-output.engine-transactions OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::AudioEngine::DeviceLease
    MEMBERS singleeditor::AudioEngine::getDeviceManagerForConfiguration singleeditor::AudioEngine::getDeviceProbeConfiguration
        singleeditor::AudioEngine::takeDeviceLeaseForProbe singleeditor::AudioEngine::takeDeviceLeaseForSwitch
        singleeditor::AudioEngine::takeDeviceLease singleeditor::AudioEngine::returnDeviceLease
        singleeditor::AudioEngine::rebindTransportForRoute singleeditor::AudioEngine::commitDeviceLease
        singleeditor::AudioEngine::makeDeviceRequest singleeditor::AudioEngine::ensureDeviceRequestState
        singleeditor::AudioEngine::finishDeviceRequest singleeditor::AudioEngine::selectAudioDeviceType
        singleeditor::AudioEngine::applyAudioDeviceSetup singleeditor::AudioEngine::enableMaximumDeviceOutputs
        singleeditor::AudioEngine::preferMaximumOutputChannelsForCurrentDevice singleeditor::AudioEngine::pingSpeakerRole
        singleeditor::AudioEngine::readManagedOutputEvents singleeditor::AudioEngine::activeOutputSnapshot
        singleeditor::AudioEngine::refreshManagedOutputLatency
    TRIGGER "Selected startup output or explicit output configuration, switch, probe, ping or managed device event"
    RETENTION "Exact device lease and selected output through transaction completion, restoration or shutdown")
single_editor_declare_code_unit(
    ID singleeditor.output-meter.engine-consumer OWNER singleeditor.output-meter
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::AudioEngine::OutputMeterAnalysisLease singleeditor::AudioEngine::TransportRenderSource::MeterState
    MEMBERS singleeditor::AudioEngine::acquireOutputMeterAnalysisLease
        singleeditor::AudioEngine::TransportRenderSource::startOutputMeter
        singleeditor::AudioEngine::TransportRenderSource::stopOutputMeter
        singleeditor::AudioEngine::publishOutputMeterSnapshot
    TRIGGER "First visible post-Core meter or explicit analysis consumer acquires an output-meter lease"
    RETENTION "Real meter filters and history exist through active consumers and retire on the last control-thread release")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.engine-consumers OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::AudioEngine::AmbisonicFieldAnalysisLease
    MEMBERS singleeditor::AudioEngine::acquireAmbisonicFieldAnalysisLease singleeditor::AudioEngine::getAmbisonicFieldSnapshot
        singleeditor::AudioEngine::DocumentAudioSource::acquireAmbisonicFieldAnalysisLease
        singleeditor::AudioEngine::DocumentAudioSource::releaseAmbisonicFieldAnalysisLease
        singleeditor::AudioEngine::DocumentAudioSource::getAmbisonicFieldSnapshot
        singleeditor::AudioEngine::setAmbisonicOrientationPreview singleeditor::AudioEngine::activateAmbisonicDecodeMatrixArtifact
        singleeditor::AudioEngine::previewAmbisonicDecodeMatrixArtifact
        singleeditor::AudioEngine::DocumentAudioSource::setAmbisonicOrientationPreview
        singleeditor::AudioEngine::DocumentAudioSource::activateAmbisonicDecodeMatrixArtifact
        singleeditor::AudioEngine::DocumentAudioSource::previewAmbisonicDecodeMatrixArtifact
    TRIGGER "Explicit Ambisonic analysis consumer, orientation preview or speaker-matrix activation and preview"
    RETENTION "Active field consumer or admitted monitor action. Mixed ordinary-state reconciliation helpers remain separate")
single_editor_declare_code_unit(
    ID singleeditor.diagnostics.engine-observation OWNER singleeditor.diagnostics
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor
    MEMBERS singleeditor::AudioEngine::getDeviceDiagnostics singleeditor::AudioEngine::DocumentAudioSource::renderDiagnostics
    TRIGGER "Explicit Local Diagnostics provider copies current device and renderer facts without warming or opening a device"
    RETENTION "Bounded copied diagnostic snapshot through the current review or export")
single_editor_declare_code_unit(
    ID singleeditor.cli-automation.engine-render-probe OWNER singleeditor.cli-automation
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditorCLI
    MEMBERS singleeditor::AudioEngine::renderTransportBlockForDiagnostics
    TRIGGER "Explicit CLI RoutingInspectionService renders a requested probe block without opening an output device"
    RETENTION "Inspection-owned offline transport and destination block through the requested probe")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.waveform-model OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/Waveform/WaveformModel.cpp TARGETS SingleEditor
    SCOPES singleeditor::WaveformModel::PeakBuildJob
        singleeditor::WaveformModel::PeakCache
        "singleeditor::`anonymous-namespace'::StateReaderHandle"
    MEMBERS singleeditor::WaveformModel::WaveformModel
        singleeditor::WaveformModel::~WaveformModel
        singleeditor::WaveformModel::hasWaveform
        singleeditor::WaveformModel::getLengthInSeconds
        singleeditor::WaveformModel::getChannelCount
        singleeditor::WaveformModel::getState
        singleeditor::WaveformModel::getChannelMinMax
        singleeditor::WaveformModel::getEnvelopeGainAt
        singleeditor::WaveformModel::getPeakCache
        singleeditor::WaveformModel::isPeakCacheReady
        singleeditor::WaveformModel::adoptPeakCache
        singleeditor::WaveformModel::buildLeadingPreviewPeakCacheFromReader
        singleeditor::WaveformModel::buildLeadingPreviewPeakCacheForState
        singleeditor::WaveformModel::loadPersistentStartupPreviewForFile
        singleeditor::WaveformModel::lookupPersistentPeakCacheHeaderForFile
        singleeditor::WaveformModel::documentChanged
        singleeditor::WaveformModel::schedulePeakCacheBuild
        singleeditor::WaveformModel::publishPeakCache
        singleeditor::WaveformModel::queuePeakCachePersistence
        singleeditor::WaveformModel::cancelPendingPeakCachePersistence
        singleeditor::WaveformModel::getLatestBuildRequestId
        singleeditor::WaveformModel::addChangeListener
        singleeditor::WaveformModel::removeChangeListener
        singleeditor::WaveformModel::loadPersistentPeakCache
        singleeditor::WaveformModel::hasPersistentStartupPreviewCache
        singleeditor::WaveformModel::prepareIncomingCaches
        singleeditor::WaveformModel::getSpectrogramCache
        singleeditor::WaveformModel::getSpectrogramStatus
        "singleeditor::`anonymous namespace'::loadPersistentDisplayPeakCache"
        "singleeditor::`anonymous namespace'::openReaderForState"
        "singleeditor::`anonymous namespace'::buildPeakLodPyramid"
        "singleeditor::`anonymous namespace'::makeCacheKey"
        "singleeditor::`anonymous namespace'::initialisePeakLevelStorage"
        "singleeditor::`anonymous namespace'::hasMatchingPersistentCacheHeader"
        "singleeditor::`anonymous namespace'::readPeakValues"
        "singleeditor::`anonymous namespace'::getWaveformCacheDirectory"
        "singleeditor::`anonymous namespace'::inferFormatNameFromFile"
        "singleeditor::`anonymous namespace'::fillPeakTimeExtents"
        "singleeditor::`anonymous namespace'::getWaveformCacheFile"
        "singleeditor::`anonymous namespace'::rebuildMixedBins"
        "singleeditor::`anonymous namespace'::getStateTotalSamples"
        "singleeditor::`anonymous namespace'::getStateChannelCount"
        "singleeditor::`anonymous namespace'::canUseFileCache"
        "singleeditor::`anonymous namespace'::sanitisePeakLevelBins"
        "singleeditor::`anonymous namespace'::chooseStartupDisplayBinCount"
        "singleeditor::`anonymous namespace'::fillBinTimeExtents"
        "singleeditor::`anonymous namespace'::chooseBinCount"
    PDB_METHODS "`singleeditor::`anonymous namespace'::buildProgressivePeakCacheForState<`singleeditor::WaveformModel::PeakBuildJob::runJob'::`8'::<lambda_1> const &,`singleeditor::WaveformModel::PeakBuildJob::runJob'::`11'::<lambda_2> >'::`2'::<lambda_2>::operator()"
    TRIGGER "Ordinary initial preview, source-backed waveform peaks and current document peak publication"
    PDB_VFTABLE_CLASSES
        "singleeditor::WaveformModel::PeakBuildJob" ".?AVPeakBuildJob@WaveformModel@singleeditor@@"
    RETENTION "Current peak cache, reader and bounded peak worker through generation replacement")

# Complete recovered peaks belong to the admitted restore candidate. The common
# readers/cache primitives above also serve ordinary first-waveform preparation.
single_editor_declare_code_unit(
    ID singleeditor.recovery.complete-peaks OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE Source/Waveform/WaveformModel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::WaveformModel::buildCompletePeakCacheForState
    PDB_FUNCTIONS
        "singleeditor::`anonymous namespace'::accumulatePeakBinsFromReader<`singleeditor::WaveformModel::buildCompletePeakCacheForState'::`2'::<lambda_1> >"
        "singleeditor::`anonymous namespace'::buildPeakCacheForState<`singleeditor::WaveformModel::buildCompletePeakCacheForState'::`2'::<lambda_1> >"
    TRIGGER "An admitted RecoveryRestoreCandidate reads its recovered state to publish complete restored peaks"
    RETENTION "Restore-owned recovered reader and peak result until cancellation or current candidate publication")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.engine-consumer OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditor SingleEditorCLI
    MEMBERS singleeditor::AudioEngine::acquireSpatialEnergyAnalysisLease
        singleeditor::AudioEngine::refreshSpatialEnergyAnalysis
    TRIGGER "An explicit post-Core lease or visible admitted Spatial Energy panel refreshes the existing output snapshot"
    RETENTION "Lease-owned analysis configuration without opening devices or creating ordinary startup consumers")

set(SINGLE_EDITOR_RECOVERY_STORAGE_SOURCES
    Source/Recovery/RecoveryStore.cpp
    Source/Recovery/RecoveryDiscovery.cpp
    Source/Recovery/RecoveryInspection.cpp
)

set(SINGLE_EDITOR_RECOVERY_WRITER_SOURCES
    Source/Recovery/AsyncRecoveryWriter.cpp
    Source/Recovery/RecoveryMutationProducer.cpp
    Source/Recovery/RecoveryProtectionSession.cpp
    Source/Recovery/RecoverySessionHost.cpp
    Source/Recovery/RecoveryMaintenanceQueue.cpp
    Source/Recovery/RecoveryService.cpp
    Source/Recovery/RecoveryOwnerLifecycle.cpp
    Source/Recovery/RecoveryAudioGraph.cpp
    Source/Recovery/ProgressiveRecoveryAudio.cpp
    Source/Recovery/RecoveryCheckpointCodec.cpp
)

set(SINGLE_EDITOR_RECOVERY_RESTORE_SOURCES
    Source/Recovery/RecoveryRestoreCandidate.cpp
    Source/Recovery/RecoveryAttemptStore.cpp
)

set(SINGLE_EDITOR_RECOVERY_BINDING_SOURCES
    Source/Recovery/RecoverySourceIdentity.cpp
    Source/Recovery/RecoverySourceBinding.cpp
)

# Explicit product owner sets, separate from target source ordering. The native
# service is created after CoreReady or by an explicit recovery action; these
# sources do not supply the cold ordinary document/transport implementation.
foreach(source IN ITEMS
        ${SINGLE_EDITOR_RECOVERY_STORAGE_SOURCES}
        ${SINGLE_EDITOR_RECOVERY_WRITER_SOURCES}
        ${SINGLE_EDITOR_RECOVERY_RESTORE_SOURCES}
        ${SINGLE_EDITOR_RECOVERY_BINDING_SOURCES}
        Source/Recovery/RecoveryRestoreQueue.cpp
        Source/Document/RecoveredPlaylistDocument.cpp
        Source/UI/Components/RecoveryCenterComponent.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.recovery.source.${unit_name}" OWNER singleeditor.recovery
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Post-Core protection or discovery, or explicit Recovery Center and restore action"
        RETENTION "Recovery service, admitted work, protected document and reader leases through drain or document release")
endforeach()

# Diagnostic collection and explicit backend probes have separate lifecycle
# declarations even where the GUI and CLI share the implementation.
foreach(unit_name IN ITEMS DiagnosticBundle DiagnosticSnapshot ApplicationDiagnosticIdentity)
    string(TOLOWER "${unit_name}" unit_id)
    single_editor_declare_code_unit(
        ID "singleeditor.diagnostics.source.${unit_id}" OWNER singleeditor.diagnostics
        LIFECYCLE ModuleLazy SOURCE "Source/Diagnostics/${unit_name}.cpp" WHOLE_SOURCE
        TARGETS SingleEditor SingleEditorCLI
        TRIGGER "Explicit GUI diagnostics review/export or CLI diagnostics command"
        RETENTION "Copied diagnostic snapshot and reviewed export request, released on completion or cancellation")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.diagnostics.source.reproductionfiles OWNER singleeditor.diagnostics
    LIFECYCLE ModuleLazy SOURCE Source/Diagnostics/ReproductionFiles.cpp
    SCOPES singleeditor::diagnostics::ReproductionFiles TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit reviewed diagnostic reproduction-file capture"
    RETENTION "Bounded copied inputs for the requested export without assigning shared header path helpers to diagnostics")
foreach(unit_name IN ITEMS CodecDiagnosticProbe)
    string(TOLOWER "${unit_name}" unit_id)
    single_editor_declare_code_unit(
        ID "singleeditor.diagnostics.source.${unit_id}" OWNER singleeditor.diagnostics
        LIFECYCLE BackendLazy SOURCE "Source/Diagnostics/${unit_name}.cpp" WHOLE_SOURCE
        TARGETS SingleEditor SingleEditorCLI
        TRIGGER "Explicit codec or audio-device diagnostic probe request"
        RETENTION "Bounded probe worker and its owned process or device, drained on completion or cancellation")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.diagnostics.source.devicediagnosticprobe OWNER singleeditor.diagnostics
    LIFECYCLE BackendLazy SOURCE Source/Diagnostics/DeviceDiagnosticProbe.cpp
    SCOPES singleeditor::diagnostics::DeviceDiagnosticProbe
        "singleeditor::diagnostics::`anonymous-namespace'::ProbeFiles"
        "singleeditor::diagnostics::`anonymous-namespace'::Callback"
        "singleeditor::diagnostics::`anonymous-namespace'::observe::__l38::StopBeforeCallbackDestruction"
    MEMBERS "singleeditor::diagnostics::`anonymous namespace'::reason"
        "singleeditor::diagnostics::`anonymous namespace'::integer"
        "singleeditor::diagnostics::`anonymous namespace'::observe"
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit requested device diagnostic probe and its bounded worker callback"
    RETENTION "Probe-owned request, temporary files and device callback without claiming shared device factories as diagnostic-only ownership")
foreach(unit_name IN ITEMS GuiDeviceProbe NativeDeviceMetadata)
    string(TOLOWER "${unit_name}" unit_id)
    single_editor_declare_code_unit(
        ID "singleeditor.diagnostics.source.${unit_id}" OWNER singleeditor.diagnostics
        LIFECYCLE BackendLazy SOURCE "Source/Diagnostics/${unit_name}.cpp" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Explicit GUI device diagnostic request or copied active-endpoint metadata enrichment"
        RETENTION "Request-owned device probe or read-only metadata worker until completion or cancellation")
endforeach()
foreach(source IN ITEMS Source/Diagnostics/LiveDiagnosticProviders.cpp Source/UI/Components/LocalDiagnosticsComponent.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.diagnostics.source.${unit_name}" OWNER singleeditor.diagnostics
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Explicit GUI local diagnostics window and owner-thread snapshot request"
        RETENTION "Diagnostics window and copied provider snapshot, workers drained on close")
endforeach()

single_editor_declare_code_unit(
    ID singleeditor.vgmstream.decode OWNER singleeditor.vgmstream
    LIFECYCLE BackendLazy SOURCE Source/Audio/VgmstreamDecodeService.cpp WHOLE_SOURCE
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Identified WEM source, bank media preview, or explicit decoder diagnostics"
    RETENTION "Admitted decoder work, published decoded reader and cache source leases")

# Shared video/WEM/bank request state has one concrete owner. It is constructed
# only by optional requests, and is also observed by explicit diagnostics.
foreach(unit_name IN ITEMS OptionalMediaFailureState OptionalMediaOperation)
    string(TOLOWER "${unit_name}" unit_id)
    single_editor_declare_code_unit(
        ID "singleeditor.optional-media.${unit_id}" OWNER singleeditor.optional-media
        LIFECYCLE ModuleLazy SOURCE "Source/Audio/${unit_name}.cpp" WHOLE_SOURCE
        TARGETS SingleEditor SingleEditorCLI
        TRIGGER "Explicit video, packed audio or bank request and observation of its existing request state"
        RETENTION "Lazy session failure policy and request-owned state until completion or cancellation")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.optional-media.process OWNER singleeditor.optional-media
    LIFECYCLE BackendLazy SOURCE Source/Audio/BoundedDecodeProcess.cpp WHOLE_SOURCE
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Admitted optional decoder or explicit codec and device diagnostic process request"
    RETENTION "Request-owned process, output observation and cleanup through completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.optional-media.cache-work OWNER singleeditor.optional-media
    LIFECYCLE BackendLazy SOURCE Source/Audio/DecodedWaveCache.cpp
    SCOPES singleeditor::media::DecodedWaveCache::Context singleeditor::media::DecodedWaveCache::WaveFacts
    MEMBERS singleeditor::media::DecodedWaveCache::capture singleeditor::media::DecodedWaveCache::inspectComplete
        singleeditor::media::DecodedWaveCache::sealFile singleeditor::media::DecodedWaveCache::seal
        singleeditor::media::DecodedWaveCache::reusable
        "singleeditor::media::`anonymous namespace'::digest" "singleeditor::media::`anonymous namespace'::stamp"
        "singleeditor::media::`anonymous namespace'::current" "singleeditor::media::`anonymous namespace'::le32"
        "singleeditor::media::`anonymous namespace'::le16" "singleeditor::media::`anonymous namespace'::hashBytes"
        "singleeditor::media::`anonymous namespace'::bodyHash"
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Admitted video or VGM decoder cache lookup and publication"
    RETENTION "Request-local cache facts and streams with persistent cache files governed by their existing owner")
single_editor_declare_code_unit(
    ID singleeditor.optional-media.cache-observation OWNER singleeditor.optional-media
    LIFECYCLE ModuleLazy SOURCE Source/Audio/DecodedWaveCache.cpp
    MEMBERS singleeditor::media::DecodedWaveCache::lookupReasonName
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit diagnostics formatting of an already observed cache outcome"
    RETENTION "Static reason labels only without cache access or worker creation")
# Extension classification is used by ordinary document opening. It must not
# inherit the lazy execution class of the actual Media Foundation operations.
single_editor_declare_code_unit(
    ID singleeditor.core-file.video-extension OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/Audio/WindowsMediaAudioService.cpp
    MEMBERS singleeditor::WindowsMediaAudioService::isSupportedVideoExtension
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Ordinary document source classification before choosing a reader"
    RETENTION "Stateless extension predicate without codec initialization")
single_editor_declare_code_unit(
    ID singleeditor.windows-media.execution OWNER singleeditor.windows-media
    LIFECYCLE BackendLazy SOURCE Source/Audio/WindowsMediaAudioService.cpp
    MEMBERS singleeditor::WindowsMediaAudioService::encodeWaveFileToMp3
        singleeditor::WindowsMediaAudioService::getOrCreateVideoAudioCache
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit MP3 Save/export or admitted video-audio decode request"
    RETENTION "Process-retained Media Foundation API binding, request-owned codec and file workers")
single_editor_declare_code_unit(
    ID singleeditor.windows-media.execution-helpers OWNER singleeditor.windows-media
    LIFECYCLE BackendLazy SOURCE Source/Audio/WindowsMediaAudioService.cpp
    SCOPES "singleeditor::`anonymous-namespace'::ScopedCom"
        "singleeditor::`anonymous-namespace'::MediaFoundationRuntime"
    MEMBERS singleeditor::decodeVideoAudioWithFfmpeg
        "singleeditor::`anonymous namespace'::runtime" "singleeditor::`anonymous namespace'::setAttribute"
        "singleeditor::`anonymous namespace'::createWaveReader" "singleeditor::`anonymous namespace'::writePcmSample"
        "singleeditor::`anonymous namespace'::videoCacheFileFor" "singleeditor::`anonymous namespace'::findFfmpegExecutable"
    TARGETS SingleEditor SingleEditorCLI
    MEMBERS "singleeditor::mediaRuntimeLoaded"
    TRIGGER "Admitted MP3 encoding or video decoding after its explicit execution entry point"
    RETENTION "Codec runtime and request-local COM, PCM and decoder lookup helpers")

# Shared dependencies get explicit declarations at their earliest actual use.
# This charges existing ordinary text shaping and playback-rate preparation to
# CoreEager; it does not permit new startup work, imports or blanket JUCE ownership.
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.main-presentation OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp
    MEMBERS singleeditor::MainComponent::MainComponent singleeditor::MainComponent::~MainComponent
        singleeditor::MainComponent::paint singleeditor::MainComponent::resized
        singleeditor::MainComponent::lookAndFeelChanged singleeditor::MainComponent::refreshUiState
        singleeditor::MainComponent::refreshWindowAndTitle singleeditor::MainComponent::refreshFooterReadout
        singleeditor::MainComponent::refreshTransportReadout singleeditor::MainComponent::updateTransportAvailability
        singleeditor::MainComponent::updateSelectionPopup singleeditor::MainComponent::applyChromePresentation
        singleeditor::MainComponent::timerCallback
    TARGETS SingleEditor
    PDB_CLASSES
        "singleeditor::MainComponent::updateTransportAvailability::__l2::<lambda_1>"
    MEMBERS "singleeditor::MainComponent::formatAudioDetails"
    MEMBERS "singleeditor::MainComponent::componentMovedOrResized"
        "singleeditor::MainComponent::componentParentHierarchyChanged"
        "singleeditor::MainComponent::componentVisibilityChanged"
        "singleeditor::MainComponent::formatMeterReadout"
        "singleeditor::MainComponent::formatPlaybackState"
        "singleeditor::MainComponent::formatTimecode"
        "singleeditor::MainComponent::paintOverChildren"
        "singleeditor::MainComponent::playbackStateChanged"
        "singleeditor::MainComponent::refreshTimelineMarkers"
        "singleeditor::MainComponent::repaintMetadataSidebarIfVisible"
        "singleeditor::MainComponent::setDocumentOperationStatus"
        "singleeditor::MainComponent::updateCenterViewVisibility"
        "singleeditor::MainComponent::updateSidebarToggleButtons"
        "singleeditor::ModernStatusLabel::getTooltip"
        "singleeditor::ModernStatusLabel::paint"
        "singleeditor::ModernStatusLabel::setModernText"
        "singleeditor::`anonymous namespace'::configureActionButton"
        "singleeditor::`anonymous namespace'::configureLabel"
        "singleeditor::`anonymous namespace'::configureTransportButton"
        "singleeditor::`anonymous namespace'::configureTransportToggle"
        "singleeditor::`anonymous namespace'::setButtonIcon"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::MainComponent'::`2'::<lambda_5>::operator()'::`8'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::MainComponent::paintOverChildren'::`7'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "`singleeditor::MainComponent::MainComponent'::`2'::<lambda_47>::operator()"
        "`singleeditor::MainComponent::resized'::`14'::<lambda_1>::operator()"
        "`singleeditor::MainComponent::resized'::`49'::<lambda_3>::operator()"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_53>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_54>,void,std::optional<juce::Range<double> > >::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_5>,void,bool>::_Do_call"
    TRIGGER "Initial ordinary main-content construction, presentation, transport state and shared UI timer"
    RETENTION "Main editor presentation and callbacks through window release without eagerly creating optional consumers")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.core-command-controller OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp
    MEMBERS singleeditor::MainComponent::registerCoreCommandProvider
        singleeditor::MainComponent::resolveCoreCommandState singleeditor::MainComponent::performCoreCommand
        singleeditor::MainComponent::dispatchGuiCommand singleeditor::MainComponent::keyPressed
    TARGETS SingleEditor
    PDB_CLASSES
        "singleeditor::MainComponent::keyPressed::__l2::<lambda_1>"
    MEMBERS "singleeditor::MainComponent::makeCommandContext"
    MEMBERS "singleeditor::GuiCommandProvider::GuiCommandProvider"
    TRIGGER "Initial minimal playback provider, shared ordinary command state, dispatch and keyboard input"
    RETENTION "Core command routing shared with later editing commands while each optional handler keeps its own admission")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.command-actions OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::reportCommandResult"
        "singleeditor::MainComponent::currentShortcutFocusContext"
        "singleeditor::MainComponent::captureFocusReturnToken"
        "singleeditor::MainComponent::restoreFocusReturnToken"
        "singleeditor::MainComponent::componentForSemanticAnchor"
        "singleeditor::MainComponent::cycleMajorRegionFocus"
        "singleeditor::MainComponent::handleTopLevelKeyPress"
    PDB_CLASSES "singleeditor::MainComponent::restoreFocusReturnToken::__l2::<lambda_1>"
    PDB_METHODS "`juce::MessageManager::callAsync<`singleeditor::MainComponent::restoreFocusReturnToken'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::componentForSemanticAnchor'::`2'::<lambda_1>,juce::Component *,juce::Component *>::_Do_call"
    MEMBERS "singleeditor::FocusReturnToken::FocusReturnToken"
        "singleeditor::MainComponent::WindowDragZone::mouseDown"
        "singleeditor::MainComponent::WindowDragZone::mouseDrag"
        "singleeditor::MainComponent::dismissSelectionPopup"
        "singleeditor::MainComponent::setModernAppearanceTheme"
        "singleeditor::MainComponent::zoomTimeIn"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_11>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_12>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_13>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_14>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_15>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_16>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_17>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_19>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_21>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_22>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_23>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_24>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_25>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_26>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_27>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_28>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_29>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_2>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_30>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_31>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_32>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_33>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_36>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_37>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_38>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_39>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_3>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_40>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_41>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_42>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_43>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_44>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_45>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_46>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_4>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_51>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_55>,void,int,bool>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_56>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_6>,void,enum singleeditor::WaveformViewComponent::ViewMode>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_7>,void,enum singleeditor::WaveformViewComponent::DisplayMode>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_8>,void,enum singleeditor::WaveformOverviewComponent::Action>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::registerCoreCommandProvider'::`2'::<lambda_2>,singleeditor::GuiCommandResult,std::basic_string_view<char,std::char_traits<char> >,singleeditor::GuiCommandDispatchContext const &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::registerPostCoreCommandProvider'::`2'::<lambda_2>,singleeditor::GuiCommandResult,std::basic_string_view<char,std::char_traits<char> >,singleeditor::GuiCommandDispatchContext const &>::_Do_call"
    TRIGGER "Explicit keyboard, menu, dialog, palette and region-focus actions or their queued completion"
    RETENTION "Current command result and guarded focus-return token until action completion or view release")
single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/UI/Commands/GuiCommandModel.h"
    ID singleeditor.core-edit-ui.command-registry OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Commands/GuiCommandModel.cpp TARGETS SingleEditor
    SCOPES singleeditor::GuiCommandResult
    MEMBERS singleeditor::GuiCommandRegistry::registerProvider
        singleeditor::GuiCommandRegistry::unregisterProvider
        singleeditor::GuiCommandRegistry::setActiveDocumentGeneration
        singleeditor::GuiCommandRegistry::getActiveDocumentGeneration
        singleeditor::GuiCommandRegistry::resolve singleeditor::GuiCommandRegistry::dispatch
        singleeditor::GuiCommandRegistry::isValidCommandId
        "singleeditor::`anonymous namespace'::isCancelled"
    TRIGGER "Initial minimal playback provider and ordinary keyboard/toolbar command routing with current-generation checks"
    RETENTION "Window-owned registry and transient command state shared with separately admitted optional providers")
single_editor_declare_code_unit(
    ID singleeditor.workspace.controller OWNER singleeditor.workspace
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/MainComponent.cpp
    MEMBERS singleeditor::MainComponent::ensureWorkspaceModuleHost
        singleeditor::MainComponent::ensureDeferredWorkspaceControls
        singleeditor::MainComponent::registerWorkspaceCommandProvider
        singleeditor::MainComponent::resolveWorkspaceCommandState singleeditor::MainComponent::performWorkspaceCommand
    TARGETS SingleEditor
    PDB_CLASSES
        "singleeditor::MainComponent::performWorkspaceCommand::__l66::<lambda_1>"
        "singleeditor::MainComponent::performWorkspaceCommand::__l88::<lambda_2>"
        "singleeditor::MainComponent::resolveWorkspaceCommandState::__l2::<lambda_1>"
        "singleeditor::MainComponent::performWorkspaceCommand::__l94::<lambda_3>"
    PDB_METHODS "`juce::MessageManager::callAsync<`singleeditor::MainComponent::performWorkspaceCommand'::`88'::<lambda_2> >'::`2'::AsyncCallInvoker::messageCallback"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`13'::<lambda_1>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`16'::<lambda_2>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_10>,void,enum WorkspaceModuleId,bool>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_11>,void,enum WorkspaceModuleId,enum singleeditor::WorkspaceFloatingWindowCommand>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_12>,void,enum WorkspaceModuleId>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_13>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_14>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_9>,bool,enum WorkspaceModuleId>::_Do_call"
    MEMBERS "singleeditor::MainComponent::ensureDeferredOutputMeter"
        "singleeditor::MainComponent::setModernSimpleView"
        "singleeditor::`anonymous namespace'::removeCommandForModule"
        "singleeditor::`anonymous namespace'::sidebarModuleForWindowCommand"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_10>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::MainComponent'::`2'::<lambda_9>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureDeferredWorkspaceControls'::`7'::<lambda_3>,void,juce::File const &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::registerWorkspaceCommandProvider'::`2'::<lambda_1>,singleeditor::GuiCommandState,std::basic_string_view<char,std::char_traits<char> >,singleeditor::GuiCommandDispatchContext const &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::registerWorkspaceCommandProvider'::`2'::<lambda_2>,singleeditor::GuiCommandResult,std::basic_string_view<char,std::char_traits<char> >,singleeditor::GuiCommandDispatchContext const &>::_Do_call"
    TRIGGER "Empty-workspace control construction or CoreReady-gated control and workspace host attachment, followed by explicit workspace command requests"
    RETENTION "Workspace host and registered workspace command provider through document or main-view release")
single_editor_declare_code_unit(
    ID singleeditor.ambisonics.workspace-factory OWNER singleeditor.ambisonics
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_6>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted Ambisonic energy view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact Ambisonic energy factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.timeline-markers.workspace-factory OWNER singleeditor.timeline-markers
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`21'::<lambda_5>::operator()'::`2'::<lambda_1>,void,double>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`21'::<lambda_5>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted timeline markers view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact timeline markers factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.workspace-factory OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_4>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted spatial energy view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact spatial energy factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.output-meter.workspace-factory OWNER singleeditor.output-meter
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_3>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted output meter view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact output meter factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.workspace.playlist-session OWNER singleeditor.workspace
    LIFECYCLE PostCoreWorkspace SOURCE "Source/Document/PlaylistModel.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::PlaylistModel::restoreFiles"
    TRIGGER "Posted empty-workspace continuation restores the explicitly enabled previous playlist"
    RETENTION "Restored playlist entries without automatically loading a document")
get_target_property(intent_graphics_sources juce_graphics INTERFACE_SOURCES)
foreach(source IN LISTS intent_graphics_sources)
    if(source MATCHES "/juce_graphics_Harfbuzz\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            ID singleeditor.text-shaping.harfbuzz OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            WHOLE_SOURCE TARGETS SingleEditor
            TRIGGER "Ordinary initial window and waveform text shaping through JUCE Graphics and HarfBuzz"
            RETENTION "Linked text-shaping provider used by core and deferred UI text consumers")
    elseif(source MATCHES "/juce_graphics_Sheenbidi\\.c$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            ID singleeditor.text-shaping.sheenbidi OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            WHOLE_SOURCE TARGETS SingleEditor
            TRIGGER "Existing ordinary SimpleShapedText paragraph analysis and visual ordering"
            RETENTION "Dedicated linked SheenBidi provider and its Unicode tables shared by initial and later text")
    elseif(source MATCHES "/juce_graphics\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/colour/juce_Colour.cpp"
                "${intent_definition_modules_root}/juce_graphics/colour/juce_Colour.h"
                "${intent_definition_modules_root}/juce_graphics/colour/juce_ColourGradient.cpp"
                "${intent_definition_modules_root}/juce_graphics/colour/juce_ColourGradient.h"
                "${intent_definition_modules_root}/juce_graphics/contexts/juce_GraphicsContext.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_Font.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_Typeface.cpp"
                "${intent_definition_modules_root}/juce_graphics/geometry/juce_AffineTransform.cpp"
                "${intent_definition_modules_root}/juce_graphics/geometry/juce_AffineTransform.h"
                "${intent_definition_modules_root}/juce_graphics/geometry/juce_Path.cpp"
                "${intent_definition_modules_root}/juce_graphics/geometry/juce_Path.h"
                "${intent_definition_modules_root}/juce_graphics/geometry/juce_PathStrokeType.cpp"
                "${intent_definition_modules_root}/juce_graphics/native/juce_RenderingHelpers.h"
            ID singleeditor.core-edit-ui.graphics-primitives OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            # Shared 2D geometry, colour, font and raster primitives used by the
            # first ordinary waveform/text paint. Keep SVG/image codecs and the
            # rest of this mixed JUCE source outside this explicit scope set.
            SCOPES juce::Graphics juce::Path juce::PathStrokeType juce::AffineTransform
                juce::Colour juce::ColourGradient juce::Font juce::Typeface
                juce::EdgeTable juce::RenderingHelpers
            TARGETS SingleEditor
            PDB_FUNCTIONS "std::_Sort_unchecked<juce::EdgeTable::LineItem *,std::less<void> >"
            MEMBERS "juce::juce_getTypefaceForFont"
            TRIGGER "Ordinary first window, filename and actual waveform painting"
            PDB_VFTABLE_CLASSES
                "juce::RenderingHelpers::GlyphCache" ".?AVGlyphCache@RenderingHelpers@juce@@"
                "juce::RenderingHelpers::ClipRegions::EdgeTableRegion<juce::RenderingHelpers::SoftwareRendererSavedState>" ".?AU?$EdgeTableRegion@VSoftwareRendererSavedState@RenderingHelpers@juce@@@ClipRegions@RenderingHelpers@juce@@"
                "juce::Font::SharedFontInternal" ".?AVSharedFontInternal@Font@juce@@"
                "juce::RenderingHelpers::ClipRegions::RectangleListRegion<juce::RenderingHelpers::SoftwareRendererSavedState>" ".?AV?$RectangleListRegion@VSoftwareRendererSavedState@RenderingHelpers@juce@@@ClipRegions@RenderingHelpers@juce@@"
                "juce::Typeface" ".?AVTypeface@juce@@"
            RETENTION "Core graphics primitives shared by later UI consumers")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/detail/juce_Ranges.h"
                "${intent_definition_modules_root}/juce_graphics/detail/juce_ShapedText.cpp"
                "${intent_definition_modules_root}/juce_graphics/detail/juce_SimpleShapedText.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_FontOptions.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_FontOptions.h"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_GlyphArrangement.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_GlyphArrangement.h"
            ID singleeditor.text-shaping.fitted-text OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES
                "juce::detail::Ranges"
                "juce::detail::CanBreakBeforeIterator"
                "juce::detail::IntegralCanBreakBeforeIterator"
                "juce::detail::LineState"
                "juce::detail::LineOfWrappedGlyphCursorRanges"
                juce::FontOptions juce::GlyphArrangement juce::PositionedGlyph
                juce::detail::ShapedText juce::detail::SimpleShapedText
                juce::detail::JustifiedText juce::detail::Shaper
                "juce::detail::FillLinesOptions"
                "juce::detail::SanitisedString"
                "juce::detail::WrappedGlyphsCursor"
                "juce::detail::WrappedGlyphsCursorRange"
            MEMBERS juce::createFittedText juce::detail::lowLevelShape
                juce::detail::findSuitableFontsForText juce::addGlyphsFromShapedText
                juce::detail::makeSpan juce::detail::makeSubSpanLookup
            TARGETS SingleEditor
            PDB_CLASSES
                "juce::detail::RangedValues<juce::Font>"
                "juce::detail::RangedValuesIterator<juce::Font>"
                "juce::detail::SubSpanLookup<juce::Unicode::Codepoint const >"
                "juce::createFittedText::__l2::<lambda_1>"
                "juce::detail::IntersectingRangedValues<juce::Font const ,juce::detail::LineMetrics const ,enum juce::detail::JustifiedText::DrawType const ,float const >"
                "juce::detail::IntersectingRangedValues<juce::Font const ,juce::detail::LineMetrics const ,enum juce::detail::JustifiedText::DrawType const ,float const >::IntersectionIterator"
                "juce::detail::RangedValues<enum juce::detail::JustifiedText::DrawType>"
                "juce::detail::RangedValues<float>"
                "juce::detail::RangedValues<juce::detail::GlyphLookupEntry>"
                "juce::detail::RangedValues<juce::detail::GlyphsStorage>"
                "juce::detail::RangedValues<juce::detail::LineMetrics>"
                "juce::detail::RangedValues<juce::detail::ShapingParams>"
                "juce::detail::RangedValues<std::optional<juce::Font> >"
                "juce::detail::RangedValuesIterator<juce::detail::ShapingParams>"
            PDB_FUNCTIONS
                "std::_Med3_unchecked<juce::detail::WrappedGlyphsCursor::ShapedGlyphSpan *,`juce::detail::getShapedGlyphSpansInVisualOrder'::`2'::<lambda_1> >"
                "std::_Pop_heap_hole_by_index<juce::detail::WrappedGlyphsCursor::ShapedGlyphSpan *,juce::detail::WrappedGlyphsCursor::ShapedGlyphSpan,`juce::detail::getShapedGlyphSpansInVisualOrder'::`2'::<lambda_1> >"
                "std::_Sort_unchecked<juce::detail::WrappedGlyphsCursor::ShapedGlyphSpan *,`juce::detail::getShapedGlyphSpansInVisualOrder'::`2'::<lambda_1> >"
            MEMBERS "juce::detail::getHarfbuzzFeatures"
                "juce::detail::getScriptTag"
                "juce::detail::resolveFontsWithFallback"
                "juce::getBitmapLayer"
                "juce::getGlyphPathInGlyphUnits"
                "juce::isFontSuitableForCodepoint"
                "juce::withGlyphArrangementOptions"
            TRIGGER "Initial filename and waveform ruler drawText or drawFittedText through the existing shaping path"
            RETENTION "Shared font options, fitted glyph layout and shaped runs used by initial and later text")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/detail/juce_SimpleShapedText.h"
                "${intent_definition_modules_root}/juce_graphics/unicode/juce_Unicode.cpp"
                "${intent_definition_modules_root}/juce_graphics/unicode/juce_UnicodeGenerated.cpp"
                "${intent_definition_modules_root}/juce_graphics/unicode/juce_UnicodeUtils.cpp"
            ID singleeditor.text-shaping.unicode-analysis OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Unicode juce::UnicodeDataTable juce::UnicodeAnalysisPoint juce::UnicodeEntry
                juce::detail::ShapedTextOptions
            TARGETS SingleEditor
            MEMBERS "juce::BreakPairTable::table"
            TRIGGER "Initial filename and waveform text require Unicode analysis and shaping options"
            RETENTION "Existing text-analysis cache and built-in Unicode table shared with later visible text")
        single_editor_declare_code_unit(
            ID singleeditor.text-shaping.bidi-wrappers OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::BidiAlgorithm juce::BidiParagraph juce::BidiLine
            TARGETS SingleEditor
            TRIGGER "Existing initial text shaping creates a bidi paragraph and computes its visual order"
            RETENTION "Scoped paragraph and line handles during ordinary and later text layout")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/native/juce_DirectWriteTypeface_windows.cpp"
            ID singleeditor.text-shaping.windows-typeface OWNER singleeditor.text-shaping
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::WindowsDirectWriteTypeface juce::AggregateFontCollection juce::TypefaceCache
            TARGETS SingleEditor
            TRIGGER "Initial ordinary system Typeface creation and Windows font-family or character lookup"
            PDB_VFTABLE_CLASSES
                "juce::WindowsDirectWriteTypeface::AnalysisSource" ".?AVAnalysisSource@WindowsDirectWriteTypeface@juce@@"
                "juce::TypefaceCache" ".?AVTypefaceCache@juce@@"
                "juce::WindowsDirectWriteTypeface" ".?AVWindowsDirectWriteTypeface@juce@@"
            RETENTION "Shared system font collection and actual typeface retained by core text consumers")
        single_editor_declare_code_unit(
            ID singleeditor.core-edit-ui.path-geometry OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::PathStrokeHelpers juce::PathFlatteningIterator
            TARGETS SingleEditor
            TRIGGER "Existing initial window and waveform path strokes or geometry flattening"
            RETENTION "Shared path drawing helpers without admitting SVG parsing or resource decoding")
        single_editor_declare_code_unit(
            ID singleeditor.core-edit-ui.native-drawing-context OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Direct2DGraphicsContext
            TARGETS SingleEditor
            TRIGGER "Default Windows native peer selects Direct2D for the actual initial host and waveform paint"
            PDB_VFTABLE_CLASSES
                "juce::Direct2DGraphicsContext::Pimpl" ".?AUPimpl@Direct2DGraphicsContext@juce@@"
                "juce::Direct2DGraphicsContext" ".?AVDirect2DGraphicsContext@juce@@"
            RETENTION "Initial HWND drawing context and shared drawing state through window release")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${CMAKE_CURRENT_BINARY_DIR}/single-editor-juce-graphics/juce_DirectX_windows.cpp"
                "${intent_definition_modules_root}/juce_graphics/images/juce_Image.cpp"
                "${intent_definition_modules_root}/juce_graphics/images/juce_Image.h"
                "${intent_definition_modules_root}/juce_graphics/images/juce_ImagePixelDataNativeExtensions.h"
                "${intent_definition_modules_root}/juce_graphics/native/juce_Direct2DGraphicsContext_windows.cpp"
                "${intent_definition_modules_root}/juce_graphics/native/juce_Direct2DImage_windows.cpp"
                "${intent_definition_modules_root}/juce_graphics/native/juce_DirectX_windows.h"
            ID singleeditor.core-edit-ui.native-drawing-support OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES
                "juce::BitmapDataDetail"
                "juce::SubsectionPixelData"
                "juce::SoftwarePixelData"
                "juce::SoftwareImageType"
                "juce::NativeImageType"
                juce::Direct2DDeviceResources juce::Direct2DPixelData
                juce::Direct2DPixelDataPages juce::Direct2DPixelDataPage
                juce::Direct2DImageContext juce::DxgiAdapter juce::DxgiAdapters
                juce::RectangleListSpriteBatch juce::ImagePixelData
                juce::Image juce::ImageType juce::ImagePixelDataNativeExtensions juce::D2DHelpers
                "juce::Direct2DBitmap"
                "juce::Direct2DDeviceContext"
                "juce::Direct2DFactories"
            TARGETS SingleEditor
            PDB_CLASSES "juce::SharedResourcePointer<juce::Direct2DFactories>"
            MEMBERS "juce::PagesAndArea::make"
                "juce::makeGradientStopCollection"
                "juce::makePages"
                "juce::readFromDirect2DBitmap"
            SCOPES "juce::PushedLayers"
            TRIGGER "Initial native drawing context creates its shared device resources and uses rectangle batches and pixel surfaces for ordinary painting"
            PDB_VFTABLE_CLASSES
                "juce::ImagePixelDataNativeExtensions::Concrete<`juce::Direct2DPixelData::getNativeExtensions'::`2'::Wrapped>" ".?AV?$Concrete@UWrapped@?1??getNativeExtensions@Direct2DPixelData@juce@@UEAA?AVImagePixelDataNativeExtensions@4@XZ@@ImagePixelDataNativeExtensions@juce@@"
                "juce::ImagePixelDataNativeExtensions::Concrete<`juce::ImagePixelData::getNativeExtensions'::`2'::Wrapped>" ".?AV?$Concrete@UWrapped@?1??getNativeExtensions@ImagePixelData@juce@@UEAA?AVImagePixelDataNativeExtensions@4@XZ@@ImagePixelDataNativeExtensions@juce@@"
                "juce::ImagePixelDataNativeExtensions::Concrete<`juce::SubsectionPixelData::getNativeExtensions'::`2'::Wrapped>" ".?AV?$Concrete@UWrapped@?1??getNativeExtensions@SubsectionPixelData@juce@@UEAA?AVImagePixelDataNativeExtensions@4@XZ@@ImagePixelDataNativeExtensions@juce@@"
                "juce::NativeImageType" ".?AVNativeImageType@juce@@"
                "juce::Direct2DPixelData::Context" ".?AUContext@Direct2DPixelData@juce@@"
                "juce::SubsectionPixelData" ".?AVSubsectionPixelData@juce@@"
                "juce::Direct2DImageContext::ImagePimpl" ".?AUImagePimpl@Direct2DImageContext@juce@@"
                "juce::SoftwareImageType" ".?AVSoftwareImageType@juce@@"
                "juce::SoftwarePixelData" ".?AVSoftwarePixelData@juce@@"
                "juce::Direct2DImageContext" ".?AVDirect2DImageContext@juce@@"
                "juce::Direct2DPixelData" ".?AVDirect2DPixelData@juce@@"
                "juce::DxgiAdapter" ".?AUDxgiAdapter@juce@@"
                "juce::ImagePixelData" ".?AVImagePixelData@juce@@"
                "juce::ImageType" ".?AVImageType@juce@@"
            RETENTION "Native rendering support shared by initial and later paints without image codec or optional renderer admission")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_AttributedString.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_AttributedString.h"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_TextLayout.cpp"
                "${intent_definition_modules_root}/juce_graphics/fonts/juce_TextLayout.h"
            ID singleeditor.text-shaping.requested-rich-layout OWNER singleeditor.text-shaping
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::TextLayout juce::AttributedString
            TARGETS SingleEditor
            PDB_CLASSES "juce::detail::IntersectingRangedValues<juce::Font const ,juce::detail::LineMetrics const ,enum juce::detail::JustifiedText::DrawType const ,float const ,juce::Colour>"
                "juce::detail::IntersectingRangedValues<juce::Font const ,juce::detail::LineMetrics const ,enum juce::detail::JustifiedText::DrawType const ,float const ,juce::Colour>::IntersectionIterator"
            TRIGGER "Requested alert, delayed tooltip or admitted metadata inspector creates attributed multiline text"
            RETENTION "Request-owned rich text layout shared across dialogs and module views, separate from initial fitted text")
        # Raster codecs share this physical source with core graphics, but
        # default image-format instances are created on the first image request.
        # Tray/cover artwork gates and renderer preparation remain the callers;
        # this declaration adds neither a codec probe nor startup resource work.
        single_editor_declare_code_unit(
            ID singleeditor.image-codecs.dispatch OWNER singleeditor.image-codecs
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::ImageFileFormat juce::DefaultImageFormats
            TARGETS SingleEditor
            MEMBERS "juce::ImageCache::Pimpl::singletonHolder"
            TRIGGER "Explicit image-format request from admitted tray, artwork or renderer resource work"
            RETENTION "Function-local default format instances retained after the first image request")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/png.c"
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/pngerror.c"
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/pngget.c"
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/pngrio.c"
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/pngrtran.c"
                "${intent_definition_modules_root}/juce_graphics/image_formats/pnglib/pngtrans.c"
            ID singleeditor.image-codecs.png OWNER singleeditor.image-codecs
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::PNGImageFormat juce::pnglibNamespace
            TARGETS SingleEditor
            TRIGGER "Admitted image request selects the built-in PNG codec"
            PDB_VFTABLE_CLASSES
                "juce::PNGImageFormat" ".?AVPNGImageFormat@juce@@"
            RETENTION "Request-owned PNG reader or writer and consumer-retained decoded image")
        single_editor_declare_code_unit(
            ID singleeditor.image-codecs.jpeg OWNER singleeditor.image-codecs
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::JPEGImageFormat juce::jpeglibNamespace
            TARGETS SingleEditor
            TRIGGER "Admitted image request selects the built-in JPEG codec"
            PDB_VFTABLE_CLASSES
                "juce::JPEGImageFormat" ".?AVJPEGImageFormat@juce@@"
            RETENTION "Request-owned JPEG reader or writer and consumer-retained decoded image")
        single_editor_declare_code_unit(
            ID singleeditor.image-codecs.gif OWNER singleeditor.image-codecs
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::GIFImageFormat juce::GIFLoader
            TARGETS SingleEditor
            TRIGGER "Admitted generic JUCE image-format request selects the existing GIF decoder"
            PDB_VFTABLE_CLASSES
                "juce::GIFImageFormat" ".?AVGIFImageFormat@juce@@"
            RETENTION "Request-owned GIF loader and consumer-retained decoded image")
    endif()
endforeach()
# Explicit shared window and event families used by the initial ordinary host.
# These are not whole JUCE GUI/event translation-unit declarations and do not
# admit new startup work or classify optional widget/provider families as core.
get_target_property(intent_gui_sources juce_gui_basics INTERFACE_SOURCES)
foreach(source IN LISTS intent_gui_sources)
    if(source MATCHES "/juce_gui_basics\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/components/juce_Component.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/components/juce_Component.h"
                "${intent_definition_modules_root}/juce_gui_basics/desktop/juce_Desktop.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/desktop/juce_Desktop.h"
                "${intent_definition_modules_root}/juce_gui_basics/native/juce_Windowing_windows.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_ComponentPeer.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_ComponentPeer.h"
            ID singleeditor.window-shell.gui-primitives OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Component juce::ComponentPeer juce::HWNDComponentPeer juce::Desktop
            TARGETS SingleEditor
            MEMBERS "juce::screenSaverDefeater"
                "juce::getPointerTypeFunction"
                "juce::getDPIForMonitor"
                "juce::getThreadDPIAwarenessContext"
                "juce::closeTouchInputHandle"
                "juce::hasCheckedForMultiTouch"
                "juce::getPointerPenInfo"
                "juce::getWindowDPIAwarenessContext"
                "juce::getTouchInputInfo"
                "juce::getProcessDPIAwareness"
                "juce::setProcessDPIAwareness"
                "juce::registerTouchWindow"
                "juce::hasCheckedForDPIAwareness"
                "juce::getGestureInfo"
                "juce::getAwarenessFromDPIAwarenessContext"
                "juce::setProcessDPIAwarenessContext"
                "juce::getDPIForWindow"
                "juce::setProcessDPIAware"
                "juce::enableNonClientDPIScaling"
                "juce::getPointerTouchInfo"
                "juce::canUsePointerAPI"
            TRIGGER "Ordinary initial host component, native window peer, desktop and input dispatch"
            PDB_VFTABLE_CLASSES
                "juce::HWNDComponentPeer::FileDropTarget" ".?AUFileDropTarget@HWNDComponentPeer@juce@@"
                "juce::HWNDComponentPeer::WindowClassHolder" ".?AVWindowClassHolder@HWNDComponentPeer@juce@@"
                "juce::Component" ".?AVComponent@juce@@"
                "juce::ComponentPeer" ".?AVComponentPeer@juce@@"
                "juce::Desktop" ".?AVDesktop@juce@@"
                "juce::HWNDComponentPeer" ".?AVHWNDComponentPeer@juce@@"
            RETENTION "Window and shared desktop component infrastructure through application shutdown")
        single_editor_declare_code_unit(
            ID singleeditor.window-shell.passive-animation-owner OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            MEMBERS "juce::ComponentAnimator::ComponentAnimator"
                "juce::ComponentAnimator::~ComponentAnimator"
                "juce::ComponentAnimator::cancelAllAnimations"
            TARGETS SingleEditor
            TRIGGER "Desktop constructs its passive animator value and drains it during shutdown"
            RETENTION "Existing empty animator owner and cleanup only without admitting animation tasks or generic drag behavior")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/desktop/juce_Displays.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/detail/juce_ComponentHelpers.h"
                "${intent_definition_modules_root}/juce_gui_basics/detail/juce_MouseInputSourceImpl.h"
                "${intent_definition_modules_root}/juce_gui_basics/detail/juce_MouseInputSourceList.h"
                "${intent_definition_modules_root}/juce_gui_basics/detail/juce_ScalingHelpers.h"
                "${intent_definition_modules_root}/juce_gui_basics/keyboard/juce_KeyPress.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/keyboard/juce_KeyPress.h"
                "${intent_definition_modules_root}/juce_gui_basics/keyboard/juce_ModifierKeys.h"
                "${intent_definition_modules_root}/juce_gui_basics/mouse/juce_MouseCursor.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/mouse/juce_MouseEvent.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/mouse/juce_MouseEvent.h"
                "${intent_definition_modules_root}/juce_gui_basics/mouse/juce_MouseInputSource.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/native/juce_Windowing_windows.cpp"
            ID singleeditor.window-shell.input-and-coordinates OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::MouseInputSource juce::MouseEvent juce::MouseCursor
                juce::detail::MouseInputSourceImpl juce::detail::MouseInputSourceList
                juce::KeyPress juce::ModifierKeys juce::Displays
                juce::detail::ComponentHelpers juce::detail::ScalingHelpers
            TARGETS SingleEditor
            MEMBERS "juce::getCurrentDisplayFromScaleFactor"
                "juce::processDisplay"
            TRIGGER "Initial native window display and coordinate setup, component sizing and input dispatch"
            PDB_VFTABLE_CLASSES
                "juce::MouseCursor::PlatformSpecificHandle::BuiltinImpl" ".?AVBuiltinImpl@PlatformSpecificHandle@MouseCursor@juce@@"
                "juce::MouseCursor::PlatformSpecificHandle::ImageImpl" ".?AVImageImpl@PlatformSpecificHandle@MouseCursor@juce@@"
                "juce::detail::MouseInputSourceImpl" ".?AVMouseInputSourceImpl@detail@juce@@"
                "juce::detail::MouseInputSourceList" ".?AVMouseInputSourceList@detail@juce@@"
            RETENTION "Desktop-owned input sources and component coordinate helpers shared with subsequent views")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/keyboard/juce_KeyboardFocusTraverser.cpp"
            ID singleeditor.window-shell.focus-traversal OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES "juce::FocusTraverser"
                "juce::KeyboardFocusTraverser"
                "juce::detail::FocusHelpers"
            PDB_FUNCTIONS "std::_Buffered_inplace_merge_divide_and_conquer2<juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::_Buffered_inplace_merge_unchecked<juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::_Buffered_merge_sort_unchecked<juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::_Chunked_merge_unchecked<juce::Component * *,juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::_Insertion_sort_unchecked<juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::_Stable_sort_unchecked<juce::Component * *,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
                "std::stable_sort<std::_Vector_iterator<std::_Vector_val<std::_Simple_types<juce::Component *> > >,`juce::detail::FocusHelpers::findAllComponents'::`2'::<lambda_1> >"
            TARGETS SingleEditor
            TRIGGER "Initial native focus traversal orders eligible core window components through the inspected stable-sort comparator"
            PDB_VFTABLE_CLASSES
                "juce::FocusTraverser" ".?AVFocusTraverser@juce@@"
                "juce::KeyboardFocusTraverser" ".?AVKeyboardFocusTraverser@juce@@"
            RETENTION "Per-traversal component ordering and shared keyboard focus helpers without module creation")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/accessibility/juce_AccessibilityHandler.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/accessibility/juce_AccessibilityHandler.h"
                "${intent_definition_modules_root}/juce_gui_basics/detail/juce_ButtonAccessibilityHandler.h"
                "${intent_definition_modules_root}/juce_gui_basics/native/accessibility/juce_Accessibility_windows.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/native/accessibility/juce_AccessibilityTextHelpers.h"
            ID singleeditor.window-shell.accessibility-bridge OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AccessibilityHandler juce::AccessibilityNativeHandle
                juce::AccessibilityTextHelpers
                "juce::UIAProviderBase"
                "juce::UIAWindowProvider"
                "juce::UIATransformProvider"
                "juce::UIAInvokeProvider"
                "juce::UIAValueProvider"
                "juce::UIARangeValueProvider"
                "juce::UIAToggleProvider"
                "juce::UIASelectionProvider"
                "juce::UIASelectionItemProvider"
                "juce::UIAGridProvider"
                "juce::UIAGridItemProvider"
                "juce::UIAExpandCollapseProvider"
                "juce::UIAScrollProvider"
                "juce::UIAScrollItemProvider"
                "juce::detail::ButtonAccessibilityHandler"
            TARGETS SingleEditor
            MEMBERS "juce::SpVoiceWrapper::singletonHolder"
            TRIGGER "Native accessibility requests to the ordinary initial window and component handlers"
            PDB_VFTABLE_CLASSES
                "juce::detail::ButtonAccessibilityHandler::ButtonValueInterface" ".?AVButtonValueInterface@ButtonAccessibilityHandler@detail@juce@@"
                "juce::AccessibilityHandler" ".?AVAccessibilityHandler@juce@@"
                "juce::AccessibilityNativeHandle" ".?AVAccessibilityNativeHandle@juce@@"
                "juce::detail::ButtonAccessibilityHandler" ".?AVButtonAccessibilityHandler@detail@juce@@"
                "juce::UIAExpandCollapseProvider" ".?AVUIAExpandCollapseProvider@juce@@"
                "juce::UIAGridItemProvider" ".?AVUIAGridItemProvider@juce@@"
                "juce::UIAGridProvider" ".?AVUIAGridProvider@juce@@"
                "juce::UIAInvokeProvider" ".?AVUIAInvokeProvider@juce@@"
                "juce::UIARangeValueProvider" ".?AVUIARangeValueProvider@juce@@"
                "juce::UIAScrollItemProvider" ".?AVUIAScrollItemProvider@juce@@"
                "juce::UIAScrollProvider" ".?AVUIAScrollProvider@juce@@"
                "juce::UIASelectionItemProvider" ".?AVUIASelectionItemProvider@juce@@"
                "juce::UIASelectionProvider" ".?AVUIASelectionProvider@juce@@"
                "juce::UIAToggleProvider" ".?AVUIAToggleProvider@juce@@"
                "juce::UIATransformProvider" ".?AVUIATransformProvider@juce@@"
                "juce::UIAValueProvider" ".?AVUIAValueProvider@juce@@"
                "juce::UIAWindowProvider" ".?AVUIAWindowProvider@juce@@"
            RETENTION "Window and component accessibility bridge with physical screen-reader acceptance remaining separate")
        single_editor_declare_code_unit(
            ID singleeditor.core-edit-ui.requested-text-accessibility OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES "juce::UIATextProvider"
            TARGETS SingleEditor
            TRIGGER "OS text-interface request to an explicitly opened TextEditor or admitted text view"
            PDB_VFTABLE_CLASSES
                "juce::UIATextProvider::UIATextRangeProvider" ".?AVUIATextRangeProvider@UIATextProvider@juce@@"
                "juce::UIATextProvider" ".?AVUIATextProvider@juce@@"
            RETENTION "Requested text and text-range COM providers while ordinary Label value providers remain separate")
        # The chooser creates either the native provider or the JUCE browser
        # only when Open/SaveAs or an admitted module requests a file dialog.
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/filebrowser/juce_DirectoryContentsList.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/filebrowser/juce_FileBrowserComponent.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/filebrowser/juce_FileChooser.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/filebrowser/juce_FileChooserDialogBox.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/filebrowser/juce_FileTreeComponent.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/native/juce_FileChooser_windows.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_TreeView.cpp"
            ID singleeditor.core-file.requested-file-dialog OWNER singleeditor.core-file
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::FileChooser juce::Win32NativeFileChooser juce::FileBrowserComponent
                juce::FileChooserDialogBox juce::FileListComponent juce::FileTreeComponent
                juce::DirectoryContentsDisplayComponent juce::DirectoryContentsList
                juce::DirectoryScanner juce::TreeView juce::TreeViewItem
                "juce::FileListTreeItem"
            TARGETS SingleEditor
            TRIGGER "Explicit Open, SaveAs, folder selection or admitted module file selection constructs the chooser provider"
            PDB_VFTABLE_CLASSES
                "juce::FileChooserDialogBox::ContentComponent" ".?AVContentComponent@FileChooserDialogBox@juce@@"
                "juce::TreeView::TreeViewport" ".?AVTreeViewport@TreeView@juce@@"
                "juce::Win32NativeFileChooser::CustomComponentHolder" ".?AVCustomComponentHolder@Win32NativeFileChooser@juce@@"
                "juce::TreeView::ItemComponent::ItemAccessibilityHandler::ItemCellInterface" ".?AVItemCellInterface@ItemAccessibilityHandler@ItemComponent@TreeView@juce@@"
                "juce::FileListComponent::ItemComponent" ".?AVItemComponent@FileListComponent@juce@@"
                "juce::TreeView::TargetGroupHighlight" ".?AVTargetGroupHighlight@TreeView@juce@@"
                "juce::TreeView::ItemComponent::ItemAccessibilityHandler" ".?AVItemAccessibilityHandler@ItemComponent@TreeView@juce@@"
                "juce::FileChooser::Native" ".?AVNative@FileChooser@juce@@"
                "juce::FileTreeComponent::Controller" ".?AVController@FileTreeComponent@juce@@"
                "juce::FileChooser::NonNative" ".?AVNonNative@FileChooser@juce@@"
                "juce::TreeView::ContentComponent" ".?AVContentComponent@TreeView@juce@@"
                "juce::TreeView::InsertPointHighlight" ".?AVInsertPointHighlight@TreeView@juce@@"
                "juce::TreeView::ItemComponent" ".?AVItemComponent@TreeView@juce@@"
                "juce::DirectoryContentsDisplayComponent" ".?AVDirectoryContentsDisplayComponent@juce@@"
                "juce::DirectoryContentsList" ".?AVDirectoryContentsList@juce@@"
                "juce::DirectoryScanner" ".?AVDirectoryScanner@juce@@"
                "juce::FileBrowserComponent" ".?AVFileBrowserComponent@juce@@"
                "juce::FileChooserDialogBox" ".?AVFileChooserDialogBox@juce@@"
                "juce::FileListComponent" ".?AVFileListComponent@juce@@"
                "juce::FileListTreeItem" ".?AVFileListTreeItem@juce@@"
                "juce::FileTreeComponent" ".?AVFileTreeComponent@juce@@"
                "juce::TreeView" ".?AVTreeView@juce@@"
                "juce::TreeViewItem" ".?AVTreeViewItem@juce@@"
                "juce::Win32NativeFileChooser" ".?AVWin32NativeFileChooser@juce@@"
            RETENTION "Requested chooser and its native dialog or fallback directory browser until completion without a startup directory scan")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_AlertWindow.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_MessageBoxOptions.h"
            ID singleeditor.core-edit-ui.requested-alerts OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AlertWindow juce::MessageBoxOptions
            TARGETS SingleEditor
            TRIGGER "Explicit save/discard confirmation or an admitted recovery, workspace, media or diagnostic action requests a dialog"
            PDB_VFTABLE_CLASSES
                "juce::AlertWindow" ".?AVAlertWindow@juce@@"
            RETENTION "Requested dialog, options and completion state until dismissal")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_TableListBox.cpp"
            ID singleeditor.audio-output.routing-table OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::TableListBox juce::TableListBoxModel juce::TableHeaderComponent
            TARGETS SingleEditor
            TRIGGER "Explicit output routing settings constructs AsioSettingsComponent and its route matrix"
            PDB_VFTABLE_CLASSES
                "juce::TableListBox::RowComp::RowAccessibilityHandler" ".?AVRowAccessibilityHandler@RowComp@TableListBox@juce@@"
                "juce::TableListBox::RowComp::RowAccessibilityHandler::RowComponentCellInterface" ".?AVRowComponentCellInterface@RowAccessibilityHandler@RowComp@TableListBox@juce@@"
                "juce::TableListBox::Header" ".?AVHeader@TableListBox@juce@@"
                "juce::TableListBox::RowComp" ".?AVRowComp@TableListBox@juce@@"
                "juce::TableHeaderComponent::ColumnInfo" ".?AUColumnInfo@TableHeaderComponent@juce@@"
                "juce::TableHeaderComponent::DragOverlayComp" ".?AVDragOverlayComp@TableHeaderComponent@juce@@"
                "juce::TableHeaderComponent" ".?AVTableHeaderComponent@juce@@"
                "juce::TableListBox" ".?AVTableListBox@juce@@"
            RETENTION "Requested route table, header and rows for the admitted settings session")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_TooltipWindow.cpp"
            ID singleeditor.window-shell.delayed-tooltips OWNER singleeditor.window-shell
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::TooltipWindow
            TARGETS SingleEditor
            TRIGGER "MainWindow delayed tooltip callback after initial content installation"
            PDB_VFTABLE_CLASSES
                "juce::TooltipWindow" ".?AVTooltipWindow@juce@@"
            RETENTION "Window-owned tooltip watcher and transient visible tip through shutdown")
        single_editor_declare_code_unit(
            ID singleeditor.window-shell.flex-layout OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::FlexBox juce::FlexItem
                "juce::FlexBoxLayoutCalculation"
            TARGETS SingleEditor
            PDB_FUNCTIONS "std::_Buffered_inplace_merge_divide_and_conquer2<juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
                "std::_Buffered_inplace_merge_unchecked<juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
                "std::_Buffered_merge_sort_unchecked<juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
                "std::_Chunked_merge_unchecked<juce::FlexBoxLayoutCalculation::ItemWithState *,juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
                "std::_Insertion_sort_unchecked<juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
                "std::_Stable_sort_unchecked<juce::FlexBoxLayoutCalculation::ItemWithState *,`juce::FlexBoxLayoutCalculation::createStates'::`2'::<lambda_1> >"
            TRIGGER "Initial custom title and top-bar layout during window component sizing"
            RETENTION "Layout values and shared positioning implementation with no module service activation")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/drawables/juce_Drawable.h"
            ID singleeditor.image-codecs.svg OWNER singleeditor.image-codecs
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::SVGState juce::Drawable juce::DrawableComposite juce::DrawableShape
                juce::DrawablePath juce::DrawableRectangle juce::DrawableImage juce::DrawableText
            TARGETS SingleEditor
            TRIGGER "First actually drawn ModernIconButton requests a tinted built-in SVG after its chrome becomes visible"
            PDB_VFTABLE_CLASSES
                "juce::Drawable" ".?AVDrawable@juce@@"
                "juce::DrawableComposite" ".?AVDrawableComposite@juce@@"
                "juce::DrawableImage" ".?AVDrawableImage@juce@@"
                "juce::DrawablePath" ".?AVDrawablePath@juce@@"
                "juce::DrawableShape" ".?AVDrawableShape@juce@@"
                "juce::DrawableText" ".?AVDrawableText@juce@@"
            RETENTION "Requested drawables and tint cache reused across controls and themes")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/native/juce_Direct2DHwndContext_windows.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/native/juce_Windowing_windows.cpp"
            ID singleeditor.core-edit-ui.native-drawing-host OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Direct2DHwndContext
                "juce::RenderContext"
                "juce::GDIRenderContext"
                "juce::D2DRenderContext"
            TARGETS SingleEditor
            TRIGGER "Initial Windows native peer creates the HWND Direct2D drawing host in the windowing module"
            PDB_VFTABLE_CLASSES
                "juce::Direct2DHwndContext::HwndPimpl" ".?AUHwndPimpl@Direct2DHwndContext@juce@@"
                "juce::D2DRenderContext::WrappedD2DHwndContext" ".?AVWrappedD2DHwndContext@D2DRenderContext@juce@@"
                "juce::GDIRenderContext::TemporaryImage" ".?AUTemporaryImage@GDIRenderContext@juce@@"
                "juce::D2DRenderContext::WrappedD2DHwndContextTransparent" ".?AVWrappedD2DHwndContextTransparent@D2DRenderContext@juce@@"
                "juce::D2DRenderContext" ".?AVD2DRenderContext@juce@@"
                "juce::Direct2DHwndContext" ".?AVDirect2DHwndContext@juce@@"
                "juce::GDIRenderContext" ".?AVGDIRenderContext@juce@@"
            RETENTION "Actual HWND drawing target through native window release")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/buttons/juce_Button.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_ResizableWindow.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_ResizableWindow.h"
                "${intent_definition_modules_root}/juce_gui_basics/windows/juce_TopLevelWindow.cpp"
            ID singleeditor.window-shell.window-controls OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::TopLevelWindow juce::ResizableWindow juce::DocumentWindow juce::Button
            TARGETS SingleEditor
            TRIGGER "Initial DocumentWindow construction, native window controls and shared button base"
            PDB_VFTABLE_CLASSES
                "juce::DocumentWindow::ButtonListenerProxy" ".?AVButtonListenerProxy@DocumentWindow@juce@@"
                "juce::Button::CallbackHelper" ".?AUCallbackHelper@Button@juce@@"
                "juce::Button" ".?AVButton@juce@@"
                "juce::DocumentWindow" ".?AVDocumentWindow@juce@@"
                "juce::ResizableWindow" ".?AVResizableWindow@juce@@"
                "juce::TopLevelWindow" ".?AVTopLevelWindow@juce@@"
            RETENTION "Initial window and button infrastructure shared by later UI consumers")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/lookandfeel/juce_LookAndFeel.cpp"
            ID singleeditor.core-edit-ui.gui-look-and-feel OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            # Startup WindowChromeLookAndFeel already derives from V4, V3, V2
            # and LookAndFeel. This is not the deferred Modern skin.
            SCOPES juce::LookAndFeel juce::LookAndFeel_V2 juce::LookAndFeel_V3 juce::LookAndFeel_V4
            TARGETS SingleEditor
            TRIGGER "Initial lightweight window chrome and inherited ordinary control painting"
            PDB_VFTABLE_CLASSES
                "juce::LookAndFeel_V2::GlassWindowButton" ".?AVGlassWindowButton@LookAndFeel_V2@juce@@"
                "juce::LookAndFeel_V2::SliderLabelComp" ".?AVSliderLabelComp@LookAndFeel_V2@juce@@"
                "juce::LookAndFeel" ".?AVLookAndFeel@juce@@"
                "juce::LookAndFeel_V2" ".?AVLookAndFeel_V2@juce@@"
                "juce::LookAndFeel_V3" ".?AVLookAndFeel_V3@juce@@"
                "juce::LookAndFeel_V4" ".?AVLookAndFeel_V4@juce@@"
            RETENTION "Shared JUCE style base used by startup chrome and subsequent renderer instances")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/buttons/juce_TextButton.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/buttons/juce_ToggleButton.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_ComboBox.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_Label.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_Slider.cpp"
            ID singleeditor.core-edit-ui.editor-controls OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Label juce::Slider juce::TextButton juce::ToggleButton juce::ComboBox
            TARGETS SingleEditor
            TRIGGER "Ordinary MainComponent labels, transport buttons and basic edit controls before CoreReady"
            PDB_VFTABLE_CLASSES
                "juce::Slider::Pimpl::PopupDisplayComponent" ".?AUPopupDisplayComponent@Pimpl@Slider@juce@@"
                "juce::Slider::Pimpl" ".?AVPimpl@Slider@juce@@"
                "juce::ComboBox" ".?AVComboBox@juce@@"
                "juce::Label" ".?AVLabel@juce@@"
                "juce::Slider" ".?AVSlider@juce@@"
                "juce::TextButton" ".?AVTextButton@juce@@"
                "juce::ToggleButton" ".?AVToggleButton@juce@@"
            RETENTION "Basic editor controls shared by core and later optional UI consumers")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/layout/juce_Viewport.cpp"
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_ListBox.cpp"
            ID singleeditor.core-edit-ui.list-viewport OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::ListBox juce::ListBoxModel juce::Viewport juce::ScrollBar
            TARGETS SingleEditor
            TRIGGER "Initial playlist ListBox and metadata viewport with their shared scrolling infrastructure"
            PDB_VFTABLE_CLASSES
                "juce::Viewport::AccessibilityIgnoredComponent" ".?AVAccessibilityIgnoredComponent@Viewport@juce@@"
                "juce::Viewport::DragToScrollListener" ".?AUDragToScrollListener@Viewport@juce@@"
                "juce::ListBox::RowComponent" ".?AVRowComponent@ListBox@juce@@"
                "juce::ScrollBar::ScrollbarButton" ".?AVScrollbarButton@ScrollBar@juce@@"
                "juce::ListBox::RowComponent::RowAccessibilityHandler::RowCellInterface" ".?AVRowCellInterface@RowAccessibilityHandler@RowComponent@ListBox@juce@@"
                "juce::ListBox::RowComponent::RowAccessibilityHandler" ".?AVRowAccessibilityHandler@RowComponent@ListBox@juce@@"
                "juce::ListBox::ListViewport" ".?AVListViewport@ListBox@juce@@"
                "juce::ListBox" ".?AVListBox@juce@@"
                "juce::ScrollBar" ".?AVScrollBar@juce@@"
                "juce::Viewport" ".?AVViewport@juce@@"
            RETENTION "Core sidebar list and viewport controls reused by later requested views")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/widgets/juce_TextEditor.cpp"
            ID singleeditor.core-edit-ui.text-input OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::TextEditor juce::TextInputTarget
            TARGETS SingleEditor
            PDB_METHODS "juce::detail::JustifiedText::accessTogetherWith<`juce::TextEditor::drawContent'::`2'::<lambda_4> const &,juce::detail::RangedValues<signed char> >"
            TRIGGER "Explicit text editing, admitted optional views or post-Core typed metadata search"
            PDB_VFTABLE_CLASSES
                "juce::TextEditor::LengthAndCharacterRestriction" ".?AVLengthAndCharacterRestriction@TextEditor@juce@@"
                "juce::TextEditor::TextEditorViewport" ".?AUTextEditorViewport@TextEditor@juce@@"
                "juce::TextEditor::EditorAccessibilityHandler" ".?AVEditorAccessibilityHandler@TextEditor@juce@@"
                "juce::TextEditor::GlobalMouseListener" ".?AVGlobalMouseListener@TextEditor@juce@@"
                "juce::TextEditor::RemoveAction" ".?AURemoveAction@TextEditor@juce@@"
                "juce::TextEditor::EditorAccessibilityHandler::TextEditorTextInterface" ".?AVTextEditorTextInterface@EditorAccessibilityHandler@TextEditor@juce@@"
                "juce::TextEditor::InsertAction" ".?AUInsertAction@TextEditor@juce@@"
                "juce::TextEditor::TextHolderComponent" ".?AUTextHolderComponent@TextEditor@juce@@"
                "juce::TextEditor" ".?AVTextEditor@juce@@"
            RETENTION "Requested text editor and editing state while the initial ComboBox label remains separate")
        # ComboBox owns a PopupMenu value and populates it before CoreReady.
        # The actual menu window is a separate, requested presentation path.
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/menus/juce_PopupMenu.cpp"
            ID singleeditor.core-edit-ui.popup-data OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::PopupMenu::Item juce::PopupMenu::MenuItemIterator
            MEMBERS juce::PopupMenu::PopupMenu juce::PopupMenu::~PopupMenu
                juce::PopupMenu::clear juce::PopupMenu::addItem
                juce::PopupMenu::addSeparator juce::PopupMenu::getNumItems
            TARGETS SingleEditor
            TRIGGER "Initial ComboBox menu value, item population and selection lookup before CoreReady"
            RETENTION "Shared menu item data retained by core controls without constructing popup windows")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/menus/juce_PopupMenu.cpp"
            ID singleeditor.core-edit-ui.popup-presentation OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::PopupMenu::HelperClasses::MenuWindow
                juce::PopupMenu::HelperClasses::ItemComponent
                juce::PopupMenu::HelperClasses::HeaderItemComponent
                juce::PopupMenu::HelperClasses::MouseSourceState
                juce::PopupMenu::Options juce::PopupMenuCompletionCallback
            MEMBERS juce::PopupMenu::createWindow juce::PopupMenu::showWithOptionalCallback
                juce::PopupMenu::showMenuAsync
            TARGETS SingleEditor
            MEMBERS "juce::PopupMenuSettings::menuWasHiddenBecauseOfAppChange"
            TRIGGER "Explicit ComboBox, menu or context-menu presentation request"
            PDB_VFTABLE_CLASSES
                "juce::PopupMenu::HelperClasses::ItemComponent::ItemAccessibilityHandler" ".?AVItemAccessibilityHandler@ItemComponent@HelperClasses@PopupMenu@juce@@"
                "juce::PopupMenu::HelperClasses::HeaderItemComponent" ".?AUHeaderItemComponent@HelperClasses@PopupMenu@juce@@"
                "juce::PopupMenu::HelperClasses::ItemComponent" ".?AUItemComponent@HelperClasses@PopupMenu@juce@@"
                "juce::PopupMenu::HelperClasses::MenuWindow" ".?AUMenuWindow@HelperClasses@PopupMenu@juce@@"
                "juce::PopupMenu::HelperClasses::MouseSourceState" ".?AVMouseSourceState@HelperClasses@PopupMenu@juce@@"
                "juce::PopupMenuCompletionCallback" ".?AUPopupMenuCompletionCallback@juce@@"
            RETENTION "Requested popup window, pointer tracking and completion callback until dismissal")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_gui_basics/misc/juce_DropShadower.cpp"
            ID singleeditor.core-edit-ui.requested-software-shadows OWNER singleeditor.core-edit-ui
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES "juce::DropShadower"
            TARGETS SingleEditor
            TRIGGER "Requested nonopaque or popup peers reach the software shadow path after initial opaque TopLevelWindow suppression"
            PDB_VFTABLE_CLASSES
                "juce::DropShadower::VirtualDesktopWatcher" ".?AVVirtualDesktopWatcher@DropShadower@juce@@"
                "juce::DropShadower::ShadowWindow" ".?AVShadowWindow@DropShadower@juce@@"
                "juce::DropShadower::ParentVisibilityChangedListener" ".?AVParentVisibilityChangedListener@DropShadower@juce@@"
                "juce::DropShadower" ".?AVDropShadower@juce@@"
            RETENTION "Requested shadow helper peers and visibility watcher through transient window release")
    endif()
endforeach()

# Application settings have a startup reader and separately admitted writer.
get_target_property(intent_settings_sources juce_data_structures INTERFACE_SOURCES)
foreach(source IN LISTS intent_settings_sources)
    if(source MATCHES "/juce_data_structures[.]cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_data_structures/app_properties/juce_ApplicationProperties.cpp"
            ID singleeditor.window-shell.settings-store-reader OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            TARGETS SingleEditor
            SCOPES juce::PropertiesFile::Options
            MEMBERS juce::ApplicationProperties::setStorageParameters juce::ApplicationProperties::openFiles
                juce::ApplicationProperties::getUserSettings juce::ApplicationProperties::~ApplicationProperties
                juce::ApplicationProperties::closeFiles juce::PropertiesFile::PropertiesFile
                juce::PropertiesFile::~PropertiesFile juce::PropertiesFile::reload
                juce::PropertiesFile::loadAsBinary juce::PropertiesFile::loadAsXml juce::PropertiesFile::createProcessLock
            TRIGGER "Initial application reads renderer and visual settings from user and common property files"
            RETENTION "Application-owned settings stores, fallback values and process lock through shutdown")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_data_structures/app_properties/juce_PropertiesFile.cpp"
            ID singleeditor.window-shell.settings-store-writer OWNER singleeditor.window-shell
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            TARGETS SingleEditor
            MEMBERS juce::ApplicationProperties::saveIfNeeded juce::PropertiesFile::saveIfNeeded
                juce::PropertiesFile::needsToBeSaved juce::PropertiesFile::setNeedsToBeSaved
                juce::PropertiesFile::save juce::PropertiesFile::saveAsXml juce::PropertiesFile::saveAsBinary
                juce::PropertiesFile::writeToStream juce::PropertiesFile::timerCallback juce::PropertiesFile::propertyChanged
            TRIGGER "Existing deferred session/window writer, explicit preference action or shutdown requests settings serialization"
            RETENTION "Dirty values and save timer through write completion or settings destruction")
    endif()
endforeach()

get_target_property(intent_event_sources juce_events INTERFACE_SOURCES)
foreach(source IN LISTS intent_event_sources)
    if(source MATCHES "/juce_events\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_events/broadcasters/juce_AsyncUpdater.cpp"
                "${intent_definition_modules_root}/juce_events/messages/juce_ApplicationBase.cpp"
                "${intent_definition_modules_root}/juce_events/messages/juce_ApplicationBase.h"
                "${intent_definition_modules_root}/juce_events/messages/juce_MessageManager.cpp"
                "${intent_definition_modules_root}/juce_events/messages/juce_MessageManager.h"
                "${intent_definition_modules_root}/juce_events/native/juce_Messaging_windows.cpp"
                "${intent_definition_modules_root}/juce_events/timers/juce_Timer.cpp"
                "${intent_definition_modules_root}/juce_events/timers/juce_Timer.h"
            ID singleeditor.window-shell.event-loop OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::MessageManager juce::Timer juce::AsyncUpdater
                juce::InternalMessageQueue juce::JUCEApplicationBase
            TARGETS SingleEditor
            MEMBERS "juce::numScopedInitInstances"
                "juce::settingChangeCallback"
                "juce::isEventBlockedByModalComps"
                "juce::detail::RunningInUnity::state"
                "juce::ShutdownDetector::singletonHolder"
            TRIGGER "Initial GUI message loop, startup host timer and ordinary load result publication"
            PDB_VFTABLE_CLASSES
                "juce::JUCEApplicationBase::MultipleInstanceHandler" ".?AUMultipleInstanceHandler@JUCEApplicationBase@juce@@"
                "juce::Timer::TimerThread" ".?AVTimerThread@Timer@juce@@"
                "juce::MessageManager::QuitMessage" ".?AVQuitMessage@MessageManager@juce@@"
                "juce::AsyncUpdater::AsyncUpdaterMessage" ".?AVAsyncUpdaterMessage@AsyncUpdater@juce@@"
                "juce::Timer::TimerThread::CallTimersMessage" ".?AUCallTimersMessage@TimerThread@Timer@juce@@"
                "juce::AsyncUpdater" ".?AVAsyncUpdater@juce@@"
                "juce::JUCEApplicationBase" ".?AVJUCEApplicationBase@juce@@"
                "juce::Timer" ".?AVTimer@juce@@"
            RETENTION "Message manager and owned timer or async-update consumers through release or shutdown")
    endif()
endforeach()
get_target_property(intent_core_sources juce_core INTERFACE_SOURCES)
foreach(source IN LISTS intent_core_sources)
    if(source MATCHES "/juce_core\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        get_filename_component(intent_core_directory "${source}" DIRECTORY)
        single_editor_declare_code_unit(
            ID singleeditor.text-values.primitives OWNER singleeditor.text-values
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES
                "juce::NumberToStringConverters::StackArrayStream"
                "juce::StringRef"
                "juce::StringCreationHelper"
                "juce::StartEndString"
                juce::String juce::StringHolderUtils juce::StringArray juce::StringPairArray
                juce::CharacterFunctions juce::CharPointer_ASCII juce::CharPointer_UTF8
                juce::CharPointer_UTF16 juce::CharPointer_UTF32
                juce::var juce::DynamicObject juce::Identifier juce::StringPool juce::NamedValueSet
            INCLUDED_DEFINITIONS
                "${intent_core_directory}/containers/juce_DynamicObject.cpp"
                "${intent_core_directory}/containers/juce_NamedValueSet.cpp"
                "${intent_core_directory}/containers/juce_Variant.cpp"
                "${intent_core_directory}/text/juce_CharacterFunctions.cpp"
                "${intent_core_directory}/text/juce_CharacterFunctions.h"
                "${intent_core_directory}/text/juce_CharPointer_ASCII.h"
                "${intent_core_directory}/text/juce_CharPointer_UTF16.h"
                "${intent_core_directory}/text/juce_CharPointer_UTF8.h"
                "${intent_core_directory}/text/juce_Identifier.cpp"
                "${intent_core_directory}/text/juce_Identifier.h"
                "${intent_core_directory}/text/juce_String.cpp"
                "${intent_core_directory}/text/juce_String.h"
                "${intent_core_directory}/text/juce_StringArray.cpp"
                "${intent_core_directory}/text/juce_StringArray.h"
                "${intent_core_directory}/text/juce_StringPairArray.cpp"
                "${intent_core_directory}/text/juce_StringPairArray.h"
                "${intent_core_directory}/text/juce_StringPool.cpp"
            MEMBERS
                "juce::compareStrings"
                "juce::NumberToStringConverters::createFromDouble"
                "juce::NumberToStringConverters::createFromInteger"
                "juce::NumberToStringConverters::doubleToString"
                "juce::NumberToStringConverters::numberToString"
                "juce::NumberToStringConverters::printDigits"
            PDB_CLASSES
                "juce::StringEncodingConverter<juce::CharPointer_UTF8,juce::CharPointer_UTF16>"
                "juce::WildCardMatcher<juce::CharPointer_UTF8>"
            PDB_FUNCTIONS
                "juce::NumberToStringConverters::createFromInteger<int>"
                "juce::NumberToStringConverters::createFromInteger<unsigned int>"
                "juce::addPooledString<juce::CharPointer_UTF8>"
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary file and command-line text, initial window labels and PCM reader metadata values"
            PDB_VFTABLE_CLASSES
                "juce::var::VariantType::RefCountedArray" ".?AURefCountedArray@VariantType@var@juce@@"
                "juce::NumberToStringConverters::StackArrayStream" ".?AUStackArrayStream@NumberToStringConverters@juce@@"
                "juce::DynamicObject" ".?AVDynamicObject@juce@@"
            RETENTION "Caller-owned text and key-value storage shared with later actions without starting services")
        single_editor_declare_code_unit(
            ID singleeditor.window-shell.settings-value-reader OWNER singleeditor.window-shell
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            TARGETS SingleEditor
            MEMBERS "juce::PropertySet::PropertySet"
                "juce::PropertySet::~PropertySet"
                "juce::PropertySet::getValue"
                "juce::PropertySet::getBoolValue"
                "juce::PropertySet::getIntValue"
                "juce::PropertySet::getDoubleValue"
                "juce::PropertySet::setFallbackPropertySet"
            TRIGGER "Application renderer and initial visual/window preferences read the existing user and common property values"
            RETENTION "Application-owned settings values and fallback link through shutdown")
        single_editor_declare_code_unit(
            ID singleeditor.window-shell.settings-value-writer OWNER singleeditor.window-shell
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            TARGETS SingleEditor
            MEMBERS "juce::PropertySet::setValue"
                "juce::PropertySet::removeValue"
            TRIGGER "Admitted session/window writes or explicit preferences change existing application properties"
            RETENTION "Changed values notify the settings store until serialization or shutdown")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/native/juce_Threads_windows.cpp"
                "${intent_definition_modules_root}/juce_core/threads/juce_ReadWriteLock.cpp"
                "${intent_definition_modules_root}/juce_core/threads/juce_Thread.cpp"
                "${intent_definition_modules_root}/juce_core/threads/juce_ThreadPool.cpp"
                "${intent_definition_modules_root}/juce_core/threads/juce_WaitableEvent.cpp"
            ID singleeditor.core-lifecycle.gui-workers OWNER singleeditor.core-lifecycle
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Thread juce::ThreadPool juce::ThreadPoolJob
                juce::WaitableEvent juce::CriticalSection juce::ReadWriteLock
            TARGETS SingleEditor
            TRIGGER "Ordinary startup preparation and waveform jobs use the existing core worker pools and synchronization"
            PDB_VFTABLE_CLASSES
                "juce::ThreadPool::ThreadPoolThread" ".?AUThreadPoolThread@ThreadPool@juce@@"
                "juce::Thread" ".?AVThread@juce@@"
                "juce::ThreadPoolJob" ".?AVThreadPoolJob@juce@@"
            RETENTION "Shared worker and synchronization support through job completion and application shutdown")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/native/juce_SystemStats_windows.cpp"
                "${intent_definition_modules_root}/juce_core/time/juce_Time.cpp"
                "${intent_definition_modules_root}/juce_core/time/juce_Time.h"
            ID singleeditor.core-lifecycle.clock-values OWNER singleeditor.core-lifecycle
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::Time
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary source-file modification identity, core timing and shared time values"
            RETENTION "Value and platform clock helpers without starting a timer or optional service")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/maths/juce_BigInteger.cpp"
            ID singleeditor.channel-layout.bitsets OWNER singleeditor.channel-layout
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::BigInteger
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary AudioChannelSet stores its channel membership in the existing bitset representation"
            RETENTION "Channel-set values shared with later admitted output masks without requesting device enumeration")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/memory/juce_MemoryBlock.cpp"
                "${intent_definition_modules_root}/juce_core/native/juce_Files_windows.cpp"
            ID singleeditor.core-file.input-streams OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::InputStream juce::FileInputStream juce::MemoryBlock
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary direct input stream and PCM header or metadata reads"
            PDB_VFTABLE_CLASSES
                "juce::FileInputStream" ".?AVFileInputStream@juce@@"
            RETENTION "Reader-owned input handles and bounded value buffers through input release")
        single_editor_declare_code_unit(
            ID singleeditor.buffer-streams.adapters OWNER singleeditor.buffer-streams
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::BufferedInputStream juce::SubregionStream
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Requested image or archive read, MP3 fallback or compressed settings format"
            PDB_VFTABLE_CLASSES
                "juce::BufferedInputStream" ".?AVBufferedInputStream@juce@@"
                "juce::SubregionStream" ".?AVSubregionStream@juce@@"
            RETENTION "Caller-owned buffered or bounded input view until request completion")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/streams/juce_MemoryInputStream.cpp"
            ID singleeditor.buffer-streams.memory-gui OWNER singleeditor.buffer-streams
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::MemoryInputStream TARGETS SingleEditor
            TRIGGER "First ordinary text shaping reads JUCE Unicode data through an in-memory compressed stream"
            PDB_VFTABLE_CLASSES
                "juce::MemoryInputStream" ".?AVMemoryInputStream@juce@@"
            RETENTION "Request-local input view shared with later resource, cache and diagnostic consumers")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/streams/juce_MemoryInputStream.cpp"
            ID singleeditor.buffer-streams.memory-cli OWNER singleeditor.buffer-streams
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::MemoryInputStream TARGETS SingleEditorCLI
            TRIGGER "Selected Ambisonic resource pack, decoded-cache seal or explicit archive operation"
            PDB_VFTABLE_CLASSES
                "juce::MemoryInputStream" ".?AVMemoryInputStream@juce@@"
            RETENTION "Caller-owned input view with no GUI text-shaping consumer in the CLI image")
        set(intent_inflate_members
            juce::zlibNamespace::inflate juce::zlibNamespace::inflateEnd
            juce::zlibNamespace::inflateInit_ juce::zlibNamespace::inflateInit2_
            juce::zlibNamespace::inflateReset juce::zlibNamespace::inflateReset2
            juce::zlibNamespace::inflate_fast juce::zlibNamespace::inflate_table
            juce::zlibNamespace::updatewindow juce::zlibNamespace::adler32
            juce::zlibNamespace::zcalloc juce::zlibNamespace::zcfree)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/zip/juce_GZIPDecompressorInputStream.cpp"
                "${intent_definition_modules_root}/juce_core/zip/zlib/inflate.c"
            ID singleeditor.compression.inflate-gui OWNER singleeditor.compression
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::GZIPDecompressorInputStream MEMBERS ${intent_inflate_members}
            TARGETS SingleEditor
            TRIGGER "First ordinary text shaping decompresses the built-in Unicode data table"
            PDB_VFTABLE_CLASSES
                "juce::GZIPDecompressorInputStream" ".?AVGZIPDecompressorInputStream@juce@@"
            RETENTION "Request-local inflater and shared codec support, Unicode result retained by text infrastructure")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/zip/juce_GZIPDecompressorInputStream.cpp"
                "${intent_definition_modules_root}/juce_core/zip/zlib/inflate.c"
            ID singleeditor.compression.inflate-cli OWNER singleeditor.compression
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::GZIPDecompressorInputStream MEMBERS ${intent_inflate_members}
            TARGETS SingleEditorCLI
            TRIGGER "Selected compressed resource pack or explicit archive capability with no ordinary GUI text consumer"
            PDB_VFTABLE_CLASSES
                "juce::GZIPDecompressorInputStream" ".?AVGZIPDecompressorInputStream@juce@@"
            RETENTION "Request-local inflater through admitted operation completion")
        single_editor_declare_code_unit(
            ID singleeditor.compression.deflate OWNER singleeditor.compression
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/zip/zlib/trees.h"
                "${intent_definition_modules_root}/juce_core/zip/zlib/trees.c"
                "${intent_definition_modules_root}/juce_core/zip/zlib/deflate.c"
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::GZIPCompressorOutputStream
            MEMBERS juce::zlibNamespace::deflate juce::zlibNamespace::deflateEnd
                juce::zlibNamespace::deflateInit2_ juce::zlibNamespace::deflateReset
                juce::zlibNamespace::deflate_fast juce::zlibNamespace::deflate_slow
                juce::zlibNamespace::deflate_stored juce::zlibNamespace::fill_window
                juce::zlibNamespace::flush_pending juce::zlibNamespace::longest_match
                juce::zlibNamespace::read_buf juce::zlibNamespace::slide_hash
                juce::zlibNamespace::_tr_flush_block juce::zlibNamespace::_tr_stored_block
                juce::zlibNamespace::bi_flush juce::zlibNamespace::bi_windup
                juce::zlibNamespace::build_tree juce::zlibNamespace::compress_block
                juce::zlibNamespace::init_block juce::zlibNamespace::pqdownheap
                juce::zlibNamespace::scan_tree juce::zlibNamespace::send_tree juce::zlibNamespace::crc32
            TARGETS SingleEditor SingleEditorCLI
            MEMBERS "juce::zlibNamespace::crc_braid_table"
                "juce::zlibNamespace::crc_table"
                "juce::zlibNamespace::_length_code" "juce::zlibNamespace::_dist_code"
                "juce::zlibNamespace::configuration_table"
                "juce::zlibNamespace::extra_lbits" "juce::zlibNamespace::extra_dbits"
                "juce::zlibNamespace::extra_blbits" "juce::zlibNamespace::bl_order"
                "juce::zlibNamespace::static_ltree" "juce::zlibNamespace::static_dtree"
                "juce::zlibNamespace::base_length" "juce::zlibNamespace::base_dist"
                "juce::zlibNamespace::static_l_desc" "juce::zlibNamespace::static_d_desc"
                "juce::zlibNamespace::static_bl_desc"
            TRIGGER "Requested archive compression or PNG/archive CRC capability, independent of Unicode zlib inflation"
            PDB_VFTABLE_CLASSES
                "juce::GZIPCompressorOutputStream" ".?AVGZIPCompressorOutputStream@juce@@"
            RETENTION "Request-local encoder and format checksum support, no startup registrar")
        single_editor_declare_code_unit(
            ID singleeditor.compression.archives OWNER singleeditor.compression
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::ZipFile
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Post-Core tray pack, selected Ambisonic bank pack or explicit diagnostic bundle operation"
            RETENTION "Request-owned archive entries and streams until pack load or diagnostic operation completion"
            PDB_VFTABLE_CLASSES
                "juce::ZipFile::ZipInputStream" ".?AUZipInputStream@ZipFile@juce@@")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/xml/juce_XmlDocument.cpp"
            ID singleeditor.xml.reader OWNER singleeditor.xml
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::XmlDocument
            MEMBERS juce::parseXML
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Initial GUI PropertiesFile settings or ordinary WAV iXML and axml metadata request XML parsing"
            RETENTION "Request-local XML parser shared with later admitted icon and bank metadata consumers")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/xml/juce_XmlElement.cpp"
            ID singleeditor.xml.values OWNER singleeditor.xml
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::XmlElement
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Initial GUI settings and ordinary PCM XML metadata use shared element, attribute and text values"
            RETENTION "Caller-owned XML value tree through settings or metadata parsing and later serialization")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/files/juce_File.cpp"
                "${intent_definition_modules_root}/juce_core/native/juce_Files_windows.cpp"
            ID singleeditor.core-file.basic-io OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            MEMBERS juce::File::existsAsFile juce::File::getFileName juce::File::getFullPathName
                juce::File::getSize juce::File::createInputStream juce::File::getLastModificationTime
                juce::File::File juce::File::~File juce::File::parseAbsolutePath
                juce::File::addTrailingSeparator juce::File::isAbsolutePath
                juce::File::getCurrentWorkingDirectory juce::File::getParentDirectory
                juce::File::getChildFile juce::File::getSiblingFile
                juce::File::getFileExtension juce::File::getFileNameWithoutExtension
                juce::File::hasFileExtension juce::File::withFileExtension
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary source-file identity and reader opening before playable publication"
            RETENTION "Core file primitives shared with explicit later operations")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_core/files/juce_File.cpp"
                "${intent_definition_modules_root}/juce_core/native/juce_Files_windows.cpp"
            ID singleeditor.core-file.cli-input-discovery OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            TARGETS SingleEditorCLI
            SCOPES "juce::DirectoryIterator"
                "juce::RangedDirectoryIterator"
            MEMBERS "juce::File::findChildFiles"
            TRIGGER "CLI input argument admission expands an explicitly supplied wildcard through CliArguments addInputPath"
            RETENTION "Request-local directory traversal before the requested CLI operation without declaring GUI startup scanning")
    endif()
endforeach()
# Codec factories/implementations are reached by an admitted format request or
# the explicit fallback registry; ordinary WAV uses its direct reader first.
# Reader and writer instances retain their own streams/buffers. These are scoped
# declarations inside the mixed JUCE unit, never ownership of all audio formats.
single_editor_declare_code_unit(
    ID singleeditor.core-playback.render-callbacks OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE Source/Audio/AudioEngine.cpp
    MEMBERS singleeditor::AudioEngine::DocumentAudioSource::DocumentAudioSource
        singleeditor::AudioEngine::DocumentAudioSource::~DocumentAudioSource
        singleeditor::AudioEngine::DocumentAudioSource::prepareToPlay
        singleeditor::AudioEngine::DocumentAudioSource::releaseResources
        singleeditor::AudioEngine::DocumentAudioSource::getNextAudioBlock
        singleeditor::AudioEngine::DocumentAudioSource::setNextReadPosition
        singleeditor::AudioEngine::DocumentAudioSource::getNextReadPosition
        singleeditor::AudioEngine::DocumentAudioSource::getTotalLength
        singleeditor::AudioEngine::DocumentAudioSource::isLooping
        singleeditor::AudioEngine::TransportRenderSource::TransportRenderSource
        singleeditor::AudioEngine::TransportRenderSource::~TransportRenderSource
        singleeditor::AudioEngine::TransportRenderSource::prepareToPlay
        singleeditor::AudioEngine::TransportRenderSource::releaseResources
        singleeditor::AudioEngine::TransportRenderSource::getNextAudioBlock
        singleeditor::AudioEngine::DocumentAudioSource::configureBinauralState
        singleeditor::AudioEngine::DocumentAudioSource::rebuildBinauralRendererLocked
        singleeditor::AudioEngine::DocumentAudioSource::rebuildAmbisonicFieldAnalyzerLocked
        singleeditor::AudioEngine::DocumentAudioSource::rebuildAmbisonicStageGraphLocked
        singleeditor::AudioEngine::DocumentAudioSource::collectRetiredBinauralRenderersLocked
        singleeditor::AudioEngine::DocumentAudioSource::publishAmbisonicStageGraphLocked
        singleeditor::AudioEngine::TransportRenderSource::renderSpeakerPing
    TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Ordinary document playback preparation, sample delivery, seek and transport release"
    RETENTION "Owned ordinary document and transport sources through replacement or audio-engine shutdown")

get_target_property(intent_format_sources juce_audio_formats INTERFACE_SOURCES)
get_target_property(intent_device_sources juce_audio_devices INTERFACE_SOURCES)
foreach(source IN LISTS intent_device_sources)
    if(source MATCHES "/juce_audio_devices\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/audio_io/juce_AudioDeviceManager.h"
            ID singleeditor.audio-output.device-manager OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AudioDeviceManager juce::AudioIODevice
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Admitted initial system output or explicit output configuration and device lease operation"
            PDB_VFTABLE_CLASSES
                "juce::AudioDeviceManager::LevelMeter" ".?AULevelMeter@AudioDeviceManager@juce@@"
                "juce::AudioDeviceManager::CallbackHandler" ".?AVCallbackHandler@AudioDeviceManager@juce@@"
                "juce::AudioDeviceManager" ".?AVAudioDeviceManager@juce@@"
                "juce::AudioIODevice" ".?AVAudioIODevice@juce@@"
            RETENTION "Selected device manager and device facade through output replacement or shutdown")
        single_editor_declare_code_unit(
            ID singleeditor.core-playback.transport-provider OWNER singleeditor.core-playback
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE ${intent_base} TARGETS SingleEditor SingleEditorCLI
            SCOPES "juce::AudioTransportSource"
            TRIGGER "Existing AudioEngine constructs its transport and binds ordinary document playback, seek and source release"
            PDB_VFTABLE_CLASSES
                "juce::AudioTransportSource" ".?AVAudioTransportSource@juce@@"
            RETENTION "Engine-owned ordinary transport through document replacement or engine shutdown")
single_editor_declare_code_unit(
    ID singleeditor.core-playback.source-player-provider OWNER singleeditor.core-playback
    LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE ${intent_base} TARGETS SingleEditor SingleEditorCLI
    SCOPES "juce::AudioSourcePlayer"
    TRIGGER "AudioEngine construction binds its ordinary render source, then admitted selected-output readiness attaches the existing source player callback"
    PDB_VFTABLE_CLASSES
        "juce::AudioSourcePlayer" ".?AVAudioSourcePlayer@juce@@"
    RETENTION "Engine-owned source-player facade and callback binding through selected-output replacement or engine shutdown")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPacket.h"
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPConversion.h"
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPConverters.h"
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPFactory.h"
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPIterator.h"
                "${intent_definition_modules_root}/juce_audio_basics/midi/ump/juce_UMPUtils.h"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/juce_MidiDeviceListConnectionBroadcaster.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/juce_MidiDevices.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/juce_MidiDevices.h"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPEndpointId.h"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPEndpoints.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPLegacyVirtualOutput.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPOutput.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPSession.cpp"
                "${intent_definition_modules_root}/juce_audio_devices/midi_io/ump/juce_UMPSession.h"
                "${intent_definition_modules_root}/juce_audio_devices/native/juce_Midi_windows.cpp"
            ID singleeditor.audio-output.manager-midi-support OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::MidiDeviceListConnection juce::MidiDeviceListConnectionBroadcaster
                juce::MidiInput juce::MidiOutput juce::WindowsMidiHelpers
                juce::universal_midi_packets
            TARGETS SingleEditor SingleEditorCLI
            PDB_METHODS "std::_Func_impl_no_alloc<`juce::MidiDeviceListConnection::make'::`2'::<lambda_1>,void>::_Do_call"
            TRIGGER "Admitted AudioDeviceManager construction registers its existing MIDI device-list connection and endpoint listener"
            PDB_VFTABLE_CLASSES
                "juce::universal_midi_packets::Endpoints" ".?AVEndpoints@universal_midi_packets@juce@@"
                "juce::WindowsMidiHelpers::Win32::InputImplNative" ".?AVInputImplNative@Win32@WindowsMidiHelpers@juce@@"
                "juce::universal_midi_packets::Output::Impl" ".?AVImpl@Output@universal_midi_packets@juce@@"
                "juce::WindowsMidiHelpers::Win32::EndpointsImplNative" ".?AVEndpointsImplNative@Win32@WindowsMidiHelpers@juce@@"
                "juce::universal_midi_packets::Endpoints::Impl" ".?AVImpl@Endpoints@universal_midi_packets@juce@@"
                "juce::universal_midi_packets::Input::Impl" ".?AVImpl@Input@universal_midi_packets@juce@@"
                "juce::WindowsMidiHelpers::Win32::SessionImplNative" ".?AVSessionImplNative@Win32@WindowsMidiHelpers@juce@@"
                "juce::MidiInput::Impl" ".?AVImpl@MidiInput@juce@@"
                "juce::WindowsMidiHelpers::Win32::OutputDevice" ".?AVOutputDevice@Win32@WindowsMidiHelpers@juce@@"
                "juce::WindowsMidiHelpers::Win32::InputDevice" ".?AVInputDevice@Win32@WindowsMidiHelpers@juce@@"
                "juce::MidiDeviceListConnectionBroadcaster" ".?AVMidiDeviceListConnectionBroadcaster@juce@@"
            RETENTION "Existing manager-attached MIDI catalog, endpoint and callback support without adding a MIDI feature or new device request")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_devices/native/juce_WASAPI_windows.cpp"
            ID singleeditor.audio-output.wasapi OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::WasapiClasses::WASAPIDeviceBase juce::WasapiClasses::WASAPIDeviceBaseDelegate
                juce::WasapiClasses::WASAPIAudioIODevice juce::WasapiClasses::WASAPIAudioIODeviceType
                juce::WasapiClasses::WASAPIInputDevice juce::WasapiClasses::WASAPIOutputDevice
            TARGETS SingleEditor SingleEditorCLI
            MEMBERS "singleeditor::copyWasapiOutputCatalog"
                "singleeditor::copyWasapiOutputEndpoint"
                "singleeditor::createWasapiOutputByEndpoint"
            TRIGGER "Selected Windows system output or explicit WASAPI inspection and activation"
            PDB_VFTABLE_CLASSES
                "juce::WasapiClasses::WASAPIAudioIODeviceType::ChangeNotificationClient" ".?AVChangeNotificationClient@WASAPIAudioIODeviceType@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIDeviceBase::SessionEventCallback" ".?AUSessionEventCallback@WASAPIDeviceBase@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIAudioIODevice" ".?AVWASAPIAudioIODevice@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIAudioIODeviceType" ".?AVWASAPIAudioIODeviceType@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIDeviceBase" ".?AVWASAPIDeviceBase@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIDeviceBaseDelegate" ".?AUWASAPIDeviceBaseDelegate@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIInputDevice" ".?AVWASAPIInputDevice@WasapiClasses@juce@@"
                "juce::WasapiClasses::WASAPIOutputDevice" ".?AVWASAPIOutputDevice@WasapiClasses@juce@@"
            RETENTION "WASAPI type catalog, selected endpoint, callback and notification lifetime")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_devices/native/juce_DirectSound_windows.cpp"
            ID singleeditor.audio-output.directsound OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::DSoundDeviceList juce::DSoundAudioIODevice juce::DSoundAudioIODeviceType
                juce::DSoundInternalInChannel juce::DSoundInternalOutChannel
            TARGETS SingleEditor SingleEditorCLI
            MEMBERS "juce::dsDirectSoundCreate"
                "juce::dsDirectSoundCaptureCreate"
                "juce::dsDirectSoundEnumerateW"
                "juce::dsDirectSoundCaptureEnumerateW"
            TRIGGER "Admitted system-device catalog or explicitly selected DirectSound output"
            PDB_VFTABLE_CLASSES
                "juce::DSoundAudioIODevice" ".?AVDSoundAudioIODevice@juce@@"
                "juce::DSoundAudioIODeviceType" ".?AVDSoundAudioIODeviceType@juce@@"
            RETENTION "DirectSound catalog and selected input/output channel resources until device release")
        single_editor_declare_code_unit(
            ID singleeditor.audio-output.system-factories OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            MEMBERS juce::AudioIODeviceType::createAudioIODeviceType_WASAPI
                juce::AudioIODeviceType::createAudioIODeviceType_DirectSound
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "SystemAudioDeviceManager creates the allowed Windows device types for an admitted output request"
            RETENTION "Request-owned device types, excluding ASIO from the ordinary system catalog")
        single_editor_declare_code_unit(
            ID singleeditor.audio-output.asio-driver OWNER singleeditor.audio-output
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::ASIOAudioIODevice juce::ASIOAudioIODeviceType juce::ASIOSampleFormat
            MEMBERS juce::AudioIODeviceType::createAudioIODeviceType_ASIO
            TARGETS SingleEditor
            MEMBERS "singleeditor::AsioDriverEvents::publish"
                "singleeditor::acknowledgeAsioRevalidation"
                "singleeditor::armAsioDriverEvents"
                "singleeditor::asio_native::canonicalId"
                "singleeditor::asio_native::readString"
                "singleeditor::copyAsioDriverIdentity"
                "singleeditor::prepareClosedAsioDriver"
                "singleeditor::readAsioDriverEvents"
                "singleeditor::showManagedAsioControlPanel"
            MEMBERS "juce::currentASIODev"
            TRIGGER "Explicit ASIO settings or user-selected ASIO restoration through the typed adapter"
            PDB_VFTABLE_CLASSES
                "juce::ASIOAudioIODevice" ".?AVASIOAudioIODevice@juce@@"
                "juce::ASIOAudioIODeviceType" ".?AVASIOAudioIODeviceType@juce@@"
            RETENTION "Selected ASIO type and device through discovery, inspection or active output leases")
    endif()
endforeach()
foreach(source IN LISTS intent_format_sources)
    if(source MATCHES "/juce_audio_formats\\.cpp$")
        single_editor_code_source_coordinates("${source}" intent_source intent_base)
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_formats/format/juce_AudioFormatReader.cpp"
                "${intent_definition_modules_root}/juce_audio_formats/format/juce_AudioFormatReader.h"
            ID singleeditor.core-file.pcm-readers OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AudioFormatReader juce::MemoryMappedAudioFormatReader
                juce::WavAudioFormatReader juce::MemoryMappedWavReader
                juce::AiffAudioFormatReader juce::MemoryMappedAiffReader
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary WAV or AIFF input, shared reader access and requested memory mapping"
            PDB_VFTABLE_CLASSES
                "juce::AiffAudioFormatReader" ".?AVAiffAudioFormatReader@juce@@"
                "juce::AudioFormatReader" ".?AVAudioFormatReader@juce@@"
                "juce::MemoryMappedAiffReader" ".?AVMemoryMappedAiffReader@juce@@"
                "juce::MemoryMappedAudioFormatReader" ".?AVMemoryMappedAudioFormatReader@juce@@"
                "juce::MemoryMappedWavReader" ".?AVMemoryMappedWavReader@juce@@"
                "juce::WavAudioFormatReader" ".?AVWavAudioFormatReader@juce@@"
            RETENTION "Reader-owned input streams and mapped ranges through document or CLI input release")
        single_editor_declare_code_unit(
            ID singleeditor.core-file.pcm-factories OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            MEMBERS juce::AudioFormat::AudioFormat juce::AudioFormat::~AudioFormat
                juce::AudioFormat::getFileExtensions juce::AudioFormat::canHandleFile
                juce::WavAudioFormat::WavAudioFormat juce::WavAudioFormat::~WavAudioFormat
                juce::WavAudioFormat::createReaderFor juce::WavAudioFormat::createMemoryMappedReader
                juce::AiffAudioFormat::AiffAudioFormat juce::AiffAudioFormat::~AiffAudioFormat
                juce::AiffAudioFormat::createReaderFor juce::AiffAudioFormat::createMemoryMappedReader
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Direct ordinary PCM format selection and stream or mapped reader construction"
            RETENTION "Request-owned format descriptors with shared format-base implementation")
        single_editor_declare_code_unit(
            ID singleeditor.core-file.pcm-metadata OWNER singleeditor.core-file
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AiffFileHelpers::BASCChunk juce::AiffFileHelpers::CATEChunk
            MEMBERS juce::AiffFileHelpers::InstChunk::copyTo juce::AiffFileHelpers::chunkName
                juce::WavFileHelpers::IXMLChunk::addToMetadata juce::WavFileHelpers::IXMLChunk::getAswgMetadataKeys
                juce::WavFileHelpers::AcidChunk::addToMetadata juce::WavFileHelpers::AcidChunk::setBoolFlag
                juce::WavFileHelpers::SMPLChunk::setValue juce::WavFileHelpers::InstChunk::setValue
                juce::WavFileHelpers::CueChunk::setValue juce::WavFileHelpers::canonicalWavChannelSet
            TARGETS SingleEditor SingleEditorCLI
            MEMBERS "juce::WavFileHelpers::ListInfoChunk::types"
            TRIGGER "Ordinary WAV and AIFF readers decode embedded metadata and canonical channel layout"
            RETENTION "Reader-local metadata values and format helpers, with writer conversion declared separately")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${CMAKE_CURRENT_BINARY_DIR}/single-editor-juce-formats/juce_WavAudioFormat.cpp"
                "${intent_definition_modules_root}/juce_audio_formats/codecs/juce_AiffAudioFormat.cpp"
            ID singleeditor.document-save.pcm-writers OWNER singleeditor.document-save
            LIFECYCLE CoreDeferred SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::AudioFormatWriter juce::WavAudioFormatWriter juce::AiffAudioFormatWriter
            MEMBERS juce::WavAudioFormat::createWriterFor juce::AiffAudioFormat::createWriterFor
                juce::WavFileHelpers::IXMLChunk::createFrom
                juce::WavFileHelpers::BWAVChunk::createFrom juce::WavFileHelpers::TracktionChunk::createFrom
                juce::WavFileHelpers::ListInfoChunk::createFrom juce::WavFileHelpers::ListInfoChunk::writeValue
                juce::WavFileHelpers::ListChunk::appendLabelOrNoteChunk juce::WavFileHelpers::ListChunk::getValue
                juce::WavFileHelpers::SMPLChunk::getValue juce::WavFileHelpers::InstChunk::getValue
                juce::WavFileHelpers::AcidChunk::getFlagIfPresent
                juce::AiffFileHelpers::InstChunk::getValue8 juce::AiffFileHelpers::InstChunk::getValue16
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Explicit Save, Save As or CLI output operation requiring PCM encoding"
            PDB_VFTABLE_CLASSES
                "juce::AiffAudioFormatWriter" ".?AVAiffAudioFormatWriter@juce@@"
                "juce::AudioFormatWriter" ".?AVAudioFormatWriter@juce@@"
                "juce::WavAudioFormatWriter" ".?AVWavAudioFormatWriter@juce@@"
            RETENTION "Writer-owned output streams and pending output buffers through finalization or cancellation")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_formats/codecs/juce_FlacAudioFormat.cpp"
            ID singleeditor.codec.flac OWNER singleeditor.codec.flac
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::FlacAudioFormat juce::FlacReader juce::FlacWriter juce::FlacNamespace
            # C-linkage entries lose the enclosing namespace in MAP/PDB names.
            # Exact declarations audited against the bundled libFLAC definitions.
            MEMBERS
                FLAC__format_blocksize_is_subset FLAC__format_cuesheet_is_legal
                FLAC__format_picture_is_legal FLAC__format_sample_rate_is_subset
                FLAC__format_sample_rate_is_valid FLAC__format_seektable_is_legal
                FLAC__format_seektable_sort
                FLAC__stream_decoder_delete FLAC__stream_decoder_finish FLAC__stream_decoder_flush
                FLAC__stream_decoder_get_bits_per_sample FLAC__stream_decoder_get_channels
                FLAC__stream_decoder_get_decode_position FLAC__stream_decoder_get_state
                FLAC__stream_decoder_get_total_samples FLAC__stream_decoder_init_stream
                FLAC__stream_decoder_new FLAC__stream_decoder_process_single
                FLAC__stream_decoder_process_until_end_of_metadata
                FLAC__stream_decoder_process_until_end_of_stream FLAC__stream_decoder_reset
                FLAC__stream_decoder_seek_absolute
                FLAC__stream_encoder_delete FLAC__stream_encoder_finish
                FLAC__stream_encoder_get_bits_per_sample FLAC__stream_encoder_get_blocksize
                FLAC__stream_encoder_get_verify_decoder_state FLAC__stream_encoder_init_stream
                FLAC__stream_encoder_new FLAC__stream_encoder_process
                FLAC__stream_encoder_set_apodization FLAC__stream_encoder_set_bits_per_sample
                FLAC__stream_encoder_set_blocksize FLAC__stream_encoder_set_channels
                FLAC__stream_encoder_set_compression_level FLAC__stream_encoder_set_do_escape_coding
                FLAC__stream_encoder_set_do_exhaustive_model_search FLAC__stream_encoder_set_do_mid_side_stereo
                FLAC__stream_encoder_set_do_qlp_coeff_prec_search FLAC__stream_encoder_set_loose_mid_side_stereo
                FLAC__stream_encoder_set_max_lpc_order FLAC__stream_encoder_set_max_residual_partition_order
                FLAC__stream_encoder_set_min_residual_partition_order FLAC__stream_encoder_set_qlp_coeff_precision
                FLAC__stream_encoder_set_rice_parameter_search_dist FLAC__stream_encoder_set_sample_rate
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Admitted FLAC read/write or explicit format-fallback registry request"
            PDB_VFTABLE_CLASSES
                "juce::FlacAudioFormat" ".?AVFlacAudioFormat@juce@@"
                "juce::FlacReader" ".?AVFlacReader@juce@@"
                "juce::FlacWriter" ".?AVFlacWriter@juce@@"
            RETENTION "Linked codec code with reader/writer-owned streams and state plus thread-retained fallback format descriptors")
        single_editor_declare_code_unit(
            INCLUDED_DEFINITIONS
                "${intent_definition_modules_root}/juce_audio_formats/codecs/juce_OggVorbisAudioFormat.cpp"
                "${intent_definition_modules_root}/juce_audio_formats/codecs/oggvorbis/bitwise.c"
            ID singleeditor.codec.ogg-vorbis OWNER singleeditor.codec.ogg-vorbis
            LIFECYCLE BackendLazy SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            SCOPES juce::OggVorbisAudioFormat juce::OggReader juce::OggWriter juce::OggVorbisNamespace
            # Exact C-linkage entries from framing.c and the bundled libvorbis.
            MEMBERS
                ogg_page_bos ogg_page_checksum_set ogg_page_continued ogg_page_eos
                ogg_page_granulepos ogg_page_pageno ogg_page_serialno ogg_page_version
                ogg_stream_check ogg_stream_clear ogg_stream_flush ogg_stream_init
                ogg_stream_iovecin ogg_stream_packetin ogg_stream_packetout ogg_stream_packetpeek
                ogg_stream_pagein ogg_stream_pageout ogg_stream_reset ogg_stream_reset_serialno
                ogg_sync_buffer ogg_sync_check ogg_sync_clear ogg_sync_init
                ogg_sync_pageseek ogg_sync_reset ogg_sync_wrote
                ov_clear ov_comment ov_info ov_open_callbacks ov_pcm_seek ov_pcm_seek_page
                ov_pcm_tell ov_pcm_total ov_raw_seek ov_read_float ov_time_seek ov_time_seek_page ov_time_total
                vorbis_analysis vorbis_analysis_blockout vorbis_analysis_buffer vorbis_analysis_headerout
                vorbis_analysis_init vorbis_analysis_wrote vorbis_bitrate_addblock vorbis_bitrate_flushpacket
                vorbis_block_clear vorbis_block_init vorbis_comment_add vorbis_comment_add_tag
                vorbis_comment_clear vorbis_comment_init vorbis_comment_query vorbis_dsp_clear
                vorbis_encode_init_vbr vorbis_encode_setup_init vorbis_encode_setup_vbr
                vorbis_info_blocksize vorbis_info_clear vorbis_info_init vorbis_packet_blocksize
                vorbis_synthesis vorbis_synthesis_blockin vorbis_synthesis_halfrate_p
                vorbis_synthesis_headerin vorbis_synthesis_idheader vorbis_synthesis_init
                vorbis_synthesis_pcmout vorbis_synthesis_read vorbis_synthesis_restart vorbis_synthesis_trackonly
            TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Admitted Ogg Vorbis read/write or explicit format-fallback registry request"
            PDB_VFTABLE_CLASSES
                "juce::OggReader" ".?AVOggReader@juce@@"
                "juce::OggVorbisAudioFormat" ".?AVOggVorbisAudioFormat@juce@@"
                "juce::OggWriter" ".?AVOggWriter@juce@@"
            RETENTION "Linked codec code with reader/writer-owned streams and state plus thread-retained fallback format descriptors")
    endif()
endforeach()
get_target_property(intent_samplerate_sources samplerate SOURCES)
get_target_property(intent_samplerate_root samplerate SOURCE_DIR)
foreach(source IN LISTS intent_samplerate_sources)
    if(source MATCHES "(samplerate|src_linear|src_sinc|src_zoh)\\.c$")
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${intent_samplerate_root}")
        single_editor_code_source_coordinates("${absolute}" intent_source intent_base)
        get_filename_component(unit_name "${source}" NAME_WE)
        single_editor_declare_code_unit(
            ID "singleeditor.playback-rate.source.${unit_name}" OWNER singleeditor.playback-rate
            LIFECYCLE CoreEager SOURCE "${intent_source}" SOURCE_BASE "${intent_base}"
            WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
            TRIGGER "Ordinary PlaybackRateSource preparation validates ratio and converts source/device rate mismatch"
            RETENTION "Linked resampler provider and transport-owned converter state")
    endif()
endforeach()

# Dedicated workspace/view TUs are reached through the existing post-Core host
# or explicit dialog factories. Keep mixed MainComponent, MetadataSidebar and
# AsioSettingsSession (including its early saved-selection query) separate.
foreach(source IN ITEMS Source/UI/Workspace/WorkspaceModuleLayout.cpp
        Source/UI/Workspace/WorkspacePresetCatalog.cpp
        Source/UI/Workspace/WorkspaceFloatingWindowModel.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.workspace.source.${unit_name}" OWNER singleeditor.workspace
        LIFECYCLE PostCoreWorkspace SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "CoreReady workspace host attachment and workspace layout/preset operations"
        RETENTION "Host-owned canonical layout, Simple projection and retained workspace preset state")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.workspace.playlist-view OWNER singleeditor.workspace
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/Components/PlaylistSidebarComponent.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Empty-workspace construction or current-generation real CoreReady creates the playlist view"
    RETENTION "One main-view-owned playlist listener and view reused by the borrowed workspace host across document generations")
single_editor_declare_code_unit(
    ID singleeditor.output-meter.toolbar-view OWNER singleeditor.output-meter
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/Components/MeterBridgeComponent.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Empty-workspace construction or real CoreReady creates the toolbar view, analysis requires its separate visible CoreReady lease"
    RETENTION "Main-view-owned toolbar retains presentation only, hide, readiness withdrawal and destruction release the producer lease")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.tray-pack-loader OWNER singleeditor.window-shell
    LIFECYCLE CoreDeferred SOURCE Source/App/WindowShellResources.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit tray action starts the existing owner worker after its window-shell admission checks"
    RETENTION "Bounded verified tray-pack bytes and decoded image for the requesting tray operation, not the initial window icon")

# Shared admitted bank operations have dedicated implementation boundaries;
# they are also called before a playback backend is requested.
single_editor_declare_code_unit(
    ID singleeditor.bnk.shared-state-selection OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/Bnk/BnkStateSelection.cpp TARGETS SingleEditor
    SCOPES singleeditor::bnkstate
    TRIGGER "Admitted bank metadata, event controls or preview requests resolve group-qualified State identities and options"
    RETENTION "Request-local strings and State descriptors, no decoder or startup registration")
single_editor_declare_code_unit(
    ID singleeditor.bnk.shared-media-locator OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/Bnk/BnkMediaLocator.cpp TARGETS SingleEditor
    SCOPES singleeditor::bnkmedia
    PDB_METHODS singleeditor::BoundedSidecarInput::check
        singleeditor::BoundedSidecarInput::continueWork singleeditor::BoundedSidecarInput::files
        singleeditor::BoundedSidecarInput::reject singleeditor::BoundedSidecarInput::rejectText
        singleeditor::BoundedSidecarInput::withinRoot
    TRIGGER "Admitted bank document or preview request locates an original media source within its caller-owned bounded scope"
    RETENTION "Request-local file identities, preserving the caller cancellation scope without granting an audio read lease")
single_editor_declare_code_unit(
    ID singleeditor.window-shell.classic-appearance OWNER singleeditor.window-shell
    LIFECYCLE CoreEager SOURCE Source/UI/LookAndFeel/ClassicLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ClassicLookAndFeel::ClassicLookAndFeel singleeditor::ClassicLookAndFeel::~ClassicLookAndFeel
    TRIGGER "Selected Classic renderer construction through the initial window appearance factory"
    RETENTION "Application-owned Classic look and feel through native window destruction")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.classic-controls OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/LookAndFeel/ClassicLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ClassicLookAndFeel::drawButtonBackground singleeditor::ClassicLookAndFeel::drawButtonText
        singleeditor::ClassicLookAndFeel::drawToggleButton singleeditor::ClassicLookAndFeel::drawComboBox
        singleeditor::ClassicLookAndFeel::positionComboBoxText singleeditor::ClassicLookAndFeel::getComboBoxFont
        singleeditor::ClassicLookAndFeel::getTextButtonFont
    TRIGGER "Initial ordinary controls request drawing and layout from the selected Classic renderer"
    RETENTION "Application-owned control appearance shared with later admitted controls")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.classic-text-editing OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/LookAndFeel/ClassicLookAndFeel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::ClassicLookAndFeel::fillTextEditorBackground singleeditor::ClassicLookAndFeel::drawTextEditorOutline
    TRIGGER "Explicit text editing or requested dialog draws a Classic text editor"
    RETENTION "Application-owned text editor appearance, no initial ordinary reader work")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.about-dialog-completion OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    PDB_CLASSES "singleeditor::MainComponent::performCoreCommand::__l145::<lambda_1>"
    TRIGGER "Explicit About command completion restores focus through the retained safe component pointer"
    RETENTION "Requested dialog callback and focus token until completion, distinct from eager command dispatch")

single_editor_declare_code_unit(
    ID singleeditor.bnk.document-admission OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/Bnk/BnkDocument.cpp TARGETS SingleEditor
    SCOPES singleeditor::BoundedSidecarInput
    MEMBERS singleeditor::BnkDocument::createStateFromFile
        singleeditor::BnkDocument::loadFile
        singleeditor::BnkDocument::adoptParsedState
        singleeditor::BnkDocument::selectEventById
        singleeditor::BnkDocument::selectMediaById
        singleeditor::BnkDocument::getPlayableMedia
        singleeditor::BnkDocument::getPlayableMediaById
        singleeditor::BnkDocument::getEmbeddedMediaCacheFile
        "singleeditor::`anonymous namespace'::tryFindOriginalMediaFile"
        "singleeditor::`anonymous namespace'::getEmbeddedMediaCacheDir"
        "singleeditor::`anonymous namespace'::getResolvedEventMediaForContext"
    PDB_FUNCTIONS "std::_Sort_unchecked<singleeditor::BnkDocument::PlayableMediaDescriptor *,`singleeditor::BnkDocument::getPlayableMedia'::`2'::<lambda_2> >"
    MEMBERS "singleeditor::BnkDocument::getSwitchValuesForSelectedEvent"
        "singleeditor::BnkDocument::getSelectedEventMediaIds"
        "singleeditor::BnkDocument::getSelectedEventMediaNames"
        "singleeditor::BnkDocument::rtpcBindingTargetsEvent"
        "singleeditor::BnkDocument::describePlaybackHint"
        "singleeditor::BnkDocument::addListener"
        "singleeditor::BnkDocument::removeListener"
    TRIGGER "Explicit bank open or event/media selection admits parsed bank state and resolves requested bank audio"
    RETENTION "Selected bank state and event/media references until replacement, without assigning the eager empty-state constructor or passive queries")
single_editor_declare_code_unit(
    ID singleeditor.bnk.passive-state OWNER singleeditor.bnk
    LIFECYCLE CoreEager SOURCE "Source/Bnk/BnkDocument.cpp" SOURCE_BASE project TARGETS SingleEditor
    MEMBERS "singleeditor::BnkDocument::clear"
        "singleeditor::BnkDocument::getState"
        "singleeditor::BnkDocument::getBankInfo"
        "singleeditor::BnkDocument::hasBank"
        "singleeditor::BnkDocument::notifyChanged"
    TRIGGER "Ordinary first-window state queries and ordinary reader-failure cleanup may inspect or clear the existing empty bank state before real CoreReady"
    RETENTION "Existing passive bank state and copied bank-info values. This declaration does not admit parsing, bank controls or media resolution")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.service-owner OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/Audio/SpatialEnergyService.cpp TARGETS SingleEditor SingleEditorCLI
    SCOPES singleeditor::SpatialEnergyService::Worker
        singleeditor::SpatialEnergyService::State
        singleeditor::SpatialEnergyService::Control
        singleeditor::SpatialEnergyService::Lease
        singleeditor::SpatialEnergyService::Configuration
    MEMBERS singleeditor::SpatialEnergyService::acquire
        singleeditor::SpatialEnergyService::withdraw
        singleeditor::SpatialEnergyService::release
        singleeditor::SpatialEnergyService::configure
    TRIGGER "An explicit post-Core analysis lease creates the worker, which prepares requested route and geometry state"
    RETENTION "Lease-owned worker and filter state drained when the last consumer releases, excluding the passive engine seam and ordinary callback guard")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.glass-sampling OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/LookAndFeel/ModernGlassBackdrop.cpp
    WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "The enabled visible ModernGlassSurface consumer requests its existing post-Core backdrop refresh or an explicit hover refresh"
    RETENTION "Bounded sampled and blurred images owned by the visible consumer until refresh, hide or disable")
foreach(source IN ITEMS Source/Metadata/TypedMetadataInspectorModel.cpp
        Source/UI/Components/TypedMetadataInspectorComponent.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.metadata.source.${unit_name}" OWNER singleeditor.metadata
        LIFECYCLE CoreDeferred SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Current-generation CoreReady promotion activates typed metadata inspector"
        RETENTION "Current document metadata model/view, generation replacement releases prior state")
endforeach()
foreach(source IN ITEMS Source/UI/Components/AmbisonicFieldPanelComponent.cpp
        Source/UI/Components/SpeakerGeometryMagComponent.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.ambisonics.view.${unit_name}" OWNER singleeditor.ambisonics
        LIFECYCLE ModuleLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Post-Core visible or explicitly opened Ambisonic Field Mag factory"
        RETENTION "Field Mag and its owned speaker-geometry editor, released with the workspace view")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.output-meter.view OWNER singleeditor.output-meter
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/OutputMeterPanelComponent.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Post-Core visible or explicitly opened Output Meter workspace view"
    RETENTION "Workspace-owned meter view and visible metering consumer lease")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.view OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/SpatialEnergyPanelComponent.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Post-Core explicit Spatial Energy Mag view factory"
    RETENTION "Spatial view and its requested energy/output diagnostics consumers")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.visual-field OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/SpatialEnergyVisualField.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Visible Spatial Energy view prepares its field map or renders the current leased snapshot"
    RETENTION "View-owned worker field and plane/sphere cache, released with the view")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.meter OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/Audio/SpatialEnergyMeter.cpp WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Explicit post-Core Spatial Energy consumer lease prepares the analysis worker"
    RETENTION "Lease-owned meter and snapshots, drained and destroyed on final consumer release")
single_editor_declare_code_unit(
    ID singleeditor.spatial-energy.layout OWNER singleeditor.spatial-energy
    LIFECYCLE ModuleLazy SOURCE Source/Audio/SpatialEnergyLayout.cpp WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
    TRIGGER "Leased Spatial Energy preparation or explicit diagnostic capture of the existing output route"
    RETENTION "Copied layout facts in leased snapshots or explicit diagnostic evidence, without worker activation for diagnostics")
single_editor_declare_code_unit(
    ID singleeditor.timeline-markers.view OWNER singleeditor.timeline-markers
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/TimelineMarkersComponent.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Post-Core visible Timeline Markers Mag factory"
    RETENTION "Workspace marker view bound to current document and transport state")
single_editor_declare_code_unit(
    ID singleeditor.audio-output.settings-view OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/UI/Components/AsioSettingsComponent.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit Audio output routing dialog for the selected WASAPI or ASIO backend"
    RETENTION "Routing dialog and its shared output settings session")

# Header emission follows the actual winning compiland. These classes serve
# ordinary system output as well as explicit diagnostic output requests.
single_editor_declare_code_unit(
    ID singleeditor.audio-output.system-manager-gui OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/Diagnostics/DeviceDiagnosticProbe.cpp TARGETS SingleEditor
    SCOPES singleeditor::SystemAudioDeviceManager
    PDB_FUNCTIONS "std::make_unique<singleeditor::SystemAudioDeviceManager,0>"
    TRIGGER "Admitted ordinary system output or explicit device diagnostic request creates the shared system manager"
    PDB_VFTABLE_CLASSES
        "singleeditor::SystemAudioDeviceManager" ".?AVSystemAudioDeviceManager@singleeditor@@"
    RETENTION "System output manager and allowed device types until output replacement or request shutdown")
single_editor_declare_code_unit(
    ID singleeditor.audio-output.system-manager-cli OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/Audio/AudioEngine.cpp TARGETS SingleEditorCLI
    SCOPES singleeditor::SystemAudioDeviceManager
    PDB_FUNCTIONS "std::make_unique<singleeditor::SystemAudioDeviceManager,0>"
    TRIGGER "Admitted CLI playback or explicit device request creates the shared system manager"
    PDB_VFTABLE_CLASSES
        "singleeditor::SystemAudioDeviceManager" ".?AVSystemAudioDeviceManager@singleeditor@@"
    RETENTION "System output manager and allowed device types until playback or request shutdown")
single_editor_declare_code_unit(
    ID singleeditor.core-file.async-load-job OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    SCOPES "singleeditor::`anonymous-namespace'::AsyncFileLoadJob"
    PDB_METHODS "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`12'::<lambda_3> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`15'::<lambda_4> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`2'::<lambda_8> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`26'::<lambda_5> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`29'::<lambda_6> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`39'::<lambda_7> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`5'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "`juce::MessageManager::callAsync<`singleeditor::`anonymous namespace'::AsyncFileLoadJob::runJob'::`8'::<lambda_2> >'::`2'::AsyncCallInvoker::messageCallback"
    TRIGGER "Ordinary initial file open creates the worker for reader admission, metadata and leading waveform publication"
    RETENTION "Queued or running document load until completion or cancellation, including shared optional request guards but excluding separately owned backend implementations")

single_editor_declare_code_unit(
    ID singleeditor.preferences.view OWNER singleeditor.preferences
    LIFECYCLE ModuleLazy SOURCE Source/UI/Components/PreferencesComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::PreferencesComponent::PreferencesComponent
        singleeditor::PreferencesComponent::~PreferencesComponent
        singleeditor::PreferencesComponent::paint singleeditor::PreferencesComponent::resized
        singleeditor::PreferencesComponent::refreshCloseBehaviour
        singleeditor::PreferencesComponent::refreshCacheInfo
        singleeditor::PreferencesComponent::refreshWwiseProjectPath
        singleeditor::PreferencesComponent::refreshChannelOrderDefaults
        singleeditor::PreferencesComponent::refreshDeviceControls
        singleeditor::PreferencesComponent::refreshDeviceDetails
        singleeditor::PreferencesComponent::applySelectedDeviceType
        singleeditor::PreferencesComponent::applySelectedDeviceSetup
        singleeditor::PreferencesComponent::applySelectedDefaultChannelOrder
        singleeditor::PreferencesComponent::setDeviceStatus
        singleeditor::PreferencesComponent::formatBytes
        singleeditor::PreferencesComponent::formatSampleRate
        singleeditor::PreferencesComponent::formatBufferSize
        "singleeditor::`anonymous namespace'::configureLabel"
        "singleeditor::`anonymous namespace'::configureComboBox"
        "singleeditor::`anonymous namespace'::presetIdForComboSelection"
        "singleeditor::`anonymous namespace'::populateDefaultChannelOrderBox"
        "singleeditor::`anonymous namespace'::countEnabledChannels"
    PDB_CLASSES
        "std::_Func_impl_no_alloc<``singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_7>::operator()'::`2'::<lambda_1>,void,juce::FileChooser const &>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_10>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_11>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_12>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_13>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_14>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_15>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_16>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_17>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_18>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_19>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_1>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_2>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_3>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_4>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_5>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_6>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_7>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_8>,void>"
        "std::_Func_impl_no_alloc<`singleeditor::PreferencesComponent::PreferencesComponent'::`2'::<lambda_9>,void>"
    TRIGGER "Explicit Preferences dialog command"
    RETENTION "Preferences dialog and its explicit settings callbacks")
single_editor_declare_code_unit(
    ID singleeditor.preferences.controller OWNER singleeditor.preferences
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::showPreferences"
    PDB_CLASSES "singleeditor::MainComponent::showPreferences::__l5::<lambda_2>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showPreferences'::`2'::<lambda_1>,void>"
    TRIGGER "Explicit application.preferences command creates the Preferences dialog"
    RETENTION "Dialog-owned ASIO navigation and modal completion callbacks, with focus restoration at dismissal")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.startup-preferences OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreEager SOURCE Source/UI/Components/PreferencesComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::PreferencesComponent::applyStoredChannelOrderDefaults
        singleeditor::PreferencesComponent::applyStoredVisualPreferences
    TRIGGER "Application initialise applies stored channel-order, theme, material and reduced-motion defaults before MainWindow"
    RETENTION "Stored default values shared with later explicit Preferences actions without dialog construction or device enumeration")
single_editor_declare_code_unit(
    ID singleeditor.command-palette.view OWNER singleeditor.command-palette
    LIFECYCLE ModuleLazy SOURCE Source/UI/Commands/CommandPaletteComponent.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit command-palette open command"
    RETENTION "Palette view and captured invocation/focus context until dismissal")
single_editor_declare_code_unit(
    ID singleeditor.command-palette.model OWNER singleeditor.command-palette
    LIFECYCLE ModuleLazy SOURCE Source/UI/Commands/CommandPaletteModel.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit palette open builds the searchable command index and visible palette input requests matches"
    RETENTION "Palette-owned index and query results, released when the palette closes")
single_editor_declare_code_unit(
    ID singleeditor.command-palette.registry-list OWNER singleeditor.command-palette
    LIFECYCLE ModuleLazy SOURCE Source/UI/Commands/GuiCommandModel.cpp TARGETS SingleEditor
    MEMBERS singleeditor::GuiCommandRegistry::list singleeditor::GuiCommandRegistry::commandCount
    PDB_FUNCTIONS "std::_Buffered_inplace_merge_unchecked<singleeditor::GuiCommandSnapshot *,`singleeditor::GuiCommandRegistry::list'::`2'::<lambda_1> >"
    PDB_FUNCTIONS "std::_Buffered_merge_sort_unchecked<singleeditor::GuiCommandSnapshot *,`singleeditor::GuiCommandRegistry::list'::`2'::<lambda_1> >"
        "std::_Chunked_merge_unchecked<singleeditor::GuiCommandSnapshot *,singleeditor::GuiCommandSnapshot *,`singleeditor::GuiCommandRegistry::list'::`2'::<lambda_1> >"
        "std::_Insertion_sort_unchecked<singleeditor::GuiCommandSnapshot *,`singleeditor::GuiCommandRegistry::list'::`2'::<lambda_1> >"
    TRIGGER "Admitted CommandPaletteModel builds its searchable command list"
    RETENTION "Request-local command snapshot list with registration and ordinary dispatch remaining core-owned")
single_editor_declare_code_unit(
    ID singleeditor.command-palette.controller OWNER singleeditor.command-palette
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::showCommandPalette"
        "singleeditor::MainComponent::dismissCommandPalette"
    PDB_CLASSES "singleeditor::MainComponent::showCommandPalette::__l2::<lambda_1>::()::__l2::<lambda_1>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showCommandPalette'::`2'::<lambda_1>,void,std::basic_string<char,std::char_traits<char>,std::allocator<char> > >"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showCommandPalette'::`2'::<lambda_2>,void>"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::showCommandPalette'::`2'::<lambda_1>::operator()'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::`scalar deleting destructor'"
        "`juce::MessageManager::callAsync<``singleeditor::MainComponent::showCommandPalette'::`2'::<lambda_1>::operator()'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
    TRIGGER "Explicit palette command creates its view and installs selection and dismissal callbacks"
    RETENTION "Palette-owned view, focus token and queued selected-command dispatch until dismissal")

# The ordinary open controller also dispatches optional formats and restores a
# retained workspace after cancellation. Charge these shared entry points to
# their first ordinary-open use, without pulling their callees or lambdas into
# the declaration. Optional backend execution keeps its own units below.
single_editor_declare_code_unit(
    ID singleeditor.core-file.open-controller OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::openInitialFile
        singleeditor::MainComponent::primeStartupLoadingState
        singleeditor::MainComponent::beginPreparedInitialAudioLoad
        singleeditor::MainComponent::beginPreparedInitialAudioLoadInternal
        singleeditor::MainComponent::openPreparedInitialAudio
        singleeditor::MainComponent::openFiles
        singleeditor::MainComponent::loadPlaylistIndex
        singleeditor::MainComponent::finishReusedAudioLoad
        singleeditor::MainComponent::captureAudioReturnImage
        singleeditor::MainComponent::selectLoadedAudioEntry
        singleeditor::MainComponent::restoreAudioReturn
        singleeditor::MainComponent::ordinaryAudioLoadOwnsDocument
        singleeditor::MainComponent::observeOrdinaryAudioLoadState
        singleeditor::MainComponent::cancelOrdinaryAudioLoad
        singleeditor::MainComponent::beginAsyncLoad
        singleeditor::MainComponent::showAsyncLoadPreview
        singleeditor::MainComponent::showAsyncLoadMetadata
        singleeditor::MainComponent::completeAsyncLoad
    PDB_CLASSES
        "singleeditor::MainComponent::openFiles::__l2::<lambda_1>"
    MEMBERS "singleeditor::MainComponent::playlistChanged"
        "singleeditor::MainComponent::currentPlaylistIndexChanged"
        "singleeditor::MainComponent::showPreparedInitialAudioPreview"
        "singleeditor::MainComponent::markFileNameShownIfNeeded"
        "singleeditor::MainComponent::markLoadPlayableIfNeeded"
        "singleeditor::MainComponent::notifyStartupCoreSurfacePreparedIfNeeded"
        "singleeditor::MainComponent::notifyStartupCoreReadyIfNeeded"
        "singleeditor::MainComponent::notifyPrimaryContentVisibleIfNeeded"
        "singleeditor::MainComponent::hasProgressiveWaveformPending"
        "singleeditor::MainComponent::canQueuePlaybackDuringLoad"
        "singleeditor::MainComponent::hasPlayableAudioReady"
        "singleeditor::MainComponent::requestAudioPlaybackOrQueue"
        "singleeditor::MainComponent::loadedFileChanged"
    MEMBERS "singleeditor::MainComponent::AudioReturnImage::AudioReturnImage"
        "singleeditor::MainComponent::OrdinaryAudioLoad::OrdinaryAudioLoad"
        "singleeditor::`anonymous namespace'::fileInfoMatches"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::openFiles'::`2'::<lambda_1>,void>::_Do_call"
    TRIGGER "Ordinary initial or subsequent file open, current load publication and retained workspace return"
    RETENTION "Active document load identity and retained audio, playlist and Undo state through publication or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.core-file.playlist-mutations OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE "Source/Document/PlaylistModel.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::PlaylistModel::removeAt"
        "singleeditor::PlaylistModel::replaceCurrentFile"
        "singleeditor::PlaylistModel::moveEntry"
    TRIGGER "Explicit playlist removal or reorder, or successful user-requested Save publication"
    RETENTION "Updated playlist entry order and file identity while preserving recovered document history")
single_editor_declare_code_unit(
    ID singleeditor.core-file.playlist-input OWNER singleeditor.core-file
    LIFECYCLE CoreEager SOURCE "Source/Document/PlaylistModel.cpp" TARGETS SingleEditor
    MEMBERS "singleeditor::PlaylistModel::addFiles"
        "singleeditor::PlaylistModel::indexOfEntry"
        "singleeditor::PlaylistModel::indexOfFile"
        "singleeditor::PlaylistModel::getNumEntries"
        "singleeditor::PlaylistModel::getEntry"
        "singleeditor::PlaylistModel::getCurrentIndex"
        "singleeditor::PlaylistModel::getCurrentEntry"
        "singleeditor::PlaylistModel::setCurrentIndex"
        "singleeditor::PlaylistModel::addListener"
        "singleeditor::PlaylistModel::removeListener"
        "singleeditor::PlaylistModel::Entry::activationFile"
    TRIGGER "Ordinary initial-file admission, current entry lookup and playlist notifications before real CoreReady"
    RETENTION "Main-view playlist entries and listeners through document changes and main-view teardown")
single_editor_declare_code_unit(
    ID singleeditor.core-file.open-dialog OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::chooseAndOpenFile"
    PDB_CLASSES "singleeditor::MainComponent::chooseAndOpenFile::__l2::<lambda_1>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::chooseAndOpenFile'::`2'::<lambda_1>,void,juce::FileChooser const &>"
    TRIGGER "Explicit document.open command creates and launches the native file chooser"
    RETENTION "Chooser results and focus-return token until completion. Ordinary initial-path loading stays in open-controller")
single_editor_declare_code_unit(
    ID singleeditor.core-file.explicit-file-actions OWNER singleeditor.core-file
    LIFECYCLE CoreDeferred SOURCE "Source/UI/MainComponent.cpp" SOURCE_BASE project TARGETS SingleEditor
    MEMBERS "singleeditor::MainComponent::fileDragEnter"
        "singleeditor::MainComponent::fileDragExit"
        "singleeditor::MainComponent::filesDropped"
        "singleeditor::MainComponent::isInterestedInFileDrag"
        "singleeditor::MainComponent::isSupportedAudioPath"
        "singleeditor::MainComponent::removeCurrentPlaylistEntry"
        "singleeditor::MainComponent::requestPlaylistActivation"
    PDB_METHODS "`singleeditor::MainComponent::requestPlaylistActivation'::`2'::<lambda_1>::operator()"
        "`singleeditor::MainComponent::requestPlaylistRemoval'::`10'::<lambda_1>::operator()"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureDeferredWorkspaceControls'::`7'::<lambda_1>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureDeferredWorkspaceControls'::`7'::<lambda_2>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::removeCurrentPlaylistEntry'::`2'::<lambda_1>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::requestPlaylistActivation'::`2'::<lambda_1>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::requestPlaylistRemoval'::`10'::<lambda_1>,void>::_Do_call"
    TRIGGER "Explicit OS file drag/drop or playlist activation/removal, including the resulting document-confirmation continuation"
    RETENTION "Requested files or stable playlist entry identity until the explicit action completes. Ordinary initial-path loading remains open-controller")
single_editor_declare_code_unit(
    ID singleeditor.core-lifecycle.document-controller OWNER singleeditor.core-lifecycle
    LIFECYCLE CoreEager SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::beginDocumentGeneration
        singleeditor::MainComponent::ensureDocumentGeneration
    MEMBERS "singleeditor::MainComponent::clearReferenceAudioSuitePreview"
        "singleeditor::MainComponent::cancelReferenceAudioSuiteApply"
        "singleeditor::MainComponent::releaseDspLifecycleIfIdle"
        "singleeditor::MainComponent::clearParametricEqPreview"
        "singleeditor::MainComponent::clearTruePeakLimiterPreview"
        "singleeditor::MainComponent::cancelParametricEqApply"
        "singleeditor::MainComponent::cancelTruePeakLimiterApply"
        "singleeditor::MainComponent::cancelBnkPreview"
        "singleeditor::MainComponent::cancelPackedAudioLoad"
        "singleeditor::MainComponent::releaseBnkRuntime"
    MEMBERS "singleeditor::CoreLifecycleCoordinator::CoreLifecycleCoordinator"
        "singleeditor::MainComponent::getOutputMeterPanel"
        "singleeditor::MainComponent::getParametricEqMag"
        "singleeditor::MainComponent::getTruePeakLimiterMag"
    TRIGGER "Initial or replacement document generation, before ordinary loading and actual CoreReady milestones"
    RETENTION "Current document generation and cancellation of prior optional consumers")
single_editor_declare_code_unit(
    ID singleeditor.document-save.controller OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::saveDocument
        singleeditor::MainComponent::exportDocument
        singleeditor::MainComponent::getStoredSaveEncodingOptions
        singleeditor::MainComponent::chooseSaveEncodingOptions
        singleeditor::MainComponent::showExternalSourceSaveConflict
        singleeditor::MainComponent::beginSaveToTarget
        singleeditor::MainComponent::chooseSaveTarget
        singleeditor::MainComponent::confirmDiscardOrSaveIfNeeded
    PDB_CLASSES
        "singleeditor::MainComponent::showExternalSourceSaveConflict::__l2::<lambda_1>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::chooseSaveEncodingOptions::__l2::<lambda_1>"
        "singleeditor::MainComponent::confirmDiscardOrSaveIfNeeded::__l2::<lambda_1>"
        "singleeditor::MainComponent::showExternalSourceSaveConflict::__l2::<lambda_1>"
        "singleeditor::MainComponent::confirmDiscardOrSaveIfNeeded::__l2::<lambda_1>::()::__l10::<lambda_1>"
        "singleeditor::MainComponent::saveDocument::__l20::<lambda_1>"
        "singleeditor::MainComponent::saveDocument::__l17::<lambda_2>::()::__l25::<lambda_1>"
        "singleeditor::MainComponent::saveDocument::__l17::<lambda_2>"
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::confirmDiscardOrSaveIfNeeded'::`2'::<lambda_1>::operator()'::`10'::<lambda_1>,void,bool>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::saveDocument'::`17'::<lambda_2>::operator()<singleeditor::SingleTrackDocument::SaveWriteResult>'::`25'::<lambda_1>,void,bool>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::showExternalSourceSaveConflict'::`2'::<lambda_1>::operator()'::`2'::<lambda_1>,void,int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::saveDocument'::`17'::<lambda_2>,void,singleeditor::SingleTrackDocument::SaveWriteResult const &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showExternalSourceSaveConflict'::`2'::<lambda_1>,void,bool>::_Do_call"
    MEMBERS "singleeditor::SaveEncodingOptionsDialog::ChoiceField::resized"
    TRIGGER "Explicit Save, Save As or Export request, including the user's save decision before close or replacement"
    RETENTION "Save request, target and encoding dialogs until publication, cancellation or generation replacement")
single_editor_declare_code_unit(
    ID singleeditor.document-save.controller-closures OWNER singleeditor.document-save
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    PDB_CLASSES
        "singleeditor::MainComponent::chooseSaveTarget::__l2::<lambda_2>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::chooseSaveTarget::__l5::<lambda_1>"
        "singleeditor::MainComponent::chooseSaveTarget::__l2::<lambda_2>"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::chooseSaveTarget'::`2'::<lambda_2>::operator()'::`2'::<lambda_1>,void,std::optional<singleeditor::SingleTrackDocument::SaveEncodingOptions> >"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::chooseSaveTarget'::`5'::<lambda_1>,void,bool>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::chooseSaveTarget'::`2'::<lambda_2>,void,juce::FileChooser const &>"
        "singleeditor::MainComponent::beginSaveToTarget::__l2::<lambda_1>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::beginSaveToTarget'::`2'::<lambda_1>,void,singleeditor::SingleTrackDocument::SaveWriteResult const &>"
    TRIGGER "Callbacks created only by admitted Save target, encoding and publication requests"
    RETENTION "Save callback captures until idle notification, dialog completion or publication callback")

# Optional controller execution follows admitted Mag/backend consumers. Mixed
# MainComponent cancellation and polling helpers remain outside these lazy units.
# The settings-only startup predicate is separate from actual backend operations.
single_editor_declare_code_unit(
    ID singleeditor.parametric-eq-v1.controller OWNER singleeditor.parametric-eq-v1
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::ensureParametricEqProcessor
        singleeditor::MainComponent::setParametricEqPreviewEnabled
        singleeditor::MainComponent::updateParametricEqPreview
        singleeditor::MainComponent::applyParametricEq
        singleeditor::MainComponent::finishParametricEqApply
        singleeditor::MainComponent::updateParametricEqCurve
    SCOPES singleeditor::MainComponent::ParametricEqApplyThread
    PDB_CLASSES
        "singleeditor::MainComponent::applyParametricEq::__l2::<lambda_1>"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::applyParametricEq'::`2'::<lambda_1>,void,singleeditor::AudioSuiteApplyResult>::_Do_call"
    TRIGGER "Current CoreReady and active EQ Mag parameters, Preview or Apply request"
    PDB_VFTABLE_CLASSES
        "singleeditor::MainComponent::ParametricEqApplyThread" ".?AVParametricEqApplyThread@MainComponent@singleeditor@@"
    RETENTION "EQ processor, preview plan and admitted apply thread through completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.parametric-eq-v1.workspace-factory OWNER singleeditor.parametric-eq-v1
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_1>,void,enum singleeditor::ParametricEqMagComponent::ViewMode>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_2>,void,unsigned __int64>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_3>,void,singleeditor::ParametricEqParameters const &>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_4>,void,bool>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_5>,void>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>::operator()'::`2'::<lambda_6>,void,enum singleeditor::RendererMagSize>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_7>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted Parametric EQ view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact Parametric EQ factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.true-peak-limiter-v1.controller OWNER singleeditor.true-peak-limiter-v1
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::ensureTruePeakLimiterProcessor
        singleeditor::MainComponent::setTruePeakLimiterPreviewEnabled
        singleeditor::MainComponent::updateTruePeakLimiterPreview
        singleeditor::MainComponent::applyTruePeakLimiter
        singleeditor::MainComponent::finishTruePeakLimiterApply
        singleeditor::MainComponent::recallLastAppliedTruePeakLimiter
    SCOPES singleeditor::MainComponent::TruePeakLimiterApplyThread
    PDB_CLASSES
        "singleeditor::MainComponent::applyTruePeakLimiter::__l2::<lambda_1>"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::applyTruePeakLimiter'::`2'::<lambda_1>,void,singleeditor::AudioSuiteApplyResult>::_Do_call"
    TRIGGER "Current CoreReady and active Limiter Mag parameters, Preview, Apply or recall request"
    PDB_VFTABLE_CLASSES
        "singleeditor::MainComponent::TruePeakLimiterApplyThread" ".?AVTruePeakLimiterApplyThread@MainComponent@singleeditor@@"
    RETENTION "Limiter processor, preview or recalled plan and admitted apply thread through completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.true-peak-limiter-v1.workspace-factory OWNER singleeditor.true-peak-limiter-v1
    LIFECYCLE ModuleLazy SOURCE "Source/UI/MainComponent.cpp" TARGETS SingleEditor
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>::operator()'::`2'::<lambda_1>,void,singleeditor::TruePeakLimiterParameters const &>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>::operator()'::`2'::<lambda_2>,void,bool>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>::operator()'::`2'::<lambda_3>,void>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>::operator()'::`2'::<lambda_4>,void>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>::operator()'::`2'::<lambda_5>,void,enum singleeditor::RendererMagSize>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureWorkspaceModuleHost'::`2'::<lambda_8>,std::unique_ptr<juce::Component,std::default_delete<juce::Component> > >::_Do_call"
    TRIGGER "Current-generation CoreReady and workspace-host admitted True Peak Limiter view creation after Add/restore, followed by actions from that view"
    RETENTION "Exact True Peak Limiter factory and view action entrypoints. Excludes registration, callback storage and generic function-manager lifetime")
single_editor_declare_code_unit(
    ID singleeditor.audio-output.session OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/Audio/AsioSettingsSession.cpp TARGETS SingleEditor
    MEMBERS singleeditor::AsioSettingsSession::defaultBackend
        singleeditor::AsioSettingsSession::refreshProfiles
        singleeditor::AsioSettingsSession::shutdown
        singleeditor::AsioSettingsSession::changed
        singleeditor::AsioSettingsSession::invalidate
        singleeditor::AsioSettingsSession::selectBackend
        singleeditor::AsioSettingsSession::refreshDrivers
        singleeditor::AsioSettingsSession::chooseWindowsDefault
        singleeditor::AsioSettingsSession::selectDriver
        singleeditor::AsioSettingsSession::inspectionSelection
        singleeditor::AsioSettingsSession::inspect
        singleeditor::AsioSettingsSession::currentBus
        singleeditor::AsioSettingsSession::rebuildDraft
        singleeditor::AsioSettingsSession::selectProfile
        singleeditor::AsioSettingsSession::setFormat
        singleeditor::AsioSettingsSession::setConnection
        singleeditor::AsioSettingsSession::validFormat
        singleeditor::AsioSettingsSession::expected
        singleeditor::AsioSettingsSession::selection
        singleeditor::AsioSettingsSession::validate
        singleeditor::AsioSettingsSession::activate
        singleeditor::AsioSettingsSession::savedBackend
        singleeditor::AsioSettingsSession::restoreSavedSelection
        singleeditor::AsioSettingsSession::ping
        singleeditor::AsioSettingsSession::cancel
        singleeditor::AsioSettingsSession::canMaintainOutput
        singleeditor::AsioSettingsSession::activeBus
        singleeditor::AsioSettingsSession::retryOutput
        singleeditor::AsioSettingsSession::canOpenControlPanel
        singleeditor::AsioSettingsSession::openControlPanel
        singleeditor::AsioSettingsSession::beginMaintenance
        singleeditor::AsioSettingsSession::serviceDriverEvents
        singleeditor::AsioSettingsSession::hasVisibleConsumer
        singleeditor::AsioSettingsSession::refreshLiveStatus
        singleeditor::AsioSettingsSession::canFollowSourceSampleRate
        singleeditor::AsioSettingsSession::setFollowSourceSampleRate
        singleeditor::AsioSettingsSession::loadedFileChanged
        singleeditor::AsioSettingsSession::serviceFollowSource
        singleeditor::AsioSettingsSession::persistCommitted
        singleeditor::AsioSettingsSession::poll
    TRIGGER "Explicit audio output setup or remembered user-selected backend restoration after startup handoff"
    RETENTION "Owned output session, discovery and device lease transactions until cancellation, replacement or shutdown")
single_editor_declare_code_unit(
    ID singleeditor.core-lifecycle.output-selection OWNER singleeditor.core-lifecycle
    LIFECYCLE CoreEager SOURCE Source/Audio/AsioSettingsSession.cpp TARGETS SingleEditor
    MEMBERS singleeditor::AsioSettingsSession::hasSavedSelection
    TRIGGER "Minimal startup decision whether a user-selected output must be reserved before ordinary device warmup"
    RETENTION "Read-only application settings query, no backend discovery or session allocation")

# MainComponent is a mixed controller. These selectors cover only named entry
# points for admitted operations; ordinary-open classifiers, cancellation,
# refresh and the shared timer remain outside these lazy declarations. Admitted
# completion handlers below have explicit request/service guards at their callers.
# Lambdas and
# shared helpers require their own evidence and are not inherited by name.
single_editor_declare_code_unit(
    ID singleeditor.recovery.completion OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::attachRecoveryAfterCore
        singleeditor::MainComponent::discoverRecoveryAfterCore
        singleeditor::MainComponent::pollRecoveryRestore
        singleeditor::MainComponent::pollRecovery
        singleeditor::MainComponent::continueRecoveryClose
        singleeditor::MainComponent::attachRecoveredCandidate
        singleeditor::MainComponent::publishRecoveredPeaks
    PDB_CLASSES
        "singleeditor::MainComponent::continueRecoveryClose::__l2::<lambda_2>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::discoverRecoveryAfterCore::__l5::<lambda_1>"
        "singleeditor::MainComponent::discoverRecoveryAfterCore::__l5::<lambda_1>::()::__l8::<lambda_1>"
        "singleeditor::MainComponent::discoverRecoveryAfterCore::__l5::<lambda_1>::()::__l2::<lambda_2>"
        "singleeditor::MainComponent::continueRecoveryClose::__l2::<lambda_2>"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::discoverRecoveryAfterCore'::`5'::<lambda_1>::operator()'::`2'::<lambda_2> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::continueRecoveryClose'::`2'::<lambda_2>::operator()'::`2'::<lambda_1>,void,bool>::_Do_call"
        "std::_Func_impl_no_alloc<``singleeditor::MainComponent::discoverRecoveryAfterCore'::`5'::<lambda_1>::operator()'::`8'::<lambda_1>,bool>::_Do_call"
    TRIGGER "Post-Core or visible empty-workspace discovery, admitted protection/restore work, or explicit protected-document close"
    RETENTION "Existing recovery service, owner jobs and full-history recovered publications until completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.bnk.open-completion OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponentBankOpen.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::pollBnkOpen
        singleeditor::MainComponent::bnkOpenWorkspaceMatches
        singleeditor::MainComponent::restoreBnkOpenSelection
    TRIGGER "An admitted or queued bank-open request and its completion or selection restoration"
    RETENTION "Pending bank request, retained workspace identity and cancelled worker drain")
single_editor_declare_code_unit(
    ID singleeditor.bnk.preview-completion OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::pollBnkPreview
        singleeditor::MainComponent::bnkPreviewWorkspaceMatches
        singleeditor::MainComponent::formatBnkDetails
    PDB_CLASSES "singleeditor::MainComponent::pollBnkPreview::__l2::<lambda_1>"
        "singleeditor::MainComponent::pollBnkPreview::__l2::<lambda_2>"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::pollBnkPreview'::`2'::<lambda_1>,bool,singleeditor::BnkRuntimeSession::PreviewIntent const &,bool>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::pollBnkPreview'::`2'::<lambda_2>,void,singleeditor::BnkRuntimeSession::PreviewIntent const &,singleeditor::BnkPreparedPlayback &&,bool>::_Do_call"
    TRIGGER "An admitted or queued bank preview, or presentation of an existing valid bank"
    RETENTION "Bank document and preview request with exact selection/workspace publication checks")
single_editor_declare_code_unit(
    ID singleeditor.vgmstream.controller OWNER singleeditor.vgmstream
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::beginPackedAudioLoad
        singleeditor::MainComponent::pollPackedAudioLoad
        singleeditor::MainComponent::showPackedAudioFailure
    PDB_CLASSES
        "singleeditor::MainComponent::showPackedAudioFailure::__l2::<lambda_1>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::beginPackedAudioLoad::__l2::<lambda_1>"
        "singleeditor::MainComponent::showPackedAudioFailure::__l2::<lambda_1>"
    MEMBERS "singleeditor::MainComponent::PackedAudioLoad::PackedAudioLoad"
    PDB_METHODS "std::_Func_impl_no_alloc<``singleeditor::MainComponent::showPackedAudioFailure'::`2'::<lambda_1>::operator()'::`2'::<lambda_1>,void>::_Do_call"
    TRIGGER "Identified WEM decoded fallback or explicit retry, with terminal polling only while that load exists"
    RETENTION "Progressive decoder operation and retained previous workspace until seal or failure")
single_editor_declare_code_unit(
    ID singleeditor.core-lifecycle.close-controller OWNER singleeditor.core-lifecycle
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::requestCloseApplication
        singleeditor::MainComponent::cancelCloseApplication
        singleeditor::MainComponent::finishCloseApplication
        singleeditor::MainComponent::continueCloseApplication
        singleeditor::MainComponent::hasUnconfirmedCloseChanges
    PDB_CLASSES
        "singleeditor::MainComponent::continueCloseApplication::__l2::<lambda_4>::()::__l2::<lambda_2>"
        "singleeditor::MainComponent::continueCloseApplication::__l22::<lambda_2>"
        "singleeditor::MainComponent::continueCloseApplication::__l2::<lambda_4>"
        "singleeditor::MainComponent::continueCloseApplication::__l2::<lambda_5>"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::continueCloseApplication'::`2'::<lambda_4>::operator()'::`2'::<lambda_2> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::continueCloseApplication'::`2'::<lambda_4>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::continueCloseApplication'::`2'::<lambda_5>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::continueCloseApplication'::`22'::<lambda_2>,void,bool>::_Do_call"
    TRIGGER "Explicit application close and its save/discard/cancel continuation"
    RETENTION "Close attempt, exact document approvals and optional-operation drain until completion or cancellation")
single_editor_declare_code_unit(
    ID singleeditor.recovery.controller OWNER singleeditor.recovery
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::ensureRecoveryService
        singleeditor::MainComponent::showRecoveryCenter
        singleeditor::MainComponent::retryRecovery
        singleeditor::MainComponent::showRecoveryActions
        "singleeditor::`anonymous namespace'::recoveryCapacityAdvice"
    PDB_CLASSES
        "singleeditor::MainComponent::retryRecovery::__l8::<lambda_1>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_11>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_13>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_1>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::showRecoveryActions::__l2::<lambda_2>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_5>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_10>"
        "singleeditor::MainComponent::showRecoveryCenter::__l7::<lambda_4>"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_1>::operator()'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_1>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_2>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_3>,std::optional<singleeditor::RecoveryCenterComponent::StorageSettings> >::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_4>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_5>,singleeditor::recovery::Status,singleeditor::recovery::RecoveryRestoreQueue::Request,juce::Uuid &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_6>,singleeditor::recovery::RecoveryRestoreQueue::Health>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_7>,singleeditor::recovery::Status,juce::Uuid>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_8>,singleeditor::recovery::Status,juce::Uuid,juce::Uuid &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_9>,singleeditor::recovery::Status,juce::Uuid>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_10>,singleeditor::recovery::Status,std::unique_ptr<singleeditor::recovery::OwnerDiscardReview,std::default_delete<singleeditor::recovery::OwnerDiscardReview> > &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_11>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_12>,singleeditor::recovery::Status,juce::Uuid,singleeditor::recovery::SafeHead const &>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showRecoveryCenter'::`7'::<lambda_13>,void>::_Do_call"
    MEMBERS "singleeditor::`anonymous namespace'::recoveryRootDirectory"
        "singleeditor::`anonymous namespace'::recoverySourceAdvice"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::pollRecovery'::`22'::<lambda_3>,void>::_Do_call"
    TRIGGER "Post-Core protection or discovered recovery presence, explicit Recovery Center or retry action"
    RETENTION "Recovery service, explicit recovery window and admitted owner jobs")
single_editor_declare_code_unit(
    ID singleeditor.diagnostics.controller OWNER singleeditor.diagnostics
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::showLocalDiagnostics
        singleeditor::MainComponent::exportVideoAudioDiagnostics
    PDB_CLASSES
        "singleeditor::MainComponent::showLocalDiagnostics::__l7::<lambda_1>::()::__l2::<lambda_1>"
        "singleeditor::MainComponent::showLocalDiagnostics::__l10::<lambda_2>"
        "singleeditor::MainComponent::showLocalDiagnostics::__l10::<lambda_3>"
        "singleeditor::MainComponent::showLocalDiagnostics::__l10::<lambda_4>"
        "singleeditor::MainComponent::showLocalDiagnostics::__l10::<lambda_5>"
    PDB_METHODS "`juce::MessageManager::callAsync<``singleeditor::MainComponent::showLocalDiagnostics'::`7'::<lambda_1>::operator()'::`2'::<lambda_1> >'::`2'::AsyncCallInvoker::messageCallback"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showLocalDiagnostics'::`10'::<lambda_2>,std::vector<singleeditor::diagnostics::CapturedProvider,std::allocator<singleeditor::diagnostics::CapturedProvider> > >::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showLocalDiagnostics'::`10'::<lambda_3>,bool>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showLocalDiagnostics'::`10'::<lambda_4>,std::optional<singleeditor::AudioEngine::DeviceProbeConfiguration> >::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showLocalDiagnostics'::`10'::<lambda_5>,std::shared_ptr<singleeditor::diagnostics::GuiDeviceProbe>,singleeditor::AudioEngine::DeviceProbeConfiguration const &,singleeditor::diagnostics::DeviceProbeRequest const &>::_Do_call"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::showLocalDiagnostics'::`7'::<lambda_1>,void>::_Do_call"
    TRIGGER "Explicit local diagnostics command or optional-media failure diagnostics action"
    RETENTION "Diagnostics window and selected operation diagnostic handle")
single_editor_declare_code_unit(
    ID singleeditor.core-edit-ui.post-core-command-controller OWNER singleeditor.core-edit-ui
    LIFECYCLE CoreDeferred SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::registerPostCoreCommandProvider
    TRIGGER "Empty-workspace construction or deferred chrome promotion after the current generation reaches real CoreReady"
    RETENTION "Document command descriptors and callbacks with optional views and backends remaining action-triggered")
single_editor_declare_code_unit(
    ID singleeditor.bnk.control-view OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::ensureBnkControls
        singleeditor::MainComponent::ensureBnkUiComponents
        singleeditor::MainComponent::refreshAdmittedBnkControls
    PDB_CLASSES
        "singleeditor::MainComponent::ensureBnkUiComponents::__l2::<lambda_6>"
    MEMBERS "singleeditor::MainComponent::rememberBnkControlContextForSelectedEvent"
        "singleeditor::MainComponent::syncBnkAudioSelectionForCurrentContext"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_1>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_2>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_3>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_5>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_6>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkUiComponents'::`10'::<lambda_3>,void,unsigned int>::_Do_call"
    TRIGGER "An admitted bank needs its controls or lists while ordinary and empty-workspace refresh cannot create them"
    RETENTION "One bank control group and two lists until bank runtime release or main-view teardown")
single_editor_declare_code_unit(
    ID singleeditor.bnk.control-admission OWNER singleeditor.bnk
    LIFECYCLE PostCoreWorkspace SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::refreshBnkControlSurface
    TRIGGER "Post-core or empty/error workspace refresh checks for an admitted bank"
    RETENTION "Guard and empty selection cleanup only, admitted bank control work belongs to control-view")
single_editor_declare_code_unit(
    ID singleeditor.bnk.control-storage OWNER singleeditor.bnk
    LIFECYCLE ModuleLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    SCOPES singleeditor::MainComponent::BnkControls
    TRIGGER "The guarded bank control factory allocates its private native control group"
    RETENTION "Bank-owned surface and child widgets released with bank runtime")
single_editor_declare_code_unit(
    ID singleeditor.bnk.controller OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::attachBnkProjectRoot
        singleeditor::MainComponent::prepareSelectedBnkEvent
        singleeditor::MainComponent::prepareSelectedBnkMedia
        singleeditor::MainComponent::beginBnkPreview
        singleeditor::MainComponent::queueBnkPreview
        singleeditor::MainComponent::showBnkPreviewFailure
    PDB_CLASSES
        "singleeditor::MainComponent::attachBnkProjectRoot::__l2::<lambda_1>"
        "singleeditor::MainComponent::queueBnkPreview::__l2::<lambda_1>"
        "singleeditor::MainComponent::showBnkPreviewFailure::__l2::<lambda_1>"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::attachBnkProjectRoot'::`2'::<lambda_1>,void,juce::FileChooser const &>::_Do_call"
    MEMBERS "singleeditor::MainComponent::buildBnkPlaybackContext"
        "singleeditor::MainComponent::getBnkMediaResolveMode"
    PDB_METHODS "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkControls'::`2'::<lambda_4>,void>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkUiComponents'::`10'::<lambda_4>,void,unsigned int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkUiComponents'::`10'::<lambda_5>,void,unsigned int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkUiComponents'::`7'::<lambda_1>,void,unsigned int>::_Do_call"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::ensureBnkUiComponents'::`7'::<lambda_2>,void,unsigned int>::_Do_call"
    TRIGGER "Opened bank view, explicit project-root attachment or selected bank event/media preview"
    RETENTION "Bank view controls, admitted preview request and explicit failure dialog")
single_editor_declare_code_unit(
    ID singleeditor.bnk.preview-closures OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    PDB_CLASSES "singleeditor::MainComponent::beginBnkPreview::__l2::<lambda_1>"
        "std::_Func_impl_no_alloc<`singleeditor::MainComponent::beginBnkPreview'::`2'::<lambda_1>,void>"
    TRIGGER "Admitted bank preview after the current document Save or Discard decision"
    RETENTION "Captured bank request until the explicit preview transition completes")
single_editor_declare_code_unit(
    ID singleeditor.bnk.open-controller OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponentBankOpen.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::beginBnkOpen
        singleeditor::MainComponent::showBnkOpenFailure
        singleeditor::MainComponent::exportBnkOpenDiagnostics
    TRIGGER "Identified bank open or explicit bank sidecar, retry and diagnostics action"
    RETENTION "Admitted bank open request and explicit operation dialog")
single_editor_declare_code_unit(
    ID singleeditor.windows-media.controller OWNER singleeditor.windows-media
    LIFECYCLE BackendLazy SOURCE Source/UI/MainComponent.cpp TARGETS SingleEditor
    MEMBERS singleeditor::MainComponent::beginVideoAudioLoad
        singleeditor::MainComponent::showVideoAudioFailure
    PDB_CLASSES
        "singleeditor::MainComponent::beginVideoAudioLoad::__l2::<lambda_1>"
        "singleeditor::MainComponent::showVideoAudioFailure::__l2::<lambda_1>"
    TRIGGER "Identified supported video-audio source or explicit video retry action"
    RETENTION "Admitted video operation and explicit failure dialog")

set(SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE
    Source/Document/SingleTrackDocument.cpp
)

set(SINGLE_EDITOR_AUDIO_ENGINE_SOURCE
    Source/Audio/AudioEngine.cpp
)

# These dedicated format sources include their local helpers and emitted data.
# BnkDocument has an eager empty shell and is deliberately not a whole-source
# lazy declaration. MainComponent and processor parameter TUs remain mixed too.
single_editor_declare_code_unit(
    ID singleeditor.bnk.parser OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkParser.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit bank open or identified bank fallback after ordinary reader rejection"
    RETENTION "Admitted bank worker and published bank document")
single_editor_declare_code_unit(
    ID singleeditor.bnk.sidecar OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkSidecarParser.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Admitted bank open or explicit bank sidecar refresh"
    RETENTION "Bank metadata request and published bank document")
single_editor_declare_code_unit(
    ID singleeditor.bnk.open-request OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkOpenRequest.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit bank open, identified bank fallback, or reviewed bank retry"
    RETENTION "Pending bank worker, cancellation drain and latest request result")
single_editor_declare_code_unit(
    ID singleeditor.bnk.event-resolver OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkEventResolver.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Event resolution in an opened bank document"
    RETENTION "Selected bank event graph and in-flight preview consumers")
single_editor_declare_code_unit(
    ID singleeditor.bnk.wem-playback OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkWemPlaybackBackend.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Explicit event or media preview in an opened bank document"
    RETENTION "Prepared bank preview and active transport consumer leases")
single_editor_declare_code_unit(
    ID singleeditor.bnk.runtime-session OWNER singleeditor.bnk
    LIFECYCLE BackendLazy SOURCE Source/Bnk/BnkRuntimeSession.cpp WHOLE_SOURCE TARGETS SingleEditor
    TRIGGER "Admitted bank generation or return to a retained bank selection"
    RETENTION "Generation-scoped bank/context and value-owned preview work until cancelled and fenced release")
foreach(source IN ITEMS Source/UI/Components/BnkEventListViewComponent.cpp
        Source/UI/Components/BnkAudioListViewComponent.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.bnk.view.${unit_name}" OWNER singleeditor.bnk
        LIFECYCLE BackendLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Opened bank view or return to a retained bank selection creates the bank list views"
        RETENTION "Lazily created MainComponent-owned list views and document listeners, with bank replacement clearing rows")
endforeach()
target_sources(SingleEditor
    PRIVATE
        Source/Main.cpp
        Source/Diagnostics/DiagnosticBundle.cpp
        Source/Diagnostics/DiagnosticSnapshot.cpp
        Source/Diagnostics/ApplicationDiagnosticIdentity.cpp
        Source/Diagnostics/ReproductionFiles.cpp
        Source/Diagnostics/CodecDiagnosticProbe.cpp
        Source/Diagnostics/LiveDiagnosticProviders.cpp
        Source/Diagnostics/DeviceDiagnosticProbe.cpp
        Source/Diagnostics/GuiDeviceProbe.cpp
        Source/Diagnostics/NativeDeviceMetadata.cpp
        Source/UI/Components/LocalDiagnosticsComponent.cpp
        Source/App/CoreOpenProbe.cpp
        Source/App/MainWindow.cpp
        Source/App/WindowShellResources.cpp
        Source/App/SingleEditorApplication.cpp
        Source/App/StartupFocusPolicy.cpp
        Source/App/StartupPresentationCoordinator.cpp
        Source/App/WindowsWindowShell.cpp
        Source/Core/CoreLifecycleCoordinator.cpp
        ${SINGLE_EDITOR_AMBISONIC_SOURCES}
        ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
        ${SINGLE_EDITOR_AUDIO_SUITE_SOURCES}
        ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
        ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
        Source/Bnk/BnkDocument.cpp
        Source/Bnk/BnkStateSelection.cpp
        Source/Bnk/BnkMediaLocator.cpp
        Source/Bnk/BnkSidecarParser.cpp
        Source/Bnk/BnkWemPlaybackBackend.cpp
        Source/Bnk/BnkEventResolver.cpp
        Source/Bnk/BnkParser.cpp
        Source/Bnk/BnkOpenRequest.cpp
        Source/Bnk/BnkRuntimeSession.cpp
        Source/Bnk/BnkRuntimeGraph.cpp
        Source/Document/PlaylistModel.cpp
        Source/Document/DocumentSaveCoordinator.cpp
        ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
        Source/Document/WorkspaceSessionSupport.cpp
        Source/Document/RecoveredPlaylistDocument.cpp
        Source/Metadata/TypedMetadataInspectorModel.cpp
        ${SINGLE_EDITOR_WAVEFORM_SOURCES}
        Source/UI/MainComponent.cpp
        Source/UI/MainComponentBankOpen.cpp
        Source/UI/Commands/GuiCommandModel.cpp
        Source/UI/Commands/CommandPaletteModel.cpp
        Source/UI/Commands/CommandPaletteComponent.cpp
        Source/UI/Commands/FocusNavigationModel.cpp
        Source/UI/Workspace/WorkspaceFloatingWindowModel.cpp
        Source/UI/Workspace/WorkspaceModuleHostComponent.cpp
        Source/UI/Workspace/WorkspaceModuleLayout.cpp
        Source/UI/Workspace/WorkspacePresetCatalog.cpp
        ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
        Source/UI/Renderer/RendererLookAndFeelFactory.cpp
        Source/UI/LookAndFeel/ClassicLookAndFeel.cpp
        Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
        Source/UI/Components/BnkEventListViewComponent.cpp
        Source/UI/Components/BnkAudioListViewComponent.cpp
        Source/UI/Components/CustomTitleBarComponent.cpp
        Source/UI/Components/MeterBridgeComponent.cpp
        Source/UI/Components/AmbisonicFieldPanelComponent.cpp
        Source/UI/Components/SpatialEnergyPanelComponent.cpp
        Source/UI/Components/SpatialEnergyVisualField.cpp
        Source/UI/Components/SpeakerGeometryMagComponent.cpp
        Source/UI/Components/OutputMeterPanelComponent.cpp
        Source/UI/Components/TimelineMarkersComponent.cpp
        Source/UI/Components/ParametricEqMagComponent.cpp
        Source/UI/Components/TruePeakLimiterMagComponent.cpp
        Source/UI/Components/ModernIconButton.cpp
        Source/UI/Components/TopBarComponent.cpp
    Source/UI/Components/WaveformViewComponent.cpp
    Source/UI/Components/WaveformOverviewComponent.cpp
        Source/UI/Components/ModernGlassSurface.cpp
        Source/UI/LookAndFeel/ModernGlassBackdrop.cpp
        Source/UI/Components/PlaylistSidebarComponent.cpp
        Source/UI/Components/MetadataSidebarComponent.cpp
        Source/UI/Components/TypedMetadataInspectorComponent.cpp
        Source/UI/Components/PreferencesComponent.cpp
)

if (WIN32)
    target_sources(SingleEditor PRIVATE Resources/SingleEditor.rc)
endif()

target_sources(SingleEditor PRIVATE
    ${SINGLE_EDITOR_RECOVERY_STORAGE_SOURCES}
    Source/UI/Components/RecoveryCenterComponent.cpp
    Source/Recovery/RecoveryRestoreQueue.cpp
    ${SINGLE_EDITOR_RECOVERY_RESTORE_SOURCES}
    ${SINGLE_EDITOR_RECOVERY_WRITER_SOURCES}
    ${SINGLE_EDITOR_RECOVERY_BINDING_SOURCES}
)

target_compile_definitions(SingleEditor
    PRIVATE
        SINGLE_EDITOR_ENABLE_TEST_OBSERVERS=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_REPORT_APP_USAGE=0
        SINGLE_EDITOR_FAST_WAV_OPEN=1
        $<$<BOOL:${SINGLE_EDITOR_ENABLE_OPEN_TIMING}>:SINGLE_EDITOR_ENABLE_OPEN_TIMING=1>
        $<$<NOT:$<BOOL:${SINGLE_EDITOR_ENABLE_OPEN_TIMING}>>:SINGLE_EDITOR_ENABLE_OPEN_TIMING=0>
)

target_link_libraries(SingleEditor
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        "$<$<NOT:$<BOOL:${SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE}>>:juce::juce_recommended_lto_flags>"
        juce::juce_recommended_warning_flags
)

add_executable(SingleEditorLogicTests
    Tests/ChannelRoutingTests.cpp
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
    Source/UI/Workspace/WorkspaceModuleLayout.cpp
)

target_include_directories(SingleEditorLogicTests
    PRIVATE
        Source
)
target_compile_definitions(SingleEditorLogicTests PRIVATE SINGLE_EDITOR_AMBISONIC_RESOURCE_TESTING=1)

target_link_libraries(SingleEditorLogicTests
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_executable(SingleEditorDocumentTests
    Tests/DocumentSaveTests.cpp
    Tests/PackedMediaLifecycleTests.cpp
    Tests/BankDecodeOperationTests.cpp
    Tests/BankOpenRequestTests.cpp
    Tests/BankParserHardeningTests.cpp
    Tests/BnkSidecarSemanticsTests.cpp
    Tests/BnkSidecarTargetTests.cpp
    Tests/BnkStateGroupTests.cpp
    Tests/BnkRtpcIdentityTests.cpp
    Tests/BnkMediaLocatorTests.cpp
    Tests/BnkDerivedPathTests.cpp
    Tests/BankParserCorpusTests.cpp
    Tests/RecoveryAudioGraphTests.cpp
    Tests/ProgressiveRecoveryAudioTests.cpp
    Tests/RecoveryCheckpointCodecTests.cpp
    Tests/RecoveredDocumentTests.cpp
    Tests/ProgressiveRecoveredDocumentTests.cpp
    Tests/ProgressiveRecoveryWaveformTests.cpp
    Tests/RecoveryRestoreCandidateTests.cpp
    Tests/RecoveryRestoreQueueTests.cpp
    Tests/RecoverySourceBindingCheckpointTests.cpp
    Tests/RecoverySourceFreezeTests.cpp
    Tests/RecoveryMutationProducerTests.cpp
    Tests/RecoveryOwnerLifecycleTests.cpp
    Tests/RecoverySessionHostTests.cpp
    Tests/RecoveryServiceTests.cpp
    Tests/RecoverySavedCancellationTests.cpp
    Tests/RecoveryTerminalQuarantineTests.cpp
    Tests/WorkspaceSnapshotTests.cpp
    Tests/RecoveryGraphReuseTests.cpp
    Tests/RecoveryCanonicalMutationTests.cpp
    ${SINGLE_EDITOR_WAVEFORM_SOURCES}
    Source/Bnk/BnkParser.cpp
    Source/Bnk/BnkOpenRequest.cpp
    Source/Bnk/BnkRuntimeSession.cpp
    Source/Bnk/BnkSidecarParser.cpp
    Source/Bnk/BnkEventResolver.cpp
    Source/Bnk/BnkRuntimeGraph.cpp
    Source/Bnk/BnkDocument.cpp
    Source/Bnk/BnkStateSelection.cpp
    Source/Bnk/BnkMediaLocator.cpp
    Source/Bnk/BnkWemPlaybackBackend.cpp
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    Source/Document/DocumentSaveCoordinator.cpp
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
    ${SINGLE_EDITOR_RECOVERY_STORAGE_SOURCES}
    Source/Recovery/RecoveryRestoreQueue.cpp
    ${SINGLE_EDITOR_RECOVERY_WRITER_SOURCES}
    ${SINGLE_EDITOR_RECOVERY_RESTORE_SOURCES}
    ${SINGLE_EDITOR_RECOVERY_BINDING_SOURCES}
)

target_include_directories(SingleEditorDocumentTests
    PRIVATE
        Source
)
target_sources(SingleEditorDocumentTests PRIVATE Tests/BnkRuntimeSessionTests.cpp)
target_compile_definitions(SingleEditorDocumentTests PRIVATE SINGLE_EDITOR_BNK_SESSION_TESTING=1)

if (SINGLE_EDITOR_DOCUMENT_TEST_ASAN)
    if (NOT MSVC)
        message(FATAL_ERROR "SINGLE_EDITOR_DOCUMENT_TEST_ASAN currently supports MSVC only")
    endif()
    target_compile_options(SingleEditorDocumentTests PRIVATE /fsanitize=address /Zi)
    target_link_options(SingleEditorDocumentTests PRIVATE /INCREMENTAL:NO /DEBUG)
endif()

target_sources(SingleEditorDocumentTests PRIVATE
    Source/Document/PlaylistModel.cpp
    Source/Document/WorkspaceSessionSupport.cpp
    Source/Document/RecoveredPlaylistDocument.cpp
)

target_compile_definitions(SingleEditorDocumentTests
    PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1
        SINGLE_EDITOR_RECOVERY_TESTING=1
        SINGLE_EDITOR_AUDIO_RESOURCE_TESTING=1
)

target_link_libraries(SingleEditorDocumentTests
    PRIVATE
        juce::juce_gui_extra
        juce::juce_graphics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_events
        juce::juce_core
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

add_executable(SingleEditorAudioSuiteReferenceTests
    Tests/AudioSuiteReferenceTests.cpp
    Tests/WaveformEditCacheTests.cpp
    Tests/AudioDeviceWarmupTests.cpp
    ${SINGLE_EDITOR_WAVEFORM_SOURCES}
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
)

target_include_directories(SingleEditorAudioSuiteReferenceTests PRIVATE Source)
target_compile_definitions(SingleEditorAudioSuiteReferenceTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(SingleEditorAudioSuiteReferenceTests PRIVATE
    juce::juce_graphics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_core
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorParametricEqTests
    Tests/ParametricEqTests.cpp
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    Source/AudioSuite/ParametricEqProcessor.cpp
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
    Source/UI/Components/ParametricEqMagComponent.cpp
    Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
)

target_include_directories(SingleEditorParametricEqTests PRIVATE Source)
target_link_libraries(SingleEditorParametricEqTests PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_gui_extra
    juce::juce_core
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorTruePeakLimiterTests
    Tests/TruePeakLimiterTests.cpp
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    Source/AudioSuite/TruePeakLimiterProcessor.cpp
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
    Source/UI/Components/TruePeakLimiterMagComponent.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
)

target_include_directories(SingleEditorTruePeakLimiterTests PRIVATE Source)
target_link_libraries(SingleEditorTruePeakLimiterTests PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_gui_basics
    juce::juce_core
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorSpeakerGeometryMagTests
    Tests/SpeakerGeometryMagTests.cpp
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_REFERENCE_PROCESSOR_SOURCE}
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
    Source/UI/Components/AmbisonicFieldPanelComponent.cpp
    Source/UI/Components/SpeakerGeometryMagComponent.cpp
    Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
)

target_include_directories(SingleEditorSpeakerGeometryMagTests PRIVATE Source)
target_link_libraries(SingleEditorSpeakerGeometryMagTests PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_events
    juce::juce_gui_extra
    juce::juce_core
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorStartupFocusTests
    Tests/StartupFocusTests.cpp
    Source/App/StartupFocusPolicy.cpp
)

target_include_directories(SingleEditorStartupFocusTests PRIVATE Source)
target_link_libraries(SingleEditorStartupFocusTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorStartupPresentationTests
    Tests/StartupPresentationTests.cpp
    Source/App/StartupPresentationCoordinator.cpp
)

target_include_directories(SingleEditorStartupPresentationTests PRIVATE Source)
target_link_libraries(SingleEditorStartupPresentationTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorWorkspaceGridTests
    Tests/WorkspaceGridTests.cpp
    Source/UI/Workspace/WorkspaceModuleLayout.cpp
    Source/UI/Workspace/WorkspacePresetCatalog.cpp
)

target_include_directories(SingleEditorWorkspaceGridTests PRIVATE Source)
target_link_libraries(SingleEditorWorkspaceGridTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorCommandAccessibilityTests
    Tests/CommandAccessibilityTests.cpp
    Source/UI/Commands/GuiCommandModel.cpp
    Source/UI/Commands/CommandPaletteModel.cpp
    Source/UI/Commands/FocusNavigationModel.cpp
)

target_include_directories(SingleEditorCommandAccessibilityTests PRIVATE Source)
target_link_libraries(SingleEditorCommandAccessibilityTests PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorWorkspaceModuleHostTests
    Tests/WorkspaceModuleHostTests.cpp
    Source/App/WindowsWindowShell.cpp
    Source/UI/Workspace/WorkspaceModuleHostComponent.cpp
    Source/UI/Workspace/WorkspaceModuleLayout.cpp
    Source/UI/Workspace/WorkspaceFloatingWindowModel.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
)

target_include_directories(SingleEditorWorkspaceModuleHostTests PRIVATE Source)
target_link_libraries(SingleEditorWorkspaceModuleHostTests PRIVATE
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorWorkspaceFloatingWindowTests
    Tests/WorkspaceFloatingWindowTests.cpp
    Source/UI/Workspace/WorkspaceFloatingWindowModel.cpp
    Source/UI/Workspace/WorkspaceModuleLayout.cpp
)

target_include_directories(SingleEditorWorkspaceFloatingWindowTests PRIVATE Source)
target_link_libraries(SingleEditorWorkspaceFloatingWindowTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorModernVisualSystemTests
    Tests/RuntimePlaybackProbeTests.cpp
    Tests/NativeInputProbe.cpp
    Tests/ModernLimiterPresentationTests.cpp
    Source/UI/Components/TruePeakLimiterMagComponent.cpp
    Tests/TimelineMarkersTests.cpp
    Source/UI/Components/TimelineMarkersComponent.cpp
    Tests/ModernCommandPaletteTests.cpp
    Source/UI/Commands/CommandPaletteComponent.cpp
    Source/UI/Commands/CommandPaletteModel.cpp
    Source/UI/Commands/GuiCommandModel.cpp
    Tests/ModernOutputMeterTests.cpp
    Source/Audio/ChannelLayout.cpp
    Tests/WindowChromeTests.cpp
    Source/App/WindowsWindowShell.cpp
    Tests/WaveformPeakLevelTests.cpp
    Tests/WaveformOverviewTests.cpp
    Tests/WaveformPlotGeometryTests.cpp
    Source/UI/Components/WaveformOverviewComponent.cpp
    Source/Waveform/WaveformPeakLevel.cpp
    Tests/ModernGeneralPanelTests.cpp
    Tests/ModernGlassTests.cpp
    Tests/ModernVisualSystemTests.cpp
    Source/UI/Components/ModernGlassSurface.cpp
    Source/UI/LookAndFeel/ModernGlassBackdrop.cpp
    Source/UI/Components/ModernIconButton.cpp
    Source/UI/Components/PlaylistSidebarComponent.cpp
    Source/Document/PlaylistModel.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
    Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
)

target_include_directories(SingleEditorModernVisualSystemTests PRIVATE Source)
target_compile_definitions(SingleEditorModernVisualSystemTests PRIVATE SINGLE_EDITOR_ENABLE_TEST_OBSERVERS=1)
target_link_libraries(SingleEditorModernVisualSystemTests PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_formats
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorClassicRendererTests
    Tests/ClassicRendererTests.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
    Source/UI/Renderer/RendererLookAndFeelFactory.cpp
    Source/UI/LookAndFeel/ClassicLookAndFeel.cpp
    Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
)

target_include_directories(SingleEditorClassicRendererTests PRIVATE Source)
target_link_libraries(SingleEditorClassicRendererTests PRIVATE
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorCoreLifecycleTests
    Tests/CoreLifecycleTests.cpp
    Source/Core/CoreLifecycleCoordinator.cpp
)

if(WIN32)
    add_executable(SingleEditorCoreOpenCycleTests EXCLUDE_FROM_ALL
        Tests/CoreOpenCycleTests.cpp Source/App/CoreOpenProbe.cpp)
    target_include_directories(SingleEditorCoreOpenCycleTests PRIVATE Source)
    target_link_libraries(SingleEditorCoreOpenCycleTests PRIVATE juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

add_executable(SingleEditorTypedMetadataInspectorTests
    Tests/TypedMetadataInspectorTests.cpp
    Source/Metadata/TypedMetadataInspectorModel.cpp
    Source/UI/Components/TypedMetadataInspectorComponent.cpp
    Source/UI/LookAndFeel/ModernAppleLookAndFeel.cpp
    ${SINGLE_EDITOR_RENDERER_STYLE_SOURCES}
)

target_include_directories(SingleEditorTypedMetadataInspectorTests PRIVATE Source)
target_link_libraries(SingleEditorTypedMetadataInspectorTests PRIVATE
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

target_include_directories(SingleEditorCoreLifecycleTests PRIVATE Source)
target_link_libraries(SingleEditorCoreLifecycleTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorCliTests EXCLUDE_FROM_ALL
    Tests/CliAutomationTests.cpp
    ${SINGLE_EDITOR_AUTOMATION_PLAN_SOURCES}
    Source/Automation/OperationRegistry.cpp
    ${SINGLE_EDITOR_CLI_APPLICATION_SOURCES}
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_AUDIO_SUITE_SOURCES}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
)

target_include_directories(SingleEditorCliTests PRIVATE Source)
target_compile_definitions(SingleEditorCliTests PRIVATE
    SINGLE_EDITOR_CLI_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(SingleEditorCliTests PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_cryptography
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

juce_add_console_app(SingleEditorCLI
    PRODUCT_NAME "SingleEditorCLI"
    COMPANY_NAME "SingleEditor"
)
set_target_properties(SingleEditorCLI PROPERTIES EXCLUDE_FROM_ALL TRUE)

target_sources(SingleEditorCLI PRIVATE
    Source/CLI/SingleEditorCliMain.cpp
    ${SINGLE_EDITOR_CLI_APPLICATION_SOURCES}
    Source/Automation/OperationRegistry.cpp
    ${SINGLE_EDITOR_AUTOMATION_PLAN_SOURCES}
    ${SINGLE_EDITOR_AMBISONIC_SOURCES}
    ${SINGLE_EDITOR_AUDIO_ENGINE_SOURCE}
    ${SINGLE_EDITOR_AUDIO_SUITE_SOURCES}
    ${SINGLE_EDITOR_CHANNEL_ROUTING_SOURCES}
    ${SINGLE_EDITOR_PACKED_DECODE_SOURCES}
    ${SINGLE_EDITOR_DOCUMENT_AUDIO_SOURCE}
)

target_include_directories(SingleEditorCLI PRIVATE Source)
target_compile_definitions(SingleEditorCLI PRIVATE
    SINGLE_EDITOR_ENABLE_TEST_OBSERVERS=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_REPORT_APP_USAGE=0
    SINGLE_EDITOR_FAST_WAV_OPEN=1
    SINGLE_EDITOR_ENABLE_OPEN_TIMING=0
    SINGLE_EDITOR_CLI_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(SingleEditorCLI PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_events
    juce::juce_core
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    "$<$<NOT:$<BOOL:${SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE}>>:juce::juce_recommended_lto_flags>"
    juce::juce_recommended_warning_flags
)

# Recovery's storage pilot is ToolTestOnly until its async document integration
# and complete recovery acceptance exist. It adds no product startup/link path.
add_executable(SingleEditorRecoveryStoreTests EXCLUDE_FROM_ALL
    Tests/RecoveryStoreTests.cpp
    Source/Recovery/RecoveryStore.cpp
    Source/Recovery/AsyncRecoveryWriter.cpp
)
target_include_directories(SingleEditorRecoveryStoreTests PRIVATE Source)
target_compile_definitions(SingleEditorRecoveryStoreTests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SINGLE_EDITOR_RECOVERY_TESTING=1
)
target_link_libraries(SingleEditorRecoveryStoreTests PRIVATE
    juce::juce_core
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

add_executable(SingleEditorRecoveryAttemptStoreTests EXCLUDE_FROM_ALL
    Tests/RecoveryAttemptStoreTests.cpp Source/Recovery/RecoveryAttemptStore.cpp Source/Recovery/RecoveryStore.cpp)
target_include_directories(SingleEditorRecoveryAttemptStoreTests PRIVATE Source)
target_compile_definitions(SingleEditorRecoveryAttemptStoreTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorRecoveryAttemptStoreTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

add_executable(SingleEditorRecoverySourceIdentityTests EXCLUDE_FROM_ALL
    Tests/RecoverySourceIdentityTests.cpp Source/Recovery/RecoverySourceIdentity.cpp)
target_include_directories(SingleEditorRecoverySourceIdentityTests PRIVATE Source)
target_compile_definitions(SingleEditorRecoverySourceIdentityTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorRecoverySourceIdentityTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

add_executable(SingleEditorBoundedDecodeProcessTests EXCLUDE_FROM_ALL
    Tests/BoundedDecodeProcessTests.cpp
    Source/Audio/BoundedDecodeProcess.cpp
)
target_include_directories(SingleEditorBoundedDecodeProcessTests PRIVATE Source)
target_compile_definitions(SingleEditorBoundedDecodeProcessTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorBoundedDecodeProcessTests PRIVATE
    juce::juce_core juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Shared optional-media boundary; none of these targets constructs it at startup.
add_executable(SingleEditorDecodedWaveCacheTests EXCLUDE_FROM_ALL
    Tests/DecodedWaveCacheTests.cpp Source/Audio/DecodedWaveCache.cpp)
target_include_directories(SingleEditorDecodedWaveCacheTests PRIVATE Source)
target_compile_definitions(SingleEditorDecodedWaveCacheTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorDecodedWaveCacheTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# State/operation contracts; native video also uses the same session owner.
add_executable(SingleEditorOptionalMediaFailureStateTests EXCLUDE_FROM_ALL
    Tests/OptionalMediaFailureStateTests.cpp Source/Audio/OptionalMediaFailureState.cpp
    Source/Audio/OptionalMediaOperation.cpp Source/Audio/BoundedDecodeProcess.cpp)
target_include_directories(SingleEditorOptionalMediaFailureStateTests PRIVATE Source)
target_compile_definitions(SingleEditorOptionalMediaFailureStateTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorOptionalMediaFailureStateTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Local diagnostics backend remains ToolTestOnly until explicit consent/providers are wired.
add_executable(SingleEditorDiagnosticBundleTests EXCLUDE_FROM_ALL
    Tests/DiagnosticBundleTests.cpp Source/Diagnostics/DiagnosticBundle.cpp)
target_include_directories(SingleEditorDiagnosticBundleTests PRIVATE Source)
target_compile_definitions(SingleEditorDiagnosticBundleTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 SINGLE_EDITOR_DIAGNOSTICS_TESTING=1)
target_link_libraries(SingleEditorDiagnosticBundleTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

add_executable(SingleEditorDiagnosticSnapshotTests EXCLUDE_FROM_ALL
    Tests/DiagnosticSnapshotTests.cpp Source/Diagnostics/DiagnosticSnapshot.cpp Source/Diagnostics/DiagnosticBundle.cpp
    Source/Diagnostics/ReproductionFiles.cpp)
target_include_directories(SingleEditorDiagnosticSnapshotTests PRIVATE Source)
target_compile_definitions(SingleEditorDiagnosticSnapshotTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SingleEditorDiagnosticSnapshotTests PRIVATE juce::juce_core juce::juce_cryptography
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(WIN32)
    target_link_libraries(SingleEditorDiagnosticSnapshotTests PRIVATE bcrypt)
endif()

# Explicit standalone diagnostics reuse the GUI snapshot and publication core.
target_sources(SingleEditorCliTests PRIVATE Tests/DeviceDiagnosticProbeTests.cpp)
foreach(diagnostic_cli_target SingleEditorCLI SingleEditorCliTests)
    target_sources(${diagnostic_cli_target} PRIVATE Source/CLI/CliDiagnostics.cpp Source/Diagnostics/DiagnosticBundle.cpp
        Source/Diagnostics/DiagnosticSnapshot.cpp Source/Diagnostics/ApplicationDiagnosticIdentity.cpp
        Source/Diagnostics/CodecDiagnosticProbe.cpp Source/Diagnostics/DeviceDiagnosticProbe.cpp Source/Diagnostics/ReproductionFiles.cpp)
    target_include_directories(${diagnostic_cli_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    if(WIN32)
        target_link_libraries(${diagnostic_cli_target} PRIVATE bcrypt)
    endif()
endforeach()

foreach(decoder_target SingleEditor SingleEditorLogicTests SingleEditorDocumentTests
        SingleEditorAudioSuiteReferenceTests SingleEditorParametricEqTests
        SingleEditorTruePeakLimiterTests SingleEditorSpeakerGeometryMagTests
        SingleEditorCliTests SingleEditorCLI)
    target_sources(${decoder_target} PRIVATE Source/Audio/BoundedDecodeProcess.cpp Source/Audio/DecodedWaveCache.cpp)
    target_sources(${decoder_target} PRIVATE Source/Audio/OptionalMediaFailureState.cpp Source/Audio/OptionalMediaOperation.cpp)
    target_sources(${decoder_target} PRIVATE Source/Audio/SourceFileLease.cpp)
endforeach()

single_editor_copy_ambisonic_resources(SingleEditor)
single_editor_copy_window_shell_resources(SingleEditor)
single_editor_copy_ambisonic_resources(SingleEditorLogicTests)
single_editor_copy_ambisonic_resources(SingleEditorDocumentTests)
single_editor_copy_ambisonic_resources(SingleEditorCliTests)
single_editor_copy_ambisonic_resources(SingleEditorCLI)

foreach(route_target SingleEditor SingleEditorDocumentTests SingleEditorAudioSuiteReferenceTests
        SingleEditorParametricEqTests SingleEditorTruePeakLimiterTests SingleEditorSpeakerGeometryMagTests
        SingleEditorCliTests SingleEditorCLI)
    target_sources(${route_target} PRIVATE Source/Audio/DeviceRoutePlan.cpp Source/Audio/DeviceSwitchCoordinator.cpp)
    target_sources(${route_target} PRIVATE Source/Audio/PlaybackRateSource.cpp)
    target_sources(${route_target} PRIVATE Source/Audio/SpatialEnergyMeter.cpp Source/Audio/SpatialEnergyLayout.cpp
        Source/Audio/SpatialEnergyService.cpp)
    single_editor_link_samplerate(${route_target})
endforeach()
target_sources(SingleEditor PRIVATE Source/Audio/JuceAsioBackendAdapter.cpp
    Source/Audio/AsioSettingsSession.cpp Source/UI/Components/AsioSettingsComponent.cpp
    Source/Audio/WindowsOutputBackend.cpp)
foreach(source IN ITEMS Source/Audio/DeviceRoutePlan.cpp Source/Audio/DeviceSwitchCoordinator.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.audio-output.transaction.${unit_name}" OWNER singleeditor.audio-output
        LIFECYCLE BackendLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor SingleEditorCLI
        TRIGGER "Selected output route validation, activation, inspection or explicit backend maintenance"
        RETENTION "Approved route and admitted device operation until completion, cancellation or output retirement")
endforeach()
foreach(source IN ITEMS Source/Audio/JuceAsioBackendAdapter.cpp Source/Audio/WindowsOutputBackend.cpp)
    get_filename_component(unit_name "${source}" NAME_WE)
    string(TOLOWER "${unit_name}" unit_name)
    single_editor_declare_code_unit(
        ID "singleeditor.audio-output.adapter.${unit_name}" OWNER singleeditor.audio-output
        LIFECYCLE BackendLazy SOURCE "${source}" WHOLE_SOURCE TARGETS SingleEditor
        TRIGGER "Explicit backend settings or remembered user-selected output restoration"
        RETENTION "Request-owned discovery and capability objects, then active device output lease")
endforeach()
single_editor_declare_code_unit(
    ID singleeditor.audio-output.session-lifetime OWNER singleeditor.audio-output
    LIFECYCLE BackendLazy SOURCE Source/Audio/AsioSettingsSession.cpp TARGETS SingleEditor
    MEMBERS singleeditor::AsioSettingsSession::AsioSettingsSession singleeditor::AsioSettingsSession::~AsioSettingsSession
        "singleeditor::`anonymous namespace'::busFor" "singleeditor::`anonymous namespace'::failureText"
    SCOPES "singleeditor::`anonymous-namespace'::PhysicalPing"
    TRIGGER "Explicit output settings or saved-selection restoration, with ping only on explicit user action"
    RETENTION "Output settings session and request-owned physical ping callback until completion or cancellation")
single_editor_enable_asio(SingleEditor)

add_executable(SingleEditorDeviceRouteTests EXCLUDE_FROM_ALL Tests/DeviceRoutePlanTests.cpp Source/Audio/DeviceRoutePlan.cpp)
target_include_directories(SingleEditorDeviceRouteTests PRIVATE Source)
target_compile_definitions(SingleEditorDeviceRouteTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
target_link_libraries(SingleEditorDeviceRouteTests PRIVATE juce::juce_core
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

add_executable(SingleEditorPlaybackRateTests EXCLUDE_FROM_ALL Tests/PlaybackRateTests.cpp Source/Audio/PlaybackRateSource.cpp)
target_include_directories(SingleEditorPlaybackRateTests PRIVATE Source)
target_compile_definitions(SingleEditorPlaybackRateTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
target_link_libraries(SingleEditorPlaybackRateTests PRIVATE juce::juce_audio_basics
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
single_editor_link_samplerate(SingleEditorPlaybackRateTests)

add_executable(SingleEditorSpatialEnergyTests EXCLUDE_FROM_ALL
    Tests/SpatialEnergyMeterTests.cpp Source/Audio/SpatialEnergyMeter.cpp Source/Audio/SpatialEnergyLayout.cpp Source/Audio/DeviceRoutePlan.cpp)
target_include_directories(SingleEditorSpatialEnergyTests PRIVATE Source)
target_compile_definitions(SingleEditorSpatialEnergyTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
target_link_libraries(SingleEditorSpatialEnergyTests PRIVATE juce::juce_audio_basics
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

if(WIN32)
    foreach(asio_test SingleEditorAsioIntegrationTests SingleEditorAsioIsolationTests)
        add_executable(${asio_test} EXCLUDE_FROM_ALL Tests/AsioBackendTests.cpp Source/Audio/JuceAsioBackendAdapter.cpp)
        target_include_directories(${asio_test} PRIVATE Source)
        target_compile_definitions(${asio_test} PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0)
        target_link_libraries(${asio_test} PRIVATE juce::juce_audio_devices
            juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    endforeach()
    single_editor_enable_asio(SingleEditorAsioIntegrationTests)
    target_compile_definitions(SingleEditorAsioIsolationTests PRIVATE JUCE_ASIO=0)
    add_executable(SingleEditorAsioDriverTests EXCLUDE_FROM_ALL Tests/AsioDriverTests.cpp Source/Audio/JuceAsioBackendAdapter.cpp)
    target_include_directories(SingleEditorAsioDriverTests PRIVATE Source)
    target_compile_definitions(SingleEditorAsioDriverTests PRIVATE SINGLE_EDITOR_ASIO_DRIVER_FIXTURE=1
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(SingleEditorAsioDriverTests PRIVATE juce::juce_audio_devices
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    single_editor_enable_asio(SingleEditorAsioDriverTests)
endif()

# Snapshot the complete production source list after shared source additions.
# This target changes only the Application/fixtures; it uses the native window,
# controls, Save and recovery implementation and stays out of default builds.
# Fixture isolation is Windows-specific (process-local LOCALAPPDATA).
if(WIN32)
get_target_property(native_recovery_test_sources SingleEditor SOURCES)
list(REMOVE_ITEM native_recovery_test_sources Source/Main.cpp Source/App/SingleEditorApplication.cpp Resources/SingleEditor.rc)
juce_add_console_app(SingleEditorNativeRecoveryTests PRODUCT_NAME "Single Editor Recovery Test")
set_target_properties(SingleEditorNativeRecoveryTests PROPERTIES EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/NativeRecoveryTests.cpp Tests/WaveformPresentationTests.cpp Tests/PackedMediaLifecycleTests.cpp Tests/AudioDeviceDiagnosticsTests.cpp Tests/NativeDeviceMetadataTests.cpp ${native_recovery_test_sources})
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/DeferredSettingsPersistenceTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/OutputMeterConsumerTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/DeferredWorkspaceControlsTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/DeferredCacheWarmTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/OptionalLifecycleTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/MagLifecycleTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/BackendLifecycleTests.cpp)
target_sources(SingleEditorNativeRecoveryTests PRIVATE Tests/LifecycleViolationTests.cpp)
target_include_directories(SingleEditorNativeRecoveryTests PRIVATE Source "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(SingleEditorNativeRecoveryTests PRIVATE
    SINGLE_EDITOR_ENABLE_TEST_OBSERVERS=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_REPORT_APP_USAGE=0 SINGLE_EDITOR_FAST_WAV_OPEN=1
    SINGLE_EDITOR_ENABLE_OPEN_TIMING=0)
target_compile_definitions(SingleEditorNativeRecoveryTests PRIVATE SINGLE_EDITOR_DIAGNOSTIC_NATIVE_TEST=1)
target_link_libraries(SingleEditorNativeRecoveryTests PRIVATE bcrypt)
single_editor_link_samplerate(SingleEditorNativeRecoveryTests)
target_link_libraries(SingleEditor PRIVATE bcrypt)
target_link_libraries(SingleEditorNativeRecoveryTests PRIVATE
    juce::juce_audio_devices juce::juce_audio_formats juce::juce_cryptography juce::juce_dsp juce::juce_gui_extra
    juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
single_editor_copy_ambisonic_resources(SingleEditorNativeRecoveryTests)
single_editor_copy_window_shell_resources(SingleEditorNativeRecoveryTests)
endif()

# M3-29: stable PE identity for identical Release inputs. Keep this separate
# from source-membership refactors; do not rewrite timestamps after linking.
include(cmake/SingleEditorReproducibleRelease.cmake)
single_editor_configure_reproducible_release()
include(cmake/SingleEditorLinkEvidence.cmake)
single_editor_configure_link_evidence()

# Build-side resource ownership only; no runtime catalog or resource loading.
single_editor_link_core_action_icons()
single_editor_bind_ambisonic_resources()
include(cmake/SingleEditorResourceIntent.cmake)
single_editor_write_resource_intent()
# Build-side typed PE carrier intent; no target link flags or runtime loading.
if(WIN32)
    set(PE_CARRIER_ARCHITECTURE "${CMAKE_GENERATOR_PLATFORM}")
    if(PE_CARRIER_ARCHITECTURE STREQUAL "")
        set(PE_CARRIER_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    configure_file(cmake/PeCarrierIntent.json.in pe-carrier-intent/configured.json @ONLY)
    file(READ "${CMAKE_CURRENT_BINARY_DIR}/pe-carrier-intent/configured.json" pe_carrier_intent)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pe-carrier-intent/$<CONFIG>.json"
        CONTENT "${pe_carrier_intent}")
endif()
# M3-29: remaining exact source responsibilities. Build-side declarations only.

get_target_property(m3_remaining_source_1_sources juce_audio_basics INTERFACE_SOURCES)
set(m3_remaining_source_1_matches)
foreach(m3_remaining_source_1_candidate IN LISTS m3_remaining_source_1_sources)
    if(m3_remaining_source_1_candidate MATCHES "/juce_audio_basics\\.cpp$")
        list(APPEND m3_remaining_source_1_matches "${m3_remaining_source_1_candidate}")
    endif()
endforeach()
list(LENGTH m3_remaining_source_1_matches m3_remaining_source_1_count)
if(NOT m3_remaining_source_1_count EQUAL 1)
    message(FATAL_ERROR "Expected one actual juce_audio_basics source for remaining responsibility declarations")
endif()
list(GET m3_remaining_source_1_matches 0 m3_remaining_source_1_absolute)
single_editor_code_source_coordinates("${m3_remaining_source_1_absolute}" m3_remaining_source_1_source m3_remaining_source_1_base)

get_target_property(m3_remaining_source_2_sources juce_dsp INTERFACE_SOURCES)
set(m3_remaining_source_2_matches)
foreach(m3_remaining_source_2_candidate IN LISTS m3_remaining_source_2_sources)
    if(m3_remaining_source_2_candidate MATCHES "/juce_dsp\\.cpp$")
        list(APPEND m3_remaining_source_2_matches "${m3_remaining_source_2_candidate}")
    endif()
endforeach()
list(LENGTH m3_remaining_source_2_matches m3_remaining_source_2_count)
if(NOT m3_remaining_source_2_count EQUAL 1)
    message(FATAL_ERROR "Expected one actual juce_dsp source for remaining responsibility declarations")
endif()
list(GET m3_remaining_source_2_matches 0 m3_remaining_source_2_absolute)
single_editor_code_source_coordinates("${m3_remaining_source_2_absolute}" m3_remaining_source_2_source m3_remaining_source_2_base)

get_target_property(m3_remaining_source_3_sources juce_gui_extra INTERFACE_SOURCES)
set(m3_remaining_source_3_matches)
foreach(m3_remaining_source_3_candidate IN LISTS m3_remaining_source_3_sources)
    if(m3_remaining_source_3_candidate MATCHES "/juce_gui_extra\\.cpp$")
        list(APPEND m3_remaining_source_3_matches "${m3_remaining_source_3_candidate}")
    endif()
endforeach()
list(LENGTH m3_remaining_source_3_matches m3_remaining_source_3_count)
if(NOT m3_remaining_source_3_count EQUAL 1)
    message(FATAL_ERROR "Expected one actual juce_gui_extra source for remaining responsibility declarations")
endif()
list(GET m3_remaining_source_3_matches 0 m3_remaining_source_3_absolute)
single_editor_code_source_coordinates("${m3_remaining_source_3_absolute}" m3_remaining_source_3_source m3_remaining_source_3_base)

get_target_property(m3_remaining_source_4_sources juce_events INTERFACE_SOURCES)
set(m3_remaining_source_4_matches)
foreach(m3_remaining_source_4_candidate IN LISTS m3_remaining_source_4_sources)
    if(m3_remaining_source_4_candidate MATCHES "/juce_events\\.cpp$")
        list(APPEND m3_remaining_source_4_matches "${m3_remaining_source_4_candidate}")
    endif()
endforeach()
list(LENGTH m3_remaining_source_4_matches m3_remaining_source_4_count)
if(NOT m3_remaining_source_4_count EQUAL 1)
    message(FATAL_ERROR "Expected one actual juce_events source for remaining responsibility declarations")
endif()
list(GET m3_remaining_source_4_matches 0 m3_remaining_source_4_absolute)
single_editor_code_source_coordinates("${m3_remaining_source_4_absolute}" m3_remaining_source_4_source m3_remaining_source_4_base)

single_editor_declare_code_unit(
    ID "singleeditor.core-lifecycle.release-handshake"
    OWNER "singleeditor.core-lifecycle"
    LIFECYCLE "CoreEager"
    SOURCE "Source/App/CoreOpenProbe.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    WHOLE_SOURCE
    TRIGGER "D147 Release-safe external mapping admission at application initialise and actual ordinary window, filename, waveform and playable milestones"
    RETENTION "Only a harness-created mapping is attached. Handle and view lifetime ends at detach or failed validation, with no timing file writer or product control IPC")

single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${intent_definition_modules_root}/juce_graphics/geometry/juce_Rectangle.h"
    ID "singleeditor.window-shell.native-window-calls"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreEager"
    SOURCE "Source/App/WindowsWindowShell.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::WindowsWindowShell::configureWindow"
        "singleeditor::WindowsWindowShell::findControlAtPoint"
        "singleeditor::WindowsWindowShell::usesNativeFrame"
        "singleeditor::WindowsWindowShell::providesWindowTitleBar"
    PDB_METHODS
        "juce::Rectangle<float>::contains"
    TRIGGER "Initial MainWindow native frame configuration and native automatic hit testing, also reused by later floating windows"
    RETENTION "Window-local geometry and mandatory native frame controls without creating optional view or backend work")

single_editor_declare_code_unit(
    ID "singleeditor.window-shell.native-simple-presentation"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreDeferred"
    SOURCE "Source/App/WindowsWindowShell.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::WindowsWindowShell::setSimplePresentation"
    TRIGGER "Explicit General or Simple command or Return to General invokes the installed onSimpleViewChanged callback"
    RETENTION "Existing native buttons and style bits remain owned by the same window while caption visibility changes")

single_editor_declare_code_unit(
    ID "singleeditor.core-edit-ui.peak-level-selection"
    OWNER "singleeditor.core-edit-ui"
    LIFECYCLE "CoreEager"
    SOURCE "Source/Waveform/WaveformPeakLevel.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    WHOLE_SOURCE
    TRIGGER "Actual ordinary waveform paint selects the existing peak LOD before publishing first-waveform readiness"
    RETENTION "Borrowed immutable peak-cache level pointers for one paint, without cache construction, spectral work or allocation")

single_editor_declare_code_unit(
    ID "singleeditor.window-shell.renderer-selection"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreEager"
    SOURCE "Source/UI/Renderer/RendererSelection.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::RendererSelection::select"
        "singleeditor::RendererSelection::seal"
        "singleeditor::RendererSelection::selected"
        "singleeditor::RendererSelection::parse"
        "singleeditor::RendererSelection::noteRendererObject"
        "singleeditor::RendererSelection::noteTokenSliceAccess"
        "singleeditor::RendererSelection::noteAssetLookup"
        "singleeditor::selectedRenderer"
        "singleeditor::selectionSealed"
        "singleeditor::modernRendererObjects"
        "singleeditor::classicRendererObjects"
        "singleeditor::modernTokenSliceAccesses"
        "singleeditor::classicTokenSliceAccesses"
        "singleeditor::modernAssetLookups"
        "singleeditor::classicAssetLookups"
        "singleeditor::rejectedSelectionChanges"
    TRIGGER "Application startup selects the renderer from preferences or command arguments and core presentation reads passive renderer and token identity"
    RETENTION "Process-local selected mode and passive counters, without loading renderer assets or starting motion")

single_editor_declare_code_unit(
    ID "singleeditor.workspace.renderer-motion-counters"
    OWNER "singleeditor.workspace"
    LIFECYCLE "PostCoreWorkspace"
    SOURCE "Source/UI/Renderer/RendererSelection.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::RendererSelection::noteMotionStart"
        "singleeditor::modernMotionStarts"
        "singleeditor::classicMotionStarts"
    TRIGGER "Admitted workspace presentation starts a visible module transition and records its renderer-specific motion event"
    RETENTION "Two passive process-local motion counters only, with no timer, renderer object or resource lease")

single_editor_declare_code_unit(
    ID "singleeditor.window-shell.renderer-core-style"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreEager"
    SOURCE "Source/UI/Renderer/RendererVisualStyle.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::RendererVisualStyle::accentBlue"
        "singleeditor::RendererVisualStyle::accentInk"
        "singleeditor::RendererVisualStyle::accentOrange"
        "singleeditor::RendererVisualStyle::accentSoft"
        "singleeditor::RendererVisualStyle::assetIdForFileName"
        "singleeditor::RendererVisualStyle::borderColour"
        "singleeditor::RendererVisualStyle::cardRadius"
        "singleeditor::RendererVisualStyle::componentRadius"
        "singleeditor::RendererVisualStyle::danger"
        "singleeditor::RendererVisualStyle::disabledText"
        "singleeditor::RendererVisualStyle::elevatedPanelBackground"
        "singleeditor::RendererVisualStyle::isClassic"
        "singleeditor::RendererVisualStyle::mutedSurface"
        "singleeditor::RendererVisualStyle::panelBackground"
        "singleeditor::RendererVisualStyle::primaryText"
        "singleeditor::RendererVisualStyle::secondaryText"
        "singleeditor::RendererVisualStyle::waveformSurface"
        "singleeditor::RendererVisualStyle::windowBackground"
        "singleeditor::RendererVisualStyle::workspaceSurface"
        "singleeditor::selectedTheme"
        "singleeditor::selectedGlass"
        "singleeditor::reducedMotion"
        "singleeditor::classicWaveformLanes"
    PDB_METHODS
        "singleeditor::ModernNorthStarVisualContract::assetToken"
    MEMBERS "singleeditor::ModernNorthStarMotionPolicy::reduceMotion"
    TRIGGER "Existing initial window and ordinary waveform require shared palette, geometry and cold icon identifiers, and startup applies stored visual preferences"
    RETENTION "Passive selected theme, material and motion preference values and immutable color or asset-name tokens. Decoding, packs and motion retain separate admission")

single_editor_declare_code_unit(
    ID "singleeditor.workspace.renderer-geometry"
    OWNER "singleeditor.workspace"
    LIFECYCLE "PostCoreWorkspace"
    SOURCE "Source/UI/Renderer/RendererVisualStyle.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::RendererVisualStyle::focusRingWidth"
        "singleeditor::RendererVisualStyle::sweetSpotSatisfied"
        "singleeditor::RendererVisualStyle::titleTextSize"
    TRIGGER "Post-Core workspace shell layout and admitted Output Meter or module paint request focus metrics, title typography and visual-size capacity"
    RETENTION "Pure renderer-dependent presentation values borrowed by existing workspace components, without backend or asset loading")

single_editor_declare_code_unit(
    ID "singleeditor.core-edit-ui.renderer-motion"
    OWNER "singleeditor.core-edit-ui"
    LIFECYCLE "CoreDeferred"
    SOURCE "Source/UI/Renderer/RendererVisualStyle.cpp"
    SOURCE_BASE "project"
    TARGETS "SingleEditor"
    MEMBERS
        "singleeditor::RendererMotionController::start"
        "singleeditor::RendererMotionController::interrupt"
        "singleeditor::RendererMotionController::advance"
    TRIGGER "CoreReady-enabled glass sampling or admitted visible workspace transitions request bounded interpolation"
    RETENTION "Consumer-owned scalar interpolation state through stop or reset. Construction, snapTo reset, callback storage and destruction are not classified by invocation time")

single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${intent_definition_modules_root}/juce_audio_basics/buffers/juce_AudioChannelSet.cpp"
    ID "singleeditor.channel-layout.audio-basics-catalog"
    OWNER "singleeditor.channel-layout"
    LIFECYCLE "CoreEager"
    SOURCE "${m3_remaining_source_1_source}"
    SOURCE_BASE "${m3_remaining_source_1_base}"
    TARGETS "SingleEditor" "SingleEditorCLI"
    SCOPES
        "juce::AudioChannelSet"
    TRIGGER "Ordinary WAV reader header interpretation constructs canonical and discrete channel sets before the file can become playable"
    RETENTION "Reader and routing channel membership values reused by later metadata and save operations")

single_editor_declare_code_unit(
    ID "singleeditor.core-playback.audio-basics-resampling"
    OWNER "singleeditor.core-playback"
    LIFECYCLE "CoreEager"
    SOURCE "${m3_remaining_source_1_source}"
    SOURCE_BASE "${m3_remaining_source_1_base}"
    TARGETS "SingleEditor" "SingleEditorCLI"
    SCOPES
        "juce::ResamplingAudioSource"
    PDB_METHODS
        "juce::PositionableAudioSource::setLooping"
    TRIGGER "Existing ordinary transport setSource prepares source-rate correction and positionable source loop behavior"
    RETENTION "Transport-owned resampler and filter history until transport release"
    PDB_VFTABLE_CLASSES
        "juce::ResamplingAudioSource" ".?AVResamplingAudioSource@juce@@")

single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${intent_definition_modules_root}/juce_audio_basics/buffers/juce_AudioProcessLoadMeasurer.cpp"
    ID "singleeditor.audio-output.audio-basics-load"
    OWNER "singleeditor.audio-output"
    LIFECYCLE "BackendLazy"
    SOURCE "${m3_remaining_source_1_source}"
    SOURCE_BASE "${m3_remaining_source_1_base}"
    TARGETS "SingleEditor" "SingleEditorCLI"
    SCOPES
        "juce::AudioProcessLoadMeasurer"
    TRIGGER "Admitted AudioDeviceManager device start and render callbacks create or update load measurements"
    RETENTION "Current device load accumulator and scoped callback timer through device stop")

single_editor_declare_code_unit(
    ID "singleeditor.audio-output.audio-basics-midi-values"
    OWNER "singleeditor.audio-output"
    LIFECYCLE "BackendLazy"
    SOURCE "${m3_remaining_source_1_source}"
    SOURCE_BASE "${m3_remaining_source_1_base}"
    TARGETS "SingleEditor" "SingleEditorCLI"
    PDB_METHODS
        "juce::MidiMessage::~MidiMessage"
        "juce::universal_midi_packets::Midi1ToMidi2DefaultTranslator::processControlChange"
        "juce::universal_midi_packets::Midi1ToMidi2DefaultTranslator::processNoteOnOrOff"
        "juce::universal_midi_packets::Midi1ToMidi2DefaultTranslator::processProgramChange"
        "juce::universal_midi_packets::View::size"
    TRIGGER "Existing admitted device-manager MIDI support translates and releases endpoint MIDI packet values"
    RETENTION "Endpoint callback-owned MIDI values with no standalone MIDI product activation")

if(SINGLE_EDITOR_LAZY_FFT_REGISTRATION)
    set(fft_registry_lifecycle "BackendLazy")
    set(fft_registry_trigger "The first admitted FFT constructor registers compiled engines once before selecting by unchanged priority")
else()
    set(fft_registry_lifecycle "CoreEager")
    set(fft_registry_trigger "The upstream static fftFallback EngineImpl constructor registers and sorts available FFT engines before process entry")
endif()
single_editor_declare_code_unit(
    ID "singleeditor.fft.provider-registry"
    OWNER "singleeditor.fft"
    LIFECYCLE "${fft_registry_lifecycle}"
    SOURCE "${m3_remaining_source_2_source}"
    SOURCE_BASE "${m3_remaining_source_2_base}"
    TARGETS "SingleEditor" "SingleEditorCLI"
    SCOPES
        "juce::dsp::FFT::Engine"
    PDB_FUNCTIONS
        "std::_Pop_heap_hole_by_index<juce::dsp::FFT::Engine * *,juce::dsp::FFT::Engine *,`juce::dsp::FFT::Engine::Engine'::`2'::<lambda_1> >"
        "std::_Sort_unchecked<juce::dsp::FFT::Engine * *,`juce::dsp::FFT::Engine::Engine'::`2'::<lambda_1> >"
    PDB_METHODS
        "juce::dsp::FFT::EngineImpl<juce::dsp::FFTFallback>::`scalar deleting destructor'"
    TRIGGER "${fft_registry_trigger}"
    RETENTION "Process-retained provider registry after its actual configured trigger. FFT plans and scratch buffers remain request-local. The overlay changes initialization timing, not provider algorithms or selection priority."
    PDB_VFTABLE_CLASSES
        "juce::dsp::FFT::Engine" ".?AUEngine@FFT@dsp@juce@@")

single_editor_declare_code_unit(
    ID "singleeditor.fft.runtime-gui"
    OWNER "singleeditor.fft"
    LIFECYCLE "ModuleLazy"
    SOURCE "${m3_remaining_source_2_source}"
    SOURCE_BASE "${m3_remaining_source_2_base}"
    TARGETS "SingleEditor"
    SCOPES
        "juce::dsp::FFTFallback"
    MEMBERS
        "juce::dsp::FFT::FFT"
        "juce::dsp::FFT::~FFT"
        "juce::dsp::FFT::performFrequencyOnlyForwardTransform"
    PDB_METHODS
        "juce::dsp::FFT::EngineImpl<juce::dsp::FFTFallback>::create"
    TRIGGER "Explicit or retained Spectral view after CoreReady, or separately admitted binaural renderer, requests a request-local FFT instance"
    RETENTION "Request-local FFT plans and scratch buffers through admitted spectral or binaural work. Shared FFT computation uses its own responsibility and does not transfer either caller into the spectral Mag."
    PDB_VFTABLE_CLASSES
        "juce::dsp::FFTFallback" ".?AUFFTFallback@dsp@juce@@")

single_editor_declare_code_unit(
    ID "singleeditor.fft.runtime-cli"
    OWNER "singleeditor.fft"
    LIFECYCLE "ModuleLazy"
    SOURCE "${m3_remaining_source_2_source}"
    SOURCE_BASE "${m3_remaining_source_2_base}"
    TARGETS "SingleEditorCLI"
    SCOPES
        "juce::dsp::FFTFallback"
    MEMBERS
        "juce::dsp::FFT::FFT"
    PDB_METHODS
        "juce::dsp::FFT::EngineImpl<juce::dsp::FFTFallback>::create"
    TRIGGER "Admitted CLI binaural renderer creates FFT plans and transforms kernel or source blocks"
    RETENTION "Binaural-renderer-owned FFT plans and scratch buffers until renderer release"
    PDB_VFTABLE_CLASSES
        "juce::dsp::FFTFallback" ".?AUFFTFallback@dsp@juce@@")

single_editor_declare_code_unit(
    ID "singleeditor.spectral.fft-window"
    OWNER "singleeditor.spectral"
    LIFECYCLE "ModuleLazy"
    SOURCE "${m3_remaining_source_2_source}"
    SOURCE_BASE "${m3_remaining_source_2_base}"
    TARGETS "SingleEditor"
    PDB_METHODS
        "juce::dsp::WindowingFunction<float>::WindowingFunction<float>"
    TRIGGER "Requested spectrogram build constructs the Hann window after its document and CoreReady admission"
    RETENTION "Spectrogram-job window coefficients until build completion")

single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${intent_definition_modules_root}/juce_gui_extra/misc/juce_SystemTrayIconComponent.cpp"
        "${intent_definition_modules_root}/juce_gui_extra/native/juce_SystemTrayIcon_windows.cpp"
    ID "singleeditor.window-shell.native-tray-provider"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreDeferred"
    SOURCE "${m3_remaining_source_3_source}"
    SOURCE_BASE "${m3_remaining_source_3_base}"
    TARGETS "SingleEditor"
    SCOPES
        "juce::SystemTrayIconComponent"
    TRIGGER "Explicit close-to-tray action constructs the native tray component and installs its icon and window callback"
    RETENTION "Native tray icon and hook through explicit tray removal or shutdown"
    PDB_VFTABLE_CLASSES
        "juce::SystemTrayIconComponent" ".?AVSystemTrayIconComponent@juce@@")

single_editor_declare_code_unit(
    ID "singleeditor.window-shell.extra-native-peer-support"
    OWNER "singleeditor.window-shell"
    LIFECYCLE "CoreEager"
    SOURCE "${m3_remaining_source_3_source}"
    SOURCE_BASE "${m3_remaining_source_3_base}"
    TARGETS "SingleEditor"
    PDB_METHODS
        "juce::ComponentPeer::addScaleFactorListener"
        "juce::AccessibilityHandler::getDescription"
        "juce::AccessibilityHandler::getHelp"
        "juce::AccessibilityHandler::getTitle"
    TRIGGER "Initial native host observes DPI and exposes the existing accessibility bridge title and descriptions"
    RETENTION "Native peer scale listeners and base accessibility responses through peer release")

single_editor_declare_code_unit(
    ID "singleeditor.core-edit-ui.recent-file-provider"
    OWNER "singleeditor.core-edit-ui"
    LIFECYCLE "CoreDeferred"
    SOURCE "${m3_remaining_source_3_source}"
    SOURCE_BASE "${m3_remaining_source_3_base}"
    TARGETS "SingleEditor"
    PDB_METHODS
        "juce::RecentlyOpenedFilesList::removeFile"
    TRIGGER "Explicit recent-file operation prunes a missing entry through WorkspaceSessionSupport"
    RETENTION "Existing recent-file value list after the requested update")

single_editor_declare_code_unit(
    ID "singleeditor.core-file.extra-file-tree-defaults"
    OWNER "singleeditor.core-file"
    LIFECYCLE "CoreDeferred"
    SOURCE "${m3_remaining_source_3_source}"
    SOURCE_BASE "${m3_remaining_source_3_base}"
    TARGETS "SingleEditor"
    PDB_METHODS
        "juce::TreeViewItem::canBeSelected"
        "juce::TreeViewItem::createItemComponent"
        "juce::TreeViewItem::customComponentUsesTreeViewMouseHandler"
        "juce::TreeViewItem::getItemHeight"
        "juce::TreeViewItem::getItemWidth"
    TRIGGER "Explicit file chooser may construct its existing fallback file tree and query base item behavior"
    RETENTION "Requested chooser tree items until chooser dismissal")

single_editor_declare_code_unit(
    ID "singleeditor.core-edit-ui.extra-text-input-defaults"
    OWNER "singleeditor.core-edit-ui"
    LIFECYCLE "CoreDeferred"
    SOURCE "${m3_remaining_source_3_source}"
    SOURCE_BASE "${m3_remaining_source_3_base}"
    TARGETS "SingleEditor"
    PDB_METHODS
        "juce::TextInputTarget::getKeyboardType"
    TRIGGER "Explicit text editor input uses the existing TextInputTarget default keyboard type"
    RETENTION "Existing requested text editor input target through control release")

single_editor_declare_code_unit(
    INCLUDED_DEFINITIONS
        "${intent_definition_modules_root}/juce_events/broadcasters/juce_AsyncUpdater.cpp"
        "${intent_definition_modules_root}/juce_events/broadcasters/juce_ChangeBroadcaster.cpp"
    ID "singleeditor.cli-automation.event-publication-values"
    OWNER "singleeditor.cli-automation"
    LIFECYCLE "ModuleLazy"
    SOURCE "${m3_remaining_source_4_source}"
    SOURCE_BASE "${m3_remaining_source_4_base}"
    TARGETS "SingleEditorCLI"
    SCOPES
        "juce::AsyncUpdater"
        "juce::ChangeBroadcaster"
    PDB_METHODS
        "juce::MessageManager::MessageBase::post"
    TRIGGER "Explicit CLI engine render inspection constructs the transport broadcaster and its async callback storage before any backend is required"
    RETENTION "Command-owned broadcaster and ref-counted callback values. Posting can fail when no message manager exists and does not initialize one"
    PDB_VFTABLE_CLASSES
        "juce::ChangeBroadcaster::ChangeBroadcasterCallback" ".?AVChangeBroadcasterCallback@ChangeBroadcaster@juce@@"
        "juce::AsyncUpdater" ".?AVAsyncUpdater@juce@@"
        "juce::AsyncUpdater::AsyncUpdaterMessage" ".?AVAsyncUpdaterMessage@AsyncUpdater@juce@@"
        "juce::ChangeBroadcaster" ".?AVChangeBroadcaster@juce@@")

single_editor_declare_code_unit(
    ID "singleeditor.diagnostics.cli-native-message-runtime"
    OWNER "singleeditor.diagnostics"
    LIFECYCLE "BackendLazy"
    SOURCE "${m3_remaining_source_4_source}"
    SOURCE_BASE "${m3_remaining_source_4_base}"
    TARGETS "SingleEditorCLI"
    SCOPES
        "juce::InternalMessageQueue"
        "juce::ScopedJuceInitialiser_GUI"
        "juce::ActionBroadcaster::ActionMessage"
    PDB_METHODS
        "juce::MessageManager::getInstance"
        "juce::MessageManager::deleteInstance"
        "juce::MessageManager::doPlatformSpecificInitialisation"
        "juce::MessageManager::doPlatformSpecificShutdown"
        "juce::MessageManager::isThisTheMessageThread"
        "juce::DeletedAtShutdown::deleteAll"
    MEMBERS "juce::numScopedInitInstances"
        "juce::JUCEApplicationBase::createInstance"
        "juce::JUCEApplicationBase::appInstance"
        "juce::MessageManager::instance"
        "juce::detail::RunningInUnity::state"
        "juce::ShutdownDetector::singletonHolder"
    TRIGGER "Validated explicit diagnostic-device-worker route initializes the JUCE native message runtime and shuts it down on worker exit"
    RETENTION "Worker-owned native message queue and cleanup iteration. Static singleton-holder wrappers and unrelated Timer machinery are not selected"
    PDB_VFTABLE_CLASSES
        "juce::ActionBroadcaster::ActionMessage" ".?AVActionMessage@ActionBroadcaster@juce@@")


single_editor_write_code_intent()
