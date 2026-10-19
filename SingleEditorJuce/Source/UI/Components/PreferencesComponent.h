#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Audio/AudioEngine.h"
#include "UI/Components/CardSurface.h"
#include "Waveform/WaveformModel.h"

namespace singleeditor
{
class PreferencesComponent final : public juce::Component
{
public:
    PreferencesComponent(AudioEngine& audioEngineToUse,
                         juce::ApplicationProperties& appPropertiesToUse,
                         std::function<void()> openAsioSettings = {});

    static constexpr const char* restorePlaylistSessionKey = "restorePlaylistSession";
    static constexpr const char* retainPlaylistAudioKey = "retainPlaylistAudioLoaded";
    static constexpr const char* closeToTrayOnCloseKey = "closeToTrayOnClose";
    static constexpr const char* rendererModeKey = "rendererMode";
    static constexpr const char* modernThemeKey = "modernGlassTheme";
    static constexpr const char* modernMaterialKey = "modernGlassMaterial";
    static constexpr const char* modernSkinKey = "modernSkin";
    static constexpr const char* reduceMotionKey = "modernReduceMotion";
    static constexpr const char* bnkPlaybackModeKey = "bnkPlaybackMode";
    static constexpr const char* defaultWwiseProjectRootKey = "defaultWwiseProjectRoot";
    static constexpr const char* defaultChannelOrder4chKey = "defaultChannelOrder4ch";
    static constexpr const char* defaultChannelOrder6chKey = "defaultChannelOrder6ch";
    static constexpr const char* defaultChannelOrder8chKey = "defaultChannelOrder8ch";
    static constexpr const char* defaultChannelOrder12chKey = "defaultChannelOrder12ch";
    static void applyStoredChannelOrderDefaults(juce::ApplicationProperties& appProperties);
    static void applyStoredVisualPreferences(juce::ApplicationProperties& appProperties);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void refreshCloseBehaviour();
    void refreshCacheInfo();
    void refreshWwiseProjectPath();
    void refreshChannelOrderDefaults();
    void refreshDeviceControls();
    void refreshDeviceDetails();
    void applySelectedDeviceType();
    void applySelectedDeviceSetup();
    void applySelectedDefaultChannelOrder(int channelCount, juce::ComboBox& comboBox, const char* settingsKey);
    void setDeviceStatus(juce::String statusText);
    static juce::String formatBytes(int64_t totalBytes);
    static juce::String formatSampleRate(double sampleRate);
    static juce::String formatBufferSize(int bufferSize);

    AudioEngine& audioEngine;
    juce::ApplicationProperties& appProperties;
    CardSurface behaviourCard { 18.0f };
    CardSurface channelOrderCard { 18.0f };
    CardSurface deviceCard { 18.0f };
    CardSurface cacheCard { 18.0f };
    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label behaviourLabel;
    juce::Label behaviourHintLabel;
    juce::Label closeBehaviourLabel;
    juce::ComboBox closeBehaviourBox;
    juce::Label rendererModeLabel;
    juce::ComboBox rendererModeBox;
    juce::Label rendererModeHintLabel;
    juce::Label reduceMotionLabel;
    juce::ComboBox reduceMotionBox;
    juce::Label restorePlaylistSessionLabel;
    juce::ComboBox restorePlaylistSessionBox;
    juce::Label retainPlaylistAudioLabel;
    juce::ComboBox retainPlaylistAudioBox;
    juce::Label bnkPlaybackModeLabel;
    juce::ComboBox bnkPlaybackModeBox;
    juce::Label wwiseProjectRootLabel;
    juce::Label wwiseProjectRootPathLabel;
    juce::TextButton chooseWwiseProjectRootButton;
    juce::TextButton clearWwiseProjectRootButton;
    juce::Label channelOrderLabel;
    juce::Label channelOrderHintLabel;
    juce::Label quadDefaultOrderLabel;
    juce::ComboBox quadDefaultOrderBox;
    juce::Label fiveOneDefaultOrderLabel;
    juce::ComboBox fiveOneDefaultOrderBox;
    juce::Label sevenOneDefaultOrderLabel;
    juce::ComboBox sevenOneDefaultOrderBox;
    juce::Label sevenOneFourDefaultOrderLabel;
    juce::ComboBox sevenOneFourDefaultOrderBox;
    juce::Label deviceLabel;
    juce::TextButton asioSettingsButton {"Output Setup..."};
    juce::Label deviceTypeLabel;
    juce::ComboBox deviceTypeBox;
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceBox;
    juce::Label sampleRateLabel;
    juce::ComboBox sampleRateBox;
    juce::Label bufferSizeLabel;
    juce::ComboBox bufferSizeBox;
    juce::Label deviceStatusLabel;
    juce::TextButton refreshDevicesButton;
    juce::Label cacheLabel;
    juce::Label cachePathLabel;
    juce::Label cacheStatsLabel;
    juce::Label cacheHintLabel;
    std::unique_ptr<juce::FileChooser> wwiseProjectRootChooser;
    juce::TextButton openCacheFolderButton;
    juce::TextButton clearCacheButton;
    juce::StringArray availableDeviceTypes;
    juce::StringArray availableOutputDevices;
    juce::Array<double> availableSampleRates;
    juce::Array<int> availableBufferSizes;
    bool suppressDeviceCallbacks = false;
    bool suppressChannelOrderCallbacks = false;
};
} // namespace singleeditor
