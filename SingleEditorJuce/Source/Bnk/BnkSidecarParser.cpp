#include "Bnk/BnkSidecarParser.h"
#include "Bnk/BoundedSidecarInput.h"
#include "Bnk/BnkStateSelection.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace singleeditor
{
namespace
{
enum class TextSidecarSection
{
    none = 0,
    switchGroups,
    switches,
    stateGroups,
    states,
    customStates,
    inMemoryAudio,
    streamedAudio
};

struct EventTargetInfo
{
    juce::String objectId;
    juce::String objectName;
};

struct AuthoringEventInfo
{
    juce::String id, name, path;
    uint32_t shortId = 0;
    std::vector<EventTargetInfo> targets;
    bool complete = true;
};

struct AuthoringObjectInfo
{
    juce::String id, name, path;
    uint32_t shortId = 0;
    std::vector<BnkDocument::EventDescriptor::StateModifier> modifiers;
    bool modifiersResolved = true;
};

struct TextSidecarStateLinkData
{
    std::unordered_map<uint32_t, juce::String> mediaObjectPaths;
    std::unordered_map<std::string, std::vector<BnkDocument::EventDescriptor::StateModifier>> ownerStates;
};

std::string makeLookupKey(juce::String value)
{
    if (!BoundedSidecarInput::admitDerivedText(static_cast<size_t>(value.length()),5)) return {};
    value = value.trim();
    value = value.trimCharactersAtStart("{").trimCharactersAtEnd("}");
    return value.toLowerCase().toStdString();
}

// GUID, ShortID, display name and hierarchy path are different namespaces.
// Duplicate names can exist; they cannot authorize an arbitrary first/last hit.
std::string authoringNameKey(const juce::String& value)
{
    if (!BoundedSidecarInput::admitDerivedText(static_cast<size_t>(value.length()),4)) return {};
    return value.trim().toLowerCase().toStdString();
}

template <typename Record>
class AuthoringLookup
{
public:
    explicit AuthoringLookup(size_t limit, size_t references = 65536) : maximumRecords(limit), maximumReferences(references) {}
    bool empty() const { return records.empty(); }
    bool admitReference()
    {
        if (references >= maximumReferences)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return false; }
        ++references; return BoundedSidecarInput::continueWork();
    }
    void add(Record record)
    {
        if (!BoundedSidecarInput::continueWork()) return;
        if (records.size() >= maximumRecords)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
        const auto index = records.size();
        if (record.id.isNotEmpty() && !ids.emplace(makeLookupKey(record.id), index).second)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
        if (record.shortId != 0 && !shortIds.emplace(record.shortId, index).second)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
        const auto nameIndex = [&](auto& map, const juce::String& text)
        {
            if (text.isEmpty()) return;
            auto [found, inserted] = map.emplace(authoringNameKey(text), index);
            if (!inserted) found->second = ambiguous;
        };
        nameIndex(names, record.name); nameIndex(paths, record.path);
        records.push_back(std::move(record));
    }
    const Record* byId(const juce::String& id) const { return lookup(ids, makeLookupKey(id)); }
    const Record* byShortId(uint32_t id) const { return lookup(shortIds, id); }
    const Record* byName(const juce::String& name) const { return lookup(names, authoringNameKey(name)); }
    const Record* byPath(const juce::String& path) const { return lookup(paths, authoringNameKey(path)); }
    bool hasName(const juce::String& name) const { return names.count(authoringNameKey(name)) != 0; }
    const Record* target(const EventTargetInfo& reference) const
    { return reference.objectId.isNotEmpty() ? byId(reference.objectId) : byName(reference.objectName); }
private:
    template <typename Map, typename Key>
    const Record* lookup(const Map& map, const Key& key) const
    {
        const auto found = map.find(key);
        return found != map.end() && found->second != ambiguous ? &records[found->second] : nullptr;
    }
    static constexpr size_t ambiguous = std::numeric_limits<size_t>::max();
    size_t maximumRecords, maximumReferences, references = 0;
    std::vector<Record> records;
    std::unordered_map<std::string, size_t> ids, names, paths;
    std::unordered_map<uint32_t, size_t> shortIds;
};
using EventTargetLookup = AuthoringLookup<AuthoringEventInfo>;
using ObjectStateLookup = AuthoringLookup<AuthoringObjectInfo>;

void unavailableStateTarget(BnkDocument::EventDescriptor& event, BnkDocument::State& state)
{
    event.targetObjectId.clear(); event.targetObjectName.clear(); event.stateModifiers.clear();
    event.stateTargetScope = BnkDocument::EventDescriptor::StateTargetScope::unavailable;
    state.info.diagnostics.addIfNotAlreadyThere(
        "Some event State previews are unavailable because their authoring target scope cannot be resolved unambiguously; media remains accessible.");
}

juce::String objectNameFromPath(juce::String objectPath)
{
    objectPath = objectPath.replaceCharacter('/', '\\').trim().trimCharactersAtEnd("\\");
    if (objectPath.isEmpty())
        return {};

    return objectPath.fromLastOccurrenceOf("\\", false, false).trim();
}

template <typename Callback>
void visitXmlTree(const juce::XmlElement& element, Callback&& callback)
{
    if (!BoundedSidecarInput::continueWork()) return;
    callback(element);
    forEachXmlChildElement(element, child)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        visitXmlTree(*child, callback);
    }
}

juce::XmlElement* uniqueChild(const juce::XmlElement& element, juce::StringRef childTagName)
{
    auto* child = element.getChildByName(childTagName);
    if (child && child->getNextElementWithTagName(childTagName))
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return nullptr; }
    return child;
}

juce::String childText(const juce::XmlElement& element, juce::StringRef childTagName)
{
    const auto* child = uniqueChild(element, childTagName);
    return child ? child->getAllSubText().trim() : juce::String();
}

uint32_t parseIdText(const juce::String& source)
{
    const auto text = source.trim().toStdString();
    uint32_t id = 0;
    const auto value = std::from_chars(text.data(), text.data() + text.size(), id);
    if (text.empty() || value.ec != std::errc() || value.ptr != text.data() + text.size())
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return 0; }
    return id;
}

uint32_t parseAttributeId(const juce::XmlElement& element, const juce::Identifier& name)
{
    if (!element.hasAttribute(name)) return 0; // Existing GUID/name-only controls.
    return parseIdText(element.getStringAttribute(name));
}

double parseFiniteDouble(const juce::String& source)
{
    const auto text = source.trim().toStdString();
    auto* first = text.data();
    const auto* last = first + text.size();
    // from_chars is locale independent. Permit one ordinary leading plus,
    // without admitting a second sign, a numeric prefix, hex or non-finite data.
    if (first != last && *first == '+')
    {
        ++first;
        if (first != last && (*first == '+' || *first == '-'))
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return 0.0; }
    }
    double number = 0.0;
    const auto value = std::from_chars(first, last, number, std::chars_format::general);
    if (first == last || value.ec != std::errc() || value.ptr != last || !std::isfinite(number))
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return 0.0; }
    return number;
}

double parseDoubleAttribute(const juce::XmlElement& element, const juce::Identifier& name, double fallback)
{
    return element.hasAttribute(name) ? parseFiniteDouble(element.getStringAttribute(name)) : fallback;
}

double parseDoubleChild(const juce::XmlElement& element, juce::StringRef name)
{
    const auto* child = uniqueChild(element, name);
    if (!child)
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return 0.0; }
    forEachXmlChildElement(*child, nested)
        if (!nested->isTextElement()) BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
    return parseFiniteDouble(child->getAllSubText());
}

double parseOptionalDoubleAttribute(const juce::XmlElement& element,
                                    const juce::Identifier& primary,
                                    const juce::Identifier& secondary,
                                    double fallback = 0.0)
{
    const auto first = parseDoubleAttribute(element, primary, fallback);
    const auto second = parseDoubleAttribute(element, secondary, first);
    if (element.hasAttribute(primary) && element.hasAttribute(secondary) && first != second)
        BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
    return second;
}

void validateParameterRange(const BnkDocument::RtpcDescriptor& descriptor)
{
    if (descriptor.minValue > descriptor.maxValue || descriptor.defaultValue < descriptor.minValue
        || descriptor.defaultValue > descriptor.maxValue
        || !std::isfinite(descriptor.maxValue - descriptor.minValue))
        BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
}

std::optional<double> readPropertyScalarValue(const juce::XmlElement& property)
{
    std::optional<double> result;
    const auto consume = [&](const juce::String& text)
    {
        const auto value = parseFiniteDouble(text);
        if (result && *result != value)
            BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
        result = value;
    };
    if (property.hasAttribute("Value")) consume(property.getStringAttribute("Value"));
    if (auto* valueList = uniqueChild(property, "ValueList"))
    {
        bool found = false;
        forEachXmlChildElement(*valueList, value)
        {
            if (!BoundedSidecarInput::continueWork()) break;
            // Platform selection is not established here. Only an invariant
            // scalar across all declared values is safe to apply to PCM.
            if (value->isTextElement())
            {
                if (value->getText().trim().isNotEmpty())
                    BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
                continue;
            }
            if (!value->hasTagName("Value"))
            { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); break; }
            forEachXmlChildElement(*value, child)
                if (!child->isTextElement()) BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
            consume(value->getAllSubText());
            found = true;
        }
        if (!found) BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
    }
    return result;
}

juce::File resolveRelativeFile(const juce::File& root, const juce::String& relativePath)
{
    if (!root.isDirectory() || relativePath.isEmpty())
        return {};

    return BoundedSidecarInput::withinRoot(root, relativePath);
}

juce::File findProjectRootNearBank(const juce::File& bankFile)
{
    if (const auto catalog = BoundedSidecarInput::frozenInputs()) return catalog->projectRoot;
    auto directory = bankFile.getParentDirectory();
    for (int depth = 0; depth < 5 && directory.isDirectory(); ++depth)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        if (!BoundedSidecarInput::files(directory, "*.wproj", false).empty())
            return directory;

        const auto parent = directory.getParentDirectory();
        if (parent == directory)
            break;
        directory = parent;
    }

    return {};
}

juce::StringArray splitSidecarColumns(juce::String line)
{
    line = line.trimEnd();
    juce::StringArray columns;
    columns.addTokens(line, "\t", {});
    columns.trim();
    // Empty cells still occupy their declared source/generated/owner columns.
    return columns;
}

void fillMediaPath(BnkDocument::EventDescriptor::MediaReference& media,
                   const juce::File& sourceFilesRoot,
                   const juce::File& soundBanksRoot,
                   const juce::File& bankDirectory)
{
    if (media.mediaFile.existsAsFile())
        return;

    const auto source = resolveRelativeFile(sourceFilesRoot, media.relativePath);
    if (source.existsAsFile()) media.sourceFile = source;

    media.mediaFile = resolveRelativeFile(soundBanksRoot, media.relativePath);
    if (media.mediaFile.existsAsFile())
        return;

    if (bankDirectory.isDirectory() && media.relativePath.isNotEmpty())
    {
        media.mediaFile = resolveRelativeFile(bankDirectory, media.relativePath);
        if (media.mediaFile.existsAsFile())
            return;
    }

    if (bankDirectory.isDirectory() && media.mediaId != 0)
    {
        media.mediaFile = resolveRelativeFile(bankDirectory, juce::String(media.mediaId) + ".wem");
        if (media.mediaFile.existsAsFile())
            return;
    }

    media.mediaFile = {};
}

void applyPlaybackShape(BnkDocument::EventDescriptor& event)
{
    if (event.media.empty())
    {
        event.playbackHint = BnkDocument::PlaybackHint::unavailable;
        event.playbackSummary = "No media sidecar mapping";
        return;
    }

    const auto switchCount = static_cast<int>(std::count_if(event.media.begin(),
                                                            event.media.end(),
                                                            [](const BnkDocument::EventDescriptor::MediaReference& media)
                                                            {
                                                                return media.switchValueToken.isNotEmpty();
                                                            }));

    if (switchCount > 0)
    {
        event.playbackHint = BnkDocument::PlaybackHint::switchDefault;
        event.playbackSummary = "Switch(default)  |  " + juce::String(event.media.size()) + " candidate(s)";
        event.mayResolveToMultipleNodes = true;
        return;
    }

    if (event.media.size() == 1)
    {
        event.playbackHint = BnkDocument::PlaybackHint::single;
        event.playbackSummary = "Single media";
        return;
    }

    event.playbackHint = BnkDocument::PlaybackHint::random;
    event.playbackSummary = "Random  |  " + juce::String(event.media.size()) + " variant(s)";
    event.mayResolveToMultipleNodes = true;
}

template <typename Descriptor>
void appendUniqueByToken(std::vector<Descriptor>& target, Descriptor descriptor)
{
    const auto exists = std::find_if(target.begin(),
                                     target.end(),
                                     [&descriptor](const Descriptor& current)
                                     {
                                         return current.token == descriptor.token
                                             || (!descriptor.name.isEmpty() && current.name == descriptor.name);
                                     });
    if (exists == target.end())
        target.push_back(std::move(descriptor));
}

void appendStateGroup(BnkDocument::State& state, BnkDocument::StateGroupDescriptor group)
{
    if (group.token.isEmpty() && group.id==0 && group.name.isEmpty())
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
    const auto key = bnkstate::identity(group.token, group.id, group.name);
    for (const auto& current : state.stateGroups)
    {
        if (!BoundedSidecarInput::continueWork()) return;
        if (bnkstate::identity(current.token, current.id, current.name) == key
            || (group.id != 0 && current.id == group.id))
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
    }
    if (state.stateGroups.size() >= 4096)
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
    state.stateGroups.push_back(std::move(group));
}

void appendStateValue(BnkDocument::State& state, BnkDocument::StateValueDescriptor value)
{
    if (value.groupToken.isEmpty() && value.groupId == 0)
    {
        const BnkDocument::StateGroupDescriptor* group = nullptr;
        for (const auto& current : state.stateGroups)
        {
            if (!BoundedSidecarInput::continueWork()) return;
            if (bnkstate::name(current.name) != bnkstate::name(value.groupName)) continue;
            if (group) { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
            group = &current;
        }
        if (group) { value.groupToken=group->token; value.groupId=group->id; value.groupName=group->name; }
        else if (value.groupName.isNotEmpty()) appendStateGroup(state, {0, {}, value.groupName});
    }
    for (const auto& current : state.stateValues)
    {
        if (!BoundedSidecarInput::continueWork()) return;
        const auto sameGuid = value.token.isNotEmpty() && current.token.isNotEmpty()
            && bnkstate::id(current.token)==bnkstate::id(value.token);
        const auto sameGroup = bnkstate::group(current)==bnkstate::group(value);
        const auto sameShortId = sameGroup && value.id != 0 && current.id == value.id;
        const auto legacyName = sameGroup && value.token.isEmpty() && value.id==0
            && bnkstate::name(value.name)==bnkstate::name(current.name);
        if (!sameGuid && !sameShortId && !legacyName) continue;
        if (!sameGroup || bnkstate::name(current.name)!=bnkstate::name(value.name)
            || (current.token.isNotEmpty() && value.token.isNotEmpty() && !sameGuid)
            || (current.id != 0 && value.id != 0 && current.id != value.id))
            BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
        return;
    }
    if (state.stateValues.size() >= 4096)
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
    state.stateValues.push_back(std::move(value));
}

bool resolveStateModifier(BnkDocument::EventDescriptor::StateModifier& modifier,
    const std::vector<BnkDocument::StateGroupDescriptor>& references, const BnkDocument::State& state)
{
    std::vector<BnkDocument::StateGroupDescriptor> groups;
    for (const auto& reference : references)
    {
        if (reference.token.isEmpty() && reference.id==0 && reference.name.isEmpty()) return false;
        const BnkDocument::StateGroupDescriptor* resolved = nullptr;
        for (const auto& candidate : state.stateGroups)
        {
            if (!BoundedSidecarInput::continueWork()) return false;
            const bool matches = reference.token.isNotEmpty()
                ? candidate.token.isNotEmpty() && bnkstate::id(reference.token)==bnkstate::id(candidate.token)
                : reference.id != 0 ? candidate.id==reference.id
                : reference.name.isNotEmpty() && bnkstate::name(reference.name)==bnkstate::name(candidate.name);
            if (!matches) continue;
            if (resolved) return false;
            resolved=&candidate;
        }
        if (!resolved && !state.stateGroups.empty()) return false;
        const auto group = resolved ? *resolved : reference;
        const auto key = bnkstate::identity(group.token,group.id,group.name);
        if (std::none_of(groups.begin(),groups.end(),[&](const auto& current)
            { return bnkstate::identity(current.token,current.id,current.name)==key; })) groups.push_back(group);
    }
    const BnkDocument::StateValueDescriptor* value = nullptr;
    for (const auto& candidate : state.stateValues)
    {
        if (!BoundedSidecarInput::continueWork()) return false;
        const bool matches = modifier.stateValueToken.isNotEmpty()
            ? candidate.token.isNotEmpty() && bnkstate::id(modifier.stateValueToken)==bnkstate::id(candidate.token)
            : modifier.stateName.isNotEmpty() && bnkstate::name(modifier.stateName)==bnkstate::name(candidate.name);
        if (!matches) continue;
        if (!groups.empty() && std::none_of(groups.begin(),groups.end(),[&](const auto& group)
            { return bnkstate::identity(group.token,group.id,group.name)==bnkstate::group(candidate); })) continue;
        if (value) return false;
        value=&candidate;
    }
    if (value)
    {
        modifier.stateGroupToken=value->groupToken; modifier.stateGroupId=value->groupId;
        modifier.stateGroupName=value->groupName; modifier.stateValueToken=value->token; modifier.stateName=value->name;
        return true;
    }
    if (!state.stateValues.empty() || groups.size()!=1) return false;
    modifier.stateGroupToken=groups[0].token; modifier.stateGroupId=groups[0].id; modifier.stateGroupName=groups[0].name;
    return modifier.stateValueToken.isNotEmpty() || modifier.stateName.isNotEmpty();
}

void readRtpcRange(const juce::XmlElement& element, BnkDocument::RtpcDescriptor& descriptor)
{
    descriptor.hasDefinition=true;
    descriptor.explicitRangeFields=static_cast<uint8_t>(
        ((element.hasAttribute("Min") || element.hasAttribute("MinValue")) ? 1 : 0)
        | ((element.hasAttribute("Max") || element.hasAttribute("MaxValue")) ? 2 : 0)
        | ((element.hasAttribute("InitialValue") || element.hasAttribute("DefaultValue")) ? 4 : 0));
    descriptor.minValue=parseOptionalDoubleAttribute(element,"Min","MinValue",0.0);
    descriptor.maxValue=parseOptionalDoubleAttribute(element,"Max","MaxValue",100.0);
    descriptor.defaultValue=parseOptionalDoubleAttribute(element,"InitialValue","DefaultValue",descriptor.minValue);
    validateParameterRange(descriptor);
}

void appendRtpcDefinition(BnkDocument::State& state, BnkDocument::RtpcDescriptor descriptor)
{
    const auto invalid=[] { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); };
    BnkDocument::RtpcDescriptor* match=nullptr;
    for (auto& current : state.rtpcs)
    {
        if (!BoundedSidecarInput::continueWork()) return;
        const auto sameGuid=descriptor.token.isNotEmpty() && current.token.isNotEmpty()
            && makeLookupKey(descriptor.token)==makeLookupKey(current.token);
        const auto sameId=descriptor.id!=0 && current.id==descriptor.id;
        const auto sameLegacy=descriptor.token.isEmpty() && current.token.isEmpty() && descriptor.id==0 && current.id==0
            && authoringNameKey(descriptor.name)==authoringNameKey(current.name)
            && authoringNameKey(descriptor.objectPath.replaceCharacter('/', '\\'))==authoringNameKey(current.objectPath.replaceCharacter('/', '\\'));
        if (!sameGuid && !sameId && !sameLegacy) continue;
        if (match) { invalid(); return; }
        match=&current;
    }
    if (match)
    {
        if ((match->token.isNotEmpty() && descriptor.token.isNotEmpty() && makeLookupKey(match->token)!=makeLookupKey(descriptor.token))
            || (match->id!=0 && descriptor.id!=0 && match->id!=descriptor.id)
            || (match->name.isNotEmpty() && descriptor.name.isNotEmpty() && authoringNameKey(match->name)!=authoringNameKey(descriptor.name))
            || (match->objectPath.isNotEmpty() && descriptor.objectPath.isNotEmpty()
                && authoringNameKey(match->objectPath.replaceCharacter('/', '\\'))!=authoringNameKey(descriptor.objectPath.replaceCharacter('/', '\\'))))
        { invalid(); return; }
        const auto merge=[&](uint8_t bit,double incoming,double& current)
        {
            if ((descriptor.explicitRangeFields & bit)==0) return;
            if ((match->explicitRangeFields & bit)!=0 && current!=incoming) invalid();
            current=incoming;
        };
        merge(1,descriptor.minValue,match->minValue); merge(2,descriptor.maxValue,match->maxValue);
        merge(4,descriptor.defaultValue,match->defaultValue);
        match->explicitRangeFields |= descriptor.explicitRangeFields;
        if ((match->explicitRangeFields & 4)==0) match->defaultValue=match->minValue;
        match->hasDefinition=true;
        if (match->token.isEmpty()) match->token=descriptor.token;
        if (match->id==0) match->id=descriptor.id;
        if (match->name.isEmpty()) match->name=descriptor.name;
        if (match->objectPath.isEmpty()) match->objectPath=descriptor.objectPath;
        validateParameterRange(*match); return;
    }
    if (state.rtpcs.size()>=4096)
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
    state.rtpcs.push_back(std::move(descriptor));
}

BnkDocument::RtpcDescriptor* findOrCreateRtpcDescriptor(BnkDocument::State& state,
                                                        const juce::String& token,
                                                        const juce::String& name)
{
    BnkDocument::RtpcDescriptor* found=nullptr;
    for (auto& current : state.rtpcs)
    {
        if (!BoundedSidecarInput::continueWork()) return nullptr;
        const auto matches=token.isNotEmpty() ? current.token.isNotEmpty() && makeLookupKey(token)==makeLookupKey(current.token)
            : current.hasDefinition && name.isNotEmpty() && authoringNameKey(name)==authoringNameKey(current.name);
        if (!matches) continue;
        if (found)
        { state.info.diagnostics.addIfNotAlreadyThere("RTPC control name is ambiguous; no parameter binding was guessed."); return nullptr; }
        found=&current;
    }
    if (found) return found;
    if (token.isEmpty())
        for (auto& current : state.rtpcs)
            if (!current.hasDefinition && current.token.isEmpty() && authoringNameKey(name)==authoringNameKey(current.name)) return &current;
    if (state.rtpcs.size()>=4096)
    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return nullptr; }

    BnkDocument::RtpcDescriptor descriptor;
    descriptor.token = token;
    descriptor.name = name;
    state.info.diagnostics.addIfNotAlreadyThere("RTPC reference has no parameter definition; control range is unavailable.");
    state.rtpcs.push_back(std::move(descriptor));
    return &state.rtpcs.back();
}

bool isRtpcAudioObjectTag(const juce::String& tagName)
{
    return tagName == "WorkUnit"
        || tagName == "ActorMixer"
        || tagName == "RandomSequenceContainer"
        || tagName == "SwitchContainer"
        || tagName == "BlendContainer"
        || tagName == "MusicSegment"
        || tagName == "MusicTrack"
        || tagName == "MusicSwitchContainer"
        || tagName == "MusicRandomSequenceContainer"
        || tagName == "Sound";
}

bool appendAuthoringPath(juce::String& path,const juce::String& rawName)
{
    if (rawName.isEmpty()) return true;
    const auto characters=static_cast<size_t>(path.length())+static_cast<size_t>(rawName.length())+1;
    if (!BoundedSidecarInput::admitDerivedText(characters,2)) return false;
    const auto name=rawName.trim();
    if (name.isNotEmpty()) path += "\\"+name;
    return true;
}

template <typename Callback>
void visitAuthoringObjects(const juce::XmlElement& element, juce::String path, Callback&& callback)
{
    if (!BoundedSidecarInput::continueWork()) return;
    const auto audioObject = isRtpcAudioObjectTag(element.getTagName());
    if ((audioObject || element.hasTagName("Folder"))
        && !appendAuthoringPath(path,element.getStringAttribute("Name"))) return;
    if (audioObject && !element.hasTagName("WorkUnit")) callback(element, path);
    forEachXmlChildElement(element, child)
        visitAuthoringObjects(*child, path, callback);
}

struct RtpcParseBudget
{
    size_t bindings=0, points=0;
    std::unordered_map<std::string,size_t> bindingIndices;
};

void appendRtpcBindingsFromObject(const juce::XmlElement& element,
                                  const juce::String& currentPath,
                                  BnkDocument::State& state, RtpcParseBudget& budget)
{
    const auto objectName = element.getStringAttribute("Name").trim();
    const auto objectId = element.getStringAttribute("ID").trim();
    auto* propertyList = uniqueChild(element,"PropertyList");
    if (propertyList == nullptr)
        return;

    forEachXmlChildElementWithTagName(*propertyList, propertyXml, "Property")
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto propertyName = propertyXml->getStringAttribute("Name").trim();
        auto* rtpcList = uniqueChild(*propertyXml,"RTPCList");
        if (rtpcList == nullptr)
            continue;

        forEachXmlChildElementWithTagName(*rtpcList, rtpcXml, "RTPC")
        {
            if (!BoundedSidecarInput::continueWork()) break;
            if (budget.bindings>=4096)
            { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
            ++budget.bindings;
            juce::String controlToken;
            juce::String controlName;
            bool foundControl=false;
            if (auto* referenceList = uniqueChild(*rtpcXml,"ReferenceList"))
            {
                forEachXmlChildElementWithTagName(*referenceList, referenceXml, "Reference")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    if (!referenceXml->getStringAttribute("Name").equalsIgnoreCase("ControlInput"))
                        continue;
                    if (foundControl)
                    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
                    foundControl=true;
                    if (auto* objectRef = uniqueChild(*referenceXml,"ObjectRef"))
                    {
                        controlToken = objectRef->getStringAttribute("ID").trim();
                        controlName = objectRef->getStringAttribute("Name").trim();
                    }
                }
            }

            if (controlToken.isEmpty() && controlName.isEmpty())
                continue;

            BnkDocument::RtpcDescriptor::Binding binding;
            binding.objectId = objectId;
            binding.objectName = objectName;
            binding.objectPath = currentPath;
            binding.propertyName = propertyName;

            if (auto* curveXml = uniqueChild(*rtpcXml,"Curve"))
            {
                if (auto* pointList = uniqueChild(*curveXml,"PointList"))
                {
                    forEachXmlChildElementWithTagName(*pointList, pointXml, "Point")
                    {
                        if (!BoundedSidecarInput::continueWork()) break;
                        if (budget.points>=65536)
                        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
                        ++budget.points;
                        BnkDocument::RtpcDescriptor::CurvePoint point;
                        point.x = parseDoubleChild(*pointXml, "XPos");
                        point.y = parseDoubleChild(*pointXml, "YPos");
                        binding.points.push_back(point);
                    }
                }
            }

            if (binding.points.empty())
                continue;

            auto* descriptor = findOrCreateRtpcDescriptor(state, controlToken, controlName);
            if (descriptor == nullptr)
                continue;

            std::string bindingKey;
            const auto field=[&](const std::string& value) { bindingKey+=std::to_string(value.size())+":"+value; };
            field(std::to_string(static_cast<size_t>(descriptor-state.rtpcs.data())));
            field(binding.objectId.isNotEmpty() ? "guid:"+makeLookupKey(binding.objectId)
                : "path:"+authoringNameKey(binding.objectPath.replaceCharacter('/', '\\')));
            field(authoringNameKey(binding.propertyName));
            const auto [entry,inserted]=budget.bindingIndices.emplace(std::move(bindingKey),descriptor->bindings.size());
            if (inserted)
                descriptor->bindings.push_back(std::move(binding));
            else
            {
                const auto& duplicate=descriptor->bindings[entry->second];
                if (authoringNameKey(duplicate.objectPath.replaceCharacter('/', '\\'))!=authoringNameKey(binding.objectPath.replaceCharacter('/', '\\'))
                    || duplicate.points.size()!=binding.points.size()
                    || !std::equal(duplicate.points.begin(),duplicate.points.end(),binding.points.begin(),
                        [](const auto& a,const auto& b) { return a.x==b.x && a.y==b.y; }))
                    BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
            }
        }
    }
}

void collectRtpcBindingsFromProjectElement(const juce::XmlElement& element,
                                           const juce::String& inheritedPath,
                                           BnkDocument::State& state, RtpcParseBudget& budget)
{
    if (!BoundedSidecarInput::continueWork()) return;
    auto currentPath = inheritedPath;
    const auto tagName = element.getTagName();
    const auto isAudioObject = isRtpcAudioObjectTag(tagName);

    if ((isAudioObject || element.hasTagName("Folder"))
        && !appendAuthoringPath(currentPath,element.getStringAttribute("Name"))) return;

    if (isAudioObject)
    {
        if (!element.hasTagName("WorkUnit")) appendRtpcBindingsFromObject(element, currentPath, state, budget);

        if (auto* children = element.getChildByName("ChildrenList"))
        {
            forEachXmlChildElement(*children, child)
                collectRtpcBindingsFromProjectElement(*child, currentPath, state, budget);
        }

        return;
    }

    forEachXmlChildElement(element, child)
        collectRtpcBindingsFromProjectElement(*child, currentPath, state, budget);
}

void enrichStateFromTextSidecar(const juce::File& bankFile, BnkDocument::State& state)
{
    const auto textSidecar = bankFile.withFileExtension(".txt");
    if (!textSidecar.existsAsFile())
        return;

    juce::StringArray lines;
    lines = juce::StringArray::fromLines(BoundedSidecarInput::text(textSidecar));

    struct Locations { juce::File source, generated; };
    std::unordered_map<uint32_t, Locations> externalMediaFiles;
    const auto recordLocation = [&](uint32_t id, bool sourceRole, const juce::File& file)
    {
        if (id == 0 || file == juce::File()) return;
        if (externalMediaFiles.find(id)==externalMediaFiles.end() && externalMediaFiles.size()>=65536)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
        auto& locations=externalMediaFiles[id];
        auto& target=sourceRole ? locations.source : locations.generated;
        if (target != juce::File() && target != file)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
        target=file;
    };
    TextSidecarSection section = TextSidecarSection::none;

    for (auto line : lines)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        line = line.trim();
        if (line.isEmpty())
            continue;

        if (line.startsWithIgnoreCase("Switch Group\t"))
        {
            section = TextSidecarSection::switchGroups;
            continue;
        }
        if (line.startsWithIgnoreCase("Switch\t"))
        {
            section = TextSidecarSection::switches;
            continue;
        }
        if (line.startsWithIgnoreCase("State Group\t"))
        {
            section = TextSidecarSection::stateGroups;
            continue;
        }
        if (line.startsWithIgnoreCase("State\t"))
        {
            section = TextSidecarSection::states;
            continue;
        }
        if (line.startsWithIgnoreCase("Custom State\t"))
        {
            section = TextSidecarSection::customStates;
            continue;
        }
        if (line.startsWithIgnoreCase("In Memory Audio\t"))
        {
            section = TextSidecarSection::inMemoryAudio;
            continue;
        }
        if (line.startsWithIgnoreCase("Streamed Audio\t"))
        {
            section = TextSidecarSection::streamedAudio;
            continue;
        }

        const auto columns = splitSidecarColumns(line);
        if (columns.isEmpty())
            continue;

        if (columns[0].containsOnly("EventIDNameWwiseObjectPathNotes"))
            continue;

        switch (section)
        {
            case TextSidecarSection::switches:
            {
                if (columns.size() < 3)
                    break;

                BnkDocument::SwitchValueDescriptor descriptor;
                descriptor.id = parseIdText(columns[0]);
                descriptor.token = columns[0];
                descriptor.name = columns[1];
                descriptor.groupName = columns[2];
                appendUniqueByToken(state.switchValues, std::move(descriptor));
                break;
            }
            case TextSidecarSection::states:
            case TextSidecarSection::customStates:
            {
                if (columns.size() < 3)
                    break;

                BnkDocument::StateValueDescriptor descriptor;
                const auto textId = parseIdText(columns[0]);
                descriptor.id = section == TextSidecarSection::states ? textId : 0;
                descriptor.name = columns[1];
                descriptor.groupName = columns[2];
                appendStateValue(state, std::move(descriptor));
                break;
            }
            case TextSidecarSection::inMemoryAudio:
            {
                if (columns.size() < 3)
                    break;

                const auto mediaId = parseIdText(columns[0]);
                const auto allowedRoot = state.projectRoot.isDirectory() ? state.projectRoot : bankFile.getParentDirectory();
                const auto sourcePath = BoundedSidecarInput::withinRoot(allowedRoot, columns[2]);
                recordLocation(mediaId,true,sourcePath);
                break;
            }
            case TextSidecarSection::streamedAudio:
            {
                if (columns.size() < 4)
                    break;

                const auto mediaId = parseIdText(columns[0]);
                const auto allowedRoot = state.projectRoot.isDirectory() ? state.projectRoot : bankFile.getParentDirectory();
                recordLocation(mediaId,true,BoundedSidecarInput::withinRoot(allowedRoot,columns[2]));
                auto generatedName = columns[3];
                if (generatedName.isEmpty())
                    break;

                const auto generatedFile = BoundedSidecarInput::withinRoot(bankFile.getParentDirectory(), generatedName);
                recordLocation(mediaId,false,generatedFile);
                break;
            }
            default:
                break;
        }
    }

    if (!externalMediaFiles.empty())
    {
        for (auto& event : state.events)
        {
            if (!BoundedSidecarInput::continueWork()) break;
            for (auto& media : event.media)
            {
                if (!BoundedSidecarInput::continueWork()) break;
                if (const auto found = externalMediaFiles.find(media.mediaId); found != externalMediaFiles.end())
                {
                    if (found->second.source != juce::File()) media.sourceFile = found->second.source;
                    if (found->second.generated != juce::File())
                    {
                        if (media.relativePath.isNotEmpty() && media.mediaFile.existsAsFile()
                            && media.mediaFile != found->second.generated)
                        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
                        media.mediaFile = found->second.generated;
                    }
                }
            }
        }
    }
}

void enrichSwitchesFromProject(const juce::File& projectRoot, BnkDocument::State& state)
{
    const auto switchesRoot = projectRoot.getChildFile("Switches");
    if (!switchesRoot.isDirectory())
        return;

    for (const auto& sidecarFile : BoundedSidecarInput::files(switchesRoot, "*.wwu"))
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto xml = BoundedSidecarInput::xml(sidecarFile);
        if (xml == nullptr)
            continue;

        visitXmlTree(*xml, [&state](const juce::XmlElement& element)
        {
            if (!element.hasTagName("SwitchGroup"))
                return;

            const auto groupName = element.getStringAttribute("Name").trim();
            if (auto* children = element.getChildByName("ChildrenList"))
            {
                forEachXmlChildElementWithTagName(*children, switchXml, "Switch")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    BnkDocument::SwitchValueDescriptor descriptor;
                    descriptor.token = switchXml->getStringAttribute("ID").trim();
                    descriptor.name = switchXml->getStringAttribute("Name").trim();
                    descriptor.groupName = groupName;
                    appendUniqueByToken(state.switchValues, std::move(descriptor));
                }
            }
        });
    }
}

void enrichStatesFromProject(const juce::File& projectRoot, BnkDocument::State& state)
{
    const auto statesRoot = projectRoot.getChildFile("States");
    if (!statesRoot.isDirectory())
        return;

    for (const auto& sidecarFile : BoundedSidecarInput::files(statesRoot, "*.wwu"))
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto xml = BoundedSidecarInput::xml(sidecarFile);
        if (xml == nullptr)
            continue;

        visitXmlTree(*xml, [&state](const juce::XmlElement& element)
        {
            if (!element.hasTagName("StateGroup"))
                return;

            const auto groupName = element.getStringAttribute("Name").trim();
            const auto groupToken = element.getStringAttribute("ID").trim();
            const auto groupId = parseAttributeId(element, "ShortID");
            appendStateGroup(state, {groupId, groupToken, groupName});
            if (auto* children = element.getChildByName("ChildrenList"))
            {
                forEachXmlChildElementWithTagName(*children, stateXml, "State")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    BnkDocument::StateValueDescriptor descriptor;
                    descriptor.token = stateXml->getStringAttribute("ID").trim();
                    descriptor.name = stateXml->getStringAttribute("Name").trim();
                    descriptor.id = parseAttributeId(*stateXml, "ShortID");
                    descriptor.groupToken = groupToken;
                    descriptor.groupId = groupId;
                    descriptor.groupName = groupName;
                    appendStateValue(state, std::move(descriptor));
                }
            }
        });
    }
}

void enrichRtpcsFromProject(const juce::File& projectRoot, BnkDocument::State& state)
{
    const auto rtpcRoot = projectRoot.getChildFile("Game Parameters");
    if (rtpcRoot.isDirectory())
    {
        for (const auto& sidecarFile : BoundedSidecarInput::files(rtpcRoot, "*.wwu"))
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto xml = BoundedSidecarInput::xml(sidecarFile);
            if (xml == nullptr)
                continue;

            visitXmlTree(*xml, [&state](const juce::XmlElement& element)
            {
                if (!element.hasTagName("GameParameter"))
                    return;

                BnkDocument::RtpcDescriptor descriptor;
                descriptor.id = parseAttributeId(element,"ShortID");
                descriptor.token = element.getStringAttribute("ID").trim();
                descriptor.name = element.getStringAttribute("Name").trim();
                descriptor.objectPath = element.getStringAttribute("ObjectPath").trim();
                readRtpcRange(element,descriptor);
                appendRtpcDefinition(state,std::move(descriptor));
            });
        }
    }

    RtpcParseBudget budget;
    auto collectBindings = [&state, &budget](const juce::File& root)
    {
        if (!root.isDirectory())
            return;

        for (const auto& sidecarFile : BoundedSidecarInput::files(root, "*.wwu"))
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto xml = BoundedSidecarInput::xml(sidecarFile);
            if (xml == nullptr)
                continue;

            collectRtpcBindingsFromProjectElement(*xml, "\\"+root.getFileName(), state, budget);
        }
    };

    collectBindings(projectRoot.getChildFile("Actor-Mixer Hierarchy"));
    collectBindings(projectRoot.getChildFile("Interactive Music Hierarchy"));
}

void collectEventTargetsFromProject(const juce::File& projectRoot, EventTargetLookup& lookup)
{
    const auto eventsRoot = projectRoot.getChildFile("Events");
    if (!eventsRoot.isDirectory())
        return;

    for (const auto& sidecarFile : BoundedSidecarInput::files(eventsRoot, "*.wwu"))
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto xml = BoundedSidecarInput::xml(sidecarFile);
        if (xml == nullptr)
            continue;

        visitXmlTree(*xml, [&lookup](const juce::XmlElement& element)
        {
            if (!element.hasTagName("Event"))
                return;

            AuthoringEventInfo info;
            info.id = element.getStringAttribute("ID").trim();
            info.name = element.getStringAttribute("Name").trim();
            info.shortId = parseAttributeId(element, "ShortID");
            if (auto* children = uniqueChild(element, "ChildrenList"))
            {
                forEachXmlChildElementWithTagName(*children, actionXml, "Action")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    bool actionHasTarget = false;
                    if (auto* references = uniqueChild(*actionXml, "ReferenceList"))
                    {
                        forEachXmlChildElementWithTagName(*references, referenceXml, "Reference")
                        {
                            if (!BoundedSidecarInput::continueWork()) break;
                            if (!referenceXml->getStringAttribute("Name").equalsIgnoreCase("Target"))
                                continue;

                            if (auto* objectRef = uniqueChild(*referenceXml, "ObjectRef"))
                            {
                                EventTargetInfo target { objectRef->getStringAttribute("ID").trim(),
                                                         objectRef->getStringAttribute("Name").trim() };
                                if (target.objectId.isNotEmpty() || target.objectName.isNotEmpty())
                                {
                                    if (info.targets.size() >= 4096)
                                    { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
                                    if (!lookup.admitReference()) return;
                                    info.targets.push_back(std::move(target)); actionHasTarget = true;
                                }
                            }
                        }
                    }
                    info.complete = info.complete && actionHasTarget;
                }
            }
            lookup.add(std::move(info));
        });
    }
}

void collectObjectStateModifiersFromProject(const juce::File& directory, ObjectStateLookup& lookup, BnkDocument::State& state)
{
    if (!directory.isDirectory())
        return;

    for (const auto& sidecarFile : BoundedSidecarInput::files(directory, "*.wwu"))
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto xml = BoundedSidecarInput::xml(sidecarFile);
        if (xml == nullptr)
            continue;

        visitAuthoringObjects(*xml, "\\" + directory.getFileName(), [&lookup, &state](const juce::XmlElement& element, const juce::String& path)
        {
            if (!element.hasAttribute("ID") || !element.hasAttribute("Name"))
                return;

            AuthoringObjectInfo info;
            info.id = element.getStringAttribute("ID").trim();
            info.name = element.getStringAttribute("Name").trim();
            info.path = path;
            auto* stateInfo = uniqueChild(element, "StateInfo");
            if (stateInfo == nullptr)
            { lookup.add(std::move(info)); return; }

            auto* customStateList = uniqueChild(*stateInfo, "CustomStateList");
            if (customStateList == nullptr)
            { lookup.add(std::move(info)); return; }

            std::vector<BnkDocument::StateGroupDescriptor> groupReferences;
            if (auto* stateGroupList = uniqueChild(*stateInfo, "StateGroupList"))
            {
                forEachXmlChildElementWithTagName(*stateGroupList, stateGroupInfoXml, "StateGroupInfo")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    if (auto* stateGroupRef = uniqueChild(*stateGroupInfoXml, "StateGroupRef"))
                    {
                        if (groupReferences.size() >= 4096)
                        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
                        groupReferences.push_back({0, stateGroupRef->getStringAttribute("ID").trim(),
                            stateGroupRef->getStringAttribute("Name").trim()});
                    }
                }
            }

            std::vector<BnkDocument::EventDescriptor::StateModifier> modifiers;
            forEachXmlChildElementWithTagName(*customStateList, customStateXml, "CustomState")
            {
                if (!BoundedSidecarInput::continueWork()) break;
                BnkDocument::EventDescriptor::StateModifier modifier;

                if (auto* stateRef = uniqueChild(*customStateXml, "StateRef"))
                {
                    modifier.stateValueToken = stateRef->getStringAttribute("ID").trim();
                    modifier.stateName = stateRef->getStringAttribute("Name").trim();
                }

                if (auto* nestedCustomState = uniqueChild(*customStateXml, "CustomState"))
                {
                    if (auto* propertyList = uniqueChild(*nestedCustomState, "PropertyList"))
                    {
                        bool foundVolume = false;
                        forEachXmlChildElementWithTagName(*propertyList, propertyXml, "Property")
                        {
                            if (!BoundedSidecarInput::continueWork()) break;
                            if (!propertyXml->getStringAttribute("Name").equalsIgnoreCase("Volume"))
                                continue;

                            if (foundVolume)
                            { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); break; }
                            foundVolume = true;
                            const auto rawValue = readPropertyScalarValue(*propertyXml);
                            if (rawValue)
                            {
                                if (std::abs(*rawValue) > static_cast<double>(std::numeric_limits<float>::max()))
                                { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); break; }
                                modifier.volumeDb = static_cast<float>(*rawValue);
                                if ((*rawValue != 0.0 && modifier.volumeDb == 0.0f)
                                    || !std::isfinite(std::pow(10.0f, modifier.volumeDb / 20.0f)))
                                { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); break; }
                                modifier.hasVolume = true;
                            }
                        }
                    }
                }

                if (modifier.stateValueToken.isNotEmpty() || modifier.stateName.isNotEmpty())
                {
                    if (!lookup.admitReference()) return;
                    if (resolveStateModifier(modifier, groupReferences, state))
                    {
                        const auto duplicate = std::find_if(modifiers.begin(),modifiers.end(),[&](const auto& current)
                        {
                            return bnkstate::group(current)==bnkstate::group(modifier)
                                && bnkstate::identity(current.stateValueToken,0,current.stateName)
                                    ==bnkstate::identity(modifier.stateValueToken,0,modifier.stateName);
                        });
                        if (duplicate==modifiers.end()) modifiers.push_back(std::move(modifier));
                        else if (duplicate->hasVolume!=modifier.hasVolume || duplicate->volumeDb!=modifier.volumeDb)
                            BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
                    }
                    else info.modifiersResolved = false;
                }
                else info.modifiersResolved = false;
            }

            info.modifiers = std::move(modifiers);
            lookup.add(std::move(info));
        });
    }
}

void enrichEventStateModifiersFromProject(const juce::File& projectRoot, BnkDocument::State& state)
{
    if (!projectRoot.isDirectory() || state.events.empty())
        return;

    EventTargetLookup eventTargets(4096);
    collectEventTargetsFromProject(projectRoot, eventTargets);

    ObjectStateLookup objectStates(65536, 16384);
    collectObjectStateModifiersFromProject(projectRoot.getChildFile("Actor-Mixer Hierarchy"), objectStates, state);
    collectObjectStateModifiersFromProject(projectRoot.getChildFile("Interactive Music Hierarchy"), objectStates, state);

    if (eventTargets.empty())
        return;

    for (auto& event : state.events)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto* authoringEvent = event.authoringId.isNotEmpty() ? eventTargets.byId(event.authoringId)
                                                                  : eventTargets.byShortId(event.id);
        if (!authoringEvent && event.authoringId.isEmpty())
        {
            authoringEvent = eventTargets.byName(event.displayName);
            if (authoringEvent && authoringEvent->shortId != 0 && authoringEvent->shortId != event.id)
                authoringEvent = nullptr;
        }
        if (!authoringEvent)
        {
            if (event.authoringId.isNotEmpty() || eventTargets.hasName(event.displayName)) unavailableStateTarget(event, state);
            continue;
        }
        const AuthoringObjectInfo* owner = nullptr;
        bool resolved = authoringEvent->complete && !authoringEvent->targets.empty();
        for (const auto& target : authoringEvent->targets)
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto* object = objectStates.target(target);
            if (!object || (owner && owner != object)) { resolved = false; break; }
            owner = object;
        }
        if (!resolved || !owner) { unavailableStateTarget(event, state); continue; }
        event.targetObjectId = owner->id; event.targetObjectName = owner->name;
        event.stateTargetScope = BnkDocument::EventDescriptor::StateTargetScope::singleObject;
        event.stateModifiers = owner->modifiers;
        event.stateModifiersResolved = owner->modifiersResolved;
        if (!event.stateModifiersResolved) state.info.diagnostics.add("State group/value reference is unresolved; media remains accessible.");
    }
}

void parseTextSidecarStateLinks(const juce::File& bankFile, TextSidecarStateLinkData& data)
{
    const auto textSidecar = bankFile.withFileExtension(".txt");
    if (!textSidecar.existsAsFile())
        return;

    juce::StringArray lines;
    lines = juce::StringArray::fromLines(BoundedSidecarInput::text(textSidecar));

    const auto recordOwner = [&](uint32_t mediaId, const juce::String& path)
    {
        if (mediaId == 0 || path.isEmpty()) return;
        const auto canonical = juce::String(authoringNameKey(path.replaceCharacter('/', '\\')));
        const auto [found, inserted] = data.mediaObjectPaths.emplace(mediaId, canonical);
        if (!inserted && found->second != canonical)
            BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
    };

    TextSidecarSection section = TextSidecarSection::none;
    for (auto line : lines)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        line = line.trim();
        if (line.isEmpty())
            continue;

        if (line.startsWithIgnoreCase("Switch Group\t"))
        {
            section = TextSidecarSection::switchGroups;
            continue;
        }
        if (line.startsWithIgnoreCase("Switch\t"))
        {
            section = TextSidecarSection::switches;
            continue;
        }
        if (line.startsWithIgnoreCase("State Group\t"))
        {
            section = TextSidecarSection::stateGroups;
            continue;
        }
        if (line.startsWithIgnoreCase("State\t"))
        {
            section = TextSidecarSection::states;
            continue;
        }
        if (line.startsWithIgnoreCase("Custom State\t"))
        {
            section = TextSidecarSection::customStates;
            continue;
        }
        if (line.startsWithIgnoreCase("In Memory Audio\t"))
        {
            section = TextSidecarSection::inMemoryAudio;
            continue;
        }
        if (line.startsWithIgnoreCase("Streamed Audio\t"))
        {
            section = TextSidecarSection::streamedAudio;
            continue;
        }

        const auto columns = splitSidecarColumns(line);
        if (columns.isEmpty())
            continue;

        switch (section)
        {
            case TextSidecarSection::customStates:
            {
                if (columns.size() < 4)
                    break;

                BnkDocument::EventDescriptor::StateModifier modifier;
                modifier.stateName = columns[1];
                modifier.stateGroupName = columns[2];
                const auto ownerPath = columns[3];
                if (ownerPath.isNotEmpty() && modifier.stateName.isNotEmpty())
                    data.ownerStates[authoringNameKey(ownerPath.replaceCharacter('/', '\\'))].push_back(std::move(modifier));
                break;
            }
            case TextSidecarSection::inMemoryAudio:
            {
                if (columns.size() < 4)
                    break;

                const auto mediaId = parseIdText(columns[0]);
                const auto ownerPath = columns[3];
                recordOwner(mediaId, ownerPath);
                break;
            }
            case TextSidecarSection::streamedAudio:
            {
                if (columns.size() < 5)
                    break;

                const auto mediaId = parseIdText(columns[0]);
                const auto ownerPath = columns[4];
                recordOwner(mediaId, ownerPath);
                break;
            }
            default:
                break;
        }
    }
}

void applyEventStateFallbackFromTextSidecar(const juce::File& bankFile,
                                            const juce::File& projectRoot,
                                            BnkDocument::State& state)
{
    TextSidecarStateLinkData textData;
    parseTextSidecarStateLinks(bankFile, textData);
    if (textData.mediaObjectPaths.empty() && textData.ownerStates.empty())
        return;

    if (std::none_of(state.events.begin(), state.events.end(), [](const auto& event)
        { return event.stateTargetScope == BnkDocument::EventDescriptor::StateTargetScope::unspecified; })) return;

    ObjectStateLookup objectStates(65536, 16384);
    if (projectRoot.isDirectory())
    {
        collectObjectStateModifiersFromProject(projectRoot.getChildFile("Actor-Mixer Hierarchy"), objectStates, state);
        collectObjectStateModifiersFromProject(projectRoot.getChildFile("Interactive Music Hierarchy"), objectStates, state);
    }

    for (auto& event : state.events)
    {
        if (!BoundedSidecarInput::continueWork()) break;
        if (event.stateTargetScope != BnkDocument::EventDescriptor::StateTargetScope::unspecified) continue;
        juce::StringArray ownerPaths;
        bool completeOwners = true;
        for (const auto& media : event.media)
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto foundOwner = textData.mediaObjectPaths.find(media.mediaId);
            if (foundOwner != textData.mediaObjectPaths.end())
                ownerPaths.addIfNotAlreadyThere(foundOwner->second);
            else completeOwners = false;
        }

        if (ownerPaths.isEmpty())
            continue;

        if (!completeOwners || ownerPaths.size() != 1)
        { unavailableStateTarget(event, state); continue; }

        const auto ownerPath = ownerPaths[0];
        const auto ownerName = objectNameFromPath(ownerPath);
        if (event.targetObjectName.isEmpty() && ownerName.isNotEmpty())
            event.targetObjectName = ownerName;

        if (!event.stateModifiers.empty())
            continue;

        const auto* owner = ownerPath.containsChar('\\') ? objectStates.byPath(ownerPath) : objectStates.byName(ownerName);
        if (owner)
        {
            event.targetObjectId = owner->id; event.targetObjectName = owner->name;
            event.stateTargetScope = BnkDocument::EventDescriptor::StateTargetScope::singleObject;
            event.stateModifiers = owner->modifiers;
            event.stateModifiersResolved = owner->modifiersResolved;
            continue;
        }

        const auto textStatesIt = textData.ownerStates.find(authoringNameKey(ownerPath));
        if (textStatesIt != textData.ownerStates.end())
        {
            event.stateModifiers = textStatesIt->second;
            for (auto& modifier : event.stateModifiers)
                if (!resolveStateModifier(modifier, {{0, {}, modifier.stateGroupName}}, state)) event.stateModifiersResolved=false;
        }
    }
}
} // namespace

namespace
{
void enrichSidecarCandidate(const juce::File& bankFile, BnkDocument::State& state,
                            const juce::File& projectRootOverride)
{
    if (!BoundedSidecarInput::continueWork()) return;

    if (projectRootOverride.isDirectory())
    {
        state.projectRoot = projectRootOverride;
        if (!state.sourceFilesRoot.isDirectory())
            state.sourceFilesRoot = projectRootOverride.getChildFile("Originals");
        if (!state.soundBanksRoot.isDirectory())
            state.soundBanksRoot = projectRootOverride.getChildFile("GeneratedSoundBanks");
    }

    const auto bankDirectory = bankFile.getParentDirectory();
    enrichStateFromTextSidecar(bankFile, state);
    if (!BoundedSidecarInput::continueWork()) return;
    const auto infoFile = bankDirectory.getChildFile("SoundbanksInfo.xml");
    if (!infoFile.existsAsFile())
    {
        if (state.projectRoot.isDirectory())
        {
            enrichSwitchesFromProject(state.projectRoot, state);
            enrichStatesFromProject(state.projectRoot, state);
            enrichRtpcsFromProject(state.projectRoot, state);
            enrichEventStateModifiersFromProject(state.projectRoot, state);
        }
        else if (const auto discoveredProject = findProjectRootNearBank(bankFile); discoveredProject.isDirectory())
        {
            state.projectRoot = discoveredProject;
            if (!state.sourceFilesRoot.isDirectory())
                state.sourceFilesRoot = discoveredProject.getChildFile("Originals");
            if (!state.soundBanksRoot.isDirectory())
                state.soundBanksRoot = discoveredProject.getChildFile("GeneratedSoundBanks");
            enrichSwitchesFromProject(state.projectRoot, state);
            enrichStatesFromProject(state.projectRoot, state);
            enrichRtpcsFromProject(state.projectRoot, state);
            enrichEventStateModifiersFromProject(state.projectRoot, state);
        }
        enrichStateFromTextSidecar(bankFile, state);
        applyEventStateFallbackFromTextSidecar(bankFile, state.projectRoot, state);
        return;
    }

    const auto xml = BoundedSidecarInput::xml(infoFile);
    if (xml == nullptr || !xml->hasTagName("SoundBanksInfo"))
    {
        BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
        return;
    }

    if (auto* rootPaths = uniqueChild(*xml, "RootPaths"))
    {
        // XML is metadata, not permission to scan an arbitrary absolute root.
        // Use the explicit project or one discovered beside the selected bank.
        if (!state.projectRoot.isDirectory())
            state.projectRoot = findProjectRootNearBank(bankFile);
        const auto allowedRoot = state.projectRoot.isDirectory() ? state.projectRoot : bankDirectory;
        if (!state.sourceFilesRoot.isDirectory())
            state.sourceFilesRoot = BoundedSidecarInput::withinRoot(allowedRoot, childText(*rootPaths, "SourceFilesRoot"));
        if (!state.soundBanksRoot.isDirectory())
            state.soundBanksRoot = BoundedSidecarInput::withinRoot(allowedRoot, childText(*rootPaths, "SoundBanksRoot"));
    }

    auto* soundBanks = uniqueChild(*xml, "SoundBanks");
    if (soundBanks == nullptr)
    {
        if (state.projectRoot.isDirectory())
            enrichEventStateModifiersFromProject(state.projectRoot, state);
        applyEventStateFallbackFromTextSidecar(bankFile, state.projectRoot, state);
        return;
    }

    juce::XmlElement* matchingSoundBank = nullptr;
    forEachXmlChildElementWithTagName(*soundBanks, candidate, "SoundBank")
    {
        if (!BoundedSidecarInput::continueWork()) break;
        if (childText(*candidate, "Path").equalsIgnoreCase(bankFile.getFileName()))
        {
            if (matchingSoundBank != nullptr)
            { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
            matchingSoundBank = candidate;
        }
    }

    if (matchingSoundBank == nullptr)
    {
        if (state.projectRoot.isDirectory())
        {
            enrichSwitchesFromProject(state.projectRoot, state);
            enrichStatesFromProject(state.projectRoot, state);
            enrichRtpcsFromProject(state.projectRoot, state);
            enrichEventStateModifiersFromProject(state.projectRoot, state);
        }
        else if (const auto discoveredProject = findProjectRootNearBank(bankFile); discoveredProject.isDirectory())
        {
            state.projectRoot = discoveredProject;
            if (!state.sourceFilesRoot.isDirectory())
                state.sourceFilesRoot = discoveredProject.getChildFile("Originals");
            if (!state.soundBanksRoot.isDirectory())
                state.soundBanksRoot = discoveredProject.getChildFile("GeneratedSoundBanks");
            enrichSwitchesFromProject(state.projectRoot, state);
            enrichStatesFromProject(state.projectRoot, state);
            enrichRtpcsFromProject(state.projectRoot, state);
            enrichEventStateModifiersFromProject(state.projectRoot, state);
        }
        enrichStateFromTextSidecar(bankFile, state);
        applyEventStateFallbackFromTextSidecar(bankFile, state.projectRoot, state);
        return;
    }

    if (auto* switchGroups = uniqueChild(*matchingSoundBank, "SwitchGroups"))
    {
        forEachXmlChildElementWithTagName(*switchGroups, switchGroupXml, "SwitchGroup")
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto groupName = switchGroupXml->getStringAttribute("Name");
            if (auto* switches = uniqueChild(*switchGroupXml, "Switches"))
            {
                forEachXmlChildElementWithTagName(*switches, switchXml, "Switch")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    BnkDocument::SwitchValueDescriptor descriptor;
                    descriptor.id = parseAttributeId(*switchXml, "Id");
                    descriptor.token = switchXml->getStringAttribute("GUID").trim();
                    descriptor.name = switchXml->getStringAttribute("Name").trim();
                    descriptor.groupName = groupName;
                    if (descriptor.token.isNotEmpty())
                        state.switchValues.push_back(std::move(descriptor));
                }
            }
        }
    }

    if (auto* stateGroups = uniqueChild(*matchingSoundBank, "StateGroups"))
    {
        state.stateGroups.clear(); state.stateValues.clear();
        forEachXmlChildElementWithTagName(*stateGroups, stateGroupXml, "StateGroup")
        {
            if (!BoundedSidecarInput::continueWork()) break;
            const auto groupName = stateGroupXml->getStringAttribute("Name");
            const auto groupToken = stateGroupXml->getStringAttribute("GUID").trim();
            const auto groupId = parseAttributeId(*stateGroupXml, "Id");
            appendStateGroup(state, {groupId, groupToken, groupName});
            if (auto* states = uniqueChild(*stateGroupXml, "States"))
            {
                forEachXmlChildElementWithTagName(*states, stateXml, "State")
                {
                    if (!BoundedSidecarInput::continueWork()) break;
                    BnkDocument::StateValueDescriptor descriptor;
                    descriptor.id = parseAttributeId(*stateXml, "Id");
                    descriptor.token = stateXml->getStringAttribute("GUID").trim();
                    descriptor.name = stateXml->getStringAttribute("Name").trim();
                    descriptor.groupToken = groupToken;
                    descriptor.groupId = groupId;
                    descriptor.groupName = groupName;
                    if (descriptor.token.isNotEmpty() || descriptor.name.isNotEmpty())
                        appendStateValue(state, std::move(descriptor));
                }
            }
        }
    }

    if (auto* gameParameters = uniqueChild(*matchingSoundBank, "GameParameters"))
    {
        forEachXmlChildElementWithTagName(*gameParameters, gameParamXml, "GameParameter")
        {
            if (!BoundedSidecarInput::continueWork()) break;
            BnkDocument::RtpcDescriptor descriptor;
            descriptor.id = parseAttributeId(*gameParamXml, "Id");
            descriptor.token = gameParamXml->getStringAttribute("GUID").trim();
            descriptor.name = gameParamXml->getStringAttribute("Name").trim();
            descriptor.objectPath = gameParamXml->getStringAttribute("ObjectPath").trim();
            readRtpcRange(*gameParamXml,descriptor);
            if (descriptor.name.isNotEmpty() || descriptor.token.isNotEmpty())
                appendRtpcDefinition(state,std::move(descriptor));
        }
    }

    std::unordered_map<uint32_t, size_t> eventLookup;
    eventLookup.reserve(state.events.size());
    for (size_t index = 0; index < state.events.size(); ++index)
        eventLookup.emplace(state.events[index].id, index);

    std::unordered_set<uint32_t> sidecarEventIds;
    std::unordered_map<uint32_t, BnkDocument::EventDescriptor::MediaReference> bankMedia;
    const auto sameMedia = [](const auto& a, const auto& b)
    { return a.shortName == b.shortName && a.relativePath == b.relativePath
          && a.mediaFile == b.mediaFile && a.sourceFile == b.sourceFile; };
    auto* includedEvents = uniqueChild(*matchingSoundBank, "IncludedEvents");
    if (includedEvents == nullptr)
        return;

    forEachXmlChildElementWithTagName(*includedEvents, eventXml, "Event")
    {
        if (!BoundedSidecarInput::continueWork()) break;
        const auto eventId = parseAttributeId(*eventXml, "Id");
        if (eventId == 0)
            continue;
        if (!sidecarEventIds.insert(eventId).second)
        { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }

        auto found = eventLookup.find(eventId);
        if (found == eventLookup.end())
        {
            if (state.events.size() >= 4096)
            { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::hardLimit); return; }
            BnkDocument::EventDescriptor descriptor;
            descriptor.id = eventId;
            descriptor.displayName = eventXml->getStringAttribute("Name");
            descriptor.summary = "Recovered from SoundbanksInfo.xml";
            state.events.push_back(std::move(descriptor));
            eventLookup.emplace(eventId, state.events.size() - 1);
            found = eventLookup.find(eventId);
        }

        auto& event = state.events[found->second];
        event.authoringId = eventXml->getStringAttribute("GUID").trim();
        event.targetObjectId.clear(); event.targetObjectName.clear(); event.stateModifiers.clear();
        event.stateTargetScope = BnkDocument::EventDescriptor::StateTargetScope::unspecified;
        event.stateModifiersResolved = true;
        if (const auto xmlName = eventXml->getStringAttribute("Name"); xmlName.isNotEmpty())
            event.displayName = xmlName;

        event.durationMinSeconds = event.durationMaxSeconds = 0.0;
        if (eventXml->hasAttribute("DurationType")
            && !eventXml->getStringAttribute("DurationType").trim().equalsIgnoreCase("OneShot"))
        {
            state.info.diagnostics.addIfNotAlreadyThere(
                "Event duration type is non-finite or unrecognized; its finite duration hint is unavailable.");
        }
        else
        {
            event.durationMinSeconds = parseDoubleAttribute(*eventXml, "DurationMin", 0.0);
            event.durationMaxSeconds = parseDoubleAttribute(*eventXml, "DurationMax", event.durationMinSeconds);
            if (event.durationMinSeconds < 0.0 || event.durationMaxSeconds < event.durationMinSeconds)
                BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData);
        }

        event.media.clear();

        std::unordered_map<uint32_t, BnkDocument::EventDescriptor::MediaReference> mediaById;
        std::vector<uint32_t> mediaOrder;
        auto appendMediaFiles = [&](juce::XmlElement* parent)
        {
            if (parent == nullptr)
                return;

            forEachXmlChildElementWithTagName(*parent, fileXml, "File")
            {
                if (!BoundedSidecarInput::continueWork()) break;
                BnkDocument::EventDescriptor::MediaReference media;
                media.mediaId = parseAttributeId(*fileXml, "Id");
                if (media.mediaId == 0)
                { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
                media.shortName = childText(*fileXml, "ShortName");
                media.displayName = media.shortName.isNotEmpty() ? media.shortName.upToLastOccurrenceOf(".", false, false)
                                                                 : ("Media " + juce::String(media.mediaId));
                media.relativePath = childText(*fileXml, "Path");
                fillMediaPath(media, state.sourceFilesRoot, state.soundBanksRoot, bankFile.getParentDirectory());
                const auto [known, firstDefinition] = bankMedia.emplace(media.mediaId, media);
                if (!firstDefinition && !sameMedia(known->second, media))
                { BoundedSidecarInput::reject(BoundedSidecarInput::Failure::invalidData); return; }
                // Identical repeated references do not multiply derived media.
                if (mediaById.find(media.mediaId) != mediaById.end()) continue;
                mediaOrder.push_back(media.mediaId);
                mediaById.emplace(media.mediaId, std::move(media));
            }
        };
        appendMediaFiles(uniqueChild(*eventXml, "IncludedMemoryFiles"));
        appendMediaFiles(uniqueChild(*eventXml, "ReferencedStreamedFiles"));

        const auto defaultSwitchValue = childText(*eventXml, "DefaultSwitchValue");
        if (auto* switchContainers = uniqueChild(*eventXml, "SwitchContainers"))
        {
            forEachXmlChildElementWithTagName(*switchContainers, switchXml, "SwitchContainer")
            {
                if (!BoundedSidecarInput::continueWork()) break;
                const auto switchValue = switchXml->getStringAttribute("SwitchValue").trim();
                if (auto* mediaXml = uniqueChild(*switchXml, "Media"))
                {
                    forEachXmlChildElementWithTagName(*mediaXml, fileRefXml, "File")
                    {
                        if (!BoundedSidecarInput::continueWork()) break;
                        const auto mediaId = parseAttributeId(*fileRefXml, "Id");
                        if (auto mediaFound = mediaById.find(mediaId); mediaFound != mediaById.end())
                        {
                            auto media = mediaFound->second;
                            media.switchValueToken = switchValue;
                            media.isDefaultSwitch = (switchValue == defaultSwitchValue);
                            event.media.push_back(std::move(media));
                        }
                    }
                }
            }
        }

        if (event.media.empty())
        {
            event.media.reserve(mediaOrder.size());
            for (const auto mediaId : mediaOrder)
            {
                if (!BoundedSidecarInput::continueWork()) break;
                if (const auto foundMedia = mediaById.find(mediaId); foundMedia != mediaById.end())
                    event.media.push_back(foundMedia->second);
            }
        }

        applyPlaybackShape(event);
    }

    std::sort(state.events.begin(),
              state.events.end(),
              [](const BnkDocument::EventDescriptor& a, const BnkDocument::EventDescriptor& b)
              {
                  return a.id < b.id;
              });

    if (state.projectRoot.isDirectory())
    {
        if (state.switchValues.empty())
            enrichSwitchesFromProject(state.projectRoot, state);
        if (state.stateValues.empty())
            enrichStatesFromProject(state.projectRoot, state);
        enrichRtpcsFromProject(state.projectRoot, state);
        enrichEventStateModifiersFromProject(state.projectRoot, state);
    }
    else if (const auto discoveredProject = findProjectRootNearBank(bankFile); discoveredProject.isDirectory())
    {
        state.projectRoot = discoveredProject;
        if (!state.sourceFilesRoot.isDirectory())
            state.sourceFilesRoot = discoveredProject.getChildFile("Originals");
        if (!state.soundBanksRoot.isDirectory())
            state.soundBanksRoot = discoveredProject.getChildFile("GeneratedSoundBanks");
        if (state.switchValues.empty())
            enrichSwitchesFromProject(state.projectRoot, state);
        if (state.stateValues.empty())
            enrichStatesFromProject(state.projectRoot, state);
        enrichRtpcsFromProject(state.projectRoot, state);
        enrichEventStateModifiersFromProject(state.projectRoot, state);
    }

    enrichStateFromTextSidecar(bankFile, state);
    applyEventStateFallbackFromTextSidecar(bankFile, state.projectRoot, state);
}
} // namespace

BnkSidecarParser::Result BnkSidecarParser::enrichStateFromSidecars(const juce::File& bankFile,
    BnkDocument::State& state, const juce::File& projectRootOverride, std::function<bool()> checkpoint,
    BoundedSidecarInput::Profile profile, std::shared_ptr<const BoundedSidecarInput::Catalog> catalog)
{
    using Budget = BoundedSidecarInput;
    Budget inputBudget(nullptr, std::move(checkpoint), profile, std::move(catalog));
    Result result;
    try
    {
        if (const auto frozen = Budget::frozenInputs(); frozen && frozen->bankFile != bankFile)
            Budget::reject(Budget::Failure::ioFailure);
        // No copy of HIRC payloads, DATA or core object identity. Only the
        // optional enrichment fields can be committed by this transaction.
        BnkDocument::State candidate;
        candidate.info = state.info; candidate.projectRoot = state.projectRoot;
        candidate.sourceFilesRoot = state.sourceFilesRoot; candidate.soundBanksRoot = state.soundBanksRoot;
        candidate.events = state.events; candidate.switchValues = state.switchValues;
        candidate.stateValues = state.stateValues; candidate.stateGroups = state.stateGroups;
        // RTPC metadata is rebuilt privately from the current input set. A failed
        // refresh leaves the prior definitions/bindings untouched on the caller.
        enrichSidecarCandidate(bankFile, candidate, projectRootOverride);
        // TXT may add catalog records after WWU object references were read.
        // Reconcile against the final private catalog before publishing options.
        if (!candidate.stateValues.empty())
            for (auto& event : candidate.events)
                for (auto& modifier : event.stateModifiers)
                {
                    if (!Budget::continueWork()) break;
                    if (!resolveStateModifier(modifier,
                        {{modifier.stateGroupId,modifier.stateGroupToken,modifier.stateGroupName}},candidate))
                        event.stateModifiersResolved=false;
                }
        if (Budget::continueWork())
        {
            size_t media = 0, modifiers = 0, bindings = 0, points = 0;
            if (candidate.events.size() > 4096 || candidate.switchValues.size() > 4096
                || candidate.stateValues.size() > 4096 || candidate.stateGroups.size() > 4096 || candidate.rtpcs.size() > 4096)
                Budget::reject(Budget::Failure::hardLimit);
            for (const auto& event : candidate.events)
            {
                if (!Budget::continueWork()) break;
                if (event.media.size() > 65536 - media || event.stateModifiers.size() > 16384 - modifiers)
                { Budget::reject(Budget::Failure::hardLimit); break; }
                media += event.media.size(); modifiers += event.stateModifiers.size();
            }
            for (const auto& rtpc : candidate.rtpcs)
            {
                if (!Budget::continueWork()) break;
                if (rtpc.bindings.size() > 4096 - bindings)
                { Budget::reject(Budget::Failure::hardLimit); break; }
                bindings += rtpc.bindings.size();
                for (const auto& binding : rtpc.bindings)
                {
                    if (!Budget::continueWork()) break;
                    if (binding.points.size() > 65536 - points)
                    { Budget::reject(Budget::Failure::hardLimit); break; }
                    points += binding.points.size();
                }
            }
        }
        result.inputs = inputBudget.inputWitnesses();
        if (inputBudget.finish())
        {
            candidate.info.eventCount = static_cast<int>(candidate.events.size());
            state.info = std::move(candidate.info); state.projectRoot = std::move(candidate.projectRoot);
            state.sourceFilesRoot = std::move(candidate.sourceFilesRoot); state.soundBanksRoot = std::move(candidate.soundBanksRoot);
            state.events = std::move(candidate.events); state.switchValues = std::move(candidate.switchValues);
            state.stateValues = std::move(candidate.stateValues); state.stateGroups = std::move(candidate.stateGroups);
            state.rtpcs = std::move(candidate.rtpcs);
        }
    }
    catch (const std::bad_alloc&) { Budget::reject(Budget::Failure::memoryQuota); }
    catch (...) { Budget::reject(Budget::Failure::ioFailure); }
    result.failure = inputBudget.failureReason();
    if (result.failure != Budget::Failure::none)
    { try { state.info.diagnostics.add(Budget::describe(result.failure)); } catch (...) {} }
    return result;
}

std::shared_ptr<const BoundedSidecarInput::Catalog> BoundedSidecarInput::freeze(
    const juce::File& bank, const juce::File& projectRoot, std::function<bool()> checkpoint)
{
    auto catalog = std::make_shared<Catalog>();
    catalog->bankFile = bank;
    BoundedSidecarInput budget(nullptr, std::move(checkpoint));
    try
    {
        const auto pinIfPresent = [&](const juce::File& file)
        {
            if (budget.check() && (file.existsAsFile() || file.isSymbolicLink())) budget.pin(file);
        };
        const auto scan = [&](const juce::File& root, const juce::String& wildcard, bool recursive)
        {
            auto matches = files(root, wildcard, recursive);
            std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b)
                { return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0; });
            catalog->queries.push_back({ root, wildcard, recursive, matches });
            for (const auto& file : matches) if (budget.check()) budget.pin(file);
            return !matches.empty();
        };
        pinIfPresent(bank.withFileExtension(".txt"));
        pinIfPresent(bank.getParentDirectory().getChildFile("SoundbanksInfo.xml"));
        if (projectRoot.isDirectory()) catalog->projectRoot = projectRoot;
        else
        {
            auto directory = bank.getParentDirectory();
            for (int depth = 0; depth < 5 && directory.isDirectory() && budget.check(); ++depth)
            {
                if (scan(directory, "*.wproj", false)) { catalog->projectRoot = directory; break; }
                const auto parent = directory.getParentDirectory();
                if (parent == directory) break;
                directory = parent;
            }
        }
        if (catalog->projectRoot != juce::File())
            for (const auto* name : { "Switches", "States", "Game Parameters", "Actor-Mixer Hierarchy", "Interactive Music Hierarchy", "Events" })
            {
                if (!budget.check()) break;
                scan(catalog->projectRoot.getChildFile(name), "*.wwu", true);
            }
        budget.finish();
    }
    catch (const std::bad_alloc&) { reject(Failure::memoryQuota); }
    catch (...) { reject(Failure::ioFailure); }
    catalog->failure = budget.failureReason();
    catalog->inputs = std::move(budget.inputs);
    return catalog;
}
} // namespace singleeditor
