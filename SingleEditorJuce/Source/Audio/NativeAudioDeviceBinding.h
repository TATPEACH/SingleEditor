#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <optional>
#include <vector>

namespace juce { class AudioIODevice; class AudioIODeviceType; }
namespace singleeditor
{
struct WindowsOutputEndpoint
{
    juce::String displayName, stableId;
    bool isDefault = false;
};
// Copy only from a caller-owned device (current Engine device or a just-created
// probe device). No native calls, name lookup, creation, or retained JUCE owner.
#if SINGLE_EDITOR_WASAPI_DIAGNOSTIC_BINDING
juce::String copyWasapiOutputEndpoint(const juce::AudioIODevice*);
// Caller has explicitly scanned its own WASAPI type. Copy its exact endpoint
// identities, or create only the selected endpoint; never use a name as identity.
std::optional<std::vector<WindowsOutputEndpoint>> copyWasapiOutputCatalog(const juce::AudioIODeviceType*);
std::unique_ptr<juce::AudioIODevice> createWasapiOutputByEndpoint(juce::AudioIODeviceType*, const juce::String& endpoint);
#else
inline juce::String copyWasapiOutputEndpoint(const juce::AudioIODevice*) { return {}; }
#endif
}
