#include "TimelineMarkersComponent.h"
#include "ModernFileSummaryComponent.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <limits>

namespace singleeditor
{
namespace
{
std::optional<int64_t> nonNegativeInteger(const juce::String& text)
{
    const auto bytes = text.trim().toStdString();
    int64_t value = 0;
    const auto parsed = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (bytes.empty() || parsed.ec != std::errc() || parsed.ptr != bytes.data() + bytes.size() || value < 0)
        return std::nullopt;
    return value;
}
}

TimelineMarkersComponent::TimelineMarkersComponent()
{
    setTitle("Markers");
    getProperties().set("workspace.headerHeight", ModernPanelPainter::headerHeight);
    list.setRowHeight(28);
    list.setMultipleSelectionEnabled(false);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness(0);
    list.setHasFocusOutline(!RendererVisualStyle::isClassic());
    list.setTooltip("Click a marker or press Enter to move the playback cursor.");
    addAndMakeVisible(list);
}

TimelineMarkersComponent::~TimelineMarkersComponent() { list.setModel(nullptr); }

void TimelineMarkersComponent::setDocument(SingleTrackDocument::StatePtr next)
{
    if (source == next) return;
    source = std::move(next);
    cues.clear();
    unreadableCues = false;
    selection.reset();
    playhead = 0.0;
    available = source != nullptr && source->info.numSamples > 0
        && std::isfinite(source->info.sampleRate) && source->info.sampleRate > 0.0;
    duration = available ? static_cast<double>(source->info.numSamples) / source->info.sampleRate : 0.0;
    if (!std::isfinite(duration) || duration > static_cast<double>(std::numeric_limits<int64_t>::max() / 1000 - 1))
    {
        duration = 0.0;
        available = false;
    }
    if (available)
    {
        // Build one indexed copy instead of repeatedly searching StringPairArray
        // for every label. Work is bounded by facts already supplied by the reader.
        std::map<juce::String, juce::String> facts;
        const auto& metadata = source->info.metadata;
        for (int i = 0; i < metadata.size(); ++i)
            if (metadata.getAllKeys()[i].startsWith("Cue") || metadata.getAllKeys()[i].startsWith("NumCue"))
                facts.emplace(metadata.getAllKeys()[i], metadata.getAllValues()[i]);
        const auto value = [&facts](const juce::String& key, juce::String fallback = {})
        {
            const auto found = facts.find(key);
            return found != facts.end() ? found->second : fallback;
        };
        const auto count = nonNegativeInteger(value("NumCuePoints", "0"));
        std::map<int64_t, juce::String> labels;
        for (const auto& [key, text] : facts)
            if (key.startsWith("CueLabel") && key.endsWith("Identifier"))
                if (const auto id = nonNegativeInteger(text))
                    labels.emplace(*id, value(key.dropLastCharacters(10) + "Text").trim());
        if (!count || *count > 100000)
            unreadableCues = true;
        else
        {
            int64_t seen = 0;
            for (const auto& [key, text] : facts)
            {
                if (!key.startsWith("Cue") || !key.endsWith("Offset")) continue;
                const auto index = nonNegativeInteger(key.substring(3, key.length() - 6));
                if (!index || *index >= *count) continue;
                ++seen;
                const auto prefix = "Cue" + juce::String(*index);
                const auto offset = nonNegativeInteger(text);
                const auto id = nonNegativeInteger(value(prefix + "Identifier", juce::String(*index)));
                const auto chunkStart = nonNegativeInteger(value(prefix + "ChunkStart", "0"));
                const auto blockStart = nonNegativeInteger(value(prefix + "BlockStart", "0"));
                if (!offset || !id || !chunkStart || !blockStart || *chunkStart != 0 || *blockStart != 0
                    || *offset > source->info.numSamples)
                {
                    unreadableCues = true;
                    continue;
                }
                const auto named = labels.find(*id);
                auto label = named != labels.end() ? named->second : juce::String();
                if (label.isEmpty()) label = "Marker " + juce::String(*index + 1);
                cues.push_back({ "M", label, static_cast<double>(*offset) / source->info.sampleRate, true, false });
            }
            unreadableCues = unreadableCues || seen != *count;
            std::stable_sort(cues.begin(), cues.end(), [](const Row& a, const Row& b) { return a.seconds < b.seconds; });
        }
    }
    list.deselectAllRows();
    list.updateContent();
    setDescription(unreadableCues ? "Some file markers are unavailable. Timeline references remain visible."
                                 : "File markers, selection boundaries and the playback cursor.");
    resized();
    repaint();
}

void TimelineMarkersComponent::setTransport(double position, std::optional<juce::Range<double>> range)
{
    const auto nextPosition = std::isfinite(position) ? juce::jlimit(0.0, duration, position) : 0.0;
    if (!available || !range || !std::isfinite(range->getStart()) || !std::isfinite(range->getEnd())
        || range->getLength() <= 0.0)
        range.reset();
    else
    {
        range = range->getIntersectionWith({ 0.0, duration });
        if (range->isEmpty()) range.reset();
    }
    if (selection != range)
    {
        selection = range;
        list.repaintRow(static_cast<int>(cues.size()) + 1);
        list.repaintRow(static_cast<int>(cues.size()) + 3);
    }
    if (std::llround(playhead * 1000) != std::llround(nextPosition * 1000))
    {
        playhead = nextPosition;
        list.repaintRow(static_cast<int>(cues.size()) + 2);
    }
}

int TimelineMarkersComponent::getNumRows() { return static_cast<int>(cues.size()) + 5; }

TimelineMarkersComponent::Row TimelineMarkersComponent::rowAt(int index) const
{
    if (index == 0) return { "A", "Start", 0.0, available, false };
    if (index > 0 && index <= static_cast<int>(cues.size())) return cues[static_cast<size_t>(index - 1)];
    switch (index - static_cast<int>(cues.size()))
    {
        case 1: return { "S", "Selection start", selection ? selection->getStart() : 0.0, selection.has_value(), true };
        case 2: return { "P", "Playhead", playhead, available, false };
        case 3: return { "E", "Selection end", selection ? selection->getEnd() : 0.0, selection.has_value(), true };
        case 4: return { "Z", "End", duration, available, false };
        default: return {};
    }
}

juce::String TimelineMarkersComponent::getNameForRow(int index)
{
    const auto row = rowAt(index);
    return row.label + ": " + (row.available ? ModernFileSummaryComponent::durationText(row.seconds) : "unavailable");
}

void TimelineMarkersComponent::paintListBoxItem(int index, juce::Graphics& g, int width, int height, bool selected)
{
    const auto row = rowAt(index);
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(3, 2);
    if ((selected || row.selectionBoundary) && row.available)
    {
        g.setColour(RendererVisualStyle::accentSoft().withAlpha(selected ? 0.9f : 0.55f));
        g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    }
    bounds = bounds.reduced(5, 0);
    const auto badge = bounds.removeFromLeft(18).withSizeKeepingCentre(16, 17);
    g.setColour(RendererVisualStyle::accentSoft().withAlpha(row.available ? 0.9f : 0.3f));
    g.fillRoundedRectangle(badge.toFloat(), 3.0f);
    g.setFont(ModernTypography::font(10.0f, juce::Font::bold));
    g.setColour(row.available ? RendererVisualStyle::accentBlue() : RendererVisualStyle::secondaryText());
    g.drawText(row.badge, badge, juce::Justification::centred);
    bounds.removeFromLeft(6);
    const auto timeText = row.available ? ModernFileSummaryComponent::durationText(row.seconds) : juce::String("--");
    const auto timeFont = ModernTypography::font(11.0f);
    const auto clockWidth = juce::GlyphArrangement::getStringWidthInt(timeFont, timeText) + 2;
    const auto clock = bounds.removeFromRight(juce::jmin(clockWidth, bounds.getWidth() * 3 / 5));
    bounds.removeFromRight(5);
    g.setFont(timeFont);
    g.setColour(RendererVisualStyle::secondaryText());
    g.drawText(timeText, clock,
               juce::Justification::centredRight, true);
    g.setColour(row.available ? RendererVisualStyle::primaryText() : RendererVisualStyle::secondaryText());
    g.setFont(ModernTypography::font(12.0f));
    auto label = row.label;
    if (width < 220 && row.selectionBoundary) label = row.badge == "S" ? "Sel. start" : "Sel. end";
    if (width < 160)
    {
        if (row.selectionBoundary) label = row.badge == "S" ? "Start" : "End";
        else if (row.badge == "P") label = "Cursor";
    }
    g.drawText(label, bounds, juce::Justification::centredLeft, true);
}

void TimelineMarkersComponent::activateRow(int index)
{
    const auto row = rowAt(index);
    if (row.available && onSeek) onSeek(row.seconds);
}
void TimelineMarkersComponent::listBoxItemClicked(int row, const juce::MouseEvent&) { activateRow(row); }
void TimelineMarkersComponent::returnKeyPressed(int row) { activateRow(row); }

void TimelineMarkersComponent::paint(juce::Graphics& g)
{
    ModernPanelPainter::paint(g, getLocalBounds(), "Markers", ModernPanelPainter::Icon::markers);
    if (unreadableCues)
    {
        g.setColour(RendererVisualStyle::secondaryText());
        g.setFont(ModernTypography::font(10.5f));
        g.drawText("Some file markers are unavailable", getLocalBounds().removeFromBottom(20).reduced(8, 0),
                   juce::Justification::centredLeft, true);
    }
}

void TimelineMarkersComponent::resized()
{
    list.setBounds(getLocalBounds().withTrimmedTop(ModernPanelPainter::headerHeight)
                   .withTrimmedBottom(unreadableCues ? 20 : 3).reduced(4, 2));
}
}
