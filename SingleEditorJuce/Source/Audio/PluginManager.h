#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace singleeditor
{
class PluginManager final
{
public:
    explicit PluginManager(juce::ApplicationProperties& propertiesToUse);

    const juce::KnownPluginList& getKnownPlugins() const noexcept;
    int getCachedPluginCount() const noexcept;
    void scanForNewPluginsManually();

private:
    void loadCachedPluginList();

    juce::ApplicationProperties& properties;
    juce::KnownPluginList knownPlugins;
};
} // namespace singleeditor
