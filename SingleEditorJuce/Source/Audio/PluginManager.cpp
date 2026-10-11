#include "PluginManager.h"

namespace singleeditor
{
PluginManager::PluginManager(juce::ApplicationProperties& propertiesToUse)
    : properties(propertiesToUse)
{
    loadCachedPluginList();
}

const juce::KnownPluginList& PluginManager::getKnownPlugins() const noexcept
{
    return knownPlugins;
}

int PluginManager::getCachedPluginCount() const noexcept
{
    return knownPlugins.getNumTypes();
}

void PluginManager::scanForNewPluginsManually()
{
    // Deliberately empty in the bootstrap milestone.
    // Manual scanning will be added as an explicit user action and never at startup.
}

void PluginManager::loadCachedPluginList()
{
    if (auto* userSettings = properties.getUserSettings())
    {
        if (auto xml = userSettings->getXmlValue("knownPlugins"); xml != nullptr)
            knownPlugins.recreateFromXml(*xml);
    }
}
} // namespace singleeditor
