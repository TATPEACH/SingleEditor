#include "UI/Components/TypedMetadataInspectorComponent.h"

#include <algorithm>
#include <array>

#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernTypography.h"

namespace singleeditor
{
namespace
{
juce::Font metadataFont(float modernEm, float classicHeight, int style = juce::Font::plain)
{
    return RendererVisualStyle::isClassic() ? juce::Font(juce::FontOptions(classicHeight, style))
                                           : ModernTypography::font(modernEm, style);
}

struct MetadataControlsLayout
{
    std::array<juce::Rectangle<int>, 5> actions;
    int viewportTop = 0;

    static MetadataControlsLayout calculate(int width) noexcept
    {
        MetadataControlsLayout result;
        width = juce::jmax(1, width);
        // Keep the copy pair together, then the display options. Narrow Mags
        // need less chrome padding, not smaller type or hidden capabilities.
        const std::array<int, 5> widths = width >= 324
            ? std::array<int, 5> { 76, 52, 52, 52, 76 }
            : std::array<int, 5> { 52, 48, 51, 47, 66 };
        int x = 0, y = 57;
        for (size_t i = 0; i < widths.size(); ++i)
        {
            if (i == 2 && x > 0 && x + widths[2] + widths[3] + widths[4] + 8 > width)
            {
                x = 0;
                y += 30;
            }
            const auto actual = juce::jmin(width, widths[i]);
            if (x > 0 && x + actual > width) { x = 0; y += 30; }
            result.actions[i] = { x, y, actual, 26 };
            x += actual + 4;
        }
        result.viewportTop = y + 31;
        return result;
    }
};

int rowHeightForDensity(const TypedMetadataField& field,
                        MetadataInspectorDensity density,
                        bool showRaw,
                        bool showProvenance,
                        bool showLongValues) noexcept
{
    auto height = density == MetadataInspectorDensity::compact ? 64
                : density == MetadataInspectorDensity::standard ? 72
                                                                  : 80;
    if (showLongValues && field.normalisedValue.length() > 120)
        height += 42;
    if (showRaw)
        height += 18;
    if (showProvenance)
        height += 18;
    return height;
}

juce::String densityName(MetadataInspectorDensity density)
{
    switch (density)
    {
        case MetadataInspectorDensity::compact: return "Compact";
        case MetadataInspectorDensity::standard: return "Standard";
        case MetadataInspectorDensity::expanded: return "Expanded";
    }
    return "Compact";
}
}

class TypedMetadataInspectorComponent::ContentComponent final : public juce::Component
{
public:
    void setModel(const TypedMetadataInspectorModel* modelToUse)
    {
        model = modelToUse;
        repaint();
    }

    void setDensity(MetadataInspectorDensity densityToUse)
    {
        density = densityToUse;
        repaint();
    }

    void setSearchQuery(juce::String queryToUse)
    {
        searchQuery = queryToUse.trim().toLowerCase();
        repaint();
    }

    void setShowLongValues(bool shouldShow)
    {
        showLongValues = shouldShow;
        repaint();
    }

    [[nodiscard]] int requiredHeight(int width) const
    {
        if (model == nullptr)
            return 72;

        const auto& snapshot = model->getSnapshot();
        auto height = snapshot.localFailure.isNotEmpty() ? 44 : 0;
        for (const auto& group : snapshot.groups)
        {
            const auto visibleCount = static_cast<int>(std::count_if(
                group.fields.begin(), group.fields.end(),
                [this](const TypedMetadataField& field) { return matchesSearch(field); }));
            if (visibleCount == 0)
                continue;
            height += 28;
            for (const auto& field : group.fields)
                if (matchesSearch(field))
                    height += rowHeightForDensity(field,
                                                  density,
                                                  snapshot.rawProjectionVisible,
                                                  snapshot.provenanceProjectionVisible,
                                                  showLongValues);
            height += 8;
        }
        return juce::jmax(72, height + (width < 280 ? 2 : 8));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(RendererVisualStyle::panelBackground());
        if (model == nullptr)
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(metadataFont(12.0f, RendererVisualStyle::bodyTextSize()));
            g.drawFittedText("Typed metadata attaches after CoreReady.",
                             getLocalBounds().reduced(8),
                             juce::Justification::centred,
                             2);
            return;
        }

        const auto& snapshot = model->getSnapshot();
        auto area = getLocalBounds().reduced(2, 0);
        if (snapshot.localFailure.isNotEmpty())
        {
            auto failure = area.removeFromTop(38).reduced(6, 3);
            g.setColour(RendererVisualStyle::danger().withAlpha(0.10f));
            g.fillRoundedRectangle(failure.toFloat(), RendererVisualStyle::componentRadius());
            g.setColour(RendererVisualStyle::danger());
            g.setFont(metadataFont(11.0f, RendererVisualStyle::captionTextSize(), juce::Font::bold));
            g.drawText("Optional metadata unavailable: " + snapshot.localFailure,
                       failure.reduced(7, 0), juce::Justification::centredLeft, true);
        }

        for (const auto& group : snapshot.groups)
        {
            const auto hasVisibleField = std::any_of(
                group.fields.begin(), group.fields.end(),
                [this](const TypedMetadataField& field) { return matchesSearch(field); });
            if (!hasVisibleField)
                continue;
            auto heading = area.removeFromTop(28);
            if (heading.intersects(g.getClipBounds()))
            {
                g.setColour(RendererVisualStyle::secondaryText());
                g.setFont(metadataFont(11.0f, RendererVisualStyle::captionTextSize(), juce::Font::bold));
                g.drawText(group.label.toUpperCase(), heading.reduced(7, 0),
                           juce::Justification::centredLeft, true);
            }

            for (const auto& field : group.fields)
            {
                if (!matchesSearch(field))
                    continue;
                const auto rowHeight = rowHeightForDensity(field,
                                                            density,
                                                            snapshot.rawProjectionVisible,
                                                            snapshot.provenanceProjectionVisible,
                                                            showLongValues);
                auto row = area.removeFromTop(rowHeight).reduced(4, 2);
                if (!row.intersects(g.getClipBounds()))
                    continue;
                g.setColour(RendererVisualStyle::workspaceSurface());
                g.fillRoundedRectangle(row.toFloat(), RendererVisualStyle::componentRadius());
                g.setColour(field.conflict == MetadataConflictState::none
                                ? RendererVisualStyle::borderColour()
                                : RendererVisualStyle::danger());
                g.drawRoundedRectangle(row.toFloat().reduced(0.5f),
                                       RendererVisualStyle::componentRadius(),
                                       RendererVisualStyle::magBorderWidth());

                auto text = row.reduced(8, 4);
                auto labelLine = text.removeFromTop(17);
                g.setColour(RendererVisualStyle::secondaryText());
                g.setFont(metadataFont(11.0f, RendererVisualStyle::captionTextSize(), juce::Font::bold));
                g.drawText(field.label, labelLine, juce::Justification::centredLeft, true);

                const auto isLong = field.normalisedValue.length() > 120;
                const auto expandedLong = isLong && showLongValues;
                auto valueLine = text.removeFromTop(expandedLong ? 54
                                                                 : (density == MetadataInspectorDensity::expanded ? 21 : 18));
                g.setColour(RendererVisualStyle::primaryText());
                g.setFont(metadataFont(density == MetadataInspectorDensity::compact ? 12.0f : 13.0f,
                    density == MetadataInspectorDensity::compact ? RendererVisualStyle::captionTextSize()
                                                                 : RendererVisualStyle::bodyTextSize()));
                if (expandedLong)
                {
                    g.drawFittedText(field.normalisedValue, valueLine,
                                     juce::Justification::topLeft, 3, 0.85f);
                }
                else
                {
                    const auto visibleValue = isLong
                        ? field.normalisedValue.substring(0, 96)
                            + "... [" + juce::String(field.normalisedValue.length()) + " chars; use Long]"
                        : field.normalisedValue;
                    g.drawText(visibleValue, valueLine, juce::Justification::centredLeft, true);
                }

                const auto summary = TypedMetadataInspectorModel::typeName(field.type)
                    + "  |  " + field.source
                    + (field.multiplicity > 1 ? " x" + juce::String(field.multiplicity) : juce::String())
                    + "  |  confidence:" + TypedMetadataInspectorModel::confidenceName(field.confidence)
                    + "  |  conflict:" + TypedMetadataInspectorModel::conflictName(field.conflict)
                    + "  |  " + TypedMetadataInspectorModel::writeCapabilityName(field.writeCapability);
                g.setColour(field.conflict == MetadataConflictState::none
                                ? RendererVisualStyle::disabledText()
                                : RendererVisualStyle::danger());
                g.setFont(metadataFont(10.0f, RendererVisualStyle::captionTextSize()));
                g.drawText(summary, text.removeFromTop(17), juce::Justification::centredLeft, true);

                if (snapshot.rawProjectionVisible)
                {
                    g.setColour(RendererVisualStyle::secondaryText());
                    g.drawText("RAW  " + model->projectedRawText(field),
                               text.removeFromTop(18), juce::Justification::centredLeft, true);
                }
                if (snapshot.provenanceProjectionVisible)
                {
                    g.setColour(RendererVisualStyle::secondaryText());
                    g.drawText("SOURCE  " + model->projectedProvenanceText(field),
                               text.removeFromTop(18), juce::Justification::centredLeft, true);
                }
            }
            area.removeFromTop(8);
        }

        if (searchQuery.isNotEmpty()
            && std::none_of(snapshot.groups.begin(), snapshot.groups.end(),
                            [this](const TypedMetadataGroup& group)
                            {
                                return std::any_of(group.fields.begin(), group.fields.end(),
                                                   [this](const TypedMetadataField& field)
                                                   {
                                                       return matchesSearch(field);
                                                   });
                            }))
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(metadataFont(12.0f, RendererVisualStyle::bodyTextSize()));
            g.drawFittedText("No metadata fields match \"" + searchQuery + "\".",
                             getLocalBounds().reduced(10), juce::Justification::centred, 2);
        }
    }

private:
    [[nodiscard]] bool matchesSearch(const TypedMetadataField& field) const
    {
        if (searchQuery.isEmpty())
            return true;
        const auto haystack = (field.label + " " + field.normalisedValue + " "
                             + field.source + " " + field.nativeKeys.joinIntoString(" ")).toLowerCase();
        return haystack.contains(searchQuery);
    }

    const TypedMetadataInspectorModel* model = nullptr;
    MetadataInspectorDensity density = MetadataInspectorDensity::compact;
    juce::String searchQuery;
    bool showLongValues = false;
};

TypedMetadataInspectorComponent::TypedMetadataInspectorComponent()
    : content(std::make_unique<ContentComponent>())
{
    lookAndFeelChanged();
    searchEditor.setTitle("Search metadata fields");
    searchEditor.setTooltip("Filter by field label, normalized value, source, or native key.");
    searchEditor.onTextChange = [this]
    {
        content->setSearchQuery(searchEditor.getText());
        refreshPresentation();
    };
    addAndMakeVisible(searchEditor);

    copyScopeBox.addItem("All", 1);
    copyScopeBox.addItem("Core", 2);
    copyScopeBox.addItem("Tags", 3);
    copyScopeBox.setSelectedId(1, juce::dontSendNotification);
    copyScopeBox.setTitle("Metadata copy scope");
    copyScopeBox.setTooltip("Choose whether Copy includes all fields, Core Facts, or Embedded Tags.");
    addAndMakeVisible(copyScopeBox);

    copyButton.setTitle("Copy typed metadata");
    copyButton.setTooltip("Copy the selected semantic group with source, confidence, conflict, and write capability.");
    copyButton.onClick = [this]
    {
        if (model != nullptr)
        {
            const auto groupId = copyScopeBox.getSelectedId() == 2 ? juce::String("core")
                               : copyScopeBox.getSelectedId() == 3 ? juce::String("embedded")
                                                                  : juce::String();
            juce::SystemClipboard::copyTextToClipboard(model->copyText(groupId));
        }
    };
    addAndMakeVisible(copyButton);

    longValuesToggle.setClickingTogglesState(true);
    longValuesToggle.setTitle("Expand long metadata values");
    longValuesToggle.setTooltip("Expand long values in the viewport. Copy always retains the full value.");
    longValuesToggle.onClick = [this]
    {
        content->setShowLongValues(longValuesToggle.getToggleState());
        refreshPresentation();
    };
    addAndMakeVisible(longValuesToggle);

    rawToggle.setClickingTogglesState(true);
    rawToggle.setTitle("Show raw metadata values");
    rawToggle.setTooltip("Show exact raw values. The projection provider is created only on request.");
    rawToggle.onClick = [this] { applyProjectionSelection(); };
    addAndMakeVisible(rawToggle);

    provenanceToggle.setClickingTogglesState(true);
    provenanceToggle.setTitle("Show metadata provenance");
    provenanceToggle.setTooltip("Show exact field provenance. The projection provider is created only on request.");
    provenanceToggle.onClick = [this] { applyProjectionSelection(); };
    addAndMakeVisible(provenanceToggle);

    for (auto* toggle : { &longValuesToggle, &rawToggle, &provenanceToggle })
        toggle->getProperties().set("modernCompactToggle", true);

    viewport.setViewedComponent(content.get(), false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible(viewport);

    setTitle("Typed metadata inspector");
    setDescription("Read-only typed document facts with source, confidence, conflicts, and optional raw provenance.");
}

TypedMetadataInspectorComponent::~TypedMetadataInspectorComponent()
{
    viewport.setViewedComponent(nullptr, false);
}

void TypedMetadataInspectorComponent::beginGeneration(std::uint64_t generation)
{
    expectedGeneration = generation;
    content->setModel(nullptr);
    model.reset();
    rawToggle.setToggleState(false, juce::dontSendNotification);
    provenanceToggle.setToggleState(false, juce::dontSendNotification);
    longValuesToggle.setToggleState(false, juce::dontSendNotification);
    searchEditor.clear();
    refreshPresentation();
}

bool TypedMetadataInspectorComponent::activatePostCore(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (generation == 0 || generation != expectedGeneration)
        return false;

    auto candidate = TypedMetadataInspectorModel::createPostCore(generation, info);
    if (candidate == nullptr)
        return false;

    model = std::move(candidate);
    content->setModel(model.get());
    refreshPresentation();
    return true;
}

bool TypedMetadataInspectorComponent::refreshFacts(
    std::uint64_t generation,
    const SingleTrackDocument::FileInfo& info)
{
    if (model == nullptr || !model->updateCoreFacts(generation, info))
        return false;

    refreshPresentation();
    return true;
}

bool TypedMetadataInspectorComponent::reportOptionalProviderFailure(
    std::uint64_t generation,
    juce::String message)
{
    if (model == nullptr
        || !model->applyOptionalProviderFailure(generation, std::move(message)))
    {
        return false;
    }

    refreshPresentation();
    return true;
}

bool TypedMetadataInspectorComponent::isActiveFor(std::uint64_t generation) const noexcept
{
    return model != nullptr
        && model->getSnapshot().active
        && model->getSnapshot().generation == generation;
}

void TypedMetadataInspectorComponent::paint(juce::Graphics& g)
{
    g.fillAll(RendererVisualStyle::panelBackground());
    if (!RendererVisualStyle::isClassic())
    {
        auto heading = getLocalBounds().removeFromTop(24).reduced(2, 0);
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(13.0f, juce::Font::bold));
        const bool narrow = getWidth() < 160;
        g.drawText(narrow ? "Facts" : "Metadata", heading.removeFromLeft(narrow ? 46 : 80),
                   juce::Justification::centredLeft, true);
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(narrow ? 10.0f : 11.0f));
        g.drawText("Read only", heading, juce::Justification::centredRight, true);
        return;
    }
    auto heading = getLocalBounds().removeFromTop(28).reduced(2, 0);
    g.setColour(RendererVisualStyle::primaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::bodyTextSize(), juce::Font::bold)));
    g.drawText("Typed Facts", heading.removeFromLeft(88), juce::Justification::centredLeft, true);

    g.setColour(RendererVisualStyle::secondaryText());
    g.setFont(juce::Font(juce::FontOptions(RendererVisualStyle::captionTextSize(), juce::Font::bold)));
    g.drawText(densityName(density) + "  ·  READ ONLY  ·  EDIT DESIGN LOCKED",
               heading, juce::Justification::centredRight, true);
}

int TypedMetadataInspectorComponent::preferredModernHeight(int width) noexcept
{
    // Two compact fields plus their group heading must still have a viewport
    // after the toolbar wraps. The containing Mag keeps its own scroll/size.
    return MetadataControlsLayout::calculate(width).viewportTop + 168;
}

void TypedMetadataInspectorComponent::lookAndFeelChanged()
{
    const auto classic = RendererVisualStyle::isClassic();
    searchEditor.setTextToShowWhenEmpty(classic || getWidth() >= 160 ? "Search metadata" : "Search",
                                       classic ? RendererVisualStyle::disabledText()
                                               : RendererVisualStyle::secondaryText());
}

void TypedMetadataInspectorComponent::resized()
{
    lookAndFeelChanged();
    density = TypedMetadataInspectorModel::densityForWidth(getWidth());
    content->setDensity(density);

    if (!RendererVisualStyle::isClassic())
    {
        searchEditor.setFont(ModernTypography::font(12.0f));
        searchEditor.setBounds(0, 26, getWidth(), 26);
        const auto layout = MetadataControlsLayout::calculate(getWidth());
        const std::array<juce::Component*, 5> controls { &copyScopeBox, &copyButton, &longValuesToggle,
                                                       &rawToggle, &provenanceToggle };
        for (size_t i = 0; i < controls.size(); ++i) controls[i]->setBounds(layout.actions[i]);
        viewport.setBounds(getLocalBounds().withTrimmedTop(layout.viewportTop));
        content->setSize(juce::jmax(1, viewport.getMaximumVisibleWidth()),
                         content->requiredHeight(juce::jmax(1, viewport.getMaximumVisibleWidth())));
        return;
    }
    auto area = getLocalBounds();
    area.removeFromTop(30);
    searchEditor.setBounds(area.removeFromTop(28));
    area.removeFromTop(4);
    auto controls = area.removeFromTop(30);
    copyScopeBox.setBounds(controls.removeFromLeft(76));
    controls.removeFromLeft(4);
    copyButton.setBounds(controls.removeFromLeft(52));
    controls.removeFromLeft(4);
    longValuesToggle.setBounds(controls.removeFromLeft(56));
    if (density == MetadataInspectorDensity::expanded)
    {
        controls.removeFromLeft(4);
        rawToggle.setBounds(controls.removeFromLeft(56));
        controls.removeFromLeft(4);
        provenanceToggle.setBounds(controls.removeFromLeft(76));
    }
    else
    {
        area.removeFromTop(4);
        auto projectionControls = area.removeFromTop(30);
        rawToggle.setBounds(projectionControls.removeFromLeft(64));
        projectionControls.removeFromLeft(4);
        provenanceToggle.setBounds(projectionControls.removeFromLeft(86));
    }
    area.removeFromTop(5);
    viewport.setBounds(area);

    const auto width = juce::jmax(1, viewport.getMaximumVisibleWidth());
    content->setSize(width, content->requiredHeight(width));
}

void TypedMetadataInspectorComponent::applyProjectionSelection()
{
    if (model == nullptr)
        return;

    model->setProjection(expectedGeneration,
                         rawToggle.getToggleState(),
                         provenanceToggle.getToggleState());
    refreshPresentation();
}

void TypedMetadataInspectorComponent::refreshPresentation()
{
    copyButton.setEnabled(model != nullptr);
    copyScopeBox.setEnabled(model != nullptr);
    searchEditor.setEnabled(model != nullptr);
    longValuesToggle.setEnabled(model != nullptr);
    rawToggle.setEnabled(model != nullptr);
    provenanceToggle.setEnabled(model != nullptr);
    resized();
    repaint();
    content->setTitle("Typed metadata fields");
    content->setDescription(model != nullptr
                                ? model->copyText()
                                : juce::String("Typed metadata attaches after CoreReady."));
    content->repaint();
}
} // namespace singleeditor
