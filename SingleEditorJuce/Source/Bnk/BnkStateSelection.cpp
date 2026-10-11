#include "Bnk/BnkStateSelection.h"
#include <unordered_set>

namespace singleeditor::bnkstate
{
juce::String id(const juce::String& value)
{ return value.trim().trimCharactersAtStart("{").trimCharactersAtEnd("}").toLowerCase(); }
juce::String name(const juce::String& value) { return value.trim().toLowerCase(); }
juce::String identity(const juce::String& token, uint32_t shortId, const juce::String& label)
{
    if (token.isNotEmpty()) return "guid:" + id(token);
    if (shortId != 0) return "short:" + juce::String(shortId);
    return "name:" + name(label);
}
juce::String group(const BnkDocument::StateValueDescriptor& value)
{ return identity(value.groupToken, value.groupId, value.groupName); }
juce::String group(const BnkDocument::EventDescriptor::StateModifier& value)
{ return identity(value.stateGroupToken, value.stateGroupId, value.stateGroupName); }
juce::String token(const BnkDocument::StateValueDescriptor& value)
{
    const auto groupKey = group(value), valueKey = identity(value.token, value.id, value.name);
    return "state/v1/" + juce::String(groupKey.length()) + ":" + groupKey
        + juce::String(valueKey.length()) + ":" + valueKey;
}
bool matches(const BnkDocument::EventDescriptor::StateModifier& modifier,
                    const BnkDocument::StateValueDescriptor& value)
{
    if (group(modifier) != group(value)) return false;
    if (modifier.stateValueToken.isNotEmpty())
        return value.token.isNotEmpty() && id(modifier.stateValueToken) == id(value.token);
    return modifier.stateName.isNotEmpty() && name(modifier.stateName) == name(value.name);
}
std::vector<BnkDocument::StateValueDescriptor> options(
    const BnkDocument::State& state, const BnkDocument::EventDescriptor& event)
{
    std::vector<BnkDocument::StateValueDescriptor> result;
    if (!event.stateModifiersResolved
        || event.stateTargetScope == BnkDocument::EventDescriptor::StateTargetScope::unavailable
        || event.stateModifiers.empty()) return result;
    std::unordered_set<std::string> groups, seen;
    for (const auto& modifier : event.stateModifiers) groups.insert(group(modifier).toStdString());
    const auto append = [&](BnkDocument::StateValueDescriptor value)
    {
        if (!seen.insert(token(value).toStdString()).second) return;
        value.isDefault = value.isDefault || value.name.equalsIgnoreCase("None");
        result.push_back(std::move(value));
    };
    for (const auto& value : state.stateValues)
        if (groups.count(group(value).toStdString())) append(value);
    // WWU may supply a complete single-group reference without a generated catalog.
    if (state.stateValues.empty())
        for (const auto& modifier : event.stateModifiers)
        {
            BnkDocument::StateValueDescriptor value;
            value.token=modifier.stateValueToken; value.name=modifier.stateName;
            value.groupToken=modifier.stateGroupToken; value.groupId=modifier.stateGroupId;
            value.groupName=modifier.stateGroupName;
            append(std::move(value));
        }
    return result;
}
}
