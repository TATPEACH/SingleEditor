#include "UI/LookAndFeel/ClassicLookAndFeel.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererLookAndFeelFactory.h"
#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class RepresentativeClassicSurface final : public juce::Component
{
public:
    RepresentativeClassicSurface()
    {
        play.setButtonText("Play");
        play.setTitle("Play audio");
        play.setDescription("Starts playback of the current document.");
        play.setWantsKeyboardFocus(true);
        play.onClick = [this] { playInvoked = true; };
        addAndMakeVisible(play);

        stop.setButtonText("Stop");
        stop.setTitle("Stop audio");
        stop.setWantsKeyboardFocus(true);
        addAndMakeVisible(stop);

        magTitle.setText("Output Meter", juce::dontSendNotification);
        magTitle.setTitle("Output Meter");
        magTitle.setColour(juce::Label::textColourId, singleeditor::RendererVisualStyle::primaryText());
        addAndMakeVisible(magTitle);
    }

    void paint(juce::Graphics& g) override
    {
        using Style = singleeditor::RendererVisualStyle;
        g.fillAll(Style::windowBackground());
        auto area = getLocalBounds().reduced(6);
        auto shell = area.removeFromTop(42).toFloat();
        g.setColour(Style::titleBarBackground());
        g.fillRect(shell);
        g.setColour(Style::borderColour());
        g.drawRect(shell, 1.0f);

        area.removeFromTop(4);
        auto mag = area.removeFromRight(180).toFloat();
        g.setColour(Style::panelBackground());
        g.fillRect(mag);
        g.setColour(Style::borderColour());
        g.drawRect(mag, 1.0f);
        auto meter = mag.reduced(16.0f, 42.0f);
        g.setColour(Style::mutedSurface());
        g.fillRect(meter);
        g.setColour(Style::meterFill());
        g.fillRect(meter.withTop(meter.getCentreY()));

        area.removeFromRight(4);
        auto waveform = area.toFloat();
        g.setColour(Style::waveformSurface());
        g.fillRect(waveform);
        g.setColour(Style::waveformBlue());
        juce::Path path;
        path.startNewSubPath(waveform.getX(), waveform.getCentreY());
        for (int x = 0; x < static_cast<int>(waveform.getWidth()); x += 8)
            path.lineTo(waveform.getX() + static_cast<float>(x),
                        waveform.getCentreY() + static_cast<float>((x / 8) % 2 == 0 ? -22 : 22));
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }

    void resized() override
    {
        auto header = getLocalBounds().reduced(12).removeFromTop(30);
        play.setBounds(header.removeFromLeft(72));
        header.removeFromLeft(4);
        stop.setBounds(header.removeFromLeft(72));
        magTitle.setBounds(getWidth() - 174, 54, 150, 22);
    }

    juce::TextButton play;
    juce::TextButton stop;
    juce::Label magTitle;
    bool playInvoked = false;
};

singleeditor::RendererResourceAuditSnapshot testRestartOnlySelectionAndIsolation()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::classic);
    require(RendererSelection::select(RendererKind::classic), "Classic can be selected before sealing");
    auto lookAndFeel = createSelectedLookAndFeel();
    require(dynamic_cast<ClassicLookAndFeel*>(lookAndFeel.get()) != nullptr,
            "Classic factory creates only Classic LookAndFeel");
    require(RendererSelection::isSealed(), "Factory seals renderer choice");
    require(!RendererSelection::select(RendererKind::modern),
            "Renderer cannot hot-switch after construction");

    const auto asset = RendererVisualStyle::assetToken(RendererAssetId::play);
    require(asset.fileName[0] == '\0', "Classic uses text/native controls, not Modern SVG assets");
    require(!RendererVisualStyle::isMotionEnabled(), "Classic has no workspace animation slice");
    require(RendererVisualStyle::cardRadius() == 0.0f, "Classic Mag chrome is square and low cost");

    const auto audit = RendererSelection::auditSnapshot();
    require(audit.selected == RendererKind::classic && audit.selectionSealed,
            "Classic audit records sealed selection");
    require(audit.classicRendererObjects == 1 && audit.modernRendererObjects == 0,
            "Classic process constructs no Modern renderer object");
    require(audit.classicTokenSliceAccesses > 0 && audit.modernTokenSliceAccesses == 0,
            "Classic process accesses only Classic token slice");
    require(audit.classicAssetLookups > 0 && audit.modernAssetLookups == 0,
            "Classic asset outcome does not touch Modern asset manifest");
    require(audit.classicMotionStarts == 0 && audit.modernMotionStarts == 0,
            "Classic process starts no renderer animation");
    require(audit.rejectedSelectionChanges == 1, "Rejected hot-switch is auditable");
    return audit;
}

singleeditor::RendererResourceAuditSnapshot testModernIsolationRemainsSymmetric()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::modern);
    auto lookAndFeel = createSelectedLookAndFeel();
    require(dynamic_cast<ModernAppleLookAndFeel*>(lookAndFeel.get()) != nullptr,
            "Modern factory creates only Modern LookAndFeel");
    const auto token = RendererVisualStyle::assetToken(RendererAssetId::play);
    require(std::string_view(token.fileName) == "play.svg", "Modern retains approved asset mapping");
    require(RendererVisualStyle::isMotionEnabled(), "Modern retains bounded post-Core motion policy");

    const auto audit = RendererSelection::auditSnapshot();
    require(audit.modernRendererObjects == 1 && audit.classicRendererObjects == 0,
            "Modern process constructs no Classic renderer object");
    require(audit.modernTokenSliceAccesses > 0 && audit.classicTokenSliceAccesses == 0,
            "Modern process accesses no Classic token slice");
    require(audit.modernAssetLookups > 0 && audit.classicAssetLookups == 0,
            "Modern process accesses no Classic asset slice");
    return audit;
}

void writeResourceReceipt(const std::string& path,
                          const singleeditor::RendererResourceAuditSnapshot& classic,
                          const singleeditor::RendererResourceAuditSnapshot& modern)
{
    std::ofstream receipt(path, std::ios::binary | std::ios::trunc);
    require(receipt.is_open(), "Renderer resource receipt could not be opened");
    const auto writeSnapshot = [&receipt](const char* id,
                                          const singleeditor::RendererResourceAuditSnapshot& audit,
                                          bool trailingComma)
    {
        receipt << "  \"" << id << "\": {\n"
                << "    \"selectionSealed\": " << (audit.selectionSealed ? "true" : "false") << ",\n"
                << "    \"modernRendererObjects\": " << audit.modernRendererObjects << ",\n"
                << "    \"classicRendererObjects\": " << audit.classicRendererObjects << ",\n"
                << "    \"modernTokenSliceAccesses\": " << audit.modernTokenSliceAccesses << ",\n"
                << "    \"classicTokenSliceAccesses\": " << audit.classicTokenSliceAccesses << ",\n"
                << "    \"modernAssetLookups\": " << audit.modernAssetLookups << ",\n"
                << "    \"classicAssetLookups\": " << audit.classicAssetLookups << ",\n"
                << "    \"modernMotionStarts\": " << audit.modernMotionStarts << ",\n"
                << "    \"classicMotionStarts\": " << audit.classicMotionStarts << ",\n"
                << "    \"rejectedSelectionChanges\": " << audit.rejectedSelectionChanges << "\n"
                << "  }" << (trailingComma ? "," : "") << "\n";
    };

    receipt << "{\n"
            << "  \"schema\": \"singleeditor.renderer-resource-audit\",\n"
            << "  \"version\": 1,\n"
            << "  \"status\": \"pass\",\n";
    writeSnapshot("classic", classic, true);
    writeSnapshot("modern", modern, false);
    receipt << "}\n";
    require(receipt.good(), "Renderer resource receipt could not be written");
}

void testRepresentativeClassicShellControlsWaveformAndMag()
{
    using namespace singleeditor;
    RendererSelection::resetForTests(RendererKind::classic);
    auto lookAndFeel = createSelectedLookAndFeel();
    juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());

    RepresentativeClassicSurface surface;
    surface.setLookAndFeel(lookAndFeel.get());
    surface.setSize(720, 420);
    surface.resized();
    surface.play.onClick();
    require(surface.playInvoked, "Classic control preserves shared command outcome");
    require(surface.play.getTitle() == "Play audio" && surface.play.getDescription().isNotEmpty(),
            "Classic representative control retains accessible name and description");
    require(surface.play.getWantsKeyboardFocus() && surface.stop.getWantsKeyboardFocus(),
            "Classic transport remains keyboard-focusable");

    const std::set<juce::uint32> colours {
        RendererVisualStyle::windowBackground().getARGB(),
        RendererVisualStyle::panelBackground().getARGB(),
        RendererVisualStyle::waveformSurface().getARGB(),
        RendererVisualStyle::mutedSurface().getARGB(),
        RendererVisualStyle::meterFill().getARGB(),
        RendererVisualStyle::waveformBlue().getARGB()
    };
    require(colours.size() >= 5, "Classic representative slice has a coherent non-placeholder semantic palette");

    surface.setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI initialiseGui;
        const auto classicAudit = testRestartOnlySelectionAndIsolation();
        const auto modernAudit = testModernIsolationRemainsSymmetric();
        testRepresentativeClassicShellControlsWaveformAndMag();
        const auto receiptPath = juce::SystemStats::getEnvironmentVariable(
            "SINGLEEDITOR_RENDERER_AUDIT_RECEIPT", {});
        if (receiptPath.isNotEmpty())
            writeResourceReceipt(receiptPath.toStdString(), classicAudit, modernAudit);
        std::cout << "Classic renderer tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Classic renderer tests failed: " << error.what() << '\n';
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        return EXIT_FAILURE;
    }
}
