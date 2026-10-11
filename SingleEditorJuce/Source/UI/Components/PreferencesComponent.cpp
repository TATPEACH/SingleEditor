#include "UI/Components/PreferencesComponent.h"

#include <array>
#include <cmath>
#include <limits>

#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/Renderer/RendererSelection.h"
#include "UI/Renderer/RendererVisualStyle.h"

namespace singleeditor
{
namespace
{
void configureLabel(juce::Label& label,
                    const juce::String& text,
                    float size,
                    juce::Colour colour,
                    juce::Justification justification = juce::Justification::centredLeft)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions(size)));
    label.setColour(juce::Label::textColourId, colour);
    ModernAppleLookAndFeel::setLabelRole(label, colour == ModernAppleLookAndFeel::primaryText()
        ? ModernAppleLookAndFeel::LabelRole::primary : ModernAppleLookAndFeel::LabelRole::secondary);
    label.setJustificationType(justification);
}

void configureComboBox(juce::ComboBox& comboBox, const juce::String& emptyText)
{
    comboBox.setJustificationType(juce::Justification::centredLeft);
    comboBox.setTextWhenNothingSelected(emptyText);
}

juce::String presetIdForComboSelection(int channelCount, const juce::ComboBox& comboBox)
{
    const auto selectedId = comboBox.getSelectedId();
    if (selectedId <= 1)
        return {};

    auto itemId = 2;
    for (const auto& preset : ChannelLayoutCatalog::candidatesForChannelCount(channelCount))
    {
        if (preset.id.startsWith("discrete-"))
            continue;

        if (selectedId == itemId)
            return preset.id;

        ++itemId;
    }

    return {};
}

void populateDefaultChannelOrderBox(juce::ComboBox& comboBox, int channelCount, const juce::String& selectedPresetId)
{
    comboBox.clear(juce::dontSendNotification);
    comboBox.addItem("Project default (Game / Wwise / Dolby)", 1);

    auto itemId = 2;
    auto selectedId = 1;
    for (const auto& preset : ChannelLayoutCatalog::candidatesForChannelCount(channelCount))
    {
        if (preset.id.startsWith("discrete-"))
            continue;

        comboBox.addItem(preset.displayName, itemId);
        if (preset.id == selectedPresetId)
            selectedId = itemId;
        ++itemId;
    }

    comboBox.setSelectedId(selectedId, juce::dontSendNotification);
}

int countEnabledChannels(const juce::BigInteger& channels, int channelLimit) noexcept
{
    auto count = 0;
    for (int channel = 0; channel < channelLimit; ++channel)
    {
        if (channels[channel])
            ++count;
    }
    return count;
}
} // namespace

void PreferencesComponent::applyStoredChannelOrderDefaults(juce::ApplicationProperties& appProperties)
{
    if (auto* settings = appProperties.getUserSettings())
    {
        ChannelLayoutCatalog::setPreferredDefaultPresetId(4, settings->getValue(defaultChannelOrder4chKey));
        ChannelLayoutCatalog::setPreferredDefaultPresetId(6, settings->getValue(defaultChannelOrder6chKey));
        ChannelLayoutCatalog::setPreferredDefaultPresetId(8, settings->getValue(defaultChannelOrder8chKey));
        ChannelLayoutCatalog::setPreferredDefaultPresetId(12, settings->getValue(defaultChannelOrder12chKey));
    }
}

void PreferencesComponent::applyStoredVisualPreferences(juce::ApplicationProperties& appProperties)
{
    const auto reduced = appProperties.getUserSettings() != nullptr
                      && appProperties.getUserSettings()->getBoolValue(reduceMotionKey, false);
    RendererVisualStyle::setReduceMotion(reduced);
    const auto theme = appProperties.getUserSettings() != nullptr
        ? appProperties.getUserSettings()->getValue(modernThemeKey, "light") : juce::String("light");
    RendererVisualStyle::setModernTheme(theme == "night" ? ModernTheme::night : ModernTheme::light);
    const auto material = appProperties.getUserSettings() != nullptr
        ? appProperties.getUserSettings()->getValue(modernMaterialKey, "frosted") : juce::String("frosted");
    RendererVisualStyle::setGlassMaterial(material == "solid" ? ModernGlassMaterial::solid
        : material == "liquid" ? ModernGlassMaterial::liquid : ModernGlassMaterial::frosted);
}

PreferencesComponent::PreferencesComponent(AudioEngine& audioEngineToUse,
                                           juce::ApplicationProperties& appPropertiesToUse,
                                           std::function<void()> openAsioSettings)
    : audioEngine(audioEngineToUse),
      appProperties(appPropertiesToUse)
{
    setSize(920, 1260);
    setOpaque(true);

    addAndMakeVisible(titleLabel);
    configureLabel(titleLabel, "Preferences", 22.0f, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(subtitleLabel);
    configureLabel(subtitleLabel,
                   "Audio output, close behavior, and waveform cache stay lightweight and on-demand.",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(behaviourCard);
    addAndMakeVisible(behaviourLabel);
    configureLabel(behaviourLabel, "Window Close Behavior", 14.5f, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(behaviourHintLabel);
    configureLabel(behaviourHintLabel, {}, 12.5f, ModernAppleLookAndFeel::secondaryText(), juce::Justification::topLeft);

    addAndMakeVisible(closeBehaviourLabel);
    configureLabel(closeBehaviourLabel,
                   "Close button action",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(closeBehaviourBox);
    closeBehaviourBox.addItem("Exit application on close (recommended)", 1);
    closeBehaviourBox.addItem("Hide to system tray on close", 2);
    if (auto* settings = appProperties.getUserSettings())
        closeBehaviourBox.setSelectedId(settings->getBoolValue(closeToTrayOnCloseKey, false) ? 2 : 1,
                                        juce::dontSendNotification);
    else
        closeBehaviourBox.setSelectedId(1, juce::dontSendNotification);
    closeBehaviourBox.onChange = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue(closeToTrayOnCloseKey, closeBehaviourBox.getSelectedId() == 2);
            settings->saveIfNeeded();
        }

        refreshCloseBehaviour();
    };

    addAndMakeVisible(rendererModeLabel);
    configureLabel(rendererModeLabel,
                   "Renderer (applies after restart)",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(rendererModeBox);
    rendererModeBox.addItem(juce::String::fromUTF8("Modern \xe2\x80\x94 release renderer"), 1);
    rendererModeBox.addItem(juce::String::fromUTF8("Classic \xe2\x80\x94 engineering preview"), 2);
    const auto storedRenderer = appProperties.getUserSettings() != nullptr
        ? RendererSelection::parse(appProperties.getUserSettings()->getValue(rendererModeKey,
                                                                              "modern").toStdString())
        : RendererKind::modern;
    rendererModeBox.setSelectedId(storedRenderer == RendererKind::classic ? 2 : 1,
                                  juce::dontSendNotification);
    rendererModeBox.onChange = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            const auto selected = rendererModeBox.getSelectedId() == 2
                ? RendererKind::classic
                : RendererKind::modern;
            settings->setValue(rendererModeKey,
                               juce::String(RendererSelection::id(selected).data()));
            settings->saveIfNeeded();
        }
    };

    addAndMakeVisible(rendererModeHintLabel);
    configureLabel(rendererModeHintLabel,
                   "The active renderer is sealed for this process. Changing this setting never hot-loads the other renderer.",
                   11.5f,
                   ModernAppleLookAndFeel::secondaryText(),
                   juce::Justification::topLeft);

    addAndMakeVisible(reduceMotionLabel);
    configureLabel(reduceMotionLabel,
                   "Workspace motion",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(reduceMotionBox);
    reduceMotionBox.addItem(juce::String::fromUTF8("Standard \xe2\x80\x94 bounded 140 ms transitions"), 1);
    reduceMotionBox.addItem(juce::String::fromUTF8("Reduce Motion \xe2\x80\x94 direct state changes"), 2);
    if (auto* settings = appProperties.getUserSettings())
        reduceMotionBox.setSelectedId(settings->getBoolValue(reduceMotionKey, false) ? 2 : 1,
                                      juce::dontSendNotification);
    else
        reduceMotionBox.setSelectedId(1, juce::dontSendNotification);
    reduceMotionBox.onChange = [this]
    {
        const auto reduced = reduceMotionBox.getSelectedId() == 2;
        RendererVisualStyle::setReduceMotion(reduced);
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue(reduceMotionKey, reduced);
            settings->saveIfNeeded();
        }
    };

    addAndMakeVisible(restorePlaylistSessionLabel);
    configureLabel(restorePlaylistSessionLabel,
                   "Restore previous playlist session",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(restorePlaylistSessionBox);
    restorePlaylistSessionBox.addItem("Off (recommended)", 1);
    restorePlaylistSessionBox.addItem("Restore last playlist on launch", 2);
    if (auto* settings = appProperties.getUserSettings())
        restorePlaylistSessionBox.setSelectedId(settings->getBoolValue(restorePlaylistSessionKey, false) ? 2 : 1,
                                                juce::dontSendNotification);
    else
        restorePlaylistSessionBox.setSelectedId(1, juce::dontSendNotification);
    restorePlaylistSessionBox.onChange = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue(restorePlaylistSessionKey, restorePlaylistSessionBox.getSelectedId() == 2);
            settings->saveIfNeeded();
        }
    };

    addAndMakeVisible(retainPlaylistAudioLabel);
    configureLabel(retainPlaylistAudioLabel,
                   "Playlist audio memory retention",
                   13.0f,
                   ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(retainPlaylistAudioBox);
    retainPlaylistAudioBox.addItem("Keep audio items loaded while switching", 1);
    retainPlaylistAudioBox.addItem("Unload inactive audio to save memory", 2);
    if (auto* settings = appProperties.getUserSettings())
        retainPlaylistAudioBox.setSelectedId(settings->getBoolValue(retainPlaylistAudioKey, true) ? 1 : 2,
                                             juce::dontSendNotification);
    else
        retainPlaylistAudioBox.setSelectedId(1, juce::dontSendNotification);
    retainPlaylistAudioBox.onChange = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue(retainPlaylistAudioKey, retainPlaylistAudioBox.getSelectedId() == 1);
            settings->saveIfNeeded();
        }
    };

    addAndMakeVisible(bnkPlaybackModeLabel);
    configureLabel(bnkPlaybackModeLabel, "BNK / WEM playback mode", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(bnkPlaybackModeBox);
    bnkPlaybackModeBox.addItem("Decode packed media (recommended)", 1);
    bnkPlaybackModeBox.addItem("Prefer source media when available", 2);
    if (auto* settings = appProperties.getUserSettings())
        bnkPlaybackModeBox.setSelectedId(settings->getIntValue(bnkPlaybackModeKey, 1), juce::dontSendNotification);
    else
        bnkPlaybackModeBox.setSelectedId(1, juce::dontSendNotification);
    bnkPlaybackModeBox.onChange = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue(bnkPlaybackModeKey, bnkPlaybackModeBox.getSelectedId());
            settings->saveIfNeeded();
        }
    };

    addAndMakeVisible(wwiseProjectRootLabel);
    configureLabel(wwiseProjectRootLabel, "Default Wwise project root", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(wwiseProjectRootPathLabel);
    configureLabel(wwiseProjectRootPathLabel, {}, 12.5f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(chooseWwiseProjectRootButton);
    chooseWwiseProjectRootButton.setButtonText("Choose...");
    chooseWwiseProjectRootButton.onClick = [this]
    {
        wwiseProjectRootChooser = std::make_unique<juce::FileChooser>("Select default Wwise project root",
                                                                      juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                                      "*");
        wwiseProjectRootChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                             [this](const juce::FileChooser& fileChooser)
                                             {
                                                 const auto chosen = fileChooser.getResult();
                                                 wwiseProjectRootChooser.reset();
                                                 if (chosen.isDirectory())
                                                 {
                                                     if (auto* settings = appProperties.getUserSettings())
                                                     {
                                                         settings->setValue(defaultWwiseProjectRootKey, chosen.getFullPathName());
                                                         settings->saveIfNeeded();
                                                     }
                                                     refreshWwiseProjectPath();
                                                 }
                                             });
    };

    addAndMakeVisible(clearWwiseProjectRootButton);
    clearWwiseProjectRootButton.setButtonText("Clear");
    clearWwiseProjectRootButton.onClick = [this]
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            settings->removeValue(defaultWwiseProjectRootKey);
            settings->saveIfNeeded();
        }
        refreshWwiseProjectPath();
    };

    addAndMakeVisible(channelOrderCard);
    addAndMakeVisible(channelOrderLabel);
    configureLabel(channelOrderLabel, "Default Source Channel Order", 14.5f, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(channelOrderHintLabel);
    configureLabel(channelOrderHintLabel,
                   "Used when a multichannel file has no explicit layout metadata. This affects newly opened files only.",
                   12.5f,
                   ModernAppleLookAndFeel::secondaryText(),
                   juce::Justification::topLeft);

    addAndMakeVisible(quadDefaultOrderLabel);
    configureLabel(quadDefaultOrderLabel, "Quad / 4ch", 13.0f, ModernAppleLookAndFeel::secondaryText());
    addAndMakeVisible(quadDefaultOrderBox);
    configureComboBox(quadDefaultOrderBox, "Project default");
    quadDefaultOrderBox.onChange = [this] { applySelectedDefaultChannelOrder(4, quadDefaultOrderBox, defaultChannelOrder4chKey); };

    addAndMakeVisible(fiveOneDefaultOrderLabel);
    configureLabel(fiveOneDefaultOrderLabel, "5.1 / 6ch", 13.0f, ModernAppleLookAndFeel::secondaryText());
    addAndMakeVisible(fiveOneDefaultOrderBox);
    configureComboBox(fiveOneDefaultOrderBox, "Project default");
    fiveOneDefaultOrderBox.onChange = [this] { applySelectedDefaultChannelOrder(6, fiveOneDefaultOrderBox, defaultChannelOrder6chKey); };

    addAndMakeVisible(sevenOneDefaultOrderLabel);
    configureLabel(sevenOneDefaultOrderLabel, "7.1 / 8ch", 13.0f, ModernAppleLookAndFeel::secondaryText());
    addAndMakeVisible(sevenOneDefaultOrderBox);
    configureComboBox(sevenOneDefaultOrderBox, "Project default");
    sevenOneDefaultOrderBox.onChange = [this] { applySelectedDefaultChannelOrder(8, sevenOneDefaultOrderBox, defaultChannelOrder8chKey); };

    addAndMakeVisible(sevenOneFourDefaultOrderLabel);
    configureLabel(sevenOneFourDefaultOrderLabel, "7.1.4 / 12ch", 13.0f, ModernAppleLookAndFeel::secondaryText());
    addAndMakeVisible(sevenOneFourDefaultOrderBox);
    configureComboBox(sevenOneFourDefaultOrderBox, "Project default");
    sevenOneFourDefaultOrderBox.onChange = [this] { applySelectedDefaultChannelOrder(12, sevenOneFourDefaultOrderBox, defaultChannelOrder12chKey); };

    addAndMakeVisible(deviceCard);
    addAndMakeVisible(deviceLabel);
    configureLabel(deviceLabel, "Audio Device", 14.5f, ModernAppleLookAndFeel::primaryText());
    if (openAsioSettings)
    {
        addAndMakeVisible(asioSettingsButton);
        asioSettingsButton.setComponentID("audio-device.asio-settings");
        asioSettingsButton.onClick = std::move(openAsioSettings);
    }

    addAndMakeVisible(deviceTypeLabel);
    configureLabel(deviceTypeLabel, "Driver", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(deviceTypeBox);
    configureComboBox(deviceTypeBox, "System default");
    deviceTypeBox.setComponentID("audio-device.type");
    deviceTypeBox.onChange = [this] { applySelectedDeviceType(); };

    addAndMakeVisible(outputDeviceLabel);
    configureLabel(outputDeviceLabel, "Output device", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(outputDeviceBox);
    configureComboBox(outputDeviceBox, "Default output");
    outputDeviceBox.setComponentID("audio-device.output");
    outputDeviceBox.onChange = [this] { applySelectedDeviceSetup(); };

    addAndMakeVisible(sampleRateLabel);
    configureLabel(sampleRateLabel, "Sample rate", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(sampleRateBox);
    configureComboBox(sampleRateBox, "Automatic");
    sampleRateBox.setComponentID("audio-device.rate");
    sampleRateBox.onChange = [this] { applySelectedDeviceSetup(); };

    addAndMakeVisible(bufferSizeLabel);
    configureLabel(bufferSizeLabel, "Buffer size", 13.0f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(bufferSizeBox);
    configureComboBox(bufferSizeBox, "Automatic");
    bufferSizeBox.setComponentID("audio-device.buffer");
    bufferSizeBox.onChange = [this] { applySelectedDeviceSetup(); };

    addAndMakeVisible(deviceStatusLabel);
    configureLabel(deviceStatusLabel,
                   {},
                   12.0f,
                   ModernAppleLookAndFeel::secondaryText(),
                   juce::Justification::topLeft);

    addAndMakeVisible(refreshDevicesButton);
    refreshDevicesButton.setButtonText("Refresh");
    refreshDevicesButton.onClick = [this] { refreshDeviceControls(); };

    addAndMakeVisible(cacheCard);
    addAndMakeVisible(cacheLabel);
    configureLabel(cacheLabel, "Waveform Cache", 14.5f, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(cachePathLabel);
    configureLabel(cachePathLabel, {}, 12.5f, ModernAppleLookAndFeel::secondaryText());

    addAndMakeVisible(cacheStatsLabel);
    configureLabel(cacheStatsLabel, {}, 13.0f, ModernAppleLookAndFeel::primaryText());

    addAndMakeVisible(cacheHintLabel);
    configureLabel(cacheHintLabel,
                   "This cache stores waveform peaks and, when requested, spectral previews. It speeds repeated visual loading, not full audio decode.",
                   12.5f,
                   ModernAppleLookAndFeel::secondaryText(),
                   juce::Justification::topLeft);

    addAndMakeVisible(openCacheFolderButton);
    openCacheFolderButton.setButtonText("Open Folder");
    openCacheFolderButton.onClick = []
    {
        WaveformModel::getPersistentCacheDirectory().revealToUser();
    };

    addAndMakeVisible(clearCacheButton);
    clearCacheButton.setButtonText("Clear Cache");
    clearCacheButton.getProperties().set("accent", true);
    clearCacheButton.onClick = [this]
    {
        WaveformModel::clearPersistentCache();
        refreshCacheInfo();
    };

    refreshCloseBehaviour();
    refreshCacheInfo();
    refreshWwiseProjectPath();
    refreshChannelOrderDefaults();
    refreshDeviceControls();
}

void PreferencesComponent::paint(juce::Graphics& g)
{
    g.fillAll(ModernAppleLookAndFeel::windowBackground());
}

void PreferencesComponent::resized()
{
    auto bounds = getLocalBounds().reduced(20);

    titleLabel.setBounds(bounds.removeFromTop(28));
    bounds.removeFromTop(4);
    subtitleLabel.setBounds(bounds.removeFromTop(22));
    bounds.removeFromTop(14);

    auto behaviourAreaBounds = bounds.removeFromTop(480);
    behaviourCard.setBounds(behaviourAreaBounds);
    auto behaviourArea = behaviourAreaBounds.reduced(16);
    behaviourLabel.setBounds(behaviourArea.removeFromTop(24));
    behaviourArea.removeFromTop(10);
    closeBehaviourLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    closeBehaviourBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(12);
    rendererModeLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    rendererModeBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(4);
    rendererModeHintLabel.setBounds(behaviourArea.removeFromTop(30));
    behaviourArea.removeFromTop(10);
    reduceMotionLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    reduceMotionBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(12);
    restorePlaylistSessionLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    restorePlaylistSessionBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(12);
    retainPlaylistAudioLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(10);
    retainPlaylistAudioBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(12);
    bnkPlaybackModeLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    bnkPlaybackModeBox.setBounds(behaviourArea.removeFromTop(30).removeFromLeft(360));
    behaviourArea.removeFromTop(10);
    wwiseProjectRootLabel.setBounds(behaviourArea.removeFromTop(18));
    behaviourArea.removeFromTop(6);
    auto projectRow = behaviourArea.removeFromTop(30);
    chooseWwiseProjectRootButton.setBounds(projectRow.removeFromRight(92));
    projectRow.removeFromRight(8);
    clearWwiseProjectRootButton.setBounds(projectRow.removeFromRight(72));
    wwiseProjectRootPathLabel.setBounds(projectRow);
    behaviourArea.removeFromTop(10);
    behaviourHintLabel.setBounds(behaviourArea.removeFromTop(92));

    bounds.removeFromTop(14);
    auto channelOrderAreaBounds = bounds.removeFromTop(200);
    channelOrderCard.setBounds(channelOrderAreaBounds);
    auto channelOrderArea = channelOrderAreaBounds.reduced(16);
    channelOrderLabel.setBounds(channelOrderArea.removeFromTop(24));
    channelOrderArea.removeFromTop(10);
    auto orderRowA = channelOrderArea.removeFromTop(30);
    auto quadRow = orderRowA.removeFromLeft((orderRowA.getWidth() / 2) - 8);
    orderRowA.removeFromLeft(16);
    auto fiveOneRow = orderRowA;
    quadDefaultOrderLabel.setBounds(quadRow.removeFromLeft(92));
    quadDefaultOrderBox.setBounds(quadRow);
    fiveOneDefaultOrderLabel.setBounds(fiveOneRow.removeFromLeft(92));
    fiveOneDefaultOrderBox.setBounds(fiveOneRow);
    channelOrderArea.removeFromTop(10);
    auto orderRowB = channelOrderArea.removeFromTop(30);
    auto sevenOneRow = orderRowB.removeFromLeft((orderRowB.getWidth() / 2) - 8);
    orderRowB.removeFromLeft(16);
    auto sevenOneFourRow = orderRowB;
    sevenOneDefaultOrderLabel.setBounds(sevenOneRow.removeFromLeft(92));
    sevenOneDefaultOrderBox.setBounds(sevenOneRow);
    sevenOneFourDefaultOrderLabel.setBounds(sevenOneFourRow.removeFromLeft(92));
    sevenOneFourDefaultOrderBox.setBounds(sevenOneFourRow);
    channelOrderArea.removeFromTop(12);
    channelOrderHintLabel.setBounds(channelOrderArea.removeFromTop(42));

    bounds.removeFromTop(14);
    auto topHalf = bounds.removeFromTop(236);
    deviceCard.setBounds(topHalf);
    auto deviceArea = topHalf.reduced(16);
    auto deviceTitle = deviceArea.removeFromTop(24);
    if (asioSettingsButton.isVisible()) asioSettingsButton.setBounds(deviceTitle.removeFromRight(132));
    deviceLabel.setBounds(deviceTitle);
    deviceArea.removeFromTop(10);
    auto driverRow = deviceArea.removeFromTop(30);
    deviceTypeLabel.setBounds(driverRow.removeFromLeft(92));
    refreshDevicesButton.setBounds(driverRow.removeFromRight(88));
    driverRow.removeFromRight(10);
    deviceTypeBox.setBounds(driverRow);
    deviceArea.removeFromTop(10);
    auto outputRow = deviceArea.removeFromTop(30);
    outputDeviceLabel.setBounds(outputRow.removeFromLeft(92));
    outputDeviceBox.setBounds(outputRow);
    deviceArea.removeFromTop(10);
    auto detailRow = deviceArea.removeFromTop(30);
    auto sampleRateRow = detailRow.removeFromLeft((detailRow.getWidth() / 2) - 6);
    detailRow.removeFromLeft(12);
    auto bufferSizeRow = detailRow;
    sampleRateLabel.setBounds(sampleRateRow.removeFromLeft(92));
    sampleRateBox.setBounds(sampleRateRow);
    bufferSizeLabel.setBounds(bufferSizeRow.removeFromLeft(86));
    bufferSizeBox.setBounds(bufferSizeRow);
    deviceArea.removeFromTop(12);
    deviceStatusLabel.setBounds(deviceArea.removeFromTop(56));

    bounds.removeFromTop(14);
    cacheCard.setBounds(bounds);
    auto cacheArea = bounds.reduced(16);
    cacheLabel.setBounds(cacheArea.removeFromTop(24));
    cacheArea.removeFromTop(8);
    cachePathLabel.setBounds(cacheArea.removeFromTop(40));
    cacheStatsLabel.setBounds(cacheArea.removeFromTop(24));
    cacheArea.removeFromTop(8);
    cacheHintLabel.setBounds(cacheArea.removeFromTop(42));
    cacheArea.removeFromTop(12);

    auto buttons = cacheArea.removeFromTop(34);
    openCacheFolderButton.setBounds(buttons.removeFromLeft(120));
    buttons.removeFromLeft(10);
    clearCacheButton.setBounds(buttons.removeFromLeft(120));
}

void PreferencesComponent::refreshCloseBehaviour()
{
    const auto closeToTrayEnabled = closeBehaviourBox.getSelectedId() == 2;
    const auto closeBehaviourText = closeToTrayEnabled
        ? "Clicking X currently hides Single Editor to the system tray, and playback stops before the window is hidden."
        : "Clicking X currently exits Single Editor completely, and no tray icon is created unless you switch this option on.";
    behaviourHintLabel.setText(juce::String(closeBehaviourText)
                                   + " Playlist restore stays off by default so the app behaves like a fast waveform player. Audio playlist retention stays on by default and reuses already-loaded standard audio items when you jump back to them. BNK and WEM default to packed-media decode so you hear the real shipped content; source-media mode is optional for comparison and debugging.",
                               juce::dontSendNotification);
}

void PreferencesComponent::refreshWwiseProjectPath()
{
    juce::File configuredPath;
    if (auto* settings = appProperties.getUserSettings())
        configuredPath = juce::File(settings->getValue(defaultWwiseProjectRootKey).trim());

    wwiseProjectRootPathLabel.setText(configuredPath.isDirectory()
                                          ? configuredPath.getFullPathName()
                                          : "Not set",
                                      juce::dontSendNotification);
}

void PreferencesComponent::refreshChannelOrderDefaults()
{
    suppressChannelOrderCallbacks = true;

    if (auto* settings = appProperties.getUserSettings())
    {
        populateDefaultChannelOrderBox(quadDefaultOrderBox,
                                       4,
                                       settings->getValue(defaultChannelOrder4chKey));
        populateDefaultChannelOrderBox(fiveOneDefaultOrderBox,
                                       6,
                                       settings->getValue(defaultChannelOrder6chKey));
        populateDefaultChannelOrderBox(sevenOneDefaultOrderBox,
                                       8,
                                       settings->getValue(defaultChannelOrder8chKey));
        populateDefaultChannelOrderBox(sevenOneFourDefaultOrderBox,
                                       12,
                                       settings->getValue(defaultChannelOrder12chKey));
    }
    else
    {
        populateDefaultChannelOrderBox(quadDefaultOrderBox, 4, {});
        populateDefaultChannelOrderBox(fiveOneDefaultOrderBox, 6, {});
        populateDefaultChannelOrderBox(sevenOneDefaultOrderBox, 8, {});
        populateDefaultChannelOrderBox(sevenOneFourDefaultOrderBox, 12, {});
    }

    suppressChannelOrderCallbacks = false;
}

void PreferencesComponent::refreshDeviceControls()
{
    auto* availableManager = audioEngine.getDeviceManagerForConfiguration();
    if (availableManager == nullptr)
    {
        if (audioEngine.isDeviceLeasedForProbe())
            setDeviceStatus("Audio device operation in progress. Use its Cancel action, or wait for the previous output to return.");
        else if (const auto active = audioEngine.getRuntimeMetricsForDiagnostics(); active)
            setDeviceStatus(juce::String(active->backend) + " output is active. Open Output Setup to inspect or change its configuration.");
        else if (audioEngine.hasExplicitOutputSelection())
            setDeviceStatus("The selected output is unavailable. Open Output Setup to retry or choose another output; no fallback was selected.");
        else
            setDeviceStatus("No managed Windows output is available. Open device setup to choose an output.");
        deviceTypeBox.setEnabled(false); outputDeviceBox.setEnabled(false);
        sampleRateBox.setEnabled(false); bufferSizeBox.setEnabled(false);
        return;
    }
    auto& deviceManager = *availableManager;
    const auto& deviceTypes = deviceManager.getAvailableDeviceTypes();

    suppressDeviceCallbacks = true;
    availableDeviceTypes.clear();
    deviceTypeBox.clear(juce::dontSendNotification);

    auto itemId = 1;
    for (auto* deviceType : deviceTypes)
    {
        if (deviceType == nullptr)
            continue;

        deviceType->scanForDevices();
        const auto typeName = deviceType->getTypeName();
        availableDeviceTypes.add(typeName);
        deviceTypeBox.addItem(typeName, itemId++);
    }

    auto selectedTypeName = deviceManager.getCurrentAudioDeviceType();
    if (selectedTypeName.isEmpty() && !availableDeviceTypes.isEmpty())
        selectedTypeName = availableDeviceTypes[0];

    const auto selectedTypeIndex = availableDeviceTypes.indexOf(selectedTypeName);
    if (selectedTypeIndex >= 0)
        deviceTypeBox.setSelectedItemIndex(selectedTypeIndex, juce::dontSendNotification);
    else if (!availableDeviceTypes.isEmpty())
        deviceTypeBox.setSelectedItemIndex(0, juce::dontSendNotification);

    suppressDeviceCallbacks = false;
    refreshDeviceDetails();
}

void PreferencesComponent::refreshDeviceDetails()
{
    auto* availableManager = audioEngine.getDeviceManagerForConfiguration();
    if (availableManager == nullptr)
    {
        setDeviceStatus("Audio device is busy with a diagnostic probe. Close and reopen Preferences after restoration.");
        return;
    }
    auto& deviceManager = *availableManager;
    auto* selectedDeviceType = deviceManager.getCurrentDeviceTypeObject();
    const auto selectedTypeName = deviceTypeBox.getText().trim();

    if (selectedDeviceType == nullptr
        || (selectedTypeName.isNotEmpty() && selectedDeviceType->getTypeName() != selectedTypeName))
    {
        for (auto* candidateType : deviceManager.getAvailableDeviceTypes())
        {
            if (candidateType != nullptr && candidateType->getTypeName() == selectedTypeName)
            {
                selectedDeviceType = candidateType;
                break;
            }
        }
    }

    suppressDeviceCallbacks = true;
    availableOutputDevices.clear();
    outputDeviceBox.clear(juce::dontSendNotification);
    availableSampleRates.clear();
    sampleRateBox.clear(juce::dontSendNotification);
    availableBufferSizes.clear();
    bufferSizeBox.clear(juce::dontSendNotification);

    if (selectedDeviceType != nullptr)
    {
        selectedDeviceType->scanForDevices();
        auto itemId = 1;
        for (const auto& deviceName : selectedDeviceType->getDeviceNames(false))
        {
            availableOutputDevices.add(deviceName);
            outputDeviceBox.addItem(deviceName, itemId++);
        }
    }

    const auto currentSetup = deviceManager.getAudioDeviceSetup();
    auto currentOutputName = currentSetup.outputDeviceName.trim();
    if (currentOutputName.isEmpty())
    {
        if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
            currentOutputName = currentDevice->getName();
    }

    if (currentOutputName.isEmpty() && selectedDeviceType != nullptr)
    {
        const auto defaultDeviceIndex = selectedDeviceType->getDefaultDeviceIndex(false);
        if (juce::isPositiveAndBelow(defaultDeviceIndex, availableOutputDevices.size()))
            currentOutputName = availableOutputDevices[defaultDeviceIndex];
    }

    const auto outputIndex = availableOutputDevices.indexOf(currentOutputName);
    if (outputIndex >= 0)
        outputDeviceBox.setSelectedItemIndex(outputIndex, juce::dontSendNotification);
    else if (!availableOutputDevices.isEmpty())
        outputDeviceBox.setSelectedItemIndex(0, juce::dontSendNotification);

    if (auto* currentDevice = deviceManager.getCurrentAudioDevice())
    {
        auto itemId = 1;
        availableSampleRates = currentDevice->getAvailableSampleRates();
        for (const auto sampleRate : availableSampleRates)
            sampleRateBox.addItem(formatSampleRate(sampleRate), itemId++);

        const auto currentSampleRate = currentSetup.sampleRate > 0.0
            ? currentSetup.sampleRate
            : currentDevice->getCurrentSampleRate();
        auto selectedSampleRateIndex = -1;
        auto bestSampleRateDelta = std::numeric_limits<double>::max();
        for (int i = 0; i < availableSampleRates.size(); ++i)
        {
            const auto delta = std::abs(availableSampleRates[i] - currentSampleRate);
            if (delta < bestSampleRateDelta)
            {
                bestSampleRateDelta = delta;
                selectedSampleRateIndex = i;
            }
        }
        if (selectedSampleRateIndex >= 0)
            sampleRateBox.setSelectedItemIndex(selectedSampleRateIndex, juce::dontSendNotification);

        itemId = 1;
        availableBufferSizes = currentDevice->getAvailableBufferSizes();
        for (const auto bufferSize : availableBufferSizes)
            bufferSizeBox.addItem(formatBufferSize(bufferSize), itemId++);

        const auto currentBufferSize = currentSetup.bufferSize > 0
            ? currentSetup.bufferSize
            : currentDevice->getCurrentBufferSizeSamples();
        auto selectedBufferSizeIndex = -1;
        auto bestBufferSizeDelta = std::numeric_limits<int>::max();
        for (int i = 0; i < availableBufferSizes.size(); ++i)
        {
            const auto delta = std::abs(availableBufferSizes[i] - currentBufferSize);
            if (delta < bestBufferSizeDelta)
            {
                bestBufferSizeDelta = delta;
                selectedBufferSizeIndex = i;
            }
        }
        if (selectedBufferSizeIndex >= 0)
            bufferSizeBox.setSelectedItemIndex(selectedBufferSizeIndex, juce::dontSendNotification);

        setDeviceStatus((selectedTypeName.isNotEmpty() ? selectedTypeName + "  |  " : juce::String())
                        + currentDevice->getName()
                        + "  |  Outputs "
                        + juce::String(countEnabledChannels(currentDevice->getActiveOutputChannels(),
                                                            currentDevice->getOutputChannelNames().size()))
                        + "/"
                        + juce::String(currentDevice->getOutputChannelNames().size())
                        + "  |  "
                        + formatSampleRate(currentDevice->getCurrentSampleRate())
                        + "  |  "
                        + formatBufferSize(currentDevice->getCurrentBufferSizeSamples()));
    }
    else if (availableOutputDevices.isEmpty())
    {
        setDeviceStatus(selectedTypeName.isNotEmpty()
                            ? "No output devices found for " + selectedTypeName + "."
                            : "No audio device types are available on this system.");
    }
    else
    {
        setDeviceStatus("Choose an output device to reopen playback with lightweight settings.");
    }

    suppressDeviceCallbacks = false;
}

void PreferencesComponent::applySelectedDeviceType()
{
    if (suppressDeviceCallbacks)
        return;

    const auto selectedTypeName = deviceTypeBox.getText().trim();
    if (selectedTypeName.isEmpty())
        return;

    audioEngine.selectAudioDeviceType(selectedTypeName);
    refreshDeviceDetails();
}

void PreferencesComponent::applySelectedDefaultChannelOrder(int channelCount,
                                                           juce::ComboBox& comboBox,
                                                           const char* settingsKey)
{
    if (suppressChannelOrderCallbacks || settingsKey == nullptr)
        return;

    const auto presetId = presetIdForComboSelection(channelCount, comboBox);
    ChannelLayoutCatalog::setPreferredDefaultPresetId(channelCount, presetId);

    if (auto* settings = appProperties.getUserSettings())
    {
        if (presetId.isEmpty())
            settings->removeValue(settingsKey);
        else
            settings->setValue(settingsKey, presetId);

        settings->saveIfNeeded();
    }
}

void PreferencesComponent::applySelectedDeviceSetup()
{
    if (suppressDeviceCallbacks)
        return;

    auto* availableManager = audioEngine.getDeviceManagerForConfiguration();
    if (availableManager == nullptr)
    {
        setDeviceStatus("Audio device is busy with a diagnostic probe. No settings were changed.");
        return;
    }
    auto& deviceManager = *availableManager;
    auto setup = deviceManager.getAudioDeviceSetup();
    auto didChangeSetup = false;

    if (const auto selectedOutputIndex = outputDeviceBox.getSelectedItemIndex();
        juce::isPositiveAndBelow(selectedOutputIndex, availableOutputDevices.size()))
    {
        const auto selectedOutputName = availableOutputDevices[selectedOutputIndex];
        if (selectedOutputName.isNotEmpty() && setup.outputDeviceName != selectedOutputName)
        {
            setup.outputDeviceName = selectedOutputName;
            didChangeSetup = true;
        }
    }

    if (const auto selectedSampleRateIndex = sampleRateBox.getSelectedItemIndex();
        juce::isPositiveAndBelow(selectedSampleRateIndex, availableSampleRates.size()))
    {
        const auto selectedSampleRate = availableSampleRates[selectedSampleRateIndex];
        if (selectedSampleRate > 0.0 && std::abs(setup.sampleRate - selectedSampleRate) > 0.01)
        {
            setup.sampleRate = selectedSampleRate;
            didChangeSetup = true;
        }
    }

    if (const auto selectedBufferSizeIndex = bufferSizeBox.getSelectedItemIndex();
        juce::isPositiveAndBelow(selectedBufferSizeIndex, availableBufferSizes.size()))
    {
        const auto selectedBufferSize = availableBufferSizes[selectedBufferSizeIndex];
        if (selectedBufferSize > 0 && setup.bufferSize != selectedBufferSize)
        {
            setup.bufferSize = selectedBufferSize;
            didChangeSetup = true;
        }
    }

    if (!didChangeSetup)
    {
        refreshDeviceDetails();
        return;
    }

    const auto error = audioEngine.applyAudioDeviceSetup(setup);
    refreshDeviceDetails();
    if (error.isNotEmpty())
        setDeviceStatus("Audio device warning: " + error);
}

void PreferencesComponent::setDeviceStatus(juce::String statusText)
{
    if (statusText.isEmpty())
        statusText = "Audio output opens on demand and stays out of the startup hot path.";

    deviceStatusLabel.setText(std::move(statusText), juce::dontSendNotification);
}

void PreferencesComponent::refreshCacheInfo()
{
    const auto cacheDir = WaveformModel::getPersistentCacheDirectory();
    const auto stats = WaveformModel::getPersistentCacheStats();
    cachePathLabel.setText(cacheDir.getFullPathName(), juce::dontSendNotification);
    cacheStatsLabel.setText(juce::String(stats.fileCount) + " files  |  " + formatBytes(stats.totalBytes),
                            juce::dontSendNotification);
}

juce::String PreferencesComponent::formatBytes(int64_t totalBytes)
{
    constexpr std::array<const char*, 5> suffixes { "B", "KB", "MB", "GB", "TB" };
    auto size = static_cast<double>(juce::jmax<int64_t>(0, totalBytes));
    std::size_t suffix = 0;
    while (size >= 1024.0 && suffix + 1 < suffixes.size())
    {
        size /= 1024.0;
        ++suffix;
    }

    return juce::String(size, size >= 100.0 || suffix == 0 ? 0 : 1) + " " + suffixes[suffix];
}

juce::String PreferencesComponent::formatSampleRate(double sampleRate)
{
    if (sampleRate <= 0.0)
        return "Automatic";

    return juce::String(sampleRate / 1000.0, sampleRate >= 100000.0 ? 0 : 1) + " kHz";
}

juce::String PreferencesComponent::formatBufferSize(int bufferSize)
{
    if (bufferSize <= 0)
        return "Automatic";

    return juce::String(bufferSize) + " samples";
}
} // namespace singleeditor
