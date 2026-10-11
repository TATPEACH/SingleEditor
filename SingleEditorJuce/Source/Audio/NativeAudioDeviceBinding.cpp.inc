// Included after the unchanged JUCE audio-device amalgamation. The reviewed
// concrete class exposes its exact endpoint IDs publicly; no ABI/private hack.
#include "NativeAudioDeviceBinding.h"

juce::String singleeditor::copyWasapiOutputEndpoint(const juce::AudioIODevice* device)
{
   #if JUCE_WINDOWS && JUCE_WASAPI
    if (const auto* wasapi = dynamic_cast<const juce::WasapiClasses::WASAPIAudioIODevice*>(device))
        if (wasapi->outputDeviceId.length() <= 2048)
            return wasapi->outputDeviceId;
   #else
    juce::ignoreUnused(device);
   #endif
    return {};
}

std::optional<std::vector<singleeditor::WindowsOutputEndpoint>> singleeditor::copyWasapiOutputCatalog(const juce::AudioIODeviceType* type)
{
   #if JUCE_WINDOWS && JUCE_WASAPI
    if (const auto* wasapi = dynamic_cast<const juce::WasapiClasses::WASAPIAudioIODeviceType*>(type))
    {
        const auto& devices = wasapi->devices;
        if (devices.outputDeviceNames.size() != devices.outputDeviceIds.size()) return {};
        std::vector<WindowsOutputEndpoint> result;
        for (int index = 0; index < devices.outputDeviceIds.size(); ++index)
            if (devices.outputDeviceIds[index].isNotEmpty() && devices.outputDeviceIds[index].length() <= 2048)
                result.push_back({devices.outputDeviceNames[index], devices.outputDeviceIds[index], index == type->getDefaultDeviceIndex(false)});
        return result;
    }
   #else
    juce::ignoreUnused(type);
   #endif
    return {};
}

std::unique_ptr<juce::AudioIODevice> singleeditor::createWasapiOutputByEndpoint(juce::AudioIODeviceType* type, const juce::String& endpoint)
{
   #if JUCE_WINDOWS && JUCE_WASAPI
    if (auto* wasapi = dynamic_cast<juce::WasapiClasses::WASAPIAudioIODeviceType*>(type))
    {
        const auto index = wasapi->devices.outputDeviceIds.indexOf(endpoint);
        if (index >= 0)
        {
            std::unique_ptr<juce::AudioIODevice> device(type->createDevice(wasapi->devices.outputDeviceNames[index], {}));
            if (device && copyWasapiOutputEndpoint(device.get()) == endpoint) return device;
        }
    }
   #else
    juce::ignoreUnused(type, endpoint);
   #endif
    return {};
}
