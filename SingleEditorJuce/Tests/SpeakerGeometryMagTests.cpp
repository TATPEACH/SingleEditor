#include "Audio/AudioEngine.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Components/AmbisonicFieldPanelComponent.h"
#include "UI/Components/SpeakerGeometryMagComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace
{
using namespace singleeditor;

struct ProductionFontEnvironment
{
    ProductionFontEnvironment() { juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel); }
    ~ProductionFontEnvironment() { juce::LookAndFeel::setDefaultLookAndFeel(nullptr); }
    ModernAppleLookAndFeel lookAndFeel;
};

void require(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "SpeakerGeometryMagTests failed: " << message << '\n';
    std::exit(1);
}

juce::Component* findDescendantWithId(juce::Component& root, const juce::String& componentId)
{
    for (auto index = 0; index < root.getNumChildComponents(); ++index)
    {
        auto* child = root.getChildComponent(index);
        if (child->getComponentID() == componentId)
            return child;
        if (auto* match = findDescendantWithId(*child, componentId))
            return match;
    }
    return nullptr;
}

int sampledPixelDifference(const juce::Image& lhs, const juce::Image& rhs)
{
    if (!lhs.isValid() || lhs.getBounds() != rhs.getBounds())
        return 0;
    auto differences = 0;
    for (auto y = 0; y < lhs.getHeight(); y += 4)
        for (auto x = 0; x < lhs.getWidth(); x += 4)
            if (lhs.getPixelAt(x, y) != rhs.getPixelAt(x, y))
                ++differences;
    return differences;
}

int sampledRegionVariation(const juce::Image& image, juce::Rectangle<int> region)
{
    region = region.getIntersection(image.getBounds());
    if (!image.isValid() || region.isEmpty())
        return 0;
    const auto reference = image.getPixelAt(region.getX(), region.getY());
    auto variations = 0;
    for (auto y = region.getY(); y < region.getBottom(); y += 4)
        for (auto x = region.getX(); x < region.getRight(); x += 4)
            if (image.getPixelAt(x, y) != reference)
                ++variations;
    return variations;
}

bool writePng(const juce::Image& image, const juce::File& destination)
{
    destination.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(destination.createOutputStream());
    return stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream);
}

juce::Rectangle<int> spatialMapPixelBounds(const juce::Image& image)
{
    auto left = image.getWidth(), top = image.getHeight(), right = -1, bottom = -1;
    // The shared tab strip ends at y=34; its Night chrome also contains blue.
    for (auto y = 35; y < image.getHeight(); ++y)
        for (auto x = 0; x < image.getWidth(); ++x)
        {
            const auto pixel = image.getPixelAt(x, y);
            // Include the map's light grid and antialiased edge. Exact fill
            // matching erodes the poles differently at different raster sizes.
            if (pixel.getRed() <= 64 && pixel.getBlue() >= 55 && pixel.getBlue() <= 120
                && pixel.getGreen() >= 30 && pixel.getGreen() <= 90
                && pixel.getBlue() - pixel.getGreen() > 15
                && pixel.getGreen() - pixel.getRed() > 10)
            {
                left = juce::jmin(left, x);
                top = juce::jmin(top, y);
                right = juce::jmax(right, x);
                bottom = juce::jmax(bottom, y);
            }
        }
    return right >= left ? juce::Rectangle<int>(left, top, right - left + 1, bottom - top + 1)
                         : juce::Rectangle<int>();
}

bool writeAmbixFoaFixture(const juce::File& destination)
{
    destination.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream(destination.createOutputStream());
    if (stream == nullptr)
        return false;

    const std::unordered_map<juce::String, juce::String> metadata {
        { juce::WavAudioFormat::bwavCodingHistory,
          "A=PCM,F=48000,W=24,M=4,Rec Mode=AmbiX; Fixture=SingleEditor-M3-21" },
        { juce::WavAudioFormat::bwavDescription,
          "AmbiX FOA | zTRK1=W | zTRK2=Y | zTRK3=Z | zTRK4=X" }
    };
    auto options = juce::AudioFormatWriterOptions()
                       .withSampleRate(48000.0)
                       .withNumChannels(4)
                       .withBitsPerSample(24)
                       .withMetadataValues(metadata);
    auto writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
        return false;

    constexpr auto sampleCount = 24000;
    juce::AudioBuffer<float> buffer(4, sampleCount);
    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto frequency = 180.0f + 70.0f * static_cast<float>(channel);
        const auto amplitude = channel == 0 ? 0.18f : 0.10f;
        for (auto sample = 0; sample < sampleCount; ++sample)
        {
            const auto phase = juce::MathConstants<float>::twoPi * frequency
                             * static_cast<float>(sample) / 48000.0f;
            buffer.setSample(channel, sample, amplitude * std::sin(phase));
        }
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, sampleCount);
}

void runNoWindowViewAndAccessibilityTest(const juce::File& outputDirectory)
{
    require(juce::TopLevelWindow::getNumTopLevelWindows() == 0,
            "test starts without a native top-level window");

    SingleTrackDocument document;
    AudioEngine engine(document);
    ModernAppleLookAndFeel lookAndFeel;
    AmbisonicFieldPanelComponent panel(engine);
    panel.setLookAndFeel(&lookAndFeel);
    panel.setBounds(0, 0, 720, 560);
    panel.setAmbisonicOrder(3);

    auto* fieldTab = dynamic_cast<juce::Button*>(
        findDescendantWithId(panel, "ambisonics-field-tab"));
    auto* geometryTab = dynamic_cast<juce::Button*>(
        findDescendantWithId(panel, "ambisonics-geometry-tab"));
    auto* geometryEditor = findDescendantWithId(panel, "speaker-geometry-editor");
    auto* geometryViewport = dynamic_cast<juce::Viewport*>(
        findDescendantWithId(panel, "ambisonics-geometry-viewport"));
    auto* stateLabel = findDescendantWithId(panel, "speaker-geometry-state");
    auto* identityLabel = findDescendantWithId(panel, "speaker-geometry-identity");
    auto* diagnosticsLabel = findDescendantWithId(panel, "speaker-geometry-diagnostics");

    require(fieldTab != nullptr && geometryTab != nullptr && geometryEditor != nullptr
                && geometryViewport != nullptr,
            "Field, Geometry, and editor identities are discoverable without UIA coordinates");
    require(stateLabel != nullptr && identityLabel != nullptr && diagnosticsLabel != nullptr,
            "state, artifact identity, and diagnostics expose stable automation identities");
    require(panel.getActiveView() == AmbisonicFieldPanelComponent::ActiveView::field
                && !geometryEditor->isVisible()
                && fieldTab->getToggleState()
                && !geometryTab->getToggleState(),
            "Field is the deterministic initial view and Geometry owns no empty panel");
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "an unhosted verification component acquires no field-analysis lease");

    for (const auto tier : { RendererInformationTier::summary, RendererInformationTier::standard,
                             RendererInformationTier::detail })
    {
        panel.getProperties().set("northStarInformationTier", static_cast<int>(tier));
        for (const auto size : { juce::Point<int>(134, 150), juce::Point<int>(180, 180),
                                 juce::Point<int>(240, 220), juce::Point<int>(320, 220),
                                 juce::Point<int>(320, 560), juce::Point<int>(620, 340),
                                 juce::Point<int>(720, 560) })
        {
            panel.setSize(size.x, size.y);
            const auto snapshot = panel.createComponentSnapshot(panel.getLocalBounds());
            if (outputDirectory != juce::File())
                require(outputDirectory.createDirectory().wasOk()
                            && writePng(snapshot, outputDirectory.getChildFile("field-"
                                + juce::String(static_cast<int>(tier)) + "-"
                                + juce::String(size.x) + "x" + juce::String(size.y) + ".png")),
                        "writes actual responsive Field rendering");
            if (tier == RendererInformationTier::standard && size.x >= 240)
            {
                const auto map = spatialMapPixelBounds(snapshot);
                require(!map.isEmpty() && std::abs(map.getWidth() - 2 * map.getHeight()) <= 4,
                        "rendered spatial map preserves the projection's 2:1 aspect ratio");
                const juce::Rectangle<int> cueArea(map.getX() + 12, map.getBottom() + 2,
                    map.getWidth() - 24, panel.getHeight() - map.getBottom() - 10);
                require(sampledRegionVariation(snapshot, cueArea) > 6,
                        "idle playback guidance is rendered below the spatial map");
            }
        }
    }
    panel.getProperties().set("northStarInformationTier", static_cast<int>(RendererInformationTier::standard));
    panel.setSize(720, 560);
    const auto fieldSnapshot = panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.0f);
    require(fieldSnapshot.isValid(), "Field view paints to an offscreen image without a peer");

    require(static_cast<bool>(geometryTab->onClick),
            "Geometry tab exposes the same callback used by native accessibility Invoke");
    geometryTab->onClick();
    require(panel.getActiveView() == AmbisonicFieldPanelComponent::ActiveView::geometry
                && geometryEditor->isVisible()
                && !fieldTab->getToggleState()
                && geometryTab->getToggleState(),
            "Geometry activation switches the existing Mag instead of creating a second panel");
    require(panel.getAccessibilityValueSummary().contains("3 order Ambisonics")
                && panel.getAccessibilityValueSummary().contains("physical device channels absent"),
            "Geometry accessibility summary declares order and route-neutral boundary");
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "Geometry view does not retain or acquire the field-analysis lease");

    const auto geometrySnapshot = panel.createComponentSnapshot(panel.getLocalBounds(), true, 1.0f);
    require(geometrySnapshot.isValid()
                && sampledPixelDifference(fieldSnapshot, geometrySnapshot) > 200,
            "offscreen Field and Geometry views render materially different real content");
    require(sampledRegionVariation(geometrySnapshot,
                                   { 8, geometrySnapshot.getHeight() / 2,
                                     geometrySnapshot.getWidth() - 16,
                                     geometrySnapshot.getHeight() / 2 - 8 }) > 600,
            "detail Geometry uses its lower area for the declared speaker map instead of blank space");

    // These include the real one-column native Mag, not just a wide offscreen fixture.
    for (const auto size : { juce::Point<int>(134, 150), juce::Point<int>(240, 180),
                             juce::Point<int>(430, 220), juce::Point<int>(720, 150),
                             juce::Point<int>(134, 285), juce::Point<int>(134, 560), juce::Point<int>(240, 260),
                             juce::Point<int>(430, 300), juce::Point<int>(450, 320),
                             juce::Point<int>(620, 340), juce::Point<int>(240, 560), juce::Point<int>(320, 560),
                             juce::Point<int>(720, 460), juce::Point<int>(720, 560) })
    {
        panel.setSize(size.x, size.y);
        geometryViewport->setViewPosition(0, 0);
        if (outputDirectory != juce::File())
        {
            require(outputDirectory.createDirectory().wasOk()
                        && writePng(panel.createComponentSnapshot(panel.getLocalBounds()),
                                    outputDirectory.getChildFile("geometry-"
                                        + juce::String(size.x) + "x" + juce::String(size.y) + ".png")),
                    "writes the real responsive panel before checking its controls");
        }
        require(panel.getLocalBounds().contains(fieldTab->getBounds())
                    && panel.getLocalBounds().contains(geometryTab->getBounds())
                    && geometryTab->getWidth() >= 60,
                "both tabs remain usable in a narrow native Mag");
        for (auto* child : geometryEditor->getChildren())
        {
            if (child->isVisible())
                require(!child->getBounds().isEmpty()
                            && geometryEditor->getLocalBounds().contains(child->getBounds()),
                        "every visible Geometry control has real space in the scrollable editor");
        }
        for (const auto* name : { "Validate", "Generate", "Audition", "Activate" })
        {
            juce::Button* action = nullptr;
            for (auto* child : geometryEditor->getChildren())
                if (auto* button = dynamic_cast<juce::Button*>(child);
                    button != nullptr && button->getButtonText() == name)
                    action = button;
            require(action != nullptr && action->isVisible()
                        && action->getWidth() >= 50 && action->getHeight() >= 24,
                    "all four staged actions remain visible and usable in Compact");
            geometryViewport->setViewPosition(0, action->getY());
            require(geometryViewport->getViewArea().contains(action->getBounds()),
                    "scrolling can fully expose every staged action in a short Mag");
        }
        geometryViewport->setViewPosition(0, geometryEditor->getHeight());
        juce::Button* importAction = nullptr;
        for (auto* child : geometryEditor->getChildren())
            if (auto* button = dynamic_cast<juce::Button*>(child);
                button != nullptr && button->getButtonText() == "Import")
                importAction = button;
        require(importAction != nullptr && importAction->isVisible()
                    && !importAction->getBounds().isEmpty()
                    && geometryViewport->getViewArea().contains(importAction->getBounds()),
                "the final Import action remains reachable at the bottom");
        if (outputDirectory != juce::File())
            require(writePng(panel.createComponentSnapshot(panel.getLocalBounds()),
                                outputDirectory.getChildFile("geometry-bottom-"
                                    + juce::String(size.x) + "x" + juce::String(size.y) + ".png")),
                    "writes actual scrolled-bottom evidence for clipped-action regression");
        if (size == juce::Point<int>(620, 340) || size == juce::Point<int>(240, 560)
            || size == juce::Point<int>(320, 560)
            || size == juce::Point<int>(720, 460))
        {
            const auto mapTop = diagnosticsLabel->getBottom() + 5;
            const juce::Rectangle<int> mapArea(0, mapTop, geometryEditor->getWidth(),
                                                importAction->getY() - mapTop);
            require(mapArea.getHeight() >= 160,
                    "medium detailed and tall compact views reserve readable speaker-map height");
            const auto editorImage = geometryEditor->createComponentSnapshot(geometryEditor->getLocalBounds());
            require(sampledRegionVariation(editorImage, mapArea) > 500,
                    "reserved map area contains the rendered speaker geometry instead of blank space");
        }
        geometryViewport->setViewPosition(0, 0);
        if (outputDirectory != juce::File() && size.x == 134)
        {
            require(outputDirectory.createDirectory().wasOk()
                        && writePng(panel.createComponentSnapshot(panel.getLocalBounds()),
                                    outputDirectory.getChildFile("speaker-geometry-compact.png")),
                    "writes actual narrow-column visual preflight");
        }
    }
    panel.setSize(134, 150);
    geometryViewport->setViewPosition(0, geometryEditor->getHeight());
    require(geometryViewport->getViewPositionY() > 0,
            "short editor is actually scrolled before testing expansion recovery");
    panel.setSize(720, 560);
    require(geometryViewport->getViewPositionY() == 0
                && !geometryViewport->getVerticalScrollBar().isVisible(),
            "expanding the editor removes scrolling and restores its content origin");

    if (outputDirectory != juce::File())
    {
        require(outputDirectory.createDirectory().wasOk(),
                "creates the requested offscreen evidence directory");
        require(writePng(fieldSnapshot, outputDirectory.getChildFile("ambisonic-field.png"))
                    && writePng(geometrySnapshot,
                                outputDirectory.getChildFile("speaker-geometry.png")),
                "writes the two peer-free visual preflight images");
        const auto ambixFixture = outputDirectory.getChildFile("ambix-foa.wav");
        require(writeAmbixFoaFixture(ambixFixture),
                "writes a self-identifying AmbiX FOA native-canary fixture");
        SingleTrackDocument fixtureDocument;
        require(fixtureDocument.loadFile(ambixFixture)
                    && fixtureDocument.getState()->info.channelLayout.isAmbisonic
                    && fixtureDocument.getState()->info.channelLayout.selectedPresetId
                           == "foa-acn-sn3d"
                    && fixtureDocument.getState()->info.channelLayout.detectedFromMetadata,
                "generated fixture selects FOA ACN/SN3D without ComboBox or pointer input");
    }

    require(static_cast<bool>(fieldTab->onClick),
            "Field tab remains directly invokable without focus or pointer input");
    fieldTab->onClick();
    require(panel.getActiveView() == AmbisonicFieldPanelComponent::ActiveView::field
                && !geometryEditor->isVisible()
                && panel.getAccessibilityValueSummary().contains("analysis sleeping"),
            "returning to Field hides the editor and restores truthful sleeping state");
    require(juce::TopLevelWindow::getNumTopLevelWindows() == 0,
            "the complete verification path creates no native window and cannot steal focus");
    panel.setLookAndFeel(nullptr);
}
void runGeometryValidationStates(const juce::File& outputDirectory)
{
    SingleTrackDocument document;
    AudioEngine engine(document);
    ModernAppleLookAndFeel lookAndFeel;
    AmbisonicFieldPanelComponent panel(engine);
    panel.setLookAndFeel(&lookAndFeel);
    panel.setSize(240, 560);
    panel.setActiveView(AmbisonicFieldPanelComponent::ActiveView::geometry);
    auto* geometry = findDescendantWithId(panel, "speaker-geometry-editor");
    require(geometry != nullptr, "validation exercise uses the actual Geometry Mag");
    auto& editor = *geometry;
    const auto label = [&] (const char* id)
    {
        auto* result = dynamic_cast<juce::Label*>(findDescendantWithId(editor, id));
        require(result != nullptr, "validation state labels are present");
        return result;
    };
    const auto button = [&] (const char* text)
    {
        for (auto* child : editor.getChildren())
            if (auto* result = dynamic_cast<juce::Button*>(child);
                result != nullptr && result->getButtonText() == text)
                return result;
        require(false, "staged action is present");
        return static_cast<juce::Button*>(nullptr);
    };
    const auto save = [&] (const char* name)
    {
        if (outputDirectory == juce::File()) return;
        require(outputDirectory.createDirectory().wasOk()
                    && writePng(panel.createComponentSnapshot(panel.getLocalBounds()),
                                outputDirectory.getChildFile(name)),
                "records actual staged geometry state rendering");
    };
    auto* identity = label("speaker-geometry-identity");
    require(identity->getText().contains("Standard 7.1.4")
                && identity->getTooltip().contains("custom-geometry-"),
            "visible identity is the room name while exact profile identity remains inspectable");
    require(label("speaker-geometry-state")->getText().contains("DRAFT")
                && button("Validate")->isEnabled() && !button("Generate")->isEnabled()
                && !button("Audition")->isEnabled() && !button("Activate")->isEnabled(),
            "a derived draft cannot generate, audition or activate before validation");
    save("geometry-draft.png");
    button("Validate")->onClick();
    require(label("speaker-geometry-state")->getText().contains("VALIDATED")
                && button("Generate")->isEnabled() && !button("Activate")->isEnabled(),
            "real validation enables generation without activating audio");
    save("geometry-validated.png");

    juce::TextEditor* azimuth = nullptr;
    for (auto* child : editor.getChildren())
        if (child->getName() == "Azimuth degrees")
            azimuth = dynamic_cast<juce::TextEditor*>(child);
    require(azimuth != nullptr && static_cast<bool>(azimuth->onReturnKey),
            "the real azimuth editor commits through its existing callback");
    const auto original = azimuth->getText();
    azimuth->setText("400", false);
    azimuth->onReturnKey();
    require(!button("Generate")->isEnabled(), "editing invalidates the prior validation");
    button("Validate")->onClick();
    auto* diagnostics = label("speaker-geometry-diagnostics");
    require(label("speaker-geometry-state")->getText().contains("ERROR")
                && diagnostics->getText().contains("Speaker direction")
                && diagnostics->getTooltip().contains("speaker_coordinate_out_of_range")
                && !button("Generate")->isEnabled() && !button("Activate")->isEnabled(),
            "invalid geometry shows actionable diagnostics and blocks later stages");
    save("geometry-invalid.png");
    azimuth->setText(original, false);
    azimuth->onReturnKey();
    button("Validate")->onClick();
    require(label("speaker-geometry-state")->getText().contains("VALIDATED")
                && button("Generate")->isEnabled()
                && engine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == nullptr
                && engine.getAmbisonicDecodeMatrixPreviewForDiagnostics() == nullptr,
            "correcting the draft recovers validation without changing active or preview audio");
    save("geometry-validation-recovered.png");
    auto* geometryEditor = dynamic_cast<SpeakerGeometryMagComponent*>(geometry);
    require(geometryEditor != nullptr, "import completion uses the production editor");
    const auto stateBeforeCancel = label("speaker-geometry-state")->getText();
    const auto identityBeforeCancel = identity->getText();
    const auto diagnosticsBeforeCancel = diagnostics->getText();
    const auto helpBeforeCancel = diagnostics->getTooltip();
    geometryEditor->importProfileFile(juce::File());
    require(label("speaker-geometry-state")->getText() == stateBeforeCancel
                && identity->getText() == identityBeforeCancel
                && diagnostics->getText() == diagnosticsBeforeCancel
                && diagnostics->getTooltip() == helpBeforeCancel
                && button("Generate")->isEnabled(),
            "cancelling profile import preserves the validated draft and its diagnostics");
    juce::TemporaryFile invalidProfile(".json");
    require(invalidProfile.getFile().replaceWithText("not a geometry JSON object"),
            "creates a malformed import fixture");
    geometryEditor->importProfileFile(invalidProfile.getFile());
    require(label("speaker-geometry-state")->getText() == stateBeforeCancel
                && identity->getText() == identityBeforeCancel && button("Generate")->isEnabled()
                && diagnostics->getText().contains("Import failed:")
                && diagnostics->getTooltip().contains("profile_not_object"),
            "failed import preserves the validated draft and exposes matching error help");
    save("geometry-import-failed.png");
    const auto importError = diagnostics->getText();
    geometryEditor->importProfileFile(juce::File());
    require(diagnostics->getText() == importError
                && diagnostics->getTooltip().contains("profile_not_object"),
            "cancelling a retry does not erase the previous actionable error");
    const auto standard = AmbisonicStandardMatrixCatalog::standardProfile(
        "standard-7.1.4-v1");
    require(standard != nullptr, "a real standard geometry is available for import recovery");
    auto imported = *standard;
    imported.parentTemplateId = standard->profileId;
    imported.profileId = "import-recovery-fixture";
    imported.displayName = "Imported room";
    imported.revision = 2;
    imported.builtInReadOnly = false;
    juce::TemporaryFile validProfile(".json");
    require(validProfile.getFile().replaceWithText(
                juce::JSON::toString(SpeakerGeometryProfileCodec::toVar(imported))),
            "creates a real serialized geometry import");
    geometryEditor->importProfileFile(validProfile.getFile());
    require(identity->getText() == "Imported room  r2"
                && label("speaker-geometry-state")->getText().contains("DRAFT")
                && !button("Generate")->isEnabled() && !button("Activate")->isEnabled()
                && !diagnostics->getTooltip().contains("profile_not_object")
                && engine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == nullptr
                && engine.getAmbisonicDecodeMatrixPreviewForDiagnostics() == nullptr,
            "valid import replaces only the draft, clears stale errors and requires validation");
    save("geometry-import-recovered.png");
    button("Validate")->onClick();
    require(button("Generate")->isEnabled(), "the recovered imported draft can be validated");
    const auto stateBeforeExport = label("speaker-geometry-state")->getText();
    const auto identityBeforeExport = identity->getText();
    const auto diagnosticsBeforeExport = diagnostics->getText();
    geometryEditor->exportProfileFile(juce::File(), imported);
    require(diagnostics->getText() == diagnosticsBeforeExport,
            "cancelling geometry export preserves the existing diagnostics");
    juce::TemporaryFile exportTarget(".json");
    require(exportTarget.getFile().replaceWithText("preserved existing profile")
                && exportTarget.getFile().setReadOnly(true),
            "creates a real read-only geometry export target");
    geometryEditor->exportProfileFile(exportTarget.getFile(), imported);
    const auto failedExportText = diagnostics->getText();
    const auto failedExportHelp = diagnostics->getTooltip();
    const auto protectedContents = exportTarget.getFile().loadFileAsString();
    require(exportTarget.getFile().setReadOnly(false), "restores owned export fixture permissions");
    require(protectedContents == "preserved existing profile"
                && failedExportText.contains("Export failed:")
                && failedExportHelp.contains(exportTarget.getFile().getFullPathName())
                && label("speaker-geometry-state")->getText() == stateBeforeExport
                && identity->getText() == identityBeforeExport && button("Generate")->isEnabled(),
            "failed geometry export reports the destination and preserves file and draft state");
    save("geometry-export-failed.png");
    geometryEditor->exportProfileFile(juce::File(), imported);
    require(diagnostics->getText() == failedExportText
                && diagnostics->getTooltip() == failedExportHelp,
            "cancelling export retry preserves the actionable error");
    geometryEditor->exportProfileFile(exportTarget.getFile(), imported);
    const auto exported = SpeakerGeometryProfileCodec::fromVar(
        juce::JSON::parse(exportTarget.getFile().loadFileAsString()));
    require(exported.success && exported.profile.profileId == imported.profileId
                && exported.profile.revision == imported.revision
                && AmbisonicSpeakerGeometryGenerator::profileIdentityHash(exported.profile)
                    == AmbisonicSpeakerGeometryGenerator::profileIdentityHash(imported)
                && diagnostics->getText().startsWith("Exported ")
                && !diagnostics->getText().contains("failed")
                && label("speaker-geometry-state")->getText() == stateBeforeExport
                && identity->getText() == identityBeforeExport
                && engine.getAmbisonicDecodeMatrixArtifactForDiagnostics() == nullptr
                && engine.getAmbisonicDecodeMatrixPreviewForDiagnostics() == nullptr,
            "successful export retry writes the exact profile without altering staged or audio state");
    save("geometry-export-recovered.png");
    panel.setLookAndFeel(nullptr);
}

void runParentVisibilityLifecycleTest(const juce::File& outputDirectory)
{
    juce::TemporaryFile fixture(".wav");
    SingleTrackDocument document;
    AudioEngine engine(document);
    ModernAppleLookAndFeel lookAndFeel;
    require(writeAmbixFoaFixture(fixture.getFile()) && document.loadFile(fixture.getFile()),
            "loads a real self-identifying FOA source for the lifecycle check");
    juce::Component host;
    host.setLookAndFeel(&lookAndFeel);
    host.setName("Single Editor Field lifecycle test");
    host.setBounds(20, 20, 1, 1);
    host.addToDesktop(juce::ComponentPeer::windowIsTemporary
        | juce::ComponentPeer::windowIgnoresKeyPresses
        | juce::ComponentPeer::windowIgnoresMouseClicks);
    juce::Component container;
    AmbisonicFieldPanelComponent panel(engine);
    panel.setSize(320, 220);
    host.addAndMakeVisible(container);
    container.addAndMakeVisible(panel);
    require(!panel.isShowing() && engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "a Field attached below a hidden native host remains dormant");
    host.setVisible(true);
    require(panel.isShowing() && engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 1,
            "showing an ancestor starts exactly one Field analysis lease");
    const auto idleImage = panel.createComponentSnapshot(panel.getLocalBounds());
    engine.setMonitorGainLinear(0.0f);
    juce::AudioBuffer<float> output(8, 256);
    require(engine.renderTransportBlockForDiagnostics(0, output)
                && engine.getAmbisonicFieldSnapshot().valid && output.getMagnitude(0, 256) == 0.0f,
            "visible Field publishes real FOA analysis even with silent monitoring");
    juce::Thread::sleep(90);
    juce::Timer::callPendingTimersSynchronously();
    const auto liveImage = panel.createComponentSnapshot(panel.getLocalBounds());
    require(sampledPixelDifference(idleImage, liveImage) > 500,
            "the visible Field timer paints the real analysis into its spatial map");
    if (outputDirectory != juce::File())
        require(outputDirectory.createDirectory().wasOk()
                    && writePng(idleImage, outputDirectory.getChildFile("field-idle.png"))
                    && writePng(liveImage, outputDirectory.getChildFile("field-live-muted.png")),
                "records actual Field paint before and after real transport analysis");
    container.setVisible(false);
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0
                && !engine.hasAmbisonicFieldAnalyzerForDiagnostics(),
            "hiding a Mag container releases Field analysis immediately");
    container.setVisible(true);
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 1,
            "restoring a Mag container reacquires exactly one lease");
    panel.setActiveView(AmbisonicFieldPanelComponent::ActiveView::geometry);
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "Geometry never retains a Field analysis subscription");
    panel.setActiveView(AmbisonicFieldPanelComponent::ActiveView::field);
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 1,
            "returning to visible Field resumes its subscription");
    host.setVisible(false);
    require(engine.getAmbisonicFieldAnalysisLeaseCountForDiagnostics() == 0,
            "hiding the native host releases Field analysis");
    host.removeFromDesktop();
    host.setLookAndFeel(nullptr);
    std::cout << "FieldParentVisibilityLifecycleTests passed\n";
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    RendererSelection::resetForTests(RendererKind::modern);
    ProductionFontEnvironment productionFonts;
    if ((argc == 2 || argc == 3) && juce::String(argv[1]) == "--lifecycle")
    {
        runParentVisibilityLifecycleTest(argc == 3 ? juce::File(juce::String::fromUTF8(argv[2]))
                                                   : juce::File());
        return 0;
    }
    juce::File outputDirectory;
    if (argc == 3 && juce::String(argv[1]) == "--output")
        outputDirectory = juce::File(juce::String::fromUTF8(argv[2]));
    else
        require(argc == 1, "usage: SpeakerGeometryMagTests [--output <directory> | --lifecycle [directory]]");

    runNoWindowViewAndAccessibilityTest(outputDirectory);
    runGeometryValidationStates(outputDirectory);
    RendererVisualStyle::setModernTheme(ModernTheme::night);
    runNoWindowViewAndAccessibilityTest(outputDirectory == juce::File()
        ? juce::File() : outputDirectory.getChildFile("night"));
    runGeometryValidationStates(outputDirectory == juce::File()
        ? juce::File() : outputDirectory.getChildFile("night"));
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    if (outputDirectory != juce::File())
        std::cout << "Offscreen evidence: " << outputDirectory.getFullPathName() << '\n';
    std::cout << "SpeakerGeometryMagTests passed\n";
    return 0;
}
