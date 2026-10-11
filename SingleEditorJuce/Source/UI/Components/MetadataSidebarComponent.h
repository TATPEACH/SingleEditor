#pragma once

#include <cstdint>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "Bnk/BnkDocument.h"
#include "Document/SingleTrackDocument.h"
#include "UI/Components/TypedMetadataInspectorComponent.h"
#include "UI/Components/ModernFileSummaryComponent.h"
#include "Metadata/LocalArtworkLoader.h"

namespace singleeditor
{
class MetadataSidebarComponent final : public juce::Component
{
public:
    MetadataSidebarComponent(AudioEngine& engineToUse,
                             SingleTrackDocument& documentToUse,
                             BnkDocument& bnkDocumentToUse);
    ~MetadataSidebarComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void refreshChannelControls();
    void beginTypedMetadataGeneration(std::uint64_t generation);
    void clearTypedMetadataInspector();
    bool activateTypedMetadataInspector(std::uint64_t generation,
                                        const SingleTrackDocument::FileInfo& info);
    bool refreshTypedMetadataFacts(std::uint64_t generation,
                                   const SingleTrackDocument::FileInfo& info);
    void setBnkPreviewContext(juce::String switchLabel,
                              juce::String switchToken,
                              juce::String stateLabel,
                              juce::String rtpcLabel);

private:
    void showOutputMappingDialog();
    void showAmbisonicOrientationDialog();
    void layoutModernInspector();
    void refreshBankInspector();
    void requestBinauralMonitor();
    class BankInspectorContent;
    class BinauralPreparation;

    AudioEngine& engine;
    SingleTrackDocument& document;
    BnkDocument& bnkDocument;
    juce::ComboBox channelOrderBox;
    juce::ComboBox monitorModeBox;
    juce::ComboBox pingRoleBox;
    juce::TextButton pingRoleButton;
    juce::ToggleButton includeLfeToggle;
    juce::TextButton outputMappingButton;
    std::unique_ptr<juce::ComboBox> ambisonicMonitorModeBox;
    std::unique_ptr<juce::TextButton> ambisonicOrientationButton;
    juce::StringArray channelOrderPresetIds;
    bool suppressChannelCallbacks = false;
    bool ambisonicPingTooltipActive = false;
    juce::String currentBnkSwitchLabel;
    juce::String currentBnkSwitchToken;
    juce::String currentBnkStateLabel;
    juce::String currentBnkRtpcLabel;
    std::uint64_t typedMetadataGeneration = 0;
    std::unique_ptr<TypedMetadataInspectorComponent> typedMetadataInspector;
    ModernFileSummaryComponent fileSummary;
    juce::TextButton monitorDisclosure, detailsDisclosure;
    juce::Component modernContent;
    juce::Viewport modernViewport;
    // BNK presentation is created only after an explicit bank open and released
    // when returning to ordinary media. The viewport never owns the content.
    std::unique_ptr<BankInspectorContent> bankInspectorContent;
    std::unique_ptr<juce::Viewport> bankInspectorViewport;
    // Created only by the post-CoreReady activation, destroyed before child views.
    std::unique_ptr<LocalArtworkLoader> artworkLoader;
    SingleTrackDocument::StatePtr pendingBinauralState;
    std::unique_ptr<BinauralPreparation> binauralPreparation;
};
} // namespace singleeditor
