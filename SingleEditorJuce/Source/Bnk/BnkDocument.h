#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

#include "Bnk/BnkRuntimeGraph.h"

namespace singleeditor
{
class BnkDocument final
{
public:
    enum class PlaybackHint
    {
        unavailable = 0,
        single,
        random,
        sequence,
        switchDefault,
        layered
    };

    enum class HircObjectType : uint8_t
    {
        unknown = 0,
        settings = 1,
        sound = 2,
        eventAction = 3,
        event = 4,
        randomSequenceContainer = 5,
        switchContainer = 6,
        actorMixer = 7,
        audioBus = 8,
        blendContainer = 9,
        musicSegment = 10,
        musicTrack = 11,
        musicSwitchContainer = 12,
        musicRandomSequenceContainer = 13
    };

    struct MediaEntry
    {
        uint32_t id = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct PlayableMediaDescriptor
    {
        enum class Availability
        {
            unresolved = 0,
            sourceMedia,
            packedMedia,
            embeddedMedia
        };

        uint32_t mediaId = 0;
        juce::String displayName;
        juce::String shortName;
        juce::String relativePath;
        juce::File mediaFile;
        juce::File sourceFile;
        int referenceCount = 0;
        bool extractableFromBank = false;
        Availability availability = Availability::unresolved;
    };

    struct HircObject
    {
        HircObjectType type = HircObjectType::unknown;
        uint32_t id = 0;
        uint32_t payloadSize = 0;
        juce::MemoryBlock payload;
    };

    struct EventDescriptor
    {
        enum class StateTargetScope { unspecified, singleObject, unavailable };
        struct StateModifier
        {
            juce::String stateGroupToken;
            uint32_t stateGroupId = 0;
            juce::String stateGroupName;
            juce::String stateValueToken;
            juce::String stateName;
            float volumeDb = 0.0f;
            bool hasVolume = false;
        };

        struct MediaReference
        {
            uint32_t mediaId = 0;
            juce::String displayName;
            juce::String shortName;
            juce::String relativePath;
            juce::String switchValueToken;
            juce::File mediaFile;
            juce::File sourceFile;
            bool isDefaultSwitch = false;
        };

        uint32_t id = 0;
        juce::String displayName;
        juce::String authoringId;
        juce::String targetObjectId;
        juce::String targetObjectName;
        StateTargetScope stateTargetScope = StateTargetScope::unspecified;
        bool stateModifiersResolved = true;
        std::vector<uint32_t> actionIds;
        juce::String summary;
        juce::String playbackSummary;
        PlaybackHint playbackHint = PlaybackHint::unavailable;
        std::vector<MediaReference> media;
        std::vector<StateModifier> stateModifiers;
        bool mayResolveToMultipleNodes = false;
        double durationMinSeconds = 0.0;
        double durationMaxSeconds = 0.0;

        bool hasPlayableMedia() const noexcept
        {
            return std::any_of(media.begin(),
                               media.end(),
                               [](const MediaReference& entry)
                               {
                                   return entry.mediaFile.existsAsFile() || entry.sourceFile.existsAsFile();
                               });
        }
    };

    struct SwitchValueDescriptor
    {
        uint32_t id = 0;
        juce::String token;
        juce::String name;
        juce::String groupName;
        bool isDefault = false;
    };

    struct StateGroupDescriptor
    {
        uint32_t id = 0;
        juce::String token, name;
    };

    struct StateValueDescriptor
    {
        uint32_t id = 0;
        juce::String token;
        juce::String name;
        juce::String groupToken;
        uint32_t groupId = 0;
        juce::String groupName;
        bool isDefault = false;
    };

    struct RtpcDescriptor
    {
        struct CurvePoint
        {
            double x = 0.0;
            double y = 0.0;
        };

        struct Binding
        {
            juce::String objectId;
            juce::String objectName;
            juce::String objectPath;
            juce::String propertyName;
            std::vector<CurvePoint> points;
        };

        uint32_t id = 0;
        juce::String token;
        juce::String name;
        juce::String objectPath;
        bool hasDefinition = false;
        // Presence bits distinguish declared fields from legacy display defaults.
        uint8_t explicitRangeFields = 0;
        double minValue = 0.0;
        double maxValue = 100.0;
        double defaultValue = 0.0;
        std::vector<Binding> bindings;
    };

    struct BankInfo
    {
        juce::File file;
        uint32_t version = 0;
        uint32_t bankId = 0;
        int mediaCount = 0;
        int hircObjectCount = 0;
        int eventCount = 0;
        juce::StringArray diagnostics;

        bool isValid() const noexcept { return file.existsAsFile(); }
    };

    struct State
    {
        BankInfo info;
        juce::File projectRoot;
        juce::File sourceFilesRoot;
        juce::File soundBanksRoot;
        int64_t dataChunkStart = 0;
        int64_t dataChunkSize = 0;
        int64_t sourceFileSize = -1;
        int64_t sourceModifiedMs = 0;
        uint64_t sourceFileIdentifier = 0;
        std::vector<MediaEntry> mediaEntries;
        std::vector<HircObject> objects;
        std::vector<EventDescriptor> events;
        std::vector<SwitchValueDescriptor> switchValues;
        std::vector<StateValueDescriptor> stateValues;
        std::vector<StateGroupDescriptor> stateGroups;
        std::vector<RtpcDescriptor> rtpcs;

        bool isValid() const noexcept { return info.isValid(); }
        bool isSourceCurrent() const
        {
            return sourceFileSize >= 0 && info.file.existsAsFile()
                && info.file.getSize() == sourceFileSize
                && info.file.getLastModificationTime().toMilliseconds() == sourceModifiedMs
                && info.file.getFileIdentifier() == sourceFileIdentifier;
        }
    };

    using StatePtr = std::shared_ptr<const State>;

    // In-memory return point for a provisional optional-media activation. Keeps
    // the exact parsed bank and resolved selection; no reparse on cancellation.
    class SelectionSnapshot final
    {
    public:
        using Ptr = std::shared_ptr<const SelectionSnapshot>;
    private:
        friend class BnkDocument;
        StatePtr state;
        std::optional<uint32_t> event, media;
        std::shared_ptr<const BnkRuntimeGraph::EventGraph> graph;
    };
    SelectionSnapshot::Ptr captureSelection() const;
    bool restoreSelection(const SelectionSnapshot&);

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void bankDocumentChanged(StatePtr) {}
    };

    BnkDocument();

    static std::shared_ptr<State> createStateFromFile(const juce::File& file, const juce::File& projectRootOverride = {});
    bool loadFile(const juce::File& file, const juce::File& projectRootOverride = {});
    bool adoptParsedState(std::shared_ptr<State> state);
    void clear();

    StatePtr getState() const noexcept;
    BankInfo getBankInfo() const;
    bool hasBank() const noexcept;
    bool selectEventById(std::optional<uint32_t> eventId);
    bool selectMediaById(std::optional<uint32_t> mediaId);
    std::optional<uint32_t> getSelectedEventId() const noexcept;
    std::optional<uint32_t> getSelectedMediaId() const noexcept;
    std::shared_ptr<const BnkRuntimeGraph::EventGraph> getSelectedEventGraph() const noexcept;
    std::vector<SwitchValueDescriptor> getSwitchValuesForSelectedEvent() const;
    std::vector<StateValueDescriptor> getAvailableStateValues() const;
    std::vector<RtpcDescriptor> getAvailableRtpcs() const;
    bool hasRtpcMetadataForSelectedEvent() const;
    static bool rtpcBindingTargetsEvent(const RtpcDescriptor::Binding&, const EventDescriptor&);
    static bool hasRtpcMetadataForEvent(const State&, const EventDescriptor&);
    std::vector<PlayableMediaDescriptor> getPlayableMedia() const;
    std::optional<PlayableMediaDescriptor> getPlayableMediaById(uint32_t mediaId) const;
    std::vector<uint32_t> getSelectedEventMediaIds(const juce::String& switchValueToken = {}) const;
    juce::StringArray getSelectedEventMediaNames(const juce::String& switchValueToken = {}) const;
    juce::StringArray getEventNamesForMedia(uint32_t mediaId) const;
    juce::File getEmbeddedMediaCacheFile(uint32_t mediaId) const;
    static juce::String describeAvailability(PlayableMediaDescriptor::Availability availability);

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

    static juce::String describeObjectType(HircObjectType type);
    static juce::String describePlaybackHint(PlaybackHint type);

private:
    static StatePtr makeEmptyState();
    void notifyChanged(StatePtr state);

    juce::CriticalSection stateLock;
    juce::ListenerList<Listener> listeners;
    StatePtr currentState;
    std::optional<uint32_t> selectedEventId;
    std::optional<uint32_t> selectedMediaId;
    std::shared_ptr<const BnkRuntimeGraph::EventGraph> selectedEventGraph;
};
} // namespace singleeditor
