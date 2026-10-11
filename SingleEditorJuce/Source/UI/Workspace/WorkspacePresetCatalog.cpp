#include "WorkspacePresetCatalog.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <juce_core/juce_core.h>

namespace
{
constexpr auto catalogSchema = "single-editor.workspace-preset-catalog";
constexpr int catalogVersion = 1;
constexpr std::size_t maximumPresetBytes = 256 * 1024;

juce::var makeObject()
{
    return juce::var(new juce::DynamicObject());
}

juce::var parseObjectOrNew(const std::string& text)
{
    juce::var value;
    if (juce::JSON::parse(juce::String::fromUTF8(text.c_str()), value).wasOk()
        && value.getDynamicObject() != nullptr)
    {
        return value;
    }
    return makeObject();
}

std::string compactJson(const juce::var& value)
{
    return juce::JSON::toString(value, true).toStdString();
}

bool validId(const std::string& value)
{
    if (value.empty() || value.size() > 64)
        return false;
    return std::all_of(value.begin(), value.end(), [] (unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
    });
}
}

WorkspaceContentCategory WorkspacePresetCatalog::classify(bool isBnk,
                                                           bool isVideoAudio,
                                                           bool isAmbisonic,
                                                           int channelCount) noexcept
{
    if (isBnk)
        return WorkspaceContentCategory::bnk;
    if (isVideoAudio)
        return WorkspaceContentCategory::videoAudio;
    if (isAmbisonic)
        return WorkspaceContentCategory::ambisonics;
    if (channelCount > 2)
        return WorkspaceContentCategory::discreteMultichannel;
    return WorkspaceContentCategory::general;
}

std::string WorkspacePresetCatalog::categoryKey(WorkspaceContentCategory category)
{
    switch (category)
    {
        case WorkspaceContentCategory::general: return "general";
        case WorkspaceContentCategory::discreteMultichannel: return "discrete-multichannel";
        case WorkspaceContentCategory::ambisonics: return "ambisonics";
        case WorkspaceContentCategory::videoAudio: return "video-audio";
        case WorkspaceContentCategory::bnk: return "bnk";
    }
    return "general";
}

bool WorkspacePresetCatalog::savePreset(std::string id,
                                        std::string displayName,
                                        std::string workspaceState)
{
    if (!validId(id) || displayName.empty() || displayName.size() > 128
        || workspaceState.empty() || workspaceState.size() > maximumPresetBytes)
    {
        return false;
    }
    juce::var workspace;
    if (juce::JSON::parse(juce::String::fromUTF8(workspaceState.c_str()), workspace).failed()
        || workspace.getDynamicObject() == nullptr)
    {
        return false;
    }

    const auto found = std::find_if(storedPresets.begin(), storedPresets.end(), [&id] (const WorkspaceStoredPreset& item)
    {
        return item.id == id;
    });
    if (found != storedPresets.end())
    {
        found->id = std::move(id);
        found->displayName = std::move(displayName);
        found->workspaceState = std::move(workspaceState);
    }
    else
        storedPresets.push_back({ std::move(id), std::move(displayName), std::move(workspaceState) });
    return true;
}

bool WorkspacePresetCatalog::bindCategory(WorkspaceContentCategory category, const std::string& presetId)
{
    if (!preset(presetId).has_value())
        return false;
    const auto key = categoryKey(category);
    const auto found = std::find_if(bindings.begin(), bindings.end(), [&key] (const Binding& item)
    {
        return item.category == key;
    });
    if (found != bindings.end())
        found->presetId = presetId;
    else
        bindings.push_back({ key, presetId });
    return true;
}

std::optional<WorkspaceStoredPreset> WorkspacePresetCatalog::preset(const std::string& id) const
{
    const auto found = std::find_if(storedPresets.begin(), storedPresets.end(), [&id] (const WorkspaceStoredPreset& item)
    {
        return item.id == id;
    });
    return found != storedPresets.end() ? std::optional<WorkspaceStoredPreset>(*found) : std::nullopt;
}

std::optional<WorkspaceStoredPreset> WorkspacePresetCatalog::resolve(WorkspaceContentCategory category) const
{
    const auto key = categoryKey(category);
    const auto found = std::find_if(bindings.begin(), bindings.end(), [&key] (const Binding& item)
    {
        return item.category == key;
    });
    return found != bindings.end() ? preset(found->presetId) : std::nullopt;
}

std::vector<WorkspaceStoredPreset> WorkspacePresetCatalog::presets() const
{
    return storedPresets;
}

std::string WorkspacePresetCatalog::serialise() const
{
    auto root = parseObjectOrNew(opaqueTopLevelJson);
    auto* object = root.getDynamicObject();
    object->setProperty("schema", catalogSchema);
    object->setProperty("version", catalogVersion);

    auto orderedPresets = storedPresets;
    std::sort(orderedPresets.begin(), orderedPresets.end(), [] (const auto& left, const auto& right)
    {
        return left.id < right.id;
    });
    juce::Array<juce::var> presetsValue;
    for (const auto& item : orderedPresets)
    {
        auto value = parseObjectOrNew(item.opaqueRecordJson);
        value.getDynamicObject()->setProperty("id", juce::String::fromUTF8(item.id.c_str()));
        value.getDynamicObject()->setProperty("name", juce::String::fromUTF8(item.displayName.c_str()));
        juce::var workspace;
        juce::JSON::parse(juce::String::fromUTF8(item.workspaceState.c_str()), workspace);
        value.getDynamicObject()->setProperty("workspace", workspace);
        presetsValue.add(value);
    }
    object->setProperty("presets", juce::var(presetsValue));

    auto bindingsValue = makeObject();
    auto orderedBindings = bindings;
    std::sort(orderedBindings.begin(), orderedBindings.end(), [] (const auto& left, const auto& right)
    {
        return left.category < right.category;
    });
    for (const auto& binding : orderedBindings)
        bindingsValue.getDynamicObject()->setProperty(juce::Identifier(binding.category),
                                                       juce::String::fromUTF8(binding.presetId.c_str()));
    object->setProperty("bindings", bindingsValue);
    return compactJson(root);
}

WorkspacePresetCatalogRestoreStatus WorkspacePresetCatalog::restore(const std::string& state)
{
    if (state.empty())
        return WorkspacePresetCatalogRestoreStatus::noState;
    juce::var root;
    if (juce::JSON::parse(juce::String::fromUTF8(state.c_str()), root).failed())
        return WorkspacePresetCatalogRestoreStatus::corrupt;
    const auto* object = root.getDynamicObject();
    if (object == nullptr || object->getProperty("schema").toString() != catalogSchema)
        return WorkspacePresetCatalogRestoreStatus::corrupt;
    const auto versionValue = object->getProperty("version");
    if (!versionValue.isInt() && !versionValue.isInt64())
        return WorkspacePresetCatalogRestoreStatus::corrupt;
    const auto version = static_cast<int>(versionValue);
    if (version > catalogVersion)
        return WorkspacePresetCatalogRestoreStatus::futureSchema;
    if (version != catalogVersion)
        return WorkspacePresetCatalogRestoreStatus::corrupt;

    const auto* presetsValue = object->getProperty("presets").getArray();
    const auto* bindingsValue = object->getProperty("bindings").getDynamicObject();
    if (presetsValue == nullptr || bindingsValue == nullptr)
        return WorkspacePresetCatalogRestoreStatus::corrupt;

    std::vector<WorkspaceStoredPreset> candidatePresets;
    std::unordered_set<std::string> ids;
    for (const auto& value : *presetsValue)
    {
        const auto* presetObject = value.getDynamicObject();
        if (presetObject == nullptr)
            return WorkspacePresetCatalogRestoreStatus::corrupt;
        const auto id = presetObject->getProperty("id").toString().toStdString();
        const auto name = presetObject->getProperty("name").toString().toStdString();
        const auto workspace = presetObject->getProperty("workspace");
        const auto workspaceState = compactJson(workspace);
        if (!validId(id) || name.empty() || name.size() > 128 || !ids.insert(id).second
            || workspace.getDynamicObject() == nullptr || workspaceState.size() > maximumPresetBytes)
        {
            return WorkspacePresetCatalogRestoreStatus::corrupt;
        }
        candidatePresets.push_back({ id, name, workspaceState, compactJson(value) });
    }

    std::vector<Binding> candidateBindings;
    for (const auto& property : bindingsValue->getProperties())
    {
        const auto category = property.name.toString().toStdString();
        const auto presetId = property.value.toString().toStdString();
        if (!validId(presetId))
            return WorkspacePresetCatalogRestoreStatus::corrupt;
        const auto target = std::find_if(candidatePresets.begin(), candidatePresets.end(), [&presetId] (const WorkspaceStoredPreset& item)
        {
            return item.id == presetId;
        });
        if (target == candidatePresets.end())
            return WorkspacePresetCatalogRestoreStatus::corrupt;
        candidateBindings.push_back({ category, presetId });
    }

    storedPresets = std::move(candidatePresets);
    bindings = std::move(candidateBindings);
    opaqueTopLevelJson = compactJson(root);
    return WorkspacePresetCatalogRestoreStatus::applied;
}

void WorkspacePresetCatalog::reset()
{
    storedPresets.clear();
    bindings.clear();
    opaqueTopLevelJson = "{}";
}
