#include "Metadata/TypedMetadataInspectorModel.h"
#include "UI/Components/TypedMetadataInspectorComponent.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace singleeditor;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "TypedMetadataInspectorTests failed: " << message << '\n';
        std::exit(1);
    }
}

SingleTrackDocument::FileInfo makeInfo(const juce::File& file)
{
    SingleTrackDocument::FileInfo info;
    info.file = file;
    info.formatName = "WAV";
    info.durationSeconds = 217.842;
    info.sampleRate = 96000.0;
    info.numChannels = 2;
    info.bitsPerSample = 24;
    info.numSamples = 20912832;
    info.channelLayout.displayName = "Stereo L R";
    info.channelLayout.orderText = "L, R";
    info.metadata.set("Artist", "Example Artist");
    info.metadata.set("Comment", "");
    info.metadata.set("Very Long Note", juce::String::repeatedString("long metadata ", 30));
    return info;
}

const TypedMetadataField* findField(const MetadataInspectorSnapshot& snapshot,
                                    const juce::String& id)
{
    for (const auto& group : snapshot.groups)
        for (const auto& field : group.fields)
            if (field.id == id)
                return &field;
    return nullptr;
}

void testPostCoreFactsAndReadOnlyContract(const juce::File& fixture)
{
    auto info = makeInfo(fixture);
    require(TypedMetadataInspectorModel::createPostCore(0, info) == nullptr,
            "generation zero cannot create the post-Core model");

    auto model = TypedMetadataInspectorModel::createPostCore(41, info);
    require(model != nullptr, "valid CoreReady facts create the inspector model");
    const auto& snapshot = model->getSnapshot();
    require(snapshot.active && snapshot.generation == 41,
            "snapshot is fenced to the active document generation");
    require(snapshot.optionalFieldsLoaded && snapshot.groups.size() == 2,
            "Core facts and already-owned embedded tags are projected post-Core");
    require(model->getProjectionProviderConstructionCount() == 0,
            "raw and provenance provider stays at zero before user request");

    const auto* path = findField(snapshot, "core.path");
    const auto* rate = findField(snapshot, "core.sample_rate");
    const auto* artist = findField(snapshot, "tag.artist");
    const auto* empty = findField(snapshot, "tag.comment");
    require(path != nullptr && path->normalisedValue == fixture.getFullPathName(),
            "path comes from document FileInfo without a second reader");
    require(rate != nullptr && rate->normalisedValue == "96.0 kHz",
            "sample rate is typed and normalized");
    require(artist != nullptr && artist->source == "Open-path reader"
                && artist->writeCapability == MetadataWriteCapability::unsupported,
            "embedded tags retain source and visible unsupported-write state");
    require(empty != nullptr && empty->type == MetadataValueType::unknown
                && empty->rawValues.size() == 1 && empty->rawValues[0].isEmpty(),
            "empty values remain truthful instead of disappearing");
    require(model->copyText().contains("write=unsupported"),
            "copy includes the read-only/write contract");
}

void testLazyProjectionAndGenerationFence(const juce::File& fixture)
{
    auto info = makeInfo(fixture);
    auto model = TypedMetadataInspectorModel::createPostCore(7, info);
    require(model != nullptr, "model created");
    const auto* artist = findField(model->getSnapshot(), "tag.artist");
    require(artist != nullptr && model->projectedRawText(*artist).isEmpty(),
            "raw projection is absent before request");
    require(!model->setProjection(6, true, true),
            "stale generation cannot construct an optional provider");
    require(model->getProjectionProviderConstructionCount() == 0,
            "stale request leaves provider at zero");

    require(model->setProjection(7, true, true), "current projection request succeeds");
    require(model->getProjectionProviderConstructionCount() == 1,
            "one provider serves raw and provenance projections");
    require(model->projectedRawText(*artist) == "Example Artist"
                && model->projectedProvenanceText(*artist).contains("metadata key"),
            "requested projections expose exact raw value and provenance");
    require(model->setProjection(7, true, false)
                && model->getProjectionProviderConstructionCount() == 1,
            "changing an active projection does not reconstruct the provider");
    require(model->setProjection(7, false, false)
                && model->projectedRawText(*artist).isEmpty(),
            "closing projections releases their visible data");

    auto replacement = info;
    replacement.sampleRate = 48000.0;
    require(!model->updateCoreFacts(8, replacement),
            "stale refresh cannot replace current document facts");
    require(findField(model->getSnapshot(), "core.sample_rate")->normalisedValue == "96.0 kHz",
            "stale refresh leaves the snapshot unchanged");
}

void testConflictsFailureAndResponsiveDensity(const juce::File& fixture)
{
    auto info = makeInfo(fixture);
    juce::StringPairArray tags(false);
    tags.set("Track Number", "3");
    tags.set("Track-Number", "04");
    tags.set("Empty", "");
    info.metadata = tags;

    auto model = TypedMetadataInspectorModel::createPostCore(99, info);
    require(model != nullptr, "conflict fixture model created");
    const auto* conflict = findField(model->getSnapshot(), "tag.track_number");
    require(conflict != nullptr
                && conflict->conflict == MetadataConflictState::duplicateNormalisedKey
                && conflict->multiplicity == 2
                && conflict->rawValues.size() == 2
                && conflict->nativeKeys.size() == 2,
            "normalized duplicate keys preserve multiplicity and native identity while surfacing conflict");
    require(model->copyText("core").contains("Core Facts")
                && !model->copyText("core").contains("Embedded Tags")
                && model->copyText("embedded").contains("multiplicity=2"),
            "hierarchical copy can select semantic groups without losing multiplicity");

    const auto before = model->copyText();
    require(model->applyOptionalProviderFailure(99, "Synthetic provider failure"),
            "optional provider failure is accepted for current generation");
    const auto after = model->copyText();
    require(after.contains("Synthetic provider failure")
                && after.contains("Sample Rate: 96.0 kHz")
                && before.contains("Sample Rate: 96.0 kHz"),
            "optional failure stays local while Core facts remain available");
    require(!model->applyOptionalProviderFailure(100, "stale"),
            "stale failure cannot contaminate a replacement generation");

    require(TypedMetadataInspectorModel::densityForWidth(269)
                == MetadataInspectorDensity::compact,
            "narrow Mag resolves Compact density");
    require(TypedMetadataInspectorModel::densityForWidth(270)
                == MetadataInspectorDensity::standard,
            "middle-width Mag resolves Standard density");
    require(TypedMetadataInspectorModel::densityForWidth(380)
                == MetadataInspectorDensity::expanded,
            "wide Mag resolves Expanded density");
}

void testResponsiveComponentGenerationLifecycle(const juce::File& fixture)
{
    auto info = makeInfo(fixture);
    ModernAppleLookAndFeel lookAndFeel;
    TypedMetadataInspectorComponent component;
    component.setLookAndFeel(&lookAndFeel);
    component.beginGeneration(120);
    require(!component.activatePostCore(119, info),
            "component rejects stale activation before constructing a model");
    require(component.getModelForTesting() == nullptr,
            "stale activation leaves the component provider-free");
    require(component.activatePostCore(120, info) && component.isActiveFor(120),
            "component activates only for its current post-Core generation");

    component.setSize(250, 640);
    require(component.getDensity() == MetadataInspectorDensity::compact,
            "component reflows to Compact at narrow Mag width");
    component.setSize(320, 640);
    require(component.getDensity() == MetadataInspectorDensity::standard,
            "component reflows to Standard at middle Mag width");
    component.setSize(430, 640);
    require(component.getDensity() == MetadataInspectorDensity::expanded,
            "component reflows to Expanded at wide Mag width");
    require(component.getNumChildComponents() == 7,
            "search, scoped copy, long-value, raw, provenance, and viewport controls are present");

    for (const int width : { 104, 120, 140, 160, 168, 240, 304, 320, 324, 480 })
    {
        component.setSize(width, 280);
        if (width < 160)
        {
            require(juce::GlyphArrangement::getStringWidth(ModernTypography::font(13.0f, juce::Font::bold), "Facts") <= 46.0f
                        && juce::GlyphArrangement::getStringWidth(ModernTypography::font(10.0f), "Read only") <= width - 4 - 46,
                    "narrow metadata heading preserves the complete read-only cue");
        }
        for (int i = 0; i < component.getNumChildComponents(); ++i)
        {
            const auto bounds = component.getChildComponent(i)->getBounds();
            require(!bounds.isEmpty() && component.getLocalBounds().contains(bounds),
                    "all metadata controls and its viewport stay available in a narrow details area");
            if (auto* toggle = dynamic_cast<juce::ToggleButton*>(component.getChildComponent(i)))
                require(juce::GlyphArrangement::getStringWidth(ModernTypography::font(12.0f), toggle->getButtonText())
                            <= toggle->getWidth() - 21,
                        "compact option label must fit without squeezing: " + toggle->getButtonText().toStdString()
                            + " text=" + std::to_string(juce::GlyphArrangement::getStringWidth(
                                ModernTypography::font(12.0f), toggle->getButtonText()))
                            + " available=" + std::to_string(toggle->getWidth() - 21));
            for (int j = i + 1; j < component.getNumChildComponents(); ++j)
                require(!bounds.intersects(component.getChildComponent(j)->getBounds()),
                        "responsive metadata control rows never overlap each other or the facts viewport");
        }
        require(component.getModelForTesting()->getSnapshot().generation == 120,
                "responsive layout leaves the real metadata model and its generation intact");
        component.setSize(width, juce::jmax(280, TypedMetadataInspectorComponent::preferredModernHeight(width)));
        juce::Viewport* factsViewport = nullptr;
        juce::TextEditor* search = nullptr;
        for (int i = 0; i < component.getNumChildComponents(); ++i)
        {
            if (auto* v = dynamic_cast<juce::Viewport*>(component.getChildComponent(i))) factsViewport = v;
            if (auto* e = dynamic_cast<juce::TextEditor*>(component.getChildComponent(i))) search = e;
        }
        require(factsViewport != nullptr && factsViewport->getHeight() >= 168,
                "the parent-requested details height retains two field rows and their group heading after controls wrap");
        require(search != nullptr && search->onTextChange != nullptr, "real metadata search callback is connected");
        const auto allFactsHeight = factsViewport->getViewedComponent()->getHeight();
        search->setText("Sample rate", false);
        search->onTextChange();
        require(factsViewport->getViewedComponent()->getHeight() < allFactsHeight,
                "searching a real core fact reduces the content to matching fields");
        search->setText({}, false);
        search->onTextChange();
        require(factsViewport->getViewedComponent()->getHeight() == allFactsHeight,
                "clearing search restores all metadata fields without replacing their model");
        const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_METADATA_OFFSCREEN_DIR", {});
        if (output.isNotEmpty())
        {
            const juce::File directory(output);
            require(directory.createDirectory().wasOk(), "metadata evidence directory is writable");
            for (const auto theme : { ModernTheme::light, ModernTheme::night })
            {
                RendererVisualStyle::setModernTheme(theme);
                ModernAppleLookAndFeel::refreshTheme(component);
                const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 2.0f);
                require(image.isValid() && image.getWidth() == width * 2 && image.getHeight() == component.getHeight() * 2,
                        "real metadata body painter renders at 200 percent without changing its generation");
                auto stream = directory.getChildFile(juce::String(theme == ModernTheme::light ? "light-" : "night-")
                    + juce::String(width) + ".png").createOutputStream();
                require(stream != nullptr && stream->setPosition(0)
                            && juce::PNGImageFormat().writeImageToStream(image, *stream) && stream->truncate().wasOk(),
                        "metadata body evidence is written");
            }
            RendererVisualStyle::setModernTheme(ModernTheme::light);
        }
    }

    component.beginGeneration(121);
    require(!component.isActiveFor(120) && component.getModelForTesting() == nullptr,
            "new generation clears all prior visible facts before replacement activation");
    component.setLookAndFeel(nullptr);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseGui;
    RendererSelection::resetForTests(RendererKind::modern);
    const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getNonexistentChildFile("single-editor-m3-13-metadata", ".wav", false);
    require(fixture.replaceWithText("typed metadata fixture"), "temporary fixture created");

    testPostCoreFactsAndReadOnlyContract(fixture);
    testLazyProjectionAndGenerationFence(fixture);
    testConflictsFailureAndResponsiveDensity(fixture);
    testResponsiveComponentGenerationLifecycle(fixture);

    require(fixture.deleteFile(), "temporary fixture removed");
    std::cout << "TypedMetadataInspectorTests passed\n";
    return 0;
}
