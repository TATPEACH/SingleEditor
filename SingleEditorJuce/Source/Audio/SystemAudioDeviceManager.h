#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace singleeditor
{
// The ordinary playback/configuration path must not discover optional ASIO
// drivers just because the GUI binary contains the JUCE ASIO implementation.
// ASIO is owned by its explicit adapter, never by this generic system list.
class SystemAudioDeviceManager final : public juce::AudioDeviceManager
{
public:
    void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override
    {
       #if JUCE_WINDOWS
        const auto add = [&types](juce::AudioIODeviceType* type)
        {
            if (type != nullptr) types.add(type);
        };
        // Preserve JUCE 8.0.12's existing Windows preference order.
        add(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::shared));
        add(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::exclusive));
        add(juce::AudioIODeviceType::createAudioIODeviceType_WASAPI(juce::WASAPIDeviceMode::sharedLowLatency));
        add(juce::AudioIODeviceType::createAudioIODeviceType_DirectSound());
       #else
        juce::AudioDeviceManager::createAudioDeviceTypes(types);
       #endif
    }
};
}
