#include "UI/Components/SpeakerGeometryMagComponent.h"

#include <algorithm>
#include <cmath>

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
namespace
{
constexpr int detailMinimumWidth = 430;
constexpr int geometryMapMinimumHeight = 180;
// Header, controls, diagnostics and file actions occupy 276 logical pixels.
constexpr int detailMinimumHeight = 276 + geometryMapMinimumHeight;

constexpr std::array<const char*, 6> standardProfiles {
    "standard-mono-v1", "standard-stereo-v1", "standard-quad-v1",
    "standard-5.1-v1", "standard-7.1-v1", "standard-7.1.4-v1"
};

constexpr std::array<SpeakerRole, 19> editableRoles {
    SpeakerRole::mono, SpeakerRole::L, SpeakerRole::R, SpeakerRole::C, SpeakerRole::LFE,
    SpeakerRole::Lc, SpeakerRole::Rc, SpeakerRole::Lw, SpeakerRole::Rw,
    SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::Lss, SpeakerRole::Rss,
    SpeakerRole::Lsr, SpeakerRole::Rsr, SpeakerRole::Ltf, SpeakerRole::Rtf,
    SpeakerRole::Ltr, SpeakerRole::Rtr
};

void configureNumericEditor(juce::TextEditor& editor, const juce::String& name)
{
    editor.setName(name);
    editor.setInputRestrictions(12, "-+.0123456789");
    editor.setSelectAllWhenFocused(true);
}

juce::String sessionStateText(SpeakerGeometrySessionState state)
{
    switch (state)
    {
        case SpeakerGeometrySessionState::empty: return "EMPTY";
        case SpeakerGeometrySessionState::draft: return "DRAFT";
        case SpeakerGeometrySessionState::validated: return "VALIDATED";
        case SpeakerGeometrySessionState::generating: return "GENERATING";
        case SpeakerGeometrySessionState::candidate: return "CANDIDATE";
        case SpeakerGeometrySessionState::auditioning: return "AUDITIONING";
        case SpeakerGeometrySessionState::active: return "ACTIVE";
        case SpeakerGeometrySessionState::error: return "ERROR";
    }
    return "UNKNOWN";
}
}

SpeakerGeometryMagComponent::SpeakerGeometryMagComponent(AudioEngine& engineToUse)
    : engine(engineToUse),
      session([this] (auto artifact)
      {
          return engine.activateAmbisonicDecodeMatrixArtifact(std::move(artifact));
      })
{
    setName("Speaker Geometry Editor");
    setTitle("Speaker Geometry Editor");
    setComponentID("speaker-geometry-editor");
    setDescription("Staged custom speaker geometry editor. Physical device channels are intentionally absent.");
    for (auto index = 0; index < static_cast<int>(standardProfiles.size()); ++index)
        templateBox.addItem(templateLabel(standardProfiles[static_cast<std::size_t>(index)]), index + 1);
    templateBox.setSelectedId(6, juce::dontSendNotification);
    templateBox.setName("Geometry Template");
    templateBox.setTooltip("Read-only standard geometry used to derive a new custom profile");

    dimensionalityBox.addItem("2D ring", 1);
    dimensionalityBox.addItem("3D sphere", 2);
    weightPolicyBox.addItem("Unity weight", 1);
    weightPolicyBox.addItem("Explicit static weight", 2);
    dimensionalityBox.setName("Geometry Dimensionality");
    weightPolicyBox.setName("Geometry Weight Policy");
    speakerBox.setName("Geometry Speaker");
    roleBox.setName("Canonical Speaker Role");
    for (const auto role : editableRoles)
    {
        const auto abbreviation = ChannelLayoutCatalog::roleToAbbreviation(role);
        roleBox.addItem(abbreviation, static_cast<int>(role) + 1);
    }

    configureNumericEditor(azimuthEditor, "Azimuth degrees");
    configureNumericEditor(elevationEditor, "Elevation degrees");
    configureNumericEditor(distanceEditor, "Distance metres");
    configureNumericEditor(weightEditor, "Static weight");
    statusLabel.setJustificationType(juce::Justification::centredRight);
    identityLabel.setJustificationType(juce::Justification::centredLeft);
    identityLabel.setMinimumHorizontalScale(1.0f);
    diagnosticsLabel.setJustificationType(juce::Justification::topLeft);
    diagnosticsLabel.setMinimumHorizontalScale(1.0f);
    for (auto* label : { &statusLabel, &identityLabel, &diagnosticsLabel })
        label->setFont(ModernTypography::font(11.0f));
    statusLabel.setName("Speaker Geometry State");
    statusLabel.setComponentID("speaker-geometry-state");
    identityLabel.setName("Speaker Geometry Identity");
    identityLabel.setComponentID("speaker-geometry-identity");
    diagnosticsLabel.setName("Speaker Geometry Diagnostics");
    diagnosticsLabel.setComponentID("speaker-geometry-diagnostics");
    for (auto* label : { &azimuthLabel, &elevationLabel, &distanceLabel, &weightLabel })
    {
        label->setJustificationType(juce::Justification::centredLeft);
        ModernAppleLookAndFeel::setLabelRole(*label, ModernAppleLookAndFeel::LabelRole::secondary);
        label->setFont(ModernTypography::font(9.0f, juce::Font::bold));
    }

    const std::array<juce::Component*, 25> components {
        &templateBox, &deriveButton, &dimensionalityBox, &weightPolicyBox,
        &speakerBox, &roleBox, &azimuthEditor, &elevationEditor,
        &distanceEditor, &weightEditor, &azimuthLabel, &elevationLabel,
        &distanceLabel, &weightLabel, &enabledButton, &availableButton,
        &validateButton, &generateButton, &auditionButton, &activateButton,
        &importButton, &exportButton, &statusLabel, &identityLabel, &diagnosticsLabel
    };
    for (auto* component : components)
    {
        addAndMakeVisible(component);
    }

    deriveButton.onClick = [this] { deriveSelectedTemplate(); };
    validateButton.onClick = [this] { validateDraft(); };
    generateButton.onClick = [this] { generateCandidate(); };
    auditionButton.onClick = [this] { toggleAudition(); };
    activateButton.onClick = [this] { activateCandidate(); };
    importButton.onClick = [this] { importProfile(); };
    exportButton.onClick = [this] { exportProfile(); };
    speakerBox.onChange = [this]
    {
        if (suppressCallbacks)
            return;
        selectedSpeakerIndex = speakerBox.getSelectedItemIndex();
        loadSelectedSpeaker();
    };
    roleBox.onChange = [this]
    {
        if (!suppressCallbacks)
            commitSelectedSpeaker();
    };
    dimensionalityBox.onChange = [this]
    {
        if (!suppressCallbacks)
            commitProfilePolicy();
    };
    weightPolicyBox.onChange = dimensionalityBox.onChange;
    enabledButton.onClick = [this] { commitSelectedSpeaker(); };
    availableButton.onClick = [this] { commitSelectedSpeaker(); };
    const auto commitEditor = [this] { commitSelectedSpeaker(); };
    for (auto* editor : { &azimuthEditor, &elevationEditor, &distanceEditor, &weightEditor })
    {
        editor->onFocusLost = commitEditor;
        editor->onReturnKey = commitEditor;
    }

    deriveSelectedTemplate();
}

SpeakerGeometryMagComponent::~SpeakerGeometryMagComponent()
{
    clearAudition();
}

void SpeakerGeometryMagComponent::setAmbisonicOrder(int order)
{
    const auto next = juce::jlimit(1, 3, order);
    if (ambisonicOrder == next)
        return;
    clearAudition();
    transientStatus.clear();
    ambisonicOrder = next;
    if (hasDraft)
        session.setDraft(draft);
    refreshState();
}

juce::String SpeakerGeometryMagComponent::templateLabel(const juce::String& profileId)
{
    if (profileId.contains("7.1.4")) return "7.1.4 room";
    if (profileId.contains("7.1")) return "7.1 room";
    if (profileId.contains("5.1")) return "5.1 room";
    if (profileId.contains("quad")) return "Quad room";
    if (profileId.contains("stereo")) return "Stereo room";
    return "Mono room";
}

void SpeakerGeometryMagComponent::deriveSelectedTemplate()
{
    clearAudition();
    transientStatus.clear();
    const auto index = juce::jlimit(0,
                                    static_cast<int>(standardProfiles.size()) - 1,
                                    templateBox.getSelectedId() - 1);
    const auto source = AmbisonicStandardMatrixCatalog::standardProfile(
        standardProfiles[static_cast<std::size_t>(index)]);
    if (source == nullptr)
        return;
    draft = *source;
    draft.parentTemplateId = source->profileId;
    draft.profileId = "custom-geometry-" + juce::Uuid().toString().substring(0, 12);
    draft.displayName = "Custom " + source->displayName;
    draft.revision = 1;
    draft.builtInReadOnly = false;
    draft.opaqueSource = juce::var();
    for (auto& speaker : draft.speakers)
        speaker.opaqueSource = juce::var();
    hasDraft = true;
    session.setDraft(draft);
    selectedSpeakerIndex = 0;
    rebuildSpeakerSelector();
    refreshState();
}

void SpeakerGeometryMagComponent::rebuildSpeakerSelector()
{
    suppressCallbacks = true;
    speakerBox.clear(juce::dontSendNotification);
    for (auto index = 0; index < static_cast<int>(draft.speakers.size()); ++index)
    {
        const auto& speaker = draft.speakers[static_cast<std::size_t>(index)];
        speakerBox.addItem(speaker.label + "  "
                               + ChannelLayoutCatalog::roleToAbbreviation(speaker.role),
                           index + 1);
    }
    selectedSpeakerIndex = juce::jlimit(0,
                                        juce::jmax(0, static_cast<int>(draft.speakers.size()) - 1),
                                        selectedSpeakerIndex);
    speakerBox.setSelectedItemIndex(selectedSpeakerIndex, juce::dontSendNotification);
    dimensionalityBox.setSelectedId(
        draft.dimensionality == SpeakerGeometryDimensionality::sphere3D ? 2 : 1,
        juce::dontSendNotification);
    weightPolicyBox.setSelectedId(
        draft.weightPolicy == SpeakerGeometryWeightPolicy::explicitStatic ? 2 : 1,
        juce::dontSendNotification);
    suppressCallbacks = false;
    loadSelectedSpeaker();
}

void SpeakerGeometryMagComponent::loadSelectedSpeaker()
{
    if (!hasDraft || !juce::isPositiveAndBelow(selectedSpeakerIndex + 1,
                                                static_cast<int>(draft.speakers.size()) + 1))
        return;
    suppressCallbacks = true;
    const auto& speaker = draft.speakers[static_cast<std::size_t>(selectedSpeakerIndex)];
    roleBox.setSelectedId(static_cast<int>(speaker.role) + 1, juce::dontSendNotification);
    azimuthEditor.setText(juce::String(speaker.azimuthDegrees, 2), false);
    elevationEditor.setText(juce::String(speaker.elevationDegrees, 2), false);
    distanceEditor.setText(juce::String(speaker.distanceMetres, 3), false);
    weightEditor.setText(juce::String(speaker.staticWeight, 3), false);
    enabledButton.setToggleState(speaker.enabled, juce::dontSendNotification);
    availableButton.setToggleState(speaker.available, juce::dontSendNotification);
    suppressCallbacks = false;
    repaint(geometryMapBounds);
}

void SpeakerGeometryMagComponent::commitSelectedSpeaker()
{
    if (suppressCallbacks || !hasDraft
        || !juce::isPositiveAndBelow(selectedSpeakerIndex + 1,
                                     static_cast<int>(draft.speakers.size()) + 1))
        return;
    auto& speaker = draft.speakers[static_cast<std::size_t>(selectedSpeakerIndex)];
    speaker.role = static_cast<SpeakerRole>(juce::jmax(0, roleBox.getSelectedId() - 1));
    speaker.azimuthDegrees = static_cast<float>(azimuthEditor.getText().getDoubleValue());
    speaker.elevationDegrees = static_cast<float>(elevationEditor.getText().getDoubleValue());
    speaker.distanceMetres = static_cast<float>(distanceEditor.getText().getDoubleValue());
    speaker.staticWeight = static_cast<float>(weightEditor.getText().getDoubleValue());
    speaker.enabled = enabledButton.getToggleState();
    speaker.available = availableButton.getToggleState();
    publishDraftMutation();
}

void SpeakerGeometryMagComponent::commitProfilePolicy()
{
    if (suppressCallbacks || !hasDraft)
        return;
    draft.dimensionality = dimensionalityBox.getSelectedId() == 2
                         ? SpeakerGeometryDimensionality::sphere3D
                         : SpeakerGeometryDimensionality::ring2D;
    draft.weightPolicy = weightPolicyBox.getSelectedId() == 2
                       ? SpeakerGeometryWeightPolicy::explicitStatic
                       : SpeakerGeometryWeightPolicy::unity;
    publishDraftMutation();
}

void SpeakerGeometryMagComponent::publishDraftMutation()
{
    clearAudition();
    transientStatus.clear();
    ++draft.revision;
    session.setDraft(draft);
    rebuildSpeakerSelector();
    refreshState();
}

void SpeakerGeometryMagComponent::validateDraft()
{
    if (!hasDraft)
        return;
    transientStatus.clear();
    session.validateDraft();
    refreshState();
}

void SpeakerGeometryMagComponent::generateCandidate()
{
    if (generationInFlight)
        return;
    transientStatus.clear();
    const auto request = session.beginGeneration(ambisonicOrder);
    if (!request.has_value())
    {
        refreshState();
        return;
    }
    generationInFlight = true;
    refreshState();
    juce::Component::SafePointer<SpeakerGeometryMagComponent> safeThis(this);
    juce::Thread::launch([safeThis, request = *request]
    {
        auto result = AmbisonicSpeakerGeometryGenerator::generate(request);
        juce::MessageManager::callAsync([safeThis, result = std::move(result)] () mutable
        {
            if (safeThis == nullptr)
                return;
            safeThis->generationInFlight = false;
            safeThis->session.acceptGenerationResult(std::move(result));
            safeThis->refreshState();
        });
    });
}

void SpeakerGeometryMagComponent::toggleAudition()
{
    transientStatus.clear();
    if (session.getState() == SpeakerGeometrySessionState::auditioning)
    {
        clearAudition();
        refreshState();
        return;
    }
    const auto artifact = session.beginAudition();
    if (artifact == nullptr || !engine.previewAmbisonicDecodeMatrixArtifact(artifact))
        session.endAudition();
    refreshState();
}

void SpeakerGeometryMagComponent::activateCandidate()
{
    clearAudition();
    transientStatus.clear();
    if (!session.activateCandidate())
        transientStatus = "ACTIVATION REJECTED - LAST VALID KEPT";
    refreshState();
}

void SpeakerGeometryMagComponent::clearAudition() noexcept
{
    if (session.getState() == SpeakerGeometrySessionState::auditioning)
    {
        engine.clearAmbisonicDecodeMatrixPreview();
        session.endAudition();
    }
}

void SpeakerGeometryMagComponent::importProfile()
{
    fileChooser = std::make_unique<juce::FileChooser>("Import Speaker Geometry", juce::File(), "*.json");
    juce::Component::SafePointer<SpeakerGeometryMagComponent> safeThis(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis != nullptr)
            safeThis->importProfileFile(chooser.getResult());
    });
}

void SpeakerGeometryMagComponent::importProfileFile(const juce::File& file)
{
    // Cancel and failed reads must leave the draft and any current audition intact.
    if (file == juce::File())
        return;
    const auto parsed = SpeakerGeometryProfileCodec::fromVar(juce::JSON::parse(file.loadFileAsString()));
    if (!parsed.success)
    {
        diagnosticsLabel.setText("Import failed: " + parsed.errorMessage,
                                 juce::dontSendNotification);
        diagnosticsLabel.setTooltip(parsed.errorCode + ": " + parsed.errorMessage);
        return;
    }
    clearAudition();
    draft = parsed.profile;
    transientStatus.clear();
    hasDraft = true;
    session.setDraft(draft);
    selectedSpeakerIndex = 0;
    rebuildSpeakerSelector();
    refreshState();
}

void SpeakerGeometryMagComponent::exportProfile()
{
    if (!hasDraft)
        return;
    fileChooser = std::make_unique<juce::FileChooser>("Export Speaker Geometry",
                                                      juce::File::getSpecialLocation(
                                                          juce::File::userDocumentsDirectory)
                                                          .getChildFile(draft.profileId + ".json"),
                                                      "*.json");
    const auto profile = draft;
    juce::Component::SafePointer<SpeakerGeometryMagComponent> safeThis(this);
    fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safeThis, profile] (const juce::FileChooser& chooser)
    {
        if (safeThis != nullptr)
            safeThis->exportProfileFile(chooser.getResult(), profile);
    });
}

void SpeakerGeometryMagComponent::exportProfileFile(const juce::File& file,
                                                   const SpeakerGeometryProfile& profile)
{
    if (file == juce::File())
        return;

    const auto written = file.replaceWithText(
        juce::JSON::toString(SpeakerGeometryProfileCodec::toVar(profile), true));
    diagnosticsLabel.setText(written ? "Exported " + file.getFileName() + "."
                                    : "Export failed: could not write " + file.getFileName() + ".",
                             juce::dontSendNotification);
    diagnosticsLabel.setTooltip(written
        ? "Geometry profile saved to " + file.getFullPathName()
        : "Choose a writable location or check the file permissions.\n" + file.getFullPathName());
}

juce::String SpeakerGeometryMagComponent::stateText() const
{
    return sessionStateText(session.getState()) + "  /  "
         + juce::String(ambisonicOrder) + "OA";
}

juce::String SpeakerGeometryMagComponent::diagnosticsText() const
{
    const auto& validation = session.getValidation();
    if (validation.diagnostics.empty())
        return session.getState() == SpeakerGeometrySessionState::validated
             ? "Validation passed. Generate a decoder candidate to audition before activation."
             : "Speaker positions only. Assign device channels in Output Mapping.";
    const auto firstError = std::find_if(validation.diagnostics.begin(), validation.diagnostics.end(),
        [] (const auto& item) { return item.severity == SpeakerGeometryDiagnosticSeverity::error; });
    const auto& item = firstError != validation.diagnostics.end() ? *firstError : validation.diagnostics.front();
    juce::String result = item.severity == SpeakerGeometryDiagnosticSeverity::error ? "Error: " : "Warning: ";
    result += item.message;
    if (validation.diagnostics.size() > 1)
        result += " (+" + juce::String(static_cast<int>(validation.diagnostics.size()) - 1) + ")";
    return result;
}

void SpeakerGeometryMagComponent::refreshState()
{
    const auto state = session.getState();
    statusLabel.setText(transientStatus.isNotEmpty() ? transientStatus : stateText(),
                        juce::dontSendNotification);
    statusLabel.setTooltip(statusLabel.getText());
    const auto candidate = session.getCandidate();
    const auto active = session.getActive();
    auto identity = hasDraft ? draft.displayName + "  r" + juce::String(draft.revision) : juce::String();
    auto identityDetails = hasDraft ? identity + "\n" + draft.profileId : juce::String();
    if (active != nullptr)
    {
        identityDetails += "\nActive decoder: " + active->matrixHash;
    }
    if (candidate != nullptr)
    {
        identityDetails += "\nCandidate decoder: " + candidate->matrixHash;
    }
    identityLabel.setText(identity, juce::dontSendNotification);
    identityLabel.setTooltip(identityDetails);
    diagnosticsLabel.setText(diagnosticsText(), juce::dontSendNotification);
    // The compact label shows the most important issue; all diagnostic codes
    // and messages remain available through its tooltip/accessibility help.
    juce::String diagnosticDetails;
    for (const auto& item : session.getValidation().diagnostics)
    {
        if (diagnosticDetails.isNotEmpty()) diagnosticDetails += "\n";
        diagnosticDetails += item.code + " - " + item.message;
    }
    diagnosticsLabel.setTooltip(diagnosticDetails.isEmpty() ? diagnosticsText() : diagnosticDetails);
    validateButton.setEnabled(hasDraft && !generationInFlight);
    generateButton.setEnabled(state == SpeakerGeometrySessionState::validated && !generationInFlight);
    auditionButton.setEnabled((candidate != nullptr || state == SpeakerGeometrySessionState::auditioning)
                              && !generationInFlight);
    auditionButton.setButtonText(state == SpeakerGeometrySessionState::auditioning
                               ? "End Audition" : "Audition");
    activateButton.setEnabled(candidate != nullptr && !generationInFlight);
    exportButton.setEnabled(hasDraft);
    repaint();
}

void SpeakerGeometryMagComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    auto title = bounds.removeFromTop(22);
    g.setColour(ModernAppleLookAndFeel::primaryText());
    g.setFont(ModernTypography::font(RendererVisualStyle::titleTextSize(), juce::Font::bold));
    auto boundaryTag = getWidth() >= 210 ? title.removeFromRight(68) : juce::Rectangle<int>();
    g.drawText(getWidth() < 280 ? "Geometry" : "Speaker Geometry", title,
               juce::Justification::centredLeft);
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(ModernTypography::font(9.0f, juce::Font::bold));
    g.drawText("Layout only", boundaryTag, juce::Justification::centredRight);
    drawGeometryMap(g);
}

void SpeakerGeometryMagComponent::drawGeometryMap(juce::Graphics& g) const
{
    if (!hasDraft || geometryMapBounds.isEmpty())
        return;

    auto bounds = geometryMapBounds.toFloat();
    g.setColour(ModernAppleLookAndFeel::mutedSurface());
    g.fillRoundedRectangle(bounds, ModernAppleLookAndFeel::componentRadius());
    g.setColour(ModernAppleLookAndFeel::borderColour().withAlpha(0.72f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), ModernAppleLookAndFeel::componentRadius(), 1.0f);

    auto content = bounds.reduced(10.0f);
    auto caption = content.removeFromTop(17.0f);
    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(ModernTypography::font(9.0f, juce::Font::bold));
    g.drawText(bounds.getWidth() < 320 ? "TOP VIEW" : "DECLARED GEOMETRY  /  TOP VIEW",
               caption,
               juce::Justification::centredLeft);
    content.removeFromTop(4.0f);

    const auto radius = 0.46f * juce::jmin(content.getWidth(), content.getHeight());
    const auto centre = content.getCentre();
    const auto ring = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
    g.setColour(ModernAppleLookAndFeel::borderColour().withAlpha(0.66f));
    g.drawEllipse(ring, 1.0f);
    g.drawEllipse(ring.reduced(radius * 0.5f), 1.0f);
    g.drawLine(centre.x, ring.getY(), centre.x, ring.getBottom(), 1.0f);
    g.drawLine(ring.getX(), centre.y, ring.getRight(), centre.y, 1.0f);

    g.setColour(ModernAppleLookAndFeel::secondaryText());
    g.setFont(ModernTypography::font(8.0f, juce::Font::bold));
    g.drawText("FRONT", juce::Rectangle<float>(centre.x - 28.0f, ring.getY() - 1.0f, 56.0f, 12.0f),
               juce::Justification::centred);
    g.drawText("BACK", juce::Rectangle<float>(centre.x - 28.0f, ring.getBottom() - 11.0f, 56.0f, 12.0f),
               juce::Justification::centred);

    for (auto index = 0; index < static_cast<int>(draft.speakers.size()); ++index)
    {
        const auto& speaker = draft.speakers[static_cast<std::size_t>(index)];
        if (!speaker.enabled)
            continue;
        const auto azimuth = juce::degreesToRadians(speaker.azimuthDegrees);
        const auto elevation = juce::degreesToRadians(speaker.elevationDegrees);
        const auto projectedRadius = radius * std::max(0.12f, std::cos(elevation));
        const juce::Point<float> point = speaker.role == SpeakerRole::LFE
            ? centre
            : juce::Point<float> {
                  centre.x - std::sin(azimuth) * projectedRadius,
                  centre.y - std::cos(azimuth) * projectedRadius
              };

        auto colour = speaker.available
                    ? ModernAppleLookAndFeel::accentBlue()
                    : ModernAppleLookAndFeel::secondaryText().withAlpha(0.55f);
        if (index == selectedSpeakerIndex)
            colour = ModernAppleLookAndFeel::accentOrange();
        if (speaker.role == SpeakerRole::LFE)
            colour = ModernAppleLookAndFeel::accentPurple();

        g.setColour(colour.withAlpha(0.24f));
        g.fillEllipse(juce::Rectangle<float>(18.0f, 18.0f).withCentre(point));
        g.setColour(colour);
        g.fillEllipse(juce::Rectangle<float>(7.0f, 7.0f).withCentre(point));

        const auto role = ChannelLayoutCatalog::roleToAbbreviation(speaker.role);
        auto labelBounds = juce::Rectangle<float>(42.0f, 14.0f).withCentre(point.translated(0.0f, -12.0f));
        g.setFont(ModernTypography::font(8.5f, juce::Font::bold));
        g.drawText(role, labelBounds, juce::Justification::centred);
    }
}

int SpeakerGeometryMagComponent::contentHeightForViewport(int width, int height,
                                                           int scrollbarWidth) noexcept
{
    // Enter the existing detail tier only when its fields fit after reserving
    // the scrollbar. The map then receives real space rather than the remainder.
    if (height >= 255 && width - scrollbarWidth >= detailMinimumWidth)
        return juce::jmax(height, detailMinimumHeight);
    return juce::jmax(height, 220);
}

void SpeakerGeometryMagComponent::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(26);
    const auto compact = getHeight() < detailMinimumHeight || getWidth() < detailMinimumWidth;
    const auto summary = getWidth() < 280 || getHeight() < 240;
    if (summary)
    {
        identityLabel.setBounds(area.removeFromTop(22));
        statusLabel.setBounds(area.removeFromTop(22));
    }
    else
    {
        auto statusRow = area.removeFromTop(24);
        identityLabel.setBounds(statusRow.removeFromLeft(statusRow.getWidth() * 2 / 3));
        statusLabel.setBounds(statusRow);
    }
    area.removeFromTop(5);

    const std::array<juce::Component*, 14> detailControls {
        &dimensionalityBox, &weightPolicyBox, &roleBox,
        &azimuthEditor, &elevationEditor, &distanceEditor, &weightEditor,
        &azimuthLabel, &elevationLabel, &distanceLabel, &weightLabel,
        &enabledButton, &availableButton, &exportButton
    };
    for (auto* component : detailControls)
        component->setVisible(!compact);
    templateBox.setVisible(!summary);
    deriveButton.setVisible(!summary);
    speakerBox.setVisible(!summary);

    if (summary)
    {
        // A real one-column Mag can be only 114 logical pixels wide. Keep the
        // transaction controls reachable; editing fields belong to larger tiers.
        geometryMapBounds = {};
        const auto width = (area.getWidth() - 5) / 2;
        auto firstRow = area.removeFromTop(28);
        validateButton.setBounds(firstRow.removeFromLeft(width));
        firstRow.removeFromLeft(5);
        generateButton.setBounds(firstRow);
        area.removeFromTop(5);
        auto secondRow = area.removeFromTop(28);
        auditionButton.setBounds(secondRow.removeFromLeft(width));
        secondRow.removeFromLeft(5);
        activateButton.setBounds(secondRow);
        area.removeFromTop(5);
        auto bottom = area.removeFromBottom(26);
        importButton.setBounds(bottom.removeFromLeft(juce::jmin(62, bottom.getWidth())));
        if (getWidth() >= 210 && area.getHeight() >= 42 + 5 + geometryMapMinimumHeight)
        {
            diagnosticsLabel.setBounds(area.removeFromTop(42));
            area.removeFromTop(5);
            geometryMapBounds = area;
        }
        else
        {
            diagnosticsLabel.setBounds(area);
        }
        return;
    }

    if (compact)
    {
        geometryMapBounds = {};
        auto templateRow = area.removeFromTop(28);
        templateBox.setBounds(templateRow.removeFromLeft(juce::jmax(110, templateRow.getWidth() - 74)));
        templateRow.removeFromLeft(5);
        deriveButton.setBounds(templateRow);
        area.removeFromTop(5);
        speakerBox.setBounds(area.removeFromTop(28));
        area.removeFromTop(6);
        auto actionRow = area.removeFromTop(28);
        const auto width = (actionRow.getWidth() - 15) / 4;
        for (auto* button : { &validateButton, &generateButton, &auditionButton, &activateButton })
        {
            button->setBounds(actionRow.removeFromLeft(width));
            actionRow.removeFromLeft(5);
        }
        area.removeFromTop(5);
        auto bottom = area.removeFromBottom(26);
        importButton.setBounds(bottom.removeFromLeft(62));
        if (area.getHeight() >= 42 + 5 + geometryMapMinimumHeight)
        {
            diagnosticsLabel.setBounds(area.removeFromTop(42));
            area.removeFromTop(5);
            geometryMapBounds = area;
        }
        else
        {
            diagnosticsLabel.setBounds(area);
        }
        return;
    }

    auto templateRow = area.removeFromTop(28);
    templateBox.setBounds(templateRow.removeFromLeft(juce::jmax(110, templateRow.getWidth() / 3)));
    templateRow.removeFromLeft(5);
    deriveButton.setBounds(templateRow.removeFromLeft(68));
    templateRow.removeFromLeft(8);
    dimensionalityBox.setBounds(templateRow.removeFromLeft(92));
    templateRow.removeFromLeft(5);
    weightPolicyBox.setBounds(templateRow);
    area.removeFromTop(5);

    auto speakerRow = area.removeFromTop(28);
    speakerBox.setBounds(speakerRow.removeFromLeft(juce::jmax(100, speakerRow.getWidth() / 3)));
    speakerRow.removeFromLeft(5);
    roleBox.setBounds(speakerRow.removeFromLeft(76));
    speakerRow.removeFromLeft(5);
    enabledButton.setBounds(speakerRow.removeFromLeft(78));
    availableButton.setBounds(speakerRow);
    area.removeFromTop(5);

    auto coordinateLabels = area.removeFromTop(15);
    const auto fieldWidth = juce::jmax(54, (coordinateLabels.getWidth() - 15) / 4);
    azimuthLabel.setBounds(coordinateLabels.removeFromLeft(fieldWidth));
    coordinateLabels.removeFromLeft(5);
    elevationLabel.setBounds(coordinateLabels.removeFromLeft(fieldWidth));
    coordinateLabels.removeFromLeft(5);
    distanceLabel.setBounds(coordinateLabels.removeFromLeft(fieldWidth));
    coordinateLabels.removeFromLeft(5);
    weightLabel.setBounds(coordinateLabels);
    auto coordinates = area.removeFromTop(28);
    azimuthEditor.setBounds(coordinates.removeFromLeft(fieldWidth));
    coordinates.removeFromLeft(5);
    elevationEditor.setBounds(coordinates.removeFromLeft(fieldWidth));
    coordinates.removeFromLeft(5);
    distanceEditor.setBounds(coordinates.removeFromLeft(fieldWidth));
    coordinates.removeFromLeft(5);
    weightEditor.setBounds(coordinates);
    area.removeFromTop(6);

    auto actionRow = area.removeFromTop(28);
    const auto actionWidth = (actionRow.getWidth() - 15) / 4;
    for (auto* button : { &validateButton, &generateButton, &auditionButton, &activateButton })
    {
        button->setBounds(actionRow.removeFromLeft(actionWidth));
        actionRow.removeFromLeft(5);
    }
    area.removeFromTop(5);
    auto fileRow = area.removeFromBottom(26);
    importButton.setBounds(fileRow.removeFromLeft(62));
    fileRow.removeFromLeft(5);
    exportButton.setBounds(fileRow.removeFromLeft(62));
    diagnosticsLabel.setBounds(area.removeFromTop(42));
    area.removeFromTop(5);
    geometryMapBounds = area;
}
} // namespace singleeditor
