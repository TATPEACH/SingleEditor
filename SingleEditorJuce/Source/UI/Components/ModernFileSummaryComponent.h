#pragma once

#include "Document/SingleTrackDocument.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include <array>

namespace singleeditor
{
// Compact projection of already loaded facts. No reader, artwork extraction,
// filesystem scan or metadata provider is started by this view.
class ModernFileSummaryComponent final : public juce::Component
{
public:
    static juce::String durationText(double seconds)
    {
        const auto ms = static_cast<juce::int64>(std::round(juce::jmax(0.0, seconds) * 1000));
        return juce::String(ms / 60000) + ":" + juce::String((ms / 1000) % 60).paddedLeft('0', 2)
            + "." + juce::String(ms % 1000).paddedLeft('0', 3);
    }

    void setInfo(const SingleTrackDocument::FileInfo& info)
    {
        if (sourceFile != info.file || info.numSamples <= 0) artwork = {};
        sourceFile = info.file;
        fileName = info.file.getFileName();
        hasSource = fileName.isNotEmpty() && info.numSamples > 0;
        values = { info.formatName, juce::String(info.sampleRate / 1000.0, 1) + " kHz",
                   info.bitsPerSample > 0 ? juce::String(info.bitsPerSample) + " bit" : juce::String("Unknown"),
                   juce::String(info.numChannels), durationText(info.durationSeconds), juce::String(info.numSamples) };
        setTitle(hasSource ? fileName : "File information");
        setDescription(hasSource ? info.file.getFullPathName() + "; " + values.joinIntoString("; ") : "Open an audio file to view its information.");
        repaint();
    }

    void setArtwork(const juce::File& source, const juce::Image& image)
    {
        if (!hasSource || source != sourceFile) return;
        artwork = image;
        repaint();
    }

    struct Layout
    {
        juce::Rectangle<int> emblem, title;
        std::array<juce::Rectangle<int>, 6> labels, values;
        float titleEm = 13.0f, bodyEm = 11.0f;
        int height = 0;
        bool compact = false, twoColumns = false;
    };

    static Layout layout(int width, int availableHeight)
    {
        Layout result;
        if (width <= 0) return result;
        // The standard General/DSP sidebar is about 230 px wide. Use its
        // available height before collapsing the cover and facts into a list.
        result.compact = width < 220 || availableHeight < 190;
        result.twoColumns = result.compact && width >= 320;
        const bool narrow = width < 180;
        const int padding = result.compact ? 6 : 8;
        const int contentWidth = juce::jmax(0, width - padding * 2);
        const int emblemSize = juce::jmin(contentWidth, result.compact ? (narrow ? 18 : 24)
                                                                   : juce::jlimit(44, 56, width / 5));
        const int headerHeight = result.compact ? emblemSize : 24;
        const int lineHeight = result.compact ? (narrow ? 15 : 16) : 18;
        result.titleEm = result.compact ? (narrow ? 11.0f : 12.0f) : 13.0f;
        result.bodyEm = result.compact ? 10.5f : 11.0f;
        result.emblem = { padding, padding, emblemSize, emblemSize };
        const int titleX = result.emblem.getRight() + 6;
        result.title = { titleX, padding, juce::jmax(0, width - padding - titleX), headerHeight };
        const int factsX = result.compact ? padding : titleX;
        const int factsWidth = juce::jmax(0, width - padding - factsX);
        const int factsY = padding + headerHeight + (result.compact ? 4 : 0);
        const int columns = result.twoColumns ? 2 : 1;
        const int columnWidth = juce::jmax(0, (factsWidth - (columns - 1) * 12) / columns);
        const int labelWidth = juce::jmin(result.compact ? 50 : 60, columnWidth * 2 / 5);
        for (int index = 0; index < 6; ++index)
        {
            const int column = result.twoColumns ? index / 3 : 0;
            const int row = result.twoColumns ? index % 3 : index;
            const int x = factsX + column * (columnWidth + 12);
            const int y = factsY + row * lineHeight;
            result.labels[static_cast<size_t>(index)] = { x, y, labelWidth, lineHeight };
            const int valueGap = juce::jmin(6, columnWidth - labelWidth);
            result.values[static_cast<size_t>(index)] = { x + labelWidth + valueGap, y,
                columnWidth - labelWidth - valueGap, lineHeight };
        }
        result.height = factsY + (result.twoColumns ? 3 : 6) * lineHeight + padding;
        return result;
    }

    void setAvailableHeight(int height) { availableHeight = juce::jmax(0, height); repaint(); }
    [[nodiscard]] int preferredHeight() const noexcept { return layout(getWidth(), availableHeight).height; }

    void paint(juce::Graphics& g) override
    {
        if (!hasSource)
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(12.0f));
            g.drawFittedText("Open an audio file to view its information.", getLocalBounds().reduced(10),
                            juce::Justification::topLeft, 3);
            return;
        }
        const auto geometry = layout(getWidth(), availableHeight);
        if (artwork.isValid())
        {
            const auto bounds = geometry.emblem.toFloat();
            juce::Graphics::ScopedSaveState saved(g);
            juce::Path mask;
            mask.addRoundedRectangle(bounds, juce::jmin(8.0f, bounds.getWidth() * 0.2f));
            g.reduceClipRegion(mask);
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(artwork, bounds, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
            g.setColour(RendererVisualStyle::secondaryText().withAlpha(0.15f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), juce::jmin(7.5f, bounds.getWidth() * 0.2f), 1.0f);
        }
        else paintFileEmblem(g, geometry.emblem.toFloat());
        g.setColour(RendererVisualStyle::primaryText());
        g.setFont(ModernTypography::font(geometry.titleEm, juce::Font::bold));
        g.drawText(fileName, geometry.title, juce::Justification::centredLeft, true);
        static const char* labels[] { "Format", "Rate", "Depth", "Channels", "Length", "Frames" };
        for (int row = 0; row < 6; ++row)
        {
            g.setColour(RendererVisualStyle::secondaryText());
            g.setFont(ModernTypography::font(geometry.bodyEm));
            const auto label = row == 3 && getWidth() < 180 ? "Ch." : labels[row];
            g.drawText(label, geometry.labels[static_cast<size_t>(row)], juce::Justification::centredLeft, true);
            g.setColour(RendererVisualStyle::primaryText());
            g.drawText(values[row], geometry.values[static_cast<size_t>(row)], juce::Justification::centredLeft, true);
        }
    }

private:
    static void paintFileEmblem(juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        g.setColour(RendererVisualStyle::accentSoft());
        g.fillRoundedRectangle(bounds, juce::jmin(8.0f, bounds.getWidth() * 0.2f));
        auto page = bounds.reduced(bounds.getWidth() * 0.24f, bounds.getHeight() * 0.20f);
        const auto fold = page.getWidth() * 0.3f;
        juce::Path path;
        path.startNewSubPath(page.getX(), page.getY());
        path.lineTo(page.getRight() - fold, page.getY());
        path.lineTo(page.getRight(), page.getY() + fold);
        path.lineTo(page.getRight(), page.getBottom());
        path.lineTo(page.getX(), page.getBottom());
        path.closeSubPath();
        g.setColour(RendererVisualStyle::accentBlue());
        g.strokePath(path, juce::PathStrokeType(1.1f));
        g.drawLine(page.getRight() - fold, page.getY(), page.getRight() - fold, page.getY() + fold, 1.0f);
        g.drawLine(page.getRight() - fold, page.getY() + fold, page.getRight(), page.getY() + fold, 1.0f);
        for (int i = 0; i < 3; ++i)
        {
            const auto y = page.getCentreY() + static_cast<float>(i) * page.getHeight() * 0.13f;
            g.drawLine(page.getX() + page.getWidth() * 0.15f, y,
                       page.getRight() - page.getWidth() * 0.15f, y, 1.0f);
        }
    }
    juce::String fileName;
    juce::File sourceFile;
    juce::Image artwork;
    juce::StringArray values;
    bool hasSource = false;
    int availableHeight = 180;
};
} // namespace singleeditor
