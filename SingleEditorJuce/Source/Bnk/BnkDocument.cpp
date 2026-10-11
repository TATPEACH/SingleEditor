#include "Bnk/BnkDocument.h"
#include "Bnk/BnkStateSelection.h"
#include "Bnk/BnkMediaLocator.h"

#include <algorithm>

#include "Bnk/BnkEventResolver.h"
#include "Bnk/BnkParser.h"
#include "Bnk/BoundedSidecarInput.h"

namespace singleeditor
{
namespace
{
juce::File tryFindOriginalMediaFile(const BnkDocument::State& state,
                                    const BnkDocument::PlayableMediaDescriptor& media)
{
    BoundedSidecarInput inputBudget;
    const auto source=bnkmedia::findOriginal(media.sourceFile,state.sourceFilesRoot,state.projectRoot,
        media.relativePath,media.shortName);
    return inputBudget.wasLimited() ? juce::File() : source;
}

juce::File getEmbeddedMediaCacheDir(const BnkDocument::State& state)
{
    const auto root = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("SingleEditor")
                          .getChildFile("bnk_media_cache");
    const auto cacheIdentity = state.info.file.getFullPathName()
                             + "|"
                             + juce::String(state.info.file.getSize())
                             + "|"
                             + juce::String(state.info.file.getLastModificationTime().toMilliseconds());
    const auto bankKey = juce::String::toHexString(static_cast<juce::int64>(cacheIdentity.hashCode64()));
    return root.getChildFile(bankKey);
}

std::vector<const BnkDocument::EventDescriptor::MediaReference*> getResolvedEventMediaForContext(
    const BnkDocument::EventDescriptor& event,
    const juce::String& switchValueToken)
{
    std::vector<const BnkDocument::EventDescriptor::MediaReference*> resolved;
    resolved.reserve(event.media.size());

    if (event.media.empty())
        return resolved;

    if (event.playbackHint == BnkDocument::PlaybackHint::switchDefault && switchValueToken.isNotEmpty())
    {
        for (const auto& media : event.media)
        {
            if (media.switchValueToken == switchValueToken)
                resolved.push_back(&media);
        }

        if (!resolved.empty())
            return resolved;

        for (const auto& media : event.media)
        {
            if (media.isDefaultSwitch)
                resolved.push_back(&media);
        }

        if (!resolved.empty())
            return resolved;
    }

    for (const auto& media : event.media)
        resolved.push_back(&media);

    return resolved;
}
} // namespace

BnkDocument::BnkDocument()
    : currentState(makeEmptyState())
{
}

std::shared_ptr<BnkDocument::State> BnkDocument::createStateFromFile(const juce::File& file, const juce::File& projectRootOverride)
{
    return BnkParser::parseFile(file, projectRootOverride);
}

bool BnkDocument::loadFile(const juce::File& file, const juce::File& projectRootOverride)
{
    return adoptParsedState(createStateFromFile(file, projectRootOverride));
}

bool BnkDocument::adoptParsedState(std::shared_ptr<State> state)
{
    if (state == nullptr || !state->isValid())
        return false;

    std::optional<uint32_t> defaultSelectedEventId;
    std::shared_ptr<const BnkRuntimeGraph::EventGraph> defaultGraph;
    if (!state->events.empty())
    {
        defaultSelectedEventId = state->events.front().id;
        defaultGraph = BnkEventResolver::resolve(*state, *defaultSelectedEventId);
    }

    {
        const juce::ScopedLock sl(stateLock);
        currentState = state;
        selectedEventId = defaultSelectedEventId;
        selectedMediaId.reset();
        selectedEventGraph = defaultGraph;
    }

    notifyChanged(state);
    return true;
}

void BnkDocument::clear()
{
    auto empty = makeEmptyState();
    {
        const juce::ScopedLock sl(stateLock);
        currentState = empty;
        selectedEventId.reset();
        selectedMediaId.reset();
        selectedEventGraph.reset();
    }

    notifyChanged(empty);
}

BnkDocument::StatePtr BnkDocument::getState() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return currentState;
}

BnkDocument::SelectionSnapshot::Ptr BnkDocument::captureSelection() const
{
    const juce::ScopedLock sl(stateLock);
    if (!currentState || currentState->info.file == juce::File()) return {};
    auto image = std::make_shared<SelectionSnapshot>();
    image->state = currentState; image->event = selectedEventId;
    image->media = selectedMediaId; image->graph = selectedEventGraph;
    return image;
}

bool BnkDocument::restoreSelection(const SelectionSnapshot& image)
{
    // This private snapshot retains parsed selection, not fresh source authority.
    // A moved bank must not erase the return point; hasBank/isSourceCurrent still
    // report the actual locator/version and guard subsequent media access.
    if (!image.state) return false;
    {
        const juce::ScopedLock sl(stateLock);
        currentState = image.state; selectedEventId = image.event;
        selectedMediaId = image.media; selectedEventGraph = image.graph;
    }
    notifyChanged(image.state);
    return true;
}

BnkDocument::BankInfo BnkDocument::getBankInfo() const
{
    return getState()->info;
}

bool BnkDocument::hasBank() const noexcept
{
    return getState()->info.isValid();
}

bool BnkDocument::selectEventById(std::optional<uint32_t> eventId)
{
    StatePtr state;
    std::shared_ptr<const BnkRuntimeGraph::EventGraph> resolvedGraph;

    {
        const juce::ScopedLock sl(stateLock);
        state = currentState;

        if (!eventId.has_value())
        {
            const auto changed = selectedEventId.has_value() || selectedEventGraph != nullptr || selectedMediaId.has_value();
            selectedEventId.reset();
            selectedMediaId.reset();
            selectedEventGraph.reset();
            if (!changed)
                return false;
        }
        else
        {
            const auto eventExists = std::find_if(state->events.begin(),
                                                  state->events.end(),
                                                  [eventId](const EventDescriptor& event)
                                                  {
                                                      return event.id == *eventId;
                                                  }) != state->events.end();
            if (!eventExists)
                return false;

            if (selectedEventId == eventId && selectedEventGraph != nullptr && !selectedMediaId.has_value())
                return false;

            resolvedGraph = BnkEventResolver::resolve(*state, *eventId);
            selectedEventId = eventId;
            selectedMediaId.reset();
            selectedEventGraph = resolvedGraph;
        }
    }

    notifyChanged(state);
    return true;
}

bool BnkDocument::selectMediaById(std::optional<uint32_t> mediaId)
{
    StatePtr state;
    {
        const juce::ScopedLock sl(stateLock);
        state = currentState;

        if (!mediaId.has_value())
        {
            if (!selectedMediaId.has_value())
                return false;

            selectedMediaId.reset();
        }
        else
        {
            const auto mediaExists = std::find_if(state->mediaEntries.begin(),
                                                  state->mediaEntries.end(),
                                                  [mediaId](const MediaEntry& media)
                                                  {
                                                      return media.id == *mediaId;
                                                  }) != state->mediaEntries.end()
                                  || std::any_of(state->events.begin(),
                                                 state->events.end(),
                                                 [mediaId](const EventDescriptor& event)
                                                 {
                                                     return std::any_of(event.media.begin(),
                                                                        event.media.end(),
                                                                        [mediaId](const EventDescriptor::MediaReference& media)
                                                                        {
                                                                            return media.mediaId == *mediaId;
                                                                        });
                                                 });
            if (!mediaExists)
                return false;

            if (selectedMediaId == mediaId)
                return false;

            selectedMediaId = mediaId;
        }
    }

    notifyChanged(state);
    return true;
}

std::optional<uint32_t> BnkDocument::getSelectedEventId() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return selectedEventId;
}

std::optional<uint32_t> BnkDocument::getSelectedMediaId() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return selectedMediaId;
}

std::shared_ptr<const BnkRuntimeGraph::EventGraph> BnkDocument::getSelectedEventGraph() const noexcept
{
    const juce::ScopedLock sl(stateLock);
    return selectedEventGraph;
}

std::vector<BnkDocument::SwitchValueDescriptor> BnkDocument::getSwitchValuesForSelectedEvent() const
{
    const juce::ScopedLock sl(stateLock);
    std::vector<SwitchValueDescriptor> options;
    if (!currentState->isValid() || !selectedEventId.has_value())
        return options;

    const auto eventIt = std::find_if(currentState->events.begin(),
                                      currentState->events.end(),
                                      [selectedEventId = selectedEventId](const EventDescriptor& event)
                                      {
                                          return event.id == *selectedEventId;
                                      });
    if (eventIt == currentState->events.end())
        return options;

    for (const auto& media : eventIt->media)
    {
        if (media.switchValueToken.isEmpty())
            continue;

        const auto exists = std::find_if(options.begin(),
                                         options.end(),
                                         [&media](const SwitchValueDescriptor& option)
                                         {
                                             return option.token == media.switchValueToken;
                                         });
        if (exists != options.end())
            continue;

        SwitchValueDescriptor option;
        option.token = media.switchValueToken;
        if (const auto known = std::find_if(currentState->switchValues.begin(),
                                            currentState->switchValues.end(),
                                            [&media](const SwitchValueDescriptor& value)
                                            {
                                                return value.token == media.switchValueToken;
                                            });
            known != currentState->switchValues.end())
        {
            option = *known;
        }
        else
        {
            option.name = media.displayName.isNotEmpty() ? media.displayName : media.shortName;
        }

        option.isDefault = option.isDefault || media.isDefaultSwitch;

        if (option.name.isEmpty())
            option.name = media.switchValueToken;

        options.push_back(std::move(option));
    }

    return options;
}

std::vector<BnkDocument::StateValueDescriptor> BnkDocument::getAvailableStateValues() const
{
    const juce::ScopedLock sl(stateLock);
    std::vector<StateValueDescriptor> states;
    if (!currentState->isValid() || !selectedEventId.has_value())
        return states;

    const auto eventIt = std::find_if(currentState->events.begin(),
                                      currentState->events.end(),
                                      [selectedEventId = selectedEventId](const EventDescriptor& event)
                                      {
                                          return event.id == *selectedEventId;
                                      });
    if (eventIt == currentState->events.end())
        return states;

    return bnkstate::options(*currentState, *eventIt);
}

bool BnkDocument::rtpcBindingTargetsEvent(const RtpcDescriptor::Binding& binding, const EventDescriptor& event)
{
    return event.stateTargetScope == EventDescriptor::StateTargetScope::singleObject
        && event.targetObjectId.isNotEmpty() && binding.objectId.isNotEmpty()
        && bnkstate::id(event.targetObjectId)==bnkstate::id(binding.objectId);
}

bool BnkDocument::hasRtpcMetadataForEvent(const State& state, const EventDescriptor& event)
{
    return std::any_of(state.rtpcs.begin(),state.rtpcs.end(),[&](const auto& rtpc)
    {
        return rtpc.hasDefinition && std::any_of(rtpc.bindings.begin(),rtpc.bindings.end(),
            [&](const auto& binding) { return rtpcBindingTargetsEvent(binding,event); });
    });
}

bool BnkDocument::hasRtpcMetadataForSelectedEvent() const
{
    const juce::ScopedLock sl(stateLock);
    if (!currentState->isValid() || !selectedEventId) return false;
    const auto event=std::find_if(currentState->events.begin(),currentState->events.end(),
        [&](const auto& candidate) { return candidate.id==*selectedEventId; });
    return event!=currentState->events.end() && hasRtpcMetadataForEvent(*currentState,*event);
}

std::vector<BnkDocument::RtpcDescriptor> BnkDocument::getAvailableRtpcs() const
{
    const juce::ScopedLock sl(stateLock);
    if (!currentState->isValid() || !selectedEventId.has_value())
        return {};

    const auto eventIt = std::find_if(currentState->events.begin(),
                                      currentState->events.end(),
                                      [selectedEventId = selectedEventId](const EventDescriptor& event)
                                      {
                                          return event.id == *selectedEventId;
                                      });
    if (eventIt == currentState->events.end())
        return {};

    std::vector<RtpcDescriptor> filtered;
    for (const auto& rtpc : currentState->rtpcs)
    {
        if (!rtpc.hasDefinition) continue;
        RtpcDescriptor selected;
        for (const auto& binding : rtpc.bindings)
            if (rtpcBindingTargetsEvent(binding,*eventIt)) selected.bindings.push_back(binding);
        if (!selected.bindings.empty())
        {
            selected.id=rtpc.id; selected.token=rtpc.token; selected.name=rtpc.name; selected.objectPath=rtpc.objectPath;
            selected.hasDefinition=rtpc.hasDefinition; selected.explicitRangeFields=rtpc.explicitRangeFields;
            selected.minValue=rtpc.minValue; selected.maxValue=rtpc.maxValue; selected.defaultValue=rtpc.defaultValue;
            filtered.push_back(std::move(selected));
        }
    }

    return filtered;
}

std::vector<BnkDocument::PlayableMediaDescriptor> BnkDocument::getPlayableMedia() const
{
    const juce::ScopedLock sl(stateLock);
    std::vector<PlayableMediaDescriptor> mediaList;
    if (!currentState->isValid())
        return mediaList;

    auto upsertMedia = [&mediaList](uint32_t mediaId) -> PlayableMediaDescriptor&
    {
        const auto found = std::find_if(mediaList.begin(),
                                        mediaList.end(),
                                        [mediaId](const PlayableMediaDescriptor& entry)
                                        {
                                            return entry.mediaId == mediaId;
                                        });
        if (found != mediaList.end())
            return *found;

        PlayableMediaDescriptor descriptor;
        descriptor.mediaId = mediaId;
        descriptor.displayName = "Media 0x" + juce::String::toHexString(static_cast<juce::int64>(mediaId)).toUpperCase();
        mediaList.push_back(std::move(descriptor));
        return mediaList.back();
    };

    for (const auto& mediaEntry : currentState->mediaEntries)
    {
        auto& descriptor = upsertMedia(mediaEntry.id);
        descriptor.extractableFromBank = descriptor.extractableFromBank
                                      || (currentState->dataChunkStart > 0 && mediaEntry.size > 0);
    }

    for (const auto& event : currentState->events)
    {
        for (const auto& media : event.media)
        {
            auto& descriptor = upsertMedia(media.mediaId);
            if (descriptor.displayName.startsWithIgnoreCase("Media 0x"))
            {
                if (media.displayName.isNotEmpty())
                    descriptor.displayName = media.displayName;
                else if (media.shortName.isNotEmpty())
                    descriptor.displayName = media.shortName;
            }

            if (descriptor.shortName.isEmpty() && media.shortName.isNotEmpty())
                descriptor.shortName = media.shortName;
            if (descriptor.relativePath.isEmpty() && media.relativePath.isNotEmpty())
                descriptor.relativePath = media.relativePath;
            if (!descriptor.mediaFile.existsAsFile() && media.mediaFile.existsAsFile())
                descriptor.mediaFile = media.mediaFile;
            if (descriptor.sourceFile == juce::File() && media.sourceFile != juce::File())
                descriptor.sourceFile = media.sourceFile;
            ++descriptor.referenceCount;
        }
    }

    const auto bankDirectory = currentState->info.file.getParentDirectory();
    for (auto& descriptor : mediaList)
    {
        if (!descriptor.mediaFile.existsAsFile() && bankDirectory.isDirectory() && descriptor.mediaId != 0)
        {
            const auto generatedWem = bankDirectory.getChildFile(juce::String(descriptor.mediaId) + ".wem");
            if (generatedWem.existsAsFile())
                descriptor.mediaFile = generatedWem;
        }

        if (descriptor.sourceFile == juce::File())
            descriptor.sourceFile = tryFindOriginalMediaFile(*currentState, descriptor);

        if (descriptor.sourceFile.existsAsFile())
            descriptor.availability = PlayableMediaDescriptor::Availability::sourceMedia;

        if (descriptor.mediaFile.existsAsFile())
        {
            const auto packedWemName = juce::String(descriptor.mediaId) + ".wem";
            if (descriptor.mediaFile.getFileName().equalsIgnoreCase(packedWemName))
                descriptor.availability = PlayableMediaDescriptor::Availability::packedMedia;
            else if (descriptor.mediaFile == descriptor.sourceFile)
                descriptor.availability = PlayableMediaDescriptor::Availability::sourceMedia;
            else if (descriptor.mediaFile.getFullPathName().containsIgnoreCase(currentState->info.file.getParentDirectory().getFullPathName()))
                descriptor.availability = PlayableMediaDescriptor::Availability::packedMedia;
            else if (!descriptor.sourceFile.existsAsFile())
                descriptor.availability = PlayableMediaDescriptor::Availability::packedMedia;
        }
        else if (descriptor.extractableFromBank)
        {
            descriptor.availability = PlayableMediaDescriptor::Availability::embeddedMedia;
        }
    }

    std::sort(mediaList.begin(),
              mediaList.end(),
              [](const PlayableMediaDescriptor& a, const PlayableMediaDescriptor& b)
              {
                  const auto nameCompare = a.displayName.compareNatural(b.displayName);
                  if (nameCompare != 0)
                      return nameCompare < 0;

                  return a.mediaId < b.mediaId;
              });

    return mediaList;
}

std::optional<BnkDocument::PlayableMediaDescriptor> BnkDocument::getPlayableMediaById(uint32_t mediaId) const
{
    if (mediaId == 0)
        return std::nullopt;

    auto mediaEntries = getPlayableMedia();
    const auto found = std::find_if(mediaEntries.begin(),
                                    mediaEntries.end(),
                                    [mediaId](const PlayableMediaDescriptor& entry)
                                    {
                                        return entry.mediaId == mediaId;
                                    });
    if (found == mediaEntries.end())
        return std::nullopt;

    return *found;
}

std::vector<uint32_t> BnkDocument::getSelectedEventMediaIds(const juce::String& switchValueToken) const
{
    const juce::ScopedLock sl(stateLock);
    std::vector<uint32_t> mediaIds;
    if (!currentState->isValid() || !selectedEventId.has_value())
        return mediaIds;

    const auto eventIt = std::find_if(currentState->events.begin(),
                                      currentState->events.end(),
                                      [selectedEventId = selectedEventId](const EventDescriptor& event)
                                      {
                                          return event.id == *selectedEventId;
                                      });
    if (eventIt == currentState->events.end())
        return mediaIds;

    for (const auto* media : getResolvedEventMediaForContext(*eventIt, switchValueToken))
    {
        if (media != nullptr
            && media->mediaId != 0
            && std::find(mediaIds.begin(), mediaIds.end(), media->mediaId) == mediaIds.end())
        {
            mediaIds.push_back(media->mediaId);
        }
    }

    return mediaIds;
}

juce::StringArray BnkDocument::getSelectedEventMediaNames(const juce::String& switchValueToken) const
{
    const juce::ScopedLock sl(stateLock);
    juce::StringArray names;
    if (!currentState->isValid() || !selectedEventId.has_value())
        return names;

    const auto eventIt = std::find_if(currentState->events.begin(),
                                      currentState->events.end(),
                                      [selectedEventId = selectedEventId](const EventDescriptor& event)
                                      {
                                          return event.id == *selectedEventId;
                                      });
    if (eventIt == currentState->events.end())
        return names;

    for (const auto* media : getResolvedEventMediaForContext(*eventIt, switchValueToken))
    {
        if (media == nullptr)
            continue;

        const auto label = media->displayName.isNotEmpty() ? media->displayName
                                                           : (media->shortName.isNotEmpty() ? media->shortName
                                                                                            : ("Media 0x" + juce::String::toHexString(static_cast<juce::int64>(media->mediaId)).toUpperCase()));
        names.addIfNotAlreadyThere(label);
    }

    return names;
}

juce::StringArray BnkDocument::getEventNamesForMedia(uint32_t mediaId) const
{
    const juce::ScopedLock sl(stateLock);
    juce::StringArray names;
    if (!currentState->isValid() || mediaId == 0)
        return names;

    for (const auto& event : currentState->events)
    {
        const auto usesMedia = std::any_of(event.media.begin(),
                                           event.media.end(),
                                           [mediaId](const EventDescriptor::MediaReference& media)
                                           {
                                               return media.mediaId == mediaId;
                                           });
        if (usesMedia)
            names.addIfNotAlreadyThere(event.displayName);
    }

    return names;
}

juce::File BnkDocument::getEmbeddedMediaCacheFile(uint32_t mediaId) const
{
    const juce::ScopedLock sl(stateLock);
    if (!currentState->isValid() || mediaId == 0)
        return {};

    const auto entry = std::find_if(currentState->mediaEntries.begin(),
                                    currentState->mediaEntries.end(),
                                    [mediaId](const MediaEntry& current)
                                    {
                                        return current.id == mediaId;
                                    });
    if (entry == currentState->mediaEntries.end() || entry->size == 0)
        return {};

    return getEmbeddedMediaCacheDir(*currentState).getChildFile(juce::String(mediaId) + ".wem");
}

void BnkDocument::addListener(Listener* listener)
{
    listeners.add(listener);
}

void BnkDocument::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

juce::String BnkDocument::describeObjectType(HircObjectType type)
{
    switch (type)
    {
        case HircObjectType::settings: return "Settings";
        case HircObjectType::sound: return "Sound";
        case HircObjectType::eventAction: return "Event Action";
        case HircObjectType::event: return "Event";
        case HircObjectType::randomSequenceContainer: return "Random/Sequence Container";
        case HircObjectType::switchContainer: return "Switch Container";
        case HircObjectType::actorMixer: return "Actor Mixer";
        case HircObjectType::audioBus: return "Audio Bus";
        case HircObjectType::blendContainer: return "Blend Container";
        case HircObjectType::musicSegment: return "Music Segment";
        case HircObjectType::musicTrack: return "Music Track";
        case HircObjectType::musicSwitchContainer: return "Music Switch Container";
        case HircObjectType::musicRandomSequenceContainer: return "Music Random Sequence Container";
        case HircObjectType::unknown:
        default: return "Unknown";
    }
}

juce::String BnkDocument::describeAvailability(PlayableMediaDescriptor::Availability availability)
{
    switch (availability)
    {
        case PlayableMediaDescriptor::Availability::sourceMedia: return "Source";
        case PlayableMediaDescriptor::Availability::packedMedia: return "Packed";
        case PlayableMediaDescriptor::Availability::embeddedMedia: return "Embedded";
        case PlayableMediaDescriptor::Availability::unresolved:
        default: return "Unresolved";
    }
}

juce::String BnkDocument::describePlaybackHint(PlaybackHint type)
{
    switch (type)
    {
        case PlaybackHint::single: return "Single";
        case PlaybackHint::random: return "Random";
        case PlaybackHint::sequence: return "Sequence";
        case PlaybackHint::switchDefault: return "Switch";
        case PlaybackHint::layered: return "Layered";
        case PlaybackHint::unavailable:
        default: return "Unavailable";
    }
}

BnkDocument::StatePtr BnkDocument::makeEmptyState()
{
    return std::make_shared<State>();
}

void BnkDocument::notifyChanged(StatePtr state)
{
    listeners.call([state](Listener& listener)
    {
        listener.bankDocumentChanged(state);
    });
}
} // namespace singleeditor
